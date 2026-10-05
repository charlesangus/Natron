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

#include "FrameRenderContext.h"

#include <cassert>
#include <functional>
#include <utility>

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

bool
FrameStore::TaskKey::operator==(const TaskKey& other) const
{
    return node == other.node && time == other.time && view == other.view && mipmapLevel == other.mipmapLevel;
}

std::size_t
FrameStore::TaskKeyHash::operator()(const TaskKey& key) const
{
    std::size_t h = std::hash<const Node*>()(key.node.get());
    // 0. and -0. compare equal, so they must hash the same.
    const double time = (key.time == 0.) ? 0. : key.time;

    h ^= std::hash<double>()(time) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(key.view.value()) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<unsigned int>()(key.mipmapLevel) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);

    return h;
}

FrameStore::FrameStore()
    : _entriesMutex()
    , _entries()
    , _imageRefs()
    , _bytesInFlight(0)
{
}

FrameStore::~FrameStore()
{
}

void
FrameStore::put(const TaskKey& key,
                std::map<ImageLayerDesc, ImagePtr> layers,
                int consumers)
{
    if (consumers <= 0) {
        return;
    }

    std::shared_ptr<Entry> entry = std::make_shared<Entry>();
    bool boundsSet = false;
    for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const RectI layerBounds = it->second->getBounds();
        if (boundsSet) {
            entry->bounds = entry->bounds.intersect(layerBounds);
        } else {
            entry->bounds = layerBounds;
            boundsSet = true;
        }
        entry->bytes += it->second->size();
    }
    entry->layers = std::move(layers);
    entry->consumersLeft = consumers;

    // Destroyed once the lock is released, so that freeing its images does not happen under it.
    std::shared_ptr<Entry> replaced;
    {
        std::lock_guard<std::mutex> k(_entriesMutex);
        std::shared_ptr<Entry>& slot = _entries[key];
        replaced.swap(slot);
        slot = entry;
        addImageRefsLocked(*entry);
        if (replaced) {
            removeImageRefsLocked(*replaced);
        }
    }
}

void
FrameStore::addImageRefsLocked(const Entry& entry)
{
    for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = entry.layers.begin(); it != entry.layers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        ImageRefs& refs = _imageRefs[it->second.get()];
        if (refs.refs++ > 0) {
            continue;
        }
        // The image cache counts the memory of the images it allocated until they are destroyed, evicted or not.
        refs.bytes = it->second->getCacheAPI() ? 0 : it->second->size();
        _bytesInFlight += refs.bytes;
    }
}

void
FrameStore::removeImageRefsLocked(const Entry& entry)
{
    for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = entry.layers.begin(); it != entry.layers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        ImageRefsMap::iterator found = _imageRefs.find(it->second.get());
        if (found == _imageRefs.end()) {
            continue;
        }
        if (--found->second.refs > 0) {
            continue;
        }
        _bytesInFlight -= found->second.bytes;
        _imageRefs.erase(found);
    }
}

bool
FrameStore::find(const TaskKey& key,
                 const std::list<ImageLayerDesc>& needed,
                 const RectI& pixelRoI,
                 std::list<ImagePtr>* out) const
{
    assert(out);
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> k(_entriesMutex);
        EntryMap::const_iterator found = _entries.find(key);
        if (found == _entries.end()) {
            return false;
        }
        entry = found->second;
    }

    // Only consumersLeft changes once an entry is stored, so its layers and bounds can be read without the lock.
    if (!entry->bounds.contains(pixelRoI)) {
        return false;
    }
    std::list<ImagePtr> images;
    for (std::list<ImageLayerDesc>::const_iterator it = needed.begin(); it != needed.end(); ++it) {
        // ImageLayerDesc orders by layer ID only, so the map also finds a stored layer with a different layout
        // (rgba for alpha); the consumer must get exactly the channels it asked for, as the pull would render them.
        std::map<ImageLayerDesc, ImagePtr>::const_iterator foundLayer = entry->layers.find(*it);
        if ((foundLayer == entry->layers.end()) || !foundLayer->second || (foundLayer->first.getChannels() != it->getChannels())) {
            return false;
        }
        images.push_back(foundLayer->second);
    }
    out->splice(out->end(), images);

    return true;
}

void
FrameStore::release(const TaskKey& key)
{
    std::shared_ptr<Entry> erased;
    {
        std::lock_guard<std::mutex> k(_entriesMutex);
        EntryMap::iterator found = _entries.find(key);
        if (found == _entries.end()) {
            return;
        }
        if (--found->second->consumersLeft > 0) {
            return;
        }
        erased.swap(found->second);
        _entries.erase(found);
        removeImageRefsLocked(*erased);
    }
}

std::size_t
FrameStore::bytesInFlight() const
{
    return _bytesInFlight;
}

void
FrameStore::clear()
{
    EntryMap erased;
    {
        std::lock_guard<std::mutex> k(_entriesMutex);
        erased.swap(_entries);
        _imageRefs.clear();
        _bytesInFlight = 0;
    }
}

FrameRenderContext::FrameRenderContext()
    : _argsMap()
    , _argsByHolder()
    , _rotoPaintTreeNodes()
    , _request()
    , _abortInfo()
    , _stats()
    , _time(0.)
    , _view(0)
    , _store()
{
}

FrameRenderContext::~FrameRenderContext()
{
}

FrameRenderContextPtr
FrameRenderContext::create(double time,
                           ViewIdx view,
                           bool isRenderUserInteraction,
                           bool isSequential,
                           const AbortableRenderInfoPtr& abortInfo,
                           const NodePtr& treeRoot,
                           int textureIndex,
                           const TimeLine* timeline,
                           const NodePtr& activeRotoPaintNode,
                           bool isAnalysis,
                           bool draftMode,
                           const RenderStatsPtr& stats)
{
    FrameRenderContextPtr ret(new FrameRenderContext());

    ret->_time = time;
    ret->_view = view;
    ret->_abortInfo = abortInfo;
    ret->_stats = stats;
    ParallelRenderArgsSetter::buildArgsMap(time, view, isRenderUserInteraction, isSequential, abortInfo, treeRoot, textureIndex, timeline,
                                           activeRotoPaintNode, isAnalysis, draftMode, stats, true /*setUpstreamArgs*/, OSGLContextPtr(),
                                           &ret->_argsMap);
    ret->indexArgs();

    return ret;
}

FrameRenderContextPtr
FrameRenderContext::createFromSetter(const ParallelRenderArgsSetter& setter,
                                     const AbortableRenderInfoPtr& abortInfo,
                                     const RenderStatsPtr& stats,
                                     double time,
                                     ViewIdx view)
{
    FrameRenderContextPtr ret(new FrameRenderContext());

    ret->_time = time;
    ret->_view = view;
    ret->_abortInfo = abortInfo;
    ret->_stats = stats;
    ret->_argsMap = setter.getInstalledArgs();
    ret->indexArgs();

    return ret;
}

void
FrameRenderContext::indexArgs()
{
    for (std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator it = _argsMap.begin(); it != _argsMap.end(); ++it) {
        EffectInstancePtr effect = it->first->getEffectInstance();
        if (effect) {
            _argsByHolder[effect->getTLSHolder()] = it->second;
        }
        if (it->second) {
            for (NodesList::const_iterator node = it->second->rotoPaintNodes.begin(); node != it->second->rotoPaintNodes.end(); ++node) {
                _rotoPaintTreeNodes.insert(node->get());
            }
        }
    }
}

void
FrameRenderContext::setRequest(const std::shared_ptr<FrameRequestMap>& request)
{
    _request = request;
    if (!request) {
        return;
    }
    for (std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator it = _argsMap.begin(); it != _argsMap.end(); ++it) {
        FrameRequestMap::const_iterator found = request->find(it->first);
        if (found != request->end()) {
            it->second->request = found->second;
        }
    }
}

ParallelRenderArgsPtr
FrameRenderContext::getArgs(const NodePtr& node) const
{
    std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator found = _argsMap.find(node);

    return found == _argsMap.end() ? ParallelRenderArgsPtr() : found->second;
}

ParallelRenderArgsPtr
FrameRenderContext::getArgsForHolder(const TLSHolderBase* holder) const
{
    std::unordered_map<const TLSHolderBase*, ParallelRenderArgsPtr>::const_iterator found = _argsByHolder.find(holder);

    return found == _argsByHolder.end() ? ParallelRenderArgsPtr() : found->second;
}

bool
FrameRenderContext::isRotoPaintTreeNode(const Node* node) const
{
    return node && (_rotoPaintTreeNodes.find(node) != _rotoPaintTreeNodes.end());
}

NATRON_NAMESPACE_EXIT
