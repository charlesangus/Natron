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
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
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
static_assert(utility_raygen_storage_buffers == 2 && utility_shade_surface_storage_buffers == 20 && utility_shade_lights_storage_buffers == 3);
static_assert(utility_resolve_hits_storage_buffers == 15);
static_assert(sizeof(KernelPathState) == 32);
static_assert(sizeof(KernelLightRecord) == 144);
static_assert(sizeof(KernelGeometryRecord) == 40);
static_assert(sizeof(KernelMaterialRecord) == 144 && sizeof(KernelTextureBinding) == 112 && sizeof(KernelProjectionRecord) == 256);

const std::size_t kRayArrays = kUtilityRayArrays;
const std::size_t kHitArrays = kUtilityHitArrays;

// Bounds how long a stop waits for the batch in flight, which on an integrated GPU or lavapipe is the
// time of this many rays.
const std::uint64_t kMaxBatchRays = std::min<std::uint64_t>(kUtilityMaxDispatchRays, std::uint64_t(1) << 18);
// Lights sampled per shading pass. A full batch of rays then needs a few tens of megabytes of shadow
// rays, and more lights take more passes over the same hits rather than more memory.
const std::uint32_t kLightBatch = 4;
// Bounds the work, in kernel threads, one submission records beyond its first dispatch, so that neither many
// lights nor many cutout layers make a submission whose stop waits longer than that, or that runs into
// submitAndWait()'s time limit. A full batch with one light batch and no holes still takes one submission.
const std::uint64_t kMaxSubmitWork = 8 * kMaxBatchRays;

// Vulkan has no zero-sized buffer, and a scene without colours or triangles still binds all of them.
const VkDeviceSize kMinTableBytes = 16;
// The texels go up in submissions of at most this many bytes, the stop flag looked at between them, as
// a scene's textures can run to gigabytes.
const std::size_t kTexelUploadChunkBytes = std::size_t(64) << 20;

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
    // The device addresses of the tables at the KernelTableSlot slots, which ShadeSurface and ResolveHits
    // read through them rather than through bindings the storage buffer range would bound.
    gpu::GpuBufferHandle addresses;
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

    // A warm-up still compiling is left to end on its own: the GPU context waits for it before it destroys the
    // device.
    ~Implementation()
    {
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
     * @brief Records a batch's work into as many submissions as kMaxSubmitWork asks for, each submitted and
     * waited for before the next is recorded, the stop flag looked at between them. The contexts submitted
     * are kept until the recorder goes away, as they hold the scratch buffers later submissions bind.
     **/
    class Recorder {
    public:
        Recorder(Implementation& imp,
                 const std::atomic<bool>& stop)
            : _imp(imp)
            , _stop(stop)
        {
        }

        bool begin();
        gpu::GpuRecordContext& ctx() { return *_current; }
        // Makes room for `work` more kernel threads, submitting what is recorded first when it would take the
        // submission past kMaxSubmitWork. False when the frame was stopped or a submission failed.
        bool reserve(std::uint64_t work);
        // Submits what is recorded and waits for it, so that the host may read what it wrote, then goes on
        // recording. False when the frame was stopped or the submission failed.
        bool sync() { return next(); }
        bool finish(const std::string& what);

    private:
        bool next();

        Implementation& _imp;
        const std::atomic<bool>& _stop;
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
    bool renderBatch(const WavefrontFrame& frame,
                     const WavefrontRect& rect,
                     Recorder& rec,
                     WavefrontFilm& film,
                     UtilityRenderBuffer* deviceColor,
                     std::uint32_t& deepest);
    UtilityRenderBuffer* deviceColorOf(const WavefrontFilm& film);

    using Outcome = Tracer::Outcome;
    using Status = Tracer::Status;

    gpu::GpuKernelRegistry& registry;
    gpu::GpuBufferPool& pool;
    gpu::GpuDevice& device;
    GpuTracer tracer;
    std::shared_ptr<gpu::GpuKernel> rayGenKernel;
    std::shared_ptr<gpu::GpuKernel> rayGenUvKernel;
    std::shared_ptr<gpu::GpuKernel> shadeKernel;
    std::shared_ptr<gpu::GpuKernel> shadeLightsKernel;
    std::shared_ptr<gpu::GpuKernel> resolveKernel;
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
        // Made here rather than with the first film on the device, so that the film's kernels compile too.
        if (!filmKernelsTried) {
            filmKernelsTried = true;

            gpu::GpuStatus status;

            filmKernels = makeUtilityFilmKernels(device, &status);
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
    for (std::size_t first = 0; first < bytes; first += kTexelUploadChunkBytes) {
        if (stop.load(std::memory_order_relaxed)) {
            return stopped();
        }

        const std::size_t chunk = std::min(kTexelUploadChunkBytes, bytes - first);
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

    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(registry, ctx, &pool); !s) {
        return fail("WavefrontGpu could not create a record context", s);
    }
    // The largest buffer the pool hands out that a binding still takes whole.
    const std::uint64_t bindLimit = ctx->maxScratchBytes() ? ctx->maxScratchBytes() : ctx->maxStorageBufferBytes();
    const std::uint64_t texelVersion = shadeTables.texelVersion();
    gpu::GpuBufferHandle keptTexels;

    if (tables.texels && tables.texelVersion == texelVersion) {
        keptTexels = std::move(tables.texels);
    }
    tables = ShadeTableBuffers();

    const KernelShadeBuffers source = shadeTables.buffers();
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
        || !upload(*ctx, source.sts, source.stCount * 2 * sizeof(float), "texture coordinates", next.sts)
        || !upload(*ctx, source.normals, source.normalCount * 4 * sizeof(float), "normals", next.normals)
        || !upload(*ctx, source.lights, source.lightCount * sizeof(KernelLightRecord), "light records", next.lights)
        || !upload(*ctx, source.lightData, source.lightDataCount * sizeof(float), "light tables", next.lightData)
        || !upload(*ctx, source.linkMasks, source.linkMaskCount * sizeof(std::uint32_t), "light-link masks", next.linkMasks)) {
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
    tables.valid = true;

    return true;
}

bool
WavefrontGpu::Implementation::uploadUnwrap(const WavefrontUnwrapConstPtr& next)
{
    if (next == unwrap && unwrapGrid && unwrapTriangles && unwrapAddresses) {
        return true;
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
WavefrontGpu::Implementation::Recorder::begin()
{
    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(_imp.registry, _current, &_imp.pool); !s) {
        return _imp.fail("WavefrontGpu could not create a record context", s);
    }
    _recorded = 0;

    return true;
}

bool
WavefrontGpu::Implementation::Recorder::next()
{
    if (gpu::GpuStatus s = _current->submitAndWait(); !s) {
        return _imp.fail("WavefrontGpu could not render part of a batch", s);
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
    if (_recorded > 0 && _recorded + work > kMaxSubmitWork && !next()) {
        return false;
    }
    _recorded += work;

    return true;
}

bool
WavefrontGpu::Implementation::Recorder::finish(const std::string& what)
{
    if (gpu::GpuStatus s = _current->submitAndWait(); !s) {
        return _imp.fail(what, s);
    }

    return true;
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

bool
WavefrontGpu::Implementation::renderBatch(const WavefrontFrame& frame,
                                          const WavefrontRect& rect,
                                          Recorder& rec,
                                          WavefrontFilm& film,
                                          UtilityRenderBuffer* deviceColor,
                                          std::uint32_t& deepest)
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
        const std::array<gpu::GpuBinding, 5> bindings = {
            gpu::GpuBinding::storage(0, *rays), gpu::GpuBinding::storage(1, *weights), gpu::GpuBinding::storage(2, *hitValues),
            gpu::GpuBinding::storage(3, *hitIds), gpu::GpuBinding::storage(4, unwrapAddresses.buffer())
        };

        if (gpu::GpuStatus s = rec.ctx().dispatchBindings(rayGenUvKernel, bindings, std::as_bytes(std::span(&rayGenParams, 1)), threads); !s) {
            return fail("WavefrontGpu could not generate rays", s);
        }
    } else {
        const std::array<gpu::GpuBinding, 2> bindings = { gpu::GpuBinding::storage(0, *rays), gpu::GpuBinding::storage(1, *weights) };

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
    const bool holes = shadeTables.hasHoles();
    const bool transmission = shadeTables.hasTransmission();
    // Shadow rays then take closest hits, so that a hole or a transmissive surface lets them go on.
    const bool resolveShadows = holes || transmission;
    const std::uint32_t limit = wavefrontBounceLimit(frame, transmission);
    KernelResolveParams resolveParams {};
    ResolveScratch scratch;

    if ((limit > 0 || resolveShadows) && !reserveFlagReadback(std::max<std::size_t>(limit, 1))) {
        return false;
    }

    resolveParams.linkWords = shadeTables.linkWords();
    if (holes) {
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
    const std::uint32_t batch = static_cast<std::uint32_t>(std::min<std::uint64_t>({ lightCount, kLightBatch, std::max<std::uint64_t>(1, bindable / count) }));
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

    if (!shade(shadeParams, shadeBindingsOf(*rays, *hitValues, *hitIds, *rays, *queueRays[0], *queuePaths[0]))) {
        return false;
    }

    KernelResolveParams bounceResolve {};

    bounceResolve.linkWords = shadeTables.linkWords();
    // Bounce 1 goes ahead unread, so that a scene of one bounce waits only for the read after it.
    std::uint32_t ran = 0;

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
    if (deviceColor) {
        if (gpu::GpuStatus s = deviceColor->recordDeviceBatch(rec.ctx(), *hitIds, *color, *weights, rect.x, rect.y, rect.width, rect.height, wavefrontShowsBackground(frame, shadeTables)); !s) {
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
        { aovs.any(), weights, &readback.weight, count * sizeof(float) },
        { aovs.color, color, &readback.color, count * 4 * sizeof(float) },
        { aovs.depth, depth, &readback.depth, count * sizeof(float) },
        { aovs.primId, primId, &readback.primId, count * sizeof(std::int32_t) },
        { aovs.instanceId, instanceId, &readback.instanceId, count * sizeof(std::int32_t) },
        // Which of the bounces that ran had a path to trace.
        { ran > 0, liveBounces, &flagReadback, ran * sizeof(std::uint32_t) },
    };

    for (const auto& copy : copies) {
        if (!copy.wanted) {
            continue;
        }
        if (gpu::GpuStatus s = rec.ctx().copyBuffer(*copy.source, 0, *copy.target, 0, copy.bytes); !s) {
            return fail("WavefrontGpu could not read a batch back", s);
        }
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
    resetBouncesTraced();
    if (!imp.usable) {
        imp.outcome = imp.unusable;

        return false;
    }
    if (!imp.warm(stop)) {
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
    }

    const WavefrontRect& window = frame.window;
    UtilityRenderBuffer* deviceColor = imp.deviceColorOf(film);
    int row = 0;

    while (row < window.height) {
        if (stop.load(std::memory_order_relaxed)) {
            return imp.stopped();
        }

        Implementation::Recorder rec(imp, stop);

        if (!rec.begin()) {
            return false;
        }
        if (deviceColor && (row == 0)) {
            if (gpu::GpuStatus s = deviceColor->beginDeviceFrame(rec.ctx(), imp.registry, imp.pool, imp.filmKernels, imp.runtimeHold); !s) {
                return imp.fail("WavefrontGpu could not hold the film on the device", s);
            }
            rec.ctx().computeBarrier();
        }

        const std::uint64_t bindable = std::min(rec.ctx().maxScratchBytes(), rec.ctx().maxStorageBufferBytes()) / (kRayArrays * sizeof(float));
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
        std::uint32_t deepest = 0;

        if (!imp.renderBatch(frame, rect, rec, film, deviceColor, deepest)) {
            return false;
        }
        noteBounceTraced(deepest);
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
