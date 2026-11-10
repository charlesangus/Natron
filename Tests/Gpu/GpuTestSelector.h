#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

#include "Engine/GpuDeviceList.h"

namespace gputest {

// The ranked device pick never chooses a CPU device, so a host with only lavapipe (CI) has to name it.
// Returns the index of the first CPU device when NATRON_GPU_DEVICE is unset, no hardware device is present and
// NATRON_TEST_REQUIRE_GPU_HARDWARE is not 1; otherwise empty, which leaves the choice to GpuDevice.
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
    int cpuIndex = -1;
    for (const NATRON_NAMESPACE::GpuDeviceSummary& d : NATRON_NAMESPACE::enumerateGpuDevices()) {
        if (d.type != NATRON_NAMESPACE::GpuDeviceType::Cpu) {
            return std::string();
        }
        if (cpuIndex < 0) {
            cpuIndex = d.index;
        }
    }
    return cpuIndex < 0 ? std::string() : std::to_string(cpuIndex);
}

} // namespace gputest
