#include "GpuRecord.h"

#include "GpuPipelineCache.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace gpu {

GpuStatus
GpuRecordContext::create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out, GpuBufferPool* pool,
                         GpuPipelineCache* pipelines)
{
    GpuDevice& device = registry.device();
    if (device.isLost()) {
        return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::create: device lost" };
    }
    std::unique_ptr<GpuRecordContext> ctx(new GpuRecordContext(registry, pool, pipelines));
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
    std::vector<VkSemaphoreSubmitInfo> waitInfos;
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
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
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

void
GpuRecordContext::parkInFlightWork()
{
    struct InFlight {
        std::vector<GpuBufferHandle> held;
        std::deque<GpuUniformBuffer> uniforms;
        std::vector<std::unique_ptr<GpuTexture>> textures;
        std::vector<std::shared_ptr<GpuKernel>> kernels;
    };
    auto parked = std::make_shared<InFlight>();
    parked->held = std::move(held_);
    parked->uniforms = std::move(uniforms_);
    parked->textures = std::move(textures_);
    parked->kernels = std::move(kernels_);
    GpuDevice::parkForever(std::move(parked));
}

} // namespace gpu
