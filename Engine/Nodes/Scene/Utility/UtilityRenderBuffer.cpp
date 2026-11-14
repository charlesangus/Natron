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
#include <span>
#include <utility>

#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/GpuContext.h"

#include "utility_film_clear_spirv.h"
#include "utility_film_pack_spirv.h"
#include "utility_film_scatter_spirv.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::atomic<int> gLiveBuffers { 0 };
std::atomic<std::uint64_t> gDeviceDownloads { 0 };

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
// Kernels/FilmClear.slang, FilmScatter.slang and FilmPack.slang declare these push constants.
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
    std::uint32_t padding0;
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

static_assert(sizeof(KernelFilmClearParams) == utility_film_clear_push_bytes);
static_assert(sizeof(KernelFilmScatterParams) == utility_film_scatter_push_bytes);
static_assert(sizeof(KernelFilmPackParams) == utility_film_pack_push_bytes);
static_assert(utility_film_clear_storage_buffers == 2 && utility_film_scatter_storage_buffers == 5 && utility_film_pack_storage_buffers == 2);

gpu::GpuStatus
failure(const std::string& message)
{
    return gpu::GpuStatus { VK_ERROR_UNKNOWN, message };
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

    gpu::GpuStatus allocate(VkDeviceSize bytes)
    {
        const std::lock_guard<std::mutex> l(_mutex);

        if (gpu::GpuStatus s = pool.acquire(bytes, _pixels); !s) {
            return s;
        }

        return pool.acquire(2 * bytes, _accumulation);
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

    const KernelFilmPackParams params = {
        film->width, film->height, x1, y1, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(nComps), static_cast<std::uint32_t>(outOffset), outRowStride, 0, 0, 0
    };
    const std::array<gpu::GpuBinding, 2> bindings = { gpu::GpuBinding::storage(0, lease.pixels.buffer()), gpu::GpuBinding::storage(1, out) };

    // Orders the copy after the frame's writes, which earlier submissions on the queue made.
    context.computeBarrier();
    if (gpu::GpuStatus s = context.dispatchBindings(lease.kernels->pack, bindings, std::as_bytes(std::span(&params, 1)), { params.width, params.height, 1 }); !s) {
        return s;
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
    if (status) {
        status = recordPack(film, *context, 0, 0, static_cast<int>(film->width), static_cast<int>(film->height), 4, staging, 0, film->width * 4);
    }
    if (status) {
        status = context->submitAndWait();
    }
    if (status) {
        std::memcpy(out, staging.mapped(), bytes);
    }

    return status;
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

    *status = makeKernel(device, clear, &kernels->clear);
    if (status->ok()) {
        *status = makeKernel(device, scatter, &kernels->scatter);
    }
    if (status->ok()) {
        *status = makeKernel(device, pack, &kernels->pack);
    }

    return status->ok() ? kernels : std::shared_ptr<const UtilityFilmKernels>();
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
#endif // NATRON_HAVE_GPU

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
    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        if (_onDevice) {
            downloadLocked();
        }
    }
    ++_mappers;

    return _pixels.empty() ? nullptr : _pixels.data();
}

void
UtilityRenderBuffer::Unmap()
{
    --_mappers;
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
    return _converged.load();
}

VtValue
UtilityRenderBuffer::GetResource(bool /*multiSampled*/) const
{
#ifdef NATRON_HAVE_GPU
    const std::lock_guard<std::mutex> l(_deviceMutex);

    if (_onDevice && _film) {
        return VtValue(UtilityDeviceResource(_film));
    }
#endif

    return VtValue();
}

std::uint64_t
UtilityRenderBuffer::deviceDownloadCount()
{
    return gDeviceDownloads.load();
}

void
UtilityRenderBuffer::clear(const VtValue& value)
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
    _clearPixel.clear();
    _deviceRestart = true;
    _clearComponents.assign((_format == HdFormatInvalid) ? 0 : HdGetComponentCount(_format), 0.f);
    _sampleSums.assign(static_cast<std::size_t>(_width) * _height * _clearComponents.size(), 0.f);
    _sampleWeights.assign(static_cast<std::size_t>(_width) * _height, -1.f);
    if (value.IsEmpty() || _pixels.empty()) {
        // What the device holds is the buffer as it is, and a frame that follows writes the host's.
        if (_onDevice) {
            downloadLocked();
        }

        return;
    }
    const std::vector<double> components = clearComponents(value);

    if (components.empty()) {
        if (_onDevice) {
            downloadLocked();
        }

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
    for (std::size_t offset = 0; offset < _pixels.size(); offset += pixelSize) {
        std::memcpy(_pixels.data() + offset, pixel.data(), pixelSize);
    }
    _clearPixel = std::move(pixel);
}

void
UtilityRenderBuffer::setConverged(bool converged)
{
    _converged.store(converged);
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
#ifdef NATRON_HAVE_GPU
    _film.reset();
#endif
    _onDevice = false;
}

void
UtilityRenderBuffer::downloadLocked()
{
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
    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        if (_film) {
            _film->releaseIfLost();
        }
        if (_onDevice && _film && !_film->released()) {
            downloadLocked();
        }
        _onDevice = false;
        film = std::move(_film);
    }
    // Every holder lets go too: a strip still to copy its window renders again on the CPU, and a viewer
    // showing the frame keeps the pixels it already has. Recorded copies hold a lease of their own.
    if (film) {
        film->release();
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
UtilityRenderBuffer::beginDeviceFrame(gpu::GpuRecordContext& context,
                                      gpu::GpuKernelRegistry& registry,
                                      gpu::GpuBufferPool& pool,
                                      const std::shared_ptr<const UtilityFilmKernels>& kernels,
                                      const std::shared_ptr<void>& runtimeHold)
{
    const std::lock_guard<std::mutex> l(_deviceMutex);
    const bool restart = _deviceRestart;

    if ((_format != HdFormatFloat32Vec4) || (_width == 0) || (_height == 0) || (_clearPixel.size() != kFilmPixelBytes) || !kernels) {
        return failure("The Utility renderer's colour cannot be held on the GPU");
    }

    const bool usable = _film && !_film->released() && (&_film->device == &registry.device()) && (_film->width == _width) && (_film->height == _height);

    if (!restart && !usable) {
        return failure("The Utility renderer's film left the GPU part way through the frame's samples");
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(_width) * _height * kFilmPixelBytes;
    UtilityDeviceFilm::Lease previous;

    // A film still held elsewhere has a copy of it recorded and not yet run, which must see the frame it was asked for.
    if (!usable || (_film.use_count() > 1)) {
        if (!restart && !_film->lease(&previous)) {
            return failure("The Utility renderer's film was released with its GPU");
        }
        _film.reset();

        std::shared_ptr<UtilityDeviceFilm> film = std::make_shared<UtilityDeviceFilm>(registry, pool, kernels, runtimeHold, _width, _height);

        if (gpu::GpuStatus s = film->allocate(bytes); !s) {
            return s;
        }
        registerFilm(film);
        _film = film;
    }

    UtilityDeviceFilm::Lease lease;

    if (!_film->lease(&lease)) {
        _film.reset();

        return failure("The Utility renderer's film was released with its GPU");
    }
    if (restart) {
        KernelFilmClearParams params = { { 0, 0, 0, 0 }, _width, _height, 0, 0 };

        std::memcpy(params.value, _clearPixel.data(), kFilmPixelBytes);

        const std::array<gpu::GpuBinding, 2> bindings = { gpu::GpuBinding::storage(0, lease.pixels.buffer()), gpu::GpuBinding::storage(1, lease.accumulation.buffer()) };

        if (gpu::GpuStatus s = context.dispatchBindings(lease.kernels->clear, bindings, std::as_bytes(std::span(&params, 1)), { _width, _height, 1 }); !s) {
            return s;
        }
    } else if (previous.pixels) {
        context.computeBarrier();
        if (gpu::GpuStatus s = context.copyBuffer(previous.pixels.buffer(), 0, lease.pixels.buffer(), 0, bytes); !s) {
            return s;
        }
        if (gpu::GpuStatus s = context.copyBuffer(previous.accumulation.buffer(), 0, lease.accumulation.buffer(), 0, 2 * bytes); !s) {
            return s;
        }
        context.hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(previous)));
    }
    context.hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(lease)));
    _onDevice = true;
    _deviceRestart = false;

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
                                       int height)
{
    std::shared_ptr<UtilityDeviceFilm> film;
    KernelFilmScatterParams params = {};

    {
        const std::lock_guard<std::mutex> l(_deviceMutex);

        // A Map() part way through the frame's samples reads the film back; what lands after it is newer.
        if (_film && !_film->released()) {
            film = _film;
            _onDevice = true;
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

    params.filmWidth = film->width;
    params.filmHeight = film->height;
    params.x = x;
    params.y = y;
    params.width = static_cast<std::uint32_t>(width);
    params.count = count;

    const std::array<gpu::GpuBinding, 5> bindings = {
        gpu::GpuBinding::storage(0, hitIds),
        gpu::GpuBinding::storage(1, color),
        gpu::GpuBinding::storage(2, weights),
        gpu::GpuBinding::storage(3, lease.pixels.buffer()),
        gpu::GpuBinding::storage(4, lease.accumulation.buffer()),
    };

    context.computeBarrier();
    if (gpu::GpuStatus s = context.dispatchBindings(lease.kernels->scatter, bindings, std::as_bytes(std::span(&params, 1)), { count, 1, 1 }); !s) {
        return s;
    }
    context.hold(std::make_shared<UtilityDeviceFilm::Lease>(std::move(lease)));

    return gpu::GpuStatus();
}
#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT
