#include "GpuRecord.h"

#include "GpuPipelineCache.h"
#include "GpuSizeClass.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace gpu {

namespace {
    std::atomic<uint64_t> gMaxStorageBufferBytesForTests { 0 };
} // namespace

GpuStatus
GpuRecordContext::create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out, GpuBufferPool* pool,
                         GpuPipelineCache* pipelines, GpuTextureCache* textures)
{
    GpuDevice& device = registry.device();
    if (device.isLost()) {
        return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::create: device lost" };
    }
    std::unique_ptr<GpuRecordContext> ctx(new GpuRecordContext(registry, pool, pipelines, textures));
    registry.recordingBegan();
    // From here the destructor balances recordingBegan(), including on the error paths.
    if (GpuStatus s = registry.commandPoolForThisThread(ctx->pool_); !s) {
        return s;
    }

    VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = ctx->pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (GpuStatus s = device.check(vkAllocateCommandBuffers(device.device(), &ai, &ctx->cmd_),
                                   "vkAllocateCommandBuffers");
        !s) {
        return s;
    }

    VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (GpuStatus s = device.check(vkBeginCommandBuffer(ctx->cmd_, &bi), "vkBeginCommandBuffer"); !s) {
        return s;
    }
    ctx->open_ = true;
    out = std::move(ctx);
    return {};
}

GpuRecordContext::~GpuRecordContext()
{
    if (cmd_) {
        vkFreeCommandBuffers(registry_.device().device(), pool_, 1, &cmd_);
    }
    registry_.recordingEnded();
}

GpuStatus
GpuRecordContext::dispatch(GpuKernelId kernel, std::span<const GpuBuffer* const> buffers,
                           std::span<const std::byte> push, std::array<uint32_t, 3> threads)
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::dispatch: context already submitted" };
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
    return k->record(cmd_, handles, push, threads);
}

GpuStatus
GpuRecordContext::dispatchBindings(GpuKernelId kernel, std::span<const GpuBinding> bindings,
                                   std::span<const std::byte> push, std::array<uint32_t, 3> threads)
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::dispatchBindings: context already submitted" };
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
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::dispatchBindings: context already submitted" };
    }
    if (!kernel) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::dispatchBindings: no kernel" };
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
    return kernel->recordBindings(cmd_, bindings, push, kernel->groupCount(threads));
}

GpuStatus
GpuRecordContext::texture(const GpuTextureDesc& desc, std::span<const float> texels, const GpuTexture*& out)
{
    out = nullptr;
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::texture: context already submitted" };
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
    if (!open_) {
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
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::updateBuffer: context already submitted" };
    }
    if (!dst || data.empty() || data.size() > 65536 || (data.size() % 4) != 0 || (offset % 4) != 0
        || offset + data.size() > dst.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::updateBuffer: invalid range" };
    }
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

void
GpuRecordContext::acquireFromTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                                      VkDeviceSize bytes)
{
    if (open_) {
        transfer.recordComputeAcquire(cmd_, buffer, offset, bytes);
    }
}

void
GpuRecordContext::releaseToTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                                    VkDeviceSize bytes)
{
    if (open_) {
        transfer.recordComputeRelease(cmd_, buffer, offset, bytes);
    }
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
GpuRecordContext::hold(std::shared_ptr<void> object)
{
    if (object) {
        heldObjects_.push_back(std::move(object));
    }
}

GpuStatus
GpuRecordContext::copyFromExternal(const GpuBuffer& src, VkDeviceSize srcBytes, const GpuBuffer& dst,
                                   std::span<const VkBufferCopy> regions)
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyFromExternal: context already submitted" };
    }
    if (!src || !dst || srcBytes == 0 || srcBytes > src.size()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyFromExternal: invalid buffers" };
    }
    for (const VkBufferCopy& r : regions) {
        if (r.size == 0 || r.srcOffset + r.size > srcBytes || r.dstOffset + r.size > dst.size()) {
            return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::copyFromExternal: a region is out of range" };
        }
    }
    const uint32_t family = registry_.device().queueFamily(QueueKind::Compute);

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
    if (open_ && pool != VK_NULL_HANDLE && count > 0) {
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
GpuRecordContext::submit(std::span<const TimelineValue> waits, TimelineValue signal)
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::submit: context already submitted" };
    }
    if (!signal) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::submit: no semaphore to signal" };
    }
    open_ = false;
    GpuDevice& device = registry_.device();
    if (GpuStatus s = device.check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer"); !s) {
        return s;
    }
    std::vector<VkSemaphoreSubmitInfo> waitInfos = externalWaitInfos();
    for (const TimelineValue& w : waits) {
        if (w) {
            waitInfos.push_back(w.waitInfo());
        }
    }
    const VkSemaphoreSubmitInfo signalInfo = signal.waitInfo();
    VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    ci.commandBuffer = cmd_;
    VkSubmitInfo2 info { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    info.waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size());
    info.pWaitSemaphoreInfos = waitInfos.data();
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &ci;
    info.signalSemaphoreInfoCount = 1;
    info.pSignalSemaphoreInfos = &signalInfo;
    return device.submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&info, 1), VK_NULL_HANDLE);
}

GpuStatus
GpuRecordContext::submitAndWait(uint64_t timeoutNs)
{
    if (!open_) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuRecordContext::submitAndWait: context already submitted" };
    }
    open_ = false;
    GpuDevice& device = registry_.device();
    const VkDevice vk = device.device();
    if (GpuStatus s = device.check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer"); !s) {
        return s;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (GpuStatus s = device.check(vkCreateFence(vk, &fi, nullptr, &fence), "vkCreateFence"); !s) {
        return s;
    }
    VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    ci.commandBuffer = cmd_;
    const std::vector<VkSemaphoreSubmitInfo> waitInfos = externalWaitInfos();
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size());
    submit.pWaitSemaphoreInfos = waitInfos.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &ci;
    GpuStatus s = device.submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), fence);
    bool mayBePending = (s.result == VK_ERROR_DEVICE_LOST);
    if (s) {
        VkResult waited = vkWaitForFences(vk, 1, &fence, VK_TRUE, timeoutNs);
        if (waited == VK_TIMEOUT) {
            std::fprintf(stderr, "GpuRecordContext::submitAndWait: the device work did not finish within %llu ms; the GPU device is treated as lost\n",
                         static_cast<unsigned long long>(timeoutNs / 1000000));
            device.abandonInFlightWork("vkWaitForFences");
            parkInFlightWork();
            // The fence is leaked with the rest, and the command buffer goes with its pool.
            cmd_ = VK_NULL_HANDLE;
            return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::submitAndWait: the device work timed out" };
        }
        if (waited != VK_SUCCESS) {
            s = device.check(waited, "vkWaitForFences");
            mayBePending = true;
        }
    }
    vkDestroyFence(vk, fence, nullptr);
    if (mayBePending) {
        // After a loss the command buffer may still be pending, and freeing it then is invalid.
        cmd_ = VK_NULL_HANDLE;
    }
    return s;
}

std::vector<VkSemaphoreSubmitInfo>
GpuRecordContext::externalWaitInfos() const
{
    std::vector<VkSemaphoreSubmitInfo> infos;
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

void
GpuRecordContext::parkInFlightWork()
{
    struct InFlight {
        std::vector<GpuBufferHandle> held;
        std::deque<GpuUniformBuffer> uniforms;
        std::vector<std::unique_ptr<GpuTexture>> textures;
        std::vector<std::shared_ptr<const GpuTexture>> cachedTextures;
        std::vector<std::shared_ptr<GpuKernel>> kernels;
        std::vector<std::shared_ptr<void>> objects;
    };
    auto parked = std::make_shared<InFlight>();
    parked->held = std::move(held_);
    parked->uniforms = std::move(uniforms_);
    parked->textures = std::move(textures_);
    parked->cachedTextures = std::move(cachedTextures_);
    parked->kernels = std::move(kernels_);
    parked->objects = std::move(heldObjects_);
    GpuDevice::parkForever(std::move(parked));
}

} // namespace gpu
