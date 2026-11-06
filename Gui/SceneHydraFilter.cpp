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

#include "Gui/SceneHydraFilter.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#ifdef emit
#define NATRON_SCENEHYDRAFILTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#ifdef NATRON_SCENEHYDRAFILTER_RESTORE_EMIT
#undef NATRON_SCENEHYDRAFILTER_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::mutex filtersMutex;
std::unordered_map<std::string, SceneHydraFilter::WeakPtr> filters;

// UNDOCUMENTED: the viewer cannot compute its engine's renderInstanceId. It is the string
// "UsdImagingGLEngine_%s_%p" in libusd_usdImagingGL.so, whose %p appears to be the engine, known
// only once the constructor has returned, and HdUtils::RenderInstanceTracker's register-first
// protocol (pxr/imaging/hd/utils.h) needs it earlier. So the id is learned here, which relies on
// UsdImagingGLEngine's constructor building its render index synchronously on the calling thread.
// hd/utils.h only says the callback "currently" runs during render index construction.
thread_local std::string appendedInstanceId;

HdSceneIndexBaseRefPtr
appendFilter(const std::string& renderInstanceId,
             const HdSceneIndexBaseRefPtr& input,
             const HdContainerDataSourceHandle& /*inputArgs*/)
{
    TfRefPtr<SceneHydraFilter> filter = SceneHydraFilter::New(input);
    {
        std::lock_guard<std::mutex> lock(filtersMutex);
        filters[renderInstanceId] = SceneHydraFilter::WeakPtr(filter);
    }
    appendedInstanceId = renderInstanceId;

    return filter;
}
} // anon namespace

void
SceneHydraFilter::registerForAllRenderers()
{
    static std::once_flag once;
    std::call_once(once, []() {
        // UNDOCUMENTED: the earliest insertion is assumed to put this ahead of the render
        // delegate's own filters; the registry header promises only relative order. Pruning
        // removes prims wherever it sits, so a later place would cost time, not correctness.
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
            appendFilter,
            HdContainerDataSourceHandle(),
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

std::string
SceneHydraFilter::takeAppendedInstanceId()
{
    std::string id;
    id.swap(appendedInstanceId);

    return id;
}

SceneHydraFilter::WeakPtr
SceneHydraFilter::find(const std::string& renderInstanceId)
{
    std::lock_guard<std::mutex> lock(filtersMutex);
    const auto it = filters.find(renderInstanceId);

    return (it != filters.end()) ? it->second : WeakPtr();
}

TfRefPtr<SceneHydraFilter>
SceneHydraFilter::New(const HdSceneIndexBaseRefPtr& input)
{
    return TfCreateRefPtr(new SceneHydraFilter(input));
}

SceneHydraFilter::SceneHydraFilter(const HdSceneIndexBaseRefPtr& input)
    : HdSingleInputFilteringSceneIndexBase(input)
{
}

SceneHydraFilter::~SceneHydraFilter() = default;

bool
SceneHydraFilter::isHidden(const SdfPath& path) const
{
    if (_pruned.empty()) {
        return false;
    }
    const std::set<SdfPath>::const_iterator prefix = SdfPathFindLongestPrefix(_pruned, path);
    if (prefix == _pruned.end()) {
        return false;
    }

    return !((*prefix == _revealRoot) && (_revealed.count(path) != 0));
}

HdSceneIndexPrim
SceneHydraFilter::GetPrim(const SdfPath& primPath) const
{
    return isHidden(primPath) ? HdSceneIndexPrim() : _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector
SceneHydraFilter::GetChildPrimPaths(const SdfPath& primPath) const
{
    if (isHidden(primPath)) {
        return SdfPathVector();
    }
    SdfPathVector children = _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    if (!_pruned.empty()) {
        children.erase(std::remove_if(children.begin(), children.end(), [this](const SdfPath& child) { return isHidden(child); }), children.end());
    }

    return children;
}

void
SceneHydraFilter::prune(const SdfPath& path)
{
    if (isRevealing() && (path.HasPrefix(_revealRoot) || _revealRoot.HasPrefix(path))) {
        cancelReveal();
    }
    const bool wasVisible = !isHidden(path);
    _pruned.insert(path);
    if (wasVisible) {
        HdSceneIndexObserver::RemovedPrimEntries entries;
        entries.push_back(HdSceneIndexObserver::RemovedPrimEntry(path));
        ++_removedSent;
        _SendPrimsRemoved(entries);
    }
}

void
SceneHydraFilter::restore(const SdfPath& path)
{
    if (beginReveal(path)) {
        stepReveal((std::numeric_limits<std::size_t>::max)());
    }
}

bool
SceneHydraFilter::beginReveal(const SdfPath& path)
{
    cancelReveal();
    if (_pruned.count(path) == 0) {
        return false;
    }
    if (!path.IsAbsoluteRootPath() && isHidden(path.GetParentPath())) {
        return false;
    }
    _revealRoot = path;
    _revealStack.assign(1, path);

    return true;
}

std::size_t
SceneHydraFilter::stepReveal(std::size_t maxPrims)
{
    if (!isRevealing()) {
        return 0;
    }
    const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
    HdSceneIndexObserver::AddedPrimEntries entries;
    // Depth first, so a prim is never announced before its parent. Children are read only as each
    // parent comes up, so a small batch does not pay for walking the whole subtree first.
    while (!_revealStack.empty() && (std::size_t(entries.size()) < maxPrims)) {
        const SdfPath path = _revealStack.back();
        _revealStack.pop_back();
        if ((path != _revealRoot) && (_pruned.count(path) != 0)) {
            continue;
        }
        _revealed.insert(path);
        entries.push_back(HdSceneIndexObserver::AddedPrimEntry(path, input->GetPrim(path).primType));
        const SdfPathVector children = input->GetChildPrimPaths(path);
        _revealStack.insert(_revealStack.end(), children.rbegin(), children.rend());
    }
    if (_revealStack.empty()) {
        _pruned.erase(_revealRoot);
        _revealRoot = SdfPath();
        _revealed.clear();
    }
    _addedSent += entries.size();
    const std::size_t sent = entries.size();
    if (sent != 0) {
        _SendPrimsAdded(entries);
    }

    return sent;
}

void
SceneHydraFilter::cancelReveal()
{
    if (!isRevealing()) {
        return;
    }
    const SdfPath root = _revealRoot;
    const bool announced = !_revealed.empty();
    _revealRoot = SdfPath();
    _revealed.clear();
    _revealStack.clear();
    if (announced) {
        HdSceneIndexObserver::RemovedPrimEntries entries;
        entries.push_back(HdSceneIndexObserver::RemovedPrimEntry(root));
        ++_removedSent;
        _SendPrimsRemoved(entries);
    }
}

void
SceneHydraFilter::_PrimsAdded(const HdSceneIndexBase& /*sender*/,
                              const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    _inputAdded += entries.size();
    _lastInputAdded = Clock::now();
    if (_pruned.empty()) {
        _SendPrimsAdded(entries);

        return;
    }
    HdSceneIndexObserver::AddedPrimEntries kept;
    for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
        if (!isHidden(entry.primPath)) {
            kept.push_back(entry);
        }
    }
    if (!kept.empty()) {
        _SendPrimsAdded(kept);
    }
}

void
SceneHydraFilter::_PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                                const HdSceneIndexObserver::RemovedPrimEntries& entries)
{
    _inputRemoved += entries.size();
    if (_pruned.empty()) {
        _SendPrimsRemoved(entries);

        return;
    }
    HdSceneIndexObserver::RemovedPrimEntries kept;
    for (const HdSceneIndexObserver::RemovedPrimEntry& entry : entries) {
        if (!isHidden(entry.primPath)) {
            kept.push_back(entry);
        }
    }
    if (!kept.empty()) {
        _SendPrimsRemoved(kept);
    }
}

void
SceneHydraFilter::_PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                                const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    _inputDirtied += entries.size();
    if (_pruned.empty()) {
        _SendPrimsDirtied(entries);

        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries kept;
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : entries) {
        if (!isHidden(entry.primPath)) {
            kept.push_back(entry);
        }
    }
    if (!kept.empty()) {
        _SendPrimsDirtied(kept);
    }
}

NATRON_NAMESPACE_EXIT
