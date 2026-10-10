#pragma once

#include <vulkan/vulkan.h>

#include "vk_mem_alloc.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace gpu {

struct GpuStatus {
    VkResult result = VK_SUCCESS;
    std::string message;

    bool ok() const { return result == VK_SUCCESS; }
    explicit operator bool() const { return ok(); }
};

enum class QueueKind {
    Compute,
    Transfer,
};

using GpuDebugMessageFn = std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types, const char* message)>;

struct GpuDeviceOptions {
    // Empty means: consult NATRON_GPU_DEVICE, then fall back to the ranked pick.
    std::string deviceSelector;
    // NATRON_GPU_VALIDATION=1 also enables it. Silently off when the Khronos layer is not installed.
    bool enableValidation = false;
    // Uses per-kernel descriptor pools even where VK_KHR_push_descriptor is available.
    bool disablePushDescriptor = false;
    // Receives VK_EXT_debug_utils messages (validation layer and loader) from whichever thread made
    // the Vulkan call. Unset means no messenger is installed.
    GpuDebugMessageFn debugMessage;
};

struct GpuDeviceInfo {
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t apiVersion = 0;
    uint32_t physicalDeviceIndex = 0;

    uint32_t computeFamily = 0;
    uint32_t transferFamily = 0;
    // True when no transfer-only family exists and transfers use the compute queue.
    bool transferUsesComputeQueue = false;

    bool validationEnabled = false;
    bool debugMessenger = false;
    bool shaderFloat16 = false;
    bool externalMemoryFd = false;
    bool externalSemaphoreFd = false;
    bool externalMemoryHost = false;
    bool memoryBudget = false;
    bool pushDescriptor = false;
};

class GpuDevice {
public:
    static GpuStatus create(const GpuDeviceOptions& options, std::unique_ptr<GpuDevice>& out);

    ~GpuDevice();
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    const GpuDeviceInfo& info() const { return info_; }
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VmaAllocator allocator() const { return allocator_; }
    uint32_t queueFamily(QueueKind kind) const;

    bool isLost() const { return lost_.load(std::memory_order_acquire); }

    GpuStatus submit(QueueKind kind, std::span<const VkSubmitInfo2> submits, VkFence fence);

    GpuStatus createTimelineSemaphore(uint64_t initialValue, VkSemaphore& out);
    GpuStatus waitSemaphore(VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs);
    GpuStatus waitIdle();

    GpuStatus queryBudget(std::vector<VmaBudget>& out);

    // Funnels a raw Vulkan result through the lost-device latch.
    GpuStatus check(VkResult result, const char* what);

private:
    struct Queue {
        VkQueue handle = VK_NULL_HANDLE;
        std::mutex mutex;
    };

    GpuDevice() = default;

    Queue& queueFor(QueueKind kind);
    GpuStatus lostStatus() const;

    GpuDeviceInfo info_;
    GpuDebugMessageFn debugMessage_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    uint32_t memoryHeapCount_ = 0;

    Queue computeQueue_;
    Queue transferQueue_;

    std::atomic<bool> lost_ { false };
};

} // namespace gpu
