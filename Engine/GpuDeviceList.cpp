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

#include "GpuDeviceList.h"

#include <cstring>

#ifdef NATRON_HAVE_GPU
#include <vulkan/vulkan.h>
#endif

NATRON_NAMESPACE_ENTER

std::string
uuidHex(const std::array<uint8_t, 16>& uuid)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : uuid) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xf]);
    }
    return out;
}

std::string
gpuSelectorForDeviceId(const std::vector<GpuDeviceSummary>& devices, const std::string& id)
{
    if (id.empty() || id == "auto") {
        return std::string();
    }
    for (const GpuDeviceSummary& d : devices) {
        if (uuidHex(d.uuid) == id) {
            return std::to_string(d.index);
        }
    }
    return std::string();
}

std::vector<GpuDeviceSummary>
enumerateGpuDevices()
{
    std::vector<GpuDeviceSummary> result;
#ifdef NATRON_HAVE_GPU
    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        return result;
    }

    uint32_t count = 0;
    std::vector<VkPhysicalDevice> devices;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
        devices.resize(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
            devices.clear();
        }
        devices.resize(devices.empty() ? 0 : count);
    }

    for (size_t i = 0; i < devices.size(); ++i) {
        VkPhysicalDeviceIDProperties idProps {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &idProps;
        vkGetPhysicalDeviceProperties2(devices[i], &props);

        GpuDeviceSummary s;
        s.index = static_cast<int>(i);
        s.name = props.properties.deviceName;
        s.type = props.properties.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU
                     ? static_cast<GpuDeviceType>(props.properties.deviceType)
                     : GpuDeviceType::Other;
        std::memcpy(s.uuid.data(), idProps.deviceUUID, VK_UUID_SIZE);
        s.vendorID = props.properties.vendorID;
        s.deviceID = props.properties.deviceID;
        result.push_back(std::move(s));
    }
    vkDestroyInstance(instance, nullptr);
#endif
    return result;
}

NATRON_NAMESPACE_EXIT
