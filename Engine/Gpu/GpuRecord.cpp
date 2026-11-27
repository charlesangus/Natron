#include "GpuRecord.h"

#include "GpuPipelineCache.h"
#include "GpuSizeClass.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace gpu {

namespace {
    std::atomic<uint64_t> gMaxStorageBufferBytesForTests { 0 };
    std::atomic<uint64_t> gBatchWorkForTests { 0 };
    std::atomic<uint64_t> gLargestBatchWorkForTests { 0 };
    std::mutex gBatchFinishedHookMutex;
    std::function<void()> gBatchFinishedHook;
    std::atomic<bool> gHasBatchFinishedHook { false };

    // A device's speed spans a factor of hundreds, between a software rasterizer and a discrete GPU, so a batch size
    // is learned from how long batches take. Until then the minimum is used, small enough that the slowest device
    // still gives a short submit, at the cost of a little idling on a fast one.
    constexpr uint64_t kMinBatchWork = 1ull << 24;
    constexpr uint64_t kMaxBatchWork = 1ull << 34;
    constexpr uint64_t kBatchTargetNs = 100'000'000ull;

    // A paced batch waits behind whatever else is on the queue, which GpuTransfer::wait tolerates this long too.
    constexpr uint64_t kPacedBatchTimeoutNs = 60'000'000'000ull;
    constexpr uint64_t kStopPollNs = 20'000'000ull;

    GpuStatus stopped()
    {
        return { VK_INCOMPLETE, "GpuRecordContext: stopped" };
    }

    std::atomic<uint32_t> gMaxDrawIndirectCountForTests { 0 };

    constexpr uint32_t kRasterDescriptorPoolSets = 64;

    GpuStatus abortedStatus(const char* where)
    {
        GpuStatus s { VK_INCOMPLETE, std::string(where) + ": aborted" };
        s.aborted = true;
        return s;
    }

    void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, const VkImageMemoryBarrier2* image = nullptr)
    {
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = srcStage;
        mb.srcAccessMask = srcAccess;
        mb.dstStageMask = dstStage;
        mb.dstAccessMask = dstAccess;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        if (image) {
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = image;
        }
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkImageMemoryBarrier2 attachmentBarrier(const GpuTexture& texture, VkPipelineStageFlags2 srcStage,
                                            VkAccessFlags2 srcAccess, VkImageLayout from, VkPipelineStageFlags2 dstStage,
                                            VkAccessFlags2 dstAccess, VkImageLayout to)
    {
        VkImageMemoryBarrier2 ib { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        ib.srcStageMask = srcStage;
        ib.srcAccessMask = srcAccess;
        ib.dstStageMask = dstStage;
        ib.dstAccessMask = dstAccess;
        ib.oldLayout = from;
        ib.newLayout = to;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = texture.image();
        ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return ib;
    }

    constexpr VkPipelineStageFlags2 kDrawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
        | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
} // namespace

GpuStatus
GpuRecordContext::create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out, GpuBufferPool* pool,
                         GpuPipelineCache* pipelines, GpuTextureCache* textures)
{
    return createOn(QueueKind::Compute, registry, out, pool, pipelines, textures);
}

GpuStatus
GpuRecordContext::createRaster(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                               GpuBufferPool* pool, GpuPipelineCache* pipelines, GpuTextureCache* textures)
{
    if (!registry.device().info().rasterFloatBlend) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "GpuRecordContext::createRaster: the device has no graphics queue that can blend float colour attachments" };
    }
    return createOn(QueueKind::Graphics, registry, out, pool, pipelines, textures);
}

GpuStatus
GpuRecordContext::createOn(QueueKind queue, GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                           GpuBufferPool* pool, GpuPipelineCache* pipelines, GpuTextureCache* textures)
{
    GpuDevice& device = registry.device();
    if (device.isLost()) {
        return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::create: device lost" };
    }
    device.reapRetired();
    std::unique_ptr<GpuRecordContext> ctx(new GpuRecordContext(queue, registry, pool, pipelines, textures));
    registry.recordingBegan();
    // From here the destructor balances recordingBegan(), including on the error paths.
    if (GpuStatus s = registry.commandPoolForThisThread(ctx->pool_, queue); !s) {
        return s;
    }
    if (queue == QueueKind::Graphics && device.info().pushDescriptor) {
        ctx->pushDescriptorFn_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device.device(), "vkCmdPushDescriptorSetKHR"));
        if (!ctx->pushDescriptorFn_) {
            return { VK_ERROR_EXTENSION_NOT_PRESENT, "GpuRecordContext::createRaster: vkCmdPushDescriptorSetKHR unavailable" };
        }
    }

    if (GpuStatus s = ctx->beginCommandBuffer(ctx->cmd_); !s) {
        return s;
    }
    ctx->open_ = true;
    out = std::move(ctx);
    return {};
}

GpuRecordContext::~GpuRecordContext()
{
    GpuDevice& device = registry_.device();
    // Without a command buffer the work may still be pending after a loss, reading the descriptor sets.
    if (cmd_ && !device.hasAbandonedWork()) {
        for (VkDescriptorPool pool : descriptorPools_) {
            vkDestroyDescriptorPool(device.device(), pool, nullptr);
        }
    }
    if (cmd_) {
        vkFreeCommandBuffers(device.device(), pool_, 1, &cmd_);
    }
    for (const FinishedBatch& batch : finished_) {
        vkFreeCommandBuffers(device.device(), pool_, 1, &batch.cmd);
    }
    if (!retired_) {
        registry_.recordingEnded();
    }
}

GpuStatus
GpuRecordContext::beginCommandBuffer(VkCommandBuffer& out)
{
    GpuDevice& device = registry_.device();
    VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (GpuStatus s = device.check(vkAllocateCommandBuffers(device.device(), &ai, &out), "vkAllocateCommandBuffers"); !s) {
        return s;
    }
    VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return device.check(vkBeginCommandBuffer(out, &bi), "vkBeginCommandBuffer");
}

GpuStatus
GpuRecordContext::chargeWork(uint64_t work)
{
    if (GpuStatus s = recordable("GpuRecordContext::chargeWork"); !s) {
        return s;
    }
    work_ += work;
    if (work_ < batchWork()) {
        return {};
    }
    GpuDevice& device = registry_.device();
    VkCommandBuffer next = VK_NULL_HANDLE;
    GpuStatus status = beginCommandBuffer(next);
    if (status) {
        status = device.check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    }
    if (!status) {
        if (next) {
            vkFreeCommandBuffers(device.device(), pool_, 1, &next);
        }
        return status;
    }
    finished_.push_back({ cmd_, work_ });
    if (gBatchWorkForTests.load(std::memory_order_relaxed) != 0) {
        uint64_t largest = gLargestBatchWorkForTests.load(std::memory_order_relaxed);
        while ((work_ > largest) && !gLargestBatchWorkForTests.compare_exchange_weak(largest, work_, std::memory_order_relaxed)) {
        }
    }
    cmd_ = next;
    work_ = 0;
    return {};
}

uint64_t
GpuRecordContext::batchWork() const
{
    const uint64_t forced = gBatchWorkForTests.load(std::memory_order_relaxed);
    if (forced != 0) {
        return forced;
    }
    const uint64_t learned = registry_.learnedBatchWork_.load(std::memory_order_relaxed);
    return (learned != 0) ? learned : kMinBatchWork;
}

void
GpuRecordContext::setBatchWorkForTests(uint64_t work)
{
    gBatchWorkForTests.store(work, std::memory_order_relaxed);
    gLargestBatchWorkForTests.store(0, std::memory_order_relaxed);
}

uint64_t
GpuRecordContext::largestBatchWorkForTests()
{
    return gLargestBatchWorkForTests.load(std::memory_order_relaxed);
}

void
GpuRecordContext::setBatchFinishedHookForTests(const std::function<void()>& hook)
{
    std::lock_guard<std::mutex> lock(gBatchFinishedHookMutex);
    gBatchFinishedHook = hook;
    gHasBatchFinishedHook.store(bool(hook), std::memory_order_release);
}

GpuStatus
GpuRecordContext::recordable(const char* where) const
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": context already submitted" };
    }
    if (aborted_) {
        return abortedStatus(where);
    }
    if (rasterTarget_) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": a raster pass is open" };
    }
    return {};
}

GpuStatus
GpuRecordContext::checkAbort(const char* where)
{
    if (GpuStatus s = recordable(where); !s) {
        return s;
    }
    if (abortCheck_ && abortCheck_()) {
        aborted_ = true;
        return abortedStatus(where);
    }
    return {};
}

void
GpuRecordContext::setAbortCheck(std::function<bool()> aborted)
{
    abortCheck_ = std::move(aborted);
}

GpuStatus
GpuRecordContext::dispatch(GpuKernelId kernel, std::span<const GpuBuffer* const> buffers,
                           std::span<const std::byte> push, std::array<uint32_t, 3> threads)
{
    if (GpuStatus s = checkAbort("GpuRecordContext::dispatch"); !s) {
        return s;
    }
    GpuKernel* k = nullptr;
    if (GpuStatus s = registry_.get(kernel, k); !s) {
        return s;
    }
    std::vector<VkBuffer> handles(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        if (!buffers[i] || !*buffers[i]) {
            return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::dispatch: empty buffer at binding " + std::to_string(i) };
        }
        if (GpuStatus s = checkBindable("GpuRecordContext::dispatch", (uint32_t)i, buffers[i]->size()); !s) {
            return s;
        }
        handles[i] = buffers[i]->buffer();
    }
    touched_.insert(touched_.end(), handles.begin(), handles.end());
    return k->record(cmd_, handles, push, threads);
}

bool
GpuRecordContext::outsideComputeFamily() const
{
    const GpuDeviceInfo& info = registry_.device().info();
    return isRaster() && info.graphicsFamily != info.computeFamily;
}

GpuStatus
GpuRecordContext::checkTextureBindings(const char* where, std::span<const GpuBinding> bindings) const
{
    if (!outsideComputeFamily()) {
        return {};
    }
    for (const GpuBinding& b : bindings) {
        if (b.kind == GpuBinding::Kind::Texture) {
            return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": a raster context outside the compute family samples no textures; textures belong to the compute family" };
        }
    }
    return {};
}

void
GpuRecordContext::touchBindings(std::span<const GpuBinding> bindings)
{
    for (const GpuBinding& b : bindings) {
        if (b.buffer != VK_NULL_HANDLE) {
            touched_.push_back(b.buffer);
        }
    }
}

GpuStatus
GpuRecordContext::dispatchBindings(GpuKernelId kernel, std::span<const GpuBinding> bindings,
                                   std::span<const std::byte> push, std::array<uint32_t, 3> threads)
{
    if (GpuStatus s = checkAbort("GpuRecordContext::dispatchBindings"); !s) {
        return s;
    }
    if (GpuStatus s = checkTextureBindings("GpuRecordContext::dispatchBindings", bindings); !s) {
        return s;
    }
    for (const GpuBinding& b : bindings) {
        if (b.kind != GpuBinding::Kind::StorageBuffer) {
            continue;
        }
        if (GpuStatus s = checkBindable("GpuRecordContext::dispatchBindings", b.binding, b.bufferSize); !s) {
            return s;
        }
    }
    GpuKernel* k = nullptr;
    if (GpuStatus s = registry_.get(kernel, k); !s) {
        return s;
    }
    touchBindings(bindings);
    return k->recordBindings(cmd_, bindings, push, k->groupCount(threads));
}

GpuStatus
GpuRecordContext::runtimeKernel(std::string_view source, ShaderStage stage, std::string_view entry,
                                std::shared_ptr<GpuKernel>& out)
{
    out.reset();
    if (!pipelines_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::runtimeKernel: context has no pipeline cache" };
    }
    return pipelines_->get(source, stage, entry, out);
}

GpuStatus
GpuRecordContext::dispatchBindings(const std::shared_ptr<GpuKernel>& kernel, std::span<const GpuBinding> bindings,
                                   std::span<const std::byte> push, std::array<uint32_t, 3> threads)
{
    if (GpuStatus s = checkAbort("GpuRecordContext::dispatchBindings"); !s) {
        return s;
    }
    if (!kernel) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::dispatchBindings: no kernel" };
    }
    if (GpuStatus s = checkTextureBindings("GpuRecordContext::dispatchBindings", bindings); !s) {
        return s;
    }
    for (const GpuBinding& b : bindings) {
        if (b.kind != GpuBinding::Kind::StorageBuffer) {
            continue;
        }
        if (GpuStatus s = checkBindable("GpuRecordContext::dispatchBindings", b.binding, b.bufferSize); !s) {
            return s;
        }
    }
    if (std::find(kernels_.begin(), kernels_.end(), kernel) == kernels_.end()) {
        kernels_.push_back(kernel);
        registry_.noteExternalKernel(kernel);
    }
    touchBindings(bindings);
    return kernel->recordBindings(cmd_, bindings, push, kernel->groupCount(threads));
}

GpuStatus
GpuRecordContext::texture(const GpuTextureDesc& desc, std::span<const float> texels, const GpuTexture*& out)
{
    out = nullptr;
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::texture: context already submitted" };
    }
    if (outsideComputeFamily()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::texture: a raster context outside the compute family takes no textures; they are uploaded on the compute queue" };
    }
    std::unique_ptr<GpuTexture> created;
    if (GpuStatus s = GpuTexture::create(registry_.device(), desc, texels, created); !s) {
        if (s.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            outOfMemory_ = true;
        }
        return s;
    }
    textures_.push_back(std::move(created));
    out = textures_.back().get();
    return {};
}

GpuStatus
GpuRecordContext::cachedTexture(const std::string& key, const GpuTextureDesc& desc, std::span<const float> texels,
                                const GpuTexture*& out)
{
    out = nullptr;
    if (outsideComputeFamily()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::cachedTexture: a raster context outside the compute family takes no textures; they are uploaded on the compute queue" };
    }
    if (!textureCache_) {
        return texture(desc, texels, out);
    }
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::cachedTexture: context already submitted" };
    }
    std::shared_ptr<const GpuTexture> held;
    if (GpuStatus s = textureCache_->acquire(key, desc, texels, held); !s) {
        if (s.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            outOfMemory_ = true;
        }
        return s;
    }
    cachedTextures_.push_back(held);
    out = held.get();
    return {};
}

GpuStatus
GpuRecordContext::uniform(std::span<const std::byte> data, const GpuUniformBuffer*& out)
{
    out = nullptr;
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::uniform: context already submitted" };
    }
    GpuUniformBuffer buffer;
    if (GpuStatus s = GpuUniformBuffer::create(registry_.device(), data, buffer); !s) {
        if (s.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            outOfMemory_ = true;
        }
        return s;
    }
    uniforms_.push_back(std::move(buffer));
    out = &uniforms_.back();
    return {};
}

void
GpuRecordContext::computeBarrier()
{
    if (!open_ || rasterTarget_) {
        return;
    }
    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

uint64_t
GpuRecordContext::maxStorageBufferBytes() const
{
    const uint64_t device = registry_.device().info().maxStorageBufferRange;
    const uint64_t cap = gMaxStorageBufferBytesForTests.load(std::memory_order_relaxed);
    return (cap != 0) ? std::min(device, cap) : device;
}

uint64_t
GpuRecordContext::maxScratchBytes() const
{
    return largestSizeWithin(maxStorageBufferBytes());
}

uint64_t
GpuRecordContext::maxScratchBytesFor(uint64_t deviceRange)
{
    const uint64_t cap = gMaxStorageBufferBytesForTests.load(std::memory_order_relaxed);
    return largestSizeWithin((cap != 0) ? std::min(deviceRange, cap) : deviceRange);
}

void
GpuRecordContext::setMaxStorageBufferBytesForTests(uint64_t bytes)
{
    gMaxStorageBufferBytesForTests.store(bytes, std::memory_order_relaxed);
}

uint32_t
GpuRecordContext::maxDrawIndirectCount() const
{
    const uint32_t device = registry_.device().info().maxDrawIndirectCount;
    const uint32_t cap = gMaxDrawIndirectCountForTests.load(std::memory_order_relaxed);
    return (cap != 0) ? std::min(device, cap) : device;
}

void
GpuRecordContext::setMaxDrawIndirectCountForTests(uint32_t count)
{
    gMaxDrawIndirectCountForTests.store(count, std::memory_order_relaxed);
}

// The descriptors bind whole buffers, and one over the range is invalid however little of it the kernel reads.
GpuStatus
GpuRecordContext::checkBindable(const char* where, uint32_t binding, VkDeviceSize bytes) const
{
    const uint64_t range = maxStorageBufferBytes();
    if (bytes > range) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, std::string(where) + ": the " + std::to_string(bytes) + "-byte buffer at binding " + std::to_string(binding) + " is over the " + std::to_string(range) + "-byte storage buffer range" };
    }
    return {};
}

GpuStatus
GpuRecordContext::scratch(VkDeviceSize size, GpuBuffer*& out)
{
    out = nullptr;
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::scratch: context already submitted" };
    }
    if (!bufferPool_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::scratch: context has no buffer pool" };
    }
    GpuBufferHandle handle;
    if (GpuStatus s = bufferPool_->acquire(size, handle); !s) {
        if (s.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            outOfMemory_ = true;
        }
        return s;
    }
    held_.push_back(handle);
    out = &held_.back().buffer();
    return {};
}

GpuStatus
GpuRecordContext::updateBuffer(const GpuBuffer& dst, VkDeviceSize offset, std::span<const std::byte> data)
{
    if (GpuStatus s = recordable("GpuRecordContext::updateBuffer"); !s) {
        return s;
    }
    if (!dst || data.empty() || data.size() > 65536 || (data.size() % 4) != 0 || (offset % 4) != 0
        || offset + data.size() > dst.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::updateBuffer: invalid range" };
    }
    touched_.push_back(dst.buffer());
    vkCmdUpdateBuffer(cmd_, dst.buffer(), offset, data.size(), data.data());
    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &dep);
    return {};
}

GpuStatus
GpuRecordContext::acquireFromTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                                      VkDeviceSize bytes)
{
    if (GpuStatus s = recordable("GpuRecordContext::acquireFromTransfer"); !s) {
        return s;
    }
    if (outsideComputeFamily()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::acquireFromTransfer: a raster context outside the compute family takes uploads through a compute context" };
    }
    touched_.push_back(buffer.buffer());
    transfer.recordComputeAcquire(cmd_, buffer, offset, bytes);
    return {};
}

GpuStatus
GpuRecordContext::releaseToTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                                    VkDeviceSize bytes)
{
    if (GpuStatus s = recordable("GpuRecordContext::releaseToTransfer"); !s) {
        return s;
    }
    if (outsideComputeFamily()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::releaseToTransfer: a raster context outside the compute family hands downloads to a compute context" };
    }
    touched_.push_back(buffer.buffer());
    transfer.recordComputeRelease(cmd_, buffer, offset, bytes);
    return {};
}

GpuStatus
GpuRecordContext::drawable(const char* where)
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": context already submitted" };
    }
    if (aborted_) {
        return abortedStatus(where);
    }
    if (!rasterTarget_) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": no raster pass is open" };
    }
    if (abortCheck_ && abortCheck_()) {
        aborted_ = true;
        return abortedStatus(where);
    }
    return {};
}

GpuStatus
GpuRecordContext::beginRaster(const GpuTexture& attachment, const std::array<float, 4>& clear)
{
    if (!isRaster()) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "GpuRecordContext::beginRaster: not a raster context" };
    }
    if (GpuStatus s = recordable("GpuRecordContext::beginRaster"); !s) {
        return s;
    }
    if (!attachment.isAttachment()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::beginRaster: the texture is not a colour attachment" };
    }

    // The draws read vertex, index and indirect data that earlier dispatches and copies wrote. The attachment's
    // old texels are discarded, so a copy of an earlier pass need only finish first.
    const VkImageMemoryBarrier2 toAttachment = attachmentBarrier(
        attachment, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    memoryBarrier(cmd_,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | kDrawStages,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, kDrawStages,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT
                      | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  &toAttachment);

    const uint32_t width = attachment.desc().width;
    const uint32_t height = attachment.desc().height;
    VkRenderingAttachmentInfo colour { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    colour.imageView = attachment.view();
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (size_t i = 0; i < 4; ++i) {
        colour.clearValue.color.float32[i] = clear[i];
    }
    VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { width, height } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd_, &ri);
    rasterTarget_ = &attachment;
    rendered_ = nullptr;
    return {};
}

GpuStatus
GpuRecordContext::rasterPipeline(GpuRasterId raster, GpuRasterBlend blend, std::shared_ptr<GpuRasterPipeline>& out)
{
    out.reset();
    const VkFormat format = rasterTarget_->format();
    if (pipelines_) {
        if (GpuStatus s = pipelines_->raster(registry_, raster, format, blend, out); !s) {
            return s;
        }
        if (std::find(rasterPipelines_.begin(), rasterPipelines_.end(), out) == rasterPipelines_.end()) {
            rasterPipelines_.push_back(out);
        }
        return {};
    }
    const auto key = std::make_tuple(static_cast<uint32_t>(raster), format, blend);
    if (auto it = ownRasters_.find(key); it != ownRasters_.end()) {
        out = it->second;
        return {};
    }
    const GpuRasterDesc* desc = registry_.rasterDesc(raster);
    if (!desc) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext: unknown raster id " + std::to_string(static_cast<uint32_t>(raster)) };
    }
    std::unique_ptr<GpuRasterPipeline> created;
    if (GpuStatus s = GpuRasterPipeline::create(registry_.device(), *desc, format, blend, created); !s) {
        return s;
    }
    out = std::shared_ptr<GpuRasterPipeline>(std::move(created));
    ownRasters_.emplace(key, out);
    rasterPipelines_.push_back(out);
    return {};
}

GpuStatus
GpuRecordContext::rasterDescriptorSet(VkDescriptorSetLayout layout, uint32_t storageBuffers, VkDescriptorSet& out)
{
    GpuDevice& device = registry_.device();
    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    if (!descriptorPools_.empty()) {
        ai.descriptorPool = descriptorPools_.back();
        const VkResult r = vkAllocateDescriptorSets(device.device(), &ai, &out);
        if (r == VK_SUCCESS) {
            return {};
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            return device.check(r, "vkAllocateDescriptorSets");
        }
    }
    VkDescriptorPoolSize size { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kRasterDescriptorPoolSets * std::max<uint32_t>(storageBuffers, 4) };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = kRasterDescriptorPoolSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (GpuStatus s = device.check(vkCreateDescriptorPool(device.device(), &pi, nullptr, &pool), "vkCreateDescriptorPool");
        !s) {
        return s;
    }
    descriptorPools_.push_back(pool);
    ai.descriptorPool = pool;
    return device.check(vkAllocateDescriptorSets(device.device(), &ai, &out), "vkAllocateDescriptorSets");
}

GpuStatus
GpuRecordContext::bindRaster(const char* where, GpuRasterId raster, GpuRasterBlend blend,
                             std::span<const GpuBuffer* const> buffers, std::span<const std::byte> push)
{
    std::shared_ptr<GpuRasterPipeline> pipeline;
    if (GpuStatus s = rasterPipeline(raster, blend, pipeline); !s) {
        return s;
    }
    const GpuRasterDesc& desc = pipeline->desc();
    if (buffers.size() != desc.storageBufferCount) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": " + std::to_string(buffers.size()) + " buffers for " + std::to_string(desc.storageBufferCount) + " storage bindings" };
    }
    if (push.size() != desc.pushConstantBytes) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": " + std::to_string(push.size()) + " push-constant bytes for a " + std::to_string(desc.pushConstantBytes) + "-byte block" };
    }
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        if (!buffers[i] || !*buffers[i]) {
            return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": empty buffer at binding " + std::to_string(i) };
        }
        if (GpuStatus s = checkBindable(where, static_cast<uint32_t>(i), buffers[i]->size()); !s) {
            return s;
        }
        infos[i] = { buffers[i]->buffer(), 0, VK_WHOLE_SIZE };
        touched_.push_back(buffers[i]->buffer());
        VkWriteDescriptorSet& w = writes[i];
        w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstBinding = static_cast<uint32_t>(i);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &infos[i];
    }

    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline());
    if (!writes.empty()) {
        if (pushDescriptorFn_) {
            pushDescriptorFn_(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipelineLayout(), 0,
                              static_cast<uint32_t>(writes.size()), writes.data());
        } else {
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (GpuStatus s = rasterDescriptorSet(pipeline->setLayout(), desc.storageBufferCount, set); !s) {
                return s;
            }
            for (VkWriteDescriptorSet& w : writes) {
                w.dstSet = set;
            }
            vkUpdateDescriptorSets(registry_.device().device(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                                   nullptr);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipelineLayout(), 0, 1, &set, 0,
                                    nullptr);
        }
    }
    if (!push.empty()) {
        vkCmdPushConstants(cmd_, pipeline->pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, static_cast<uint32_t>(push.size()), push.data());
    }
    const uint32_t width = rasterTarget_->desc().width;
    const uint32_t height = rasterTarget_->desc().height;
    const VkViewport viewport { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    const VkRect2D scissor { { 0, 0 }, { width, height } };
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    return {};
}

GpuStatus
GpuRecordContext::draw(GpuRasterId raster, GpuRasterBlend blend, std::span<const GpuBuffer* const> buffers,
                       std::span<const std::byte> push, uint32_t vertexCount, uint32_t instanceCount)
{
    if (GpuStatus s = drawable("GpuRecordContext::draw"); !s) {
        return s;
    }
    if (vertexCount == 0 || instanceCount == 0) {
        return {};
    }
    if (GpuStatus s = bindRaster("GpuRecordContext::draw", raster, blend, buffers, push); !s) {
        return s;
    }
    vkCmdDraw(cmd_, vertexCount, instanceCount, 0, 0);
    return {};
}

GpuStatus
GpuRecordContext::drawIndexedIndirect(GpuRasterId raster, GpuRasterBlend blend,
                                      std::span<const GpuBuffer* const> buffers, std::span<const std::byte> push,
                                      const GpuBuffer& indexBuffer, const GpuBuffer& indirectBuffer, uint32_t drawCount)
{
    const char* where = "GpuRecordContext::drawIndexedIndirect";
    if (GpuStatus s = drawable(where); !s) {
        return s;
    }
    if (!indexBuffer || !indirectBuffer) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": empty index or indirect buffer" };
    }
    constexpr VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
    if (VkDeviceSize(drawCount) * stride > indirectBuffer.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": " + std::to_string(drawCount) + " commands overrun the " + std::to_string(indirectBuffer.size()) + "-byte indirect buffer" };
    }
    if (drawCount == 0) {
        return {};
    }
    if (GpuStatus s = bindRaster(where, raster, blend, buffers, push); !s) {
        return s;
    }
    touched_.push_back(indexBuffer.buffer());
    touched_.push_back(indirectBuffer.buffer());
    vkCmdBindIndexBuffer(cmd_, indexBuffer.buffer(), 0, VK_INDEX_TYPE_UINT32);
    const uint32_t perDraw = maxDrawIndirectCount();
    for (uint32_t first = 0; first < drawCount; first += perDraw) {
        if (first > 0 && abortCheck_ && abortCheck_()) {
            aborted_ = true;
            return abortedStatus(where);
        }
        const uint32_t count = std::min(perDraw, drawCount - first);
        vkCmdDrawIndexedIndirect(cmd_, indirectBuffer.buffer(), VkDeviceSize(first) * stride, count,
                                 static_cast<uint32_t>(stride));
    }
    return {};
}

GpuStatus
GpuRecordContext::endRaster()
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::endRaster: context already submitted" };
    }
    if (!rasterTarget_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::endRaster: no raster pass is open" };
    }
    vkCmdEndRendering(cmd_);
    const VkImageMemoryBarrier2 toCopy = attachmentBarrier(
        *rasterTarget_, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memoryBarrier(cmd_, kDrawStages, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                      | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  &toCopy);
    rendered_ = rasterTarget_;
    rasterTarget_ = nullptr;
    return {};
}

GpuStatus
GpuRecordContext::copyToImage(const GpuBuffer& dst)
{
    if (!isRaster()) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "GpuRecordContext::copyToImage: not a raster context" };
    }
    if (GpuStatus s = recordable("GpuRecordContext::copyToImage"); !s) {
        return s;
    }
    if (!rendered_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyToImage: no raster pass was ended" };
    }
    if (!dst || dst.size() < rendered_->packedBytes()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyToImage: the buffer is smaller than the attachment" };
    }
    memoryBarrier(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | kDrawStages,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { rendered_->desc().width, rendered_->desc().height, 1 };
    touched_.push_back(dst.buffer());
    vkCmdCopyImageToBuffer(cmd_, rendered_->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buffer(), 1, &region);
    memoryBarrier(cmd_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | kDrawStages
                      | VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                      | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT
                      | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    return {};
}

void
GpuRecordContext::ownershipBarrier(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize bytes, bool release)
{
    const GpuDeviceInfo& info = registry_.device().info();
    const uint32_t mine = isRaster() ? info.graphicsFamily : info.computeFamily;
    const uint32_t other = isRaster() ? info.computeFamily : info.graphicsFamily;
    VkBufferMemoryBarrier2 b { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    if (release) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstAccessMask = VK_ACCESS_2_NONE;
        b.srcQueueFamilyIndex = mine;
        b.dstQueueFamilyIndex = other;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex = other;
        b.dstQueueFamilyIndex = mine;
    }
    touched_.push_back(buffer.buffer());
    b.buffer = buffer.buffer();
    b.offset = offset;
    b.size = bytes;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

GpuStatus
GpuRecordContext::releaseToOtherQueue(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize bytes)
{
    if (GpuStatus s = recordable("GpuRecordContext::releaseToOtherQueue"); !s) {
        return s;
    }
    if (!buffer || bytes == 0 || offset + bytes > buffer.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::releaseToOtherQueue: invalid range" };
    }
    const GpuDeviceInfo& info = registry_.device().info();
    if (info.graphicsQueue && info.graphicsFamily != info.computeFamily) {
        ownershipBarrier(buffer, offset, bytes, true);
    }
    return {};
}

GpuStatus
GpuRecordContext::acquireFromOtherQueue(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize bytes)
{
    if (GpuStatus s = recordable("GpuRecordContext::acquireFromOtherQueue"); !s) {
        return s;
    }
    if (!buffer || bytes == 0 || offset + bytes > buffer.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::acquireFromOtherQueue: invalid range" };
    }
    const GpuDeviceInfo& info = registry_.device().info();
    if (info.graphicsQueue && info.graphicsFamily != info.computeFamily) {
        ownershipBarrier(buffer, offset, bytes, false);
    }
    return {};
}

GpuDevice&
GpuRecordContext::device() const
{
    return registry_.device();
}

void
GpuRecordContext::waitExternalSemaphore(VkSemaphore semaphore)
{
    if (open_ && semaphore != VK_NULL_HANDLE) {
        externalWaits_.push_back(semaphore);
    }
}

void
GpuRecordContext::waitTimeline(TimelineValue value)
{
    if (open_ && value) {
        timelineWaits_.push_back(value);
    }
}

void
GpuRecordContext::hold(std::shared_ptr<void> object)
{
    if (object) {
        heldObjects_.push_back(std::move(object));
    }
}

GpuStatus
GpuRecordContext::copyBuffer(const GpuBuffer& src, const GpuBuffer& dst, std::span<const VkBufferCopy> regions)
{
    if (GpuStatus s = recordable("GpuRecordContext::copyBuffer"); !s) {
        return s;
    }
    if (!src || !dst) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyBuffer: invalid buffers" };
    }
    for (const VkBufferCopy& r : regions) {
        if (r.size == 0 || r.srcOffset + r.size > src.size() || r.dstOffset + r.size > dst.size()) {
            return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyBuffer: a region is out of range" };
        }
    }
    if (regions.empty()) {
        return {};
    }

    VkMemoryBarrier2 before { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    before.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    before.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &before;
    vkCmdPipelineBarrier2(cmd_, &dep);

    touched_.push_back(src.buffer());
    touched_.push_back(dst.buffer());
    vkCmdCopyBuffer(cmd_, src.buffer(), dst.buffer(), static_cast<uint32_t>(regions.size()), regions.data());

    VkMemoryBarrier2 after { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    after.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
        | VK_PIPELINE_STAGE_2_HOST_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep2 { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep2.memoryBarrierCount = 1;
    dep2.pMemoryBarriers = &after;
    vkCmdPipelineBarrier2(cmd_, &dep2);
    return {};
}

GpuStatus
GpuRecordContext::copyBuffer(const GpuBuffer& src, VkDeviceSize srcOffset, const GpuBuffer& dst, VkDeviceSize dstOffset,
                             VkDeviceSize bytes)
{
    const VkBufferCopy region { srcOffset, dstOffset, bytes };

    return copyBuffer(src, dst, std::span<const VkBufferCopy>(&region, 1));
}

GpuStatus
GpuRecordContext::fillBuffer(const GpuBuffer& dst, VkDeviceSize size, uint32_t value)
{
    if (GpuStatus s = recordable("GpuRecordContext::fillBuffer"); !s) {
        return s;
    }
    if (!dst || (size % 4) != 0 || size > dst.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::fillBuffer: invalid buffer or size" };
    }
    if (size == 0) {
        return {};
    }

    VkMemoryBarrier2 before { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    before.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    before.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &before;
    vkCmdPipelineBarrier2(cmd_, &dep);

    touched_.push_back(dst.buffer());
    vkCmdFillBuffer(cmd_, dst.buffer(), 0, size, value);

    VkMemoryBarrier2 after { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    after.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep2 { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep2.memoryBarrierCount = 1;
    dep2.pMemoryBarriers = &after;
    vkCmdPipelineBarrier2(cmd_, &dep2);
    return {};
}

GpuStatus
GpuRecordContext::copyFromExternal(const GpuBuffer& src, VkDeviceSize srcBytes, const GpuBuffer& dst,
                                   std::span<const VkBufferCopy> regions)
{
    if (GpuStatus s = recordable("GpuRecordContext::copyFromExternal"); !s) {
        return s;
    }
    if (!src || !dst || srcBytes == 0 || srcBytes > src.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyFromExternal: invalid buffers" };
    }
    for (const VkBufferCopy& r : regions) {
        if (r.size == 0 || r.srcOffset + r.size > srcBytes || r.dstOffset + r.size > dst.size()) {
            return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyFromExternal: a region is out of range" };
        }
    }
    const uint32_t family = registry_.device().queueFamily(queue_);

    touched_.push_back(src.buffer());
    touched_.push_back(dst.buffer());
    VkBufferMemoryBarrier2 acquire { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    acquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    acquire.srcAccessMask = VK_ACCESS_2_NONE;
    acquire.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    acquire.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex = family;
    acquire.buffer = src.buffer();
    acquire.offset = 0;
    acquire.size = srcBytes;
    // Earlier dispatches may still read or write what the copies overwrite.
    VkMemoryBarrier2 before { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    before.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    before.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &before;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &acquire;
    vkCmdPipelineBarrier2(cmd_, &dep);

    if (!regions.empty()) {
        vkCmdCopyBuffer(cmd_, src.buffer(), dst.buffer(), static_cast<uint32_t>(regions.size()), regions.data());
    }

    VkBufferMemoryBarrier2 release { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    release.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    release.srcAccessMask = VK_ACCESS_2_NONE;
    release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    release.dstAccessMask = VK_ACCESS_2_NONE;
    release.srcQueueFamilyIndex = family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.buffer = src.buffer();
    release.offset = 0;
    release.size = srcBytes;
    VkMemoryBarrier2 after { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    after.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep2 { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep2.memoryBarrierCount = 1;
    dep2.pMemoryBarriers = &after;
    dep2.bufferMemoryBarrierCount = 1;
    dep2.pBufferMemoryBarriers = &release;
    vkCmdPipelineBarrier2(cmd_, &dep2);
    return {};
}

void
GpuRecordContext::resetQueries(VkQueryPool pool, uint32_t first, uint32_t count)
{
    if (open_ && !rasterTarget_ && pool != VK_NULL_HANDLE && count > 0) {
        vkCmdResetQueryPool(cmd_, pool, first, count);
    }
}

void
GpuRecordContext::writeTimestamp(VkQueryPool pool, uint32_t query)
{
    if (open_ && pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, query);
    }
}

GpuStatus
GpuRecordContext::submit(std::span<const TimelineValue> waits, TimelineValue signal, const std::function<bool()>& stop)
{
    const char* where = "GpuRecordContext::submit";
    registry_.device().reapRetired();
    if (!open_ || !aborted_) {
        if (GpuStatus s = recordable(where); !s) {
            return s;
        }
    }
    if (!signal) {
        if (aborted_) {
            open_ = false;
            return abortedStatus(where);
        }
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::submit: no semaphore to signal" };
    }
    // An aborted context still submits its waits and signal with no commands: waiters on `signal` and later
    // submissions waiting on it would otherwise hang, and the timeline must keep advancing in order.
    const bool runCommands = !aborted_;
    open_ = false;
    GpuDevice& device = registry_.device();
    if (runCommands) {
        if (GpuStatus s = device.check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer"); !s) {
            return s;
        }
    }
    std::vector<VkSemaphoreSubmitInfo> waitInfos = externalWaitInfos();
    for (const TimelineValue& w : waits) {
        if (w) {
            waitInfos.push_back(w.waitInfo());
        }
    }
    if (runCommands && !finished_.empty()) {
        // The batches are held back on the host because a submit behind one still blocked on its wait would leave
        // nothing for stop to cancel. The first still carries the waits: a host wait does not make the uploads'
        // writes visible to the device.
        if (GpuStatus s = waitBeforeBatches(waits, kPacedBatchTimeoutNs, stop); !s) {
            return s;
        }
        if (GpuStatus s = runBatchesBeforeLast(waitInfos, kPacedBatchTimeoutNs, stop); !s) {
            return s;
        }
        waitInfos.clear();
    }
    const VkSemaphoreSubmitInfo signalInfo = signal.waitInfo();
    VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    ci.commandBuffer = cmd_;
    VkSubmitInfo2 info { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    info.waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size());
    info.pWaitSemaphoreInfos = waitInfos.data();
    info.commandBufferInfoCount = runCommands ? 1 : 0;
    info.pCommandBufferInfos = runCommands ? &ci : nullptr;
    info.signalSemaphoreInfoCount = 1;
    info.pSignalSemaphoreInfos = &signalInfo;
    GpuStatus s = device.submit(queue_, std::span<const VkSubmitInfo2>(&info, 1), VK_NULL_HANDLE);
    if (!runCommands) {
        if (!s) {
            s.aborted = true;
            return s;
        }
        return abortedStatus(where);
    }
    return s;
}

GpuStatus
GpuRecordContext::submitAndWait(uint64_t timeoutNs, const std::function<bool()>& stop)
{
    const char* where = "GpuRecordContext::submitAndWait";
    registry_.device().reapRetired();
    if (!open_ || !aborted_) {
        if (GpuStatus s = recordable(where); !s) {
            return s;
        }
    }
    // An aborted context runs no commands, but still consumes the external waits, whose semaphores the other API
    // would otherwise find still signalled.
    const bool runCommands = !aborted_;
    open_ = false;
    if (!runCommands && externalWaits_.empty()) {
        return abortedStatus(where);
    }
    GpuDevice& device = registry_.device();
    if (runCommands) {
        if (GpuStatus s = device.check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer"); !s) {
            return s;
        }
    }
    std::vector<VkSemaphoreSubmitInfo> waitInfos = externalWaitInfos();
    if (runCommands && !finished_.empty()) {
        if (GpuStatus s = runBatchesBeforeLast(waitInfos, timeoutNs, stop); !s) {
            return s;
        }
        waitInfos.clear();
    }
    GpuStatus s = submitFenced(runCommands ? cmd_ : VK_NULL_HANDLE, waitInfos, timeoutNs, stop);
    if (!runCommands) {
        if (!s) {
            s.aborted = true;
            return s;
        }
        return abortedStatus(where);
    }
    return s;
}

GpuStatus
GpuRecordContext::submitFenced(VkCommandBuffer cmd, std::span<const VkSemaphoreSubmitInfo> waits, uint64_t timeoutNs,
                               const std::function<bool()>& stop)
{
    GpuDevice& device = registry_.device();
    const VkDevice vk = device.device();
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (GpuStatus s = device.check(vkCreateFence(vk, &fi, nullptr, &fence), "vkCreateFence"); !s) {
        return s;
    }
    VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    ci.commandBuffer = cmd;
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = cmd ? 1 : 0;
    submit.pCommandBufferInfos = cmd ? &ci : nullptr;
    GpuStatus s = device.submit(queue_, std::span<const VkSubmitInfo2>(&submit, 1), fence);
    bool mayBePending = (s.result == VK_ERROR_DEVICE_LOST);
    if (s) {
        VkResult waited = VK_TIMEOUT;
        for (uint64_t waitedNs = 0;;) {
            const uint64_t slice = stop ? std::min(kStopPollNs, timeoutNs - waitedNs) : timeoutNs - waitedNs;
            waited = vkWaitForFences(vk, 1, &fence, VK_TRUE, slice);
            waitedNs += slice;
            if ((waited != VK_TIMEOUT) || (waitedNs >= timeoutNs)) {
                break;
            }
            if (stop()) {
                // Work that finished as the stop came is kept.
                waited = vkGetFenceStatus(vk, fence);
                if (waited == VK_NOT_READY) {
                    retireWork(fence);
                    return stopped();
                }
                break;
            }
        }
        if (waited == VK_TIMEOUT) {
            std::fprintf(stderr, "GpuRecordContext::submitAndWait: the device work did not finish within %llu ms; the GPU device is treated as lost\n",
                         static_cast<unsigned long long>(timeoutNs / 1000000));
            // The fence is leaked with the rest.
            abandonWork("vkWaitForFences");
            return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::submitAndWait: the device work timed out" };
        }
        if (waited != VK_SUCCESS) {
            s = device.check(waited, "vkWaitForFences");
            mayBePending = true;
        }
    }
    vkDestroyFence(vk, fence, nullptr);
    if (mayBePending && cmd) {
        // After a loss a command buffer may still be pending, and freeing it then is invalid.
        cmd_ = VK_NULL_HANDLE;
        finished_.clear();
    }
    return s;
}

GpuStatus
GpuRecordContext::waitBeforeBatches(std::span<const TimelineValue> waits, uint64_t timeoutNs, const std::function<bool()>& stop)
{
    GpuDevice& device = registry_.device();
    for (const TimelineValue& w : waits) {
        if (!w) {
            continue;
        }
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &w.semaphore;
        wi.pValues = &w.value;
        for (uint64_t waitedNs = 0;; waitedNs += kStopPollNs) {
            const VkResult waited = vkWaitSemaphores(device.device(), &wi, kStopPollNs);
            if (waited == VK_SUCCESS) {
                break;
            }
            if (waited != VK_TIMEOUT) {
                return device.check(waited, "vkWaitSemaphores");
            }
            if (stop && stop()) {
                return stopped();
            }
            if (waitedNs >= timeoutNs) {
                std::fprintf(stderr, "GpuRecordContext::submit: what the work waits for did not finish within %llu ms; the GPU device is treated as lost\n",
                             static_cast<unsigned long long>(timeoutNs / 1000000));
                abandonWork("vkWaitSemaphores");
                return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::submit: the work's waits timed out" };
            }
        }
    }
    return {};
}

GpuStatus
GpuRecordContext::runBatchesBeforeLast(std::span<const VkSemaphoreSubmitInfo> waits, uint64_t timeoutNs, const std::function<bool()>& stop)
{
    for (size_t i = 0; i < finished_.size(); ++i) {
        if (stop && stop()) {
            return stopped();
        }
        const uint64_t work = finished_[i].work;
        const auto begin = std::chrono::steady_clock::now();
        GpuStatus status = submitFenced(finished_[i].cmd, (i == 0) ? waits : std::span<const VkSemaphoreSubmitInfo>(), timeoutNs, stop);
        if (!status) {
            return status;
        }
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        // A batch behind others on the queue looks slow, which only makes later batches shorter than they need be.
        if ((ns > 0) && (work > 0) && ((i > 0) || (externalWaits_.empty() && timelineWaits_.empty()))) {
            const double sample = static_cast<double>(work) * static_cast<double>(kBatchTargetNs) / static_cast<double>(ns);
            const uint64_t measured = (sample >= static_cast<double>(kMaxBatchWork)) ? kMaxBatchWork : std::max(kMinBatchWork, static_cast<uint64_t>(sample));
            std::atomic<uint64_t>& learned = registry_.learnedBatchWork_;
            const uint64_t previous = learned.load(std::memory_order_relaxed);
            learned.store((previous != 0) ? (previous * 3 + measured) / 4 : measured, std::memory_order_relaxed);
        }
        if (gHasBatchFinishedHook.load(std::memory_order_acquire)) {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lock(gBatchFinishedHookMutex);
                hook = gBatchFinishedHook;
            }
            if (hook) {
                hook();
            }
        }
    }
    if (stop && stop()) {
        return stopped();
    }
    return {};
}

void
GpuRecordContext::abandonWork(const char* what)
{
    registry_.device().abandonInFlightWork(what);
    parkInFlightWork();
    // The command buffers go with their pool.
    cmd_ = VK_NULL_HANDLE;
    finished_.clear();
}

std::vector<VkSemaphoreSubmitInfo>
GpuRecordContext::externalWaitInfos() const
{
    std::vector<VkSemaphoreSubmitInfo> infos;
    for (const TimelineValue& value : timelineWaits_) {
        infos.push_back(value.waitInfo());
    }
    for (VkSemaphore semaphore : externalWaits_) {
        VkSemaphoreSubmitInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        info.semaphore = semaphore;
        // A binary semaphore: the value is ignored.
        info.value = 0;
        info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        infos.push_back(info);
    }
    return infos;
}

std::shared_ptr<void>
GpuRecordContext::takeHoldings(std::shared_ptr<void> finalizer)
{
    struct InFlight {
        // Declared first, so that it runs once everything else is released.
        std::shared_ptr<void> finalizer;
        std::vector<GpuBufferHandle> held;
        std::deque<GpuUniformBuffer> uniforms;
        std::vector<std::unique_ptr<GpuTexture>> textures;
        std::vector<std::shared_ptr<const GpuTexture>> cachedTextures;
        std::vector<std::shared_ptr<GpuKernel>> kernels;
        std::vector<std::shared_ptr<void>> objects;
        std::vector<std::shared_ptr<GpuRasterPipeline>> rasters;
    };
    auto holdings = std::make_shared<InFlight>();
    holdings->finalizer = std::move(finalizer);
    holdings->held = std::move(held_);
    holdings->uniforms = std::move(uniforms_);
    holdings->textures = std::move(textures_);
    holdings->cachedTextures = std::move(cachedTextures_);
    holdings->kernels = std::move(kernels_);
    holdings->objects = std::move(heldObjects_);
    holdings->rasters = std::move(rasterPipelines_);
    ownRasters_.clear();
    return holdings;
}

void
GpuRecordContext::parkInFlightWork()
{
    GpuDevice::parkForever(takeHoldings(nullptr));
}

void
GpuRecordContext::retireWork(VkFence fence)
{
    GpuDevice& device = registry_.device();
    // The pool belongs to this thread, so only this thread may free the command buffers, once the work is done.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::vector<VkCommandBuffer> buffers;
    if (cmd_) {
        buffers.push_back(cmd_);
    }
    for (const FinishedBatch& batch : finished_) {
        buffers.push_back(batch.cmd);
    }
    cmd_ = VK_NULL_HANDLE;
    finished_.clear();
    registry_.retireCommandBuffers(pool_, std::move(buffers), done);
    // The registry resets the descriptor sets the work reads once no recording is left, so this one lasts until the
    // work is done.
    std::shared_ptr<void> finalizer(nullptr, [registry = &registry_, vk = device.device(), pools = std::move(descriptorPools_), done](void*) {
        for (VkDescriptorPool pool : pools) {
            vkDestroyDescriptorPool(vk, pool, nullptr);
        }
        done->store(true, std::memory_order_release);
        registry->recordingEnded();
    });
    descriptorPools_.clear();
    retired_ = true;
    device.retireWhenSignalled(fence, takeHoldings(std::move(finalizer)), std::move(touched_));
    touched_.clear();
}

} // namespace gpu
