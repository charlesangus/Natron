#pragma once

#include "GpuDevice.h"
#include "GpuTransfer.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    // Device buffer copied to host memory, which glTexSubImage2D reads in place.
    Readback,
};

const char* toString(GlHandoffPath path);

// The value a hand-off override variable gives `path` by: "zerocopy", "hostsync" or "readback".
const char* overrideName(GlHandoffPath path);

// The path `value` names as overrideName() does, ignoring case; empty for null or any other value.
std::optional<GlHandoffPath> parseOverride(const char* value);

// Whether `s` reports the device lost: a hand-off then stops trying paths and falls back until the
// device is recovered.
bool isDeviceLost(const GpuStatus& s);

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

// What chooseHandoffPath() settled on.
struct GlHandoffChoice {
    // The first path that built; empty when none did or the device was lost.
    std::optional<GlHandoffPath> path;
    // The device was lost while a path was being built.
    bool lost = false;
    // For every path stepped over, "<overrideName> unavailable: missing ..." or "<overrideName> failed: ...",
    // separated by "; ".
    std::string reasons;
};

// The hand-off ladder: steps from `first` down to Readback, quietly passing over each path `caps` does not
// support or `build` fails to make, and stops at the first that builds or at a device loss.
GlHandoffChoice chooseHandoffPath(const GlInteropCaps& caps,
                                  GlHandoffPath first,
                                  const std::function<GpuStatus(GlHandoffPath)>& build);

// Vulkan semaphores that another API can import, as both GL hand-off directions order their work with.
GpuStatus createExportableSemaphore(GpuDevice& device, VkSemaphore* out);

// The GL half of sharing Vulkan memory and semaphores, in either direction: what GlInterop does to see
// Vulkan's buffers, for any producer or consumer on the GL side. Every call needs the GL context
// current that the entry points were loaded from; the GL names it hands out belong to that context.
class GlExternal {
public:
    // Fails when the context lacks GL_EXT_memory_object_fd's entry points.
    static GpuStatus load(const GlProcLoader& loader, std::unique_ptr<GlExternal>& out);

    ~GlExternal();
    GlExternal(const GlExternal&) = delete;
    GlExternal& operator=(const GlExternal&) = delete;

    // Imports the first `bytes` of `buffer`, made by GpuBuffer::createExportable() at least `bytes` plus
    // kImportSlackBytes long, as a new GL buffer object, which the new memory object *glMemory backs.
    GpuStatus importBuffer(GpuDevice& device, const GpuBuffer& buffer, VkDeviceSize bytes, unsigned int* glMemory,
                           unsigned int* glBuffer);
    // Imports `semaphore`, made by createExportableSemaphore(), as a new GL semaphore object.
    GpuStatus importSemaphore(GpuDevice& device, VkSemaphore semaphore, unsigned int* glSemaphore);
    // Deletes what importBuffer() and importSemaphore() made; zero names are skipped and the names zeroed.
    void deleteBuffer(unsigned int* glMemory, unsigned int* glBuffer);
    void deleteSemaphore(unsigned int* glSemaphore);

    // Signals glSemaphore once every GL command issued so far is done, handing glBuffer over to the
    // waiting API, and flushes so that the signal is submitted.
    GpuStatus signal(unsigned int glSemaphore, unsigned int glBuffer);
    // Blocks until every GL command issued so far is done.
    GpuStatus finish();

    // The GL error raised since the last check, as a failed status naming `what`, or success.
    GpuStatus error(const char* what);

    // The margin llvmpipe needs between a buffer and the memory imported for it.
    static constexpr VkDeviceSize kImportSlackBytes = 256;

    struct Api;

private:
    GlExternal() = default;

    std::unique_ptr<Api> api_;
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
    // The frame last produced into `slot`.
    const GlFrame& frame(int slot) const { return slots_[slot].frame; }

    // Picks a slot no upload() is pending on, preferring the one GL did not read last, and submits
    // `record` on the compute queue after GL's previous read of that slot and after every value in
    // `waits`. With hostCopy the frame is also copied to host memory on every path, as the Readback path always
    // does. Stores the chosen slot in *slot. A frame larger than capacity() fails with
    // VK_ERROR_OUT_OF_POOL_MEMORY, and a failed status from `record` is returned, both without
    // submitting anything.
    GpuStatus produce(const GlFrame& frame,
                      const GlRecordFn& record,
                      std::span<const TimelineValue> waits,
                      int* slot,
                      bool hostCopy = false);

    // The slot the next produce() will use, or -1 while every slot awaits upload().
    int nextSlot() const;

    // Points *data at the host copy that the last produce(hostCopy = true) into `slot` left, in the slot's mapped
    // memory, and *size at its length, without waiting: VK_TIMEOUT while the GPU is still writing it. The next
    // produce() into the slot overwrites it and destruction unmaps it, so the view is valid only until either.
    GpuStatus hostCopyView(int slot, const unsigned char** data, VkDeviceSize* size);

    // Blocks the CPU until the last produce() into `slot` has finished on the GPU.
    GpuStatus waitProduced(int slot, uint64_t timeoutNs = UINT64_MAX);

    // Consumes the pending produce() of `slot` into the frame-sized rectangle at (x, y) of level 0
    // of a GL_TEXTURE_2D whose internal format accepts the frame's pixel format.
    GpuStatus upload(int slot, unsigned int texture, int x, int y);

    // Consumes the pending produce() of `slot` without uploading it, so the slot can be produced into again.
    GpuStatus discard(int slot);

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
        bool hostCopyValid = false;
        bool pendingUpload = false;
        bool glSignaled = false;
        bool releasedToGl = false;
    };

    GlInterop() = default;

    GpuStatus initVulkan(bool exportable, bool semaphores);
    GpuStatus ensureReadback(Slot& slot);
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
