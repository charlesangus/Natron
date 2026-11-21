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

#ifndef Engine_Nodes_Scene_Utility_GpuTracer_h
#define Engine_Nodes_Scene_Utility_GpuTracer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef NATRON_HAVE_GPU

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Global/Macros.h"

#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"

namespace gpu {
class GpuBuffer;
class GpuBufferPool;
class GpuKernel;
class GpuKernelRegistry;
class GpuRecordContext;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief Traces with Vulkan ray queries: one bottom-level structure per geometry under top-level structures of
 * instances. Ids, distances and barycentrics are those EmbreeTracer reports.
 *
 * One top-level structure takes at most 2^24 instances, and fewer where the device's instance count or
 * allocation size is lower. A scene past that is ordered along a space-filling curve through the instances'
 * centres and split into consecutive runs of that many, one structure each, which every ray searches in turn.
 *
 * Geometry is built once per topology and refit in place when only its points move. The top-level
 * structures are rebuilt whole whenever the instances or the geometry under them change; stats() still
 * counts a rebuild that only follows moves or refits as an instanceSceneUpdate, as EmbreeTracer does.
 *
 * The geometry's builds, refits and compaction run on a thread of its own, since one structure's build cannot
 * be split and on a software device takes seconds: a stop returns at once and leaves it running, for the next
 * update() to wait for and keep. Destroying the tracer waits for it.
 *
 * A device failure never throws. update() returns false and status() says why; a trace that fails
 * reports every ray it did not finish as a miss and records why in status(). The registry and the pool
 * must outlive the tracer.
 **/
class GpuTracer : public Tracer {
public:
    GpuTracer(gpu::GpuKernelRegistry& registry,
              gpu::GpuBufferPool& pool);
    virtual ~GpuTracer();

    // Empty when the device can trace, else the Vulkan extensions and features it lacks.
    static std::string missingRayQuerySupport(const gpu::GpuDevice& device);

    virtual bool update(const UtilityScene::Snapshot& snapshot,
                        int threads,
                        const std::atomic<bool>* stop = nullptr) OVERRIDE;
    virtual void intersect(const RayBatch& rays,
                           const HitBatch& hits) const OVERRIDE;
    virtual void occluded(const RayBatch& rays,
                          std::uint8_t* blocked,
                          const OcclusionFilter* filter = nullptr) const OVERRIDE;
    virtual Stats stats() const OVERRIDE;
    virtual Outcome status() const OVERRIDE;

    /**
     * @brief Records a trace of count rays already on the device. rays holds the eight arrays of
     * Kernels/UtilityTypes.slang with count values each; hitValues receives t, u and v and hitIds the
     * geometry, instance and primitive, count values per array. The context keeps the structures alive
     * until it is destroyed; no update() may run before then.
     **/
    gpu::GpuStatus recordIntersect(gpu::GpuRecordContext& ctx,
                                   const gpu::GpuBuffer& rays,
                                   std::uint32_t count,
                                   const gpu::GpuBuffer& hitValues,
                                   const gpu::GpuBuffer& hitIds) const;

    // As recordIntersect(), writing one uint32 per ray to blocked: 1 when anything is in the way, else 0.
    gpu::GpuStatus recordOccluded(gpu::GpuRecordContext& ctx,
                                  const gpu::GpuBuffer& rays,
                                  std::uint32_t count,
                                  const gpu::GpuBuffer& blocked) const;

    /**
     * @brief As recordOccluded(), with the device form of an OcclusionFilter: rayLights holds a light
     * index per ray and casterMasks maskStride words per instance from maskOffset, of which an instance
     * blocks a ray only where the ray's light's bit is set. casterMasks is read through its device address,
     * so it needs VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, and the storage buffer range does not bound it.
     **/
    gpu::GpuStatus recordOccludedFiltered(gpu::GpuRecordContext& ctx,
                                          const gpu::GpuBuffer& rays,
                                          std::uint32_t count,
                                          const gpu::GpuBuffer& blocked,
                                          const gpu::GpuBuffer& rayLights,
                                          const gpu::GpuBuffer& casterMasks,
                                          std::uint32_t maskStride,
                                          std::uint32_t maskOffset) const;

    // Lowers the instances one top-level structure takes, so that tests split small scenes. Never raises it.
    void limitInstancesPerStructure(std::size_t count);

    // The top-level structures the last update split the scene's instances over.
    std::size_t instanceStructureCount() const;

    // The kernels the record functions dispatch; empty when the tracer cannot run.
    std::vector<std::shared_ptr<gpu::GpuKernel>> kernels() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Scene_Utility_GpuTracer_h
