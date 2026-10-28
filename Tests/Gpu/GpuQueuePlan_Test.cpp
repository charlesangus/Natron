#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "GpuDevice.h"

using namespace gpu;

namespace {

VkQueueFamilyProperties
family(VkQueueFlags flags, uint32_t queueCount)
{
    VkQueueFamilyProperties p {};
    p.queueFlags = flags;
    p.queueCount = queueCount;
    return p;
}

constexpr VkQueueFlags kGraphics = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
constexpr VkQueueFlags kCompute = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
constexpr VkQueueFlags kTransfer = VK_QUEUE_TRANSFER_BIT;

} // namespace

TEST(GpuQueuePlan, TransferOnlyFamilyCarriesCopies)
{
    const std::vector<VkQueueFamilyProperties> fams = {
        family(kGraphics, 1),
        family(kCompute, 4),
        family(kTransfer, 2),
    };
    const std::optional<GpuQueuePlan> plan = planQueues(fams);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->computeFamily, 1u);
    EXPECT_EQ(plan->computeQueueIndex, 0u);
    EXPECT_EQ(plan->computeQueues, 1u);
    EXPECT_EQ(plan->transferFamily, 2u);
    EXPECT_EQ(plan->transferQueues, 2u);
    EXPECT_EQ(plan->uploadQueueIndex, 0u);
    EXPECT_EQ(plan->downloadQueueIndex, 1u);
    EXPECT_FALSE(plan->transferSharesComputeQueue);
}

TEST(GpuQueuePlan, SingleQueueTransferFamilySharedByUploadAndDownload)
{
    const std::vector<VkQueueFamilyProperties> fams = {
        family(kGraphics, 1),
        family(kTransfer, 1),
        family(kCompute, 4),
    };
    const std::optional<GpuQueuePlan> plan = planQueues(fams);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->computeFamily, 2u);
    EXPECT_EQ(plan->computeQueues, 1u);
    EXPECT_EQ(plan->transferFamily, 1u);
    EXPECT_EQ(plan->transferQueues, 1u);
    EXPECT_EQ(plan->uploadQueueIndex, 0u);
    EXPECT_EQ(plan->downloadQueueIndex, 0u);
    EXPECT_FALSE(plan->transferSharesComputeQueue);
}

TEST(GpuQueuePlan, TransferFamilyOpensAtMostTwoQueues)
{
    const std::vector<VkQueueFamilyProperties> fams = { family(kCompute, 1), family(kTransfer, 8) };
    const std::optional<GpuQueuePlan> plan = planQueues(fams);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->transferFamily, 1u);
    EXPECT_EQ(plan->transferQueues, 2u);
    EXPECT_EQ(plan->uploadQueueIndex, 0u);
    EXPECT_EQ(plan->downloadQueueIndex, 1u);
}

TEST(GpuQueuePlan, WithoutTransferFamilyCopiesUseComputeQueues)
{
    const std::vector<VkQueueFamilyProperties> fams = { family(kGraphics, 1), family(kCompute, 4) };
    const std::optional<GpuQueuePlan> plan = planQueues(fams);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->computeFamily, 1u);
    EXPECT_EQ(plan->transferFamily, 1u);
    EXPECT_EQ(plan->transferQueues, 0u);
    EXPECT_EQ(plan->computeQueues, 3u);
    EXPECT_EQ(plan->computeQueueIndex, 0u);
    EXPECT_EQ(plan->uploadQueueIndex, 1u);
    EXPECT_EQ(plan->downloadQueueIndex, 2u);
    EXPECT_FALSE(plan->transferSharesComputeQueue);
}

TEST(GpuQueuePlan, WithoutTransferFamilyTwoComputeQueuesShareTheSecond)
{
    const std::vector<VkQueueFamilyProperties> fams = { family(kCompute, 2) };
    const std::optional<GpuQueuePlan> plan = planQueues(fams);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->transferFamily, 0u);
    EXPECT_EQ(plan->computeQueues, 2u);
    EXPECT_EQ(plan->uploadQueueIndex, 1u);
    EXPECT_EQ(plan->downloadQueueIndex, 1u);
    EXPECT_FALSE(plan->transferSharesComputeQueue);
}

TEST(GpuQueuePlan, SingleComputeQueueIsSharedWithCopies)
{
    const std::vector<VkQueueFamilyProperties> fams = { family(kGraphics, 1) };
    const std::optional<GpuQueuePlan> plan = planQueues(fams);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->computeFamily, 0u);
    EXPECT_EQ(plan->transferFamily, 0u);
    EXPECT_EQ(plan->computeQueues, 1u);
    EXPECT_EQ(plan->transferQueues, 0u);
    EXPECT_EQ(plan->uploadQueueIndex, 0u);
    EXPECT_EQ(plan->downloadQueueIndex, 0u);
    EXPECT_TRUE(plan->transferSharesComputeQueue);
}

TEST(GpuQueuePlan, NoComputeFamilyPlansNothing)
{
    EXPECT_FALSE(planQueues({}).has_value());
    const std::vector<VkQueueFamilyProperties> fams = { family(kTransfer, 2) };
    EXPECT_FALSE(planQueues(fams).has_value());
}

TEST(GpuPerftestToken, UnsetOrEmptyGetsTheTokenAlone)
{
    EXPECT_EQ(withPerftestToken(nullptr, "transfer_queue"), "transfer_queue");
    EXPECT_EQ(withPerftestToken("", "transfer_queue"), "transfer_queue");
}

TEST(GpuPerftestToken, OtherTokensAreKept)
{
    EXPECT_EQ(withPerftestToken("sam", "transfer_queue"), "sam,transfer_queue");
    EXPECT_EQ(withPerftestToken("sam,video_decode", "transfer_queue"), "sam,video_decode,transfer_queue");
    EXPECT_EQ(withPerftestToken("transfer_queue_x", "transfer_queue"), "transfer_queue_x,transfer_queue");
}

TEST(GpuPerftestToken, PresentTokenIsNotAddedTwice)
{
    EXPECT_EQ(withPerftestToken("transfer_queue", "transfer_queue"), "transfer_queue");
    EXPECT_EQ(withPerftestToken("sam,transfer_queue", "transfer_queue"), "sam,transfer_queue");
    EXPECT_EQ(withPerftestToken("transfer_queue,sam", "transfer_queue"), "transfer_queue,sam");
    EXPECT_EQ(withPerftestToken("sam transfer_queue", "transfer_queue"), "sam transfer_queue");
}
