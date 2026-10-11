#pragma once

#include <vulkan/vulkan.h>

#include "vk_mem_alloc.h"

#include <cstdint>
#include <memory>

#include "GpuDevice.h"

namespace gpu {

enum class GpuBufferKind {
    DeviceLocal,
    HostImported,
    Exportable,
    Staging,
};

constexpr VkBufferUsageFlags kGpuBufferDefaultUsage
    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// Move-only owner of a VkBuffer and its memory. It must be destroyed before the GpuDevice it came from.
// Every factory leaves `out` empty on failure and reports through the returned status.
class GpuBuffer {
public:
    GpuBuffer() = default;
    ~GpuBuffer() { reset(); }
    GpuBuffer(GpuBuffer&& other) noexcept { moveFrom(other); }
    GpuBuffer& operator=(GpuBuffer&& other) noexcept
    {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    static GpuStatus createDeviceLocal(GpuDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out);

    // Wraps caller-owned memory without copying; the caller keeps it alive until this buffer
    // is destroyed. A rejected or driver-refused import sets
    // GpuStatus::fallbackToStaging. A size above maxMemoryAllocationSize is rejected, never
    // shortened; callers split larger ranges themselves.
    static GpuStatus createHostImported(GpuDevice& dev, void* hostPtr, VkDeviceSize size, VkBufferUsageFlags usage,
                                        GpuBuffer& out);

    // Backed by a dedicated allocation from a private pool, exportable as an OPAQUE_FD handle.
    static GpuStatus createExportable(GpuDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out);

    // Host-visible and persistently mapped.
    static GpuStatus createStaging(GpuDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out);

    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    GpuBufferKind kind() const { return kind_; }
    explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }

    // Host pointer for Staging and HostImported buffers, null for the others.
    void* mapped() const { return mapped_; }
    VkDeviceMemory memory() const { return memory_; }
    VkDeviceSize memoryOffset() const { return memoryOffset_; }

    void reset();

private:
    void moveFrom(GpuBuffer& other) noexcept;

    GpuDevice* device_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VmaPool pool_ = nullptr;
    // Owned memory only for HostImported; VMA-backed kinds read it from the allocation.
    VkDeviceMemory importedMemory_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize memoryOffset_ = 0;
    VkDeviceSize size_ = 0;
    GpuBufferKind kind_ = GpuBufferKind::DeviceLocal;
    void* mapped_ = nullptr;
    // VMA keeps a pointer to this for the pool's lifetime, so it lives on the heap across moves.
    std::unique_ptr<VkExportMemoryAllocateInfo> exportInfo_;
};

} // namespace gpu
