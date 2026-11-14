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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Which casters may block which shadow rays: an instance c blocks ray r only when bit
 * rayLights[r] is set in the stride words from casterMasks + c * stride, so a light's shadows fall only
 * from the instances its shadow-link collection holds.
 **/
struct OcclusionFilter {
    const std::uint32_t* rayLights = nullptr;
    const std::uint32_t* casterMasks = nullptr;
    std::uint32_t stride = 0;
    // The instances casterMasks covers.
    std::size_t casterCount = 0;

    bool blocks(std::size_t ray,
                std::uint32_t caster) const
    {
        const std::uint32_t light = rayLights[ray];

        return ((casterMasks[static_cast<std::size_t>(caster) * stride + light / 32] >> (light % 32)) & 1u) != 0;
    }
};

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

    enum class Status {
        Ok,
        // stop ended the last update early.
        Stopped,
        // The device could not hold the structures or a batch; the message gives the sizes involved.
        OutOfMemory,
        // The device lacks what the tracer needs, or the tracer could not be set up on it.
        Unsupported,
        DeviceLost,
        Failed,
    };

    struct Outcome {
        Status status = Status::Ok;
        std::string message;
    };

    virtual ~Tracer() { }

    /**
     * @brief Brings the acceleration structures up to the snapshot, doing nothing for a version they
     * already hold. Uses at most threads threads, the caller included. Returns false when it did not
     * finish, because stop ended it or because it failed, as status() tells. Only another update may
     * be called after a false return, and it redoes what was left unfinished.
     **/
    virtual bool update(const UtilityScene::Snapshot& snapshot,
                        int threads,
                        const std::atomic<bool>* stop = nullptr)
        = 0;
    // The first failure since the last update() began, Stopped when stop ended it, or the reason the
    // tracer cannot run at all. Traces after a successful update may record a failure here too.
    virtual Outcome status() const = 0;
    virtual void intersect(const RayBatch& rays,
                           const HitBatch& hits) const
        = 0;
    // Writes 1 for each ray with anything in its way, 0 for the others. A filter with casterMasks
    // ignores the instances it says cannot block a ray.
    virtual void occluded(const RayBatch& rays,
                          std::uint8_t* blocked,
                          const OcclusionFilter* filter = nullptr) const
        = 0;
    virtual Stats stats() const = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_Tracer_h
