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

#include "Gui/SceneDrawBoxes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Engine/Nodes/Scene/SceneTeardown.h"

#ifdef emit
#define NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/curves.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/modelAPI.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#ifdef NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#undef NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// A write waits for every reader, so a pass under the lock stays short.
const std::chrono::milliseconds kPassBudget(4);
const std::chrono::milliseconds kPassGap(1);
// Instances settled between checks for a dropped pass, off the lock.
const std::size_t kInstanceChunk = 1 << 16;
const std::size_t kBudgetBoxRowFloats = 12;

// The scene graph's rows, so every row it lists can be boxed.
Usd_PrimFlagsPredicate
rowPredicate()
{
    return UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract);
}

// Mirrors the filter: the rule nearest a prim, its own included, decides it, and a Pass Through row
// shows itself alone.
bool
parentShown(const SceneDrawFilter::Rules& rules,
            const SdfPath& path)
{
    const SdfPath parent = path.GetParentPath();

    for (SdfPath at = parent; !at.IsEmpty(); at = at.GetParentPath()) {
        const SceneDrawFilter::Rules::const_iterator found = rules.find(at);
        if (found == rules.end()) {
            continue;
        }
        switch (found->second) {
        case SceneDrawFilter::eRuleShow:
            return true;
        case SceneDrawFilter::eRulePassThrough:
            return at == parent;
        case SceneDrawFilter::eRuleHide:
        case SceneDrawFilter::eRuleHidden:
            return false;
        }
    }

    return true;
}

bool
boxesAnything(const SceneDrawFilter::Rules& rules)
{
    for (const SceneDrawFilter::Rules::value_type& rule : rules) {
        if ((rule.second == SceneDrawFilter::eRuleHide) || (rule.second == SceneDrawFilter::eRulePassThrough)) {
            return true;
        }
    }

    return false;
}

bool
transformMightVary(const UsdPrim& prim)
{
    const UsdGeomXformable xformable(prim);

    return xformable && xformable.TransformMightBeTimeVarying();
}

// Whether anything UsdGeomBBoxCache reads for the prim's world bound may change with time. Below a
// model with an authored extentsHint the cache reads the hint alone.
bool
boundMightVary(const UsdPrim& prim)
{
    for (UsdPrim ancestor = prim.GetParent(); ancestor && !ancestor.IsPseudoRoot(); ancestor = ancestor.GetParent()) {
        if (transformMightVary(ancestor)) {
            return true;
        }
    }
    UsdPrimRange range(prim, rowPredicate());
    for (UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
        const UsdPrim& at = *it;
        const UsdGeomImageable imageable(at);
        if (!imageable) {
            continue;
        }
        if (imageable.GetVisibilityAttr().ValueMightBeTimeVarying() || transformMightVary(at)) {
            return true;
        }
        if (at.IsModel()) {
            const UsdAttribute hint = UsdGeomModelAPI(at).GetExtentsHintAttr();
            if (hint && hint.HasAuthoredValue()) {
                if (hint.ValueMightBeTimeVarying()) {
                    return true;
                }
                it.PruneChildren();
                continue;
            }
        }
        const UsdGeomBoundable boundable(at);
        if (boundable && boundable.GetExtentAttr().ValueMightBeTimeVarying()) {
            return true;
        }
        const UsdGeomPointBased pointBased(at);
        if (pointBased && pointBased.GetPointsAttr().ValueMightBeTimeVarying()) {
            return true;
        }
        const UsdGeomPointInstancer instancer(at);
        if (instancer && (instancer.GetPositionsAttr().ValueMightBeTimeVarying() || instancer.GetOrientationsAttr().ValueMightBeTimeVarying() || instancer.GetScalesAttr().ValueMightBeTimeVarying() || instancer.GetProtoIndicesAttr().ValueMightBeTimeVarying())) {
            return true;
        }
    }

    return false;
}

// The topology of a mesh or curve may change with time, and with it the faces it counts.
bool
countsMightVary(const UsdPrim& prim)
{
    UsdPrimRange range(prim, rowPredicate());
    for (const UsdPrim& at : range) {
        const UsdGeomMesh mesh(at);
        if (mesh && mesh.GetFaceVertexCountsAttr().ValueMightBeTimeVarying()) {
            return true;
        }
        const UsdGeomCurves curves(at);
        if (curves && curves.GetCurveVertexCountsAttr().ValueMightBeTimeVarying()) {
            return true;
        }
    }

    return false;
}

// Whether the viewer leaves the prim and everything below it out: invisible, or of a purpose it
// does not draw.
bool
notDrawn(const UsdPrim& prim,
         double time)
{
    const UsdGeomImageable imageable(prim);

    if (!imageable) {
        return false;
    }
    TfToken visibility;
    if (imageable.GetVisibilityAttr().Get(&visibility, UsdTimeCode(time)) && (visibility == UsdGeomTokens->invisible)) {
        return true;
    }
    TfToken purpose;
    if (imageable.GetPurposeAttr().Get(&purpose) && ((purpose == UsdGeomTokens->guide) || (purpose == UsdGeomTokens->render))) {
        return true;
    }

    return false;
}

// Instancers inside prototypes count this deep, and draw nothing deeper.
const int kMaxInstancerNesting = 8;

U64 subtreeFaces(const UsdPrim& root, double time, int depth);

std::vector<U64>
prototypeFaces(const UsdGeomPointInstancer& instancer,
               double time,
               int depth,
               SdfPathVector* paths)
{
    SdfPathVector targets;
    instancer.GetPrototypesRel().GetForwardedTargets(&targets);
    std::vector<U64> faces;
    faces.reserve(targets.size());
    const UsdStageWeakPtr stage = instancer.GetPrim().GetStage();
    for (const SdfPath& target : targets) {
        const UsdPrim prototype = stage->GetPrimAtPath(target);
        faces.push_back(prototype ? subtreeFaces(prototype, time, depth) : 0);
    }
    if (paths) {
        *paths = targets;
    }

    return faces;
}

U64
instancedFaces(const VtIntArray& protoIndices,
               const std::vector<bool>& mask,
               const std::vector<U64>& faces)
{
    U64 total = 0;

    for (std::size_t index = 0; index < protoIndices.size(); ++index) {
        const int prototype = protoIndices[index];
        if ((prototype < 0) || (std::size_t(prototype) >= faces.size())) {
            continue;
        }
        if (!mask.empty() && ((index >= mask.size()) || !mask[index])) {
            continue;
        }
        total += faces[prototype];
    }

    return total;
}

U64
subtreeFaces(const UsdPrim& root,
             double time,
             int depth)
{
    U64 total = 0;
    UsdPrimRange range(root, rowPredicate());

    for (UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
        const UsdPrim& prim = *it;
        if (notDrawn(prim, time) || !prim.IsLoaded()) {
            it.PruneChildren();
            continue;
        }
        const UsdGeomPointInstancer instancer(prim);
        if (instancer) {
            it.PruneChildren();
            if (depth < kMaxInstancerNesting) {
                VtIntArray protoIndices;
                instancer.GetProtoIndicesAttr().Get(&protoIndices, UsdTimeCode(time));
                total += instancedFaces(protoIndices, instancer.ComputeMaskAtTime(UsdTimeCode(time)), prototypeFaces(instancer, time, depth + 1, nullptr));
            }
            continue;
        }
        if (prim.IsA<UsdGeomGprim>()) {
            total += SceneDrawBoxes::gprimFaces(prim, time);
        }
    }

    return total;
}

enum UnitStepEnum {
    eUnitStepWalk,
    eUnitStepPrune,
    eUnitStepUnit,
    eUnitStepUnitPrune
};

// What the units pass does at a prim whose ancestors the rules draw. A rule of the prim's own
// decides it; without one, a Children parent makes it a box.
UnitStepEnum
unitStep(const SceneDrawFilter::Rules& rules,
         const UsdPrim& prim,
         double time)
{
    const SdfPath& path = prim.GetPath();
    const SceneDrawFilter::Rules::const_iterator found = rules.find(path);

    if (found != rules.end()) {
        if ((found->second == SceneDrawFilter::eRuleHide) || (found->second == SceneDrawFilter::eRuleHidden)) {
            return eUnitStepPrune;
        }
    } else {
        const SceneDrawFilter::Rules::const_iterator parent = rules.find(path.GetParentPath());
        if ((parent != rules.end()) && (parent->second == SceneDrawFilter::eRuleExpand)) {
            return eUnitStepPrune;
        }
    }
    if (notDrawn(prim, time)) {
        return eUnitStepPrune;
    }
    if (!prim.IsLoaded() || prim.IsA<UsdGeomPointInstancer>()) {
        return eUnitStepUnitPrune;
    }
    if (prim.IsA<UsdGeomGprim>()) {
        return eUnitStepUnit;
    }

    return eUnitStepWalk;
}
} // anon namespace

SceneDrawBoxes::SceneDrawBoxes(const StageReader& reader,
                               const std::function<bool()>& shouldYield,
                               const std::function<void()>& published,
                               const RowLister& lister)
    : _reader(reader)
    , _shouldYield(shouldYield)
    , _published(published)
    , _lister(lister)
{
    _thread = std::thread([this]() { run(); });
}

SceneDrawBoxes::~SceneDrawBoxes()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _quit = true;
    }
    _changed.notify_all();
    _thread.join();
    SceneTeardown::release(std::move(_list));
}

void
SceneDrawBoxes::setRules(const SceneDrawFilter::Rules& rules)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (rules == _rules) {
            return;
        }
        _rules = rules;
        ++_generation;
        ++_drawnGeneration;
        ++_contentGeneration;
    }
    _changed.notify_all();
}

void
SceneDrawBoxes::setTime(double time)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (time == _time) {
            return;
        }
        _time = time;
        ++_generation;
        ++_drawnGeneration;
    }
    _changed.notify_all();
}

void
SceneDrawBoxes::setStageVersion(U64 version,
                                U64 loadVersion)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if ((version == _stageVersion) && (loadVersion == _loadVersion)) {
            return;
        }
        // Hydra holds back what a load brings while a budget is in place, so a budget solved
        // before the load still stands for what is drawn.
        if (version != _stageVersion) {
            ++_drawnGeneration;
        }
        _stageVersion = version;
        _loadVersion = loadVersion;
        ++_generation;
        ++_contentGeneration;
    }
    _changed.notify_all();
}

void
SceneDrawBoxes::setCounting(bool counting)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (counting == _counting) {
            return;
        }
        _counting = counting;
        ++_generation;
        ++_drawnGeneration;
        if (!counting) {
            ++_contentGeneration;
            _units.reset();
        }
    }
    _changed.notify_all();
}

void
SceneDrawBoxes::setCamera(const Camera& camera)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _camera = camera;
}

SceneDrawBoxes::Camera
SceneDrawBoxes::getCamera() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _camera;
}

U64
SceneDrawBoxes::requestBudget(const Camera& camera,
                              const SceneViewBudget::Settings& settings)
{
    U64 serial = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        serial = ++_budgetSerial;
        _budgetPending = true;
        _budgetLive = true;
        _budgetCamera = camera;
        _budgetSettings = settings;
        _camera = camera;
        if (!_counting) {
            _counting = true;
            ++_generation;
            ++_drawnGeneration;
        }
    }
    _changed.notify_all();

    return serial;
}

void
SceneDrawBoxes::cancelBudget()
{
    std::lock_guard<std::mutex> lock(_mutex);

    ++_budgetSerial;
    _budgetPending = false;
    _budgetLive = false;
}

void
SceneDrawBoxes::stopBudget()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_budgetSerial;
        _budgetPending = false;
        _budgetLive = false;
        _budget.reset();
    }
    if (_published) {
        _published();
    }
}

SceneDrawBoxes::BudgetPtr
SceneDrawBoxes::getBudget() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _budget;
}

bool
SceneDrawBoxes::waitForBudget(U64 serial,
                              std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _changed.wait_for(lock, timeout, [&]() { return _budget && (_budget->serial >= serial); });
}

bool
SceneDrawBoxes::budgetDue() const
{
    return _budgetPending && _units && (_units->generation == _generation);
}

void
SceneDrawBoxes::solveBudget()
{
    std::shared_ptr<Budget> budget = std::make_shared<Budget>();
    BudgetPtr previous;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!budgetDue()) {
            return;
        }
        _budgetPending = false;
        budget->serial = _budgetSerial;
        budget->camera = _budgetCamera;
        budget->settings = _budgetSettings;
        budget->units = _units;
        previous = _budget;
    }
    // Hysteresis holds only against a result for the same settings and draw states: a unit a draw
    // state or load brings back has no decision of its own in the previous result to hold.
    const bool comparable = previous && previous->units && (previous->units->contentGeneration == budget->units->contentGeneration)
        && (previous->settings.limit == budget->settings.limit)
        && (previous->settings.countWholeScene == budget->settings.countWholeScene)
        && (previous->settings.cullDistance == budget->settings.cullDistance)
        && (previous->settings.fractionDistance == budget->settings.fractionDistance)
        && (previous->settings.fractionPercent == budget->settings.fractionPercent);
    budget->result = SceneViewBudget::solve(budget->units->units, budget->camera, budget->settings,
                                            comparable ? previous->result : SceneViewBudget::Result());
    budget->filter = SceneDrawFilter::budgetFrom(budget->result, budget->units->units);
    const U64 serial = budget->serial;
    const auto stale = [this, serial]() {
        std::lock_guard<std::mutex> lock(_mutex);

        return (serial != _budgetSerial) || _quit;
    };
    if (!buildBudgetBoxes(budget->result, budget->units->units, budget->camera, stale, &budget->budgetBoxSet)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if ((budget->serial != _budgetSerial) || _quit) {
            return;
        }
        _budget = budget;
    }
    _changed.notify_all();
    if (_published) {
        _published();
    }
} // SceneDrawBoxes::solveBudget

void
SceneDrawBoxes::relist()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_generation;
        ++_drawnGeneration;
        ++_contentGeneration;
    }
    _changed.notify_all();
}

SceneDrawBoxes::ListPtr
SceneDrawBoxes::getList() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _list;
}

SceneDrawBoxes::UnitsPtr
SceneDrawBoxes::getUnits() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _units;
}

U64
SceneDrawBoxes::getCountsDiscarded() const
{
    return _countsDiscarded;
}

U64
SceneDrawBoxes::getGeneration() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _generation;
}

U64
SceneDrawBoxes::getDrawnGeneration() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _drawnGeneration;
}

bool
SceneDrawBoxes::waitForList(U64 generation,
                            std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _changed.wait_for(lock, timeout, [&]() { return _list && _list->complete && (_list->generation >= generation); });
}

bool
SceneDrawBoxes::waitForUnits(U64 generation,
                             std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _changed.wait_for(lock, timeout, [&]() { return _units && (_units->generation >= generation); });
}

U64
SceneDrawBoxes::gprimFaces(const UsdPrim& prim,
                           double time)
{
    const UsdTimeCode at(time);

    if (prim.IsA<UsdGeomMesh>()) {
        VtIntArray counts;
        UsdGeomMesh(prim).GetFaceVertexCountsAttr().Get(&counts, at);

        return counts.size();
    }
    if (prim.IsA<UsdGeomBasisCurves>()) {
        U64 total = 0;
        const VtIntArray counts = UsdGeomBasisCurves(prim).ComputeSegmentCounts(at);
        for (const int segments : counts) {
            total += U64((std::max)(segments, 0));
        }

        return total;
    }
    if (prim.IsA<UsdGeomCurves>()) {
        VtIntArray counts;
        UsdGeomCurves(prim).GetCurveVertexCountsAttr().Get(&counts, at);
        U64 total = 0;
        for (const int vertices : std::as_const(counts)) {
            total += U64((std::max)(vertices - 1, 0));
        }

        return total;
    }
    if (prim.IsA<UsdGeomPoints>()) {
        VtVec3fArray points;
        UsdGeomPoints(prim).GetPointsAttr().Get(&points, at);

        return points.size();
    }
    // Storm draws implicit surfaces as the meshes usdImaging's adapters generate: 10 radial
    // segments, 10 axial ones on a sphere and 4 per capsule cap.
    const U64 radial = 10;
    if (prim.IsA<UsdGeomCube>()) {
        return 6;
    }
    if (prim.IsA<UsdGeomPlane>()) {
        return 1;
    }
    if (prim.IsA<UsdGeomSphere>()) {
        return radial * 10;
    }
    if (prim.IsA<UsdGeomCylinder>()) {
        return radial * 3;
    }
    if (prim.IsA<UsdGeomCone>()) {
        return radial * 2;
    }
    if (prim.IsA<UsdGeomCapsule>()) {
        return radial * (2 * 4 + 1);
    }

    return 0;
} // SceneDrawBoxes::gprimFaces

void
SceneDrawBoxes::setOrderView(const GfMatrix4d& viewProjection,
                             int width,
                             int height)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hasOrderView && (viewProjection == _orderViewProjection) && (width == _orderWidth) && (height == _orderHeight)) {
            return;
        }
        _hasOrderView = true;
        _orderViewProjection = viewProjection;
        _orderWidth = width;
        _orderHeight = height;
        ++_orderWanted;
    }
    _changed.notify_all();
}

SceneDrawFilter::Order
SceneDrawBoxes::getOrder() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _order;
}

bool
SceneDrawBoxes::waitForOrder(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _changed.wait_for(lock, timeout, [&]() { return _order && (_orderMade == _orderWanted); });
}

double
SceneDrawBoxes::screenArea(const Box& box,
                           const GfMatrix4d& viewProjection,
                           int width,
                           int height)
{
    if ((width <= 0) || (height <= 0) || (box.kind == eKindMarker) || (box.kind == eKindPending)) {
        return 0.;
    }
    double left = std::numeric_limits<double>::infinity();
    double right = -left;
    double bottom = left;
    double top = -left;
    int behind = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const GfVec4d clip = GfVec4d((corner & 1) ? box.max[0] : box.min[0],
                                     (corner & 2) ? box.max[1] : box.min[1],
                                     (corner & 4) ? box.max[2] : box.min[2],
                                     1.)
            * viewProjection;
        if (clip[3] <= 0.) {
            ++behind;
            continue;
        }
        left = (std::min)(left, clip[0] / clip[3]);
        right = (std::max)(right, clip[0] / clip[3]);
        bottom = (std::min)(bottom, clip[1] / clip[3]);
        top = (std::max)(top, clip[1] / clip[3]);
    }
    if (behind == 8) {
        return 0.;
    }
    if (behind != 0) {
        return double(width) * double(height);
    }
    const double x = (std::min)(right, 1.) - (std::max)(left, -1.);
    const double y = (std::min)(top, 1.) - (std::max)(bottom, -1.);
    if ((x <= 0.) || (y <= 0.)) {
        return 0.;
    }

    return x * y * 0.25 * double(width) * double(height);
}

std::vector<SdfPath>
SceneDrawBoxes::nearFirst(const List& list,
                          const GfMatrix4d& viewProjection,
                          int width,
                          int height)
{
    struct Ranked {
        double area;
        double depth;
        std::size_t index;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(list.boxes.size());
    for (std::size_t index = 0; index < list.boxes.size(); ++index) {
        const Box& box = list.boxes[index];
        const GfVec3d centre = (GfVec3d(box.min) + GfVec3d(box.max)) * 0.5;
        const GfVec4d clip = GfVec4d(centre[0], centre[1], centre[2], 1.) * viewProjection;
        // Clip w is the distance in front of the eye.
        const double depth = (clip[3] > 0.) ? clip[3] : std::numeric_limits<double>::infinity();
        ranked.push_back(Ranked { screenArea(box, viewProjection, width, height), depth, index });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.area != b.area) {
            return a.area > b.area;
        }

        return a.depth < b.depth;
    });
    std::vector<SdfPath> paths;
    paths.reserve(ranked.size());
    for (const Ranked& entry : ranked) {
        paths.push_back(list.boxes[entry.index].path);
    }

    return paths;
}

SceneDrawBoxes::List
SceneDrawBoxes::unadmitted(const List& list,
                           const std::function<bool(const SdfPath&)>& waits)
{
    List kept;
    kept.generation = list.generation;
    kept.time = list.time;
    kept.complete = list.complete;
    for (const Box& box : list.boxes) {
        if (waits(box.path)) {
            kept.boxes.push_back(box);
        }
    }

    return kept;
}

int
SceneDrawBoxes::pick(const List& list,
                     const GfMatrix4d& viewProjection,
                     int width,
                     int height,
                     const GfVec2d& pixel,
                     double markerPixels,
                     GfVec3d* point)
{
    if ((width <= 0) || (height <= 0) || list.boxes.empty()) {
        return -1;
    }
    const GfMatrix4d inverse = viewProjection.GetInverse();
    const double nx = 2. * pixel[0] / width - 1.;
    const double ny = 1. - 2. * pixel[1] / height;
    // The ray from the near plane, at t = 0, to the far plane, at t = 1.
    const GfVec3d origin = inverse.Transform(GfVec3d(nx, ny, -1.));
    const GfVec3d direction = inverse.Transform(GfVec3d(nx, ny, 1.)) - origin;
    int best = -1;
    // Normalized device depth, which grows away from the eye along any ray in either projection.
    double bestDepth = std::numeric_limits<double>::infinity();
    GfVec3d bestPoint;

    for (std::size_t index = 0; index < list.boxes.size(); ++index) {
        const Box& box = list.boxes[index];
        GfVec3d hit;
        double depth = 0.;
        if ((box.kind == eKindMarker) || (box.kind == eKindPending)) {
            const GfVec3d centre(box.min);
            const GfVec4d clip = GfVec4d(centre[0], centre[1], centre[2], 1.) * viewProjection;
            if (clip[3] <= 0.) {
                continue;
            }
            const GfVec3d ndc(clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]);
            if ((ndc[2] < -1.) || (ndc[2] > 1.)) {
                continue;
            }
            const double sx = (ndc[0] + 1.) * 0.5 * width;
            const double sy = (1. - ndc[1]) * 0.5 * height;
            if ((std::abs(sx - pixel[0]) > markerPixels) || (std::abs(sy - pixel[1]) > markerPixels)) {
                continue;
            }
            hit = centre;
            depth = ndc[2];
        } else {
            double enter = -std::numeric_limits<double>::infinity();
            double leave = std::numeric_limits<double>::infinity();
            bool missed = false;
            for (int axis = 0; (axis < 3) && !missed; ++axis) {
                const double low = box.min[axis];
                const double high = box.max[axis];
                if (direction[axis] == 0.) {
                    missed = (origin[axis] < low) || (origin[axis] > high);
                    continue;
                }
                double t0 = (low - origin[axis]) / direction[axis];
                double t1 = (high - origin[axis]) / direction[axis];
                if (t0 > t1) {
                    std::swap(t0, t1);
                }
                enter = std::max(enter, t0);
                leave = std::min(leave, t1);
                missed = enter > leave;
            }
            if (missed || (enter < 0.) || (enter > 1.)) {
                continue;
            }
            hit = origin + direction * enter;
            depth = viewProjection.Transform(hit)[2];
        }
        if (depth < bestDepth) {
            best = int(index);
            bestDepth = depth;
            bestPoint = hit;
        }
    }
    if ((best >= 0) && point) {
        *point = bestPoint;
    }

    return best;
} // SceneDrawBoxes::pick

namespace {
bool
bitOr(const std::vector<bool>& bits,
      std::size_t index,
      bool fallback)
{
    return (index < bits.size()) ? bits[index] : fallback;
}

// Where along the ray from origin, at t = 0, to origin + direction, at t = 1, it enters the box;
// false if it misses it or enters outside [0, 1].
bool
enterBox(const GfVec3d& origin,
         const GfVec3d& direction,
         const GfVec3f& min,
         const GfVec3f& max,
         double* t)
{
    double enter = -std::numeric_limits<double>::infinity();
    double leave = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.) {
            if ((origin[axis] < min[axis]) || (origin[axis] > max[axis])) {
                return false;
            }
            continue;
        }
        double t0 = (min[axis] - origin[axis]) / direction[axis];
        double t1 = (max[axis] - origin[axis]) / direction[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) {
            return false;
        }
    }
    if ((enter < 0.) || (enter > 1.)) {
        return false;
    }
    *t = enter;

    return true;
}

// The marker at centre's normalized device depth if it is within markerPixels of pixel on screen.
bool
hitMarker(const GfVec3d& centre,
          const GfMatrix4d& viewProjection,
          int width,
          int height,
          const GfVec2d& pixel,
          double markerPixels,
          double* depth)
{
    const GfVec4d clip = GfVec4d(centre[0], centre[1], centre[2], 1.) * viewProjection;
    if (clip[3] <= 0.) {
        return false;
    }
    const GfVec3d ndc(clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]);
    if ((ndc[2] < -1.) || (ndc[2] > 1.)) {
        return false;
    }
    const double sx = (ndc[0] + 1.) * 0.5 * width;
    const double sy = (1. - ndc[1]) * 0.5 * height;
    if ((std::abs(sx - pixel[0]) > markerPixels) || (std::abs(sy - pixel[1]) > markerPixels)) {
        return false;
    }
    *depth = ndc[2];

    return true;
}
} // anon namespace

namespace {
struct ClusterCell {
    int level = 0;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const ClusterCell& other) const
    {
        return (level == other.level) && (x == other.x) && (y == other.y) && (z == other.z);
    }
};

struct ClusterCellHash {
    std::size_t operator()(const ClusterCell& cell) const
    {
        U64 h = U64(std::uint32_t(cell.level)) * 0x9E3779B97F4A7C15ull;
        for (const std::int64_t v : { cell.x, cell.y, cell.z }) {
            h ^= U64(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        }

        return std::size_t(h);
    }
};

void
pushBoxRows(std::vector<float>* rows,
            const GfVec3f& min,
            const GfVec3f& size)
{
    rows->insert(rows->end(), { size[0], 0.f, 0.f, 0.f, size[1], 0.f, 0.f, 0.f, size[2], min[0], min[1], min[2] });
}
} // anon namespace

// About five pixels of a 1080-pixel-tall view.
const double SceneDrawBoxes::kBudgetClusterNdc = 0.01;

bool
SceneDrawBoxes::buildBudgetBoxes(const SceneViewBudget::Result& result,
                                 const std::vector<Unit>& units,
                                 const Camera& camera,
                                 const std::function<bool()>& stale,
                                 BudgetBoxSet* budgetBoxSet)
{
    *budgetBoxSet = BudgetBoxSet();
    if (!result.valid) {
        return true;
    }
    std::unordered_set<SdfPath, SdfPath::Hash> hiddenInstances;
    for (const Unit& unit : units) {
        if (!unit.nativeInstance.IsEmpty() && (result.isDemoted(unit.path) || result.isCulled(unit.path))) {
            hiddenInstances.insert(unit.nativeInstance);
        }
    }
    // An instance spans about 2 r focal / d of clip space at distance d, focal being the larger of
    // the projection's x and y scales.
    double focal = 0.;
    if (camera.valid) {
        for (int column = 0; column < 2; ++column) {
            const GfMatrix4d& m = camera.viewProjection;
            focal = (std::max)(focal, GfVec3d(m[0][column], m[1][column], m[2][column]).GetLength());
        }
    }
    std::unordered_map<ClusterCell, std::size_t, ClusterCellHash> cellOf;
    std::vector<GfVec3d> clusterMin;
    std::vector<GfVec3d> clusterMax;
    std::vector<std::vector<float>> groupRows;
    std::size_t sinceCheck = 0;
    for (const Unit& unit : units) {
        if (++sinceCheck >= kInstanceChunk) {
            sinceCheck = 0;
            if (stale()) {
                return false;
            }
        }
        if (unit.state != SceneViewBudget::eStateDrawn) {
            continue;
        }
        if (!unit.instances) {
            if (result.isCulled(unit.path)) {
                continue;
            }
            if (result.isDemoted(unit.path) || (!unit.nativeInstance.IsEmpty() && hiddenInstances.count(unit.nativeInstance))) {
                Box box;
                box.path = unit.path;
                box.min = unit.min;
                box.max = unit.bounded ? unit.max : unit.min;
                box.kind = unit.bounded ? eKindBox : eKindMarker;
                budgetBoxSet->boxes.push_back(box);
            }
            continue;
        }
        const auto found = result.instancers.find(unit.path);
        if (found == result.instancers.end()) {
            continue;
        }
        const SceneViewBudget::InstancerResult& record = found->second;
        const Instances& instances = *unit.instances;
        const Instances::Spheres spheres = instances.prototypeSpheres();
        // Per prototype, its group's index in budgetBoxSet->groups.
        std::vector<std::size_t> groupOf(instances.prototypes.size(), std::size_t(-1));
        for (std::size_t index = 0; index < instances.size(); ++index) {
            if (++sinceCheck >= kInstanceChunk) {
                sinceCheck = 0;
                if (stale()) {
                    return false;
                }
            }
            if (!instances.draws(index) || bitOr(record.culled, index, false)) {
                continue;
            }
            if (!bitOr(record.demoted, index, false) && bitOr(record.kept, index, true)) {
                continue;
            }
            ++budgetBoxSet->instanceCount;
            const std::size_t prototype = std::size_t(instances.protoIndices[index]);
            if (focal > 0.) {
                GfVec3d centre;
                double radius = 0.;
                instances.instanceSphere(spheres, index, &centre, &radius);
                const double distance = (centre - camera.eye).GetLength();
                const double cell = kBudgetClusterNdc * distance / focal;
                if ((2. * radius < cell) && (cell > 0.) && std::isfinite(cell)) {
                    ClusterCell key;
                    key.level = int(std::ceil(std::log2(cell)));
                    const double size = std::ldexp(1., key.level);
                    key.x = std::int64_t(std::floor(centre[0] / size));
                    key.y = std::int64_t(std::floor(centre[1] / size));
                    key.z = std::int64_t(std::floor(centre[2] / size));
                    const GfVec3d low = centre - GfVec3d(radius);
                    const GfVec3d high = centre + GfVec3d(radius);
                    const auto made = cellOf.emplace(key, budgetBoxSet->clusters.size());
                    if (made.second) {
                        BudgetBoxCluster cluster;
                        cluster.instancer = unit.path;
                        cluster.prototype = instances.prototypes[prototype].path;
                        cluster.instance = index;
                        budgetBoxSet->clusters.push_back(cluster);
                        clusterMin.push_back(low);
                        clusterMax.push_back(high);
                    } else {
                        const std::size_t at = made.first->second;
                        for (int axis = 0; axis < 3; ++axis) {
                            clusterMin[at][axis] = (std::min)(clusterMin[at][axis], low[axis]);
                            clusterMax[at][axis] = (std::max)(clusterMax[at][axis], high[axis]);
                        }
                    }
                    ++budgetBoxSet->clusters[made.first->second].count;
                    continue;
                }
            }
            if (groupOf[prototype] == std::size_t(-1)) {
                groupOf[prototype] = budgetBoxSet->groups.size();
                BudgetBoxGroup group;
                group.instancer = unit.path;
                group.prototype = instances.prototypes[prototype].path;
                budgetBoxSet->groups.push_back(std::move(group));
                BudgetBoxRange range;
                range.kind = eBudgetBoxRangeGroup;
                range.min = instances.prototypes[prototype].min;
                range.max = instances.prototypes[prototype].bounded ? instances.prototypes[prototype].max : range.min;
                range.bounded = instances.prototypes[prototype].bounded;
                range.at = groupOf[prototype];
                budgetBoxSet->ranges.push_back(range);
                groupRows.emplace_back();
            }
            BudgetBoxGroup& group = budgetBoxSet->groups[groupOf[prototype]];
            std::vector<float>& rows = groupRows[groupOf[prototype]];
            const GfMatrix4d transform = instances.instanceTransform(index);
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 3; ++column) {
                    rows.push_back(float(transform[row][column]));
                }
            }
            group.indices.push_back(index);
        }
    }

    // Gprim boxes, gprim markers, cluster boxes and cluster markers, then the groups.
    std::vector<BudgetBoxRange> ranges;
    std::vector<float>& rows = budgetBoxSet->rows;
    std::size_t total = budgetBoxSet->boxes.size() + budgetBoxSet->clusters.size();
    for (const BudgetBoxGroup& group : budgetBoxSet->groups) {
        total += group.indices.size();
    }
    rows.reserve(total * kBudgetBoxRowFloats);
    for (int pass = 0; pass < 4; ++pass) {
        BudgetBoxRange range;
        range.kind = (pass < 2) ? eBudgetBoxRangeGprims : eBudgetBoxRangeClusters;
        range.bounded = ((pass % 2) == 0);
        range.min = GfVec3f(0.f);
        range.max = range.bounded ? GfVec3f(1.f) : GfVec3f(0.f);
        range.first = rows.size() / kBudgetBoxRowFloats;
        if (pass < 2) {
            for (const Box& box : budgetBoxSet->boxes) {
                if ((box.kind != eKindMarker) != range.bounded) {
                    continue;
                }
                pushBoxRows(&rows, box.min, range.bounded ? GfVec3f(box.max - box.min) : GfVec3f(0.f));
            }
        } else {
            for (std::size_t at = 0; at < budgetBoxSet->clusters.size(); ++at) {
                const GfVec3f low(clusterMin[at]);
                const GfVec3f size(clusterMax[at] - clusterMin[at]);
                if ((size != GfVec3f(0.f)) != range.bounded) {
                    continue;
                }
                budgetBoxSet->clusters[at].row = rows.size() / kBudgetBoxRowFloats;
                pushBoxRows(&rows, low, size);
            }
        }
        range.count = rows.size() / kBudgetBoxRowFloats - range.first;
        if (range.count > 0) {
            ranges.push_back(range);
        }
    }
    for (BudgetBoxRange range : budgetBoxSet->ranges) {
        const std::vector<float>& from = groupRows[range.at];
        range.first = rows.size() / kBudgetBoxRowFloats;
        range.count = from.size() / kBudgetBoxRowFloats;
        rows.insert(rows.end(), from.begin(), from.end());
        ranges.push_back(range);
    }
    budgetBoxSet->ranges = std::move(ranges);

    return true;
} // SceneDrawBoxes::buildBudgetBoxes

bool
SceneDrawBoxes::pickBudgetBox(const BudgetBoxSet& budgetBoxSet,
                              const GfMatrix4d& viewProjection,
                              int width,
                              int height,
                              const GfVec2d& pixel,
                              double markerPixels,
                              bool markers,
                              BudgetBoxHit* hit)
{
    if ((width <= 0) || (height <= 0) || budgetBoxSet.empty()) {
        return false;
    }
    const GfMatrix4d inverse = viewProjection.GetInverse();
    const double nx = 2. * pixel[0] / width - 1.;
    const double ny = 1. - 2. * pixel[1] / height;
    const GfVec3d origin = inverse.Transform(GfVec3d(nx, ny, -1.));
    const GfVec3d direction = inverse.Transform(GfVec3d(nx, ny, 1.)) - origin;
    const double length = direction.GetLength();
    if (length <= 0.) {
        return false;
    }
    const GfVec3d unit = direction / length;
    double bestDepth = std::numeric_limits<double>::infinity();
    bool found = false;
    const auto offer = [&](double depth, const GfVec3d& point, const SdfPath& path, const SdfPath& instancer, bool isInstance, std::size_t instance) {
        if (depth >= bestDepth) {
            return;
        }
        bestDepth = depth;
        found = true;
        hit->path = path;
        hit->instancer = instancer;
        hit->isInstance = isInstance;
        hit->instance = instance;
        hit->point = point;
    };

    for (const Box& box : budgetBoxSet.boxes) {
        double depth = 0.;
        if (markers || (box.kind == eKindMarker)) {
            const GfVec3d centre = (GfVec3d(box.min) + GfVec3d(box.max)) * 0.5;
            if (hitMarker(centre, viewProjection, width, height, pixel, markerPixels, &depth)) {
                offer(depth, centre, box.path, SdfPath(), false, 0);
            }
            continue;
        }
        double t = 0.;
        if (enterBox(origin, direction, box.min, box.max, &t)) {
            const GfVec3d point = origin + direction * t;
            offer(viewProjection.Transform(point)[2], point, box.path, SdfPath(), false, 0);
        }
    }
    for (const BudgetBoxCluster& cluster : budgetBoxSet.clusters) {
        const float* r = &budgetBoxSet.rows[cluster.row * kBudgetBoxRowFloats];
        const GfVec3f low(r[9], r[10], r[11]);
        const GfVec3f high = low + GfVec3f(r[0], r[4], r[8]);
        double depth = 0.;
        if (markers || (high == low)) {
            const GfVec3d centre = (GfVec3d(low) + GfVec3d(high)) * 0.5;
            if (hitMarker(centre, viewProjection, width, height, pixel, markerPixels, &depth)) {
                offer(depth, centre, cluster.prototype, cluster.instancer, true, cluster.instance);
            }
            continue;
        }
        double t = 0.;
        if (enterBox(origin, direction, low, high, &t)) {
            const GfVec3d point = origin + direction * t;
            offer(viewProjection.Transform(point)[2], point, cluster.prototype, cluster.instancer, true, cluster.instance);
        }
    }
    for (const BudgetBoxRange& range : budgetBoxSet.ranges) {
        if (range.kind != eBudgetBoxRangeGroup) {
            continue;
        }
        const BudgetBoxGroup& group = budgetBoxSet.groups[range.at];
        const GfVec3d localCentre = (GfVec3d(range.min) + GfVec3d(range.max)) * 0.5;
        const double localRadius = (GfVec3d(range.max) - GfVec3d(range.min)).GetLength() * 0.5;
        const bool asMarker = markers || !range.bounded;
        for (std::size_t at = 0; at < range.count; ++at) {
            const float* r = &budgetBoxSet.rows[(range.first + at) * kBudgetBoxRowFloats];
            const GfVec3d r0(r[0], r[1], r[2]);
            const GfVec3d r1(r[3], r[4], r[5]);
            const GfVec3d r2(r[6], r[7], r[8]);
            const GfVec3d r3(r[9], r[10], r[11]);
            const GfVec3d centre = r0 * localCentre[0] + r1 * localCentre[1] + r2 * localCentre[2] + r3;
            double depth = 0.;
            if (asMarker) {
                if (hitMarker(centre, viewProjection, width, height, pixel, markerPixels, &depth)) {
                    offer(depth, centre, group.prototype, group.instancer, true, group.indices[at]);
                }
                continue;
            }
            // A bounding sphere first: most instances are far from the ray.
            const double scale = std::max({ r0.GetLength(), r1.GetLength(), r2.GetLength() });
            const GfVec3d toCentre = centre - origin;
            const GfVec3d across = toCentre - unit * GfDot(toCentre, unit);
            if (across.GetLength() > localRadius * scale) {
                continue;
            }
            const GfMatrix3d linear(r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], r2[0], r2[1], r2[2]);
            double determinant = 0.;
            const GfMatrix3d toLocal = linear.GetInverse(&determinant);
            if (determinant == 0.) {
                continue;
            }
            const GfVec3d localOrigin = (origin - r3) * toLocal;
            const GfVec3d localDirection = direction * toLocal;
            double t = 0.;
            if (enterBox(localOrigin, localDirection, range.min, range.max, &t)) {
                const GfVec3d point = origin + direction * t;
                offer(viewProjection.Transform(point)[2], point, group.prototype, group.instancer, true, group.indices[at]);
            }
        }
    }

    return found;
} // SceneDrawBoxes::pickBudgetBox

std::vector<SdfPath>
SceneDrawBoxes::boxedRows(const UsdStageRefPtr& stage,
                          const SceneDrawFilter::Rules& rules)
{
    std::vector<SdfPath> rows;

    if (!stage) {
        return rows;
    }
    const Usd_PrimFlagsPredicate predicate = rowPredicate();
    for (const SceneDrawFilter::Rules::value_type& rule : rules) {
        if ((rule.second != SceneDrawFilter::eRuleHide) && (rule.second != SceneDrawFilter::eRulePassThrough)) {
            continue;
        }
        if (!rule.first.IsAbsoluteRootOrPrimPath() || !parentShown(rules, rule.first)) {
            continue;
        }
        const UsdPrim prim = stage->GetPrimAtPath(rule.first);
        if (!prim || !predicate(prim)) {
            continue;
        }
        if (rule.second == SceneDrawFilter::eRuleHide) {
            rows.push_back(rule.first);
            continue;
        }
        for (const UsdPrim& child : prim.GetFilteredChildren(predicate)) {
            if (rules.find(child.GetPath()) == rules.end()) {
                rows.push_back(child.GetPath());
            }
        }
    }

    return rows;
}

std::vector<SdfPath>
SceneDrawBoxes::standInRows(const UsdStageRefPtr& stage,
                            const SceneDrawFilter::Rules& rules,
                            bool byModel)
{
    std::vector<SdfPath> rows;

    if (!stage) {
        return rows;
    }
    if (byModel) {
        // A prim is a model only below a chain of models, so the range never leaves the model
        // hierarchy, which stays small however many prims lie below it.
        UsdPrimRange range = stage->Traverse(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract && UsdPrimIsModel);
        for (UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
            const SdfPath& path = it->GetPath();
            const SceneDrawFilter::Rules::const_iterator rule = rules.find(path);
            if (rule != rules.end()) {
                if (rule->second == SceneDrawFilter::eRuleHidden) {
                    it.PruneChildren();
                    continue;
                }
                if (rule->second == SceneDrawFilter::eRuleHide) {
                    it.PruneChildren();
                }
            }
            rows.push_back(path);
        }
        if (!rows.empty()) {
            return rows;
        }
    }
    const Usd_PrimFlagsPredicate predicate = rowPredicate();
    for (const SceneDrawFilter::Rules::value_type& rule : rules) {
        if ((rule.second != SceneDrawFilter::eRuleShow) && (rule.second != SceneDrawFilter::eRulePassThrough)) {
            continue;
        }
        if (!rule.first.IsPrimPath() || !parentShown(rules, rule.first)) {
            continue;
        }
        const UsdPrim prim = stage->GetPrimAtPath(rule.first);
        if (prim && predicate(prim)) {
            rows.push_back(rule.first);
        }
    }
    const std::vector<SdfPath> boxed = boxedRows(stage, rules);
    rows.insert(rows.end(), boxed.begin(), boxed.end());
    if (!rows.empty()) {
        return rows;
    }
    for (const UsdPrim& child : stage->GetPseudoRoot().GetFilteredChildren(predicate)) {
        const SceneDrawFilter::Rules::const_iterator rule = rules.find(child.GetPath());
        if ((rule == rules.end()) || (rule->second != SceneDrawFilter::eRuleHidden)) {
            rows.push_back(child.GetPath());
        }
    }

    return rows;
} // SceneDrawBoxes::standInRows

void
SceneDrawBoxes::run()
{
    for (;;) {
        Request request;
        bool due = false;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _changed.wait(lock, [this]() { return _quit || (_taken != _generation) || (_orderMade != _orderWanted) || budgetDue(); });
            if (_quit) {
                lock.unlock();
                // The destructor joins this thread on the main thread, and freeing what a large
                // stage filled these with takes seconds.
                SceneTeardown::release(std::make_tuple(std::move(_entries), std::move(_unitEntries), std::move(_bboxCache)));
                _entries.clear();
                _unitEntries.clear();
                _bboxCache.reset();

                return;
            }
            if (_taken != _generation) {
                request.rules = _rules;
                request.time = _time;
                request.generation = _generation;
                request.contentGeneration = _contentGeneration;
                request.drawnGeneration = _drawnGeneration;
                request.counting = _counting;
                _taken = _generation;
                due = true;
            } else if (_orderMade != _orderWanted) {
                const ListPtr list = _list;
                const GfMatrix4d viewProjection = _orderViewProjection;
                const int width = _orderWidth;
                const int height = _orderHeight;
                const U64 wanted = _orderWanted;
                lock.unlock();
                SceneDrawFilter::Order order;
                if (list) {
                    order = std::make_shared<const std::vector<SdfPath>>(nearFirst(*list, viewProjection, width, height));
                }
                lock.lock();
                _order = std::move(order);
                _orderMade = wanted;
                lock.unlock();
                _changed.notify_all();
                continue;
            }
        }
        if (due) {
            compute(request);
        }
        solveBudget();
    }
}

void
SceneDrawBoxes::compute(const Request& request)
{
    if (computeBoxes(request) && request.counting) {
        countUnits(request);
    }
}

void
SceneDrawBoxes::syncStage(const UsdStageRefPtr& stage,
                          U64 version,
                          double time)
{
    if ((get_pointer(stage) != _entriesStage) || (version != _entriesVersion)) {
        _entries.clear();
        _unitEntries.clear();
        _bboxCache.reset();
        _entriesStage = get_pointer(stage);
        _entriesVersion = version;
    }
    const UsdTimeCode at(time);
    if (!_bboxCache) {
        _bboxCache.reset(new UsdGeomBBoxCache(at, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true));
    } else if (_bboxCache->GetTime() != at) {
        _bboxCache->SetTime(at);
    }
}

bool
SceneDrawBoxes::computeBoxes(const Request& request)
{
    std::vector<Box> boxes;

    if (!_lister && !boxesAnything(request.rules)) {
        publish(request, std::move(boxes), true);

        return true;
    }
    std::vector<SdfPath> rows;
    // Where each row without a box yet is, and which rows those are.
    std::vector<GfVec3f> origins;
    std::vector<std::size_t> unseen;
    std::unique_ptr<UsdGeomXformCache> xforms;
    std::size_t originNext = 0;
    bool listed = false;
    std::size_t next = 0;
    bool done = false;
    bool dropped = false;
    bool pendingSent = false;
    while (!done && !dropped && !_quit) {
        bool yielded = false;
        bool pendingDue = false;
        _reader([&](const UsdStageRefPtr& stage, U64 version) {
            if (!stage) {
                rows.clear();
                boxes.clear();
                done = true;

                return;
            }
            if ((get_pointer(stage) != _entriesStage) || (version != _entriesVersion)) {
                listed = false;
            }
            syncStage(stage, version, request.time);
            const UsdTimeCode time(request.time);
            if (!listed) {
                rows = _lister ? _lister(stage, request.rules) : boxedRows(stage, request.rules);
                boxes.clear();
                boxes.reserve(rows.size());
                next = 0;
                unseen.clear();
                for (std::size_t index = 0; index < rows.size(); ++index) {
                    if (_entries.find(rows[index]) == _entries.end()) {
                        unseen.push_back(index);
                    }
                }
                origins.assign(rows.size(), GfVec3f(0.f));
                originNext = 0;
                xforms.reset(new UsdGeomXformCache(time));
                listed = true;
            }
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            const auto passOver = [&]() {
                return (_shouldYield && _shouldYield()) || (std::chrono::steady_clock::now() - start > kPassBudget);
            };
            // Transforms cost far less than bounds, so every row has a marker before any bound.
            while (originNext < unseen.size()) {
                if ((_contentGeneration != request.contentGeneration) || _quit) {
                    dropped = true;

                    return;
                }
                const std::size_t index = unseen[originNext];
                origins[index] = GfVec3f(xforms->GetLocalToWorldTransform(stage->GetPrimAtPath(rows[index])).ExtractTranslation());
                ++originNext;
                if (passOver()) {
                    yielded = true;

                    return;
                }
            }
            while (next < rows.size()) {
                if ((_contentGeneration != request.contentGeneration) || _quit) {
                    dropped = true;

                    return;
                }
                const SdfPath& row = rows[next];
                std::unordered_map<SdfPath, Entry, SdfPath::Hash>::iterator found = _entries.find(row);
                if ((found == _entries.end()) || (found->second.varying && (found->second.time != request.time))) {
                    found = _entries.insert_or_assign(row, computeEntry(stage->GetPrimAtPath(row), request.time)).first;
                }
                boxes.push_back(found->second.box);
                ++next;
                if ((next < rows.size()) && passOver()) {
                    yielded = true;
                    pendingDue = !pendingSent;

                    return;
                }
            }
            done = true;
        });
        if (pendingDue) {
            // The rows still to come keep the box they had, or show as markers until theirs.
            pendingSent = true;
            std::vector<Box> partial(boxes);
            partial.reserve(rows.size());
            for (std::size_t index = next; index < rows.size(); ++index) {
                const std::unordered_map<SdfPath, Entry, SdfPath::Hash>::const_iterator found = _entries.find(rows[index]);
                if (found != _entries.end()) {
                    partial.push_back(found->second.box);
                    continue;
                }
                Box pending;
                pending.path = rows[index];
                pending.min = origins[index];
                pending.max = origins[index];
                pending.kind = eKindPending;
                partial.push_back(pending);
            }
            publish(request, std::move(partial), false);
        }
        if (yielded) {
            // A shared_mutex taken again at once can keep a waiting writer out.
            do {
                std::this_thread::sleep_for(kPassGap);
            } while (_shouldYield && _shouldYield() && !_quit && (_contentGeneration == request.contentGeneration));
        }
    }
    if (!done || dropped) {
        if (_quit) {
            SceneTeardown::release(std::make_tuple(std::move(rows), std::move(boxes), std::move(origins), std::move(unseen), std::move(xforms)));
        }

        return false;
    }
    std::unordered_map<SdfPath, Entry, SdfPath::Hash> kept;
    kept.reserve(rows.size());
    for (const SdfPath& row : rows) {
        std::unordered_map<SdfPath, Entry, SdfPath::Hash>::iterator found = _entries.find(row);
        if (found != _entries.end()) {
            kept.insert(*found);
        }
    }
    _entries.swap(kept);
    publish(request, std::move(boxes), true);

    return true;
} // SceneDrawBoxes::computeBoxes

bool
SceneDrawBoxes::countUnits(const Request& request)
{
    std::vector<Unit> units;
    std::unique_ptr<UsdPrimRange> range;
    UsdPrimRange::iterator next;
    const void* passStage = nullptr;
    U64 passVersion = 0;
    bool started = false;
    bool done = false;
    bool dropped = false;

    while (!done && !dropped && !_quit) {
        bool yielded = false;
        _reader([&](const UsdStageRefPtr& stage, U64 version) {
            if (!stage) {
                units.clear();
                done = true;

                return;
            }
            syncStage(stage, version, request.time);
            if (!started || (get_pointer(stage) != passStage) || (version != passVersion)) {
                // The walk so far was of another stage, or of this one before an edit.
                range.reset(new UsdPrimRange(UsdPrimRange::Stage(stage, rowPredicate())));
                next = range->begin();
                units.clear();
                passStage = get_pointer(stage);
                passVersion = version;
                started = true;
            }
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            while (next != range->end()) {
                if ((_contentGeneration != request.contentGeneration) || _quit) {
                    dropped = true;

                    return;
                }
                const UsdPrim prim = *next;
                const UnitStepEnum step = unitStep(request.rules, prim, request.time);
                if ((step == eUnitStepUnit) || (step == eUnitStepUnitPrune)) {
                    std::unordered_map<SdfPath, UnitEntry, SdfPath::Hash>::iterator found = _unitEntries.find(prim.GetPath());
                    if ((found == _unitEntries.end()) || (found->second.varying && (found->second.time != request.time))) {
                        found = _unitEntries.insert_or_assign(prim.GetPath(), computeUnit(prim, request.time)).first;
                    }
                    units.push_back(found->second.unit);
                }
                if ((step == eUnitStepPrune) || (step == eUnitStepUnitPrune)) {
                    next.PruneChildren();
                }
                ++next;
                if ((_shouldYield && _shouldYield()) || (std::chrono::steady_clock::now() - start > kPassBudget)) {
                    yielded = true;

                    return;
                }
            }
            done = true;
        });
        if (yielded) {
            do {
                std::this_thread::sleep_for(kPassGap);
            } while (_shouldYield && _shouldYield() && !_quit && (_contentGeneration == request.contentGeneration));
        }
    }
    if (!done || dropped) {
        if (!_quit) {
            ++_countsDiscarded;
        }

        return false;
    }
    for (Unit& unit : units) {
        if (!unit.instances) {
            continue;
        }
        const std::unordered_map<SdfPath, UnitEntry, SdfPath::Hash>::iterator found = _unitEntries.find(unit.path);
        if ((found == _unitEntries.end()) || !found->second.settle) {
            continue;
        }
        if (!settleInstances(request, &found->second.unit)) {
            if (!_quit) {
                ++_countsDiscarded;
            }

            return false;
        }
        found->second.settle = false;
        unit = found->second.unit;
    }
    std::unordered_map<SdfPath, UnitEntry, SdfPath::Hash> kept;
    kept.reserve(units.size());
    for (const Unit& unit : units) {
        std::unordered_map<SdfPath, UnitEntry, SdfPath::Hash>::iterator found = _unitEntries.find(unit.path);
        if (found != _unitEntries.end()) {
            kept.insert(*found);
        }
    }
    _unitEntries.swap(kept);
    publishUnits(request, std::move(units));

    return true;
} // SceneDrawBoxes::countUnits

SceneDrawBoxes::UnitEntry
SceneDrawBoxes::computeUnit(const UsdPrim& prim,
                            double time)
{
    UnitEntry entry;
    const UsdGeomPointInstancer instancer(prim);

    // A PointInstancer's world bound visits every instance, too long a read to hold the lock for
    // with millions of them: settleInstances() takes it from the arrays read here, off the lock.
    if (instancer && prim.IsLoaded()) {
        entry.time = time;
        entry.unit.path = prim.GetPath();
        entry.varying = boundMightVary(prim) || countsMightVary(prim);
        entry.settle = true;
        readInstances(instancer, time, &entry);

        return entry;
    }
    const Entry bound = computeEntry(prim, time);

    entry.time = time;
    entry.unit.path = bound.box.path;
    entry.unit.min = bound.box.min;
    entry.unit.max = bound.box.max;
    entry.unit.bounded = (bound.box.kind != eKindMarker);
    if (!prim.IsLoaded()) {
        // An unloaded payload draws nothing, so it counts no faces.
        entry.unit.state = SceneViewBudget::eStateUnloaded;
        entry.varying = bound.varying;

        return entry;
    }
    entry.varying = bound.varying || countsMightVary(prim);
    entry.unit.faces = gprimFaces(prim, time);
    entry.unit.mesh = prim.IsA<UsdGeomMesh>();
    if (prim.IsInstanceProxy()) {
        for (UsdPrim at = prim.GetParent(); at; at = at.GetParent()) {
            if (at.IsInstance()) {
                entry.unit.nativeInstance = at.GetPath();
                break;
            }
        }
    }

    return entry;
} // SceneDrawBoxes::computeUnit

void
SceneDrawBoxes::readInstances(const UsdGeomPointInstancer& instancer,
                              double time,
                              UnitEntry* entry)
{
    const UsdPrim prim = instancer.GetPrim();
    const UsdTimeCode at(time);
    std::shared_ptr<Instances> instances = std::make_shared<Instances>();
    UsdGeomXformCache xforms(at);
    instances->toWorld = xforms.GetLocalToWorldTransform(prim);
    instancer.GetProtoIndicesAttr().Get(&instances->protoIndices, at);
    instancer.GetPositionsAttr().Get(&instances->positions, at);
    instancer.GetOrientationsAttr().Get(&instances->orientations, at);
    instancer.GetOrientationsfAttr().Get(&instances->orientationsf, at);
    instancer.GetScalesAttr().Get(&instances->scales, at);
    instances->mask = instancer.ComputeMaskAtTime(at);
    SdfPathVector paths;
    const std::vector<U64> faces = prototypeFaces(instancer, time, 1, &paths);
    const UsdStageWeakPtr stage = prim.GetStage();
    instances->prototypes.reserve(paths.size());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        Prototype prototype;
        prototype.path = paths[index];
        prototype.faces = faces[index];
        const UsdPrim prototypePrim = stage->GetPrimAtPath(paths[index]);
        if (prototypePrim) {
            entry->varying = entry->varying || boundMightVary(prototypePrim) || countsMightVary(prototypePrim);
            GfBBox3d box = _bboxCache->ComputeUntransformedBound(prototypePrim);
            GfMatrix4d local(1.);
            bool resets = false;
            const UsdGeomXformable xformable(prototypePrim);
            if (xformable && xformable.GetLocalTransformation(&local, &resets, at)) {
                box.Transform(local);
            }
            const GfRange3d range = box.ComputeAlignedRange();
            if (!range.IsEmpty()) {
                prototype.min = GfVec3f(range.GetMin());
                prototype.max = GfVec3f(range.GetMax());
                prototype.bounded = true;
            }
        }
        instances->prototypes.push_back(prototype);
    }
    entry->unit.instances = instances;
} // SceneDrawBoxes::readInstances

bool
SceneDrawBoxes::settleInstances(const Request& request,
                                Unit* unit) const
{
    const Instances& instances = *unit->instances;
    const std::size_t size = instances.size();
    GfRange3d range;
    U64 faces = 0;

    for (std::size_t begin = 0; begin < size; begin += kInstanceChunk) {
        if ((_contentGeneration != request.contentGeneration) || _quit) {
            return false;
        }
        const std::size_t end = (std::min)(begin + kInstanceChunk, size);
        for (std::size_t index = begin; index < end; ++index) {
            if (!instances.draws(index)) {
                continue;
            }
            const Prototype& prototype = instances.prototypes[std::size_t(instances.protoIndices[index])];
            faces += prototype.faces;
            const GfMatrix4d toWorld = instances.instanceTransform(index);
            if (prototype.bounded) {
                range.UnionWith(GfBBox3d(GfRange3d(GfVec3d(prototype.min), GfVec3d(prototype.max)), toWorld).ComputeAlignedRange());
            } else {
                range.UnionWith(toWorld.Transform(GfVec3d(0.)));
            }
        }
    }
    unit->faces = faces;
    if (range.IsEmpty()) {
        const GfVec3f origin(instances.toWorld.ExtractTranslation());
        unit->min = origin;
        unit->max = origin;
        unit->bounded = false;
    } else {
        unit->min = GfVec3f(range.GetMin());
        unit->max = GfVec3f(range.GetMax());
        unit->bounded = true;
    }

    return true;
} // SceneDrawBoxes::settleInstances

SceneDrawBoxes::Entry
SceneDrawBoxes::computeEntry(const UsdPrim& prim,
                             double time)
{
    Entry entry;

    entry.time = time;
    entry.box.path = prim.GetPath();
    entry.varying = boundMightVary(prim);
    const GfRange3d range = _bboxCache->ComputeWorldBound(prim).ComputeAlignedRange();
    if (range.IsEmpty()) {
        UsdGeomXformCache xforms((UsdTimeCode(time)));
        const GfVec3f origin(xforms.GetLocalToWorldTransform(prim).ExtractTranslation());
        entry.box.min = origin;
        entry.box.max = origin;
        entry.box.kind = eKindMarker;
    } else {
        entry.box.min = GfVec3f(range.GetMin());
        entry.box.max = GfVec3f(range.GetMax());
        entry.box.kind = prim.IsLoaded() ? eKindBox : eKindUnloaded;
    }

    return entry;
}

void
SceneDrawBoxes::publish(const Request& request,
                        std::vector<Box>&& boxes,
                        bool complete)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (request.contentGeneration != _contentGeneration) {
            return;
        }
        std::shared_ptr<List> list = std::make_shared<List>();
        list->generation = request.generation;
        list->time = request.time;
        list->complete = complete;
        list->boxes = std::move(boxes);
        _list = list;
        if (_hasOrderView) {
            ++_orderWanted;
        }
    }
    _changed.notify_all();
    if (_published) {
        _published();
    }
}

void
SceneDrawBoxes::publishUnits(const Request& request,
                             std::vector<Unit>&& units)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if ((request.contentGeneration != _contentGeneration) || !_counting) {
            ++_countsDiscarded;

            return;
        }
        std::shared_ptr<Units> published = std::make_shared<Units>();
        published->generation = request.generation;
        published->contentGeneration = request.contentGeneration;
        published->drawnGeneration = request.drawnGeneration;
        published->time = request.time;
        published->units = std::move(units);
        _units = published;
        if (_budgetLive) {
            ++_budgetSerial;
            _budgetPending = true;
        }
    }
    _changed.notify_all();
    if (_published) {
        _published();
    }
}

NATRON_NAMESPACE_EXIT
