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
    // Set when the caller can retry the same request through a staging copy instead of treating it as fatal.
    bool fallbackToStaging = false;

    bool ok() const { return result == VK_SUCCESS; }
    explicit operator bool() const { return ok(); }
};

enum class QueueKind {
    Compute,
    Transfer,
};

using GpuDebugMessageFn = std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types, const char* message)>;

// Makes one chosen operation fail with the VkResult a driver would return, so the real error handling runs.
// Operations are counted from 1 per device: every GpuDevice::submit and every GpuRuntime::acquire.
// The fault fires once, on exactly operation `atOperation`.
struct GpuFaultPlan {
    enum class Kind {
        Off,
        DeviceLost,
        OutOfMemory,
    };
    Kind kind = Kind::Off;
    uint64_t atOperation = 0;

    // Parses "lost@N" or "oom@N" with N >= 1.
    static bool parse(const std::string& text, GpuFaultPlan& out);
};

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
    // Test override: caps the reported maxStorageBufferRange when smaller than the device limit. 0 = none.
    uint64_t maxStorageBufferRange = 0;
    // Size of one host<->device copy chunk.
    uint64_t copyChunkSize = 512ull << 20;
    GpuFaultPlan fault;
    // Runs once, on the thread that first sees VK_ERROR_DEVICE_LOST, with no device lock held.
    std::function<void()> onDeviceLost;
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

    // Effective values: the device limit, or the GpuDeviceOptions override when that is smaller.
    uint64_t maxStorageBufferRange = 0;
    uint64_t copyChunkSize = 0;
    // 0 when VK_EXT_external_memory_host is absent.
    uint64_t minImportedHostPointerAlignment = 0;
    uint64_t deviceLocalHeapSize = 0;
    uint64_t maxMemoryAllocationSize = 0;
    // Integrated GPU, or a device-local and host-visible heap that covers all device memory.
    bool unifiedMemory = false;
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

    // Counts one operation against the fault plan and returns the injected failure when it is due.
    GpuStatus countOperation();

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

    GpuFaultPlan fault_;
    std::atomic<uint64_t> operations_ { 0 };
    std::function<void()> onDeviceLost_;
    std::atomic<bool> lost_ { false };
};

} // namespace gpu
