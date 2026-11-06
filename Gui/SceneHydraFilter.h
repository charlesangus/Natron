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

#ifndef NATRON_GUI_SCENEHYDRAFILTER_H
#define NATRON_GUI_SCENEHYDRAFILTER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <chrono>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_SCENEHYDRAFILTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/filteringSceneIndex.h>
#ifdef NATRON_SCENEHYDRAFILTER_RESTORE_EMIT
#undef NATRON_SCENEHYDRAFILTER_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

// Removes subtrees from what one engine's render index sees, without touching the stage.
//
// HdsiPrefixPathPruningSceneIndex (pxr/imaging/hdsi/prefixPathPruningSceneIndex.h) has the same
// topology semantics and a public SetExcludePathPrefixes, but a prefix is its only unit of change:
// it has no way to announce part of a subtree, which a per-frame reveal needs on a parent with a
// million children.
//
// Every method that changes state sends notices that reach the render index directly, so callers
// hold the viewer lock and the GL context, between renders. Hydra reads GetPrim() from its sync
// threads, which is safe only because nothing changes during a render.
class SceneHydraFilter final
    : public PXR_NS::HdSingleInputFilteringSceneIndexBase {
public:
    using Clock = std::chrono::steady_clock;
    using WeakPtr = PXR_NS::TfWeakPtr<SceneHydraFilter>;

    // Must run before the render index it is to reach is constructed.
    static void registerForAllRenderers();

    // The renderInstanceId of the last filter appended on this thread, and forgets it.
    static std::string takeAppendedInstanceId();

    static WeakPtr find(const std::string& renderInstanceId);

    static PXR_NS::TfRefPtr<SceneHydraFilter> New(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    ~SceneHydraFilter() override;

    PXR_NS::HdSceneIndexPrim GetPrim(const PXR_NS::SdfPath& primPath) const override;
    PXR_NS::SdfPathVector GetChildPrimPaths(const PXR_NS::SdfPath& primPath) const override;

    void prune(const PXR_NS::SdfPath& path);
    void restore(const PXR_NS::SdfPath& path);

    // Only one reveal runs at a time; starting another cancels the first.
    bool beginReveal(const PXR_NS::SdfPath& path);
    std::size_t stepReveal(std::size_t maxPrims);
    void cancelReveal();
    bool isRevealing() const { return !_revealRoot.IsEmpty(); }

    const std::set<PXR_NS::SdfPath>& getPruned() const { return _pruned; }
    unsigned long long getInputAddedCount() const { return _inputAdded; }
    unsigned long long getInputRemovedCount() const { return _inputRemoved; }
    unsigned long long getInputDirtiedCount() const { return _inputDirtied; }
    unsigned long long getAddedSentCount() const { return _addedSent; }
    unsigned long long getRemovedSentCount() const { return _removedSent; }
    Clock::time_point getLastInputAddedTime() const { return _lastInputAdded; }

protected:
    void _PrimsAdded(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    explicit SceneHydraFilter(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    bool isHidden(const PXR_NS::SdfPath& path) const;

    std::set<PXR_NS::SdfPath> _pruned;
    // The reveal root stays in _pruned until the last prim is out: what it has announced so far is
    // the exception to the prefix, listed in _revealed.
    PXR_NS::SdfPath _revealRoot;
    std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> _revealed;
    std::vector<PXR_NS::SdfPath> _revealStack;

    unsigned long long _inputAdded = 0;
    unsigned long long _inputRemoved = 0;
    unsigned long long _inputDirtied = 0;
    unsigned long long _addedSent = 0;
    unsigned long long _removedSent = 0;
    Clock::time_point _lastInputAdded;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEHYDRAFILTER_H
