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

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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

    /**
     * @brief Updates the geometry and instance tables raster passes draw from, without building or refitting
     * any structure: a frame whose every hit comes from the visibility buffer never traces, and the builds
     * cost far more than its draws. A trace before the next update() fails. The next update() builds what this
     * skipped, and one of the same version builds only that.
     **/
    bool updateForRaster(const UtilityScene::Snapshot& snapshot,
                         const std::atomic<bool>* stop = nullptr);
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

    /**
     * @brief One drawn geometry of a RasterScene. points and indices are the very buffers its triangle
     * structure was built from, read through their device addresses: pointSteps steps of pointCount points
     * (three floats each, one after another), above one step only for a deforming geometry, and three uint32
     * indices per triangle, which can also be bound as an index buffer. Its instances are the instanceCount
     * entries of RasterScene::instances from firstInstance on.
     **/
    struct RasterGeometry {
        const gpu::GpuBuffer* points = nullptr;
        const gpu::GpuBuffer* indices = nullptr;
        std::uint32_t pointCount = 0;
        std::uint32_t triangleCount = 0;
        std::uint32_t pointSteps = 1;
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
    };

    /**
     * @brief The last update's scene as a raster pass draws it, grouped by geometry. Only the instance tables
     * are new; the points and triangles are the tracer's own. instances holds two uint32 per drawn instance:
     * the placement a hit reports as its instance, and the first float4 row of its transform in transforms.
     * There the first row holds the step count in x as a uint32: 0 for an instance that holds still, followed
     * by the four rows of its row-vector object-to-world matrix, or two or more, followed by that many steps
     * as UtilityTypes.slang's stepAtTime() reads them. commands holds one VkDrawIndexedIndirectCommand per
     * geometry, drawing its triangles for its instances. hitTables holds what rebuilds a hit from a drawn
     * placement and triangle, in rows of four uint32: a header holding the device address of transforms, low word
     * first; then one row per placement of the snapshot, holding the dense geometry index a hit reports, the first
     * row of the placement's transform and the row of its geometry's record, all kNoHit for a placement that is
     * not drawn; then two rows per drawn geometry, the device addresses of its points and of its indices, then its
     * point count and point steps. All four are read through device addresses or as an indirect buffer, and live
     * as long as the scene the shared pointer holds.
     **/
    struct RasterScene {
        std::vector<RasterGeometry> geometries;
        const gpu::GpuBuffer* instances = nullptr;
        const gpu::GpuBuffer* transforms = nullptr;
        const gpu::GpuBuffer* commands = nullptr;
        const gpu::GpuBuffer* hitTables = nullptr;
    };

    /**
     * @brief The raster form of the last update's scene, built on the first request after it. Null, with
     * a status that says why, when the tracer cannot run or the tables cannot be held; an empty scene has
     * no geometries. No update() may run while it is in use.
     **/
    gpu::GpuStatus rasterScene(std::shared_ptr<const RasterScene>& out) const;

    // What recordVisibility() draws with.
    struct VisibilityPass {
        // The rows of the row-vector view-projection, from world space into Vulkan's clip space: y runs down
        // the attachment and depth from 0 at w = near to w at far.
        float viewProjection[4][4] = { { 1.f, 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 1.f } };
        // The pass's sub-pixel offset in texels of the pass's attachment, applied after the projection.
        float jitter[2] = { 0.f, 0.f };
        // The pass's time, from 0 at the shutter's opening to 1 at its closing, as a ray's: points are
        // interpolated linearly, translation and stretch linearly and rotation by slerp.
        float time = 0.5f;
        // When not empty, what RasterCutout.slang binds, in its binding order, so that a texel where a surface has
        // a hole is left to the surface behind it rather than drawn.
        std::span<const gpu::GpuBuffer* const> cutoutBuffers;
        // The window pixel of the attachment's first texel, which the cutout test's camera rays go through.
        int origin[2] = { 0, 0 };
    };

    /**
     * @brief Records into the raster pass open on ctx, a raster context's whose colour attachment is
     * R32G32_UINT and that has a depth attachment, each drawn instance at the pass's time: every texel a
     * triangle covers nearest gets the instance and primitive ids a ray through its centre would hit, and
     * the others keep the pass's clear value. One indirect draw per geometry; the context's abort check runs
     * before each. The context keeps the scene alive until it is destroyed; no update() may run before then.
     **/
    gpu::GpuStatus recordVisibility(gpu::GpuRecordContext& ctx,
                                    const VisibilityPass& pass) const;

    // Lowers the instances one top-level structure takes, so that tests split small scenes. Never raises it.
    void limitInstancesPerStructure(std::size_t count);

    // The rays intersect() and occluded() put in one submission at most. It starts at 2^20 and halves, down to
    // 2^12, after each submission that takes longer than a second.
    std::size_t raysPerSubmission() const;

    // Lowers raysPerSubmission(), so that tests trace in several submissions. Never raises it.
    void limitRaysPerSubmission(std::size_t count);

    // Replaces the clock that times the trace submissions, so that tests can make them look slow.
    void useClock(std::function<std::chrono::steady_clock::time_point()> now);

    /**
     * @brief Lowers the memory budget an update compares the scene's acceleration structures with, so that
     * tests can exceed it. Without this the budget is the device-local heap's that VK_EXT_memory_budget
     * reports, and nothing is compared without the extension. An update over it fails with OutOfMemory.
     **/
    void limitMemoryBudget(std::uint64_t bytes);

    // The top-level structures the last update split the scene's instances over.
    std::size_t instanceStructureCount() const;

    // The structures of boxes the last update swept the moving and deforming instances into, one per shutter
    // segment: none when nothing moves.
    std::size_t moverStructureCount() const;

    // The kernels the record functions dispatch; empty when the tracer cannot run.
    std::vector<std::shared_ptr<gpu::GpuKernel>> kernels() const;

private:
    bool updateScene(const UtilityScene::Snapshot& snapshot,
                     const std::atomic<bool>* stop,
                     bool structures);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Scene_Utility_GpuTracer_h
