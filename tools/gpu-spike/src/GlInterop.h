#pragma once

#include "GpuDevice.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gpu {

// Resolves GL entry points for the context that is current when the GlInterop calls are made.
using GlProcLoader = std::function<void*(const char*)>;

enum class GlHandoffPath
{
    // Exported Vulkan memory imported as a GL buffer, ordered by exported semaphores.
    ZeroCopy,
    // Exported memory without semaphores: the CPU waits on a Vulkan fence and on glFinish.
    ZeroCopyHostSync,
    // Device buffer copied to host memory, then uploaded through an ordinary GL PBO.
    Readback,
};

const char* toString(GlHandoffPath path);

struct GlInteropCaps
{
    bool vkExternalMemoryFd = false;
    bool vkExternalSemaphoreFd = false;
    bool vkBufferExportable = false;
    bool vkSemaphoreExportable = false;
    bool glMemoryObject = false;
    bool glMemoryObjectFd = false;
    bool glSemaphore = false;
    bool glSemaphoreFd = false;
    bool uuidMatch = false;

    std::array<uint8_t, 16> vkDeviceUuid{};
    std::array<uint8_t, 16> vkDriverUuid{};
    std::array<uint8_t, 16> glDeviceUuid{};
    std::array<uint8_t, 16> glDriverUuid{};
    std::string glVendor;
    std::string glRenderer;
    std::string glVersion;

    GlHandoffPath best = GlHandoffPath::Readback;
    // Names every extension, capability or UUID check that keeps ZeroCopy from being available.
    std::string missing;

    bool supports(GlHandoffPath path) const;
};

// Hands a width x height RGBA32F image produced by Vulkan into GL texture level 0.
// All GL-touching calls, including destruction, need the creating GL context current.
class GlInterop
{
public:
    static GpuStatus probe(GpuDevice& device, const GlProcLoader& loader, GlInteropCaps& out);

    static GpuStatus create(GpuDevice& device,
                            const GlProcLoader& loader,
                            uint32_t width,
                            uint32_t height,
                            GlHandoffPath path,
                            std::unique_ptr<GlInterop>& out);

    ~GlInterop();
    GlInterop(const GlInterop&) = delete;
    GlInterop& operator=(const GlInterop&) = delete;

    GlHandoffPath path() const { return path_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Tightly packed RGBA32F rows; the producer writes it from the compute queue.
    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    unsigned int glBuffer() const { return glBuffer_; }

    // Submits the caller's writes to buffer() on the compute queue, after GL's previous read.
    GpuStatus produce(const std::function<void(VkCommandBuffer)>& record);

    // Blocks the CPU until the last produce() has finished on the GPU.
    GpuStatus waitProduced(uint64_t timeoutNs = UINT64_MAX);

    // Consumes the last produce() into an RGBA32F GL_TEXTURE_2D of at least width x height.
    GpuStatus upload(unsigned int texture);

private:
    struct Gl;

    GlInterop() = default;

    GpuStatus initVulkan(bool exportable, bool semaphores);
    GpuStatus initGl(bool semaphores);
    GpuStatus glError(const char* what);

    GpuDevice* device_ = nullptr;
    std::unique_ptr<Gl> gl_;
    GlHandoffPath path_ = GlHandoffPath::Readback;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkDeviceSize size_ = 0;

    VkExportMemoryAllocateInfo exportInfo_{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    VmaPool pool_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VmaAllocation readbackAllocation_ = nullptr;
    void* readbackMapped_ = nullptr;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore vkReady_ = VK_NULL_HANDLE;
    VkSemaphore glDone_ = VK_NULL_HANDLE;

    unsigned int glMemory_ = 0;
    unsigned int glBuffer_ = 0;
    unsigned int glReady_ = 0;
    unsigned int glDoneGl_ = 0;

    bool submitted_ = false;
    bool fenceWaited_ = true;
    bool pendingUpload_ = false;
    bool glSignaled_ = false;
    bool releasedToGl_ = false;
};

} // namespace gpu
