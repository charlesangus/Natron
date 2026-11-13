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

#ifndef Engine_Nodes_Scene_Utility_UtilityGpuLimits_h
#define Engine_Nodes_Scene_Utility_UtilityGpuLimits_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <cstddef>
#include <cstdint>

#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"

NATRON_NAMESPACE_ENTER

// The float arrays of a ray batch and the uint arrays of its hit ids, as Kernels/UtilityTypes.slang lays them out.
inline constexpr std::size_t kUtilityRayArrays = 8;
inline constexpr std::size_t kUtilityHitArrays = 3;

// Vulkan guarantees 65535 workgroups along x, and the kernels run 64 invocations to a group.
inline constexpr std::uint64_t kUtilityMaxDispatchRays = 65535ull * 64ull;

inline Tracer::Status
classifyUtilityGpuStatus(const gpu::GpuStatus& status,
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

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Scene_Utility_UtilityGpuLimits_h
