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
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"

#include "utility_raygen_spirv.h"
#include "utility_shade_flat_spirv.h"

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_ENTER

namespace {
std::atomic<gpu::GpuRuntime*> gTestRuntime { nullptr };

// The application's GPU starts on a thread of its own at launch; a frame asked for that early waits for it.
const std::chrono::seconds kGpuInitWait(10);
} // anon namespace

void
setUtilityGpuRuntimeForTesting(gpu::GpuRuntime* runtime)
{
    gTestRuntime.store(runtime);
}

UtilityGpu
acquireUtilityGpu()
{
    UtilityGpu result;

#ifdef NATRON_HAVE_GPU
    if (gpu::GpuRuntime* runtime = gTestRuntime.load()) {
        result.runtime = runtime;

        return result;
    }

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

#ifdef NATRON_HAVE_GPU

namespace {
static_assert(sizeof(KernelRayGenParams) == utility_raygen_push_bytes);
static_assert(sizeof(KernelShadeParams) == utility_shade_flat_push_bytes);
static_assert(utility_raygen_storage_buffers == 1 && utility_shade_flat_storage_buffers == 11);
static_assert(sizeof(KernelGeometryRecord) == 16);

const std::size_t kRayArrays = 8;
const std::size_t kHitArrays = 3;

// Vulkan guarantees 65535 workgroups along x, and the ray generation and shading kernels take a batch in
// one dispatch.
const std::uint64_t kMaxBatchRays = 65535ull * 64ull;

// Vulkan has no zero-sized buffer, and a scene without colours or triangles still binds all of them.
const VkDeviceSize kMinTableBytes = 16;

Tracer::Status
classify(const gpu::GpuStatus& status,
         const gpu::GpuDevice& device)
{
    if (device.isLost() || status.result == VK_ERROR_DEVICE_LOST) {
        return Tracer::Status::DeviceLost;
    }
    if (status.result == VK_ERROR_OUT_OF_DEVICE_MEMORY || status.result == VK_ERROR_OUT_OF_HOST_MEMORY) {
        return Tracer::Status::OutOfMemory;
    }
    if (status.result == VK_ERROR_FEATURE_NOT_PRESENT) {
        return Tracer::Status::Unsupported;
    }

    return Tracer::Status::Failed;
}

gpu::GpuKernelDesc
rayGenDesc()
{
    return gpu::GpuKernelDesc {
        utility_raygen_spirv, utility_raygen_spirv_words, utility_raygen_entry, utility_raygen_storage_buffers, utility_raygen_push_bytes, { utility_raygen_group_size[0], utility_raygen_group_size[1], utility_raygen_group_size[2] }
    };
}

gpu::GpuKernelDesc
shadeFlatDesc()
{
    return gpu::GpuKernelDesc {
        utility_shade_flat_spirv, utility_shade_flat_spirv_words, utility_shade_flat_entry, utility_shade_flat_storage_buffers, utility_shade_flat_push_bytes, { utility_shade_flat_group_size[0], utility_shade_flat_group_size[1], utility_shade_flat_group_size[2] }
    };
}

// The host-visible buffers of one batch, kept from frame to frame and grown when a batch outgrows them.
struct BatchOutputs {
    std::size_t capacity = 0;
    gpu::GpuBuffer hitIds;
    gpu::GpuBuffer color;
    gpu::GpuBuffer depth;
    gpu::GpuBuffer primId;
    gpu::GpuBuffer instanceId;
};

struct ShadeTableBuffers {
    std::uint64_t version = 0;
    bool valid = false;
    gpu::GpuBuffer geometries;
    gpu::GpuBuffer colors;
    gpu::GpuBuffer triangles;
    gpu::GpuBuffer instanceIds;
};
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

    bool upload(const void* data,
                std::size_t bytes,
                const char* what,
                gpu::GpuBuffer& out)
    {
        if (!stage(bytes, what, out)) {
            return false;
        }
        if (bytes) {
            std::memcpy(out.mapped(), data, bytes);
        }

        return true;
    }

    bool uploadTables(std::uint64_t version);
    bool reserveOutputs(std::size_t rays);
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
    WavefrontShadeTables shadeTables;
    ShadeTableBuffers tables;
    BatchOutputs outputs;
};

bool
WavefrontGpu::Implementation::uploadTables(std::uint64_t version)
{
    if (tables.valid && tables.version == version) {
        return true;
    }
    tables.valid = false;

    const KernelShadeBuffers source = shadeTables.buffers();

    if (!upload(source.geometries, source.geometryCount * sizeof(KernelGeometryRecord), "geometry records", tables.geometries)
        || !upload(source.colors, source.colorCount * 4 * sizeof(float), "colours", tables.colors)
        || !upload(source.triangles, source.triangleIndexCount * sizeof(std::int32_t), "triangle indices", tables.triangles)
        || !upload(source.instanceIds, source.instanceCount * sizeof(std::int32_t), "instance ids", tables.instanceIds)) {
        return false;
    }
    tables.version = version;
    tables.valid = true;

    return true;
}

bool
WavefrontGpu::Implementation::reserveOutputs(std::size_t rays)
{
    if (outputs.capacity >= rays) {
        return true;
    }
    outputs = BatchOutputs();
    if (!stage(kHitArrays * rays * sizeof(std::uint32_t), "hit ids", outputs.hitIds)
        || !stage(rays * 4 * sizeof(float), "shaded colour", outputs.color)
        || !stage(rays * sizeof(float), "depth", outputs.depth)
        || !stage(rays * sizeof(std::int32_t), "prim ids", outputs.primId)
        || !stage(rays * sizeof(std::int32_t), "instance ids", outputs.instanceId)) {
        outputs = BatchOutputs();

        return false;
    }
    outputs.capacity = rays;

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

    if (!reserveOutputs(count)) {
        return false;
    }

    gpu::GpuBuffer* rays = nullptr;
    gpu::GpuBuffer* hitValues = nullptr;
    gpu::GpuBuffer* color = &outputs.color;

    if (gpu::GpuStatus s = ctx.scratch(kRayArrays * count * sizeof(float), rays); !s) {
        return fail("WavefrontGpu could not hold a batch of " + std::to_string(count) + " rays", s);
    }
    if (gpu::GpuStatus s = ctx.scratch(kHitArrays * count * sizeof(float), hitValues); !s) {
        return fail("WavefrontGpu could not hold the hits of " + std::to_string(count) + " rays", s);
    }
    if (deviceColor) {
        if (gpu::GpuStatus s = ctx.scratch(count * 4 * sizeof(float), color); !s) {
            return fail("WavefrontGpu could not hold the colours of " + std::to_string(count) + " rays", s);
        }
    }

    const KernelRayGenParams rayGenParams = makeRayGenParams(frame, rect);
    const std::array<gpu::GpuBinding, 1> rayGenBindings = { gpu::GpuBinding::storage(0, *rays) };

    if (gpu::GpuStatus s = ctx.dispatchBindings(rayGenKernel, rayGenBindings, std::as_bytes(std::span(&rayGenParams, 1)), threads); !s) {
        return fail("WavefrontGpu could not generate rays", s);
    }
    ctx.computeBarrier();
    if (gpu::GpuStatus s = tracer.recordIntersect(ctx, *rays, static_cast<std::uint32_t>(count), *hitValues, outputs.hitIds); !s) {
        return fail("WavefrontGpu could not intersect", s);
    }
    ctx.computeBarrier();

    const KernelShadeParams shadeParams = makeShadeParams(frame, static_cast<std::uint32_t>(count));
    const std::array<gpu::GpuBinding, 11> shadeBindings = {
        gpu::GpuBinding::storage(0, *rays),
        gpu::GpuBinding::storage(1, *hitValues),
        gpu::GpuBinding::storage(2, outputs.hitIds),
        gpu::GpuBinding::storage(3, tables.geometries),
        gpu::GpuBinding::storage(4, tables.colors),
        gpu::GpuBinding::storage(5, tables.triangles),
        gpu::GpuBinding::storage(6, tables.instanceIds),
        gpu::GpuBinding::storage(7, *color),
        gpu::GpuBinding::storage(8, outputs.depth),
        gpu::GpuBinding::storage(9, outputs.primId),
        gpu::GpuBinding::storage(10, outputs.instanceId),
    };

    if (gpu::GpuStatus s = ctx.dispatchBindings(shadeKernel, shadeBindings, std::as_bytes(std::span(&shadeParams, 1)), threads); !s) {
        return fail("WavefrontGpu could not shade", s);
    }
    if (deviceColor) {
        if (gpu::GpuStatus s = deviceColor->recordDeviceBatch(ctx, outputs.hitIds, *color, rect.x, rect.y, rect.width, rect.height); !s) {
            return fail("WavefrontGpu could not write a batch's colours to the film", s);
        }
    }
    if (gpu::GpuStatus s = ctx.submitAndWait(); !s) {
        return fail("WavefrontGpu could not render a batch of " + std::to_string(count) + " rays", s);
    }

    WavefrontTile tile;

    tile.rect = rect;
    // The geometry array comes first in the hit ids, as Kernels/UtilityTypes.slang lays them out.
    tile.hitGeometry = static_cast<const std::uint32_t*>(outputs.hitIds.mapped());
    tile.color = deviceColor ? nullptr : static_cast<const float*>(outputs.color.mapped());
    tile.depth = static_cast<const float*>(outputs.depth.mapped());
    tile.primId = static_cast<const std::int32_t*>(outputs.primId.mapped());
    tile.instanceId = static_cast<const std::int32_t*>(outputs.instanceId.mapped());
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
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, shadeFlatDesc(), kernel); !s) {
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
    if (!imp.usable) {
        imp.outcome = imp.unusable;

        return false;
    }
    if (!imp.tracer.update(snapshot, threads, &stop)) {
        imp.outcome = imp.tracer.status();
        if (imp.outcome.status == Tracer::Status::Ok) {
            return imp.stopped();
        }

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
        const std::uint64_t batchRays = std::min(bindable, kMaxBatchRays);
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

std::string
WavefrontGpu::deviceName() const
{
    return _imp->device.info().name;
}

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT
