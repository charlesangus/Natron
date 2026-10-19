#pragma once

#include "GpuDevice.h"
#include "GpuTransfer.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace gpu {

// Resolves GL entry points for the context that is current when the GlInterop calls are made.
using GlProcLoader = std::function<void*(const char*)>;

enum class GlHandoffPath {
    // Exported Vulkan memory imported as a GL buffer, ordered by exported semaphores.
    ZeroCopy,
    // Exported memory without semaphores: the CPU waits on a Vulkan fence and on glFinish.
    ZeroCopyHostSync,
    // Device buffer copied to host memory, then uploaded through an ordinary GL PBO.
    Readback,
};

const char* toString(GlHandoffPath path);

struct GlInteropCaps {
    bool vkExternalMemoryFd = false;
    bool vkExternalSemaphoreFd = false;
    bool vkBufferExportable = false;
    bool vkSemaphoreExportable = false;
    bool glMemoryObject = false;
    bool glMemoryObjectFd = false;
    bool glSemaphore = false;
    bool glSemaphoreFd = false;
    bool uuidMatch = false;

    std::array<uint8_t, 16> vkDeviceUuid {};
    std::array<uint8_t, 16> vkDriverUuid {};
    std::array<uint8_t, 16> glDeviceUuid {};
    std::array<uint8_t, 16> glDriverUuid {};
    std::string glVendor;
    std::string glRenderer;
    std::string glVersion;

    GlHandoffPath best = GlHandoffPath::Readback;
    // Names every extension, capability or UUID check that keeps ZeroCopy from being available.
    std::string missing;

    bool supports(GlHandoffPath path) const;
};

enum class GlPixelFormat {
    // Tightly packed RGBA float32, uploaded as GL_RGBA / GL_FLOAT.
    Rgba32F,
    // One byte each of B, G, R, A in memory order, uploaded as GL_BGRA / GL_UNSIGNED_INT_8_8_8_8_REV.
    Bgra8,
};

uint32_t bytesPerPixel(GlPixelFormat format);

struct GlFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    GlPixelFormat format = GlPixelFormat::Rgba32F;

    VkDeviceSize bytes() const { return VkDeviceSize(width) * height * bytesPerPixel(format); }
};

// Records the producer's writes of one frame into `dst`, tightly packed rows from offset 0.
using GlRecordFn = std::function<GpuStatus(VkCommandBuffer cb, VkBuffer dst)>;

// Hands images produced by Vulkan into GL textures through kSlots buffers, so Vulkan can fill one
// while GL still reads the other. All GL-touching calls, including destruction, need the creating
// GL context current.
class GlInterop {
public:
    static constexpr int kSlots = 2;

    static GpuStatus probe(GpuDevice& device, const GlProcLoader& loader, GlInteropCaps& out);

    // Every slot holds up to capacityBytes; a frame of any size and format that fits may use it.
    static GpuStatus create(GpuDevice& device,
                            const GlProcLoader& loader,
                            VkDeviceSize capacityBytes,
                            GlHandoffPath path,
                            std::unique_ptr<GlInterop>& out);

    ~GlInterop();
    GlInterop(const GlInterop&) = delete;
    GlInterop& operator=(const GlInterop&) = delete;

    GlHandoffPath path() const { return path_; }
    VkDeviceSize capacity() const { return capacity_; }

    VkBuffer buffer(int slot) const { return slots_[slot].buffer; }
    unsigned int glBuffer(int slot) const { return slots_[slot].glBuffer; }
    // The frame last produced into `slot`.
    const GlFrame& frame(int slot) const { return slots_[slot].frame; }

    // Picks a slot no upload() is pending on, preferring the one GL did not read last, and submits
    // `record` on the compute queue after GL's previous read of that slot and after every value in
    // `waits`. Stores the chosen slot in *slot. A frame larger than capacity() fails with
    // VK_ERROR_OUT_OF_POOL_MEMORY, and a failed status from `record` is returned, both without
    // submitting anything.
    GpuStatus produce(const GlFrame& frame,
                      const GlRecordFn& record,
                      std::span<const TimelineValue> waits,
                      int* slot);

    // Blocks the CPU until the last produce() into `slot` has finished on the GPU.
    GpuStatus waitProduced(int slot, uint64_t timeoutNs = UINT64_MAX);

    // Consumes the pending produce() of `slot` into the frame-sized rectangle at (x, y) of level 0
    // of a GL_TEXTURE_2D whose internal format accepts the frame's pixel format.
    GpuStatus upload(int slot, unsigned int texture, int x, int y);

private:
    struct Gl;

    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = nullptr;
        void* readbackMapped = nullptr;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore vkReady = VK_NULL_HANDLE;
        VkSemaphore glDone = VK_NULL_HANDLE;

        unsigned int glMemory = 0;
        unsigned int glBuffer = 0;
        unsigned int glReady = 0;
        unsigned int glDoneGl = 0;

        GlFrame frame;
        bool inFlight = false;
        bool pendingUpload = false;
        bool glSignaled = false;
        bool releasedToGl = false;
    };

    GlInterop() = default;

    GpuStatus initVulkan(bool exportable, bool semaphores);
    GpuStatus initVulkanSlot(Slot& slot, bool exportable, bool semaphores);
    GpuStatus initGlSlot(Slot& slot, bool semaphores);
    GpuStatus glError(const char* what);

    GpuDevice* device_ = nullptr;
    std::unique_ptr<Gl> gl_;
    GlHandoffPath path_ = GlHandoffPath::Readback;
    VkDeviceSize capacity_ = 0;

    VkExportMemoryAllocateInfo exportInfo_ { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    VmaPool pool_ = nullptr;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots_ {};
    int lastUploaded_ = -1;
};

} // namespace gpu
