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
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
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
 * A hidden rprim, instancer or light is either out of Hydra or held invisible in it.
 *
 * Out of Hydra, it keeps its place in this scene index with an empty type and no data source, the
 * way HdsiPrimTypePruningSceneIndex (pxr/imaging/hdsi/primTypePruningSceneIndex.h) preserves
 * hierarchy. The render index holds nothing for an empty type, so its memory goes. Everything under a
 * Hidden row is out of Hydra, and so is a prim Hydra has never held, such as one under a boxed row
 * since the scene opened or since its payload loaded.
 *
 * Held invisible, it keeps its type and data with an HdVisibilitySchema of false on top. That is
 * what a prim gets when a boxed row hides it while Hydra holds it, and showing it again only flips
 * it back. Adding or removing an rprim makes Storm gather its draw items and rebuild its draw batches
 * again, at a cost that grows with every draw item in the scene (hd/changeTracker.h
 * GetRprimIndexVersion); a visibility change reaches HdStCommandBuffer::SyncDrawItemVisibility
 * instead (hdSt/commandBuffer.h) and keeps the batches. A Hidden row then takes the prim out.
 *
 * Every other prim passes through, because drawn geometry keeps needing a material or camera that
 * lives under a boxed row. That rules out a RemovedPrimEntry, which takes the whole subtree, and so a
 * hide or a show announces each prim: HdsiPrefixPathPruningSceneIndex is out too, since a prefix is
 * its only unit of change and it cannot release part of a subtree.
 *
 * Lights stop only under a Hidden row. A BBox row, or a row boxed by a Children parent, hides
 * geometry and leaves its lights, so drilling into a scene never darkens it.
 *
 * A native instance draws through an instancer far from its row, under /UsdNiPropagatedPrototypes
 * (usdImaging/niPrototypePropagatingSceneIndex.h), which names the instance among its
 * instanceLocations. So a row over an instance turns that instance off in the instancer's mask
 * (hd/instancerTopologySchema.h), and the instancer and its prototypes stay in Hydra, Hidden or not.
 *
 * The notices of one call go out together, once per prim, and a prim that ends where it started
 * sends none. They reach the render index directly, so callers hold the viewer lock and the GL
 * context, between renders. Hydra reads GetPrim() from its sync threads, which is safe only because
 * nothing changes during a render.
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
        eRuleExpand,
        // As eRuleHide, and lights below go too.
        eRuleHidden
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
     * otherwise through stepReveal(). A reveal under way carries on under the new rules: what they
     * hide goes at once, and what it announced and they still show stays.
     */
    void applyDelta(const SceneDrawDelta& delta, std::size_t revealBatch = 0);

    /** @brief As applyDelta() on each in turn, with the notices of all of them sent at once. */
    void applyDeltas(const std::vector<const SceneDrawDelta*>& deltas, std::size_t revealBatch = 0);

    /** @brief Announces up to maxPrims more prims of the reveal under way. */
    std::size_t stepReveal(std::size_t maxPrims);
    bool isRevealing() const { return _revealing; }

    using LightPredicate = std::function<bool(const PXR_NS::SdfPath&)>;

    /**
     * @brief Whether any light of the input scene that `counts` accepts (every one, if empty) is
     * drawn: one the rules keep, or a reveal has announced. The input also carries the renderer's
     * own lights, such as a headlight, which no row governs; a caller deciding whether to add one
     * must leave them out, or adding it would reverse the decision. Only the thread that changes
     * the filter may call it.
     */
    bool hasDrawnLights(const LightPredicate& counts = LightPredicate()) const;

    /** @brief Whether a Hidden row, or a reveal from one, keeps lights from Hydra. */
    bool hidesLights() const;

    const Rules& getRules() const { return _rules; }
    unsigned long long getInputAddedCount() const { return _inputAdded; }
    unsigned long long getInputRemovedCount() const { return _inputRemoved; }
    unsigned long long getInputDirtiedCount() const { return _inputDirtied; }
    unsigned long long getHiddenSentCount() const { return _hiddenSent; }
    unsigned long long getShownSentCount() const { return _shownSent; }
    // The hides and shows that went as a visibility change, with the prim kept in Hydra.
    unsigned long long getVisibilitySentCount() const { return _visibilitySent; }
    std::size_t getHeldInvisibleCount() const { return _invisibleCount; }
    // Native instances the rules turn off in their instancer's mask.
    std::size_t getMaskedInstanceCount() const { return _maskedCount; }
    Clock::time_point getLastInputAddedTime() const { return _lastInputAdded; }

protected:
    void _PrimsAdded(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const PXR_NS::HdSceneIndexBase& sender, const PXR_NS::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    enum OutputEnum : unsigned char {
        eOutputShown,
        eOutputInvisible,
        eOutputAbsent
    };

    using RulePtr = const Rules::value_type*;

    // The input's hierarchy, kept from its notices, so that a change walks the prims a row governs
    // without reading them through the input, which costs microseconds a prim on a stage.
    struct Node {
        const PXR_NS::SdfPath* path = nullptr;
        PXR_NS::TfToken type;
        Node* parent = nullptr;
        Node* firstChild = nullptr;
        Node* lastChild = nullptr;
        Node* prev = nullptr;
        Node* next = nullptr;
        // What Hydra holds. It lags the rules only where a reveal has yet to arrive.
        OutputEnum output = eOutputShown;
        OutputEnum before = eOutputShown;
        bool hideable = false;
        bool light = false;
        bool pending = false;
    };

    // now and was are the rules governing node in _rules and _revealFrom; belowNow and belowWas say
    // whether those have a rule under it, without which its descendants all share its rule.
    struct Frame {
        Node* node;
        Node* next;
        RulePtr now;
        RulePtr was;
        bool belowNow;
        bool belowWas;
        bool entered;
        // Left by an earlier change, so it cannot tell where the rules changed and looks at every
        // prim the rules may show.
        bool structural;
    };

    // An instancer that names the prims it draws. hidden is indexed as its instanceLocations.
    struct Instancer {
        std::vector<PXR_NS::SdfPath> locations;
        std::vector<bool> hidden;
        std::size_t hiddenCount = 0;
        bool pending = false;
    };

    explicit SceneDrawFilter(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    void readInstancer(const PXR_NS::SdfPath& path, const PXR_NS::HdSceneIndexPrim& prim);
    void forgetInstancer(const PXR_NS::SdfPath& path);
    PXR_NS::HdSceneIndexPrim withMask(const PXR_NS::SdfPath& primPath, PXR_NS::HdSceneIndexPrim prim) const;
    bool setInstanceHidden(const PXR_NS::SdfPath& instancer, Instancer& record, std::size_t index, bool hidden);
    void updateInstances(const std::vector<PXR_NS::SdfPath>& roots);

    Node& nodeAt(const PXR_NS::SdfPath& path);
    Node* indexPrim(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& primType);
    void forget(Node* top);
    void setOutput(Node& node, OutputEnum output);
    void transition(Node& node, OutputEnum after);
    Frame rootFrame(Node* node, bool structural) const;
    Frame childFrame(const Frame& parent, Node* child) const;
    bool descends(const Frame& frame, unsigned mode) const;
    std::size_t visit(const Frame& frame, unsigned mode);
    std::size_t walk(std::vector<Frame>* stack, unsigned mode, std::size_t maxShown);
    void finishRevealIfDone();
    void sendChanges();

    Rules _rules;
    // The rules before the last change, while its reveal runs.
    bool _revealing = false;
    Rules _revealFrom;
    std::vector<Frame> _revealStack;

    std::unordered_map<PXR_NS::SdfPath, Node, PXR_NS::SdfPath::Hash> _nodes;
    Node* _root = nullptr;

    // Every light the input has announced, whether drawn or not.
    std::unordered_set<const Node*> _lights;

    std::size_t _invisibleCount = 0;

    std::unordered_map<PXR_NS::SdfPath, Instancer, PXR_NS::SdfPath::Hash> _instancers;
    // Each instance location to its instancer and its index there, sorted so that a changed row finds
    // the instances below it without looking at the others.
    std::multimap<PXR_NS::SdfPath, std::pair<PXR_NS::SdfPath, std::size_t>> _locations;
    std::size_t _maskedCount = 0;

    // What a call has changed so far, sent by sendChanges().
    std::vector<Node*> _changes;
    std::vector<PXR_NS::SdfPath> _maskChanges;

    unsigned long long _inputAdded = 0;
    unsigned long long _inputRemoved = 0;
    unsigned long long _inputDirtied = 0;
    unsigned long long _hiddenSent = 0;
    unsigned long long _shownSent = 0;
    unsigned long long _visibilitySent = 0;
    Clock::time_point _lastInputAdded;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWFILTER_H
