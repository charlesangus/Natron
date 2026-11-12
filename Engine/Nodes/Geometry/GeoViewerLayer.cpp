/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GeoViewerLayer.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <utility>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Engine/Nodes/Geometry/GeoSdfData.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<U64> nextViewerLayerId(1);

// Past this many, the frames held longest go first: playback evaluates no further ahead.
const std::size_t kMaxHeldFrames = 64;

} // namespace

struct GeoViewerLayer::Slot {
    struct Held {
        GeoDetailConstPtr detail;
        U64 order = 0;
    };

    std::mutex mutex;
    // Under mutex.
    GeoDetailConstPtr detail = std::make_shared<const GeoDetail>();
    int frame = 0;
    bool hasFrame = false;
    std::map<int, Held> held;
    U64 nextOrder = 0;
    // What the layer last read: the provider sets them.
    int providedFrame = 0;
    bool hasProvidedFrame = false;
};

namespace {

std::mutex slotsMutex;
// Under slotsMutex.
std::map<std::string, std::weak_ptr<GeoViewerLayer::Slot>> slots;

} // namespace

GeoViewerLayer::GeoViewerLayer(const std::string& consumer)
    : _key("geoviewer" + std::to_string(nextViewerLayerId.fetch_add(1)))
    , _consumer(consumer.empty() ? std::string("viewer") : consumer)
    , _identifier(GeoLayers::identifier(_key, std::string(), std::string(), _consumer))
    , _slot(std::make_shared<Slot>())
{
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        slots[_identifier] = _slot;
    }
    const std::weak_ptr<Slot> weakSlot = _slot;
    GeoLayers::setProvider(_key, [weakSlot]() {
        const std::shared_ptr<Slot> slot = weakSlot.lock();
        if (!slot) {
            return GeoDetailConstPtr();
        }
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->providedFrame = slot->frame;
        slot->hasProvidedFrame = slot->hasFrame;

        return slot->detail; }, _consumer);
}

GeoViewerLayer::~GeoViewerLayer()
{
    GeoLayers::removeProvider(_key, _consumer);
    std::lock_guard<std::mutex> lock(slotsMutex);
    slots.erase(_identifier);
}

const std::string&
GeoViewerLayer::getIdentifier() const
{
    return _identifier;
}

bool
GeoViewerLayer::serve(const GeoDetailConstPtr& detail,
                      ScenePayload* payload,
                      std::string* errorMessage)
{
    {
        std::lock_guard<std::mutex> lock(_slot->mutex);
        _slot->detail = detail ? detail : std::make_shared<const GeoDetail>();
        _slot->hasFrame = false;
        _slot->held.clear();
    }

    return openPayload(payload, errorMessage);
}

bool
GeoViewerLayer::serveFrame(const GeoDetailConstPtr& detail,
                           int frame,
                           bool sequential,
                           ScenePayload* payload,
                           std::string* errorMessage)
{
    bool opened;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        opened = !_payload.isEmpty();
    }
    {
        std::lock_guard<std::mutex> lock(_slot->mutex);
        if (!sequential) {
            _slot->held.clear();
        }
        Slot::Held& held = _slot->held[frame];
        held.detail = detail ? detail : std::make_shared<const GeoDetail>();
        if (!opened) {
            // The layer opens on the first detail, so showing it later edits nothing, and a stage
            // composing it sees its prims appear as it is synced rather than as a resync.
            _slot->detail = held.detail;
        }
        held.order = _slot->nextOrder++;
        while (_slot->held.size() > kMaxHeldFrames) {
            auto oldest = _slot->held.begin();
            for (auto it = _slot->held.begin(); it != _slot->held.end(); ++it) {
                if (it->second.order < oldest->second.order) {
                    oldest = it;
                }
            }
            _slot->held.erase(oldest);
        }
    }

    return openPayload(payload, errorMessage);
}

bool
GeoViewerLayer::showFrame(int frame)
{
    std::lock_guard<std::mutex> lock(_slot->mutex);
    const auto found = _slot->held.find(frame);
    if (found == _slot->held.end()) {
        return false;
    }
    const GeoDetailConstPtr detail = found->second.detail;
    _slot->held.erase(found);
    const bool changed = (detail != _slot->detail) || !_slot->hasFrame || (_slot->frame != frame);
    _slot->detail = detail;
    _slot->frame = frame;
    _slot->hasFrame = true;

    return changed;
}

bool
GeoViewerLayer::openPayload(ScenePayload* payload,
                            std::string* errorMessage)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_payload.isEmpty()) {
        if (!GeoLayers::registerPlugin(errorMessage)) {
            return false;
        }
        // No stage composes the layer before this payload exists, so opening it here races no
        // reader.
        TfErrorMark mark;
        const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(_identifier);
        if (!layer) {
            if (errorMessage) {
                *errorMessage = "The viewer could not open its geometry layer " + _identifier;
            }
            mark.Clear();

            return false;
        }
        // The stack is the same at every frame, so a stage synced to it again edits nothing.
        _payload = SceneOps::appendLayer(ScenePayload(), layer, U64(std::hash<std::string>()(_identifier)));
    }
    if (payload) {
        *payload = _payload;
    }

    return true;
}

GeoDetailConstPtr
GeoViewerLayer::getDetail() const
{
    std::lock_guard<std::mutex> lock(_slot->mutex);

    return _slot->detail;
}

bool
GeoViewerLayer::getServedFrame(const std::string& identifier,
                               int* frame)
{
    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        const auto found = slots.find(identifier);
        if (found != slots.end()) {
            slot = found->second.lock();
        }
    }
    if (!slot) {
        return false;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    if (frame) {
        *frame = slot->providedFrame;
    }

    return slot->hasProvidedFrame;
}

std::vector<std::string>
GeoViewerLayer::findLayers(const ScenePayload& payload)
{
    std::vector<std::string> found;

    for (const SceneLayerEntry& entry : SceneOps::getLayers(payload)) {
        if (entry.layer && GeoLayers::isConsumerLayer(entry.layer->GetIdentifier())) {
            found.push_back(entry.layer->GetIdentifier());
        }
    }

    return found;
}

bool
GeoViewerLayer::refreshLayers(const ScenePayload& payload,
                              bool* edited,
                              std::string* errorMessage)
{
    if (edited) {
        *edited = false;
    }
    bool ok = true;
    for (const std::string& identifier : findLayers(payload)) {
        bool layerEdited = false;
        std::string error;
        if (!GeoLayers::refresh(identifier, &error, &layerEdited)) {
            ok = false;
            if (errorMessage && errorMessage->empty()) {
                *errorMessage = error;
            }
        }
        if (layerEdited && edited) {
            *edited = true;
        }
    }

    return ok;
}

std::shared_ptr<const GeoUsdConvert>
GeoViewerLayer::layerConvert(const std::string& identifier)
{
    return geoLayerConvert(SdfLayer::Find(identifier));
}

bool
GeoViewerLayer::pickPrimitives(const GeoUsdConvert& convert,
                               const std::string& primPath,
                               Pick* pick)
{
    if (primPath.empty() || !SdfPath::IsValidPathString(primPath) || !convert.detail()) {
        return false;
    }
    // A pick inside an instancer's prototypes lands below the prim the partition authors.
    for (SdfPath path = SdfPath(primPath).GetPrimPath(); !path.IsEmpty() && (path != SdfPath::AbsoluteRootPath()); path = path.GetParentPath()) {
        const int partition = convert.specPartition(path);
        if ((partition < 0) || (convert.partitionPath(std::size_t(partition)) != path)) {
            continue;
        }
        if (pick) {
            const GeoIndexMap& primitives = convert.detail()->primitiveMap();
            pick->primPath = path.GetString();
            pick->primitives.clear();
            for (std::size_t offset : convert.partitionPrimitives(std::size_t(partition))) {
                pick->primitives.push_back(primitives.indexFromOffset(GeoOffset(offset)));
            }
            std::sort(pick->primitives.begin(), pick->primitives.end());
        }

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
