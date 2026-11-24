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

#include "ScenePayloadImpl.h"

#include <algorithm>
#include <cstdint>

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_ENTER

ScenePayload::ScenePayload()
    : _imp()
{
}

ScenePayload::ScenePayload(const std::shared_ptr<const ScenePayloadImpl>& imp)
    : _imp(imp)
{
}

ScenePayload::~ScenePayload()
{
}

bool
ScenePayload::isEmpty() const
{
    return !_imp || (_imp->numLayers == 0);
}

std::size_t
ScenePayload::getNumOpenRegions() const
{
    return _imp ? _imp->openRegions.size() : 0;
}

std::size_t
ScenePayload::getNumLayers() const
{
    return _imp ? _imp->numLayers : 0;
}

U64
ScenePayload::getStackHash() const
{
    return _imp ? _imp->stackHash : 0;
}

U64
ScenePayload::getContentHash() const
{
    return _imp ? _imp->contentHash : 0;
}

const SceneMetadata&
ScenePayload::getMetadata() const
{
    static const SceneMetadata fallback;

    return _imp ? _imp->metadata : fallback;
}

bool
ScenePayload::isSameStack(const ScenePayload& other) const
{
    const ScenePayloadImpl* a = _imp.get();
    const ScenePayloadImpl* b = other._imp.get();

    // Stacks that share a base share everything below it, so the walk stops at the first
    // common link rather than at the bottom.
    while (a != b) {
        if (!a || !b || (a->numLayers != b->numLayers) || (a->top.layer != b->top.layer)) {
            return false;
        }
        a = a->base.get();
        b = b->base.get();
    }

    return true;
}

bool
SceneLayerRegionSettings::isEmpty() const
{
    return savePath.empty() && !defaultPrim && !comment && !startTime && !endTime && !timeCodesPerSecond && !framesPerSecond
        && !upAxis && !metersPerUnit && !colorConfiguration && !colorManagementSystem && !renderSettingsPrimPath
        && customData.empty();
}

void
SceneLayerRegionSettings::mergeFrom(const SceneLayerRegionSettings& stronger)
{
    if (!stronger.savePath.empty()) {
        savePath = stronger.savePath;
    }
    if (!stronger.node.empty()) {
        node = stronger.node;
    }
    const auto take = [](auto* field, const auto& other) {
        if (other) {
            *field = other;
        }
    };
    take(&defaultPrim, stronger.defaultPrim);
    take(&comment, stronger.comment);
    take(&startTime, stronger.startTime);
    take(&endTime, stronger.endTime);
    take(&timeCodesPerSecond, stronger.timeCodesPerSecond);
    take(&framesPerSecond, stronger.framesPerSecond);
    take(&upAxis, stronger.upAxis);
    take(&metersPerUnit, stronger.metersPerUnit);
    take(&colorConfiguration, stronger.colorConfiguration);
    take(&colorManagementSystem, stronger.colorManagementSystem);
    take(&renderSettingsPrimPath, stronger.renderSettingsPrimPath);
    for (const SceneLayerCustomDatum& datum : stronger.customData) {
        std::vector<SceneLayerCustomDatum>::iterator found = std::find_if(customData.begin(), customData.end(), [&datum](const SceneLayerCustomDatum& d) {
            return d.name == datum.name;
        });
        if (found != customData.end()) {
            *found = datum;
        } else {
            customData.push_back(datum);
        }
    }
}

U64
SceneLayerRegionSettings::hash() const
{
    ContentHasher h;

    h.appendString(savePath);
    h.appendString(node);
    const auto appendString = [&h](const std::optional<std::string>& field) {
        h.appendBool(bool(field));
        if (field) {
            h.appendString(*field);
        }
    };
    const auto appendDouble = [&h](const std::optional<double>& field) {
        h.appendBool(bool(field));
        if (field) {
            h.appendDouble(*field);
        }
    };
    appendString(defaultPrim);
    appendString(comment);
    appendDouble(startTime);
    appendDouble(endTime);
    appendDouble(timeCodesPerSecond);
    appendDouble(framesPerSecond);
    h.appendBool(bool(upAxis));
    if (upAxis) {
        h.appendI64((std::int64_t)*upAxis);
    }
    appendDouble(metersPerUnit);
    appendString(colorConfiguration);
    appendString(colorManagementSystem);
    appendString(renderSettingsPrimPath);
    h.appendU64(customData.size());
    for (const SceneLayerCustomDatum& datum : customData) {
        h.appendString(datum.name);
        h.appendString(datum.type);
        h.appendString(datum.text);
    }

    return h.finalize();
}

NATRON_NAMESPACE_EXIT
