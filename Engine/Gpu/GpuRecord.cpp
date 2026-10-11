#include "GpuRecord.h"

#include <vector>

namespace gpu {

GpuStatus
GpuRecordContext::create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out, GpuBufferPool* pool)
{
    GpuDevice& device = registry.device();
    if (device.isLost()) {
        return { VK_ERROR_DEVICE_LOST, "GpuRecordContext::create: device lost" };
    }
    std::unique_ptr<GpuRecordContext> ctx(new GpuRecordContext(registry, pool));
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
    if (s) {
        s = device.check(vkWaitForFences(vk, 1, &fence, VK_TRUE, timeoutNs), "vkWaitForFences");
    }
    vkDestroyFence(vk, fence, nullptr);
    if (!s) {
        // The work may still be pending after a timeout, and freeing its command buffer then is invalid.
        cmd_ = VK_NULL_HANDLE;
    }
    return s;
}

} // namespace gpu
