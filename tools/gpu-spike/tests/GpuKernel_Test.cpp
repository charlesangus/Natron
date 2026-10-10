#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "GpuKernel.h"
#include "fill_spirv.h"

using namespace gpu;

namespace {

constexpr uint32_t kW = 1921;
constexpr uint32_t kH = 1081;
constexpr VkDeviceSize kBytes = VkDeviceSize(kW) * kH * sizeof(uint32_t);

struct FillParams
{
    uint32_t width;
    uint32_t height;
    uint32_t asFloat;
};

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* mapped = nullptr;
};

class GpuKernelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GpuStatus s = GpuDevice::create({}, dev);
        ASSERT_TRUE(s.ok()) << s.message << " (VkResult " << s.result << ")";
        std::cout << "device: " << dev->info().name << " pushDesc=" << dev->info().pushDescriptor
                  << "\n";

        GpuKernelDesc d;
        d.spirv = fill_spirv;
        d.spirvWords = fill_spirv_words;
        d.entry = "main";
        d.storageBufferCount = 1;
        d.pushConstantBytes = sizeof(FillParams);
        d.groupSize = {fill_group_size[0], fill_group_size[1], fill_group_size[2]};
        s = GpuKernel::create(*dev, d, kernel);
        ASSERT_TRUE(s.ok()) << s.message << " (VkResult " << s.result << ")";
        ASSERT_TRUE(GpuTimer::create(*dev, timer).ok());
    }

    void TearDown() override
    {
        if (dev) {
            dev->waitIdle();
        }
        destroy(device);
        destroy(readback);
    }

    Buffer make(VkBufferUsageFlags usage, bool host)
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kBytes;
        bi.usage = usage;
        VmaAllocationCreateInfo ai{};
        if (host) {
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        Buffer b;
        VmaAllocationInfo info{};
        EXPECT_EQ(vmaCreateBuffer(dev->allocator(), &bi, &ai, &b.buffer, &b.alloc, &info), VK_SUCCESS);
        b.mapped = info.pMappedData;
        return b;
    }

    void destroy(Buffer& b)
    {
        if (b.buffer) {
            vmaDestroyBuffer(dev->allocator(), b.buffer, b.alloc);
            b = {};
        }
    }

    void runFill(uint32_t asFloat)
    {
        device = make(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
        readback = make(VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        ASSERT_NE(device.buffer, VK_NULL_HANDLE);
        ASSERT_NE(readback.mapped, nullptr);

        VkCommandPool pool = VK_NULL_HANDLE;
        ASSERT_TRUE(dev->commandPool(QueueKind::Compute, pool).ok());
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateCommandBuffers(dev->device(), &ai, &cmd), VK_SUCCESS);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS);

        timer->begin(cmd);
        FillParams p{kW, kH, asFloat};
        const VkBuffer bufs[] = {device.buffer};
        GpuStatus s = kernel->record(cmd, bufs, std::as_bytes(std::span(&p, 1)), {kW, kH, 1});
        ASSERT_TRUE(s.ok()) << s.message;
        timer->end(cmd);

        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferCopy copy{0, 0, kBytes};
        vkCmdCopyBuffer(cmd, device.buffer, readback.buffer, 1, &copy);

        VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        dep.pMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &dep);
        ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ASSERT_EQ(vkCreateFence(dev->device(), &fi, nullptr, &fence), VK_SUCCESS);
        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = cmd;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &ci;
        s = dev->submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), fence);
        ASSERT_TRUE(s.ok()) << s.message;
        ASSERT_EQ(vkWaitForFences(dev->device(), 1, &fence, VK_TRUE, 30'000'000'000ull), VK_SUCCESS);
        vkDestroyFence(dev->device(), fence, nullptr);
        kernel->releaseDescriptors();
        vkFreeCommandBuffers(dev->device(), pool, 1, &cmd);

        ASSERT_EQ(vmaInvalidateAllocation(dev->allocator(), readback.alloc, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    }

    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuKernel> kernel;
    std::unique_ptr<GpuTimer> timer;
    Buffer device;
    Buffer readback;
};

} // namespace

TEST_F(GpuKernelTest, GroupCountRoundsUp)
{
    const auto g = kernel->groupCount({kW, kH, 1});
    EXPECT_EQ(g[0], (kW + fill_group_size[0] - 1) / fill_group_size[0]);
    EXPECT_EQ(g[1], (kH + fill_group_size[1] - 1) / fill_group_size[1]);
    EXPECT_EQ(g[2], 1u);
}

TEST_F(GpuKernelTest, FillUint)
{
    runFill(0);
    const auto* out = static_cast<const uint32_t*>(readback.mapped);
    size_t bad = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            bad += out[size_t(y) * kW + x] != x + y * kW;
        }
    }
    EXPECT_EQ(bad, 0u);

    double ns = 0;
    ASSERT_TRUE(timer->elapsedNs(ns).ok());
    std::cout << "timestamps " << (timer->supported() ? "supported" : "unsupported")
              << " elapsed=" << ns << " ns\n";
    if (timer->supported()) {
        EXPECT_GT(ns, 0.0);
    }
}

TEST_F(GpuKernelTest, FillFloat)
{
    runFill(1);
    const auto* out = static_cast<const float*>(readback.mapped);
    size_t bad = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            bad += out[size_t(y) * kW + x] != float(x + y * kW);
        }
    }
    EXPECT_EQ(bad, 0u);
}

TEST_F(GpuKernelTest, RejectsMismatchedBindings)
{
    GpuStatus s = kernel->record(VK_NULL_HANDLE, {}, {}, {1, 1, 1});
    EXPECT_FALSE(s.ok());
}
