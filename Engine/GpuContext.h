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

#include "Engine/EngineFwd.h"

namespace gpu {
class GpuRuntime;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief What the setting and the NATRON_GPU environment variable resolve to. Force accepts any device,
 * including CPU-type ones such as lavapipe.
 **/
enum class GpuResolvedMode
{
    Off,
    Auto,
    Force
};

/**
 * @brief Measured at startup with one 64 MB round trip per transfer path.
 **/
struct GpuCalibration
{
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

struct GpuContextOptions
{
    GpuResolvedMode mode = GpuResolvedMode::Off;

    // Empty selects automatically.
    std::string device;

    // 0 selects automatically.
    std::uint64_t memoryBudgetMB = 0;

    // Receives the single informational line written when the GPU is unavailable. Unset writes it to the
    // Qt info log.
    std::function<void (const std::string&)> infoLog;
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
class GpuContext
{
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

    GpuResolvedMode mode() const { return _mode; }

    /**
     * @brief Valid once initialization succeeded.
     **/
    GpuCalibration calibration() const;

    std::string deviceName() const;

    /**
     * @brief The runtime, or null while unavailable. Never null once isAvailable() returned true, until destruction.
     **/
    gpu::GpuRuntime* runtime() const;

private:

    struct Shared;

    void initialize();
    void finish(bool available);

    const GpuContextOptions _options;
    const GpuResolvedMode _mode;
    std::shared_ptr<Shared> _shared;
    std::thread _thread;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GPUCONTEXT_H
