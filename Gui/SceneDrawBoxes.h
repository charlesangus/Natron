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

#ifndef NATRON_GUI_SCENEDRAWBOXES_H
#define NATRON_GUI_SCENEDRAWBOXES_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Engine/Nodes/Scene/SceneDrawFilter.h"
#include "Engine/Nodes/Scene/SceneViewBudget.h"
#include "Global/GlobalDefines.h"

#ifdef emit
#define NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#ifdef NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#undef NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief The world bounds of the rows a draw filter boxes, computed on a thread of its own.
 *
 * A boxed row is one with a BBox rule, or a child without a rule of its own of a Children row. A
 * Hidden row has no box. Bounds come from UsdGeomBBoxCache with extentsHint, so an unloaded
 * payload whose model authors a hint has a box without its layers opening.
 *
 * The stage is read under the caller's lock in short holds: one ends when shouldYield says a writer
 * is waiting or after a few milliseconds, since a write waits for every reader. A change of rules or
 * of the stage drops the list in progress, so a list never mixes two of them; a change of time alone
 * lets it finish, and the next list takes the new time, so playback still sees boxes.
 *
 * A list is published whole. One that needs more than a hold goes out once early, the rows still
 * to come keeping their old box or showing as pending markers. Rows whose bounds cannot vary keep
 * their box over a time change, and every row keeps it over a rule change, so a frame change
 * recomputes only animated rows while the old boxes stay drawn.
 *
 * With counting on, the same thread also lists the units a polygon budget weighs: every gprim and
 * PointInstancer the rules draw, with its world box and the faces it draws, and every unloaded
 * payload among them, which draws nothing. A PointInstancer is one unit carrying its prototypes'
 * boxes and counts and its instances' authored arrays, never a box per instance. The units are
 * published whole and apart from the boxes, after them, so counting never delays a box. A
 * PointInstancer's arrays are read under the lock and its world bound and face count are taken from
 * them after the walk, off the lock, a chunk of instances at a time so a dropped pass stops soon.
 *
 * A budget, once asked for, is solved on the same thread against the units published for the
 * current request, and solved again with each new list of units until it is cancelled. A request or
 * a cancellation drops a solve still running: its result is never published.
 */
class SceneDrawBoxes {
public:
    enum KindEnum {
        eKindBox,
        // The row's payload is unloaded, and its box comes from an authored hint.
        eKindUnloaded,
        // No bounds without loading: min and max are both the row's origin.
        eKindMarker,
        // Not measured yet, at the row's origin.
        eKindPending
    };

    struct Box {
        PXR_NS::SdfPath path;
        PXR_NS::GfVec3f min;
        PXR_NS::GfVec3f max;
        KindEnum kind = eKindBox;
    };

    struct List {
        // Of the request it answers; requests are numbered from 1.
        U64 generation = 0;
        double time = 0.;
        // False while rows of a long pass are still pending.
        bool complete = true;
        std::vector<Box> boxes;
    };

    using ListPtr = std::shared_ptr<const List>;
    // A null stage means there is no scene. version changes with every edit of the stage.
    using StageRead = std::function<void(const PXR_NS::UsdStageRefPtr& stage, U64 version)>;
    // Calls read once, under the lock that keeps the stage from changing.
    using StageReader = std::function<void(const StageRead& read)>;
    // The rows to box on a stage under rules, called under the reader's lock.
    using RowLister = std::function<std::vector<PXR_NS::SdfPath>(const PXR_NS::UsdStageRefPtr& stage, const SceneDrawFilter::Rules& rules)>;

    using Prototype = SceneViewBudget::Prototype;
    using Instances = SceneViewBudget::Instances;
    using InstancesPtr = SceneViewBudget::InstancesPtr;
    // Its state is eStateDrawn, or eStateUnloaded for an unloaded payload.
    using Unit = SceneViewBudget::Unit;
    using Camera = SceneViewBudget::Camera;

    struct Units {
        // Of the request it answers, as List's.
        U64 generation = 0;
        // Bumped by a rules, stage or relist change, not by time.
        U64 contentGeneration = 0;
        double time = 0.;
        std::vector<Unit> units;
    };

    using UnitsPtr = std::shared_ptr<const Units>;

    // The budget boxes of one prototype of one instancer, its instances drawn one box each.
    struct BudgetBoxGroup {
        PXR_NS::SdfPath instancer;
        PXR_NS::SdfPath prototype;
        std::vector<std::size_t> indices;
    };

    // The instances too small on screen at the solve's camera to draw apart, merged by where they
    // lie into one world box each, as they would cover about the same pixels.
    struct BudgetBoxCluster {
        // The first instance merged, which a click on the box picks.
        PXR_NS::SdfPath instancer;
        PXR_NS::SdfPath prototype;
        std::size_t instance = 0;
        std::size_t count = 0;
        // Its box's in BudgetBoxSet::rows.
        std::size_t row = 0;
    };

    enum BudgetBoxRangeEnum {
        eBudgetBoxRangeGprims,
        eBudgetBoxRangeClusters,
        eBudgetBoxRangeGroup
    };

    // Rows drawn as one instanced box: the box min..max in each row's space.
    struct BudgetBoxRange {
        BudgetBoxRangeEnum kind = eBudgetBoxRangeGprims;
        std::size_t first = 0;
        std::size_t count = 0;
        PXR_NS::GfVec3f min { 0.f };
        PXR_NS::GfVec3f max { 0.f };
        // False for markers.
        bool bounded = true;
        // With eBudgetBoxRangeGroup, the group's index.
        std::size_t at = 0;
    };

    struct BudgetBoxSet {
        // The gprims', world space; a marker for one without bounds.
        std::vector<Box> boxes;
        std::vector<BudgetBoxGroup> groups;
        std::vector<BudgetBoxCluster> clusters;
        // Twelve per box drawn, in the order of ranges, ready to upload: the rows of a linear map,
        // then a translation, row vectors on the left as instanceTransform() has them. Gprims and
        // clusters map the unit cube onto their world box.
        std::vector<float> rows;
        std::vector<BudgetBoxRange> ranges;
        // The demoted and fractioned instances the groups and clusters stand for.
        std::size_t instanceCount = 0;

        bool empty() const { return ranges.empty(); }
    };

    struct Budget {
        // Of the requestBudget() it answers.
        U64 serial = 0;
        SceneViewBudget::Camera camera;
        SceneViewBudget::Settings settings;
        SceneViewBudget::Result result;
        SceneDrawFilter::Budget filter;
        // What it was solved against.
        UnitsPtr units;
        BudgetBoxSet budgetBoxSet;
    };

    struct BudgetBoxHit {
        // The gprim's, or the prototype's for an instance.
        PXR_NS::SdfPath path;
        bool isInstance = false;
        PXR_NS::SdfPath instancer;
        std::size_t instance = 0;
        PXR_NS::GfVec3d point;
    };

    using BudgetPtr = std::shared_ptr<const Budget>;

    // Without a lister the rows are boxedRows().
    SceneDrawBoxes(const StageReader& reader,
                   const std::function<bool()>& shouldYield,
                   const std::function<void()>& published,
                   const RowLister& lister = RowLister());

    ~SceneDrawBoxes();

    void setRules(const SceneDrawFilter::Rules& rules);
    void setTime(double time);
    void setStageVersion(U64 version);
    // Off by default. Turning it on starts a pass; turning it off drops the units.
    void setCounting(bool counting);
    // Kept for the passes that read it; a camera change alone starts no pass, as counts never
    // depend on it.
    void setCamera(const Camera& camera);
    Camera getCamera() const;

    /**
     * @brief Solves a budget for camera once the units are in, and again whenever they change, until
     * cancelBudget() or stopBudget(). Turns counting on. Returns the serial the result will carry.
     */
    U64 requestBudget(const Camera& camera,
                      const SceneViewBudget::Settings& settings);
    // Drops a solve under way or to come and holds the published budget, as during a camera move.
    void cancelBudget();
    // As cancelBudget(), and the published budget goes too.
    void stopBudget();
    // Null with none.
    BudgetPtr getBudget() const;
    // Whether a budget answering serial or a later request was published within timeout.
    bool waitForBudget(U64 serial,
                       std::chrono::milliseconds timeout) const;
    // The lister now gives other rows for the same stage and rules.
    void relist();

    ListPtr getList() const;
    // Null until counting has published.
    UnitsPtr getUnits() const;
    // The counts dropped before they published, as a change of the stage or rules made them stale.
    U64 getCountsDiscarded() const;

    // The latest request.
    U64 getGeneration() const;
    // As Units::contentGeneration, of the latest request.
    U64 getContentGeneration() const { return _contentGeneration; }

    // Whether a complete list answering generation or a later request was published within timeout.
    bool waitForList(U64 generation,
                     std::chrono::milliseconds timeout) const;
    // As waitForList(), for the units.
    bool waitForUnits(U64 generation,
                      std::chrono::milliseconds timeout) const;

    // Faces as the budget counts them for one gprim's own geometry at time: a mesh's faces, a
    // curve's segments, a point cloud's points, and Storm's tessellation for an implicit surface.
    static U64 gprimFaces(const PXR_NS::UsdPrim& prim,
                          double time);

    /**
     * @brief Has the worker put the rows of its latest list in nearFirst() order for this view, and
     * again for each list it publishes after; the order is published as getOrder(). Callers hand it
     * a view the camera has rested at, so a moving camera costs no sorting.
     */
    void setOrderView(const PXR_NS::GfMatrix4d& viewProjection,
                      int width,
                      int height);

    // Empty until setOrderView() has been called and a list published.
    SceneDrawFilter::Order getOrder() const;

    // Whether an order of the latest list for the latest view was published within timeout.
    bool waitForOrder(std::chrono::milliseconds timeout) const;

    /**
     * @brief The pixels of a width x height view a box covers, by the screen rectangle of its corners.
     * One reaching behind the eye covers the whole view, as it is around the eye or next to it; a
     * marker, a pending box and one wholly behind the eye or off the view cover nothing.
     */
    static double screenArea(const Box& box,
                             const PXR_NS::GfMatrix4d& viewProjection,
                             int width,
                             int height);

    // The paths of list's boxes, the one covering most of the view first, then the nearer.
    static std::vector<PXR_NS::SdfPath> nearFirst(const List& list,
                                                  const PXR_NS::GfMatrix4d& viewProjection,
                                                  int width,
                                                  int height);

    // The boxes of list whose row waits says has prims yet to come, in list's order.
    static List unadmitted(const List& list,
                           const std::function<bool(const PXR_NS::SdfPath&)>& waits);

    /**
     * @brief The index in list of the box or marker nearest the eye under pixel, -1 for none, and
     * the point hit. viewProjection maps world space to clip space as GfMatrix4d does, row vectors
     * on the left; pixel counts from the top left of a width x height view. A box is hit through its
     * volume, at its near face; one around the eye is never hit, or it would take every click that
     * hits nothing nearer. A marker is hit within markerPixels of its centre on screen, there.
     */
    static int pick(const List& list,
                    const PXR_NS::GfMatrix4d& viewProjection,
                    int width,
                    int height,
                    const PXR_NS::GfVec2d& pixel,
                    double markerPixels,
                    PXR_NS::GfVec3d* point);

    /**
     * @brief What draws in place of what result takes out of units: a demoted gprim, every drawn
     * gprim of a native instance the budget hides some of, and each demoted or fractioned-out
     * instance. Culled ones get none. An instance whose bounding sphere spans under
     * kBudgetClusterNdc of the camera's view, as clip space measures it, joins a cluster instead.
     * False, with budgetBoxSet partial, as soon as stale() is true.
     */
    static const double kBudgetClusterNdc;
    static bool buildBudgetBoxes(const SceneViewBudget::Result& result,
                                 const std::vector<Unit>& units,
                                 const Camera& camera,
                                 const std::function<bool()>& stale,
                                 BudgetBoxSet* budgetBoxSet);

    /**
     * @brief The budget box nearest the eye under pixel, as pick() finds a box or marker; with markers,
     * each is hit as a marker at its box's centre.
     */
    static bool pickBudgetBox(const BudgetBoxSet& budgetBoxSet,
                              const PXR_NS::GfMatrix4d& viewProjection,
                              int width,
                              int height,
                              const PXR_NS::GfVec2d& pixel,
                              double markerPixels,
                              bool markers,
                              BudgetBoxHit* hit);

    // By path. A row the stage lacks, or one under a row that hides it, is left out.
    static std::vector<PXR_NS::SdfPath> boxedRows(const PXR_NS::UsdStageRefPtr& stage,
                                                  const SceneDrawFilter::Rules& rules);

    /**
     * @brief The rows a stand-in for the whole drawn scene boxes. With byModel, every model prim
     * (kind component, assembly or group, as UsdModelAPI has it) the rules leave drawn, a BBox or
     * Hidden row ending its branch; models nest, and so do their boxes. Without byModel, or on a
     * stage with no model, every drawn row with a rule, and the rows boxedRows() gives; with no rule
     * either, the top-level prims but Hidden ones. Instance proxies are not looked into.
     */
    static std::vector<PXR_NS::SdfPath> standInRows(const PXR_NS::UsdStageRefPtr& stage,
                                                    const SceneDrawFilter::Rules& rules,
                                                    bool byModel);

private:
    struct Request {
        SceneDrawFilter::Rules rules;
        double time = 0.;
        U64 generation = 0;
        U64 contentGeneration = 0;
        bool counting = false;
    };

    struct Entry {
        Box box;
        bool varying = false;
        double time = 0.;
    };

    struct UnitEntry {
        Unit unit;
        bool varying = false;
        double time = 0.;
        // A PointInstancer whose bound and faces settleInstances() has still to take.
        bool settle = false;
    };

    void run();
    void compute(const Request& request);
    // False when the pass was dropped.
    bool computeBoxes(const Request& request);
    // Drops what was read of another stage, or of this one before an edit, and sets the bounds'
    // time. Needs the stage read lock.
    void syncStage(const PXR_NS::UsdStageRefPtr& stage,
                   U64 version,
                   double time);
    Entry computeEntry(const PXR_NS::UsdPrim& prim,
                       double time);
    void publish(const Request& request,
                 std::vector<Box>&& boxes,
                 bool complete);
    // False when the pass was dropped.
    bool countUnits(const Request& request);
    UnitEntry computeUnit(const PXR_NS::UsdPrim& prim,
                          double time);
    // Needs the stage read lock.
    void readInstances(const PXR_NS::UsdGeomPointInstancer& instancer,
                       double time,
                       UnitEntry* entry);
    // Off the lock. False when the pass was dropped.
    bool settleInstances(const Request& request,
                         Unit* unit) const;
    // Whether a budget is due, under _mutex.
    bool budgetDue() const;
    void solveBudget();
    void publishUnits(const Request& request,
                      std::vector<Unit>&& units);

    SceneDrawBoxes(const SceneDrawBoxes&) = delete;
    SceneDrawBoxes& operator=(const SceneDrawBoxes&) = delete;

    const StageReader _reader;
    const std::function<bool()> _shouldYield;
    const std::function<void()> _published;
    const RowLister _lister;

    mutable std::mutex _mutex;
    mutable std::condition_variable _changed;
    SceneDrawFilter::Rules _rules;
    double _time = 0.;
    U64 _stageVersion = 0;
    bool _counting = false;
    Camera _camera;
    U64 _generation = 0;
    U64 _taken = 0;
    bool _hasOrderView = false;
    PXR_NS::GfMatrix4d _orderViewProjection;
    int _orderWidth = 0;
    int _orderHeight = 0;
    // Counts changes of the list or the view; the order answers _orderMade of them.
    U64 _orderWanted = 0;
    U64 _orderMade = 0;
    SceneDrawFilter::Order _order;
    std::atomic<U64> _contentGeneration { 0 };
    std::atomic<U64> _countsDiscarded { 0 };
    std::atomic<bool> _quit { false };
    ListPtr _list;
    UnitsPtr _units;
    U64 _budgetSerial = 0;
    bool _budgetPending = false;
    // Requested and neither cancelled nor stopped, so new units are solved again.
    bool _budgetLive = false;
    Camera _budgetCamera;
    SceneViewBudget::Settings _budgetSettings;
    BudgetPtr _budget;

    // The worker's own.
    std::unordered_map<PXR_NS::SdfPath, Entry, PXR_NS::SdfPath::Hash> _entries;
    std::unordered_map<PXR_NS::SdfPath, UnitEntry, PXR_NS::SdfPath::Hash> _unitEntries;
    std::unique_ptr<PXR_NS::UsdGeomBBoxCache> _bboxCache;
    const void* _entriesStage = nullptr;
    U64 _entriesVersion = 0;

    std::thread _thread;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWBOXES_H
