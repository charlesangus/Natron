#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "GpuKernel.h"
#include "GpuTestDevice.h"
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
        GpuStatus s = gputest::createDevice(dev);
        ASSERT_TRUE(s.ok()) << s.message << " (VkResult " << s.result << ")";

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
        if (pool_) {
            vkDestroyCommandPool(dev->device(), pool_, nullptr);
        }
        timer.reset();
        kernel.reset();
        dev.reset();
        check.expectClean("the test");
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

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = dev->queueFamily(QueueKind::Compute);
        ASSERT_EQ(vkCreateCommandPool(dev->device(), &pci, nullptr, &pool_), VK_SUCCESS);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
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

        ASSERT_EQ(vmaInvalidateAllocation(dev->allocator(), readback.alloc, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::unique_ptr<GpuKernel> kernel;
    std::unique_ptr<GpuTimer> timer;
    Buffer device;
    Buffer readback;
};

} // namespace

TEST_F(GpuKernelTest, GroupCountRoundsUp)
{
    ASSERT_EQ(kernel->desc().groupSize, (std::array<uint32_t, 3>{16, 16, 1}));
    EXPECT_EQ(kernel->groupCount({kW, kH, 1}), (std::array<uint32_t, 3>{121, 68, 1}));
    EXPECT_EQ(kernel->groupCount({32, 16, 1}), (std::array<uint32_t, 3>{2, 1, 1}));
    EXPECT_EQ(kernel->groupCount({0xFFFFFFFFu, 1, 1}), (std::array<uint32_t, 3>{0x10000000u, 1, 1}));
}

TEST_F(GpuKernelTest, RefusesGroupCountsBeyondTheDeviceLimit)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->physicalDevice(), &props);
    const uint32_t limit = props.limits.maxComputeWorkGroupCount[1];
    if (limit == 0xFFFFFFFFu) {
        GTEST_SKIP() << "device has no group count limit below 2^32";
    }
    FillParams p{1, 1, 0};
    const VkBuffer bufs[] = {VK_NULL_HANDLE};
    GpuStatus s = kernel->recordGroups(VK_NULL_HANDLE, bufs, std::as_bytes(std::span(&p, 1)), {1, limit + 1, 1});
    EXPECT_EQ(s.result, VK_ERROR_FEATURE_NOT_PRESENT) << s.message;
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
