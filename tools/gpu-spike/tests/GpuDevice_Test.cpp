#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

#include "GpuDevice.h"

using namespace gpu;

namespace {

class GpuDeviceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GpuStatus s = GpuDevice::create({}, dev);
        ASSERT_TRUE(s.ok()) << s.message << " (VkResult " << s.result << ")";
        std::cout << "device: " << dev->info().name
                  << " transferFallback=" << dev->info().transferUsesComputeQueue
                  << " validation=" << dev->info().validationEnabled
                  << " fp16=" << dev->info().shaderFloat16
                  << " memBudgetExt=" << dev->info().memoryBudget
                  << " extMemFd=" << dev->info().externalMemoryFd
                  << " extSemFd=" << dev->info().externalSemaphoreFd
                  << " extMemHost=" << dev->info().externalMemoryHost
                  << " pushDesc=" << dev->info().pushDescriptor << "\n";
    }

    std::unique_ptr<GpuDevice> dev;
};

} // namespace

TEST_F(GpuDeviceTest, TransferQueueOrFallbackFlag)
{
    const GpuDeviceInfo& i = dev->info();
    if (i.transferUsesComputeQueue) {
        EXPECT_EQ(i.transferFamily, i.computeFamily);
    } else {
        EXPECT_NE(i.transferFamily, i.computeFamily);
    }
}

TEST_F(GpuDeviceTest, BudgetIsNonZero)
{
    std::vector<VmaBudget> budgets;
    ASSERT_TRUE(dev->queryBudget(budgets).ok());
    ASSERT_FALSE(budgets.empty());
    VkDeviceSize total = 0;
    for (const VmaBudget& b : budgets) {
        total += b.budget;
    }
    EXPECT_GT(total, 0u);
}

TEST_F(GpuDeviceTest, CommandPoolIsPerThread)
{
    VkCommandPool mine = VK_NULL_HANDLE;
    VkCommandPool mineAgain = VK_NULL_HANDLE;
    ASSERT_TRUE(dev->commandPool(QueueKind::Compute, mine).ok());
    ASSERT_TRUE(dev->commandPool(QueueKind::Compute, mineAgain).ok());
    EXPECT_EQ(mine, mineAgain);

    VkCommandPool other = VK_NULL_HANDLE;
    GpuStatus s;
    std::thread t([&] { s = dev->commandPool(QueueKind::Compute, other); });
    t.join();
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_NE(other, VK_NULL_HANDLE);
    EXPECT_NE(mine, other);
}

TEST_F(GpuDeviceTest, EmptySubmitSignalsTimelineSemaphore)
{
    VkSemaphore sem = VK_NULL_HANDLE;
    ASSERT_TRUE(dev->createTimelineSemaphore(0, sem).ok());

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = sem;
    signal.value = 1;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    ASSERT_TRUE(dev->submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), VK_NULL_HANDLE).ok());
    GpuStatus w = dev->waitSemaphore(sem, 1, 5'000'000'000ull);
    EXPECT_TRUE(w.ok()) << w.message;

    uint64_t value = 0;
    ASSERT_EQ(vkGetSemaphoreCounterValue(dev->device(), sem, &value), VK_SUCCESS);
    EXPECT_GE(value, 1u);

    signal.semaphore = sem;
    submit.signalSemaphoreInfoCount = 1;
    signal.value = 2;
    ASSERT_TRUE(dev->submit(QueueKind::Transfer, std::span<const VkSubmitInfo2>(&submit, 1), VK_NULL_HANDLE).ok());
    EXPECT_TRUE(dev->waitSemaphore(sem, 2, 5'000'000'000ull).ok());

    vkDestroySemaphore(dev->device(), sem, nullptr);
    EXPECT_FALSE(dev->isLost());
}

TEST_F(GpuDeviceTest, LostStateIsSticky)
{
    EXPECT_FALSE(dev->isLost());
    GpuStatus s = dev->check(VK_ERROR_DEVICE_LOST, "synthetic");
    EXPECT_EQ(s.result, VK_ERROR_DEVICE_LOST);
    EXPECT_TRUE(dev->isLost());
    VkCommandPool p;
    EXPECT_EQ(dev->commandPool(QueueKind::Compute, p).result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(dev->waitIdle().result, VK_ERROR_DEVICE_LOST);
}
