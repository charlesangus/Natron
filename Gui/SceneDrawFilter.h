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

#ifndef NATRON_GUI_SCENEDRAWFILTER_H
#define NATRON_GUI_SCENEDRAWFILTER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/filteringSceneIndex.h>
#ifdef NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#undef NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief Hides geometry and lights from what one engine's render index sees, by subtree, without
 * touching the stage.
 *
 * A hidden rprim, instancer or light keeps its place in this scene index with an empty type and no
 * data source, the way HdsiPrimTypePruningSceneIndex (pxr/imaging/hdsi/primTypePruningSceneIndex.h)
 * preserves hierarchy. The render index holds nothing for an empty type, so the prim leaves Hydra
 * and its memory goes with it. Every other prim passes through, because drawn geometry keeps needing
 * a material or camera that lives under a boxed row. That rules out a RemovedPrimEntry, which takes
 * the whole subtree, and so a hide or a show announces each prim: HdsiPrefixPathPruningSceneIndex is
 * out too, since a prefix is its only unit of change and it cannot release part of a subtree.
 *
 * Only a row drawn in full, or a Children row's own prim, lights the scene: the same rule that hides
 * geometry takes the light away.
 *
 * Every method that changes state sends notices that reach the render index directly, so callers
 * hold the viewer lock and the GL context, between renders. Hydra reads GetPrim() from its sync
 * threads, which is safe only because nothing changes during a render.
 */
class SceneDrawFilter final
    : public PXR_NS::HdSingleInputFilteringSceneIndexBase {
public:
    using Clock = std::chrono::steady_clock;
    using WeakPtr = PXR_NS::TfWeakPtr<SceneDrawFilter>;

    enum RuleEnum {
        eRuleShow,
        eRuleHide,
        // The prim itself shows, and everything below without a rule of its own hides.
        eRuleExpand
    };

    using Rules = std::map<PXR_NS::SdfPath, RuleEnum>;

    // Must run before the render index it is to reach is constructed.
    static void registerForAllRenderers();

    // The renderInstanceId of the last filter appended on this thread, and forgets it.
    static std::string takeAppendedInstanceId();

    static WeakPtr find(const std::string& renderInstanceId);

    static PXR_NS::TfRefPtr<SceneDrawFilter> New(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    static void applyToRules(const SceneDrawDelta& delta, Rules* rules);

    ~SceneDrawFilter() override;

    PXR_NS::HdSceneIndexPrim GetPrim(const PXR_NS::SdfPath& primPath) const override;
    PXR_NS::SdfPathVector GetChildPrimPaths(const PXR_NS::SdfPath& primPath) const override;

    /**
     * @brief The rules a filter on a new engine starts from. It sends no notices, so it belongs
     * before the first render has populated anything.
     */
    void seedRules(const Rules& rules);

    /**
     * @brief Hides at once what the delta hides. What it shows goes out now if revealBatch is 0, and
     * otherwise through stepReveal(). A reveal under way stops: what it announced hides again, and
     * whatever the new rules still show is revealed from the start.
     */
    void applyDelta(const SceneDrawDelta& delta, std::size_t revealBatch = 0);

    /** @brief Announces up to maxPrims more prims of the reveal under way. */
    std::size_t stepReveal(std::size_t maxPrims);
    bool isRevealing() const { return _revealing; }

    const Rules& getRules() const { return _rules; }
    unsigned long long getInputAddedCount() const { return _inputAdded; }
    unsigned long long getInputRemovedCount() const { return _inputRemoved; }
    unsigned long long getInputDirtiedCount() const { return _inputDirtied; }
    unsigned long long getHiddenSentCount() const { return _hiddenSent; }
    unsigned long long getShownSentCount() const { return _shownSent; }
    Clock::time_point getLastInputAddedTime() const { return _lastInputAdded; }

protected:
    void _PrimsAdded(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    explicit SceneDrawFilter(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    bool isHidden(const PXR_NS::SdfPath& path) const;
    bool mayChangeBelow(const PXR_NS::SdfPath& path, bool toHidden) const;
    void finishRevealIfDone();

    Rules _rules;
    // While a reveal runs, _revealFrom holds the rules it started from: a prim those hid and _rules
    // show stays hidden until it is in _revealed.
    bool _revealing = false;
    Rules _revealFrom;
    std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> _revealed;
    std::vector<PXR_NS::SdfPath> _revealStack;
    std::vector<PXR_NS::SdfPath> _revealRoots;

    unsigned long long _inputAdded = 0;
    unsigned long long _inputRemoved = 0;
    unsigned long long _inputDirtied = 0;
    unsigned long long _hiddenSent = 0;
    unsigned long long _shownSent = 0;
    Clock::time_point _lastInputAdded;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWFILTER_H
