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

#include "GpuContext.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <shared_mutex>

#include <QtCore/QDebug>

#include "Engine/HostImageMemory.h"
#include "Engine/Image.h"
#include "Engine/RenderStats.h"
#include "Engine/StartupGLDiag.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
enum InitState {
    eInitPending = 0,
    eInitReady = 1,
    eInitUnavailable = 2
};

std::string
lowerCase(const char* s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    return out;
}
}

struct GpuContext::Shared {
    std::atomic<int> state { eInitPending };
    mutable std::mutex mutex;
    mutable std::condition_variable cond;
    GpuCalibration calibration;
    std::string deviceName;

    // Taken before holders is read or changed, and held through a recreation.
    std::mutex holdMutex;
    int holders = 0;
    std::chrono::steady_clock::time_point nextRecoveryAttempt;

#ifdef NATRON_HAVE_GPU
    // Written before state becomes Ready and only reset by the destructor.
    std::unique_ptr<gpu::GpuRuntime> runtime;

    // The free hook may run on any thread, possibly after the runtime started to go away, so it reaches the
    // runtime only through this pointer, which teardown clears under the exclusive lock.
    struct HookTarget {
        std::shared_mutex mutex;
        gpu::GpuRuntime* runtime = nullptr;
    };
    std::shared_ptr<HookTarget> hookTarget = std::make_shared<HookTarget>();
    int hookId = 0;
    std::size_t previousAlignment = 0;
    bool alignmentChanged = false;
#endif
};

GpuResolvedMode
GpuContext::resolveMode(GpuComputeModeEnum setting,
                        const char* env)
{
    if (env) {
        const std::string v = lowerCase(env);
        if (v == "off") {
            return GpuResolvedMode::Off;
        }
        if (v == "auto") {
            return GpuResolvedMode::Auto;
        }
        if (v == "force") {
            return GpuResolvedMode::Force;
        }
    }

    return setting == eGpuComputeModeOff ? GpuResolvedMode::Off : GpuResolvedMode::Auto;
}

bool
GpuContext::isDeviceAllowed(GpuResolvedMode mode,
                            bool cpuTypeDevice)
{
    switch (mode) {
    case GpuResolvedMode::Off:
        return false;
    case GpuResolvedMode::Auto:
        return !cpuTypeDevice;
    case GpuResolvedMode::Force:
        return true;
    }

    return false;
}

GpuContext::GpuContext(const GpuContextOptions& options)
    : _options(options)
    , _mode(options.mode)
    , _createdAt(std::chrono::steady_clock::now())
    , _shared(std::make_shared<Shared>())
{
#ifdef NATRON_HAVE_GPU
    if (_mode != GpuResolvedMode::Off) {
        _thread = std::thread([this] { initialize(); });
        if (StartupGLDiag::switchOn(StartupGLDiag::Switch::GpuInitSync)) {
            _thread.join();
            StartupGLDiag::logSwitchAction(StartupGLDiag::Switch::GpuInitSync, "Vulkan compute initialisation finished before startup continued");
        }
        return;
    }
#endif
    _shared->state.store(eInitUnavailable);
}

GpuContext::~GpuContext()
{
    if (_thread.joinable()) {
        _thread.join();
    }
#ifdef NATRON_HAVE_GPU
    {
        std::unique_lock<std::shared_mutex> k(_shared->hookTarget->mutex);
        _shared->hookTarget->runtime = nullptr;
    }
    if (_shared->hookId != 0) {
        HostImageMemory::removeFreeHook(_shared->hookId);
    }
    _shared->runtime.reset();
    if (_shared->alignmentChanged) {
        HostImageMemory::setImportAlignment(_shared->previousAlignment);
    }
#endif
}

bool
GpuContext::isAvailable() const
{
    if (_shared->state.load(std::memory_order_acquire) != eInitReady) {
        return false;
    }
#ifdef NATRON_HAVE_GPU
    return !_shared->runtime->isLost();
#else
    return false;
#endif
}

bool
GpuContext::isInitializing() const
{
    return _shared->state.load(std::memory_order_acquire) == eInitPending;
}

bool
GpuContext::waitForInit(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> k(_shared->mutex);

    return _shared->cond.wait_for(k, timeout, [this] { return _shared->state.load() != eInitPending; });
}

GpuCalibration
GpuContext::calibration() const
{
    std::lock_guard<std::mutex> k(_shared->mutex);

    return _shared->calibration;
}

std::string
GpuContext::deviceName() const
{
    std::lock_guard<std::mutex> k(_shared->mutex);

    return _shared->deviceName;
}

gpu::GpuRuntime*
GpuContext::runtime() const
{
#ifdef NATRON_HAVE_GPU
    if (_shared->state.load(std::memory_order_acquire) == eInitReady) {
        return _shared->runtime.get();
    }
#endif

    return nullptr;
}

std::shared_ptr<void>
GpuContext::holdRuntime()
{
    if (!runtime()) {
        return std::shared_ptr<void>();
    }
    std::shared_ptr<Shared> shared = _shared;
    {
        std::lock_guard<std::mutex> k(shared->holdMutex);
        ++shared->holders;
    }

    return std::shared_ptr<void>(static_cast<void*>(shared.get()), [shared](void*) {
        std::lock_guard<std::mutex> k(shared->holdMutex);
        --shared->holders;
    });
}

bool
GpuContext::recoverIfLost()
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuRuntime* rt = runtime();
    if (!rt) {
        return false;
    }
    if (!rt->isLost()) {
        return true;
    }
    std::lock_guard<std::mutex> k(_shared->holdMutex);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if ((_shared->holders > 0) || (now < _shared->nextRecoveryAttempt)) {
        return false;
    }
    if (!rt->isLost()) {
        return true;
    }
    gpu::GpuStatus status;
    {
        // The free hook reaches the transfer, which the recreation replaces.
        std::unique_lock<std::shared_mutex> hook(_shared->hookTarget->mutex);
        status = rt->recreate();
    }
    std::string message;
    if (status) {
        if (_options.memoryBudgetMB > 0) {
            rt->pool().setUserBudget(_options.memoryBudgetMB << 20);
        }
        message = "GPU compute recovered from a device loss";
    } else {
        _shared->nextRecoveryAttempt = now + std::chrono::seconds(10);
        message = "GPU compute could not recover from a device loss: " + status.message;
    }
    if (_options.infoLog) {
        _options.infoLog(message);
    } else {
        qInfo().noquote() << QString::fromStdString(message);
    }

    return status.ok();
#else

    return false;
#endif
}

void
GpuContext::finish(bool available)
{
    {
        std::lock_guard<std::mutex> k(_shared->mutex);
        _shared->state.store(available ? eInitReady : eInitUnavailable, std::memory_order_release);
    }
    _shared->cond.notify_all();
}

#ifdef NATRON_HAVE_GPU

namespace {
// A zero-copy import is limited by the bus alone and a few MB reach its rate, while staging is paced by its
// copy threads and pieces, which only a transfer of several pieces exercises.
constexpr std::size_t kImportCalibrationBytes = std::size_t(16) << 20;
constexpr std::size_t kStagingCalibrationBytes = std::size_t(64) << 20;
// Shifts the pointer off the import alignment so the transfer must take the staging path.
constexpr std::size_t kStagingMisalignment = 64;

struct RoundTrip {
    double uploadBytesPerSecond = 0.;
    double downloadBytesPerSecond = 0.;
    gpu::TransferPath uploadPath = gpu::TransferPath::Staging;
    gpu::TransferPath downloadPath = gpu::TransferPath::Staging;
};

double
bytesPerSecond(std::size_t bytes,
               std::chrono::steady_clock::time_point begin,
               std::chrono::steady_clock::time_point end)
{
    const double seconds = std::max(std::chrono::duration<double>(end - begin).count(), 1e-9);

    return (double)bytes / seconds;
}

struct CalibrationTimings {
    double importMs = 0.;
    double stagingMs = 0.;
};

double
elapsedMs(std::chrono::steady_clock::time_point begin,
          std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

gpu::GpuStatus
roundTrip(gpu::GpuRuntime& runtime,
          gpu::GpuBuffer& buffer,
          void* host,
          std::size_t bytes,
          RoundTrip& out)
{
    using Clock = std::chrono::steady_clock;
    gpu::StreamOptions up;
    up.handoff = gpu::ComputeHandoff::None;
    up.path = &out.uploadPath;
    gpu::TimelineValue uploaded;
    const Clock::time_point t0 = Clock::now();
    if (gpu::GpuStatus s = runtime.upload({ host, bytes }, buffer, 0, uploaded, up); !s) {
        return s;
    }
    if (gpu::GpuStatus s = runtime.wait(uploaded); !s) {
        return s;
    }
    const Clock::time_point t1 = Clock::now();

    gpu::StreamOptions down;
    down.handoff = gpu::ComputeHandoff::None;
    down.path = &out.downloadPath;
    gpu::TimelineValue downloaded;
    if (gpu::GpuStatus s = runtime.download(buffer, 0, { host, bytes }, downloaded, down); !s) {
        return s;
    }
    if (gpu::GpuStatus s = runtime.wait(downloaded); !s) {
        return s;
    }
    const Clock::time_point t2 = Clock::now();
    out.uploadBytesPerSecond = bytesPerSecond(bytes, t0, t1);
    out.downloadBytesPerSecond = bytesPerSecond(bytes, t1, t2);

    return {};
}

// The first trip pays one-time costs (page faults, import pinning, staging allocation), so the second is
// the one kept.
gpu::GpuStatus
steadyRoundTrip(gpu::GpuRuntime& runtime,
                gpu::GpuBuffer& buffer,
                void* host,
                std::size_t bytes,
                RoundTrip& out)
{
    RoundTrip warmup;
    if (gpu::GpuStatus s = roundTrip(runtime, buffer, host, bytes, warmup); !s) {
        return s;
    }

    return roundTrip(runtime, buffer, host, bytes, out);
}

// Regions never stage when the device imports host pointers, so only the path they take is measured.
gpu::GpuStatus
calibrate(gpu::GpuRuntime& runtime,
          GpuCalibration& out,
          CalibrationTimings& timings)
{
    using Clock = std::chrono::steady_clock;
    const bool importing = runtime.transfer().hostImportSupported();
    const std::size_t bytes = importing ? kImportCalibrationBytes : kStagingCalibrationBytes;
    gpu::GpuBufferHandle device;
    if (gpu::GpuStatus s = runtime.acquire(bytes, device); !s) {
        return s;
    }
    const std::size_t hostBytes = bytes + kStagingMisalignment;
    void* host = HostImageMemory::allocate(hostBytes);
    if (!host) {
        return { VK_ERROR_OUT_OF_HOST_MEMORY, "calibration buffer allocation failed" };
    }
    std::memset(host, 0x5a, hostBytes);

    RoundTrip trip;
    const Clock::time_point begin = Clock::now();
    gpu::GpuStatus status = steadyRoundTrip(runtime, device.buffer(), importing ? host : static_cast<char*>(host) + kStagingMisalignment, bytes, trip);
    (importing ? timings.importMs : timings.stagingMs) = elapsedMs(begin, Clock::now());
    HostImageMemory::release(host, hostBytes);
    HostImageMemory::trim();
    if (!status) {
        return status;
    }

    out.hostImport = importing && trip.uploadPath == gpu::TransferPath::HostImport && trip.downloadPath == gpu::TransferPath::HostImport;
    if (out.hostImport) {
        out.hostImportUploadBytesPerSecond = trip.uploadBytesPerSecond;
        out.hostImportDownloadBytesPerSecond = trip.downloadBytesPerSecond;
    } else {
        out.stagingUploadBytesPerSecond = trip.uploadBytesPerSecond;
        out.stagingDownloadBytesPerSecond = trip.downloadBytesPerSecond;
    }
    out.uploadBytesPerSecond = trip.uploadBytesPerSecond;
    out.downloadBytesPerSecond = trip.downloadBytesPerSecond;
    out.unifiedMemory = runtime.device().info().unifiedMemory;

    return {};
}
}

void
GpuContext::initialize()
{
    Shared& shared = *_shared;
    std::string failure;
    std::shared_ptr<Shared::HookTarget> target = shared.hookTarget;
    std::unique_ptr<gpu::GpuRuntime> runtime;

    try {
        if (_options.beforeInit) {
            _options.beforeInit();
        }
        // The cache is switched on at creation, so the hook that drops its imports must exist first. It does
        // nothing until the runtime is published through the hook target.
        shared.hookId = HostImageMemory::addFreeHook([target](void* ptr, std::size_t paddedBytes) {
            std::shared_lock<std::shared_mutex> k(target->mutex);
            if (target->runtime && !target->runtime->isLost()) {
                target->runtime->transfer().forgetHostAllocation(ptr, paddedBytes);
            }
        });

        gpu::GpuRuntimeOptions rtOptions;
        rtOptions.device.deviceSelector = _options.device;
        rtOptions.transfer.cacheHostImports = true;
        // One memcpy thread moves a fraction of the bus rate; the ranges left to staging are those that cannot be
        // imported, and they are worth splitting across several threads in large pieces.
        rtOptions.transfer.copyThreads = 4;
        rtOptions.transfer.stripBytes = VkDeviceSize(32) << 20;
        gpu::GpuStatus status = gpu::GpuRuntime::create(rtOptions, runtime);
        const std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
        if (!status) {
            failure = status.message.empty() ? "no usable Vulkan device" : status.message;
        } else if (!isDeviceAllowed(_mode, runtime->device().info().type == VK_PHYSICAL_DEVICE_TYPE_CPU)) {
            failure = "only a CPU-type Vulkan device is present (" + runtime->device().info().name + ")";
        } else {
            const gpu::GpuTransfer& transfer = runtime->transfer();
            if (transfer.hostImportSupported()) {
                shared.previousAlignment = HostImageMemory::importAlignment();
                HostImageMemory::setImportAlignment((std::size_t)transfer.hostImportAlignment());
                shared.alignmentChanged = true;
            }
            {
                std::unique_lock<std::shared_mutex> k(target->mutex);
                target->runtime = runtime.get();
            }
            GpuCalibration calibration;
            CalibrationTimings timings;
            status = calibrate(*runtime, calibration, timings);
            if (!status) {
                failure = status.message;
            } else {
                if (_options.memoryBudgetMB > 0) {
                    runtime->pool().setUserBudget(_options.memoryBudgetMB << 20);
                }
                {
                    std::lock_guard<std::mutex> k(shared.mutex);
                    shared.calibration = calibration;
                    shared.deviceName = runtime->device().info().name;
                }
                shared.runtime = std::move(runtime);
                finish(true);
                const std::chrono::steady_clock::time_point ready = std::chrono::steady_clock::now();
                qInfo().noquote() << QString::fromLatin1("GPU compute ready after %1 ms (loader, instance, device and runtime %2 ms, "
                                                         "host import calibration %3 ms, staging calibration %4 ms)")
                                         .arg(elapsedMs(_createdAt, ready), 0, 'f', 1)
                                         .arg(elapsedMs(_createdAt, created), 0, 'f', 1)
                                         .arg(timings.importMs, 0, 'f', 1)
                                         .arg(timings.stagingMs, 0, 'f', 1);

                return;
            }
        }
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "unknown error";
    }

    {
        std::unique_lock<std::shared_mutex> k(target->mutex);
        target->runtime = nullptr;
    }
    runtime.reset();

    if (shared.hookId != 0) {
        HostImageMemory::removeFreeHook(shared.hookId);
        shared.hookId = 0;
    }
    if (shared.alignmentChanged) {
        HostImageMemory::setImportAlignment(shared.previousAlignment);
        shared.alignmentChanged = false;
    }
    const std::string message = "GPU compute is unavailable: " + failure;
    if (_options.infoLog) {
        _options.infoLog(message);
    } else {
        qInfo().noquote() << QString::fromStdString(message);
    }
    finish(false);
}

#else // NATRON_HAVE_GPU

void
GpuContext::initialize()
{
}

#endif // NATRON_HAVE_GPU

int
GpuImageLayout::nComps() const
{
    return layer.getNumComponents();
}

std::size_t
GpuImageLayout::pixelBytes() const
{
    if (bounds.isNull()) {
        return 0;
    }

    return (std::size_t)bounds.area() * (std::size_t)std::max(0, nComps()) * sizeof(float);
}

std::size_t
GpuImageLayout::deviceBytes() const
{
    return HostImageMemory::paddedSize(pixelBytes());
}

ImagePtr
GpuImageLayout::newRamImage() const
{
    if (pixelBytes() == 0) {
        return ImagePtr();
    }
    try {
        return std::make_shared<Image>(key, Image::makeParams(rod, bounds, par, mipmapLevel, false, layer, eImageBitDepthFloat, fielding, eStorageModeRAM));
    } catch (const std::bad_alloc&) {
        return ImagePtr();
    }
}

std::size_t
gpuHostTransferBytes(const Image& image)
{
    const RectI bounds = image.getBounds();
    if (bounds.isNull()) {
        return 0;
    }
    const std::size_t bytes = (std::size_t)bounds.area() * image.getComponentsCount() * getSizeOfForBitDepth(image.getBitDepth());
    if ((image.getStorageMode() != eStorageModeRAM) || (bytes < HostImageMemory::minPooledBytes())) {
        return bytes;
    }

    return HostImageMemory::paddedSize(bytes);
}

struct GpuDeviceImage::Imp {
    mutable std::mutex mutex;
    GpuImageLayout layout;
    gpu::GpuRuntime* runtime = nullptr;
    std::weak_ptr<RenderStats> stats;
    int pins = 0;
    bool evicted = false;
    bool downloaded = false;
    std::function<void()> onEvict;
#ifdef NATRON_HAVE_GPU
    gpu::GpuBufferHandle buffer;

    void markEvictableLocked()
    {
        if (onEvict && buffer && (pins == 0) && !evicted) {
            buffer.markEvictable(onEvict);
        }
    }

    ImagePtr downloadLocked()
    {
        if (!buffer || !runtime) {
            return ImagePtr();
        }
        ImagePtr image = layout.newRamImage();
        if (!image) {
            return ImagePtr();
        }
        const std::size_t bytes = gpuHostTransferBytes(*image);
        if ((bytes == 0) || (bytes > buffer.size())) {
            return ImagePtr();
        }
        {
            Image::WriteAccess access(image.get());
            unsigned char* data = access.pixelAt(layout.bounds.x1, layout.bounds.y1);
            if (!data) {
                return ImagePtr();
            }
            gpu::StreamOptions options;
            options.handoff = gpu::ComputeHandoff::Submit;
            gpu::TimelineValue done;
            if (!runtime->download(buffer.buffer(), 0, { data, bytes }, done, options) || !runtime->wait(done)) {
                return ImagePtr();
            }
        }
        downloaded = true;
        if (RenderStatsPtr s = stats.lock()) {
            s->noteDownload(layout.pixelBytes());
        }

        return image;
    }
#endif
};

GpuDeviceImage::GpuDeviceImage()
    : _imp(new Imp)
{
}

GpuDeviceImage::~GpuDeviceImage()
{
}

std::shared_ptr<GpuDeviceImage>
GpuDeviceImage::create(gpu::GpuRuntime& runtime,
                       const gpu::GpuBufferHandle& buffer,
                       const GpuImageLayout& layout,
                       const RenderStatsPtr& stats)
{
#ifdef NATRON_HAVE_GPU
    if (!buffer || (buffer.size() < layout.deviceBytes())) {
        return std::shared_ptr<GpuDeviceImage>();
    }
    std::shared_ptr<GpuDeviceImage> image(new GpuDeviceImage);
    image->_imp->runtime = &runtime;
    image->_imp->buffer = buffer;
    image->_imp->layout = layout;
    image->_imp->stats = stats;

    return image;
#else
    Q_UNUSED(runtime);
    Q_UNUSED(buffer);
    Q_UNUSED(layout);
    Q_UNUSED(stats);

    return std::shared_ptr<GpuDeviceImage>();
#endif
}

std::size_t
GpuDeviceImage::bytes() const
{
#ifdef NATRON_HAVE_GPU
    std::lock_guard<std::mutex> k(_imp->mutex);

    return _imp->buffer ? (std::size_t)_imp->buffer.size() : 0;
#else
    return 0;
#endif
}

RectI
GpuDeviceImage::bounds() const
{
    return _imp->layout.bounds;
}

int
GpuDeviceImage::nComps() const
{
    return _imp->layout.nComps();
}

ImagePtr
GpuDeviceImage::downloadToRam()
{
#ifdef NATRON_HAVE_GPU
    std::lock_guard<std::mutex> k(_imp->mutex);
    if (_imp->pins > 0) {
        return ImagePtr();
    }

    return _imp->downloadLocked();
#else
    return ImagePtr();
#endif
}

ImagePtr
GpuDeviceImage::evictToRam()
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuBufferHandle freed;
    ImagePtr image;
    {
        std::lock_guard<std::mutex> k(_imp->mutex);
        if ((_imp->pins > 0) || _imp->evicted) {
            return ImagePtr();
        }
        image = _imp->downloadLocked();
        if (!image) {
            return ImagePtr();
        }
        _imp->evicted = true;
        _imp->onEvict = nullptr;
        std::swap(freed, _imp->buffer);
    }

    return image;
#else
    return ImagePtr();
#endif
}

const GpuImageLayout&
GpuDeviceImage::layout() const
{
    return _imp->layout;
}

gpu::GpuRuntime*
GpuDeviceImage::runtime() const
{
    return _imp->runtime;
}

gpu::GpuBuffer*
GpuDeviceImage::pin()
{
#ifdef NATRON_HAVE_GPU
    std::lock_guard<std::mutex> k(_imp->mutex);
    if (!_imp->buffer || _imp->evicted || _imp->downloaded) {
        return nullptr;
    }
    if (_imp->pins++ == 0) {
        _imp->buffer.clearEvictable();
    }
    _imp->buffer.touch();

    return &_imp->buffer.buffer();
#else
    return nullptr;
#endif
}

void
GpuDeviceImage::unpin()
{
#ifdef NATRON_HAVE_GPU
    std::lock_guard<std::mutex> k(_imp->mutex);
    if (_imp->pins <= 0) {
        return;
    }
    if (--_imp->pins == 0) {
        _imp->markEvictableLocked();
    }
#endif
}

void
GpuDeviceImage::setEvictable(const std::function<void()>& onEvict)
{
#ifdef NATRON_HAVE_GPU
    std::lock_guard<std::mutex> k(_imp->mutex);
    _imp->onEvict = onEvict;
    _imp->markEvictableLocked();
#else
    Q_UNUSED(onEvict);
#endif
}

NATRON_NAMESPACE_EXIT
