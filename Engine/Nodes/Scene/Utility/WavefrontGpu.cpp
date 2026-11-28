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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuPipelineCache.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuTexture.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityGpuLimits.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityProfile.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"

#include "utility_raster_hits_spirv.h"
#include "utility_raygen_spirv.h"
#include "utility_raygen_uv_spirv.h"
#include "utility_resolve_hits_spirv.h"
#include "utility_shade_lights_spirv.h"
#include "utility_shade_surface_spirv.h"

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_ENTER

namespace {
// The application's GPU starts on a thread of its own at launch; a frame asked for that early waits for it,
// looking at its cancel flag every kGpuInitPoll.
const std::chrono::milliseconds kGpuInitWait(10000);
const std::chrono::milliseconds kGpuInitPoll(20);
} // anon namespace

UtilityGpu
acquireUtilityGpu(const std::atomic<bool>* cancel)
{
    UtilityGpu result;

#ifdef NATRON_HAVE_GPU
    GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;

    if (!context) {
        result.unavailable = "GPU compute is turned off in the preferences";

        return result;
    }
    for (std::chrono::milliseconds waited(0); context->isInitializing() && waited < kGpuInitWait; waited += kGpuInitPoll) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.unavailable = "the frame was stopped while the GPU was starting";
            result.stopped = true;

            return result;
        }
        (void)context->waitForInit(kGpuInitPoll);
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
static_assert(sizeof(KernelRayGenParams) == utility_raygen_push_bytes && sizeof(KernelRayGenParams) == utility_raygen_uv_push_bytes);
static_assert(utility_raygen_uv_storage_buffers == 5);
static_assert(sizeof(KernelShadeParams) == utility_shade_surface_push_bytes);
static_assert(sizeof(KernelShadeLightsParams) == utility_shade_lights_push_bytes);
static_assert(sizeof(KernelResolveParams) == utility_resolve_hits_push_bytes);
static_assert(utility_raygen_storage_buffers == 3 && utility_shade_surface_storage_buffers == 20 && utility_shade_lights_storage_buffers == 3);
static_assert(utility_resolve_hits_storage_buffers == 15);
static_assert(sizeof(KernelRasterHitsParams) == utility_raster_hits_push_bytes && utility_raster_hits_storage_buffers == 5);
static_assert(sizeof(KernelPathState) == 32);
static_assert(sizeof(KernelLightRecord) == 144);
static_assert(sizeof(KernelGeometryRecord) == 56);
static_assert(sizeof(KernelMaterialRecord) == 144 && sizeof(KernelTextureBinding) == 112 && sizeof(KernelProjectionRecord) == 256);

const std::size_t kRayArrays = kUtilityRayArrays;
const std::size_t kHitArrays = kUtilityHitArrays;

// The fewest rays a batch takes while the device's speed is unknown, short enough on an integrated GPU or
// lavapipe that the device is not kept busy for long by work a stop has made useless.
const std::uint64_t kMinBatchRays = std::min<std::uint64_t>(kUtilityMaxDispatchRays, std::uint64_t(1) << 18);
// Lights sampled per shading pass. A full batch of rays then needs a few tens of megabytes of shadow
// rays per light, and more lights take more passes over the same hits rather than more memory.
const std::uint32_t kLightBatch = 4;
// The work, in kernel threads, one submission records beyond its first dispatch until the executor has timed
// its device: a stop that lands while the device runs it returns at once, but leaves the device busy with it.
// A batch of kMinBatchRays with one light batch and no holes takes one submission.
const std::uint64_t kMaxSubmitWork = 8 * kMinBatchRays;
// The most a submission's work is allowed to grow to on a fast device, which bounds the submissions that a
// scene far costlier per thread than the one the device was timed on makes.
const std::uint64_t kMaxLearnedSubmitWork = std::uint64_t(1) << 32;
// How long a submission is meant to keep the device busy, as the work it holds is learned. A frame that fits
// this goes in one submission; one that does not is split, so that the device leaves a stopped frame soon.
const std::uint64_t kSubmitTargetNs = 50'000'000ull;
// The same for an interactive frame, whose first pass is waited for and whose stop must take hold within a frame
// of the display. The device runs at most about this much of a frame's passes ahead of the host as well.
const std::uint64_t kInteractiveSubmitTargetNs = 16'000'000ull;
// How long a wait for a pass left running sleeps between looks at the stop flag.
const std::uint64_t kPassWaitSliceNs = 2'000'000ull;
// The timestamp pairs a profiled executor cycles through, far more than it has submissions in flight.
const std::uint32_t kTimestampPairs = 512;
const std::uint32_t kNoTimestamp = ~0u;
// The threads one ray of a batch costs at most in its largest dispatches, the camera rays and their hits
// then a full light batch of shadow rays, so that a batch's first submission stays near the learned work.
const std::uint64_t kWorkPerBatchRay = 2 + kLightBatch;
// As long as the compute submissions may take: a visibility pass draws the whole scene once per tile.
const std::uint64_t kRasterSubmitTimeoutNs = 30'000'000'000ull;
const std::uint64_t kSubmitTimeoutNs = 30'000'000'000ull;

// Vulkan has no zero-sized buffer, and a scene without colours or triangles still binds all of them.
const VkDeviceSize kMinTableBytes = 16;
// The texels, and any other table past this size, go up in submissions of at most this many bytes, the stop flag
// looked at between them, as a scene's textures and meshes can run to gigabytes.
const std::size_t kUploadChunkBytes = std::size_t(64) << 20;

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
rayGenUvDesc()
{
    return gpu::GpuKernelDesc {
        utility_raygen_uv_spirv, utility_raygen_uv_spirv_words, utility_raygen_uv_entry, utility_raygen_uv_storage_buffers, utility_raygen_uv_push_bytes, { utility_raygen_uv_group_size[0], utility_raygen_uv_group_size[1], utility_raygen_uv_group_size[2] }
    };
}

gpu::GpuKernelDesc
shadeSurfaceDesc()
{
    return gpu::GpuKernelDesc {
        utility_shade_surface_spirv, utility_shade_surface_spirv_words, utility_shade_surface_entry, utility_shade_surface_storage_buffers, utility_shade_surface_push_bytes, { utility_shade_surface_group_size[0], utility_shade_surface_group_size[1], utility_shade_surface_group_size[2] }
    };
}

gpu::GpuKernelDesc
shadeLightsDesc()
{
    return gpu::GpuKernelDesc {
        utility_shade_lights_spirv, utility_shade_lights_spirv_words, utility_shade_lights_entry, utility_shade_lights_storage_buffers, utility_shade_lights_push_bytes, { utility_shade_lights_group_size[0], utility_shade_lights_group_size[1], utility_shade_lights_group_size[2] }
    };
}

gpu::GpuKernelDesc
rasterHitsDesc()
{
    return gpu::GpuKernelDesc {
        utility_raster_hits_spirv, utility_raster_hits_spirv_words, utility_raster_hits_entry, utility_raster_hits_storage_buffers, utility_raster_hits_push_bytes, { utility_raster_hits_group_size[0], utility_raster_hits_group_size[1], utility_raster_hits_group_size[2] }
    };
}

// The rows of the row-vector matrix that takes OpenGL's clip space, which the frame's projection maps into, to
// Vulkan's over one tile of the window: depth from 0 to w rather than from -w to w, and x and y stretched so that
// the tile's pixels fill the attachment. Vulkan puts -1 in y at the attachment's first row, which is then the
// tile's bottom row, as the batch's arrays are ordered.
PXR_NS::GfMatrix4d
tileClip(const WavefrontRect& window,
         int tileX,
         int tileY,
         int tileWidth,
         int tileHeight)
{
    const double scaleX = static_cast<double>(window.width) / tileWidth;
    const double scaleY = static_cast<double>(window.height) / tileHeight;
    const double centreX = (2. * tileX + tileWidth) / window.width - 1.;
    const double centreY = (2. * tileY + tileHeight) / window.height - 1.;

    return PXR_NS::GfMatrix4d(scaleX, 0., 0., 0.,
                              0., scaleY, 0., 0.,
                              0., 0., 0.5, 0.,
                              -centreX * scaleX, -centreY * scaleY, 0.5, 1.);
}

void
storeAddress(VkDeviceAddress address,
             std::uint32_t& low,
             std::uint32_t& high)
{
    low = static_cast<std::uint32_t>(address & 0xffffffffull);
    high = static_cast<std::uint32_t>(address >> 32);
}

gpu::GpuKernelDesc
resolveHitsDesc()
{
    return gpu::GpuKernelDesc {
        utility_resolve_hits_spirv, utility_resolve_hits_spirv_words, utility_resolve_hits_entry, utility_resolve_hits_storage_buffers, utility_resolve_hits_push_bytes, { utility_resolve_hits_group_size[0], utility_resolve_hits_group_size[1], utility_resolve_hits_group_size[2] }
    };
}

// The host-visible copies of what a batch's film reads, kept from frame to frame and grown when a batch
// outgrows them. Only the arrays the film reads exist.
struct BatchReadback {
    std::size_t capacity = 0;
    WavefrontAovs aovs { false, false, false, false };
    gpu::GpuBuffer hitGeometry;
    gpu::GpuBuffer weight;
    gpu::GpuBuffer color;
    gpu::GpuBuffer depth;
    gpu::GpuBuffer primId;
    gpu::GpuBuffer instanceId;
};

constexpr std::size_t kShadeTableCount = 17;

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
    gpu::GpuBufferHandle normals;
    gpu::GpuBufferHandle lights;
    gpu::GpuBufferHandle lightData;
    gpu::GpuBufferHandle linkMasks;
    gpu::GpuBufferHandle instanceMotion;
    gpu::GpuBufferHandle motionRows;
    // The device addresses of the tables at the KernelTableSlot slots, which ShadeSurface and ResolveHits
    // read through them rather than through bindings the storage buffer range would bound.
    gpu::GpuBufferHandle addresses;
    // Uploaded only when the images change, not with the rest.
    std::uint64_t texelVersion = 0;
    gpu::GpuBufferHandle texels;
    // What each of the other tables held, in the order uploadTables() puts them, so one that did not change is
    // kept rather than uploaded again.
    std::array<std::uint64_t, kShadeTableCount> keys {};
};

std::uint64_t
contentKey(const void* data,
           std::size_t bytes)
{
    std::uint64_t key = 0x9E3779B97F4A7C15ULL ^ bytes;
    const unsigned char* at = static_cast<const unsigned char*>(data);
    const auto mix = [&key](std::uint64_t word) {
        key = (key ^ word) * 0xFF51AFD7ED558CCDULL;
        key ^= key >> 32;
    };

    for (std::size_t i = 0; i + sizeof(std::uint64_t) <= bytes; i += sizeof(std::uint64_t)) {
        std::uint64_t word;

        std::memcpy(&word, at + i, sizeof(word));
        mix(word);
    }

    const std::size_t tail = bytes % sizeof(std::uint64_t);

    if (tail) {
        std::uint64_t word = 0;

        std::memcpy(&word, at + (bytes - tail), tail);
        mix(word);
    }

    return key;
}

bool
sameAovs(const WavefrontAovs& a,
         const WavefrontAovs& b)
{
    return a.color == b.color && a.depth == b.depth && a.primId == b.primId && a.instanceId == b.instanceId;
}

// How often a render waiting for the kernels to compile looks at its stop flag.
const std::chrono::milliseconds kWarmUpPoll(20);

struct WarmUp {
    std::mutex mutex;
    std::condition_variable cond;
    bool finished = false;
    gpu::GpuStatus status;
    // Set when the executor goes away, so that the kernels not yet compiled are skipped.
    std::atomic<bool> abandon { false };
};

/**
 * @brief Runs one group of each kernel, one submission each, so that the driver has compiled every pipeline
 * before a frame's batches run under submitAndWait()'s time limit: a software device compiles a pipeline at its
 * first dispatch, which on a loaded machine has taken longer than that limit. Every Utility kernel returns at
 * once for a thread past the count its zeroed push constants give, so the dispatches touch no buffer. Runs on
 * a thread of its own because a record context must be destroyed on the thread that made it, and a render
 * stopped meanwhile must not wait for it.
 **/
void
warmKernels(gpu::GpuKernelRegistry& registry,
            const std::vector<std::shared_ptr<gpu::GpuKernel>>& kernels,
            const std::shared_ptr<WarmUp>& state)
{
    gpu::GpuBuffer dummy;
    gpu::GpuStatus status = gpu::GpuBuffer::createDeviceLocal(registry.device(), kMinTableBytes, gpu::kGpuBufferDefaultUsage, dummy);

    for (const std::shared_ptr<gpu::GpuKernel>& kernel : kernels) {
        if (!status || state->abandon.load(std::memory_order_relaxed)) {
            break;
        }

        std::unique_ptr<gpu::GpuRecordContext> ctx;

        status = gpu::GpuRecordContext::create(registry, ctx);
        if (!status) {
            break;
        }

        std::vector<gpu::GpuBinding> bindings;

        for (const gpu::GpuKernelBinding& binding : kernel->bindings()) {
            bindings.push_back(gpu::GpuBinding::storage(binding.binding, dummy));
        }

        const std::vector<std::byte> push(kernel->desc().pushConstantBytes, std::byte(0));

        status = ctx->dispatchBindings(kernel, bindings, push, { 1, 1, 1 });
        if (status) {
            // The work is one empty group per kernel, so only the compilation can make it long, never a hang.
            status = ctx->submitAndWait(std::numeric_limits<std::uint64_t>::max());
        }
    }
    {
        const std::lock_guard<std::mutex> l(state->mutex);

        state->status = status;
        state->finished = true;
    }
    state->cond.notify_all();
}

// How long a GPU context going away waits for the warm-ups still compiling on its device.
const std::chrono::seconds kWarmUpTeardownWait(2);

// The warm-up threads still running, by device. Never destroyed, as a detached thread may end during
// static destruction.
struct WarmUpThreads {
    std::mutex mutex;
    std::condition_variable ended;
    std::map<const gpu::GpuDevice*, int> running;
};

WarmUpThreads&
warmUpThreads()
{
    static WarmUpThreads* threads = new WarmUpThreads;

    return *threads;
}

/**
 * @brief The warm-up thread: the executor detaches it when it goes first, so the thread keeps the kernels and
 * the runtime hold itself, and lets go of them before it counts itself out, after which the device may go.
 **/
void
runWarmUp(gpu::GpuKernelRegistry* registry,
          std::vector<std::shared_ptr<gpu::GpuKernel>> kernels,
          std::shared_ptr<WarmUp> state,
          std::shared_ptr<void> runtimeHold)
{
    const gpu::GpuDevice* device = &registry->device();

    warmKernels(*registry, kernels, state);
    kernels.clear();
    state.reset();
    runtimeHold.reset();

    WarmUpThreads& threads = warmUpThreads();

    {
        const std::lock_guard<std::mutex> l(threads.mutex);

        --threads.running[device];
    }
    threads.ended.notify_all();
}

std::thread
startWarmUp(gpu::GpuKernelRegistry& registry,
            std::vector<std::shared_ptr<gpu::GpuKernel>> kernels,
            const std::shared_ptr<WarmUp>& state,
            const std::shared_ptr<void>& runtimeHold)
{
    static std::once_flag registered;

    std::call_once(registered, []() {
        GpuContext::addTeardownWaiter([](const gpu::GpuRuntime& runtime) { return WavefrontGpu::waitForWarmUps(runtime.device(), kWarmUpTeardownWait); });
    });

    WarmUpThreads& threads = warmUpThreads();
    const gpu::GpuDevice* device = &registry.device();

    {
        const std::lock_guard<std::mutex> l(threads.mutex);

        ++threads.running[device];
    }
    try {
        return std::thread(runWarmUp, &registry, std::move(kernels), state, runtimeHold);
    } catch (...) {
        {
            const std::lock_guard<std::mutex> l(threads.mutex);

            --threads.running[device];
        }
        threads.ended.notify_all();
        throw;
    }
}
} // anon namespace

namespace {
/**
 * @brief A visibility pass submitted to signal its semaphore, which the compute work using its hits waits for on
 * the device. Held by that compute work's context, so it goes once that work is done, or, when the work is never
 * submitted, once the pass is: its command buffers and the compute scratch it writes are not freed under it.
 **/
struct PendingRaster {
    explicit PendingRaster(gpu::GpuDevice& device)
        : device(device)
    {
    }

    ~PendingRaster()
    {
        if (signalled && !device.waitSemaphore(semaphore, 1, kRasterSubmitTimeoutNs)) {
            // Freeing work the device may still run is invalid: a lost or hung device keeps it.
            (void)ctx.release();

            return;
        }
        ctx.reset();
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device.device(), semaphore, nullptr);
        }
    }

    PendingRaster(const PendingRaster&) = delete;
    PendingRaster& operator=(const PendingRaster&) = delete;

    gpu::GpuDevice& device;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool signalled = false;
    std::unique_ptr<gpu::GpuRecordContext> ctx;
};

// The timeline the passes left running signal, one value each, in submission order.
struct PassTimeline {
    explicit PassTimeline(gpu::GpuDevice& device)
        : device(device)
    {
    }

    ~PassTimeline()
    {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device.device(), semaphore, nullptr);
        }
    }

    PassTimeline(const PassTimeline&) = delete;
    PassTimeline& operator=(const PassTimeline&) = delete;

    std::uint64_t reached() const
    {
        std::uint64_t value = 0;

        if (vkGetSemaphoreCounterValue(device.device(), semaphore, &value) != VK_SUCCESS) {
            return 0;
        }

        return value;
    }

    gpu::GpuDevice& device;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::uint64_t submitted = 0;
};

/**
 * @brief A batch's last submission, left running when the film is not to be shown its pass yet, with every
 * context of the batch: the earlier ones hold scratch buffers it binds. It goes once the device is done with it;
 * on a device that never finishes it, its contexts and the timeline are left to the device.
 **/
struct DeferredPass {
    DeferredPass() = default;

    ~DeferredPass()
    {
        if (!signalled) {
            return;
        }
        if (!timeline->device.waitSemaphore(timeline->semaphore, value, kSubmitTimeoutNs)) {
            for (std::unique_ptr<gpu::GpuRecordContext>& ctx : contexts) {
                (void)ctx.release();
            }
            (void)new std::shared_ptr<PassTimeline>(timeline);
        }
    }

    DeferredPass(const DeferredPass&) = delete;
    DeferredPass& operator=(const DeferredPass&) = delete;

    std::shared_ptr<PassTimeline> timeline;
    std::uint64_t value = 0;
    bool signalled = false;
    std::vector<std::unique_ptr<gpu::GpuRecordContext>> contexts;
    // The work it holds and its estimated device time, and whether nothing else was running ahead of it, which
    // makes the time to its end that of the work alone.
    std::uint64_t work = 0;
    std::uint64_t estimatedNs = 0;
    bool alone = false;
    std::chrono::steady_clock::time_point submittedAt;
    std::uint32_t timestamp = kNoTimestamp;
};
} // anon namespace

struct WavefrontGpu::Implementation {
    Implementation(gpu::GpuKernelRegistry& registry,
                   gpu::GpuBufferPool& pool,
                   const std::shared_ptr<void>& runtimeHold,
                   gpu::GpuPipelineCache* pipelines)
        : registry(registry)
        , pool(pool)
        , device(registry.device())
        , pipelines(pipelines)
        , tracer(registry, pool)
        , runtimeHold(runtimeHold)
    {
    }

    // A warm-up still compiling is left to end on its own: the GPU context waits for it before it destroys the
    // device.
    ~Implementation()
    {
        deferred.clear();
        if (timestamps != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device.device(), timestamps, nullptr);
        }
        if (warmUpThread.joinable()) {
            warmUp->abandon.store(true, std::memory_order_relaxed);
            warmUpThread.detach();
        }
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

    // The table needs VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, which the pool's buffers have on a device with
    // ray queries, as this executor's must.
    KernelTableAddress tableAddress(const gpu::GpuBufferHandle& table) const
    {
        return kernelTableAddress(table.buffer().deviceAddress(device));
    }

    /**
     * @brief Records a batch's work into as many submissions as its work asks for, each submitted and waited
     * for before the next is recorded, the stop flag looked at between them and during each wait, but for a last
     * one that defer() leaves running. The
     * contexts submitted are kept until the recorder goes away, as they hold the scratch buffers later
     * submissions bind. A stop during a wait leaves that submission to finish on the device on its own.
     **/
    class Recorder {
    public:
        Recorder(Implementation& imp,
                 const std::atomic<bool>& stop,
                 std::uint64_t work)
            : _imp(imp)
            , _stop(stop)
            , _work(work)
        {
        }

        bool begin();
        gpu::GpuRecordContext& ctx() { return *_current; }
        // Makes room for `work` more kernel threads, submitting what is recorded first when it would take the
        // submission past the work the recorder was given. False when the frame was stopped or a submission failed.
        bool reserve(std::uint64_t work);
        // The work a submission is given, which the film's clears and copies split by as well.
        std::uint64_t work() const { return _work; }
        // Submits what is recorded and waits for it, so that the host may read what it wrote, then goes on
        // recording. False when the frame was stopped or the submission failed.
        bool sync() { return next(); }
        bool finish(const std::string& what);
        // Submits what is recorded without waiting for it, handing the batch's contexts to the passes left
        // running. False when the frame was stopped or the submission failed.
        bool defer(const std::string& what);
        const std::atomic<bool>& stop() const { return _stop; }

    private:
        bool next();
        bool submit(const std::string& what);

        Implementation& _imp;
        const std::atomic<bool>& _stop;
        const std::uint64_t _work;
        std::uint32_t _timestamp = kNoTimestamp;
        std::vector<std::unique_ptr<gpu::GpuRecordContext>> _submitted;
        std::unique_ptr<gpu::GpuRecordContext> _current;
        std::uint64_t _recorded = 0;
    };

    bool warm(const std::atomic<bool>& stop);
    bool uploadTables(std::uint64_t version,
                      const std::atomic<bool>& stop);
    bool uploadChunked(const void* data,
                       std::size_t bytes,
                       const char* what,
                       gpu::GpuBufferHandle& out,
                       const std::atomic<bool>& stop);
    bool uploadUnwrap(const WavefrontUnwrapConstPtr& unwrap);
    bool uploadCameraSteps(const std::vector<float>& steps);
    bool reserveReadback(std::size_t rays,
                         const WavefrontAovs& aovs);
    struct ResolveScratch {
        gpu::GpuBuffer* retrace = nullptr;
        gpu::GpuBuffer* nextHitValues = nullptr;
        gpu::GpuBuffer* nextHitIds = nullptr;
        // One word per pass, ResolveHits.slang's pending.
        gpu::GpuBuffer* pending = nullptr;
    };

    bool reserveResolve(gpu::GpuRecordContext& ctx,
                        std::size_t count,
                        std::uint32_t maxCutouts,
                        ResolveScratch& out);
    bool reserveFlagReadback(std::size_t words);
    // Reads words of flags from first on, through flagReadback, which out then points into until the next read.
    bool readFlags(Recorder& rec,
                   const gpu::GpuBuffer& flags,
                   std::size_t first,
                   std::size_t words,
                   const std::uint32_t*& out);
    bool recordResolve(Recorder& rec,
                       const ResolveScratch& scratch,
                       KernelResolveParams params,
                       std::size_t count,
                       std::uint32_t maxCutouts,
                       const gpu::GpuBuffer& rays,
                       const gpu::GpuBuffer& hitValues,
                       const gpu::GpuBuffer& hitIds,
                       const gpu::GpuBuffer& blocked,
                       const gpu::GpuBuffer& rayLights,
                       const gpu::GpuBuffer& shadowContributions);
    bool rasterHits(const WavefrontFrame& frame,
                    const WavefrontRect& rect,
                    Recorder& rec,
                    gpu::GpuBuffer& rays,
                    gpu::GpuBuffer& weights,
                    gpu::GpuBuffer& hitValues,
                    gpu::GpuBuffer& hitIds,
                    bool cutouts);
    bool renderBatch(const WavefrontFrame& frame,
                     const WavefrontRect& rect,
                     Recorder& rec,
                     WavefrontFilm& film,
                     UtilityRenderBuffer* deviceColor,
                     UtilityDeviceAovs* deviceAovs,
                     std::uint32_t& deepest);
    void learnSubmitWork(std::uint64_t work,
                         std::uint64_t ns);
    std::uint64_t submitWorkFor(const WavefrontFrame& frame) const;
    std::uint64_t estimatedNs(std::uint64_t work) const;
    // Drops the passes left running that the device has finished, learning from them.
    void retirePasses();
    bool waitPass(DeferredPass& pass,
                  const std::atomic<bool>* stop);
    // Waits for every pass left running; with no stop flag, as long as the device takes.
    bool finishPasses(const std::atomic<bool>* stop);
    // Before anything the passes left running read is replaced.
    bool settlePasses() { return deferred.empty() || finishPasses(nullptr); }
    // The pair of timestamps a profiled submission writes at its start and end; kNoTimestamp when not profiled.
    std::uint32_t takeTimestamp();
    // Adds the device time between a pair's timestamps to the profile, once the device has written them.
    void readTimestamp(std::uint32_t pair);
    // Keeps the device no further ahead of the host than one submission of the frame's.
    bool throttlePasses(const WavefrontFrame& frame,
                        const std::atomic<bool>& stop);
    std::uint64_t batchBytesPerRay(const WavefrontFrame& frame) const;
    std::uint64_t batchRaysFor(const WavefrontFrame& frame) const;
    bool haveFilmKernels();
    UtilityRenderBuffer* deviceColorOf(const WavefrontFilm& film);
    UtilityDeviceAovs* deviceAovsOf(const WavefrontFilm& film,
                                    const gpu::GpuRecordContext& ctx,
                                    const WavefrontRect& window);

    using Outcome = Tracer::Outcome;
    using Status = Tracer::Status;

    gpu::GpuKernelRegistry& registry;
    gpu::GpuBufferPool& pool;
    gpu::GpuDevice& device;
    gpu::GpuPipelineCache* pipelines;
    GpuTracer tracer;
    std::shared_ptr<gpu::GpuKernel> rayGenKernel;
    std::shared_ptr<gpu::GpuKernel> rayGenUvKernel;
    std::shared_ptr<gpu::GpuKernel> shadeKernel;
    std::shared_ptr<gpu::GpuKernel> shadeLightsKernel;
    std::shared_ptr<gpu::GpuKernel> resolveKernel;
    // Null when the device cannot draw visibility buffers.
    std::shared_ptr<gpu::GpuKernel> rasterHitsKernel;
    // The visibility buffer's attachments, kept while the tiles keep their size.
    std::unique_ptr<gpu::GpuTexture> visibilityColour;
    std::unique_ptr<gpu::GpuTexture> visibilityDepth;
    std::uint32_t rasterTileLimit = 0;
    bool alwaysTraceRaster = false;
    // Whether the frame render() draws is a Raster one whose pixels its camera hits decide alone.
    bool untraced = false;
    const std::shared_ptr<void> runtimeHold;
    std::shared_ptr<const UtilityFilmKernels> filmKernels;
    bool filmKernelsTried = false;
    std::shared_ptr<WarmUp> warmUp;
    std::thread warmUpThread;
    bool warmed = false;
    bool usable = false;
    // Set while unusable.
    Outcome unusable;
    Outcome outcome;
    bool sceneFailed = false;
    WavefrontShadeTables shadeTables;
    ShadeTableBuffers tables;
    BatchReadback readback;
    // The host-visible copy of the flags that tell whether rays go on, kept from frame to frame.
    gpu::GpuBuffer flagReadback;
    std::size_t flagReadbackWords = 0;
    // The UV unwrap on the device, kept while the frames draw the same one.
    WavefrontUnwrapConstPtr unwrap;
    gpu::GpuBufferHandle unwrapGrid;
    gpu::GpuBufferHandle unwrapTriangles;
    // The device addresses of the grid then the triangles, which RayGenUv reads through them.
    gpu::GpuBufferHandle unwrapAddresses;
    // RayGen's cameraSteps, kept while the frames draw the same camera.
    std::vector<float> cameraSteps;
    gpu::GpuBufferHandle cameraStepsBuffer;
    // The kernel threads a submission holds, learned from how long the frames' submissions take on this device.
    std::uint64_t submitWork = kMaxSubmitWork;
    std::size_t submissions = 0;
    // Since the executor was made: the shading tables uploaded, and the times a new scene found none changed.
    std::size_t shadeTablesUploaded = 0;
    std::size_t shadeTableSetsKept = 0;
    std::shared_ptr<PassTimeline> passTimeline;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    bool timestampsTried = false;
    std::uint32_t nextTimestamp = 0;
    double timestampPeriodNs = 1.;
    std::uint64_t timestampMask = 0;
    // The scene version the passes left running draw, whose structures stay until they are done.
    std::uint64_t deferredVersion = 0;
    bool deferredTraces = false;
    // Last, so that the passes left running are waited for before anything they read goes.
    std::deque<std::unique_ptr<DeferredPass>> deferred;
};

/**
 * @brief Compiles every pipeline the executor dispatches, once, before its first frame: warmKernels() tells
 * why. A stop returns at once and leaves the compilation running, for the next render() to wait for.
 **/
bool
WavefrontGpu::Implementation::warm(const std::atomic<bool>& stop)
{
    if (warmed) {
        return true;
    }
    if (!warmUp) {
        std::vector<std::shared_ptr<gpu::GpuKernel>> kernels = tracer.kernels();

        kernels.insert(kernels.end(), { rayGenKernel, rayGenUvKernel, shadeKernel, shadeLightsKernel, resolveKernel });
        if (rasterHitsKernel) {
            kernels.push_back(rasterHitsKernel);
        }
        if (filmKernels) {
            const std::vector<std::shared_ptr<gpu::GpuKernel>> film = utilityFilmKernelList(*filmKernels);

            kernels.insert(kernels.end(), film.begin(), film.end());
        }

        std::shared_ptr<WarmUp> state = std::make_shared<WarmUp>();

        try {
            warmUpThread = startWarmUp(registry, std::move(kernels), state, runtimeHold);
        } catch (const std::system_error& e) {
            outcome = Outcome { Status::Failed, std::string("WavefrontGpu could not start compiling its kernels: ") + e.what() };

            return false;
        }
        warmUp = std::move(state);
    }

    gpu::GpuStatus status;

    {
        std::unique_lock<std::mutex> l(warmUp->mutex);

        while (!warmUp->finished) {
            if (stop.load(std::memory_order_relaxed)) {
                return stopped();
            }
            warmUp->cond.wait_for(l, kWarmUpPoll);
        }
        status = warmUp->status;
    }
    if (warmUpThread.joinable()) {
        warmUpThread.join();
    }
    if (!status) {
        return fail("WavefrontGpu could not compile its kernels", status);
    }
    warmed = true;

    return true;
}

bool
WavefrontGpu::Implementation::uploadChunked(const void* data,
                                            std::size_t bytes,
                                            const char* what,
                                            gpu::GpuBufferHandle& out,
                                            const std::atomic<bool>& stop)
{
    gpu::GpuBufferHandle buffer;

    out = gpu::GpuBufferHandle();
    if (gpu::GpuStatus s = pool.acquire(std::max<VkDeviceSize>(bytes, kMinTableBytes), buffer); !s) {
        return fail("WavefrontGpu could not hold the " + std::to_string(bytes) + " bytes of " + what, s);
    }
    for (std::size_t first = 0; first < bytes; first += kUploadChunkBytes) {
        if (stop.load(std::memory_order_relaxed)) {
            return stopped();
        }

        const std::size_t chunk = std::min(kUploadChunkBytes, bytes - first);
        std::unique_ptr<gpu::GpuRecordContext> ctx;
        std::shared_ptr<gpu::GpuBuffer> staging = std::make_shared<gpu::GpuBuffer>();

        if (gpu::GpuStatus s = gpu::GpuRecordContext::create(registry, ctx, &pool); !s) {
            return fail("WavefrontGpu could not create a record context", s);
        }
        if (!stage(chunk, what, *staging)) {
            return false;
        }
        std::memcpy(staging->mapped(), static_cast<const unsigned char*>(data) + first, chunk);
        if (gpu::GpuStatus s = ctx->copyBuffer(*staging, 0, buffer.buffer(), first, chunk); !s) {
            return fail(std::string("WavefrontGpu could not upload its ") + what, s);
        }
        ctx->hold(std::move(staging));
        if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
            return fail(std::string("WavefrontGpu could not upload its ") + what, s);
        }
    }
    out = std::move(buffer);

    return true;
}

bool
WavefrontGpu::Implementation::uploadTables(std::uint64_t version,
                                           const std::atomic<bool>& stop)
{
    if (tables.valid && tables.version == version) {
        return true;
    }
    if (stop.load(std::memory_order_relaxed)) {
        return stopped();
    }

    const KernelShadeBuffers source = shadeTables.buffers();
    const std::uint64_t texelVersion = shadeTables.texelVersion();
    const std::uint64_t geometryData = shadeTables.geometryDataVersion();
    // The per-geometry arrays are large and change only with the version that says so; the rest are hashed.
    const auto geometryKey = [geometryData](std::size_t bytes) { return contentKey(&geometryData, sizeof(geometryData)) ^ bytes; };
    const std::array<std::uint64_t, kShadeTableCount> keys = {
        contentKey(source.geometries, source.geometryCount * sizeof(KernelGeometryRecord)),
        geometryKey(source.colorCount * 4 * sizeof(float)),
        geometryKey(source.triangleIndexCount * sizeof(std::int32_t)),
        contentKey(source.instanceIds, source.instanceCount * sizeof(std::int32_t)),
        contentKey(source.materials, source.materialCount * sizeof(KernelMaterialRecord)),
        contentKey(source.textureBindings, source.textureBindingCount * sizeof(KernelTextureBinding)),
        contentKey(source.images, source.imageCount * 4 * sizeof(std::uint32_t)),
        contentKey(source.projections, source.projectionCount * sizeof(KernelProjectionRecord)),
        geometryKey(source.pointCount * 4 * sizeof(float)),
        contentKey(source.instanceTransforms, source.instanceTransformRowCount * 4 * sizeof(float)),
        geometryKey(source.stCount * 2 * sizeof(float)),
        geometryKey(source.normalCount * 4 * sizeof(float)),
        contentKey(source.lights, source.lightCount * sizeof(KernelLightRecord)),
        contentKey(source.lightData, source.lightDataCount * sizeof(float)),
        contentKey(source.linkMasks, source.linkMaskCount * sizeof(std::uint32_t)),
        contentKey(source.instanceMotion, source.instanceMotionCount * sizeof(std::uint32_t)),
        contentKey(source.motionRows, source.motionRowCount * 4 * sizeof(float)),
    };

    // Nothing the passes left running read is replaced, so they need not be waited for.
    if (tables.valid && tables.texels && tables.texelVersion == texelVersion && tables.keys == keys) {
        tables.version = version;
        ++shadeTableSetsKept;
        utilityProfile::count(utilityProfile::Counter::ShadeTableSetsKept);

        return true;
    }
    if (!settlePasses()) {
        return false;
    }

    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(registry, ctx, &pool); !s) {
        return fail("WavefrontGpu could not create a record context", s);
    }
    // The largest buffer the pool hands out that a binding still takes whole.
    const std::uint64_t bindLimit = ctx->maxScratchBytes() ? ctx->maxScratchBytes() : ctx->maxStorageBufferBytes();
    gpu::GpuBufferHandle keptTexels;

    if (tables.texels && tables.texelVersion == texelVersion) {
        keptTexels = std::move(tables.texels);
    }

    ShadeTableBuffers previous = tables.valid ? std::move(tables) : ShadeTableBuffers();

    tables = ShadeTableBuffers();

    gpu::GpuBufferHandle* const previousHandles[kShadeTableCount] = {
        &previous.geometries,
        &previous.colors,
        &previous.triangles,
        &previous.instanceIds,
        &previous.materials,
        &previous.textureBindings,
        &previous.images,
        &previous.projections,
        &previous.points,
        &previous.instanceTransforms,
        &previous.sts,
        &previous.normals,
        &previous.lights,
        &previous.lightData,
        &previous.linkMasks,
        &previous.instanceMotion,
        &previous.motionRows,
    };

    // The tables the kernels bind rather than read through their addresses. They grow with the materials,
    // textures and lights rather than with the geometry, and one over the range sends the scene to the CPU.
    const std::pair<std::size_t, const char*> bound[] = {
        { source.materialCount * sizeof(KernelMaterialRecord), "material records" },
        { source.textureBindingCount * sizeof(KernelTextureBinding), "texture bindings" },
        { source.imageCount * 4 * sizeof(std::uint32_t), "image records" },
        { source.projectionCount * sizeof(KernelProjectionRecord), "projection records" },
        { source.lightCount * sizeof(KernelLightRecord), "light records" },
    };

    for (const std::pair<std::size_t, const char*>& table : bound) {
        if (table.first > bindLimit) {
            outcome = Outcome { Status::OverStorageRange, "WavefrontGpu: the " + std::to_string(table.first) + " bytes of " + table.second + " exceed the GPU's " + std::to_string(ctx->maxStorageBufferBytes()) + "-byte storage buffer range" };

            return false;
        }
    }

    ShadeTableBuffers next;

    // Uploaded in submissions of its own, before the address table that holds its address.
    if (keptTexels) {
        next.texels = std::move(keptTexels);
    } else if (!uploadChunked(source.texels, source.texelCount * 4 * sizeof(float), "texels", next.texels, stop)) {
        return false;
    }
    next.texelVersion = texelVersion;
    // A table past one chunk goes up in submissions of its own, the stop flag looked at between them, so that a
    // stop never waits for a whole mesh's worth of copies; the rest share one submission.
    bool stoppedHere = false;
    std::size_t slot = 0;
    const auto put = [&](const void* data, std::size_t bytes, const char* what, gpu::GpuBufferHandle& out) {
        if (stop.load(std::memory_order_relaxed)) {
            stoppedHere = true;

            return false;
        }

        const std::size_t table = slot++;
        // The buffer the table replaces goes back to the pool before its successor is asked for.
        gpu::GpuBufferHandle kept = std::move(*previousHandles[table]);

        if (kept && previous.keys[table] == keys[table]) {
            out = std::move(kept);

            return true;
        }
        kept = gpu::GpuBufferHandle();
        ++shadeTablesUploaded;
        utilityProfile::count(utilityProfile::Counter::ShadeTablesUploaded);
        if (bytes > kUploadChunkBytes) {
            if (!uploadChunked(data, bytes, what, out, stop)) {
                stoppedHere = outcome.status == Status::Stopped;

                return false;
            }

            return true;
        }

        return upload(*ctx, data, bytes, what, out);
    };

    if (!put(source.geometries, source.geometryCount * sizeof(KernelGeometryRecord), "geometry records", next.geometries)
        || !put(source.colors, source.colorCount * 4 * sizeof(float), "colours", next.colors)
        || !put(source.triangles, source.triangleIndexCount * sizeof(std::int32_t), "triangle indices", next.triangles)
        || !put(source.instanceIds, source.instanceCount * sizeof(std::int32_t), "instance ids", next.instanceIds)
        || !put(source.materials, source.materialCount * sizeof(KernelMaterialRecord), "material records", next.materials)
        || !put(source.textureBindings, source.textureBindingCount * sizeof(KernelTextureBinding), "texture bindings", next.textureBindings)
        || !put(source.images, source.imageCount * 4 * sizeof(std::uint32_t), "image records", next.images)
        || !put(source.projections, source.projectionCount * sizeof(KernelProjectionRecord), "projection records", next.projections)
        || !put(source.points, source.pointCount * 4 * sizeof(float), "points", next.points)
        || !put(source.instanceTransforms, source.instanceTransformRowCount * 4 * sizeof(float), "instance transforms", next.instanceTransforms)
        || !put(source.sts, source.stCount * 2 * sizeof(float), "texture coordinates", next.sts)
        || !put(source.normals, source.normalCount * 4 * sizeof(float), "normals", next.normals)
        || !put(source.lights, source.lightCount * sizeof(KernelLightRecord), "light records", next.lights)
        || !put(source.lightData, source.lightDataCount * sizeof(float), "light tables", next.lightData)
        || !put(source.linkMasks, source.linkMaskCount * sizeof(std::uint32_t), "light-link masks", next.linkMasks)
        || !put(source.instanceMotion, source.instanceMotionCount * sizeof(std::uint32_t), "instance motion", next.instanceMotion)
        || !put(source.motionRows, source.motionRowCount * 4 * sizeof(float), "motion rows", next.motionRows)) {
        if (stoppedHere) {
            tables.texels = std::move(next.texels);
            tables.texelVersion = texelVersion;

            return stopped();
        }

        return false;
    }

    std::array<KernelTableAddress, eKernelTableCount> addresses {};

    addresses[eKernelTableGeometries] = tableAddress(next.geometries);
    addresses[eKernelTableColors] = tableAddress(next.colors);
    addresses[eKernelTableTriangles] = tableAddress(next.triangles);
    addresses[eKernelTableInstanceIds] = tableAddress(next.instanceIds);
    addresses[eKernelTablePoints] = tableAddress(next.points);
    addresses[eKernelTableInstanceTransforms] = tableAddress(next.instanceTransforms);
    addresses[eKernelTableSts] = tableAddress(next.sts);
    addresses[eKernelTableNormals] = tableAddress(next.normals);
    addresses[eKernelTableLightData] = tableAddress(next.lightData);
    addresses[eKernelTableLinkMasks] = tableAddress(next.linkMasks);
    addresses[eKernelTableTexels] = tableAddress(next.texels);
    addresses[eKernelTableInstanceMotion] = tableAddress(next.instanceMotion);
    addresses[eKernelTableMotionRows] = tableAddress(next.motionRows);
    if (!upload(*ctx, addresses.data(), sizeof(addresses), "table addresses", next.addresses)) {
        return false;
    }
    if (stop.load(std::memory_order_relaxed)) {
        // Kept for the next frame, which then uploads only the tables.
        tables.texels = std::move(next.texels);
        tables.texelVersion = texelVersion;

        return stopped();
    }
    if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
        return fail("WavefrontGpu could not upload its shading tables", s);
    }
    tables = std::move(next);
    tables.version = version;
    tables.keys = keys;
    tables.valid = true;

    return true;
}

bool
WavefrontGpu::Implementation::uploadUnwrap(const WavefrontUnwrapConstPtr& next)
{
    if (next == unwrap && unwrapGrid && unwrapTriangles && unwrapAddresses) {
        return true;
    }
    if (!settlePasses()) {
        return false;
    }
    unwrap.reset();
    unwrapGrid = gpu::GpuBufferHandle();
    unwrapTriangles = gpu::GpuBufferHandle();
    unwrapAddresses = gpu::GpuBufferHandle();

    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(registry, ctx, &pool); !s) {
        return fail("WavefrontGpu could not create a record context", s);
    }

    gpu::GpuBufferHandle grid;
    gpu::GpuBufferHandle triangles;
    gpu::GpuBufferHandle addresses;

    if (!upload(*ctx, next->grid.data(), next->grid.size() * sizeof(std::int32_t), "UV unwrap grid", grid)
        || !upload(*ctx, next->triangles.data(), next->triangles.size() * sizeof(float), "UV unwrap triangles", triangles)) {
        return false;
    }

    const std::array<KernelTableAddress, 2> tableAddresses = { tableAddress(grid), tableAddress(triangles) };

    if (!upload(*ctx, tableAddresses.data(), sizeof(tableAddresses), "UV unwrap addresses", addresses)) {
        return false;
    }
    if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
        return fail("WavefrontGpu could not upload its UV unwrap", s);
    }
    unwrap = next;
    unwrapGrid = std::move(grid);
    unwrapTriangles = std::move(triangles);
    unwrapAddresses = std::move(addresses);

    return true;
}

bool
WavefrontGpu::Implementation::uploadCameraSteps(const std::vector<float>& steps)
{
    if (cameraStepsBuffer && steps == cameraSteps) {
        return true;
    }
    if (!settlePasses()) {
        return false;
    }
    cameraStepsBuffer = gpu::GpuBufferHandle();

    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(registry, ctx, &pool); !s) {
        return fail("WavefrontGpu could not create a record context", s);
    }
    // A camera that does not move is one row whose step count is zero.
    const std::array<float, 4> still = { 0.f, 0.f, 0.f, 0.f };
    const bool moving = !steps.empty();
    gpu::GpuBufferHandle buffer;

    if (!upload(*ctx, moving ? steps.data() : still.data(), (moving ? steps.size() : still.size()) * sizeof(float), "camera steps", buffer)) {
        return false;
    }
    if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
        return fail("WavefrontGpu could not upload its camera steps", s);
    }
    cameraSteps = steps;
    cameraStepsBuffer = std::move(buffer);

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
        || !stage(rays * sizeof(float), "sample weights", readback.weight)
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

bool
WavefrontGpu::Implementation::reserveFlagReadback(std::size_t words)
{
    if (flagReadbackWords >= words) {
        return true;
    }
    flagReadback = gpu::GpuBuffer();
    flagReadbackWords = 0;
    if (!stage(words * sizeof(std::uint32_t), "ray flags", flagReadback)) {
        return false;
    }
    flagReadbackWords = words;

    return true;
}

bool
WavefrontGpu::Implementation::readFlags(Recorder& rec,
                                        const gpu::GpuBuffer& flags,
                                        std::size_t first,
                                        std::size_t words,
                                        const std::uint32_t*& out)
{
    if (gpu::GpuStatus s = rec.ctx().copyBuffer(flags, first * sizeof(std::uint32_t), flagReadback, 0, words * sizeof(std::uint32_t)); !s) {
        return fail("WavefrontGpu could not read back which rays go on", s);
    }
    if (!rec.sync()) {
        return false;
    }
    out = static_cast<const std::uint32_t*>(flagReadback.mapped());

    return true;
}

bool
WavefrontGpu::Implementation::haveFilmKernels()
{
    if (!filmKernelsTried) {
        filmKernelsTried = true;

        gpu::GpuStatus status;

        filmKernels = makeUtilityFilmKernels(device, &status);
    }

    return filmKernels != nullptr;
}

UtilityRenderBuffer*
WavefrontGpu::Implementation::deviceColorOf(const WavefrontFilm& film)
{
    UtilityRenderBuffer* buffer = film.deviceColor();

    if (!buffer || !buffer->canRenderOnDevice()) {
        return nullptr;
    }

    // Without the film's kernels the colour comes back in tiles, as every other AOV does.
    return haveFilmKernels() ? buffer : nullptr;
}

UtilityDeviceAovs*
WavefrontGpu::Implementation::deviceAovsOf(const WavefrontFilm& film,
                                           const gpu::GpuRecordContext& ctx,
                                           const WavefrontRect& window)
{
    UtilityDeviceAovs* aovs = film.deviceAovs();

    // A window too large for one binding hands its depth and ids over in tiles, batch by batch.
    if (!aovs || !haveFilmKernels() || !UtilityDeviceAovs::fits(ctx, window.width, window.height)) {
        return nullptr;
    }

    return aovs;
}

bool
WavefrontGpu::Implementation::Recorder::begin()
{
    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(_imp.registry, _current, &_imp.pool); !s) {
        return _imp.fail("WavefrontGpu could not create a record context", s);
    }
    _recorded = 0;
    _timestamp = _imp.takeTimestamp();
    if (_timestamp != kNoTimestamp) {
        _current->resetQueries(_imp.timestamps, 2 * _timestamp, 2);
        _current->writeTimestamp(_imp.timestamps, 2 * _timestamp);
    }
    // Passes left running are ahead on the queue, and the barrier orders their writes before this one's reads.
    if (!_imp.deferred.empty()) {
        _current->computeBarrier();
    }

    return true;
}

bool
WavefrontGpu::Implementation::Recorder::submit(const std::string& what)
{
    gpu::GpuStatus submitted;
    const auto begin = std::chrono::steady_clock::now();

    if (_timestamp != kNoTimestamp) {
        _current->writeTimestamp(_imp.timestamps, 2 * _timestamp + 1);
    }
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::GpuSubmitWait);

        submitted = _current->submitAndWait(kSubmitTimeoutNs, [this]() { return _stop.load(std::memory_order_relaxed); });
    }
    ++_imp.submissions;
    if (!submitted) {
        if (submitted.result == VK_INCOMPLETE && _stop.load(std::memory_order_relaxed)) {
            return _imp.stopped();
        }

        return _imp.fail(what, submitted);
    }
    _imp.readTimestamp(_timestamp);
    _imp.learnSubmitWork(_recorded, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count()));

    return true;
}

bool
WavefrontGpu::Implementation::Recorder::next()
{
    if (!submit("WavefrontGpu could not render part of a batch")) {
        return false;
    }
    _submitted.push_back(std::move(_current));
    if (_stop.load(std::memory_order_relaxed)) {
        return _imp.stopped();
    }
    if (!begin()) {
        return false;
    }
    // A barrier orders the earlier submissions' writes on the queue before this one's reads.
    _current->computeBarrier();

    return true;
}

bool
WavefrontGpu::Implementation::Recorder::reserve(std::uint64_t work)
{
    if (_stop.load(std::memory_order_relaxed)) {
        return _imp.stopped();
    }
    if (_recorded > 0 && _recorded + work > _work && !next()) {
        return false;
    }
    _recorded += work;

    return true;
}

bool
WavefrontGpu::Implementation::Recorder::finish(const std::string& what)
{
    return submit(what);
}

bool
WavefrontGpu::Implementation::Recorder::defer(const std::string& what)
{
    if (!_imp.passTimeline) {
        std::shared_ptr<PassTimeline> timeline = std::make_shared<PassTimeline>(_imp.device);

        if (gpu::GpuStatus s = _imp.device.createTimelineSemaphore(0, timeline->semaphore); !s) {
            return _imp.fail("WavefrontGpu could not create the semaphore of its passes", s);
        }
        _imp.passTimeline = std::move(timeline);
    }
    _imp.retirePasses();

    std::unique_ptr<DeferredPass> pass(new DeferredPass);

    pass->timeline = _imp.passTimeline;
    pass->value = _imp.passTimeline->submitted + 1;
    pass->work = _recorded;
    pass->estimatedNs = _imp.estimatedNs(_recorded);
    pass->alone = _imp.deferred.empty();
    pass->submittedAt = std::chrono::steady_clock::now();
    pass->timestamp = _timestamp;
    if (_timestamp != kNoTimestamp) {
        _current->writeTimestamp(_imp.timestamps, 2 * _timestamp + 1);
    }

    gpu::GpuStatus submitted;

    {
        const utilityProfile::Scope timed(utilityProfile::Stage::GpuSubmitWait);

        submitted = _current->submit({}, gpu::TimelineValue { pass->timeline->semaphore, pass->value }, [this]() { return _stop.load(std::memory_order_relaxed); });
    }
    ++_imp.submissions;
    // An aborted context still submits the signal; one never submitted signals nothing and frees at once.
    pass->signalled = submitted || submitted.aborted;
    if (pass->signalled) {
        pass->timeline->submitted = pass->value;
    }
    pass->contexts = std::move(_submitted);
    pass->contexts.push_back(std::move(_current));
    _imp.deferred.push_back(std::move(pass));
    if (!submitted) {
        if (submitted.result == VK_INCOMPLETE && _stop.load(std::memory_order_relaxed)) {
            return _imp.stopped();
        }

        return _imp.fail(what, submitted);
    }

    return true;
}

void
WavefrontGpu::Implementation::learnSubmitWork(std::uint64_t work,
                                              std::uint64_t ns)
{
    // A submission far below the budget is mostly the fixed cost of a submit and a wait, which would teach a budget
    // smaller than the device can take.
    if (ns == 0 || work == 0 || work < submitWork / 4) {
        return;
    }

    const double sample = static_cast<double>(work) * static_cast<double>(kSubmitTargetNs) / static_cast<double>(ns);
    const std::uint64_t measured = sample >= static_cast<double>(kMaxLearnedSubmitWork) ? kMaxLearnedSubmitWork : std::max(kMaxSubmitWork, static_cast<std::uint64_t>(sample));

    submitWork = (submitWork * 3 + measured) / 4;
}

// The learned work scaled to the frame's submission time.
std::uint64_t
WavefrontGpu::Implementation::submitWorkFor(const WavefrontFrame& frame) const
{
    if (!frame.interactive) {
        return submitWork;
    }

    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(static_cast<double>(submitWork) * static_cast<double>(kInteractiveSubmitTargetNs) / static_cast<double>(kSubmitTargetNs)));
}

std::uint64_t
WavefrontGpu::Implementation::estimatedNs(std::uint64_t work) const
{
    return static_cast<std::uint64_t>(static_cast<double>(work) * static_cast<double>(kSubmitTargetNs) / static_cast<double>(std::max<std::uint64_t>(1, submitWork)));
}

void
WavefrontGpu::Implementation::retirePasses()
{
    if (deferred.empty()) {
        return;
    }

    const std::uint64_t reached = passTimeline->reached();

    while (!deferred.empty() && (!deferred.front()->signalled || deferred.front()->value <= reached)) {
        if (deferred.front()->signalled) {
            readTimestamp(deferred.front()->timestamp);
        }
        deferred.pop_front();
    }
}

std::uint32_t
WavefrontGpu::Implementation::takeTimestamp()
{
    if (!utilityProfile::enabled() || !utilityProfile::currentFrame()) {
        return kNoTimestamp;
    }
    if (!timestampsTried) {
        timestampsTried = true;

        std::uint32_t families = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &families, nullptr);

        std::vector<VkQueueFamilyProperties> properties(families);

        vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &families, properties.data());

        const std::uint32_t family = device.queueFamily(gpu::QueueKind::Compute);
        const std::uint32_t bits = family < families ? properties[family].timestampValidBits : 0;
        VkPhysicalDeviceProperties limits;

        vkGetPhysicalDeviceProperties(device.physicalDevice(), &limits);
        if (bits != 0) {
            VkQueryPoolCreateInfo info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };

            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2 * kTimestampPairs;
            if (vkCreateQueryPool(device.device(), &info, nullptr, &timestamps) != VK_SUCCESS) {
                timestamps = VK_NULL_HANDLE;
            }
            timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            timestampPeriodNs = limits.limits.timestampPeriod;
        }
    }
    if (timestamps == VK_NULL_HANDLE) {
        return kNoTimestamp;
    }

    const std::uint32_t pair = nextTimestamp;

    nextTimestamp = (nextTimestamp + 1) % kTimestampPairs;

    return pair;
}

void
WavefrontGpu::Implementation::readTimestamp(std::uint32_t pair)
{
    if (pair == kNoTimestamp || timestamps == VK_NULL_HANDLE) {
        return;
    }

    std::uint64_t ticks[2] = { 0, 0 };

    if (vkGetQueryPoolResults(device.device(), timestamps, 2 * pair, 2, sizeof(ticks), ticks, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    utilityProfile::addGpuTime(utilityProfile::Stage::GpuSubmitWait, static_cast<double>((ticks[1] - ticks[0]) & timestampMask) * timestampPeriodNs / 1e6);
}

bool
WavefrontGpu::Implementation::waitPass(DeferredPass& pass,
                                       const std::atomic<bool>* stop)
{
    if (!pass.signalled) {
        return true;
    }
    if (pass.timeline->reached() >= pass.value) {
        readTimestamp(pass.timestamp);

        return true;
    }

    const auto start = std::chrono::steady_clock::now();
    gpu::GpuStatus waited;

    {
        const utilityProfile::Scope timed(utilityProfile::Stage::GpuSubmitWait);

        for (;;) {
            if (stop && stop->load(std::memory_order_relaxed)) {
                return stopped();
            }
            waited = device.waitSemaphore(pass.timeline->semaphore, pass.value, stop ? kPassWaitSliceNs : kSubmitTimeoutNs);
            if (waited || (waited.result != VK_TIMEOUT) || !stop) {
                break;
            }
            if (std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(kSubmitTimeoutNs)) {
                break;
            }
        }
    }
    if (!waited) {
        return fail("WavefrontGpu could not wait for a pass", waited);
    }
    readTimestamp(pass.timestamp);
    // Waited for, the device was still on it, so the time since it went is what the work alone took.
    if (pass.alone) {
        learnSubmitWork(pass.work, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pass.submittedAt).count()));
    }

    return true;
}

bool
WavefrontGpu::Implementation::throttlePasses(const WavefrontFrame& frame,
                                             const std::atomic<bool>& stop)
{
    const std::uint64_t ahead = frame.interactive ? kInteractiveSubmitTargetNs : kSubmitTargetNs;

    retirePasses();
    for (;;) {
        std::uint64_t running = 0;

        for (const std::unique_ptr<DeferredPass>& pass : deferred) {
            running += pass->estimatedNs;
        }
        // The last pass alone may be over the bound: a pass is never split by this.
        if (deferred.size() <= 1 || running <= ahead) {
            return true;
        }
        if (!waitPass(*deferred.front(), &stop)) {
            return false;
        }
        deferred.pop_front();
    }
}

bool
WavefrontGpu::Implementation::finishPasses(const std::atomic<bool>* stop)
{
    while (!deferred.empty()) {
        if (!waitPass(*deferred.front(), stop)) {
            return false;
        }
        deferred.pop_front();
    }

    return true;
}

// The device bytes renderBatch() asks for per ray of a batch, an upper bound for the frame's settings.
std::uint64_t
WavefrontGpu::Implementation::batchBytesPerRay(const WavefrontFrame& frame) const
{
    const std::uint64_t f = sizeof(float);
    const bool holes = shadeTables.hasHoles();
    const bool transmission = shadeTables.hasTransmission();
    const bool resolveShadows = holes || transmission;
    const std::uint32_t limit = wavefrontBounceLimit(frame, transmission);
    const std::uint64_t lights = std::min<std::uint64_t>(shadeTables.lightCount(), kLightBatch);
    const std::uint64_t resolve = kRayArrays * f + 2 * kHitArrays * f;
    // The rays, weights, hits, hit ids, colours, depths, prim and instance ids, and the visibility buffer.
    std::uint64_t bytes = kRayArrays * f + f + 2 * kHitArrays * f + 4 * f + 3 * f + 2 * sizeof(std::uint32_t);

    if (holes) {
        bytes += resolve;
    }
    if (limit > 0) {
        bytes += (limit > 1 ? 2 : 1) * (kRayArrays * f + sizeof(KernelPathState)) + 2 * kHitArrays * f;
    }
    bytes += lights * (kRayArrays * f + 4 * f + 2 * sizeof(std::uint32_t) + (resolveShadows ? 2 * kHitArrays * f + resolve : 0));

    return bytes;
}

/**
 * @brief The rays a batch takes: the whole window when the device's free memory and the learned submission work
 * allow it, so that a pass is one submission, else fewer, each batch a submission of its own at least.
 **/
std::uint64_t
WavefrontGpu::Implementation::batchRaysFor(const WavefrontFrame& frame) const
{
    const std::uint64_t window = static_cast<std::uint64_t>(frame.window.width) * static_cast<std::uint64_t>(frame.window.height);
    const std::uint64_t budget = pool.budget();
    const std::uint64_t live = pool.stats().liveBytes;
    // Half the free memory, as the pool rounds each request up to a size class and other nodes need some too.
    const std::uint64_t memoryRays = (budget > live ? (budget - live) / 2 : 0) / std::max<std::uint64_t>(batchBytesPerRay(frame), 1);
    const std::uint64_t latencyRays = std::max(kMinBatchRays, submitWork / kWorkPerBatchRay);

    return std::min({ window, memoryRays, latencyRays });
}

bool
WavefrontGpu::Implementation::reserveResolve(gpu::GpuRecordContext& ctx,
                                             std::size_t count,
                                             std::uint32_t maxCutouts,
                                             ResolveScratch& out)
{
    const std::string rayCount = std::to_string(count) + " rays";

    if (gpu::GpuStatus s = ctx.scratch(kRayArrays * count * sizeof(float), out.retrace); !s) {
        return fail("WavefrontGpu could not hold the rays going on through holes of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(kHitArrays * count * sizeof(float), out.nextHitValues); !s) {
        return fail("WavefrontGpu could not hold the hits past holes of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch(kHitArrays * count * sizeof(std::uint32_t), out.nextHitIds); !s) {
        return fail("WavefrontGpu could not hold the hit ids past holes of " + rayCount, s);
    }
    if (gpu::GpuStatus s = ctx.scratch((maxCutouts + 1) * sizeof(std::uint32_t), out.pending); !s) {
        return fail("WavefrontGpu could not hold the pending flags of " + rayCount, s);
    }

    return true;
}

/**
 * @brief Records the passes that take count rays' hits on through holes: every pass resolves the whole
 * batch and traces it again, the rays already on a surface skipped by the tracer. The passes end once
 * one queues no ray, which the host learns only by waiting for the work so far, so it looks after
 * passes 1, 3, 7 and so on: each wait stalls the device, while a pass with no ray left costs little.
 * ResolveHits.slang tells what each pass does.
 **/
bool
WavefrontGpu::Implementation::recordResolve(Recorder& rec,
                                            const ResolveScratch& scratch,
                                            KernelResolveParams params,
                                            std::size_t count,
                                            std::uint32_t maxCutouts,
                                            const gpu::GpuBuffer& rays,
                                            const gpu::GpuBuffer& hitValues,
                                            const gpu::GpuBuffer& hitIds,
                                            const gpu::GpuBuffer& blocked,
                                            const gpu::GpuBuffer& rayLights,
                                            const gpu::GpuBuffer& shadowContributions)
{
    gpu::GpuBuffer& retrace = *scratch.retrace;
    gpu::GpuBuffer& nextHitValues = *scratch.nextHitValues;
    gpu::GpuBuffer& nextHitIds = *scratch.nextHitIds;
    gpu::GpuBuffer& pending = *scratch.pending;
    const std::array<gpu::GpuBinding, 15> bindings = {
        gpu::GpuBinding::storage(0, rays),
        gpu::GpuBinding::storage(1, hitValues),
        gpu::GpuBinding::storage(2, hitIds),
        gpu::GpuBinding::storage(3, nextHitValues),
        gpu::GpuBinding::storage(4, nextHitIds),
        gpu::GpuBinding::storage(5, retrace),
        gpu::GpuBinding::storage(6, blocked),
        gpu::GpuBinding::storage(7, rayLights),
        gpu::GpuBinding::storage(8, tables.addresses.buffer()),
        gpu::GpuBinding::storage(9, tables.materials.buffer()),
        gpu::GpuBinding::storage(10, tables.textureBindings.buffer()),
        gpu::GpuBinding::storage(11, tables.images.buffer()),
        gpu::GpuBinding::storage(12, tables.projections.buffer()),
        gpu::GpuBinding::storage(13, shadowContributions),
        gpu::GpuBinding::storage(14, pending),
    };
    const std::array<std::uint32_t, 3> threads = { static_cast<std::uint32_t>(count), 1, 1 };
    const std::uint32_t flags = params.flags;

    if (gpu::GpuStatus s = rec.ctx().fillBuffer(pending, (maxCutouts + 1) * sizeof(std::uint32_t), 0u); !s) {
        return fail("WavefrontGpu could not clear the pending flags", s);
    }
    params.count = static_cast<std::uint32_t>(count);
    for (std::uint32_t pass = 0; pass <= maxCutouts; ++pass) {
        params.flags = flags | (pass > 0 ? kKernelResolveMerge : 0u) | (pass == maxCutouts ? kKernelResolveLast : 0u);
        params.passIndex = pass;
        if (!rec.reserve(count)) {
            return false;
        }
        if (gpu::GpuStatus s = rec.ctx().dispatchBindings(resolveKernel, bindings, std::as_bytes(std::span(&params, 1)), threads); !s) {
            return fail("WavefrontGpu could not resolve the hits on holes", s);
        }
        rec.ctx().computeBarrier();
        if (pass == maxCutouts) {
            break;
        }
        if (pass > 0 && ((pass + 1) & pass) == 0) {
            const std::uint32_t* queued = nullptr;

            if (!readFlags(rec, pending, pass, 1, queued)) {
                return false;
            }
            if (queued[0] == 0) {
                break;
            }
        }
        if (!rec.reserve(count)) {
            return false;
        }
        if (gpu::GpuStatus s = tracer.recordIntersect(rec.ctx(), retrace, static_cast<std::uint32_t>(count), nextHitValues, nextHitIds); !s) {
            return fail("WavefrontGpu could not trace past holes", s);
        }
        rec.ctx().computeBarrier();
    }

    return true;
}

/**
 * @brief Fills the batch's camera rays and their hits as RayGen and recordIntersect() would for one sample of
 * every pixel, at the pass's jitter and time: each tile of the batch, at most the device's image limit on a side,
 * is drawn into a visibility buffer and its hits rebuilt from it, every tile in one raster context submitted
 * before the batch's compute context, which then takes the buffers back.
 **/
bool
WavefrontGpu::Implementation::rasterHits(const WavefrontFrame& frame,
                                         const WavefrontRect& rect,
                                         Recorder& rec,
                                         gpu::GpuBuffer& rays,
                                         gpu::GpuBuffer& weights,
                                         gpu::GpuBuffer& hitValues,
                                         gpu::GpuBuffer& hitIds,
                                         bool cutouts)
{
    std::shared_ptr<const GpuTracer::RasterScene> scene;

    if (gpu::GpuStatus s = tracer.rasterScene(scene); !s) {
        return fail("WavefrontGpu could not hold the scene for drawing", s);
    }

    const WavefrontRect& window = frame.window;
    const std::size_t count = static_cast<std::size_t>(rect.width) * rect.height;
    const WavefrontRasterPass& pass = frame.rasterPass;
    const KernelRayGenParams rayGen = makeRayGenParams(frame, rect);
    KernelRasterHitsParams params {};

    std::copy(std::begin(rayGen.nearAtCentre), std::end(rayGen.nearAtCentre), params.nearAtCentre);
    std::copy(std::begin(rayGen.nearPerX), std::end(rayGen.nearPerX), params.nearPerX);
    std::copy(std::begin(rayGen.nearPerY), std::end(rayGen.nearPerY), params.nearPerY);
    std::copy(std::begin(rayGen.farAtCentre), std::end(rayGen.farAtCentre), params.farAtCentre);
    std::copy(std::begin(rayGen.farPerX), std::end(rayGen.farPerX), params.farPerX);
    std::copy(std::begin(rayGen.farPerY), std::end(rayGen.farPerY), params.farPerY);
    params.time = pass.time;
    params.weight = pass.weight;
    params.jitterX = pass.jitter[0];
    params.jitterY = pass.jitter[1];
    params.count = static_cast<std::uint32_t>(count);
    params.batchRow = rect.y - window.y;
    params.windowWidth = window.width;
    params.windowHeight = window.height;
    storeAddress(scene->hitTables ? scene->hitTables->deviceAddress(device) : 0, params.tablesLow, params.tablesHigh);

    const PXR_NS::GfMatrix4d viewProjection = frame.worldToView * frame.projection;
    std::uint32_t side = std::max<std::uint32_t>(device.info().maxImageDimension2D, 1);

    if (rasterTileLimit > 0) {
        side = std::min(side, rasterTileLimit);
    }

    const int step = static_cast<int>(std::min<std::uint32_t>(side, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    const std::array<std::pair<gpu::GpuBuffer*, VkDeviceSize>, 4> handed = {
        std::make_pair(&rays, static_cast<VkDeviceSize>(kRayArrays * count * sizeof(float))),
        std::make_pair(&weights, static_cast<VkDeviceSize>(count * sizeof(float))),
        std::make_pair(&hitValues, static_cast<VkDeviceSize>(kHitArrays * count * sizeof(float))),
        std::make_pair(&hitIds, static_cast<VkDeviceSize>(kHitArrays * count * sizeof(std::uint32_t))),
    };
    const std::atomic<bool>& stop = rec.stop();
    // Every call after a context's abort check has fired fails, not only the one that ran it: a stop that lands
    // between draws and the rebuild must still end the frame as stopped rather than as failed.
    const auto failUnlessStopped = [this, &stop](const std::string& what, const gpu::GpuStatus& s) {
        return (s.aborted || stop.load(std::memory_order_relaxed)) ? stopped() : fail(what, s);
    };

    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (stop.load(std::memory_order_relaxed)) {
        return stopped();
    }
    if (gpu::GpuStatus s = gpu::GpuRecordContext::createRaster(registry, ctx, &pool, pipelines); !s) {
        return fail("WavefrontGpu could not create a raster context", s);
    }
    ctx->setAbortCheck([&stop]() {
        return stop.load(std::memory_order_relaxed);
    });

    std::array<const gpu::GpuBuffer*, 6> cutoutBuffers {};

    if (cutouts) {
        KernelCutoutFrame cutoutFrame {};
        const KernelShadeParams shade = makeShadeParams(frame, static_cast<std::uint32_t>(count));
        gpu::GpuBuffer* frameBuffer = nullptr;

        std::copy(std::begin(params.nearAtCentre), std::end(params.nearAtCentre), cutoutFrame.nearAtCentre);
        std::copy(std::begin(params.nearPerX), std::end(params.nearPerX), cutoutFrame.nearPerX);
        std::copy(std::begin(params.nearPerY), std::end(params.nearPerY), cutoutFrame.nearPerY);
        std::copy(std::begin(params.farAtCentre), std::end(params.farAtCentre), cutoutFrame.farAtCentre);
        std::copy(std::begin(params.farPerX), std::end(params.farPerX), cutoutFrame.farPerX);
        std::copy(std::begin(params.farPerY), std::end(params.farPerY), cutoutFrame.farPerY);
        std::copy(std::begin(shade.pixelFootprint), std::end(shade.pixelFootprint), cutoutFrame.pixelFootprint);
        cutoutFrame.jitterX = params.jitterX;
        cutoutFrame.jitterY = params.jitterY;
        cutoutFrame.windowWidth = params.windowWidth;
        cutoutFrame.windowHeight = params.windowHeight;
        if (gpu::GpuStatus s = ctx->scratch(sizeof(cutoutFrame), frameBuffer); !s) {
            return fail("WavefrontGpu could not hold the camera of a visibility pass", s);
        }
        if (gpu::GpuStatus s = ctx->updateBuffer(*frameBuffer, 0, std::as_bytes(std::span(&cutoutFrame, 1))); !s) {
            return failUnlessStopped("WavefrontGpu could not set the camera of a visibility pass", s);
        }
        cutoutBuffers = { &tables.addresses.buffer(), &tables.materials.buffer(), &tables.textureBindings.buffer(),
                          &tables.images.buffer(), &tables.projections.buffer(), frameBuffer };
    }
    for (int tileY = 0; tileY < rect.height; tileY += step) {
        for (int tileX = 0; tileX < rect.width; tileX += step) {
            const int tileWidth = std::min(step, rect.width - tileX);
            const int tileHeight = std::min(step, rect.height - tileY);
            const bool last = tileX + tileWidth >= rect.width && tileY + tileHeight >= rect.height;
            const std::size_t texels = static_cast<std::size_t>(tileWidth) * tileHeight;

            if (stop.load(std::memory_order_relaxed)) {
                return stopped();
            }
            if (!visibilityColour || visibilityColour->desc().width != static_cast<std::uint32_t>(tileWidth) || visibilityColour->desc().height != static_cast<std::uint32_t>(tileHeight)) {
                // The tiles before this one drew into the attachments the context has recorded so far.
                if (visibilityColour) {
                    ctx->hold(std::shared_ptr<gpu::GpuTexture>(std::move(visibilityColour)));
                }
                if (visibilityDepth) {
                    ctx->hold(std::shared_ptr<gpu::GpuTexture>(std::move(visibilityDepth)));
                }
                visibilityColour.reset();
                visibilityDepth.reset();
                if (gpu::GpuStatus s = gpu::GpuTexture::createAttachment(device, tileWidth, tileHeight, VK_FORMAT_R32G32_UINT, visibilityColour); !s) {
                    return fail("WavefrontGpu could not hold a visibility buffer", s);
                }
                if (gpu::GpuStatus s = gpu::GpuTexture::createDepthAttachment(device, tileWidth, tileHeight, visibilityDepth); !s) {
                    visibilityColour.reset();

                    return fail("WavefrontGpu could not hold the depth of a visibility buffer", s);
                }
            }

            gpu::GpuBuffer* visibility = nullptr;

            if (gpu::GpuStatus s = ctx->scratch(texels * 2 * sizeof(std::uint32_t), visibility); !s) {
                return fail("WavefrontGpu could not hold the visibility of " + std::to_string(texels) + " pixels", s);
            }

            GpuTracer::VisibilityPass visibilityPass;
            const PXR_NS::GfMatrix4d tileViewProjection = viewProjection * tileClip(window, tileX, rect.y - window.y + tileY, tileWidth, tileHeight);

            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    visibilityPass.viewProjection[r][c] = static_cast<float>(tileViewProjection[r][c]);
                }
            }
            visibilityPass.jitter[0] = pass.jitter[0];
            visibilityPass.jitter[1] = pass.jitter[1];
            visibilityPass.time = pass.time;
            if (cutouts) {
                visibilityPass.cutoutBuffers = cutoutBuffers;
                visibilityPass.origin[0] = tileX;
                visibilityPass.origin[1] = params.batchRow + tileY;
            }
            if (gpu::GpuStatus s = ctx->beginRasterUint(*visibilityColour, { Tracer::kNoHit, Tracer::kNoHit, 0u, 0u }, visibilityDepth.get(), 1.0f); !s) {
                return failUnlessStopped("WavefrontGpu could not begin a visibility pass", s);
            }
            if (gpu::GpuStatus s = tracer.recordVisibility(*ctx, visibilityPass); !s) {
                return failUnlessStopped("WavefrontGpu could not draw a visibility pass", s);
            }
            if (gpu::GpuStatus s = ctx->endRaster(); !s) {
                return failUnlessStopped("WavefrontGpu could not end a visibility pass", s);
            }
            if (gpu::GpuStatus s = ctx->copyToImage(*visibility); !s) {
                return failUnlessStopped("WavefrontGpu could not read a visibility buffer", s);
            }

            params.tileX = tileX;
            params.tileY = tileY;
            params.tileWidth = tileWidth;
            params.tileHeight = tileHeight;

            const std::array<gpu::GpuBinding, 5> bindings = {
                gpu::GpuBinding::storage(0, rays), gpu::GpuBinding::storage(1, weights), gpu::GpuBinding::storage(2, hitValues),
                gpu::GpuBinding::storage(3, hitIds), gpu::GpuBinding::storage(4, *visibility)
            };

            if (gpu::GpuStatus s = ctx->dispatchBindings(rasterHitsKernel, bindings, std::as_bytes(std::span(&params, 1)), { static_cast<std::uint32_t>(texels), 1, 1 }); !s) {
                return failUnlessStopped("WavefrontGpu could not rebuild the hits of a visibility pass", s);
            }
            if (last) {
                for (const auto& [buffer, bytes] : handed) {
                    if (gpu::GpuStatus s = ctx->releaseToOtherQueue(*buffer, 0, bytes); !s) {
                        return failUnlessStopped("WavefrontGpu could not hand a visibility pass's hits to its compute queue", s);
                    }
                }
            }
        }
    }

    std::shared_ptr<PendingRaster> pending = std::make_shared<PendingRaster>(device);

    if (gpu::GpuStatus s = device.createTimelineSemaphore(0, pending->semaphore); !s) {
        return fail("WavefrontGpu could not create the semaphore of a visibility pass", s);
    }

    const gpu::TimelineValue drawn { pending->semaphore, 1 };
    gpu::GpuStatus submitted;

    {
        const utilityProfile::Scope timed(utilityProfile::Stage::RasterSubmitWait);

        submitted = ctx->submit({}, drawn, [&stop]() { return stop.load(std::memory_order_relaxed); });
    }
    ++submissions;
    // An aborted context still submits the signal, and the semaphore must outlive it.
    pending->signalled = submitted || submitted.aborted;
    if (!submitted) {
        return failUnlessStopped("WavefrontGpu could not draw a visibility pass", submitted);
    }
    pending->ctx = std::move(ctx);
    // The compute work waits on the device rather than the host, and holds the raster context until it is done.
    rec.ctx().waitTimeline(drawn);
    rec.ctx().hold(pending);
    for (const auto& [buffer, bytes] : handed) {
        if (gpu::GpuStatus s = rec.ctx().acquireFromOtherQueue(*buffer, 0, bytes); !s) {
            return failUnlessStopped("WavefrontGpu could not take a visibility pass's hits on its compute queue", s);
        }
    }

    return true;
}

bool
WavefrontGpu::Implementation::renderBatch(const WavefrontFrame& frame,
                                          const WavefrontRect& rect,
                                          Recorder& rec,
                                          WavefrontFilm& film,
                                          UtilityRenderBuffer* deviceColor,
                                          UtilityDeviceAovs* deviceAovs,
                                          std::uint32_t& deepest)
{
    const std::size_t count = static_cast<std::size_t>(rect.width) * rect.height;
    const std::array<std::uint32_t, 3> threads = { static_cast<std::uint32_t>(count), 1, 1 };
    WavefrontAovs aovs = film.aovs();

    if (deviceColor) {
        aovs.color = false;
    }
    if (deviceAovs) {
        aovs.depth = false;
        aovs.primId = false;
        aovs.instanceId = false;
    }
    if (!reserveReadback(count, aovs)) {
        return false;
    }

    gpu::GpuBuffer* rays = nullptr;
    gpu::GpuBuffer* weights = nullptr;
    gpu::GpuBuffer* hitValues = nullptr;
    gpu::GpuBuffer* hitIds = nullptr;
    gpu::GpuBuffer* color = nullptr;
    gpu::GpuBuffer* depth = nullptr;
    gpu::GpuBuffer* primId = nullptr;
    gpu::GpuBuffer* instanceId = nullptr;
    const std::string rayCount = std::to_string(count) + " rays";

    if (gpu::GpuStatus s = rec.ctx().scratch(kRayArrays * count * sizeof(float), rays); !s) {
        return fail("WavefrontGpu could not hold a batch of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(count * sizeof(float), weights); !s) {
        return fail("WavefrontGpu could not hold the sample weights of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(kHitArrays * count * sizeof(float), hitValues); !s) {
        return fail("WavefrontGpu could not hold the hits of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(kHitArrays * count * sizeof(std::uint32_t), hitIds); !s) {
        return fail("WavefrontGpu could not hold the hit ids of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(count * 4 * sizeof(float), color); !s) {
        return fail("WavefrontGpu could not hold the colours of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(count * sizeof(float), depth); !s) {
        return fail("WavefrontGpu could not hold the depths of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(count * sizeof(std::int32_t), primId); !s) {
        return fail("WavefrontGpu could not hold the prim ids of " + rayCount, s);
    }
    if (gpu::GpuStatus s = rec.ctx().scratch(count * sizeof(std::int32_t), instanceId); !s) {
        return fail("WavefrontGpu could not hold the instance ids of " + rayCount, s);
    }

    const KernelRayGenParams rayGenParams = makeRayGenParams(frame, rect);

    if (!rec.reserve(2 * count)) {
        return false;
    }
    if (frame.mapping == WavefrontMapping::UvUnwrap) {
        const utilityProfile::Scope timed(utilityProfile::Stage::RayGenIntersect);
        const std::array<gpu::GpuBinding, 5> bindings = {
            gpu::GpuBinding::storage(0, *rays), gpu::GpuBinding::storage(1, *weights), gpu::GpuBinding::storage(2, *hitValues),
            gpu::GpuBinding::storage(3, *hitIds), gpu::GpuBinding::storage(4, unwrapAddresses.buffer())
        };

        if (gpu::GpuStatus s = rec.ctx().dispatchBindings(rayGenUvKernel, bindings, std::as_bytes(std::span(&rayGenParams, 1)), threads); !s) {
            return fail("WavefrontGpu could not generate rays", s);
        }
    } else if (frame.raster) {
        const utilityProfile::Scope timed(utilityProfile::Stage::Raster);

        if (!rasterHits(frame, rect, rec, *rays, *weights, *hitValues, *hitIds, untraced && shadeTables.hasHoles())) {
            return false;
        }
    } else {
        const utilityProfile::Scope timed(utilityProfile::Stage::RayGenIntersect);
        const std::array<gpu::GpuBinding, 3> bindings = { gpu::GpuBinding::storage(0, *rays), gpu::GpuBinding::storage(1, *weights), gpu::GpuBinding::storage(2, cameraStepsBuffer.buffer()) };

        if (gpu::GpuStatus s = rec.ctx().dispatchBindings(rayGenKernel, bindings, std::as_bytes(std::span(&rayGenParams, 1)), threads); !s) {
            return fail("WavefrontGpu could not generate rays", s);
        }
        rec.ctx().computeBarrier();
        if (gpu::GpuStatus s = tracer.recordIntersect(rec.ctx(), *rays, static_cast<std::uint32_t>(count), *hitValues, *hitIds); !s) {
            return fail("WavefrontGpu could not intersect", s);
        }
    }
    rec.ctx().computeBarrier();

    KernelShadeParams shadeParams = makeShadeParams(frame, static_cast<std::uint32_t>(count));
    // An untraced frame's visibility pass left its holes undrawn, and nothing its lights or bounces would add shows.
    const bool holes = !untraced && shadeTables.hasHoles();
    const bool transmission = shadeTables.hasTransmission();
    // Shadow rays then take closest hits, so that a hole or a transmissive surface lets them go on.
    const bool resolveShadows = !untraced && (holes || transmission);
    const std::uint32_t limit = untraced ? 0u : wavefrontBounceLimit(frame, transmission);
    KernelResolveParams resolveParams {};
    ResolveScratch scratch;

    if ((limit > 0 || resolveShadows) && !reserveFlagReadback(std::max<std::size_t>(limit, 1))) {
        return false;
    }

    resolveParams.linkWords = shadeTables.linkWords();
    if (holes) {
        const utilityProfile::Scope timed(utilityProfile::Stage::Resolve);

        std::copy(std::begin(shadeParams.pixelFootprint), std::end(shadeParams.pixelFootprint), std::begin(resolveParams.pixelFootprint));
        if (!reserveResolve(rec.ctx(), count, frame.maxCutouts, scratch) || !recordResolve(rec, scratch, resolveParams, count, frame.maxCutouts, *rays, *hitValues, *hitIds, *rays, *rays, *rays)) {
            return false;
        }
    }

    // Each bounce reads its rays and paths from one queue and writes the next bounce's to the other.
    std::array<gpu::GpuBuffer*, 2> queueRays = { rays, rays };
    std::array<gpu::GpuBuffer*, 2> queuePaths = { rays, rays };
    gpu::GpuBuffer* bounceHitValues = rays;
    gpu::GpuBuffer* bounceHitIds = rays;
    // One word per bounce, ShadeSurface.slang's liveBounces.
    gpu::GpuBuffer* liveBounces = rays;

    if (limit > 0) {
        for (std::size_t q = 0; q < (limit > 1 ? 2u : 1u); ++q) {
            if (gpu::GpuStatus s = rec.ctx().scratch(kRayArrays * count * sizeof(float), queueRays[q]); !s) {
                return fail("WavefrontGpu could not hold the bounced rays of " + rayCount, s);
            }
            if (gpu::GpuStatus s = rec.ctx().scratch(count * sizeof(KernelPathState), queuePaths[q]); !s) {
                return fail("WavefrontGpu could not hold the paths of " + rayCount, s);
            }
        }
        if (gpu::GpuStatus s = rec.ctx().scratch(kHitArrays * count * sizeof(float), bounceHitValues); !s) {
            return fail("WavefrontGpu could not hold the bounced hits of " + rayCount, s);
        }
        if (gpu::GpuStatus s = rec.ctx().scratch(kHitArrays * count * sizeof(std::uint32_t), bounceHitIds); !s) {
            return fail("WavefrontGpu could not hold the bounced hit ids of " + rayCount, s);
        }
        if (holes && !scratch.retrace && !reserveResolve(rec.ctx(), count, frame.maxCutouts, scratch)) {
            return false;
        }
        if (gpu::GpuStatus s = rec.ctx().scratch(limit * sizeof(std::uint32_t), liveBounces); !s) {
            return fail("WavefrontGpu could not hold the live flags of " + rayCount, s);
        }
        if (gpu::GpuStatus s = rec.ctx().fillBuffer(*liveBounces, limit * sizeof(std::uint32_t), 0u); !s) {
            return fail("WavefrontGpu could not clear the live flags", s);
        }
    }

    const std::uint32_t lightCount = shadeTables.lightCount();
    const std::uint64_t bindable = std::min(rec.ctx().maxScratchBytes(), rec.ctx().maxStorageBufferBytes()) / (kRayArrays * sizeof(float));
    const std::uint32_t batch = untraced ? 0u : static_cast<std::uint32_t>(std::min<std::uint64_t>({ lightCount, kLightBatch, std::max<std::uint64_t>(1, bindable / count) }));
    const std::size_t shadowCount = count * batch;
    gpu::GpuBuffer* shadowRays = rays;
    gpu::GpuBuffer* contributions = rays;
    gpu::GpuBuffer* shadowLights = rays;
    gpu::GpuBuffer* blocked = rays;
    gpu::GpuBuffer* shadowHitValues = rays;
    gpu::GpuBuffer* shadowHitIds = rays;
    ResolveScratch shadowScratch;

    if (batch > 0) {
        const std::string shadowRayCount = std::to_string(shadowCount) + " shadow rays";

        if (gpu::GpuStatus s = rec.ctx().scratch(kRayArrays * shadowCount * sizeof(float), shadowRays); !s) {
            return fail("WavefrontGpu could not hold a batch of " + shadowRayCount, s);
        }
        if (gpu::GpuStatus s = rec.ctx().scratch(shadowCount * 4 * sizeof(float), contributions); !s) {
            return fail("WavefrontGpu could not hold the contributions of " + shadowRayCount, s);
        }
        if (gpu::GpuStatus s = rec.ctx().scratch(shadowCount * sizeof(std::uint32_t), shadowLights); !s) {
            return fail("WavefrontGpu could not hold the lights of " + shadowRayCount, s);
        }
        if (gpu::GpuStatus s = rec.ctx().scratch(shadowCount * sizeof(std::uint32_t), blocked); !s) {
            return fail("WavefrontGpu could not hold the occlusion of " + shadowRayCount, s);
        }
        if (resolveShadows) {
            if (gpu::GpuStatus s = rec.ctx().scratch(kHitArrays * shadowCount * sizeof(float), shadowHitValues); !s) {
                return fail("WavefrontGpu could not hold the hits of " + shadowRayCount, s);
            }
            if (gpu::GpuStatus s = rec.ctx().scratch(kHitArrays * shadowCount * sizeof(std::uint32_t), shadowHitIds); !s) {
                return fail("WavefrontGpu could not hold the hit ids of " + shadowRayCount, s);
            }
            if (!reserveResolve(rec.ctx(), shadowCount, frame.maxCutouts, shadowScratch)) {
                return false;
            }
        }
    }

    shadeParams.lighting[1] = batch;
    shadeParams.lighting[2] = shadeTables.linkWords();
    shadeParams.lighting[3] = lightCount;
    shadeParams.pixels[0] = static_cast<std::uint32_t>(rect.x);
    shadeParams.pixels[1] = static_cast<std::uint32_t>(rect.y);
    shadeParams.pixels[2] = static_cast<std::uint32_t>(rect.width);
    shadeParams.bounce[1] = limit;

    const auto shadeBindingsOf = [&](const gpu::GpuBuffer& bounceRays,
                                     const gpu::GpuBuffer& bounceValues,
                                     const gpu::GpuBuffer& bounceIds,
                                     const gpu::GpuBuffer& paths,
                                     const gpu::GpuBuffer& nextRays,
                                     const gpu::GpuBuffer& nextPaths) {
        return std::array<gpu::GpuBinding, 20> {
            gpu::GpuBinding::storage(0, bounceRays),
            gpu::GpuBinding::storage(1, bounceValues),
            gpu::GpuBinding::storage(2, bounceIds),
            gpu::GpuBinding::storage(3, tables.addresses.buffer()),
            gpu::GpuBinding::storage(4, *color),
            gpu::GpuBinding::storage(5, *depth),
            gpu::GpuBinding::storage(6, *primId),
            gpu::GpuBinding::storage(7, *instanceId),
            gpu::GpuBinding::storage(8, tables.materials.buffer()),
            gpu::GpuBinding::storage(9, tables.textureBindings.buffer()),
            gpu::GpuBinding::storage(10, tables.images.buffer()),
            gpu::GpuBinding::storage(11, tables.projections.buffer()),
            gpu::GpuBinding::storage(12, tables.lights.buffer()),
            gpu::GpuBinding::storage(13, *shadowRays),
            gpu::GpuBinding::storage(14, *contributions),
            gpu::GpuBinding::storage(15, *shadowLights),
            gpu::GpuBinding::storage(16, paths),
            gpu::GpuBinding::storage(17, nextRays),
            gpu::GpuBinding::storage(18, nextPaths),
            gpu::GpuBinding::storage(19, *liveBounces),
        };
    };
    const std::array<gpu::GpuBinding, 3> lightsBindings = {
        gpu::GpuBinding::storage(0, *color),
        gpu::GpuBinding::storage(1, *contributions),
        gpu::GpuBinding::storage(2, *blocked),
    };
    const KernelShadeLightsParams lightsParams { static_cast<std::uint32_t>(count), batch, { 0u, 0u } };
    const bool shadowLinked = shadeTables.occlusionFilter(nullptr).casterMasks != nullptr;
    const std::uint32_t linkWords = shadeTables.linkWords();
    // Shades a bounce's hits into the colour, every light batch, and on the first queues each path's next ray.
    const auto shade = [&](KernelShadeParams params, const std::array<gpu::GpuBinding, 20>& shadeBindings) -> bool {
        std::uint32_t first = 0;

        do {
            params.lighting[0] = first;
            if (!rec.reserve(count)) {
                return false;
            }
            if (gpu::GpuStatus s = rec.ctx().dispatchBindings(shadeKernel, shadeBindings, std::as_bytes(std::span(&params, 1)), threads); !s) {
                return fail("WavefrontGpu could not shade", s);
            }
            if (batch == 0) {
                break;
            }
            rec.ctx().computeBarrier();

            const std::uint32_t shadows = static_cast<std::uint32_t>(shadowCount);

            if (!rec.reserve(shadowCount)) {
                return false;
            }
            if (resolveShadows) {
                KernelResolveParams shadowParams = resolveParams;

                shadowParams.flags = kKernelResolveShadow | (shadowLinked ? kKernelResolveShadowLinked : 0u);
                if (gpu::GpuStatus s = tracer.recordIntersect(rec.ctx(), *shadowRays, shadows, *shadowHitValues, *shadowHitIds); !s) {
                    return fail("WavefrontGpu could not trace shadow rays", s);
                }
                rec.ctx().computeBarrier();
                if (!recordResolve(rec, shadowScratch, shadowParams, shadowCount, frame.maxCutouts, *shadowRays, *shadowHitValues, *shadowHitIds, *blocked, *shadowLights, *contributions)) {
                    return false;
                }
            } else {
                const gpu::GpuStatus traced = shadowLinked
                    ? tracer.recordOccludedFiltered(rec.ctx(), *shadowRays, shadows, *blocked, *shadowLights, tables.linkMasks.buffer(), 2 * linkWords, linkWords)
                    : tracer.recordOccluded(rec.ctx(), *shadowRays, shadows, *blocked);

                if (!traced) {
                    return fail("WavefrontGpu could not trace shadow rays", traced);
                }
                rec.ctx().computeBarrier();
            }
            if (!rec.reserve(count)) {
                return false;
            }
            if (gpu::GpuStatus s = rec.ctx().dispatchBindings(shadeLightsKernel, lightsBindings, std::as_bytes(std::span(&lightsParams, 1)), threads); !s) {
                return fail("WavefrontGpu could not add the lights", s);
            }
            rec.ctx().computeBarrier();
            first += batch;
        } while (first < lightCount);
        rec.ctx().computeBarrier();

        return true;
    };

    {
        const utilityProfile::Scope timed(utilityProfile::Stage::Shade);

        if (!shade(shadeParams, shadeBindingsOf(*rays, *hitValues, *hitIds, *rays, *queueRays[0], *queuePaths[0]))) {
            return false;
        }
    }

    KernelResolveParams bounceResolve {};

    bounceResolve.linkWords = shadeTables.linkWords();
    // Bounce 1 goes ahead unread, so that a scene of one bounce waits only for the read after it.
    std::uint32_t ran = 0;
    std::optional<utilityProfile::Scope> timedBounces;

    if (limit > 0) {
        timedBounces.emplace(utilityProfile::Stage::Bounces);
    }
    for (std::uint32_t bounce = 1; bounce <= limit; ++bounce) {
        const std::size_t from = (bounce - 1) % 2;
        const std::size_t to = bounce % 2;
        KernelShadeParams bounceParams = shadeParams;

        if (bounce > 1) {
            const std::uint32_t* live = nullptr;

            if (!readFlags(rec, *liveBounces, bounce - 1, 1, live)) {
                return false;
            }
            if (live[0] == 0) {
                break;
            }
        }
        if (!rec.reserve(count)) {
            return false;
        }
        // A path that has ended has a ray the tracer misses, and which the shading ends again.
        if (gpu::GpuStatus s = tracer.recordIntersect(rec.ctx(), *queueRays[from], static_cast<std::uint32_t>(count), *bounceHitValues, *bounceHitIds); !s) {
            return fail("WavefrontGpu could not trace bounced rays", s);
        }
        rec.ctx().computeBarrier();
        if (holes && !recordResolve(rec, scratch, bounceResolve, count, frame.maxCutouts, *queueRays[from], *bounceHitValues, *bounceHitIds, *rays, *rays, *rays)) {
            return false;
        }
        bounceParams.bounce[0] = bounce;
        if (!shade(bounceParams, shadeBindingsOf(*queueRays[from], *bounceHitValues, *bounceHitIds, *queuePaths[from], *queueRays[to], *queuePaths[to]))) {
            return false;
        }
        ran = bounce;
    }
    timedBounces.reset();
    if (deviceColor) {
        const utilityProfile::Scope timed(utilityProfile::Stage::FilmAccumulate);

        if (gpu::GpuStatus s = deviceColor->recordDeviceBatch(rec.ctx(), *hitIds, *color, *weights, rect.x, rect.y, rect.width, rect.height, wavefrontShowsBackground(frame, shadeTables)); !s) {
            return fail("WavefrontGpu could not write a batch's colours to the film", s);
        }
    }
    if (deviceAovs) {
        const utilityProfile::Scope timed(utilityProfile::Stage::FilmAccumulate);

        if (gpu::GpuStatus s = deviceAovs->recordBatch(rec.ctx(), *hitIds, *weights, *depth, *primId, *instanceId, rect.y, rect.height); !s) {
            return fail("WavefrontGpu could not keep a batch's depth and ids on the device", s);
        }
    }

    // Nothing of a batch with no AOV to hand over is read on the host but the bounce flags, which only tell
    // bouncesTraced() how deep the rays went: it is left running, and counts the bounces that ran.
    const bool defers = frame.passesMayRun && !aovs.any();
    // The geometry array comes first in the hit ids, as Kernels/UtilityTypes.slang lays them out.
    const struct {
        bool wanted;
        const gpu::GpuBuffer* source;
        gpu::GpuBuffer* target;
        VkDeviceSize bytes;
    } copies[] = {
        { aovs.any(), hitIds, &readback.hitGeometry, count * sizeof(std::uint32_t) },
        { aovs.any(), weights, &readback.weight, count * sizeof(float) },
        { aovs.color, color, &readback.color, count * 4 * sizeof(float) },
        { aovs.depth, depth, &readback.depth, count * sizeof(float) },
        { aovs.primId, primId, &readback.primId, count * sizeof(std::int32_t) },
        { aovs.instanceId, instanceId, &readback.instanceId, count * sizeof(std::int32_t) },
        // Which of the bounces that ran had a path to trace.
        { ran > 0 && !defers, liveBounces, &flagReadback, ran * sizeof(std::uint32_t) },
    };

    {
        // Only the AOVs count as read back; the bounce flags are a few words every batch needs.
        std::optional<utilityProfile::Scope> timed;

        if (aovs.any()) {
            timed.emplace(utilityProfile::Stage::Readback);
        }
        for (const auto& copy : copies) {
            if (!copy.wanted) {
                continue;
            }
            if (gpu::GpuStatus s = rec.ctx().copyBuffer(*copy.source, 0, *copy.target, 0, copy.bytes); !s) {
                return fail("WavefrontGpu could not read a batch back", s);
            }
        }
    }
    if (defers) {
        deepest = ran;

        return rec.defer("WavefrontGpu could not render a batch of " + rayCount) && throttlePasses(frame, rec.stop());
    }
    if (!rec.finish("WavefrontGpu could not render a batch of " + rayCount)) {
        return false;
    }
    deepest = 0;
    for (std::uint32_t bounce = ran; bounce > 0; --bounce) {
        if (static_cast<const std::uint32_t*>(flagReadback.mapped())[bounce - 1] != 0) {
            deepest = bounce;
            break;
        }
    }
    if (!aovs.any()) {
        return true;
    }

    WavefrontTile tile;

    tile.rect = rect;
    tile.hitGeometry = static_cast<const std::uint32_t*>(readback.hitGeometry.mapped());
    tile.weight = static_cast<const float*>(readback.weight.mapped());
    tile.color = aovs.color ? static_cast<const float*>(readback.color.mapped()) : nullptr;
    tile.depth = aovs.depth ? static_cast<const float*>(readback.depth.mapped()) : nullptr;
    tile.primId = aovs.primId ? static_cast<const std::int32_t*>(readback.primId.mapped()) : nullptr;
    tile.instanceId = aovs.instanceId ? static_cast<const std::int32_t*>(readback.instanceId.mapped()) : nullptr;
    tile.background = wavefrontShowsBackground(frame, shadeTables);

    const utilityProfile::Scope timed(utilityProfile::Stage::WriteTile);

    film.writeTile(tile);

    return true;
}

WavefrontGpu::WavefrontGpu(gpu::GpuKernelRegistry& registry,
                           gpu::GpuBufferPool& pool,
                           const std::shared_ptr<void>& runtimeHold,
                           gpu::GpuPipelineCache* pipelines)
    : _imp(new Implementation(registry, pool, runtimeHold, pipelines))
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
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, rayGenUvDesc(), kernel); !s) {
        imp.unusable = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu could not create its UV ray generation kernel: " + s.message };

        return;
    }
    imp.rayGenUvKernel = std::move(kernel);
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, shadeSurfaceDesc(), kernel); !s) {
        imp.unusable = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu could not create its shading kernel: " + s.message };

        return;
    }
    imp.shadeKernel = std::move(kernel);
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, shadeLightsDesc(), kernel); !s) {
        imp.unusable = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu could not create its lighting kernel: " + s.message };

        return;
    }
    imp.shadeLightsKernel = std::move(kernel);
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, resolveHitsDesc(), kernel); !s) {
        imp.unusable = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu could not create its hole resolving kernel: " + s.message };

        return;
    }
    imp.resolveKernel = std::move(kernel);
    // Without it the executor still renders in Ray mode, and a frame in Raster mode fails.
    if (missingRasterSupport(imp.device.info()).empty() && gpu::GpuKernel::create(imp.device, rasterHitsDesc(), kernel)) {
        imp.rasterHitsKernel = std::move(kernel);
    }
    // Made with the executor rather than with the first film on the device, so that warm() compiles them too.
    imp.filmKernelsTried = true;

    gpu::GpuStatus filmStatus;

    imp.filmKernels = makeUtilityFilmKernels(imp.device, &filmStatus);
    imp.usable = true;
}

bool
WavefrontGpu::waitForWarmUps(const gpu::GpuDevice& device,
                             std::chrono::milliseconds timeout)
{
    WarmUpThreads& threads = warmUpThreads();
    std::unique_lock<std::mutex> l(threads.mutex);

    return threads.ended.wait_for(l, timeout, [&threads, &device]() {
        const auto found = threads.running.find(&device);

        return found == threads.running.end() || found->second == 0;
    });
}

WavefrontGpu::~WavefrontGpu()
{
}

std::string
WavefrontGpu::missingSupport(const gpu::GpuRuntime& runtime)
{
    return GpuTracer::missingRayQuerySupport(runtime.device());
}

std::string
WavefrontGpu::missingRasterSupport(const gpu::GpuDeviceInfo& info)
{
    if (!info.rasterVisibility) {
        return "the GPU cannot draw visibility buffers";
    }
    if (!info.rasterFloatBlend) {
        return "the GPU cannot blend float attachments";
    }

    return std::string();
}

std::string
WavefrontGpu::missingRasterSupport() const
{
    const std::string missing = missingRasterSupport(_imp->device.info());

    if (!missing.empty() || _imp->rasterHitsKernel) {
        return missing;
    }

    return "the GPU could not make the Raster mode kernel";
}

void
WavefrontGpu::limitRasterTile(std::uint32_t texels)
{
    _imp->rasterTileLimit = texels;
}

void
WavefrontGpu::alwaysTraceRasterFrames(bool on)
{
    _imp->alwaysTraceRaster = on;
}

bool
WavefrontGpu::drewUntraced() const
{
    return _imp->untraced;
}

bool
WavefrontGpu::render(const UtilityScene::Snapshot& snapshot,
                     const WavefrontFrame& frame,
                     WavefrontFilm& film,
                     const std::atomic<bool>& stop,
                     int threads)
{
    Implementation& imp = *_imp;

    wavefrontRequireUnwrap(frame);
    imp.outcome = Tracer::Outcome();
    imp.sceneFailed = false;
    imp.submissions = 0;
    imp.untraced = false;
    resetBouncesTraced();
    if (!imp.usable) {
        imp.outcome = imp.unusable;

        return false;
    }
    if (!imp.warm(stop)) {
        return false;
    }
    // Another scene rebuilds the structures the passes left running trace.
    if ((imp.deferredVersion != snapshot.version) && !imp.settlePasses()) {
        return false;
    }
    imp.deferredVersion = snapshot.version;
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::ShadeTables);

        if (!imp.shadeTables.update(snapshot, &stop)) {
            return imp.stopped();
        }
    }

    // A Raster frame takes its camera hits from the visibility buffer, so only cutouts, shadows and bounces
    // trace; without them the structures, whose builds outweigh the draws, are left for a frame that does. When
    // no light or bounce changes a pixel, its cutouts are left undrawn by the visibility pass instead.
    const bool unlit = imp.shadeTables.shadesUnlit()
        || (imp.shadeTables.lightCount() == 0 && wavefrontBounceLimit(frame, imp.shadeTables.hasTransmission()) == 0);

    imp.untraced = frame.raster && frame.mapping == WavefrontMapping::Camera && unlit && !imp.alwaysTraceRaster;

    const bool traces = !frame.raster || (!imp.untraced && (imp.shadeTables.hasHoles() || imp.shadeTables.lightCount() > 0 || wavefrontBounceLimit(frame, imp.shadeTables.hasTransmission()) > 0));
    bool updated = false;

    // Switching between a tracing and a raster-only update may rebuild what the passes left running read.
    if ((imp.deferredTraces != traces) && !imp.settlePasses()) {
        return false;
    }
    imp.deferredTraces = traces;
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::TracerUpdate);

        updated = traces ? imp.tracer.update(snapshot, threads, &stop) : imp.tracer.updateForRaster(snapshot, &stop);
    }
    if (!updated) {
        imp.outcome = imp.tracer.status();
        if (imp.outcome.status == Tracer::Status::Ok) {
            return imp.stopped();
        }
        imp.sceneFailed = imp.outcome.status != Tracer::Status::Stopped;

        return false;
    }
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::Uploads);

        if (!imp.uploadTables(snapshot.version, stop)) {
            return false;
        }
        if (frame.mapping == WavefrontMapping::UvUnwrap) {
            if (stop.load(std::memory_order_relaxed)) {
                return imp.stopped();
            }
            if (!imp.uploadUnwrap(frame.unwrap)) {
                return false;
            }
        } else if (!imp.uploadCameraSteps(frame.cameraSteps)) {
            return false;
        }
    }
    if (frame.raster && frame.mapping != WavefrontMapping::Camera) {
        imp.outcome = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu draws only the Camera projection in Raster mode" };

        return false;
    }
    if (frame.raster && !imp.rasterHitsKernel) {
        imp.outcome = Tracer::Outcome { Tracer::Status::Unsupported, "WavefrontGpu cannot draw in Raster mode: " + missingRasterSupport() };

        return false;
    }

    const WavefrontRect& window = frame.window;
    UtilityRenderBuffer* deviceColor = imp.deviceColorOf(film);
    UtilityDeviceAovs* deviceAovs = nullptr;
    int row = 0;

    while (row < window.height) {
        if (stop.load(std::memory_order_relaxed)) {
            return imp.stopped();
        }

        Implementation::Recorder rec(imp, stop, imp.submitWorkFor(frame));

        if (!rec.begin()) {
            return false;
        }
        if (deviceColor && (row == 0)) {
            bool halted = false;
            const auto record = [&rec, &halted](std::uint64_t work) -> gpu::GpuRecordContext* {
                if (!rec.reserve(work)) {
                    halted = true;

                    return nullptr;
                }

                return &rec.ctx();
            };

            bool superseded = false;
            const utilityProfile::Scope timed(utilityProfile::Stage::FilmBegin);

            if (gpu::GpuStatus s = deviceColor->beginDeviceFrame(record, rec.work(), imp.registry, imp.pool, imp.filmKernels, imp.runtimeHold, &superseded); !s) {
                // The recorder has already said why: the frame was stopped or a submission failed.
                if (halted) {
                    return false;
                }
                if (superseded) {
                    return imp.stopped();
                }

                return imp.fail("WavefrontGpu could not hold the film on the device", s);
            }
            if (UtilityDeviceAovs* aovs = imp.deviceAovsOf(film, rec.ctx(), window)) {
                if (gpu::GpuStatus s = aovs->beginPass(rec.ctx(), imp.pool, imp.filmKernels, imp.runtimeHold, window.x, window.y, window.width, window.height); !s) {
                    return imp.fail("WavefrontGpu could not keep the depth and ids on the device", s);
                }
                deviceAovs = aovs;
            }
            rec.ctx().computeBarrier();
        }

        const std::uint64_t bindable = std::min(rec.ctx().maxScratchBytes(), rec.ctx().maxStorageBufferBytes()) / (kRayArrays * sizeof(float));
        // A row too wide for the memory or the latency cap still goes in one batch, so that any width renders.
        const std::uint64_t wanted = std::max(imp.batchRaysFor(frame), static_cast<std::uint64_t>(window.width));
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
        std::uint32_t deepest = 0;

        if (!imp.renderBatch(frame, rect, rec, film, deviceColor, deviceAovs, deepest)) {
            return false;
        }
        noteBounceTraced(deepest);
        row += rect.height;
    }
    if (deviceAovs) {
        deviceAovs->endPass();
    }

    return true;
}

bool
WavefrontGpu::finishPasses(const std::atomic<bool>& stop)
{
    return _imp->finishPasses(&stop);
}

std::size_t
WavefrontGpu::submissions() const
{
    return _imp->submissions;
}

std::size_t
WavefrontGpu::shadeTablesUploaded() const
{
    return _imp->shadeTablesUploaded;
}

std::size_t
WavefrontGpu::shadeTableSetsKept() const
{
    return _imp->shadeTableSetsKept;
}

Tracer::Stats
WavefrontGpu::tracerStats() const
{
    return _imp->tracer.stats();
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
