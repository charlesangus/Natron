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

#include "WavefrontGpu.h"

#include <atomic>
#include <chrono>

#include "Engine/AppManager.h"
#include "Engine/GpuContext.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityGpuLimits.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"

#include "utility_raygen_spirv.h"
#include "utility_shade_surface_spirv.h"

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_ENTER

namespace {
// The application's GPU starts on a thread of its own at launch; a frame asked for that early waits for it.
const std::chrono::seconds kGpuInitWait(10);
} // anon namespace

UtilityGpu
acquireUtilityGpu()
{
    UtilityGpu result;

#ifdef NATRON_HAVE_GPU
    GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;

    if (!context) {
        result.unavailable = "GPU compute is turned off in the preferences";

        return result;
    }
    if (context->isInitializing()) {
        (void)context->waitForInit(kGpuInitWait);
    }
    if (!context->recoverIfLost() || !context->isAvailable()) {
        result.unavailable = "no usable Vulkan device is available";

        return result;
    }
    result.hold = context->holdRuntime();
    result.runtime = context->runtime();
    if (!result.hold || !result.runtime || result.runtime->isLost()) {
        result = UtilityGpu();
        result.unavailable = "the Vulkan device was lost";
    }
#else
    result.unavailable = "Natron was built without the Vulkan backend";
#endif

    return result;
}

std::uint64_t
utilityGpuBudget()
{
#ifdef NATRON_HAVE_GPU
    GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;

    if (!context || context->isInitializing()) {
        return 0;
    }

    const std::shared_ptr<void> hold = context->holdRuntime();
    gpu::GpuRuntime* runtime = hold ? context->runtime() : nullptr;

    return (runtime && !runtime->isLost()) ? runtime->pool().budget() : 0;
#else
    return 0;
#endif
}

#ifdef NATRON_HAVE_GPU

namespace {
static_assert(sizeof(KernelRayGenParams) == utility_raygen_push_bytes);
static_assert(sizeof(KernelShadeParams) == utility_shade_surface_push_bytes);
static_assert(utility_raygen_storage_buffers == 1 && utility_shade_surface_storage_buffers == 19);
static_assert(sizeof(KernelGeometryRecord) == 32);
static_assert(sizeof(KernelMaterialRecord) == 144 && sizeof(KernelTextureBinding) == 112 && sizeof(KernelProjectionRecord) == 256);

const std::size_t kRayArrays = kUtilityRayArrays;
const std::size_t kHitArrays = kUtilityHitArrays;

// Bounds how long a stop waits for the batch in flight, which on an integrated GPU or lavapipe is the
// time of this many rays.
const std::uint64_t kMaxBatchRays = std::min<std::uint64_t>(kUtilityMaxDispatchRays, std::uint64_t(1) << 18);

// Vulkan has no zero-sized buffer, and a scene without colours or triangles still binds all of them.
const VkDeviceSize kMinTableBytes = 16;

Tracer::Status
classify(const gpu::GpuStatus& status,
         const gpu::GpuDevice& device)
{
    return classifyUtilityGpuStatus(status, device);
}

gpu::GpuKernelDesc
rayGenDesc()
{
    return gpu::GpuKernelDesc {
        utility_raygen_spirv, utility_raygen_spirv_words, utility_raygen_entry, utility_raygen_storage_buffers, utility_raygen_push_bytes, { utility_raygen_group_size[0], utility_raygen_group_size[1], utility_raygen_group_size[2] }
    };
}

gpu::GpuKernelDesc
shadeSurfaceDesc()
{
    return gpu::GpuKernelDesc {
        utility_shade_surface_spirv, utility_shade_surface_spirv_words, utility_shade_surface_entry, utility_shade_surface_storage_buffers, utility_shade_surface_push_bytes, { utility_shade_surface_group_size[0], utility_shade_surface_group_size[1], utility_shade_surface_group_size[2] }
    };
}

// The host-visible copies of what a batch's film reads, kept from frame to frame and grown when a batch
// outgrows them. Only the arrays the film reads exist.
struct BatchReadback {
    std::size_t capacity = 0;
    WavefrontAovs aovs { false, false, false, false };
    gpu::GpuBuffer hitGeometry;
    gpu::GpuBuffer color;
    gpu::GpuBuffer depth;
    gpu::GpuBuffer primId;
    gpu::GpuBuffer instanceId;
};

struct ShadeTableBuffers {
    std::uint64_t version = 0;
    bool valid = false;
    gpu::GpuBufferHandle geometries;
    gpu::GpuBufferHandle colors;
    gpu::GpuBufferHandle triangles;
    gpu::GpuBufferHandle instanceIds;
    gpu::GpuBufferHandle materials;
    gpu::GpuBufferHandle textureBindings;
    gpu::GpuBufferHandle images;
    gpu::GpuBufferHandle projections;
    gpu::GpuBufferHandle points;
    gpu::GpuBufferHandle instanceTransforms;
    gpu::GpuBufferHandle sts;
    // Uploaded only when the images change, not with the rest.
    std::uint64_t texelVersion = 0;
    gpu::GpuBufferHandle texels;
};

bool
sameAovs(const WavefrontAovs& a,
         const WavefrontAovs& b)
{
    return a.color == b.color && a.depth == b.depth && a.primId == b.primId && a.instanceId == b.instanceId;
}
} // anon namespace

struct WavefrontGpu::Implementation {
    Implementation(gpu::GpuKernelRegistry& registry,
                   gpu::GpuBufferPool& pool,
                   const std::shared_ptr<void>& runtimeHold)
        : registry(registry)
        , pool(pool)
        , device(registry.device())
        , tracer(registry, pool)
        , runtimeHold(runtimeHold)
    {
    }

    bool fail(const std::string& what,
              const gpu::GpuStatus& status)
    {
        outcome = Outcome { classify(status, device), what + ": " + status.message };

        return false;
    }

    bool stopped()
    {
        outcome = Outcome { Status::Stopped, "WavefrontGpu: the frame was stopped" };

        return false;
    }

    bool stage(VkDeviceSize bytes,
               const char* what,
               gpu::GpuBuffer& out)
    {
        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(device, std::max(bytes, kMinTableBytes), gpu::kGpuBufferDefaultUsage, out); !s) {
            return fail("WavefrontGpu could not hold the " + std::to_string(bytes) + " bytes of " + what, s);
        }

        return true;
    }

    // Records a copy of `data` into a device-local buffer from the pool, through a staging buffer the context
    // holds until its work is done.
    bool upload(gpu::GpuRecordContext& ctx,
                const void* data,
                std::size_t bytes,
                const char* what,
                gpu::GpuBufferHandle& out)
    {
        out = gpu::GpuBufferHandle();
        if (gpu::GpuStatus s = pool.acquire(std::max<VkDeviceSize>(bytes, kMinTableBytes), out); !s) {
            return fail("WavefrontGpu could not hold the " + std::to_string(bytes) + " bytes of " + what, s);
        }
        if (!bytes) {
            return true;
        }

        std::shared_ptr<gpu::GpuBuffer> staging = std::make_shared<gpu::GpuBuffer>();

        if (!stage(bytes, what, *staging)) {
            return false;
        }
        std::memcpy(staging->mapped(), data, bytes);
        if (gpu::GpuStatus s = ctx.copyBuffer(*staging, 0, out.buffer(), 0, bytes); !s) {
            return fail(std::string("WavefrontGpu could not upload its ") + what, s);
        }
        ctx.hold(std::move(staging));

        return true;
    }

    bool uploadTables(std::uint64_t version);
    bool reserveReadback(std::size_t rays,
                         const WavefrontAovs& aovs);
    bool renderBatch(const WavefrontFrame& frame,
                     const WavefrontRect& rect,
                     gpu::GpuRecordContext& ctx,
                     WavefrontFilm& film,
                     UtilityRenderBuffer* deviceColor);
    UtilityRenderBuffer* deviceColorOf(const WavefrontFilm& film);

    using Outcome = Tracer::Outcome;
    using Status = Tracer::Status;

    gpu::GpuKernelRegistry& registry;
    gpu::GpuBufferPool& pool;
    gpu::GpuDevice& device;
    GpuTracer tracer;
    std::shared_ptr<gpu::GpuKernel> rayGenKernel;
    std::shared_ptr<gpu::GpuKernel> shadeKernel;
    const std::shared_ptr<void> runtimeHold;
    std::shared_ptr<const UtilityFilmKernels> filmKernels;
    bool filmKernelsTried = false;
    bool usable = false;
    // Set while unusable.
    Outcome unusable;
    Outcome outcome;
    bool sceneFailed = false;
    WavefrontShadeTables shadeTables;
    ShadeTableBuffers tables;
    BatchReadback readback;
};

bool
WavefrontGpu::Implementation::uploadTables(std::uint64_t version)
{
    if (tables.valid && tables.version == version) {
        return true;
    }
    const std::uint64_t texelVersion = shadeTables.texelVersion();
    gpu::GpuBufferHandle keptTexels;

    if (tables.texels && tables.texelVersion == texelVersion) {
        keptTexels = std::move(tables.texels);
    }
    tables = ShadeTableBuffers();

    const KernelShadeBuffers source = shadeTables.buffers();
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    ShadeTableBuffers next;

    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(registry, ctx, &pool); !s) {
        return fail("WavefrontGpu could not create a record context", s);
    }
    if (!upload(*ctx, source.geometries, source.geometryCount * sizeof(KernelGeometryRecord), "geometry records", next.geometries)
        || !upload(*ctx, source.colors, source.colorCount * 4 * sizeof(float), "colours", next.colors)
        || !upload(*ctx, source.triangles, source.triangleIndexCount * sizeof(std::int32_t), "triangle indices", next.triangles)
        || !upload(*ctx, source.instanceIds, source.instanceCount * sizeof(std::int32_t), "instance ids", next.instanceIds)
        || !upload(*ctx, source.materials, source.materialCount * sizeof(KernelMaterialRecord), "material records", next.materials)
        || !upload(*ctx, source.textureBindings, source.textureBindingCount * sizeof(KernelTextureBinding), "texture bindings", next.textureBindings)
        || !upload(*ctx, source.images, source.imageCount * 4 * sizeof(std::uint32_t), "image records", next.images)
        || !upload(*ctx, source.projections, source.projectionCount * sizeof(KernelProjectionRecord), "projection records", next.projections)
        || !upload(*ctx, source.points, source.pointCount * 4 * sizeof(float), "points", next.points)
        || !upload(*ctx, source.instanceTransforms, source.instanceTransformRowCount * 4 * sizeof(float), "instance transforms", next.instanceTransforms)
        || !upload(*ctx, source.sts, source.stCount * 2 * sizeof(float), "texture coordinates", next.sts)) {
        return false;
    }
    if (keptTexels) {
        next.texels = std::move(keptTexels);
    } else if (!upload(*ctx, source.texels, source.texelCount * 4 * sizeof(float), "texels", next.texels)) {
        return false;
    }
    next.texelVersion = texelVersion;
    if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
        return fail("WavefrontGpu could not upload its shading tables", s);
    }
    tables = std::move(next);
    tables.version = version;
    tables.valid = true;

    return true;
}

bool
WavefrontGpu::Implementation::reserveReadback(std::size_t rays,
                                              const WavefrontAovs& aovs)
{
    if (readback.capacity >= rays && sameAovs(readback.aovs, aovs)) {
        return true;
    }
    readback = BatchReadback();
    if (!aovs.any()) {
        return true;
    }
    if (!stage(rays * sizeof(std::uint32_t), "hit geometry ids", readback.hitGeometry)
        || (aovs.color && !stage(rays * 4 * sizeof(float), "shaded colour", readback.color))
        || (aovs.depth && !stage(rays * sizeof(float), "depth", readback.depth))
        || (aovs.primId && !stage(rays * sizeof(std::int32_t), "prim ids", readback.primId))
        || (aovs.instanceId && !stage(rays * sizeof(std::int32_t), "instance ids", readback.instanceId))) {
        readback = BatchReadback();

        return false;
    }
    readback.capacity = rays;
    readback.aovs = aovs;

    return true;
}

UtilityRenderBuffer*
WavefrontGpu::Implementation::deviceColorOf(const WavefrontFilm& film)
{
    UtilityRenderBuffer* buffer = film.deviceColor();

    if (!buffer || !buffer->canRenderOnDevice()) {
        return nullptr;
    }
    if (!filmKernelsTried) {
        filmKernelsTried = true;

        gpu::GpuStatus status;

        filmKernels = makeUtilityFilmKernels(device, &status);
    }

    // Without the film's kernels the colour comes back in tiles, as every other AOV does.
    return filmKernels ? buffer : nullptr;
}

bool
WavefrontGpu::Implementation::renderBatch(const WavefrontFrame& frame,
                                          const WavefrontRect& rect,
                                          gpu::GpuRecordContext& ctx,
                                          WavefrontFilm& film,
                                          UtilityRenderBuffer* deviceColor)
{
    const std::size_t count = static_cast<std::size_t>(rect.width) * rect.height;
    const std::array<std::uint32_t, 3> threads = { static_cast<std::uint32_t>(count), 1, 1 };
    WavefrontAovs aovs = film.aovs();

    if (deviceColor) {
        aovs.color = false;
    }
    if (!reserveReadback(count, aovs)) {
        return false;
    }

    gpu::GpuBuffer* rays = nullptr;
    gpu::GpuBuffer* hitValues = nullptr;
    gpu::GpuBuffer* hitIds = nullptr;
    gpu::GpuBuffer* color = nullptr;
    gpu::GpuBuffer* depth = nullptr;
    gpu::GpuBuffer* primId = nullptr;
    gpu::GpuBuffer* instanceId = nullptr;
    const std::string rayCount = std::to_string(count) + " rays";

    if (gpu::GpuStatus s = ctx.scratch(kRayArrays * count * sizeof(float), rays); !s) {
        return fail("WavefrontGpu could not hold a batch of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(kHitArrays * count * sizeof(float), hitValues); !s) {
        return fail("WavefrontGpu could not hold the hits of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(kHitArrays * count * sizeof(std::uint32_t), hitIds); !s) {
        return fail("WavefrontGpu could not hold the hit ids of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(count * 4 * sizeof(float), color); !s) {
        return fail("WavefrontGpu could not hold the colours of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(count * sizeof(float), depth); !s) {
        return fail("WavefrontGpu could not hold the depths of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(count * sizeof(std::int32_t), primId); !s) {
        return fail("WavefrontGpu could not hold the prim ids of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(count * sizeof(std::int32_t), instanceId); !s) {
        return fail("WavefrontGpu could not hold the instance ids of " + rayCount, s);
    }

    const KernelRayGenParams rayGenParams = makeRayGenParams(frame, rect);
    const std::array<gpu::GpuBinding, 1> rayGenBindings = { gpu::GpuBinding::storage(0, *rays) };

    if (gpu::GpuStatus s = ctx.dispatchBindings(rayGenKernel, rayGenBindings, std::as_bytes(std::span(&rayGenParams, 1)), threads); !s) {
        return fail("WavefrontGpu could not generate rays", s);
    }
    ctx.computeBarrier();
    if (gpu::GpuStatus s = tracer.recordIntersect(ctx, *rays, static_cast<std::uint32_t>(count), *hitValues, *hitIds); !s) {
        return fail("WavefrontGpu could not intersect", s);
    }
    ctx.computeBarrier();

    const KernelShadeParams shadeParams = makeShadeParams(frame, static_cast<std::uint32_t>(count));
    const std::array<gpu::GpuBinding, 19> shadeBindings = {
        gpu::GpuBinding::storage(0, *rays),
        gpu::GpuBinding::storage(1, *hitValues),
        gpu::GpuBinding::storage(2, *hitIds),
        gpu::GpuBinding::storage(3, tables.geometries.buffer()),
        gpu::GpuBinding::storage(4, tables.colors.buffer()),
        gpu::GpuBinding::storage(5, tables.triangles.buffer()),
        gpu::GpuBinding::storage(6, tables.instanceIds.buffer()),
        gpu::GpuBinding::storage(7, *color),
        gpu::GpuBinding::storage(8, *depth),
        gpu::GpuBinding::storage(9, *primId),
        gpu::GpuBinding::storage(10, *instanceId),
        gpu::GpuBinding::storage(11, tables.materials.buffer()),
        gpu::GpuBinding::storage(12, tables.textureBindings.buffer()),
        gpu::GpuBinding::storage(13, tables.images.buffer()),
        gpu::GpuBinding::storage(14, tables.texels.buffer()),
        gpu::GpuBinding::storage(15, tables.projections.buffer()),
        gpu::GpuBinding::storage(16, tables.points.buffer()),
        gpu::GpuBinding::storage(17, tables.instanceTransforms.buffer()),
        gpu::GpuBinding::storage(18, tables.sts.buffer()),
    };

    if (gpu::GpuStatus s = ctx.dispatchBindings(shadeKernel, shadeBindings, std::as_bytes(std::span(&shadeParams, 1)), threads); !s) {
        return fail("WavefrontGpu could not shade", s);
    }
    if (deviceColor) {
        if (gpu::GpuStatus s = deviceColor->recordDeviceBatch(ctx, *hitIds, *color, rect.x, rect.y, rect.width, rect.height); !s) {
            return fail("WavefrontGpu could not write a batch's colours to the film", s);
        }
    }

    // The geometry array comes first in the hit ids, as Kernels/UtilityTypes.slang lays them out.
    const struct {
        bool wanted;
        const gpu::GpuBuffer* source;
        gpu::GpuBuffer* target;
        VkDeviceSize bytes;
    } copies[] = {
        { aovs.any(), hitIds, &readback.hitGeometry, count * sizeof(std::uint32_t) },
        { aovs.color, color, &readback.color, count * 4 * sizeof(float) },
        { aovs.depth, depth, &readback.depth, count * sizeof(float) },
        { aovs.primId, primId, &readback.primId, count * sizeof(std::int32_t) },
        { aovs.instanceId, instanceId, &readback.instanceId, count * sizeof(std::int32_t) },
    };

    for (const auto& copy : copies) {
        if (!copy.wanted) {
            continue;
        }
        if (gpu::GpuStatus s = ctx.copyBuffer(*copy.source, 0, *copy.target, 0, copy.bytes); !s) {
            return fail("WavefrontGpu could not read a batch back", s);
        }
    }
    if (gpu::GpuStatus s = ctx.submitAndWait(); !s) {
        return fail("WavefrontGpu could not render a batch of " + rayCount, s);
    }
    if (!aovs.any()) {
        return true;
    }

    WavefrontTile tile;

    tile.rect = rect;
    tile.hitGeometry = static_cast<const std::uint32_t*>(readback.hitGeometry.mapped());
    tile.color = aovs.color ? static_cast<const float*>(readback.color.mapped()) : nullptr;
    tile.depth = aovs.depth ? static_cast<const float*>(readback.depth.mapped()) : nullptr;
    tile.primId = aovs.primId ? static_cast<const std::int32_t*>(readback.primId.mapped()) : nullptr;
    tile.instanceId = aovs.instanceId ? static_cast<const std::int32_t*>(readback.instanceId.mapped()) : nullptr;
    film.writeTile(tile);

    return true;
}

WavefrontGpu::WavefrontGpu(gpu::GpuKernelRegistry& registry,
                           gpu::GpuBufferPool& pool,
                           const std::shared_ptr<void>& runtimeHold)
    : _imp(new Implementation(registry, pool, runtimeHold))
{
    Implementation& imp = *_imp;
    const Tracer::Outcome tracerStatus = imp.tracer.status();

    if (tracerStatus.status != Tracer::Status::Ok) {
        imp.unusable = tracerStatus;

        return;
    }

    std::unique_ptr<gpu::GpuKernel> kernel;

    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, rayGenDesc(), kernel); !s) {
        imp.unusable = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu could not create its ray generation kernel: " + s.message };

        return;
    }
    imp.rayGenKernel = std::move(kernel);
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, shadeSurfaceDesc(), kernel); !s) {
        imp.unusable = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu could not create its shading kernel: " + s.message };

        return;
    }
    imp.shadeKernel = std::move(kernel);
    imp.usable = true;
}

WavefrontGpu::~WavefrontGpu()
{
}

std::string
WavefrontGpu::missingSupport(const gpu::GpuRuntime& runtime)
{
    return GpuTracer::missingRayQuerySupport(runtime.device());
}

bool
WavefrontGpu::render(const UtilityScene::Snapshot& snapshot,
                     const WavefrontFrame& frame,
                     WavefrontFilm& film,
                     const std::atomic<bool>& stop,
                     int threads)
{
    Implementation& imp = *_imp;

    imp.outcome = Tracer::Outcome();
    imp.sceneFailed = false;
    if (!imp.usable) {
        imp.outcome = imp.unusable;

        return false;
    }
    if (!imp.tracer.update(snapshot, threads, &stop)) {
        imp.outcome = imp.tracer.status();
        if (imp.outcome.status == Tracer::Status::Ok) {
            return imp.stopped();
        }
        imp.sceneFailed = imp.outcome.status != Tracer::Status::Stopped;

        return false;
    }
    if (!imp.shadeTables.update(snapshot, &stop)) {
        return imp.stopped();
    }
    if (!imp.uploadTables(snapshot.version)) {
        return false;
    }

    const WavefrontRect& window = frame.window;
    UtilityRenderBuffer* deviceColor = imp.deviceColorOf(film);
    int row = 0;

    while (row < window.height) {
        if (stop.load(std::memory_order_relaxed)) {
            return imp.stopped();
        }

        std::unique_ptr<gpu::GpuRecordContext> ctx;

        if (gpu::GpuStatus s = gpu::GpuRecordContext::create(imp.registry, ctx, &imp.pool); !s) {
            return imp.fail("WavefrontGpu could not create a record context", s);
        }
        if (deviceColor && (row == 0)) {
            if (gpu::GpuStatus s = deviceColor->beginDeviceFrame(*ctx, imp.registry, imp.pool, imp.filmKernels, imp.runtimeHold); !s) {
                return imp.fail("WavefrontGpu could not hold the film on the device", s);
            }
            ctx->computeBarrier();
        }

        const std::uint64_t bindable = std::min(ctx->maxScratchBytes(), ctx->maxStorageBufferBytes()) / (kRayArrays * sizeof(float));
        // A row too wide for the latency cap still goes in one batch, so that any width renders.
        const std::uint64_t wanted = std::max(kMaxBatchRays, static_cast<std::uint64_t>(window.width));
        const std::uint64_t batchRays = std::min({ bindable, kUtilityMaxDispatchRays, wanted });
        const std::uint64_t rows = std::min<std::uint64_t>(batchRays / static_cast<std::uint64_t>(window.width), window.height - row);

        if (rows == 0) {
            imp.outcome = Tracer::Outcome { Tracer::Status::Failed, "WavefrontGpu: the device binds no storage buffer large enough for the rays of one row of " + std::to_string(window.width) + " pixels" };

            return false;
        }

        WavefrontRect rect;

        rect.x = window.x;
        rect.y = window.y + row;
        rect.width = window.width;
        rect.height = static_cast<int>(rows);
        if (!imp.renderBatch(frame, rect, *ctx, film, deviceColor)) {
            return false;
        }
        row += rect.height;
    }

    return true;
}

Tracer::Outcome
WavefrontGpu::status() const
{
    return _imp->outcome.status == Tracer::Status::Ok && !_imp->usable ? _imp->unusable : _imp->outcome;
}

bool
WavefrontGpu::sceneFailed() const
{
    return _imp->sceneFailed;
}

bool
WavefrontGpu::deviceLost() const
{
    return _imp->device.isLost();
}

std::string
WavefrontGpu::deviceName() const
{
    return _imp->device.info().name;
}

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT
