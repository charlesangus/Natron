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

#ifndef NATRON_ENGINE_GPUCONTEXT_H
#define NATRON_ENGINE_GPUCONTEXT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Enums.h"
#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Engine/DeviceImage.h"
#include "Engine/EngineFwd.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

namespace gpu {
class GpuBuffer;
class GpuBufferHandle;
class GpuRuntime;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief What the setting and the NATRON_GPU environment variable resolve to. Force accepts any device,
 * including CPU-type ones such as lavapipe.
 **/
enum class GpuResolvedMode {
    Off,
    Auto,
    Force
};

/**
 * @brief Measured at startup with one 64 MB round trip per transfer path.
 **/
struct GpuCalibration {
    double uploadBytesPerSecond = 0.;
    double downloadBytesPerSecond = 0.;
    // Zero when the device cannot import host pointers.
    double hostImportUploadBytesPerSecond = 0.;
    double hostImportDownloadBytesPerSecond = 0.;
    double stagingUploadBytesPerSecond = 0.;
    double stagingDownloadBytesPerSecond = 0.;
    bool unifiedMemory = false;
    bool hostImport = false;
};

struct GpuContextOptions {
    GpuResolvedMode mode = GpuResolvedMode::Off;

    // Empty selects automatically.
    std::string device;

    // 0 selects automatically.
    std::uint64_t memoryBudgetMB = 0;

    // Receives the informational line written when the GPU is unavailable, and one per device-loss recovery
    // attempt, so it is called for as long as the context lives. Unset writes them to the Qt info log.
    std::function<void(const std::string&)> infoLog;

    // Runs on the initialization thread before the device is created. Tests hold initialization back with it.
    std::function<void()> beforeInit;
};

/**
 * @brief The application's GPU runtime, brought up on a background thread so startup never waits for the driver.
 *
 * Without NATRON_HAVE_GPU the context is never available. Every accessor is thread-safe and non-blocking;
 * only waitForInit() blocks, and only for as long as the caller asks.
 *
 * While available, a free hook on HostImageMemory drops the transfer's cached host imports of a buffer just
 * before it returns to the OS. The cache calls HostImageMemory::trimTo() under its lock, so the hook runs
 * under it. This cannot deadlock: GpuTransfer never calls back into the engine, so what it waits for is
 * device work that finishes by itself, and the hook takes no lock that the context holds while it
 * initializes or while it is queried. The cost is a bounded stall of the thread that trims, only when a
 * freed buffer is still in use by a transfer.
 **/
class GpuContext {
public:
    /**
     * @brief Starts initialization on a background thread unless @p options.mode is Off.
     **/
    explicit GpuContext(const GpuContextOptions& options);

    /**
     * @brief Joins the initialization thread, unregisters the free hook and destroys the runtime. No render may
     * still use the runtime.
     **/
    ~GpuContext();

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    /**
     * @brief The setting combined with an NATRON_GPU value ("off", "auto" or "force", case-insensitive). A null
     * or unrecognised env value leaves the setting in charge.
     **/
    static GpuResolvedMode resolveMode(GpuComputeModeEnum setting, const char* env);

    /**
     * @brief Whether a device may be used under @p mode. Auto refuses CPU-type devices.
     **/
    static bool isDeviceAllowed(GpuResolvedMode mode, bool cpuTypeDevice);

    /**
     * @brief True once initialization succeeded and the device has not been lost.
     **/
    bool isAvailable() const;

    /**
     * @brief True while the background initialization is still running.
     **/
    bool isInitializing() const;

    /**
     * @brief Blocks until initialization finished or @p timeout elapsed. Returns whether it finished.
     **/
    bool waitForInit(std::chrono::milliseconds timeout) const;

    /**
     * @brief When the context was created, which is when its initialization started.
     **/
    std::chrono::steady_clock::time_point createdAt() const { return _createdAt; }

    GpuResolvedMode mode() const { return _mode; }

    /**
     * @brief Valid once initialization succeeded.
     **/
    GpuCalibration calibration() const;

    std::string deviceName() const;

    /**
     * @brief The runtime once initialization succeeded, lost or not; null otherwise. Never null once isAvailable()
     * returned true, until destruction.
     **/
    gpu::GpuRuntime* runtime() const;

    /**
     * @brief Keeps recoverIfLost() from recreating the runtime until the returned token is gone, for a holder of
     * handles or images of the runtime, which a recreation would invalidate. Null while unavailable.
     **/
    std::shared_ptr<void> holdRuntime();

    /**
     * @brief Recreates the runtime when it was lost and no holdRuntime() token is alive. A failed recreation is not
     * tried again for a few seconds, since every render start asks. Returns whether the runtime is usable.
     **/
    bool recoverIfLost();

private:
    struct Shared;

    void initialize();
    void finish(bool available);

    const GpuContextOptions _options;
    const GpuResolvedMode _mode;
    const std::chrono::steady_clock::time_point _createdAt;
    std::shared_ptr<Shared> _shared;
    std::thread _thread;
};

/**
 * @brief A float image as a device buffer holds it: the interleaved rows of bounds, with nComps() channels of layer per
 * pixel and nothing between rows, which is also how a RAM Image lays out its pixels. The rest is what the RAM image it
 * comes back as is made with.
 **/
struct GpuImageLayout {
    ImageLayerDesc layer;
    RectD rod;
    RectI bounds;
    unsigned int mipmapLevel = 0;
    double par = 1.;
    ImageFieldingOrderEnum fielding = eImageFieldingOrderNone;

    // Consumers such as OfxClipInstance tell the images they fetched apart by the time and view of their key.
    ImageKey key;

    int nComps() const;

    std::size_t pixelBytes() const;

    /**
     * @brief What a device buffer holding the image must have, so that a host-import download of a RAM image of the
     * same layout, which moves gpuHostTransferBytes() of it, fits.
     **/
    std::size_t deviceBytes() const;

    /**
     * @brief A new float RAM image of this layout, outside the image cache; null when it cannot be allocated. A large
     * one is page-aligned and padded, so that it can be imported by the device.
     **/
    ImagePtr newRamImage() const;
};

/**
 * @brief How many bytes from the first pixel of image a transfer moves: its pixels, rounded up to the import alignment
 * when its buffer comes from HostImageMemory's padded pool, so that the transfer can import it rather than stage it.
 **/
std::size_t gpuHostTransferBytes(const Image& image);

/**
 * @brief A DeviceImage in a buffer of the GPU runtime's pool.
 *
 * After a download, its pixels belong to the transfer queue's family, so compute must not read them again; pin()
 * refuses from that point, and a consumer takes the RAM copy instead. Thread-safe.
 **/
class GpuDeviceImage
    : public DeviceImage {
public:
    virtual ~GpuDeviceImage();

    /**
     * @brief Holds buffer, whose first layout.deviceBytes() bytes hold the image. stats, when set, counts the
     * downloads.
     **/
    static std::shared_ptr<GpuDeviceImage> create(gpu::GpuRuntime& runtime,
                                                  const gpu::GpuBufferHandle& buffer,
                                                  const GpuImageLayout& layout,
                                                  const RenderStatsPtr& stats);

    virtual std::size_t bytes() const OVERRIDE FINAL;
    virtual RectI bounds() const OVERRIDE FINAL;
    virtual int nComps() const OVERRIDE FINAL;
    virtual ImagePtr downloadToRam() OVERRIDE FINAL;

    /**
     * @brief Fails while the image is pinned.
     **/
    virtual ImagePtr evictToRam() OVERRIDE FINAL;

    const GpuImageLayout& layout() const;

    gpu::GpuRuntime* runtime() const;

    /**
     * @brief Keeps the pixels on the device, neither evicted nor downloaded, until the matching unpin(), and returns
     * their buffer; null, without pinning, once they were evicted or downloaded.
     **/
    gpu::GpuBuffer* pin();

    void unpin();

    /**
     * @brief Lets the pool reclaim the buffer under memory pressure while the image is not pinned. onEvict runs on the
     * thread that needs the memory and must make the image's holder evict it to RAM.
     **/
    void setEvictable(const std::function<void()>& onEvict);

private:
    GpuDeviceImage();

    struct Imp;
    std::unique_ptr<Imp> _imp;
};

typedef std::shared_ptr<GpuDeviceImage> GpuDeviceImagePtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GPUCONTEXT_H
