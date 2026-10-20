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

#ifndef NATRON_ENGINE_GPUDEVICELIST_H
#define NATRON_ENGINE_GPUDEVICELIST_H

#include "Global/Macros.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

// Mirrors VkPhysicalDeviceType so callers need no Vulkan header.
enum class GpuDeviceType {
    Other = 0,
    Integrated = 1,
    Discrete = 2,
    Virtual = 3,
    Cpu = 4,
};

struct GpuDeviceSummary {
    // Position in the Vulkan physical-device list, which is what the gpu::GpuDevice selector takes.
    int index = 0;
    std::string name;
    GpuDeviceType type = GpuDeviceType::Other;
    std::array<uint8_t, 16> uuid {};
    uint32_t vendorID = 0;
    uint32_t deviceID = 0;
};

/**
 * @brief Lists the Vulkan devices through a throwaway instance, without creating a gpu::GpuDevice.
 * Empty without NATRON_HAVE_GPU, without a Vulkan loader, or when instance creation fails.
 **/
std::vector<GpuDeviceSummary> enumerateGpuDevices();

/// 32 lowercase hex characters.
std::string uuidHex(const std::array<uint8_t, 16>& uuid);

/**
 * @brief The decimal index selector of the device whose uuidHex equals `id`, or "" for "auto" or an unknown id.
 **/
std::string gpuSelectorForDeviceId(const std::vector<GpuDeviceSummary>& devices, const std::string& id);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GPUDEVICELIST_H
