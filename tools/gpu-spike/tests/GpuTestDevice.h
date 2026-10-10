#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "GpuDevice.h"

// Device creation shared by every GPU test binary.
//
// Environment:
//   GPU_SPIKE_NO_VALIDATION=1        do not request the Khronos validation layer
//   GPU_SPIKE_NO_PUSH_DESCRIPTOR=1   force the descriptor-pool path in GpuKernel
//   GPU_SPIKE_REQUIRE_HARDWARE=1     fail instead of running on a CPU device (lavapipe)
namespace gputest {

inline bool envSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

inline std::atomic<uint64_t>& validationErrors()
{
    static std::atomic<uint64_t> n{0};
    return n;
}

inline gpu::GpuDeviceOptions deviceOptions()
{
    gpu::GpuDeviceOptions o;
    o.enableValidation = !envSet("GPU_SPIKE_NO_VALIDATION");
    o.disablePushDescriptor = envSet("GPU_SPIKE_NO_PUSH_DESCRIPTOR");
    o.debugMessage = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                        const char* message) {
        const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        // The loader reports ICDs it could not load as general errors; only validation counts.
        const bool validation = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
        if (error && validation) {
            ++validationErrors();
        }
        std::fprintf(stderr, "[vulkan %s%s] %s\n", validation ? "validation " : "", error ? "error" : "warning",
                     message);
    };
    return o;
}

inline gpu::GpuStatus createDevice(std::unique_ptr<gpu::GpuDevice>& out, gpu::GpuDeviceOptions o = deviceOptions())
{
    gpu::GpuStatus s = gpu::GpuDevice::create(o, out);
    if (!s) {
        return s;
    }
    if (envSet("GPU_SPIKE_REQUIRE_HARDWARE") && out->info().type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        const std::string name = out->info().name;
        out.reset();
        return {VK_ERROR_INITIALIZATION_FAILED,
                "selected device '" + name + "' is a CPU device but GPU_SPIKE_REQUIRE_HARDWARE=1"};
    }
    std::printf("vulkan device: %s | validation=%d debugMessenger=%d pushDescriptor=%d\n", out->info().name.c_str(),
                out->info().validationEnabled, out->info().debugMessenger, out->info().pushDescriptor);
    return s;
}

// Fails the current test, or the suite when called from TearDownTestSuite, for every
// validation error since the last call.
class ValidationCheck
{
public:
    ValidationCheck() : seen_(validationErrors().load()) {}

    void expectClean(const char* where)
    {
        const uint64_t now = validationErrors().load();
        EXPECT_EQ(now - seen_, 0u) << "Vulkan validation errors during " << where << " (see stderr)";
        seen_ = now;
    }

private:
    uint64_t seen_;
};

} // namespace gputest
