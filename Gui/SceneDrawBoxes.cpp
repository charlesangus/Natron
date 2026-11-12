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

#include <cmath>
#include <limits>
#include <utility>

#ifdef emit
#define NATRON_SCENEDRAWBOXES_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/modelAPI.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
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

// The scene graph's rows, so every row it lists can be boxed.
Usd_PrimFlagsPredicate
rowPredicate()
{
    return UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract);
}

// Mirrors the filter: the rule nearest a prim, its own included, decides it, and a Children row
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
        case SceneDrawFilter::eRuleExpand:
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
        if ((rule.second == SceneDrawFilter::eRuleHide) || (rule.second == SceneDrawFilter::eRuleExpand)) {
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
} // anon namespace

SceneDrawBoxes::SceneDrawBoxes(const StageReader& reader,
                               const std::function<bool()>& shouldYield,
                               const std::function<void()>& published)
    : _reader(reader)
    , _shouldYield(shouldYield)
    , _published(published)
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
    }
    _changed.notify_all();
}

void
SceneDrawBoxes::setStageVersion(U64 version)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (version == _stageVersion) {
            return;
        }
        _stageVersion = version;
        ++_generation;
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

U64
SceneDrawBoxes::getGeneration() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _generation;
}

bool
SceneDrawBoxes::waitForList(U64 generation,
                            std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _changed.wait_for(lock, timeout, [&]() { return _list && _list->complete && (_list->generation >= generation); });
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
        if ((rule.second != SceneDrawFilter::eRuleHide) && (rule.second != SceneDrawFilter::eRuleExpand)) {
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

void
SceneDrawBoxes::run()
{
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _changed.wait(lock, [this]() { return _quit || (_taken != _generation); });
            if (_quit) {
                return;
            }
            request.rules = _rules;
            request.time = _time;
            request.generation = _generation;
            request.contentGeneration = _contentGeneration;
            _taken = _generation;
        }
        compute(request);
    }
}

void
SceneDrawBoxes::compute(const Request& request)
{
    std::vector<Box> boxes;

    if (!boxesAnything(request.rules)) {
        publish(request, std::move(boxes), true);

        return;
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
                // What was read before belongs to another stage, or to this one before an edit.
                _entries.clear();
                _bboxCache.reset();
                _entriesStage = get_pointer(stage);
                _entriesVersion = version;
                listed = false;
            }
            const UsdTimeCode time(request.time);
            if (!_bboxCache) {
                _bboxCache.reset(new UsdGeomBBoxCache(time, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true));
            } else if (_bboxCache->GetTime() != time) {
                _bboxCache->SetTime(time);
            }
            if (!listed) {
                rows = boxedRows(stage, request.rules);
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
        return;
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
} // SceneDrawBoxes::compute

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
    }
    _changed.notify_all();
    if (_published) {
        _published();
    }
}

NATRON_NAMESPACE_EXIT
