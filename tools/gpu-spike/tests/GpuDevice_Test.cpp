#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "GpuDevice.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

class GpuDeviceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
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

    void TearDown() override
    {
        dev.reset();
        check.expectClean("the test");
    }

    gputest::ValidationCheck check;
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

TEST(GpuDeviceOptions, ValidationIsOffByDefault)
{
    EXPECT_FALSE(GpuDeviceOptions {}.enableValidation);
}

TEST_F(GpuDeviceTest, PushDescriptorCanBeDisabled)
{
    gpu::GpuDeviceOptions o = gputest::deviceOptions();
    o.disablePushDescriptor = true;
    std::unique_ptr<GpuDevice> other;
    GpuStatus s = gputest::createDevice(other, o);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_FALSE(other->info().pushDescriptor);
}

TEST_F(GpuDeviceTest, DebugMessengerReachesCallback)
{
    if (!dev->info().debugMessenger) {
        GTEST_SKIP() << "VK_EXT_debug_utils unavailable";
    }
    auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(dev->instance(), "vkSubmitDebugUtilsMessageEXT"));
    ASSERT_NE(submit, nullptr);
    VkDebugUtilsMessengerCallbackDataEXT data { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT };
    data.pMessage = "synthetic error from DebugMessengerReachesCallback";
    const uint64_t before = gputest::validationErrors().load();
    submit(dev->instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
    EXPECT_EQ(gputest::validationErrors().load(), before + 1);
    check = gputest::ValidationCheck();
}

TEST_F(GpuDeviceTest, EmptySubmitSignalsTimelineSemaphore)
{
    VkSemaphore sem = VK_NULL_HANDLE;
    ASSERT_TRUE(dev->createTimelineSemaphore(0, sem).ok());

    VkSemaphoreSubmitInfo signal { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = sem;
    signal.value = 1;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
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
    VkSemaphore sem = VK_NULL_HANDLE;
    EXPECT_EQ(dev->createTimelineSemaphore(0, sem).result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(dev->waitIdle().result, VK_ERROR_DEVICE_LOST);
}
