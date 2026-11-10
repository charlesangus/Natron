#pragma once

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "GpuDevice.h"

namespace gputest {

// The ranked device pick never chooses a CPU device, so a host whose Vulkan devices are all CPU or none of whose
// hardware devices qualify (CI with lavapipe) has to name the CPU device. Returns its index when the ranked pick
// fails for missing features, NATRON_GPU_DEVICE is unset and NATRON_TEST_REQUIRE_GPU_HARDWARE is not 1;
// otherwise empty, which leaves the choice to GpuDevice.
inline std::string
cpuFallbackSelector()
{
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU_HARDWARE");
    if (require && std::strcmp(require, "1") == 0) {
        return std::string();
    }
    const char* selected = std::getenv("NATRON_GPU_DEVICE");
    if (selected && *selected) {
        return std::string();
    }
    static const std::string cached = [] {
        std::unique_ptr<gpu::GpuDevice> probe;
        gpu::GpuDeviceOptions o;
        if (gpu::GpuDevice::create(o, probe).result != VK_ERROR_FEATURE_NOT_PRESENT) {
            return std::string();
        }
        const std::vector<VkPhysicalDeviceType> types = gpu::enumeratePhysicalDeviceTypes();
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i] == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                return std::to_string(i);
            }
        }
        return std::string();
    }();
    return cached;
}

} // namespace gputest
