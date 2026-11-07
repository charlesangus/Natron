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

#ifndef Engine_Nodes_Scene_Utility_Tracer_h
#define Engine_Nodes_Scene_Utility_Tracer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <cstddef>
#include <cstdint>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Intersects batches of rays with a UtilityScene. The render pipeline sees only this, so an
 * executor can trace with whichever backend suits its device.
 *
 * update() must not overlap anything else. intersect() and occluded() may run concurrently with each
 * other.
 **/
class Tracer {
public:
    // What a hit's ids hold when its ray hit nothing.
    static constexpr std::uint32_t kNoHit = 0xffffffffu;

    // Eight arrays of count values. A ray only hits between tMin and tMax, measured along direction.
    struct RayBatch {
        const float* originX = nullptr;
        const float* originY = nullptr;
        const float* originZ = nullptr;
        const float* directionX = nullptr;
        const float* directionY = nullptr;
        const float* directionZ = nullptr;
        const float* tMin = nullptr;
        const float* tMax = nullptr;
        std::size_t count = 0;
    };

    // One value per ray in each array.
    struct HitBatch {
        // Infinity on a miss.
        float* t = nullptr;
        // Position on the triangle: (1 - u - v) * corner0 + u * corner1 + v * corner2.
        float* u = nullptr;
        float* v = nullptr;
        // The geometry's position among the snapshot's geometries, in map order.
        std::uint32_t* geometry = nullptr;
        // The index into the snapshot's instances.
        std::uint32_t* instance = nullptr;
        // The index of the triangle in Geometry::triangles.
        std::uint32_t* primitive = nullptr;
    };

    struct Stats {
        std::size_t geometryBuilds = 0;
        std::size_t geometryRefits = 0;
        std::size_t instanceSceneBuilds = 0;
        std::size_t instanceSceneUpdates = 0;
    };

    virtual ~Tracer() { }

    /**
     * @brief Brings the acceleration structures up to the snapshot, doing nothing for a version they
     * already hold. Uses at most threads threads, the caller included.
     **/
    virtual void update(const UtilityScene::Snapshot& snapshot,
                        int threads)
        = 0;
    virtual void intersect(const RayBatch& rays,
                           const HitBatch& hits) const
        = 0;
    // Writes 1 for each ray with anything in its way, 0 for the others.
    virtual void occluded(const RayBatch& rays,
                          std::uint8_t* blocked) const
        = 0;
    virtual Stats stats() const = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_Tracer_h
