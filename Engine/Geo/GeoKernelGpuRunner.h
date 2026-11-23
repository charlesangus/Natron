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

#ifndef Engine_Geo_GeoKernelGpuRunner_h
#define Engine_Geo_GeoKernelGpuRunner_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoKernelBinding.h"
#include "Engine/Kernel/KernelGpuCodegen.h"

namespace gpu {
class GpuRuntime;
}

NATRON_NAMESPACE_ENTER

struct GeoKernelGpuRunResult {
    GeoKernelRunResult result;
    bool ranOnGpu = false;
    // Why a run that had a device went to the CPU: the plan's reason, the compiler's message or the
    // device's error. Empty when it ran on the GPU or had no device.
    std::string fallback;
    // Set when fallback is the GPU compiler's rejection of plan.source, which every later run of
    // the same plan would meet again.
    bool compilerRejected = false;
};

/**
 * @brief What one element of program costs a GPU run, in the units of
 * gpu::GpuRecordContext::chargeWork: its instructions and its column transfers.
 **/
std::uint64_t geoKernelGpuElementWork(const KernelProgram& program, const KernelGpuPlan& plan);

/**
 * @brief Whether GPU placement sends a run of program over elements elements to the GPU, by the
 * weight rule image regions follow (gpuWeightAccepts): the elements count as pixels by the words
 * they move, four to a pixel, the instructions as point-node weight, and a loop makes the run
 * Heavy, as its cost per element has no bound the program states.
 **/
bool geoKernelGpuPlaced(const KernelProgram& program, const KernelGpuPlan& plan, std::size_t elements);

/**
 * @brief Runs program, compiled against GeoKernelBinding(runOver), on runtime when plan (its plan
 * against geoKernelGpuTarget()) runs on the GPU, in page-aligned chunks that fit the device's
 * storage buffer limit, each dispatched as bounded batches that the timeout and options.aborted
 * apply to one at a time. The kernel is compiled before any value is gathered. A CPU-only plan, a
 * null runtime and every GPU failure other than an abort run it on the CPU through geoKernelRun
 * instead; a device error is reported to runtime first. A plan that stores nothing returns
 * input 0 unchanged.
 **/
GeoKernelGpuRunResult geoKernelRunGpu(const KernelProgram& program,
                                      const KernelGpuPlan& plan,
                                      GeoAttribClassEnum runOver,
                                      std::span<const GeoDetail* const> inputs,
                                      const GeoKernelRunOptions& options,
                                      gpu::GpuRuntime* runtime);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoKernelGpuRunner_h
