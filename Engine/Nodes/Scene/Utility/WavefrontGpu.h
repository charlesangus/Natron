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

#ifndef Engine_Nodes_Scene_Utility_WavefrontGpu_h
#define Engine_Nodes_Scene_Utility_WavefrontGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"

namespace gpu {
class GpuBufferPool;
class GpuDevice;
struct GpuDeviceInfo;
class GpuKernelRegistry;
class GpuPipelineCache;
class GpuRuntime;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief The GPU the utility renderer may use: the application's runtime. runtime is null when there is
 * none, and unavailable then says why, or when stopped is set, because cancel was set while the
 * application's GPU was still starting. hold keeps the application from recreating the runtime under
 * whoever keeps it.
 **/
struct UtilityGpu {
    gpu::GpuRuntime* runtime = nullptr;
    std::shared_ptr<void> hold;
    std::string unavailable;
    bool stopped = false;
};

// Waits a while for the application's GPU to finish starting, returning as soon as cancel, when given, is set.
UtilityGpu acquireUtilityGpu(const std::atomic<bool>* cancel = nullptr);

// The memory the application's GPU pool may use; 0 while there is no usable GPU. Never waits for it.
std::uint64_t utilityGpuBudget();

#ifdef NATRON_HAVE_GPU

/**
 * @brief Draws frames on the device with the SPIR-V builds of the kernels WavefrontCpu runs on the CPU,
 * tracing with a GpuTracer. Each batch of whole rows, of about a quarter of a million pixels so that a
 * stop is seen within one batch on a slow device, is one record context: ray generation, intersection
 * and shading back to back, then one submission. Rays, hit surfaces and the shading tables stay on the
 * device; only the AOVs the film binds on the host, and the hit ids that tell its misses, are read back.
 *
 * Before its first frame the executor has the driver compile every kernel it dispatches, without the time
 * limit the frame's submissions have; a render stopped meanwhile returns at once and the next one waits for
 * the rest. Destroying the executor does not wait for the kernel being compiled; the GpuContext going away
 * does, for a while, and leaves its runtime alive if the compilation still runs.
 *
 * A device failure never throws: render() returns false and status() says why. The pool must outlive
 * the executor, and the registry and its device must outlive the executor and its warm-up, which
 * waitForWarmUps() waits for; a GpuContext waits for it itself.
 **/
class WavefrontGpu : public WavefrontExecutor {
public:
    // runtimeHold, when given, is kept by the device films the executor leaves in the render buffers. Creates every
    // pipeline, which the driver compiles in the call, so it can take seconds and has no stop of its own.
    // pipelines, when given, keeps Raster mode's graphics pipelines from frame to frame; without it each pass
    // builds its own. It must outlive the executor.
    WavefrontGpu(gpu::GpuKernelRegistry& registry,
                 gpu::GpuBufferPool& pool,
                 const std::shared_ptr<void>& runtimeHold = std::shared_ptr<void>(),
                 gpu::GpuPipelineCache* pipelines = nullptr);
    virtual ~WavefrontGpu();

    // Waits up to timeout for the warm-ups of executors gone before them to end on device; false if some still run.
    static bool waitForWarmUps(const gpu::GpuDevice& device,
                               std::chrono::milliseconds timeout);

    // Empty when the device can render, else the Vulkan extensions and features it lacks.
    static std::string missingSupport(const gpu::GpuRuntime& runtime);

    // Returns false when stop ended it early or the device failed, as status() tells.
    virtual bool render(const UtilityScene::Snapshot& snapshot,
                        const WavefrontFrame& frame,
                        WavefrontFilm& film,
                        const std::atomic<bool>& stop,
                        int threads) OVERRIDE;

    // Why the last render() returned false, or the reason the executor cannot run at all.
    Tracer::Outcome status() const;

    // Whether the last render() failed while the tracer built the scene on the device, as opposed to
    // while it drew the frame; only then is the scene, not one frame, what the device cannot hold.
    bool sceneFailed() const;

    bool deviceLost() const;

    std::string deviceName() const;

    // Empty when the device can draw frames in Raster mode, else what it lacks.
    static std::string missingRasterSupport(const gpu::GpuDeviceInfo& info);

    // The same for this executor's device, or why its Raster mode kernel could not be made.
    std::string missingRasterSupport() const;

    // Lowers the widest and tallest tile a raster pass draws in one visibility buffer, so that tests split small
    // frames. Never raises it past the device's image limit; 0 removes the limit.
    void limitRasterTile(std::uint32_t texels);

    // Makes a Raster frame whose camera hits alone decide its pixels resolve its cutouts and shade its lights and
    // bounces anyway, as any other frame does, so that tests compare the two.
    void alwaysTraceRasterFrames(bool on);

    // Whether the last render() drew a Raster frame from its camera hits alone: every surface unlit, or nothing
    // lighting it and no bounce, so that no structure was built, no ray traced, and its cutouts were left undrawn
    // by the visibility pass.
    bool drewUntraced() const;

    // The compute and graphics queue submissions the last render() drew its pass in, the uploads of the scene and
    // of the frame's settings aside.
    std::size_t submissions() const;

    // What the executor's tracer has built and refit since it was made.
    Tracer::Stats tracerStats() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_WavefrontGpu_h
