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

#include "UtilityRenderBuffer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/imaging/hd/types.h>

#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

#ifdef NATRON_HAVE_GPU
#include <array>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>

#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/GpuContext.h"
#include "Engine/Nodes/Scene/Utility/UtilityProfile.h"
#include "Engine/PoolParallelFor.h"

#include "utility_film_aov_pack_spirv.h"
#include "utility_film_clear_spirv.h"
#include "utility_film_ids_spirv.h"
#include "utility_film_pack_spirv.h"
#include "utility_film_pick_spirv.h"
#include "utility_film_scatter_spirv.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::atomic<int> gLiveBuffers { 0 };
std::atomic<std::uint64_t> gDeviceDownloads { 0 };
std::atomic<std::uint64_t> gPickReadbacks { 0 };
std::atomic<std::uint64_t> gPickCopies { 0 };
std::mutex gConvergedMutex;
std::condition_variable gConvergedChanged;
std::uint64_t gConverged = 0;
std::uint64_t gConvergedWakes = 0;

// Device films hold RGBA floats.
const std::size_t kFilmPixelBytes = 4 * sizeof(float);

// Clear values arrive as whichever type the AOV's descriptor was given.
std::vector<double>
clearComponents(const VtValue& value)
{
    if (value.IsHolding<float>()) {
        return { value.UncheckedGet<float>() };
    }
    if (value.IsHolding<double>()) {
        return { value.UncheckedGet<double>() };
    }
    if (value.IsHolding<int>()) {
        return { static_cast<double>(value.UncheckedGet<int>()) };
    }
    if (value.IsHolding<GfVec2f>()) {
        const GfVec2f& v = value.UncheckedGet<GfVec2f>();

        return { v[0], v[1] };
    }
    if (value.IsHolding<GfVec3f>()) {
        const GfVec3f& v = value.UncheckedGet<GfVec3f>();

        return { v[0], v[1], v[2] };
    }
    if (value.IsHolding<GfVec4f>()) {
        const GfVec4f& v = value.UncheckedGet<GfVec4f>();

        return { v[0], v[1], v[2], v[3] };
    }
    TF_CODING_ERROR("A render buffer cannot be cleared with a %s", value.GetTypeName().c_str());

    return {};
}

template <class T>
void
writeComponent(std::uint8_t* destination,
               T value)
{
    std::memcpy(destination, &value, sizeof(T));
}

void
encodeComponent(HdFormat componentFormat,
                double value,
                std::uint8_t* destination)
{
    switch (componentFormat) {
    case HdFormatUNorm8:
        writeComponent<std::uint8_t>(destination, static_cast<std::uint8_t>(std::lround(std::clamp(value, 0., 1.) * 255.)));
        break;
    case HdFormatSNorm8:
        writeComponent<std::int8_t>(destination, static_cast<std::int8_t>(std::lround(std::clamp(value, -1., 1.) * 127.)));
        break;
    case HdFormatFloat16:
        writeComponent<GfHalf>(destination, GfHalf(static_cast<float>(value)));
        break;
    case HdFormatFloat32:
        writeComponent<float>(destination, static_cast<float>(value));
        break;
    case HdFormatInt32:
        writeComponent<std::int32_t>(destination, static_cast<std::int32_t>(value));
        break;
    default:
        TF_CODING_ERROR("A render buffer cannot be cleared in format %d", static_cast<int>(componentFormat));
        break;
    }
}
#ifdef NATRON_HAVE_GPU
// Kernels/FilmClear.slang, FilmScatter.slang, FilmPack.slang and FilmPick.slang declare these push constants.
struct KernelFilmClearParams {
    std::uint32_t value[4];
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t padding0;
    std::uint32_t padding1;
};

struct KernelFilmScatterParams {
    std::uint32_t clearValue[4];
    std::uint32_t filmWidth;
    std::uint32_t filmHeight;
    std::int32_t x;
    std::int32_t y;
    std::uint32_t width;
    std::uint32_t count;
    std::uint32_t background;
    std::uint32_t padding1;
};

struct KernelFilmPackParams {
    std::uint32_t filmWidth;
    std::uint32_t filmHeight;
    std::int32_t x1;
    std::int32_t y1;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t nComps;
    std::uint32_t outOffset;
    std::uint32_t outRowStride;
    std::uint32_t padding0;
    std::uint32_t padding1;
    std::uint32_t padding2;
};

struct KernelFilmPickParams {
    std::uint32_t first;
    std::uint32_t count;
    std::uint32_t fresh;
    std::uint32_t pixels;
    std::uint32_t missDepth;
    std::uint32_t missPrimId;
    std::uint32_t missInstanceId;
    std::uint32_t padding0;
};

// Kernels/FilmIds.slang and FilmAovPack.slang declare these push constants.
struct KernelFilmIdsParams {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mode;
    std::uint32_t padding0;
};

struct KernelFilmAovPackParams {
    float zColumn[4];
    float wColumn[4];
    std::uint32_t renderWidth;
    std::uint32_t renderHeight;
    std::int32_t picksX;
    std::int32_t picksY;
    std::uint32_t picksWidth;
    std::uint32_t picksHeight;
    std::int32_t x1;
    std::int32_t y1;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t nComps;
    std::uint32_t outOffset;
    std::uint32_t outRowStride;
    std::uint32_t mode;
    std::uint32_t padding0;
    std::uint32_t padding1;
};

// Kernels/FilmPick.slang's planes of a word a pixel: weight, hit, depth, prim id, instance id.
const std::size_t kFilmPickPlanes = 5;
const std::size_t kFilmPickHitPlane = 1;

// Kernels/FilmIdsTable.slang's id table: four counter words, the entries taken, the pairs that found no room and
// the pixels whose pair it does not hold; kIdSlots slots of two words; kIdCapacity entries of two words.
const std::size_t kIdCapacity = 65536;
const std::size_t kIdSlots = 2 * kIdCapacity;
const std::size_t kIdSlotsWord = 4;
const std::size_t kIdEntriesWord = kIdSlotsWord + 2 * kIdSlots;
const std::size_t kIdTableWords = kIdEntriesWord + 2 * kIdCapacity;

static_assert(sizeof(KernelFilmClearParams) == utility_film_clear_push_bytes);
static_assert(sizeof(KernelFilmScatterParams) == utility_film_scatter_push_bytes);
static_assert(sizeof(KernelFilmPackParams) == utility_film_pack_push_bytes);
static_assert(sizeof(KernelFilmPickParams) == utility_film_pick_push_bytes);
static_assert(sizeof(KernelFilmIdsParams) == utility_film_ids_push_bytes);
static_assert(sizeof(KernelFilmAovPackParams) == utility_film_aov_pack_push_bytes);
static_assert(utility_film_ids_storage_buffers == 2 && utility_film_aov_pack_storage_buffers == 4);
static_assert(utility_film_clear_storage_buffers == 2 && utility_film_scatter_storage_buffers == 5 && utility_film_pack_storage_buffers == 2 && utility_film_pick_storage_buffers == 6);

gpu::GpuStatus
failure(const std::string& message)
{
    return gpu::GpuStatus { VK_ERROR_UNKNOWN, message };
}

// The rows of a film one dispatch binds: all of them when its accumulation, the larger of its buffers, fits the
// storage buffer range, else the most that fit and start every band on the device's offset alignment. 0 when not
// even one such band fits.
std::uint32_t
filmBandRows(const gpu::GpuRecordContext& context,
             std::uint32_t width,
             std::uint32_t height)
{
    const std::uint64_t pixelRowBytes = static_cast<std::uint64_t>(width) * kFilmPixelBytes;
    const std::uint64_t fit = context.maxStorageBufferBytes() / (2 * pixelRowBytes);

    if (fit >= height) {
        return height;
    }

    const std::uint64_t alignment = context.device().info().minStorageBufferOffsetAlignment;
    const std::uint64_t quantum = alignment / std::gcd(alignment, pixelRowBytes);

    return static_cast<std::uint32_t>((fit / quantum) * quantum);
}

// Unmeasured, so a film pixel is charged as a whole traced ray: a cheaper charge could start a large film in
// one submission that a stop waits on.
const std::uint64_t kFilmClearPixelsPerWork = 1;
const std::uint64_t kFilmCopyPixelsPerWork = 1;

// The rows of a band one clear dispatch covers: as many as maxThreads threads clear, starting every chunk on the
// device's offset alignment, and never fewer than that alignment asks for.
std::uint32_t
filmClearRows(const gpu::GpuRecordContext& context,
              std::uint32_t width,
              std::uint32_t bandRows,
              std::uint64_t maxThreads)
{
    const std::uint64_t pixelRowBytes = static_cast<std::uint64_t>(width) * kFilmPixelBytes;
    const std::uint64_t alignment = context.device().info().minStorageBufferOffsetAlignment;
    const std::uint64_t quantum = alignment / std::gcd(alignment, pixelRowBytes);
    const std::uint64_t rows = std::max(quantum, ((maxThreads / width) / quantum) * quantum);

    return static_cast<std::uint32_t>(std::min<std::uint64_t>(rows, bandRows));
}

gpu::GpuStatus
filmTooWide(std::uint32_t width)
{
    return failure("The Utility renderer's film, " + std::to_string(width) + " pixels wide, has rows too wide for the GPU's storage buffer range");
}

gpu::GpuBinding
filmPixelsBand(std::uint32_t binding,
               const gpu::GpuBuffer& pixels,
               std::uint32_t width,
               std::uint32_t firstRow,
               std::uint32_t rows)
{
    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(width) * kFilmPixelBytes;

    return gpu::GpuBinding::storage(binding, pixels, firstRow * rowBytes, rows * rowBytes);
}

gpu::GpuBinding
filmAccumulationBand(std::uint32_t binding,
                     const gpu::GpuBuffer& accumulation,
                     std::uint32_t width,
                     std::uint32_t firstRow,
                     std::uint32_t rows)
{
    const VkDeviceSize rowBytes = 2 * static_cast<VkDeviceSize>(width) * kFilmPixelBytes;

    return gpu::GpuBinding::storage(binding, accumulation, firstRow * rowBytes, rows * rowBytes);
}

gpu::GpuStatus
makeKernel(gpu::GpuDevice& device,
           const gpu::GpuKernelDesc& desc,
           std::shared_ptr<gpu::GpuKernel>* out)
{
    std::unique_ptr<gpu::GpuKernel> kernel;

    if (gpu::GpuStatus s = gpu::GpuKernel::create(device, desc, kernel); !s) {
        return s;
    }
    *out = std::move(kernel);

    return gpu::GpuStatus();
}
#endif // NATRON_HAVE_GPU
} // anon namespace

#ifdef NATRON_HAVE_GPU
struct UtilityFilmKernels {
    std::shared_ptr<gpu::GpuKernel> clear;
    std::shared_ptr<gpu::GpuKernel> scatter;
    std::shared_ptr<gpu::GpuKernel> pack;
    std::shared_ptr<gpu::GpuKernel> pick;
    std::shared_ptr<gpu::GpuKernel> ids;
    std::shared_ptr<gpu::GpuKernel> aovPack;
};

class UtilityDeviceFilm {
public:
    UtilityDeviceFilm(gpu::GpuKernelRegistry& registry,
                      gpu::GpuBufferPool& pool,
                      const std::shared_ptr<const UtilityFilmKernels>& kernels,
                      const std::shared_ptr<void>& runtimeHold,
                      unsigned int width,
                      unsigned int height)
        : registry(registry)
        , pool(pool)
        , device(registry.device())
        , width(width)
        , height(height)
        , _kernels(kernels)
        , _runtimeHold(runtimeHold)
    {
    }

    // What a user of the film keeps while it reads or writes the pixels: the hold keeps registry, pool and device
    // valid meanwhile, even once the film was released.
    struct Lease {
        std::shared_ptr<void> runtimeHold;
        gpu::GpuBufferHandle pixels;
        // Twice the pixels' size, laid out as Kernels/FilmClear.slang says.
        gpu::GpuBufferHandle accumulation;
        std::shared_ptr<const UtilityFilmKernels> kernels;
    };

    // False once the film was released.
    bool lease(Lease* out) const
    {
        const std::lock_guard<std::mutex> l(_mutex);

        if (!_runtimeHold) {
            return false;
        }
        out->runtimeHold = _runtimeHold;
        out->pixels = _pixels;
        out->accumulation = _accumulation;
        out->kernels = _kernels;

        return true;
    }

    bool released() const
    {
        const std::lock_guard<std::mutex> l(_mutex);

        return !_runtimeHold;
    }

    // A film only read from, a pass's copy, needs no accumulation.
    gpu::GpuStatus allocate(VkDeviceSize bytes,
                            bool accumulates = true)
    {
        const std::lock_guard<std::mutex> l(_mutex);

        if (gpu::GpuStatus s = pool.acquire(bytes, _pixels); !s) {
            return s;
        }

        return accumulates ? pool.acquire(2 * bytes, _accumulation) : gpu::GpuStatus();
    }

    // Takes a share of pixels, which their writer no longer writes, as the film's read-only pixels.
    void adopt(const gpu::GpuBufferHandle& pixels)
    {
        const std::lock_guard<std::mutex> l(_mutex);

        _pixels = pixels;
    }

    // Lets go of the pixels and of the runtime, which a holder of the film no longer keeps from recovering.
    void release()
    {
        // Outside the lock, and in reverse order: the pool's buffer goes back before the hold that keeps the pool.
        Lease dropped;
        {
            const std::lock_guard<std::mutex> l(_mutex);
            std::swap(dropped.runtimeHold, _runtimeHold);
            std::swap(dropped.pixels, _pixels);
            std::swap(dropped.accumulation, _accumulation);
            std::swap(dropped.kernels, _kernels);
        }
    }

    void releaseIfLost()
    {
        bool lost = false;
        {
            const std::lock_guard<std::mutex> l(_mutex);
            lost = _runtimeHold && device.isLost();
        }
        if (lost) {
            release();
        }
    }

    // Valid only while the film is not released, or under a lease.
    gpu::GpuKernelRegistry& registry;
    gpu::GpuBufferPool& pool;
    gpu::GpuDevice& device;
    const unsigned int width;
    const unsigned int height;

private:
    mutable std::mutex _mutex;
    std::shared_ptr<const UtilityFilmKernels> _kernels;
    std::shared_ptr<void> _runtimeHold;
    gpu::GpuBufferHandle _pixels;
    gpu::GpuBufferHandle _accumulation;
};

namespace {
struct FilmRegistry {
    std::mutex mutex;
    std::vector<std::weak_ptr<UtilityDeviceFilm>> films;
};

FilmRegistry&
filmRegistry()
{
    static FilmRegistry* registry = new FilmRegistry;

    return *registry;
}

void
registerFilm(const std::shared_ptr<UtilityDeviceFilm>& film)
{
    static std::once_flag releaser;

    std::call_once(releaser, []() { GpuContext::addLossReleaser(&releaseLostUtilityDeviceFilms); });

    FilmRegistry& registry = filmRegistry();
    const std::lock_guard<std::mutex> l(registry.mutex);

    registry.films.erase(std::remove_if(registry.films.begin(), registry.films.end(), [](const std::weak_ptr<UtilityDeviceFilm>& f) { return f.expired(); }), registry.films.end());
    registry.films.push_back(film);
}
} // anon namespace

void
releaseLostUtilityDeviceFilms()
{
    std::vector<std::shared_ptr<UtilityDeviceFilm>> films;
    {
        FilmRegistry& registry = filmRegistry();
        const std::lock_guard<std::mutex> l(registry.mutex);

        for (const std::weak_ptr<UtilityDeviceFilm>& weak : registry.films) {
            if (std::shared_ptr<UtilityDeviceFilm> film = weak.lock()) {
                films.push_back(std::move(film));
            }
        }
    }
    for (const std::shared_ptr<UtilityDeviceFilm>& film : films) {
        film->releaseIfLost();
    }
}

namespace {
gpu::GpuStatus
recordPack(const std::shared_ptr<UtilityDeviceFilm>& film,
           gpu::GpuRecordContext& context,
           int x1,
           int y1,
           int x2,
           int y2,
           int nComps,
           const gpu::GpuBuffer& out,
           std::uint64_t outOffset,
           std::uint32_t outRowStride)
{
    UtilityDeviceFilm::Lease lease;

    if (!film->lease(&lease)) {
        return failure("The Utility renderer's film was released with its GPU");
    }
    if (&context.device() != &film->device) {
        return failure("The Utility renderer's film is on another device");
    }
    if ((nComps < 1) || (x2 < x1) || (y2 < y1)) {
        return failure("The Utility renderer's film was asked for an empty or inverted window");
    }
    if ((x2 == x1) || (y2 == y1)) {
        return gpu::GpuStatus();
    }
    const std::uint64_t width = static_cast<std::uint64_t>(x2 - x1);
    const std::uint64_t height = static_cast<std::uint64_t>(y2 - y1);
    const std::uint64_t end = outOffset + (height - 1) * outRowStride + width * static_cast<std::uint64_t>(nComps);

    // The kernel indexes the output in 32 bits.
    if ((end * sizeof(float) > out.size()) || (end > std::numeric_limits<std::uint32_t>::max())) {
        return failure("The Utility renderer's film window does not fit the buffer it is copied to");
    }

    const std::uint32_t bandRows = filmBandRows(context, film->width, film->height);

    if (bandRows == 0) {
        return filmTooWide(film->width);
    }
    // Orders the copy after the frame's writes, which earlier submissions on the queue made.
    context.computeBarrier();
    for (std::uint32_t first = 0; first < film->height; first += bandRows) {
        const std::uint32_t rows = std::min(bandRows, film->height - first);
        const std::int64_t bandEnd = static_cast<std::int64_t>(first) + rows;
        // The first band and the last also write the zeros of the window's rows below and above the film.
        const std::int64_t lo = (first == 0) ? y1 : std::max<std::int64_t>(y1, first);
        const std::int64_t hi = (bandEnd == film->height) ? y2 : std::min<std::int64_t>(y2, bandEnd);

        if (lo >= hi) {
            continue;
        }

        const KernelFilmPackParams params = {
            film->width, rows, x1, static_cast<std::int32_t>(lo - static_cast<std::int64_t>(first)), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(hi - lo), static_cast<std::uint32_t>(nComps), static_cast<std::uint32_t>(outOffset + static_cast<std::uint64_t>(lo - y1) * outRowStride), outRowStride, 0, 0, 0
        };
        const std::array<gpu::GpuBinding, 2> bindings = { filmPixelsBand(0, lease.pixels.buffer(), film->width, first, rows), gpu::GpuBinding::storage(1, out) };

        if (gpu::GpuStatus s = context.dispatchBindings(lease.kernels->pack, bindings, std::as_bytes(std::span(&params, 1)), { params.width, params.height, 1 }); !s) {
            return s;
        }
    }
    // The lease, not the film, so that a release while the copy is pending leaves its buffer alive.
    context.hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(lease)));

    return gpu::GpuStatus();
}

// Copies the whole film into out, bytes long, waiting for the copy.
gpu::GpuStatus
downloadFilm(const std::shared_ptr<UtilityDeviceFilm>& film,
             void* out,
             std::size_t bytes)
{
    if (bytes != static_cast<std::size_t>(film->width) * film->height * kFilmPixelBytes) {
        return failure("The Utility renderer's film is not the size of the buffer it is read into");
    }

    UtilityDeviceFilm::Lease lease;

    if (!film->lease(&lease)) {
        return failure("The Utility renderer's film was released with its GPU");
    }

    std::unique_ptr<gpu::GpuRecordContext> context;
    gpu::GpuBuffer staging;
    gpu::GpuStatus status = gpu::GpuRecordContext::create(film->registry, context, &film->pool);

    if (status) {
        status = gpu::GpuBuffer::createStaging(film->device, bytes, gpu::kGpuBufferDefaultUsage, staging);
    }
    // A copy, not the pack kernel: the staging buffer, as large as the film, may be over the storage buffer range.
    if (status) {
        status = context->copyBuffer(lease.pixels.buffer(), 0, staging, 0, bytes);
    }
    if (status) {
        status = context->submitAndWait();
    }
    if (status) {
        std::memcpy(out, staging.mapped(), bytes);
    }

    return status;
}

// A copy of the film's pixels in a film of its own, made once the frame's writes so far are done.
gpu::GpuStatus
copyFilm(const std::shared_ptr<UtilityDeviceFilm>& film,
         std::shared_ptr<UtilityDeviceFilm>* out)
{
    UtilityDeviceFilm::Lease lease;

    if (!film->lease(&lease)) {
        return failure("The Utility renderer's film was released with its GPU");
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(film->width) * film->height * kFilmPixelBytes;
    std::shared_ptr<UtilityDeviceFilm> copy = std::make_shared<UtilityDeviceFilm>(film->registry, film->pool, lease.kernels, lease.runtimeHold, film->width, film->height);
    UtilityDeviceFilm::Lease copyLease;
    std::unique_ptr<gpu::GpuRecordContext> context;

    if (gpu::GpuStatus s = copy->allocate(bytes, false); !s) {
        return s;
    }
    if (!copy->lease(&copyLease)) {
        return failure("The Utility renderer's film was released with its GPU");
    }
    if (gpu::GpuStatus s = gpu::GpuRecordContext::create(film->registry, context, &film->pool); !s) {
        return s;
    }
    context->computeBarrier();
    if (gpu::GpuStatus s = context->copyBuffer(lease.pixels.buffer(), 0, copyLease.pixels.buffer(), 0, bytes); !s) {
        return s;
    }
    if (gpu::GpuStatus s = context->submitAndWait(); !s) {
        return s;
    }
    registerFilm(copy);
    *out = std::move(copy);

    return gpu::GpuStatus();
}
} // anon namespace

std::shared_ptr<const UtilityFilmKernels>
makeUtilityFilmKernels(gpu::GpuDevice& device,
                       gpu::GpuStatus* status)
{
    std::shared_ptr<UtilityFilmKernels> kernels = std::make_shared<UtilityFilmKernels>();
    const gpu::GpuKernelDesc clear {
        utility_film_clear_spirv, utility_film_clear_spirv_words, utility_film_clear_entry, utility_film_clear_storage_buffers, utility_film_clear_push_bytes, { utility_film_clear_group_size[0], utility_film_clear_group_size[1], utility_film_clear_group_size[2] }
    };
    const gpu::GpuKernelDesc scatter {
        utility_film_scatter_spirv, utility_film_scatter_spirv_words, utility_film_scatter_entry, utility_film_scatter_storage_buffers, utility_film_scatter_push_bytes, { utility_film_scatter_group_size[0], utility_film_scatter_group_size[1], utility_film_scatter_group_size[2] }
    };
    const gpu::GpuKernelDesc pack {
        utility_film_pack_spirv, utility_film_pack_spirv_words, utility_film_pack_entry, utility_film_pack_storage_buffers, utility_film_pack_push_bytes, { utility_film_pack_group_size[0], utility_film_pack_group_size[1], utility_film_pack_group_size[2] }
    };
    const gpu::GpuKernelDesc pick {
        utility_film_pick_spirv, utility_film_pick_spirv_words, utility_film_pick_entry, utility_film_pick_storage_buffers, utility_film_pick_push_bytes, { utility_film_pick_group_size[0], utility_film_pick_group_size[1], utility_film_pick_group_size[2] }
    };
    const gpu::GpuKernelDesc ids {
        utility_film_ids_spirv, utility_film_ids_spirv_words, utility_film_ids_entry, utility_film_ids_storage_buffers, utility_film_ids_push_bytes, { utility_film_ids_group_size[0], utility_film_ids_group_size[1], utility_film_ids_group_size[2] }
    };
    const gpu::GpuKernelDesc aovPack {
        utility_film_aov_pack_spirv, utility_film_aov_pack_spirv_words, utility_film_aov_pack_entry, utility_film_aov_pack_storage_buffers, utility_film_aov_pack_push_bytes, { utility_film_aov_pack_group_size[0], utility_film_aov_pack_group_size[1], utility_film_aov_pack_group_size[2] }
    };

    *status = makeKernel(device, clear, &kernels->clear);
    if (status->ok()) {
        *status = makeKernel(device, scatter, &kernels->scatter);
    }
    if (status->ok()) {
        *status = makeKernel(device, pack, &kernels->pack);
    }
    if (status->ok()) {
        *status = makeKernel(device, pick, &kernels->pick);
    }
    if (status->ok()) {
        *status = makeKernel(device, ids, &kernels->ids);
    }
    if (status->ok()) {
        *status = makeKernel(device, aovPack, &kernels->aovPack);
    }

    return status->ok() ? kernels : std::shared_ptr<const UtilityFilmKernels>();
}

std::vector<std::shared_ptr<gpu::GpuKernel>>
utilityFilmKernelList(const UtilityFilmKernels& kernels)
{
    return { kernels.clear, kernels.scatter, kernels.pack, kernels.pick, kernels.ids, kernels.aovPack };
}

UtilityDeviceResource::UtilityDeviceResource()
{
}

UtilityDeviceResource::UtilityDeviceResource(const std::shared_ptr<UtilityDeviceFilm>& film)
    : _film(film)
{
}

gpu::GpuDevice*
UtilityDeviceResource::device() const
{
    return (_film && !_film->released()) ? &_film->device : nullptr;
}

unsigned int
UtilityDeviceResource::width() const
{
    return _film ? _film->width : 0;
}

unsigned int
UtilityDeviceResource::height() const
{
    return _film ? _film->height : 0;
}

gpu::GpuStatus
UtilityDeviceResource::recordWindow(gpu::GpuRecordContext& context,
                                    int x1,
                                    int y1,
                                    int x2,
                                    int y2,
                                    int nComps,
                                    const gpu::GpuBuffer& out,
                                    std::uint64_t outOffset,
                                    std::uint32_t outRowStride) const
{
    if (!_film) {
        return failure("The Utility renderer holds no frame on the device");
    }

    return recordPack(_film, context, x1, y1, x2, y2, nComps, out, outOffset, outRowStride);
}

bool
UtilityDeviceResource::lease(gpu::GpuBufferHandle* pixels,
                             std::shared_ptr<void>* runtimeHold) const
{
    UtilityDeviceFilm::Lease lease;

    if (!_film || !_film->lease(&lease) || !lease.pixels) {
        return false;
    }
    *pixels = lease.pixels;
    *runtimeHold = lease.runtimeHold;

    return true;
}

void
UtilityDeviceResource::releaseIfLost() const
{
    if (_film) {
        _film->releaseIfLost();
    }
}

void
UtilityDeviceResource::release() const
{
    if (_film) {
        _film->release();
    }
}

bool
UtilityDeviceResource::download(std::vector<float>* out,
                                std::string* error) const
{
    if (!_film) {
        *error = "The Utility renderer holds no frame on the device";

        return false;
    }
    out->resize(static_cast<std::size_t>(_film->width) * _film->height * 4);
    const gpu::GpuStatus status = downloadFilm(_film, out->data(), out->size() * sizeof(float));

    if (!status) {
        *error = "The Utility renderer could not read its frame back from the GPU: " + status.message;

        return false;
    }

    return true;
}

/**
 * @brief A copy of a frame's picks on the device, as Kernels/FilmPick.slang lays them out, over the window from
 * (x, y), and the miss words the AOV planes were written with.
 **/
class UtilityPickSnapshot {
public:
    std::shared_ptr<UtilityDeviceFilm> film;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::array<std::uint32_t, 3> misses {};

    std::size_t pixels() const
    {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    bool released() const
    {
        return !film || film->released();
    }

    // Copies plane's pixels() words into out, waiting for the copy.
    gpu::GpuStatus readPlane(std::size_t plane,
                             std::uint32_t* out) const
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::PickReadback);
        UtilityDeviceFilm::Lease lease;

        if (!film || !film->lease(&lease)) {
            return failure("The Utility renderer's depth and ids were released with its GPU");
        }

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(pixels()) * sizeof(std::uint32_t);
        std::unique_ptr<gpu::GpuRecordContext> context;
        gpu::GpuBuffer staging;
        gpu::GpuStatus status = gpu::GpuRecordContext::create(film->registry, context, &film->pool);

        if (status) {
            status = gpu::GpuBuffer::createStaging(film->device, bytes, gpu::kGpuBufferDefaultUsage, staging);
        }
        if (status) {
            status = context->copyBuffer(lease.pixels.buffer(), plane * bytes, staging, 0, bytes);
        }
        if (status) {
            status = context->submitAndWait();
        }
        if (status) {
            std::memcpy(out, staging.mapped(), bytes);
            ++gPickReadbacks;
        }

        return status;
    }
};

// A frame's prim ids on the device: Kernels/FilmIdsTable.slang's id table and each entry's prim path hash.
class UtilityDeviceIds {
public:
    // Destroyed after the buffers below, whose pool and device it keeps alive.
    std::shared_ptr<void> runtimeHold;
    std::shared_ptr<UtilityPickSnapshot> picks;
    gpu::GpuBufferHandle table;
    gpu::GpuBufferHandle hashes;
};

UtilityDevicePicks::UtilityDevicePicks()
{
}

UtilityDevicePicks::UtilityDevicePicks(const std::shared_ptr<UtilityPickSnapshot>& picks,
                                       UtilityPickAov aov,
                                       unsigned int width,
                                       unsigned int height)
    : _picks(picks)
    , _aov(aov)
    , _width(width)
    , _height(height)
{
}

gpu::GpuDevice*
UtilityDevicePicks::device() const
{
    return (_picks && !_picks->released()) ? &_picks->film->device : nullptr;
}

UtilityPickAov
UtilityDevicePicks::aov() const
{
    return _aov;
}

unsigned int
UtilityDevicePicks::width() const
{
    return _width;
}

unsigned int
UtilityDevicePicks::height() const
{
    return _height;
}

void
UtilityDevicePicks::releaseIfLost() const
{
    if (_picks && _picks->film) {
        _picks->film->releaseIfLost();
    }
}

bool
UtilityDevicePicks::resolveIds(const std::function<std::uint32_t(std::int32_t prim, std::int32_t instance)>& hashOf,
                               std::shared_ptr<const UtilityDeviceIds>* out,
                               std::string* error) const
{
    const auto fail = [error](const std::string& message) {
        *error = "The Utility renderer's prim ids could not be resolved on the GPU: " + message;

        return false;
    };
    UtilityDeviceFilm::Lease lease;

    if (!_picks || !_picks->film || !_picks->film->lease(&lease)) {
        return fail("they were released with the GPU");
    }
    if (!lease.kernels || !lease.kernels->ids) {
        return fail("the kernel is missing");
    }

    UtilityDeviceFilm& film = *_picks->film;
    std::shared_ptr<UtilityDeviceIds> ids = std::make_shared<UtilityDeviceIds>();

    ids->runtimeHold = lease.runtimeHold;
    ids->picks = _picks;

    const VkDeviceSize tableBytes = kIdTableWords * sizeof(std::uint32_t);
    const VkDeviceSize hashBytes = kIdCapacity * sizeof(std::uint32_t);
    const VkDeviceSize countersBytes = kIdSlotsWord * sizeof(std::uint32_t);
    const VkDeviceSize entriesBytes = 2 * kIdCapacity * sizeof(std::uint32_t);
    const VkDeviceSize picksBytes = static_cast<VkDeviceSize>(_picks->pixels()) * kFilmPickPlanes * sizeof(std::uint32_t);
    std::unique_ptr<gpu::GpuRecordContext> context;
    gpu::GpuBuffer staging;
    gpu::GpuStatus status = film.pool.acquire(tableBytes, ids->table);

    if (status) {
        status = film.pool.acquire(hashBytes, ids->hashes);
    }
    if (status) {
        status = gpu::GpuRecordContext::create(film.registry, context, &film.pool);
    }
    if (status) {
        status = gpu::GpuBuffer::createStaging(film.device, countersBytes + entriesBytes, gpu::kGpuBufferDefaultUsage, staging);
    }
    if (status) {
        status = context->fillBuffer(ids->table.buffer(), tableBytes, 0);
    }
    for (std::uint32_t mode = 0; status && (mode < 2) && (_picks->pixels() > 0); ++mode) {
        const KernelFilmIdsParams params = { static_cast<std::uint32_t>(_picks->width), static_cast<std::uint32_t>(_picks->height), mode, 0 };
        const std::array<gpu::GpuBinding, 2> bindings = { gpu::GpuBinding::storage(0, lease.pixels.buffer(), 0, picksBytes), gpu::GpuBinding::storage(1, ids->table.buffer(), 0, tableBytes) };

        context->computeBarrier();
        status = context->dispatchBindings(lease.kernels->ids, bindings, std::as_bytes(std::span(&params, 1)), { params.width, params.height, 1 });
    }
    if (status) {
        const std::array<VkBufferCopy, 2> regions = { VkBufferCopy { 0, 0, countersBytes }, VkBufferCopy { kIdEntriesWord * sizeof(std::uint32_t), countersBytes, entriesBytes } };

        status = context->copyBuffer(ids->table.buffer(), staging, regions);
    }
    if (status) {
        status = context->submitAndWait();
    }
    if (!status) {
        return fail(status.message);
    }

    const std::uint32_t* words = static_cast<const std::uint32_t*>(staging.mapped());

    if (words[1] != 0) {
        return fail("the frame shows more distinct prims than the GPU keeps");
    }
    if (words[2] != 0) {
        return fail("two prims of the frame share a tag");
    }

    const std::size_t used = std::min<std::size_t>(words[0], kIdCapacity);
    const std::uint32_t* entries = words + kIdSlotsWord;
    std::vector<std::uint32_t> hashes(used);

    for (std::size_t entry = 0; entry < used; ++entry) {
        hashes[entry] = hashOf(static_cast<std::int32_t>(entries[2 * entry]), static_cast<std::int32_t>(entries[2 * entry + 1]));
    }
    if (used > 0) {
        // updateBuffer() writes at most 64 KiB at a time.
        const std::size_t chunk = 65536 / sizeof(std::uint32_t);

        status = gpu::GpuRecordContext::create(film.registry, context, &film.pool);
        for (std::size_t first = 0; status && (first < used); first += chunk) {
            const std::size_t count = std::min(chunk, used - first);

            status = context->updateBuffer(ids->hashes.buffer(), first * sizeof(std::uint32_t), std::as_bytes(std::span(hashes.data() + first, count)));
        }
        if (status) {
            status = context->submitAndWait();
        }
        if (!status) {
            return fail(status.message);
        }
    }
    *out = std::move(ids);

    return true;
} // UtilityDevicePicks::resolveIds

gpu::GpuStatus
UtilityDevicePicks::recordWindow(gpu::GpuRecordContext& context,
                                 int x1,
                                 int y1,
                                 int x2,
                                 int y2,
                                 int nComps,
                                 const gpu::GpuBuffer& out,
                                 std::uint64_t outOffset,
                                 std::uint32_t outRowStride,
                                 const std::array<float, 8>& depthFromWindow,
                                 const std::shared_ptr<const UtilityDeviceIds>& ids) const
{
    UtilityDeviceFilm::Lease lease;

    if (!_picks || !_picks->film || !_picks->film->lease(&lease)) {
        return failure("The Utility renderer's depth and ids were released with its GPU");
    }
    if (&context.device() != &_picks->film->device) {
        return failure("The Utility renderer's depth and ids are on another device");
    }
    if (!lease.kernels || !lease.kernels->aovPack) {
        return failure("The Utility renderer has no kernel to copy its depth and ids on the GPU");
    }
    if (_aov == UtilityPickAov::InstanceId) {
        return failure("The Utility renderer copies only depth and prim ids on the GPU");
    }
    const bool primIds = _aov == UtilityPickAov::PrimId;
    if (primIds && (!ids || (ids->picks != _picks) || !ids->table || !ids->hashes)) {
        return failure("The Utility renderer's prim ids were not resolved on the GPU");
    }
    if ((nComps < 1) || (x2 < x1) || (y2 < y1)) {
        return failure("The Utility renderer's depth and ids were asked for an empty or inverted window");
    }
    if ((x2 == x1) || (y2 == y1)) {
        return gpu::GpuStatus();
    }

    const std::uint64_t width = static_cast<std::uint64_t>(x2 - x1);
    const std::uint64_t height = static_cast<std::uint64_t>(y2 - y1);
    const std::uint64_t end = outOffset + (height - 1) * outRowStride + width * static_cast<std::uint64_t>(nComps);

    // The kernel indexes the output in 32 bits.
    if ((end * sizeof(float) > out.size()) || (end > std::numeric_limits<std::uint32_t>::max())) {
        return failure("The Utility renderer's depth and ids window does not fit the buffer it is copied to");
    }

    const VkDeviceSize picksBytes = static_cast<VkDeviceSize>(_picks->pixels()) * kFilmPickPlanes * sizeof(std::uint32_t);
    KernelFilmAovPackParams params = {};

    for (std::size_t row = 0; row < 4; ++row) {
        params.zColumn[row] = depthFromWindow[row];
        params.wColumn[row] = depthFromWindow[4 + row];
    }
    params.renderWidth = _width;
    params.renderHeight = _height;
    params.picksX = _picks->x;
    params.picksY = _picks->y;
    params.picksWidth = static_cast<std::uint32_t>(_picks->width);
    params.picksHeight = static_cast<std::uint32_t>(_picks->height);
    params.x1 = x1;
    params.y1 = y1;
    params.width = static_cast<std::uint32_t>(width);
    params.height = static_cast<std::uint32_t>(height);
    params.nComps = static_cast<std::uint32_t>(nComps);
    params.outOffset = static_cast<std::uint32_t>(outOffset);
    params.outRowStride = outRowStride;
    params.mode = primIds ? 1u : 0u;

    // A depth window reads no table: the picks stand in for its bindings.
    const gpu::GpuBinding picks = gpu::GpuBinding::storage(0, lease.pixels.buffer(), 0, picksBytes);
    const std::array<gpu::GpuBinding, 4> bindings = {
        picks,
        primIds ? gpu::GpuBinding::storage(1, ids->table.buffer(), 0, kIdTableWords * sizeof(std::uint32_t)) : gpu::GpuBinding::storage(1, lease.pixels.buffer(), 0, picksBytes),
        primIds ? gpu::GpuBinding::storage(2, ids->hashes.buffer(), 0, kIdCapacity * sizeof(std::uint32_t)) : gpu::GpuBinding::storage(2, lease.pixels.buffer(), 0, picksBytes),
        gpu::GpuBinding::storage(3, out),
    };

    // Orders the copy after the frame's writes, which earlier submissions on the queue made.
    context.computeBarrier();
    if (gpu::GpuStatus s = context.dispatchBindings(lease.kernels->aovPack, bindings, std::as_bytes(std::span(&params, 1)), { params.width, params.height, 1 }); !s) {
        return s;
    }
    context.hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(lease)));
    if (primIds) {
        context.hold(std::const_pointer_cast<UtilityDeviceIds>(ids));
    }

    return gpu::GpuStatus();
} // UtilityDevicePicks::recordWindow

struct UtilityDeviceAovs::Implementation {
    // Destroyed after the buffers below, whose pool and device it keeps alive.
    std::shared_ptr<void> runtimeHold;
    gpu::GpuKernelRegistry* registry = nullptr;
    gpu::GpuBufferPool* pool = nullptr;
    std::shared_ptr<const UtilityFilmKernels> kernels;
    // Shared with the submissions that write it, so that a frame stopped with one in flight leaves it its buffer.
    std::shared_ptr<gpu::GpuBufferHandle> picks;
    std::array<std::uint32_t, 3> misses {};
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool fresh = true;
    bool drawn = false;
    // The picks were handed to readers as they are, so the next pass writes a buffer of its own.
    bool published = false;

    std::uint64_t bytes() const
    {
        return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * kFilmPickPlanes * sizeof(std::uint32_t);
    }
};

UtilityDeviceAovs::UtilityDeviceAovs()
    : _imp(new Implementation)
{
}

UtilityDeviceAovs::~UtilityDeviceAovs()
{
}

bool
UtilityDeviceAovs::fits(const gpu::GpuRecordContext& context,
                        int width,
                        int height)
{
    if ((width <= 0) || (height <= 0)) {
        return false;
    }

    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * kFilmPickPlanes * sizeof(std::uint32_t);

    // The kernel indexes the words in 32 bits.
    return (bytes <= context.maxStorageBufferBytes()) && (bytes / sizeof(std::uint32_t) <= std::numeric_limits<std::uint32_t>::max());
}

gpu::GpuStatus
UtilityDeviceAovs::beginPass(gpu::GpuRecordContext& context,
                             gpu::GpuBufferPool& pool,
                             const std::shared_ptr<const UtilityFilmKernels>& kernels,
                             const std::shared_ptr<void>& runtimeHold,
                             int x,
                             int y,
                             int width,
                             int height)
{
    Implementation& imp = *_imp;

    if (!kernels || !kernels->pick) {
        return failure("The Utility renderer has no kernel to keep its depth and ids on the GPU");
    }
    if (!fits(context, width, height)) {
        return failure("The Utility renderer's depth and ids of " + std::to_string(width) + "x" + std::to_string(height) + " pixels do not fit the GPU's storage buffer range");
    }

    const bool same = imp.picks && (imp.registry == &context.registry()) && (imp.pool == &pool) && (imp.x == x) && (imp.y == y) && (imp.width == width) && (imp.height == height);

    if (!same) {
        drop();

        gpu::GpuBufferHandle picks;

        if (gpu::GpuStatus s = pool.acquire(static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * kFilmPickPlanes * sizeof(std::uint32_t), picks); !s) {
            return s;
        }
        imp.runtimeHold = runtimeHold;
        imp.registry = &context.registry();
        imp.pool = &pool;
        imp.kernels = kernels;
        imp.picks = std::make_shared<gpu::GpuBufferHandle>(std::move(picks));
        imp.x = x;
        imp.y = y;
        imp.width = width;
        imp.height = height;
    } else if (imp.published) {
        gpu::GpuBufferHandle picks;

        if (gpu::GpuStatus s = pool.acquire(imp.bytes(), picks); !s) {
            return s;
        }

        std::shared_ptr<gpu::GpuBufferHandle> written = std::make_shared<gpu::GpuBufferHandle>(std::move(picks));

        // A pass merging into the published picks starts from them; a fresh one writes every pixel.
        if (imp.drawn) {
            context.computeBarrier();
            if (gpu::GpuStatus s = context.copyBuffer(imp.picks->buffer(), 0, written->buffer(), 0, imp.bytes()); !s) {
                return s;
            }
            context.hold(imp.picks);
            ++gPickCopies;
        }
        imp.picks = std::move(written);
    }
    imp.published = false;
    imp.fresh = !imp.drawn;

    return gpu::GpuStatus();
}

gpu::GpuStatus
UtilityDeviceAovs::recordBatch(gpu::GpuRecordContext& context,
                               const gpu::GpuBuffer& hitIds,
                               const gpu::GpuBuffer& weights,
                               const gpu::GpuBuffer& depth,
                               const gpu::GpuBuffer& primId,
                               const gpu::GpuBuffer& instanceId,
                               int y,
                               int rows)
{
    Implementation& imp = *_imp;

    if (!imp.picks) {
        return failure("The Utility renderer's depth and ids are not on the GPU");
    }
    if (rows <= 0) {
        return gpu::GpuStatus();
    }
    if ((y < imp.y) || (y + rows > imp.y + imp.height)) {
        return failure("The Utility renderer drew a batch outside the window its depth and ids were kept for");
    }

    const KernelFilmPickParams params = {
        static_cast<std::uint32_t>(y - imp.y) * static_cast<std::uint32_t>(imp.width),
        static_cast<std::uint32_t>(imp.width) * static_cast<std::uint32_t>(rows),
        imp.fresh ? 1u : 0u,
        static_cast<std::uint32_t>(imp.width) * static_cast<std::uint32_t>(imp.height),
        imp.misses[0],
        imp.misses[1],
        imp.misses[2],
        0
    };
    const std::array<gpu::GpuBinding, 6> bindings = {
        gpu::GpuBinding::storage(0, hitIds),
        gpu::GpuBinding::storage(1, weights),
        gpu::GpuBinding::storage(2, depth),
        gpu::GpuBinding::storage(3, primId),
        gpu::GpuBinding::storage(4, instanceId),
        gpu::GpuBinding::storage(5, imp.picks->buffer(), 0, imp.bytes()),
    };

    context.computeBarrier();
    if (gpu::GpuStatus s = context.dispatchBindings(imp.kernels->pick, bindings, std::as_bytes(std::span(&params, 1)), { params.count, 1, 1 }); !s) {
        return s;
    }
    context.hold(imp.picks);

    return gpu::GpuStatus();
}

void
UtilityDeviceAovs::endPass()
{
    if (_imp->picks) {
        _imp->drawn = true;
        _imp->fresh = false;
    }
}

bool
UtilityDeviceAovs::drawn() const
{
    return _imp->drawn;
}

void
UtilityDeviceAovs::setMisses(const std::vector<UtilityRenderBuffer*>& depth,
                             const std::vector<UtilityRenderBuffer*>& primId,
                             const std::vector<UtilityRenderBuffer*>& instanceId)
{
    const std::vector<UtilityRenderBuffer*>* aovs[] = { &depth, &primId, &instanceId };

    for (std::size_t aov = 0; aov < 3; ++aov) {
        std::uint32_t word = 0;

        _imp->misses[aov] = (!aovs[aov]->empty() && aovs[aov]->front()->clearWord(&word)) ? word : 0;
    }
}

bool
UtilityDeviceAovs::publish(const std::vector<UtilityRenderBuffer*>& depth,
                           const std::vector<UtilityRenderBuffer*>& primId,
                           const std::vector<UtilityRenderBuffer*>& instanceId,
                           std::string* error)
{
    Implementation& imp = *_imp;
    const std::vector<UtilityRenderBuffer*>* aovs[] = { &depth, &primId, &instanceId };

    (void)error;
    if (!imp.drawn || !imp.picks) {
        for (const std::vector<UtilityRenderBuffer*>* buffers : aovs) {
            for (UtilityRenderBuffer* buffer : *buffers) {
                buffer->publishPass();
            }
        }

        return true;
    }
    if (depth.empty() && primId.empty() && instanceId.empty()) {
        return true;
    }

    // The passes' writes were made by earlier submissions on the same queue, which every reader's copy is ordered after.
    std::shared_ptr<UtilityDeviceFilm> copy = std::make_shared<UtilityDeviceFilm>(*imp.registry, *imp.pool, imp.kernels, imp.runtimeHold, imp.width, imp.height);

    copy->adopt(*imp.picks);
    imp.published = true;
    registerFilm(copy);

    std::shared_ptr<UtilityPickSnapshot> picks = std::make_shared<UtilityPickSnapshot>();

    picks->film = std::move(copy);
    picks->x = imp.x;
    picks->y = imp.y;
    picks->width = imp.width;
    picks->height = imp.height;
    picks->misses = imp.misses;

    const UtilityPickAov kinds[] = { UtilityPickAov::Depth, UtilityPickAov::PrimId, UtilityPickAov::InstanceId };

    for (std::size_t aov = 0; aov < 3; ++aov) {
        for (UtilityRenderBuffer* buffer : *aovs[aov]) {
            buffer->publishPicks(picks, kinds[aov]);
        }
    }

    return true;
}

void
UtilityDeviceAovs::drop()
{
    Implementation& imp = *_imp;

    imp.picks.reset();
    imp.kernels.reset();
    imp.registry = nullptr;
    imp.pool = nullptr;
    imp.x = imp.y = imp.width = imp.height = 0;
    imp.fresh = true;
    imp.drawn = false;
    imp.published = false;
    imp.runtimeHold.reset();
}
#endif // NATRON_HAVE_GPU

namespace {
// Fills out, a whole number of pixels long, with pixel, in copies that double each time.
void
fillWithPixel(std::vector<std::uint8_t>* out,
              const std::vector<std::uint8_t>& pixel)
{
    if (out->empty() || pixel.empty()) {
        return;
    }

    std::size_t done = std::min(pixel.size(), out->size());

    std::memcpy(out->data(), pixel.data(), done);
    while (done < out->size()) {
        const std::size_t next = std::min(done, out->size() - done);

        std::memcpy(out->data() + done, out->data(), next);
        done += next;
    }
}
} // anon namespace

UtilityRenderBuffer::UtilityRenderBuffer(const SdfPath& id)
    : HdRenderBuffer(id)
{
    ++gLiveBuffers;
}

UtilityRenderBuffer::~UtilityRenderBuffer()
{
    --gLiveBuffers;
}

int
UtilityRenderBuffer::liveCount()
{
    return gLiveBuffers.load();
}

void
UtilityRenderBuffer::Sync(HdSceneDelegate* sceneDelegate,
                          HdRenderParam* renderParam,
                          HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & DirtyDescription) {
        static_cast<UtilityRenderParam*>(renderParam)->cancelFrameAndWait();
    }
    HdRenderBuffer::Sync(sceneDelegate, renderParam, dirtyBits);
}

void
UtilityRenderBuffer::Finalize(HdRenderParam* renderParam)
{
    static_cast<UtilityRenderParam*>(renderParam)->cancelFrameAndWait();
    HdRenderBuffer::Finalize(renderParam);
}

bool
UtilityRenderBuffer::Allocate(const GfVec3i& dimensions,
                              HdFormat format,
                              bool multiSampled)
{
    _Deallocate();
    if (dimensions[0] < 0 || dimensions[1] < 0 || dimensions[2] != 1) {
        TF_WARN("A render buffer must be a 2D image, not %dx%dx%d", dimensions[0], dimensions[1], dimensions[2]);

        return false;
    }
    if (format == HdFormatInvalid) {
        TF_WARN("A render buffer needs a pixel format");

        return false;
    }
    _width = static_cast<unsigned int>(dimensions[0]);
    _height = static_cast<unsigned int>(dimensions[1]);
    _format = format;
    _multiSampled = multiSampled;
    _pixels.assign(static_cast<std::size_t>(_width) * _height * HdDataSizeOfFormat(format), 0);

    const std::lock_guard<std::mutex> l(_deviceMutex);

    if (!_hostReady.load()) {
        _sampleSums.assign(static_cast<std::size_t>(_width) * _height * _clearComponents.size(), 0.f);
        _sampleWeights.assign(static_cast<std::size_t>(_width) * _height, -1.f);
        _hostReady.store(true);
    }
    publishHostLocked();

    return true;
}

unsigned int
UtilityRenderBuffer::GetWidth() const
{
    return _width;
}

unsigned int
UtilityRenderBuffer::GetHeight() const
{
    return _height;
}

unsigned int
UtilityRenderBuffer::GetDepth() const
{
    return 1;
}

HdFormat
UtilityRenderBuffer::GetFormat() const
{
    return _format;
}

bool
UtilityRenderBuffer::IsMultiSampled() const
{
    return _multiSampled;
}

void*
UtilityRenderBuffer::Map()
{
    const std::lock_guard<std::mutex> l(_deviceMutex);

    if (!_mapped) {
        frontToHostLocked();
        _mapped = _front;
    }
    ++_mappers;

    return (!_mapped || _mapped->empty()) ? nullptr : _mapped->data();
}

void
UtilityRenderBuffer::Unmap()
{
    const std::lock_guard<std::mutex> l(_deviceMutex);

    if ((_mappers.load() > 0) && (--_mappers == 0)) {
        _mapped.reset();
    }
}

bool
UtilityRenderBuffer::IsMapped() const
{
    return _mappers.load() != 0;
}

void
UtilityRenderBuffer::Resolve()
{
}

bool
UtilityRenderBuffer::IsConverged() const
{
    if (!_converged.load()) {
        return false;
    }
#ifdef NATRON_HAVE_GPU
    const std::lock_guard<std::mutex> l(_deviceMutex);

    // A lost GPU releases the published film without a releaseDevice() to read it back first, so
    // the frame's pixels are gone and the frame must be drawn again rather than left as finished.
    if ((_frontFilm && _frontFilm->released()) || (_frontPicks && _frontPicks->released())) {
        return false;
    }
#endif

    return true;
}

VtValue
UtilityRenderBuffer::GetResource(bool /*multiSampled*/) const
{
#ifdef NATRON_HAVE_GPU
    const std::lock_guard<std::mutex> l(_deviceMutex);

    if (_frontFilm && !_frontFilm->released()) {
        return VtValue(UtilityDeviceResource(_frontFilm));
    }
    if (_frontPicks && !_frontPicks->released() && (_frontPickAov != UtilityPickAov::InstanceId)) {
        return VtValue(UtilityDevicePicks(_frontPicks, _frontPickAov, _width, _height));
    }
#endif

    return VtValue();
}

std::uint64_t
UtilityRenderBuffer::deviceDownloadCount()
{
    return gDeviceDownloads.load();
}

std::uint64_t
UtilityRenderBuffer::pickReadbackCount()
{
    return gPickReadbacks.load();
}

std::uint64_t
UtilityRenderBuffer::pickCopyCount()
{
    return gPickCopies.load();
}

std::uint64_t
UtilityRenderBuffer::convergedCount()
{
    const std::lock_guard<std::mutex> lock(gConvergedMutex);

    return gConverged;
}

void
UtilityRenderBuffer::waitForConverged(std::uint64_t seen,
                                      std::chrono::milliseconds bound)
{
    std::unique_lock<std::mutex> lock(gConvergedMutex);
    const std::uint64_t wakes = gConvergedWakes;
    (void)gConvergedChanged.wait_for(lock, bound, [seen, wakes]() { return (gConverged != seen) || (gConvergedWakes != wakes); });
}

void
UtilityRenderBuffer::wakeConvergedWaiters()
{
    {
        const std::lock_guard<std::mutex> lock(gConvergedMutex);
        ++gConvergedWakes;
    }
    gConvergedChanged.notify_all();
}

bool
UtilityRenderBuffer::clearWord(std::uint32_t* word) const
{
    const std::lock_guard<std::mutex> l(_deviceMutex);

    if (_clearPixel.size() != sizeof(std::uint32_t)) {
        return false;
    }
    std::memcpy(word, _clearPixel.data(), sizeof(std::uint32_t));

    return true;
}

void
UtilityRenderBuffer::ensureHost()
{
    if (_hostReady.load(std::memory_order_acquire)) {
        return;
    }

    const std::lock_guard<std::mutex> l(_deviceMutex);

    ensureHostLocked();
}

void
UtilityRenderBuffer::ensureHostLocked()
{
    if (_hostReady.load(std::memory_order_relaxed)) {
        return;
    }
    fillWithPixel(&_pixels, _clearPixel);
    _sampleSums.assign(static_cast<std::size_t>(_width) * _height * _clearComponents.size(), 0.f);
    _sampleWeights.assign(static_cast<std::size_t>(_width) * _height, -1.f);
    _hostReady.store(true, std::memory_order_release);
}

void
UtilityRenderBuffer::clear(const VtValue& value)
{
    clear(value, false);
}

void
UtilityRenderBuffer::clear(const VtValue& value,
                           bool deferHost)
{
    const std::lock_guard<std::mutex> l(_deviceMutex);

#ifdef NATRON_HAVE_GPU
    if (_film) {
        _film->releaseIfLost();
        if (_film->released()) {
            _film.reset();
            _onDevice = false;
        }
    }
#endif
    const std::vector<double> components = (value.IsEmpty() || _pixels.empty()) ? std::vector<double>() : clearComponents(value);

    // Without a value the buffer stays as it is, which an earlier deferred clear may not have filled yet.
    if (components.empty()) {
        ensureHostLocked();
    }
    _clearPixel.clear();
    _deviceRestart = true;
    ++_deviceEpoch;
    _clearComponents.assign((_format == HdFormatInvalid) ? 0 : HdGetComponentCount(_format), 0.f);
    if (components.empty()) {
        _sampleSums.assign(static_cast<std::size_t>(_width) * _height * _clearComponents.size(), 0.f);
        _sampleWeights.assign(static_cast<std::size_t>(_width) * _height, -1.f);
        // What the device holds is the buffer as it is, and a frame that follows writes the host's.
        if (_onDevice) {
            downloadLocked();
        }
        publishHostLocked();

        return;
    }
    _onDevice = false;
    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t componentCount = HdGetComponentCount(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    const std::size_t componentSize = pixelSize / componentCount;
    std::vector<std::uint8_t> pixel(pixelSize, 0);

    for (std::size_t c = 0; c < std::min(componentCount, components.size()); ++c) {
        encodeComponent(componentFormat, components[c], pixel.data() + c * componentSize);
        _clearComponents[c] = static_cast<float>(components[c]);
    }
    _clearPixel = std::move(pixel);
    _hostReady.store(false, std::memory_order_relaxed);
    if (deferHost) {
        // Readers until the first pass get the clear value, which frontToHostLocked() fills in for them.
        retireFrontLocked();
#ifdef NATRON_HAVE_GPU
        _frontFilm.reset();
        _frontPicks.reset();
#endif

        return;
    }
    ensureHostLocked();
    publishHostLocked();
}

void
UtilityRenderBuffer::publishPass()
{
#ifdef NATRON_HAVE_GPU
    std::shared_ptr<UtilityDeviceFilm> live;
    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        if (_onDevice && _film && !_film->released()) {
            live = _film;
        }
    }
    if (live) {
        std::shared_ptr<UtilityDeviceFilm> copy;
        const gpu::GpuStatus status = copyFilm(live, &copy);
        const std::lock_guard<std::mutex> l(_deviceMutex);

        if (status) {
            retireFrontLocked();
            _frontFilm = std::move(copy);
            _frontPicks.reset();

            return;
        }
        // Readers still get the pass, through the host, rather than the pass before.
        std::shared_ptr<std::vector<std::uint8_t>> host = std::move(_spare);

        if (!host) {
            host = std::make_shared<std::vector<std::uint8_t>>();
        }
        host->resize(_pixels.size());

        const gpu::GpuStatus downloaded = downloadFilm(live, host->data(), host->size());

        if (!downloaded) {
            TF_WARN("The Utility renderer could not hand its frame over from the GPU: %s; %s", status.message.c_str(), downloaded.message.c_str());
            _spare = std::move(host);

            return;
        }
        ++gDeviceDownloads;
        retireFrontLocked();
        _front = std::move(host);
        _frontFilm.reset();
        _frontPicks.reset();

        return;
    }
#endif
    std::shared_ptr<std::vector<std::uint8_t>> next;
    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        ensureHostLocked();
        next = std::move(_spare);
    }
    if (!next) {
        next = std::make_shared<std::vector<std::uint8_t>>();
    }
    // Only this thread writes the pixels while the frame runs, so they are copied outside the lock.
    next->assign(_pixels.begin(), _pixels.end());

    const std::lock_guard<std::mutex> l(_deviceMutex);

    retireFrontLocked();
    _front = std::move(next);
#ifdef NATRON_HAVE_GPU
    _frontFilm.reset();
    _frontPicks.reset();
#endif
}

void
UtilityRenderBuffer::publishHostLocked()
{
    ensureHostLocked();

    std::shared_ptr<std::vector<std::uint8_t>> next = std::move(_spare);

    if (!next) {
        next = std::make_shared<std::vector<std::uint8_t>>();
    }
    next->assign(_pixels.begin(), _pixels.end());
    retireFrontLocked();
    _front = std::move(next);
#ifdef NATRON_HAVE_GPU
    _frontFilm.reset();
    _frontPicks.reset();
#endif
}

void
UtilityRenderBuffer::retireFrontLocked()
{
    // A copy a reader still maps is the reader's until it unmaps.
    if (_front && (_front.use_count() == 1)) {
        _spare = std::move(_front);
    }
    _front.reset();
}

void
UtilityRenderBuffer::frontToHostLocked()
{
    if (_front) {
        return;
    }
    std::shared_ptr<std::vector<std::uint8_t>> host = std::move(_spare);

    if (!host) {
        host = std::make_shared<std::vector<std::uint8_t>>();
    }
#ifdef NATRON_HAVE_GPU
    if (_frontFilm && !_frontFilm->released()) {
        host->resize(_pixels.size());

        const gpu::GpuStatus status = [&]() {
            const utilityProfile::Scope timed(utilityProfile::Stage::ColorDownload);

            return downloadFilm(_frontFilm, host->data(), host->size());
        }();

        if (status) {
            ++gDeviceDownloads;
            _front = std::move(host);

            return;
        }
        // The host keeps the frame's clear value: the frame is lost with the device, not partly shown.
        TF_WARN("The Utility renderer could not read its frame back from the GPU: %s", status.message.c_str());
    }
    if (_frontPicks && !_frontPicks->released()) {
        const gpu::GpuStatus status = picksToHostLocked(host.get());

        if (status) {
            _front = std::move(host);

            return;
        }
        TF_WARN("The Utility renderer could not read its depth or ids back from the GPU: %s", status.message.c_str());
    }
#endif
    ensureHostLocked();
    host->assign(_pixels.begin(), _pixels.end());
    _front = std::move(host);
}

#ifdef NATRON_HAVE_GPU
gpu::GpuStatus
UtilityRenderBuffer::picksToHostLocked(std::vector<std::uint8_t>* out)
{
    const UtilityPickSnapshot& picks = *_frontPicks;
    const std::size_t aov = static_cast<std::size_t>(_frontPickAov);
    const std::size_t plane = 2 + aov;
    const std::int64_t width = _width;
    const std::int64_t height = _height;
    const bool inside = (picks.x >= 0) && (picks.y >= 0) && (picks.x + picks.width <= width) && (picks.y + picks.height <= height);
    const bool whole = inside && (picks.x == 0) && (picks.y == 0) && (picks.width == width) && (picks.height == height);
    const HdFormat wordFormat = (_frontPickAov == UtilityPickAov::Depth) ? HdFormatFloat32 : HdFormatInt32;
    // The plane holds the buffer's own pixels when it has their format and its misses are the buffer's clear value.
    const bool asIs = inside && (_format == wordFormat) && (_clearPixel.size() == sizeof(std::uint32_t)) && (std::memcmp(_clearPixel.data(), &picks.misses[aov], sizeof(std::uint32_t)) == 0);

    if (asIs && whole) {
        out->resize(_pixels.size());

        return picks.readPlane(plane, reinterpret_cast<std::uint32_t*>(out->data()));
    }
    if (_clearPixel.empty()) {
        ensureHostLocked();
        out->assign(_pixels.begin(), _pixels.end());
    } else {
        out->resize(_pixels.size());
        fillWithPixel(out, _clearPixel);
    }
    if (out->empty() || (picks.pixels() == 0)) {
        return gpu::GpuStatus();
    }

    std::vector<std::uint32_t> values(picks.pixels());

    if (gpu::GpuStatus s = picks.readPlane(plane, values.data()); !s) {
        return s;
    }
    if (asIs) {
        for (int row = 0; row < picks.height; ++row) {
            std::memcpy(out->data() + (static_cast<std::size_t>(picks.y + row) * _width + picks.x) * sizeof(std::uint32_t), values.data() + static_cast<std::size_t>(row) * picks.width, picks.width * sizeof(std::uint32_t));
        }

        return gpu::GpuStatus();
    }

    // A miss is the clear value, already there, or with none the pixel as it was.
    std::vector<std::uint32_t> hits(picks.pixels());

    if (gpu::GpuStatus s = picks.readPlane(kFilmPickHitPlane, hits.data()); !s) {
        return s;
    }

    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);

    for (int row = 0; row < picks.height; ++row) {
        const std::int64_t y = picks.y + row;

        for (int column = 0; column < picks.width; ++column) {
            const std::int64_t x = picks.x + column;
            const std::size_t at = static_cast<std::size_t>(row) * picks.width + column;

            if ((x < 0) || (y < 0) || (x >= width) || (y >= height) || !hits[at]) {
                continue;
            }

            std::uint8_t* pixel = out->data() + (static_cast<std::size_t>(y) * _width + static_cast<std::size_t>(x)) * pixelSize;

            if (_frontPickAov == UtilityPickAov::Depth) {
                float depth = 0.f;

                std::memcpy(&depth, &values[at], sizeof(float));
                encodeComponent(componentFormat, depth, pixel);
            } else {
                std::int32_t id = 0;

                std::memcpy(&id, &values[at], sizeof(std::int32_t));
                encodeComponent(componentFormat, static_cast<double>(id), pixel);
            }
        }
    }

    return gpu::GpuStatus();
}

void
UtilityRenderBuffer::publishPicks(const std::shared_ptr<UtilityPickSnapshot>& picks,
                                  UtilityPickAov aov)
{
    const std::lock_guard<std::mutex> l(_deviceMutex);

    retireFrontLocked();
    _frontFilm.reset();
    _frontPicks = picks;
    _frontPickAov = aov;
}
#endif

void
UtilityRenderBuffer::setConverged(bool converged)
{
    _converged.store(converged);
    if (!converged) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(gConvergedMutex);
        ++gConverged;
    }
    gConvergedChanged.notify_all();
}

void
UtilityRenderBuffer::setPixel(unsigned int x,
                              unsigned int y,
                              const float* components,
                              std::size_t count)
{
    if (x >= _width || y >= _height) {
        return;
    }
    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t componentCount = HdGetComponentCount(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    const std::size_t written = std::min(componentCount, count);
    std::uint8_t* pixel = _pixels.data() + (static_cast<std::size_t>(y) * _width + x) * pixelSize;

    if (componentFormat == HdFormatFloat32) {
        std::memcpy(pixel, components, written * sizeof(float));

        return;
    }
    const std::size_t componentSize = pixelSize / componentCount;

    for (std::size_t c = 0; c < written; ++c) {
        encodeComponent(componentFormat, components[c], pixel + c * componentSize);
    }
}

void
UtilityRenderBuffer::setPixel(unsigned int x,
                              unsigned int y,
                              std::int32_t value)
{
    if (x >= _width || y >= _height) {
        return;
    }
    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    std::uint8_t* pixel = _pixels.data() + (static_cast<std::size_t>(y) * _width + x) * pixelSize;

    // A double holds every int32 exactly, so this is the same integer whichever format it lands in.
    encodeComponent(componentFormat, static_cast<double>(value), pixel);
}

void
UtilityRenderBuffer::addSample(unsigned int x,
                               unsigned int y,
                               const float* components,
                               std::size_t count,
                               float weight,
                               bool filtered)
{
    if (x >= _width || y >= _height || _sampleWeights.empty()) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(y) * _width + x;
    const std::size_t componentCount = _clearComponents.size();
    const float* sample = components ? components : _clearComponents.data();
    const std::size_t sampled = components ? std::min(count, componentCount) : componentCount;
    float& weights = _sampleWeights[index];

    if (!filtered) {
        if (weight <= weights) {
            return;
        }
        weights = weight;
        if (components) {
            setPixel(x, y, components, count);
        } else if (!_clearPixel.empty()) {
            const std::size_t pixelSize = _clearPixel.size();

            std::memcpy(_pixels.data() + index * pixelSize, _clearPixel.data(), pixelSize);
        }

        return;
    }

    float* sums = _sampleSums.data() + index * componentCount;
    const float total = std::max(weights, 0.f) + weight;
    float resolved[4] = { 0.f, 0.f, 0.f, 0.f };

    weights = total;
    for (std::size_t c = 0; c < componentCount; ++c) {
        sums[c] += weight * (c < sampled ? sample[c] : _clearComponents[c]);
    }
    if (total <= 0.f) {
        return;
    }
    for (std::size_t c = 0; c < std::min<std::size_t>(componentCount, 4); ++c) {
        resolved[c] = sums[c] / total;
    }
    setPixel(x, y, resolved, std::min<std::size_t>(componentCount, 4));
}

void
UtilityRenderBuffer::addSample(unsigned int x,
                               unsigned int y,
                               const std::int32_t* value,
                               float weight)
{
    if (x >= _width || y >= _height || _sampleWeights.empty()) {
        return;
    }

    float& heaviest = _sampleWeights[static_cast<std::size_t>(y) * _width + x];

    if (weight <= heaviest) {
        return;
    }
    heaviest = weight;
    if (value) {
        setPixel(x, y, *value);
    } else if (!_clearPixel.empty()) {
        const std::size_t pixelSize = _clearPixel.size();

        std::memcpy(_pixels.data() + (static_cast<std::size_t>(y) * _width + x) * pixelSize, _clearPixel.data(), pixelSize);
    }
}

void
UtilityRenderBuffer::_Deallocate()
{
    TF_VERIFY(!IsMapped());
    _width = 0;
    _height = 0;
    _format = HdFormatInvalid;
    _multiSampled = false;
    std::vector<std::uint8_t>().swap(_pixels);
    _mappers.store(0);
    _converged.store(false);

    const std::lock_guard<std::mutex> l(_deviceMutex);

    _clearPixel.clear();
    std::vector<float>().swap(_clearComponents);
    std::vector<float>().swap(_sampleSums);
    std::vector<float>().swap(_sampleWeights);
    _deviceRestart = true;
    ++_deviceEpoch;
#ifdef NATRON_HAVE_GPU
    _film.reset();
    _frontFilm.reset();
    _frontPicks.reset();
#endif
    _hostReady.store(true);
    _onDevice = false;
    _front.reset();
    _spare.reset();
    _mapped.reset();
}

void
UtilityRenderBuffer::downloadLocked()
{
    ensureHostLocked();
    _onDevice = false;
#ifdef NATRON_HAVE_GPU
    if (!_film) {
        return;
    }

    UtilityDeviceFilm& film = *_film;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(film.width) * film.height * kFilmPixelBytes;

    if (bytes != _pixels.size()) {
        TF_CODING_ERROR("The Utility renderer's film is %ux%u, its buffer %ux%u", film.width, film.height, _width, _height);

        return;
    }

    const gpu::GpuStatus status = downloadFilm(_film, _pixels.data(), bytes);

    if (!status) {
        // The host keeps the frame's clear value: the frame is lost with the device, not partly shown.
        TF_WARN("The Utility renderer could not read its frame back from the GPU: %s", status.message.c_str());

        return;
    }
    ++gDeviceDownloads;
#endif
}

#ifdef NATRON_HAVE_GPU
void
UtilityRenderBuffer::releaseDevice()
{
    std::shared_ptr<UtilityDeviceFilm> film;
    std::shared_ptr<UtilityDeviceFilm> front;
    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        ++_deviceEpoch;
        if (_film) {
            _film->releaseIfLost();
        }
        if (_onDevice && _film && !_film->released()) {
            downloadLocked();
        }
        _onDevice = false;
        film = std::move(_film);
        if (_frontFilm) {
            _frontFilm->releaseIfLost();
            frontToHostLocked();
            front = std::move(_frontFilm);
        }
        // The other AOVs' buffers share the picks, so each only reads its own back and lets go of them.
        if (_frontPicks) {
            _frontPicks->film->releaseIfLost();
            frontToHostLocked();
            _frontPicks.reset();
        }
    }
    // Every holder lets go too: a strip still to copy its window renders again on the CPU, and a viewer
    // showing the frame keeps the pixels it already has. Recorded copies hold a lease of their own.
    if (film) {
        film->release();
    }
    if (front) {
        front->release();
    }
    _converged.store(false);
}

bool
UtilityRenderBuffer::canRenderOnDevice() const
{
    const std::lock_guard<std::mutex> l(_deviceMutex);

    return (_format == HdFormatFloat32Vec4) && !_multiSampled && (_width > 0) && (_height > 0) && (_clearPixel.size() == kFilmPixelBytes);
}

gpu::GpuStatus
UtilityRenderBuffer::beginDeviceFrame(const std::function<gpu::GpuRecordContext*(std::uint64_t work)>& record,
                                      std::uint64_t maxWork,
                                      gpu::GpuKernelRegistry& registry,
                                      gpu::GpuBufferPool& pool,
                                      const std::shared_ptr<const UtilityFilmKernels>& kernels,
                                      const std::shared_ptr<void>& runtimeHold,
                                      bool* superseded)
{
    *superseded = false;

    const char* const halted = "The Utility renderer's frame stopped or failed before its film was ready";
    gpu::GpuRecordContext* context = record(0);

    if (!context) {
        return failure(halted);
    }

    bool restart = false;
    std::uint64_t epoch = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<std::uint8_t, kFilmPixelBytes> clearPixel {};
    std::shared_ptr<UtilityDeviceFilm> fresh;
    UtilityDeviceFilm::Lease lease;
    UtilityDeviceFilm::Lease previous;

    // The film is only planned and installed under the lock: the clear or copy between submits and waits, and the
    // lock keeps the viewer's Map() and the other readers waiting for as long as it is held.
    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        restart = _deviceRestart;
        if ((_format != HdFormatFloat32Vec4) || (_width == 0) || (_height == 0) || (_clearPixel.size() != kFilmPixelBytes) || !kernels) {
            return failure("The Utility renderer's colour cannot be held on the GPU");
        }

        const bool usable = _film && !_film->released() && (&_film->device == &registry.device()) && (_film->width == _width) && (_film->height == _height);

        if (!restart && !usable) {
            return failure("The Utility renderer's film left the GPU part way through the frame's samples");
        }
        epoch = _deviceEpoch;
        width = _width;
        height = _height;
        std::memcpy(clearPixel.data(), _clearPixel.data(), kFilmPixelBytes);

        // A film still held elsewhere has a copy of it recorded and not yet run, which must see the frame it was
        // asked for. The new film replaces it only once filled, so that a frame stopped part way leaves the film
        // its earlier calls accumulated.
        if (!usable || (_film.use_count() > 1)) {
            if (!restart && !_film->lease(&previous)) {
                return failure("The Utility renderer's film was released with its GPU");
            }
            fresh = std::make_shared<UtilityDeviceFilm>(registry, pool, kernels, runtimeHold, width, height);
            if (gpu::GpuStatus s = fresh->allocate(static_cast<VkDeviceSize>(width) * height * kFilmPixelBytes); !s) {
                return s;
            }
            registerFilm(fresh);
            if (!fresh->lease(&lease)) {
                return failure("The Utility renderer's film was released with its GPU");
            }
        } else if (!_film->lease(&lease)) {
            _film.reset();

            return failure("The Utility renderer's film was released with its GPU");
        }
    }

    if (restart) {
        const std::uint32_t bandRows = filmBandRows(*context, width, height);

        if (bandRows == 0) {
            return filmTooWide(width);
        }

        const std::uint32_t chunkRows = filmClearRows(*context, width, bandRows, maxWork * kFilmClearPixelsPerWork);

        for (std::uint32_t first = 0; first < height; first += chunkRows) {
            const std::uint32_t rows = std::min(chunkRows, height - first);
            const std::uint64_t pixels = static_cast<std::uint64_t>(width) * rows;

            context = record((pixels + kFilmClearPixelsPerWork - 1) / kFilmClearPixelsPerWork);
            if (!context) {
                return failure(halted);
            }

            KernelFilmClearParams params = { { 0, 0, 0, 0 }, width, rows, 0, 0 };

            std::memcpy(params.value, clearPixel.data(), kFilmPixelBytes);

            const std::array<gpu::GpuBinding, 2> bindings = { filmPixelsBand(0, lease.pixels.buffer(), width, first, rows), filmAccumulationBand(1, lease.accumulation.buffer(), width, first, rows) };

            if (gpu::GpuStatus s = context->dispatchBindings(lease.kernels->clear, bindings, std::as_bytes(std::span(&params, 1)), { width, rows, 1 }); !s) {
                return s;
            }
        }
    } else if (previous.pixels) {
        const std::uint64_t total = static_cast<std::uint64_t>(width) * height;
        const std::uint64_t chunkPixels = std::max<std::uint64_t>(1, maxWork * kFilmCopyPixelsPerWork);

        context->computeBarrier();
        for (std::uint64_t first = 0; first < total; first += chunkPixels) {
            const std::uint64_t pixels = std::min(chunkPixels, total - first);

            context = record((pixels + kFilmCopyPixelsPerWork - 1) / kFilmCopyPixelsPerWork);
            if (!context) {
                return failure(halted);
            }

            const VkDeviceSize offset = first * kFilmPixelBytes;
            const VkDeviceSize bytes = pixels * kFilmPixelBytes;

            if (gpu::GpuStatus s = context->copyBuffer(previous.pixels.buffer(), offset, lease.pixels.buffer(), offset, bytes); !s) {
                return s;
            }
            if (gpu::GpuStatus s = context->copyBuffer(previous.accumulation.buffer(), 2 * offset, lease.accumulation.buffer(), 2 * offset, 2 * bytes); !s) {
                return s;
            }
        }
    }

    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        if (_deviceEpoch != epoch) {
            *superseded = true;

            return failure("The Utility renderer's film was cleared or released while its frame was starting");
        }
        if (fresh) {
            if (fresh->released()) {
                return failure("The Utility renderer's film was released with its GPU");
            }
            _film = std::move(fresh);
        } else if (!_film || _film->released()) {
            _film.reset();

            return failure("The Utility renderer's film was released with its GPU");
        }
        _onDevice = true;
        _deviceRestart = false;
    }
    if (previous.pixels) {
        context->hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(previous)));
    }
    context->hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(lease)));

    return gpu::GpuStatus();
}

gpu::GpuStatus
UtilityRenderBuffer::recordDeviceBatch(gpu::GpuRecordContext& context,
                                       const gpu::GpuBuffer& hitIds,
                                       const gpu::GpuBuffer& color,
                                       const gpu::GpuBuffer& weights,
                                       int x,
                                       int y,
                                       int width,
                                       int height,
                                       bool background)
{
    std::shared_ptr<UtilityDeviceFilm> film;
    KernelFilmScatterParams params = {};

    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        if (_film && !_film->released()) {
            film = _film;
        }
        if (_clearPixel.size() == kFilmPixelBytes) {
            std::memcpy(params.clearValue, _clearPixel.data(), kFilmPixelBytes);
        }
    }
    UtilityDeviceFilm::Lease lease;

    if (!film || !film->lease(&lease)) {
        return failure("The Utility renderer's colour is not on the GPU");
    }
    if ((width <= 0) || (height <= 0)) {
        return gpu::GpuStatus();
    }

    const std::uint32_t count = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);

    const std::uint32_t bandRows = filmBandRows(context, film->width, film->height);

    if (bandRows == 0) {
        return filmTooWide(film->width);
    }

    params.filmWidth = film->width;
    params.x = x;
    params.width = static_cast<std::uint32_t>(width);
    params.count = count;
    params.background = background ? 1u : 0u;

    context.computeBarrier();
    // Each band's dispatch skips the rays of the batch's pixels outside it, as the kernel skips those off the film.
    for (std::uint32_t first = 0; first < film->height; first += bandRows) {
        const std::uint32_t rows = std::min(bandRows, film->height - first);

        if ((static_cast<std::int64_t>(y) + height <= first) || (y >= static_cast<std::int64_t>(first) + rows)) {
            continue;
        }
        params.filmHeight = rows;
        params.y = static_cast<std::int32_t>(static_cast<std::int64_t>(y) - first);

        const std::array<gpu::GpuBinding, 5> bindings = {
            gpu::GpuBinding::storage(0, hitIds),
            gpu::GpuBinding::storage(1, color),
            gpu::GpuBinding::storage(2, weights),
            filmPixelsBand(3, lease.pixels.buffer(), film->width, first, rows),
            filmAccumulationBand(4, lease.accumulation.buffer(), film->width, first, rows),
        };

        if (gpu::GpuStatus s = context.dispatchBindings(lease.kernels->scatter, bindings, std::as_bytes(std::span(&params, 1)), { count, 1, 1 }); !s) {
            return s;
        }
    }
    context.hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(lease)));

    return gpu::GpuStatus();
}
#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT
