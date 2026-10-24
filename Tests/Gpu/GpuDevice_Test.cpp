#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "GpuDevice.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

class GpuDeviceTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        std::cout << "device: " << dev->info().name
                  << " transferSharesCompute=" << dev->info().transferSharesComputeQueue
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

GPU_TEST_F(GpuDeviceTest, QueueLayoutIsConsistent)
{
    const GpuDeviceInfo& i = dev->info();
    std::printf("queues: compute %u/%u upload %u/%u download %u/%u\n", i.computeFamily, i.computeQueueIndex,
                i.transferFamily, i.uploadQueueIndex, i.transferFamily, i.downloadQueueIndex);
    EXPECT_EQ(i.computeQueueIndex, 0u);
    EXPECT_LE(i.uploadQueueIndex, i.downloadQueueIndex);
    if (i.transferSharesComputeQueue) {
        EXPECT_EQ(i.transferFamily, i.computeFamily);
        EXPECT_EQ(i.uploadQueueIndex, 0u);
        EXPECT_EQ(i.downloadQueueIndex, 0u);
    } else if (i.transferFamily == i.computeFamily) {
        EXPECT_EQ(i.uploadQueueIndex, 1u);
    }
}

GPU_TEST_F(GpuDeviceTest, BudgetIsNonZero)
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

namespace {

GpuDeviceCandidate
candidate(uint32_t index, VkPhysicalDeviceType type, uint64_t gib, const char* name, bool ok = true)
{
    GpuDeviceCandidate c;
    c.index = index;
    c.type = type;
    c.deviceLocalHeapSize = gib << 30;
    c.name = name;
    c.meetsRequirements = ok;
    return c;
}

} // namespace

TEST(GpuDeviceAutoPick, EnumerationOrderDoesNotMatter)
{
    std::vector<GpuDeviceCandidate> list = {
        candidate(0, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8, "RTX 3070 Ti"),
        candidate(1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2, "Quadro P400"),
        candidate(2, VK_PHYSICAL_DEVICE_TYPE_CPU, 0, "llvmpipe"),
    };
    std::sort(list.begin(), list.end(),
              [](const GpuDeviceCandidate& a, const GpuDeviceCandidate& b) { return a.index < b.index; });
    do {
        const std::optional<GpuAutoPick> pick = pickAutoDevice(list);
        ASSERT_TRUE(pick.has_value());
        EXPECT_EQ(list[pick->candidate].name, "RTX 3070 Ti");
        EXPECT_EQ(pick->reason.rfind("auto: discrete, 8 GiB, over ", 0), 0u);
        EXPECT_NE(pick->reason.find("Quadro P400 (2 GiB)"), std::string::npos);
        EXPECT_NE(pick->reason.find("llvmpipe (cpu)"), std::string::npos);
    } while (std::next_permutation(list.begin(), list.end(),
                                   [](const GpuDeviceCandidate& a, const GpuDeviceCandidate& b) {
                                       return a.index < b.index;
                                   }));
}

TEST(GpuDeviceAutoPick, OnlyCpuDeviceIsStillPicked)
{
    const std::vector<GpuDeviceCandidate> list = { candidate(0, VK_PHYSICAL_DEVICE_TYPE_CPU, 0, "llvmpipe") };
    const std::optional<GpuAutoPick> pick = pickAutoDevice(list);
    ASSERT_TRUE(pick.has_value());
    EXPECT_EQ(pick->candidate, 0u);
}

TEST(GpuDeviceAutoPick, NoCandidatesPicksNothing)
{
    EXPECT_FALSE(pickAutoDevice({}).has_value());
    EXPECT_FALSE(
        pickAutoDevice({ candidate(0, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8, "RTX 3070 Ti", false) }).has_value());
}

TEST(GpuDeviceAutoPick, LargerDeviceFailingRequirementsIsSkipped)
{
    const std::vector<GpuDeviceCandidate> list = {
        candidate(0, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8, "RTX 3070 Ti", false),
        candidate(1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2, "Quadro P400"),
    };
    const std::optional<GpuAutoPick> pick = pickAutoDevice(list);
    ASSERT_TRUE(pick.has_value());
    EXPECT_EQ(list[pick->candidate].name, "Quadro P400");
    EXPECT_EQ(pick->reason.find("RTX 3070 Ti"), std::string::npos);
}

TEST(GpuDeviceAutoPick, IdenticalDevicesPreferLowerIndex)
{
    std::vector<GpuDeviceCandidate> list = {
        candidate(1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8, "RTX A"),
        candidate(0, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8, "RTX A"),
    };
    const std::optional<GpuAutoPick> pick = pickAutoDevice(list);
    ASSERT_TRUE(pick.has_value());
    EXPECT_EQ(list[pick->candidate].index, 0u);
}

TEST(GpuDeviceOptions, ValidationIsOffByDefault)
{
    EXPECT_FALSE(GpuDeviceOptions {}.enableValidation);
}

GPU_TEST_F(GpuDeviceTest, PushDescriptorCanBeDisabled)
{
    gpu::GpuDeviceOptions o = gputest::deviceOptions();
    o.disablePushDescriptor = true;
    std::unique_ptr<GpuDevice> other;
    GpuStatus s = gputest::createDevice(other, o);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_FALSE(other->info().pushDescriptor);
}

GPU_TEST_F(GpuDeviceTest, DebugMessengerReachesCallback)
{
    if (!dev->info().debugMessenger) {
        GPUTEST_SKIP() << "VK_EXT_debug_utils unavailable";
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

GPU_TEST_F(GpuDeviceTest, EmptySubmitSignalsTimelineSemaphore)
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
    ASSERT_TRUE(dev->submit(QueueKind::Upload, std::span<const VkSubmitInfo2>(&submit, 1), VK_NULL_HANDLE).ok());
    EXPECT_TRUE(dev->waitSemaphore(sem, 2, 5'000'000'000ull).ok());

    signal.value = 3;
    ASSERT_TRUE(dev->submit(QueueKind::Download, std::span<const VkSubmitInfo2>(&submit, 1), VK_NULL_HANDLE).ok());
    EXPECT_TRUE(dev->waitSemaphore(sem, 3, 5'000'000'000ull).ok());

    vkDestroySemaphore(dev->device(), sem, nullptr);
    EXPECT_FALSE(dev->isLost());
}

GPU_TEST_F(GpuDeviceTest, LostStateIsSticky)
{
    EXPECT_FALSE(dev->isLost());
    GpuStatus s = dev->check(VK_ERROR_DEVICE_LOST, "synthetic");
    EXPECT_EQ(s.result, VK_ERROR_DEVICE_LOST);
    EXPECT_TRUE(dev->isLost());
    VkSemaphore sem = VK_NULL_HANDLE;
    EXPECT_EQ(dev->createTimelineSemaphore(0, sem).result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(dev->waitIdle().result, VK_ERROR_DEVICE_LOST);
}
