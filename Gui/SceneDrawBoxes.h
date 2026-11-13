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

    SceneDrawBoxes(const StageReader& reader,
                   const std::function<bool()>& shouldYield,
                   const std::function<void()>& published);

    ~SceneDrawBoxes();

    void setRules(const SceneDrawFilter::Rules& rules);
    void setTime(double time);
    void setStageVersion(U64 version);

    ListPtr getList() const;

    // The latest request.
    U64 getGeneration() const;

    // Whether a complete list answering generation or a later request was published within timeout.
    bool waitForList(U64 generation,
                     std::chrono::milliseconds timeout) const;

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

    // By path. A row the stage lacks, or one under a row that hides it, is left out.
    static std::vector<PXR_NS::SdfPath> boxedRows(const PXR_NS::UsdStageRefPtr& stage,
                                                  const SceneDrawFilter::Rules& rules);

private:
    struct Request {
        SceneDrawFilter::Rules rules;
        double time = 0.;
        U64 generation = 0;
        U64 contentGeneration = 0;
    };

    struct Entry {
        Box box;
        bool varying = false;
        double time = 0.;
    };

    void run();
    void compute(const Request& request);
    Entry computeEntry(const PXR_NS::UsdPrim& prim,
                       double time);
    void publish(const Request& request,
                 std::vector<Box>&& boxes,
                 bool complete);

    SceneDrawBoxes(const SceneDrawBoxes&) = delete;
    SceneDrawBoxes& operator=(const SceneDrawBoxes&) = delete;

    const StageReader _reader;
    const std::function<bool()> _shouldYield;
    const std::function<void()> _published;

    mutable std::mutex _mutex;
    mutable std::condition_variable _changed;
    SceneDrawFilter::Rules _rules;
    double _time = 0.;
    U64 _stageVersion = 0;
    U64 _generation = 0;
    U64 _taken = 0;
    std::atomic<U64> _contentGeneration { 0 };
    std::atomic<bool> _quit { false };
    ListPtr _list;

    // The worker's own.
    std::unordered_map<PXR_NS::SdfPath, Entry, PXR_NS::SdfPath::Hash> _entries;
    std::unique_ptr<PXR_NS::UsdGeomBBoxCache> _bboxCache;
    const void* _entriesStage = nullptr;
    U64 _entriesVersion = 0;

    std::thread _thread;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWBOXES_H
