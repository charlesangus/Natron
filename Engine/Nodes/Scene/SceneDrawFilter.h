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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneViewBudget.h"
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
 * So a reveal brings every prim it will show that Hydra lacks in at once, held invisible, and its
 * steps only flip visibility: the batches are rebuilt once a reveal, not once a step. Storm still
 * syncs an invisible prim in full the first time (HdRprim::CanSkipDirtyBitPropagationAndSync), so
 * that first frame pays for the whole row. Lights are sprims, which no draw batch holds, and come
 * with the steps as before.
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
 * A row inside a native instance names an instance proxy, which Hydra never holds: the instance's
 * prims draw from the instancer's one prototype, whose root stands for the instance's own prim. Such
 * a row governs the matching prim of the prototype, nested instances in turn. A prototype is shared
 * by every instance of the instancer, so where their rows disagree on a prim the one that draws least
 * wins (Hidden, then BBox, then Children, then Full): one copy of a shared prototype cannot be hidden
 * alone without touching the stage. A Children row on the instance itself then governs the
 * prototype's root rather than turning the instance off.
 *
 * A polygon budget lies over the rules as a layer of its own, bound on Storm's filter alone: it holds
 * the gprims it demotes or culls invisible, turns instances off in their instancer's mask, a
 * PointInstancer's by index and a native one's when any gprim below it goes, and omits a fractioned
 * mesh's faces through a geomSubset child whose visibility is false, which the scene index adapter
 * turns into the topology's invisible faces (HdMeshTopologySchema has none of its own). It only
 * takes away from what the rules draw, and clearing it leaves exactly what they draw.
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

    // An engine built on this thread while one of these lives gets a filter; any other engine,
    // whichever thread builds it meanwhile, gets none. A Cycles engine built through
    // CyclesAovs::withThreadBudget() still counts: task_arena::execute() runs the build on the
    // calling thread, the arena keeping a slot for it.
    class EngineScope {
    public:
        EngineScope();
        ~EngineScope();
        EngineScope(const EngineScope&) = delete;
        EngineScope& operator=(const EngineScope&) = delete;

        // The renderInstanceId of the filter the engine built under this scope got, or empty.
        std::string getInstanceId() const;

        void setInstanceId(const std::string& renderInstanceId);

    private:
        EngineScope* _previous;
        std::string _instanceId;
    };

    static WeakPtr find(const std::string& renderInstanceId);

    // What a polygon budget takes from what the rules draw.
    struct Budget {
        // Gprims demoted or culled.
        std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> hidden;
        // By PointInstancer, indexed as its instances: true turns the instance off.
        std::unordered_map<PXR_NS::SdfPath, std::vector<bool>, PXR_NS::SdfPath::Hash> instances;
        // By mesh, the faces it leaves out, ascending.
        std::unordered_map<PXR_NS::SdfPath, PXR_NS::VtIntArray, PXR_NS::SdfPath::Hash> invisibleFaces;

        bool empty() const { return hidden.empty() && instances.empty() && invisibleFaces.empty(); }
    };

    // The instances a result demotes, culls or fractions out all go from Storm, the demoted and
    // fractioned ones to be drawn as budget boxes. A mesh's face count is its unit's.
    static Budget budgetFrom(const SceneViewBudget::Result& result,
                             const std::vector<SceneViewBudget::Unit>& units);

    // The name of the geomSubset child that carries a fractioned mesh's omitted faces.
    static const PXR_NS::TfToken& budgetFacesChildName();

    // The renderInstanceIds of every live filter.
    static std::vector<std::string> getInstanceIds();

    static PXR_NS::TfRefPtr<SceneDrawFilter> New(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    static void applyToRules(const SceneDrawDelta& delta, Rules* rules);

    // Whether the rule governing path, its own or its nearest ancestor's, is Hidden.
    static bool isUnderHiddenRow(const Rules& rules, const PXR_NS::SdfPath& path);

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
     * hide goes at once and what it announced and they still show stays. At or below a changed row,
     * the change's own reveal takes the place of what was left of it there; elsewhere the rules
     * have not changed, so what is left there still comes, after what this delta shows.
     *
     * With a non-zero revealBatch, what the delta's loads bring in joins a reveal as it arrives
     * instead of being drawn all at once, until a delta with revealBatch 0 comes. Either way, what a
     * reveal is to show reaches Hydra invisible straight away.
     */
    void applyDelta(const SceneDrawDelta& delta, std::size_t revealBatch = 0);

    /**
     * @brief Moves the filter from its rules to these, sending only what the difference changes: a
     * prim whose state ends where it began sends nothing, and the same rules send nothing at all. No
     * rules show everything.
     */
    void replaceRules(const Rules& rules);

    /** @brief As applyDelta() on each in turn, with the notices of all of them sent at once. */
    void applyDeltas(const std::vector<const SceneDrawDelta*>& deltas, std::size_t revealBatch = 0);

    /**
     * @brief Announces up to maxPrims more prims of the reveal under way, the most recent change
     * first and each subtree depth first in the input's order. It stops early at the deadline, which
     * bounds the prims it looks at where it finds few to show.
     */
    std::size_t stepReveal(std::size_t maxPrims, Clock::time_point deadline = (Clock::time_point::max)());
    bool isRevealing() const { return _revealing; }
    // Whether the last stepReveal() stopped at its deadline before its prim count.
    bool wasStepCut() const { return _stepCut; }

    /**
     * @brief Marks a render's end. A prim the input takes away and adds again within one render, as a
     * resync does, comes back as it was: drawn if Hydra drew it, rather than joining a reveal.
     */
    void endRender();

    /**
     * @brief Has geometry reach Hydra only as admit() lets it in, so no render syncs more of it than
     * a batch: Storm syncs an invisible prim in full the first time it holds it, so only a prim it
     * does not hold yet costs a render nothing.
     *
     * While admitting, an rprim or instancer the rules show and Hydra does not hold yet stays out of
     * Hydra, as a Hidden row keeps it, and waits its turn; so does a native instance the input names,
     * masked in its instancer, and what a load brings in, which gives no reveal. Lights and every
     * prim that is neither pass through. The input's changes to a waiting prim are dropped, as Hydra
     * reads it whole when it comes in. What Hydra holds keeps following the rules at once.
     *
     * Turning it off lets everything waiting in at once.
     */
    void setAdmitting(bool admitting);
    bool isAdmitting() const { return _admitting; }

    // Rows, nearest the eye first.
    using Order = std::shared_ptr<const std::vector<PXR_NS::SdfPath>>;

    /**
     * @brief Lets up to maxPrims waiting prims into Hydra, those at or below each row of order in
     * turn and then the rest in the input's order, and returns how many came in. An instancer comes
     * with every prototype it draws, as one unit, which may take it past maxPrims; at least one unit
     * comes in each call. kInstancesPerPrim waiting instances count as one prim. It stops early at the
     * deadline.
     *
     * A row is taken up again in later calls until nothing at or below it waits; another order, or a
     * change of rules, starts again from its first row.
     */
    std::size_t admit(const Order& order,
                      std::size_t maxPrims,
                      Clock::time_point deadline = (Clock::time_point::max)());

    static constexpr std::size_t kInstancesPerPrim = 64;

    // Prims and native instances waiting to be admitted.
    std::size_t pendingCount() const { return _root->queuedBelow + _waitingCount; }
    // Admitted since nothing was last waiting.
    std::size_t admittedCount() const { return _admitted; }
    // How many rows of the last order admit() was given it has come to, the one it is in included.
    std::size_t orderReached() const { return _order ? (std::min)(_orderNext + 1, _order->size()) : 0; }
    bool isFilling() const { return pendingCount() != 0; }
    // Whether anything at or below path waits to be admitted.
    bool hasPendingAt(const PXR_NS::SdfPath& path) const;

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

    /**
     * @brief Whether the input scene has a light that `counts` accepts (every one, if empty) and
     * none of them is drawn: a scene whose lights the rules all keep from Hydra, as opposed to one
     * that has none. Only the thread that changes the filter may call it.
     */
    bool hidesEveryLight(const LightPredicate& counts = LightPredicate()) const;

    /**
     * @brief Lets applyBudget() act. Only the filter of a Storm engine is bound; a render delegate's
     * follows the rules alone. Unbinding clears the budget.
     */
    void setBudgetBound(bool bound);
    bool isBudgetBound() const { return _budgetBound; }

    /**
     * @brief Replaces the budget layer, sending only what the difference changes. Does nothing on a
     * filter that is not bound. Holding invisible keeps Storm's draw batches, which adding and
     * removing prims would gather again.
     */
    void applyBudget(const Budget& budget);
    void clearBudget() { applyBudget(Budget()); }

    // Gprims Hydra holds that the budget alone makes invisible.
    std::size_t getBudgetHiddenCount() const;
    // Instances the budget turns off, whether or not a rule does too.
    std::size_t getBudgetMaskedInstanceCount() const { return _budgetMaskedCount; }
    std::size_t getBudgetFacedMeshCount() const { return _budgetFaces.size(); }
    // Empty for a mesh the budget leaves whole.
    PXR_NS::VtIntArray getBudgetInvisibleFaces(const PXR_NS::SdfPath& mesh) const;
    // Whether the instancer's mask as this filter serves it turns the instance off.
    bool isInstanceMasked(const PXR_NS::SdfPath& instancer, std::size_t index) const;

    // The rules as given, by stage path.
    const Rules& getRules() const { return _stageRules; }
    // The rules as they reach Hydra's prims, rows inside native instances moved onto their prototypes.
    const Rules& getEffectiveRules() const { return _rules; }
    unsigned long long getInputAddedCount() const { return _inputAdded; }
    unsigned long long getInputRemovedCount() const { return _inputRemoved; }
    unsigned long long getInputDirtiedCount() const { return _inputDirtied; }
    unsigned long long getHiddenSentCount() const { return _hiddenSent; }
    unsigned long long getShownSentCount() const { return _shownSent; }
    // The hides and shows that went as a visibility change, with the prim kept in Hydra.
    unsigned long long getVisibilitySentCount() const { return _visibilitySent; }
    // Prims this filter brought into Hydra, shown or held invisible for a reveal. With
    // getInputAddedCount(), it tells a frame that rebuilt the draw batches from one that did not.
    unsigned long long getInsertedSentCount() const { return _insertedSent; }
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
        bool arriving = false;
        // Waiting to be admitted.
        bool queued = false;
        // Queued nodes at or below this one.
        std::size_t queuedBelow = 0;
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
        // Instances not admitted yet, masked as hidden ones are. None before waitingFrom.
        std::vector<bool> waiting;
        std::size_t waitingCount = 0;
        std::size_t waitingFrom = 0;
        // What the instancer draws, admitted with it.
        std::vector<PXR_NS::SdfPath> prototypes;
        bool pending = false;
    };

    explicit SceneDrawFilter(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    void readInstancer(const PXR_NS::SdfPath& path, const PXR_NS::HdSceneIndexPrim& prim);
    void forgetInstancer(const PXR_NS::SdfPath& path);
    PXR_NS::HdSceneIndexPrim withMask(const PXR_NS::SdfPath& primPath, PXR_NS::HdSceneIndexPrim prim) const;
    PXR_NS::HdSceneIndexPrim withBudget(const PXR_NS::SdfPath& primPath, PXR_NS::HdSceneIndexPrim prim) const;
    // The native instance a path lies under, as its instancer and index, or null.
    const std::pair<PXR_NS::SdfPath, std::size_t>* nativeInstanceOf(const PXR_NS::SdfPath& path) const;
    bool hasBudgetLayer() const { return !_budgetHidden.empty() || !_budgetMasks.empty() || !_budgetFaces.empty(); }
    bool setInstanceHidden(const PXR_NS::SdfPath& instancer, Instancer& record, std::size_t index, bool hidden);
    void setInstanceWaiting(const PXR_NS::SdfPath& instancer, Instancer& record, std::size_t index, bool waiting);
    void queueMaskChange(const PXR_NS::SdfPath& instancer, Instancer& record);
    void updateInstances(const std::vector<PXR_NS::SdfPath>& roots);
    // An instancer whose instances all draw one prototype root, as a native instancer's do.
    static bool hasOnePrototype(const Instancer& record) { return !record.locations.empty() && (record.prototypes.size() == 1); }
    bool instanceOffBy(const Rules& rules, const Instancer& record, std::size_t index) const;
    Rules effectiveRules(const Rules& stageRules) const;
    /**
     * @brief The prim of Hydra that draws a stage path, through every native instance it lies in:
     * the path itself outside any. Empty where a prototype on the way draws more than one instance
     * or an instancer has no single prototype, with *shared set to the outermost such instance.
     */
    PXR_NS::SdfPath prototypePathOf(const PXR_NS::SdfPath& path, std::pair<PXR_NS::SdfPath, std::size_t>* shared) const;
    // Moves the filter to the effective rules, walking the changed rows. fromInput leaves a reveal
    // and the load arrivals as they are, for a change the input's instancers made.
    void applyRules(Rules effective,
                    const std::set<PXR_NS::SdfPath>& roots,
                    const std::vector<const SceneDrawDelta*>& deltas,
                    std::size_t revealBatch,
                    bool fromInput);
    // The instancers changed what the rows inside native instances govern.
    void refreshRules();

    bool isFiltering() const { return !_rules.empty() || _revealing || (_root->queuedBelow != 0) || (_waitingCount != 0); }
    bool queues(const Node& node) const { return _admitting && node.hideable && !node.light; }
    void enqueue(Node& node);
    void dequeue(Node& node);
    void admitNode(Node& node);
    Node* unitOwner(Node& node);
    std::size_t admitUnit(Node& node);
    std::size_t admitBelow(Node& top, std::size_t budget, Clock::time_point deadline);
    std::size_t admitAt(const PXR_NS::SdfPath& path, std::size_t budget, Clock::time_point deadline);
    std::size_t admitInstance(const PXR_NS::SdfPath& instancer, Instancer& record, std::size_t index);
    std::size_t admitAnyInstances(std::size_t budget, Clock::time_point deadline);
    using Locations = std::multimap<PXR_NS::SdfPath, std::pair<PXR_NS::SdfPath, std::size_t>>;
    Locations::const_iterator scanStart(const PXR_NS::SdfPath& path) const;

    Node& nodeAt(const PXR_NS::SdfPath& path);
    Node* indexPrim(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& primType);
    void forget(Node* top);
    void setOutput(Node& node, OutputEnum output);
    void transition(Node& node, OutputEnum after);
    Frame rootFrame(Node* node, bool structural) const;
    Frame childFrame(const Frame& parent, Node* child) const;
    bool descends(const Frame& frame, unsigned mode) const;
    bool hasLightBelow(const PXR_NS::SdfPath& path) const;
    std::size_t visit(const Frame& frame, unsigned mode);
    std::size_t walk(std::vector<Frame>* stack, unsigned mode, std::size_t maxShown, Clock::time_point deadline);
    void updateRevealRoots(const std::vector<const SceneDrawDelta*>& deltas, const std::vector<PXR_NS::SdfPath>& changed, std::size_t revealBatch);
    bool defersArrival(const Node& node, bool known, OutputEnum before) const;
    void finishRevealIfDone();
    void sendChanges();

    Rules _stageRules;
    Rules _rules;
    bool _instancersChanged = false;
    // The rules before the last change, while its reveal runs.
    bool _revealing = false;
    bool _stepCut = false;
    Rules _revealFrom;
    // Frontiers, each a path down from where a reveal started, so the reveal keeps no record of the
    // prims it has shown.
    std::vector<Frame> _revealStack;

    // Where what loads in is revealed rather than sent at once.
    std::set<PXR_NS::SdfPath> _arrivalRoots;
    // Prims Hydra drew when the input took them away during this render.
    std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> _removedHeld;

    std::unordered_map<PXR_NS::SdfPath, Node, PXR_NS::SdfPath::Hash> _nodes;
    Node* _root = nullptr;

    // Every light the input has announced, whether drawn or not. The paths are sorted so that a walk
    // under a boxed row, which leaves lights, goes only where one is.
    std::unordered_set<const Node*> _lights;
    std::set<PXR_NS::SdfPath> _lightPaths;

    std::size_t _invisibleCount = 0;

    std::unordered_map<PXR_NS::SdfPath, Instancer, PXR_NS::SdfPath::Hash> _instancers;
    // Each instance location to its instancer and its index there, sorted so that a changed row finds
    // the instances below it without looking at the others.
    Locations _locations;
    // Per row asked about, where its last scan of _locations stopped: no instance below the row
    // and before that point waits. Without it, every batch of a fill would walk again past all
    // the instances admitted so far. Cleared whenever _locations changes or an instance starts
    // waiting.
    mutable std::map<PXR_NS::SdfPath, Locations::const_iterator> _scannedTo;
    std::size_t _maskedCount = 0;
    // Each prototype an instancer names to that instancer.
    std::map<PXR_NS::SdfPath, PXR_NS::SdfPath> _prototypeOwners;

    bool _admitting = false;
    std::size_t _waitingCount = 0;
    std::size_t _admitted = 0;
    // Instances admitted toward the next prim they count as.
    std::size_t _instanceCredit = 0;
    Order _order;
    std::size_t _orderNext = 0;

    // What a call has changed so far, sent by sendChanges().
    std::vector<Node*> _changes;
    std::vector<PXR_NS::SdfPath> _maskChanges;

    bool _budgetBound = false;
    std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> _budgetHidden;
    // By instancer, PointInstancer or native, indexed as its mask: true turns the instance off.
    std::unordered_map<PXR_NS::SdfPath, std::vector<bool>, PXR_NS::SdfPath::Hash> _budgetMasks;
    std::unordered_map<PXR_NS::SdfPath, PXR_NS::VtIntArray, PXR_NS::SdfPath::Hash> _budgetFaces;
    std::size_t _budgetMaskedCount = 0;

    unsigned long long _inputAdded = 0;
    unsigned long long _inputRemoved = 0;
    unsigned long long _inputDirtied = 0;
    unsigned long long _hiddenSent = 0;
    unsigned long long _shownSent = 0;
    unsigned long long _visibilitySent = 0;
    unsigned long long _insertedSent = 0;
    Clock::time_point _lastInputAdded;
};

/**
 * @brief Sizes a reveal's batches from what the frames that showed them cost, so a modest machine and
 * a many-core one both keep near the frame time they aim for.
 *
 * A frame costs a fixed part and a part per prim shown: the batch is what the target leaves after
 * the fixed part, so a frame predicted as fixed + per prim x batch stays within the target. The
 * per-prim slope is fitted to the last few frames once their sizes vary enough to tell the parts
 * apart; the fixed part is what the latest frame cost beyond that slope, as it grows during a fill
 * with the prims Hydra holds (Storm walks every rprim it holds when a prim first turns varying:
 * HdDirtyList, HdChangeTracker::ResetVaryingState). A frame cannot be stopped midway. Where the
 * fixed part alone is at or beyond the target, no batch fits it, and a minBatch-sized one would take
 * far too many passes: the batch is then sized so its prim work is at most the fixed part, so a pass
 * costs at most about twice the fixed part and the fill takes a number of passes that does not depend
 * on the prim count. Where the fixed part is under the target but leaves less than minBatch prims'
 * worth, the batch is minBatch and does not grow.
 *
 * Until the frames have varied enough in size to tell the parts apart, the batch doubles after a
 * frame within the target and halves after one beyond it. It is never zero, so a fill always moves.
 */
class SceneRevealPacer {
public:
    static constexpr std::size_t kMinBatch = 64;

    explicit SceneRevealPacer(double targetMs = 33., std::size_t firstBatch = 1024, std::size_t minBatch = kMinBatch);

    std::size_t nextBatch() const { return _batch; }
    // How long a frame showing prims took, from the step to the end of its render.
    void observe(std::size_t prims, double ms);
    double getFixedMs() const { return _fixedMs; }
    double getPerPrimMs() const { return _perPrimMs; }

private:
    struct Sample {
        double prims;
        double ms;
    };

    static constexpr std::size_t kSamples = 8;

    double _targetMs;
    std::size_t _batch;
    std::size_t _minBatch;
    Sample _samples[kSamples];
    std::size_t _sampleCount = 0;
    std::size_t _nextSample = 0;
    double _fixedMs = 0.;
    double _perPrimMs = 0.;
    bool _fitted = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWFILTER_H
