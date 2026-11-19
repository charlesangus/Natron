#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

#include "GpuHarness.h"
#include "GpuKernelRegistry.h"
#include "GpuPipelineCache.h"
#include "GpuRaster.h"
#include "GpuTexture.h"

using namespace gpu;

namespace {

namespace fs = std::filesystem;

bool
formatBlends(VkPhysicalDevice pd, VkFormat format)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, format, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

class GpuRasterTest : public gputest::GpuSuite {
protected:
    struct Readback {
        GpuDevice* dev = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        void* mapped = nullptr;
        ~Readback()
        {
            if (buffer) {
                vmaDestroyBuffer(dev->allocator(), buffer, alloc);
            }
        }
    };

    static void makeReadback(VkDeviceSize bytes, Readback& out)
    {
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai {};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info {};
        out.dev = dev_.get();
        ASSERT_EQ(vmaCreateBuffer(dev_->allocator(), &bi, &ai, &out.buffer, &out.alloc, &info), VK_SUCCESS);
        out.mapped = info.pMappedData;
        ASSERT_NE(out.mapped, nullptr);
    }

    static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkImageLayout from, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageLayout to)
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
        ib.image = image;
        ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Clears `texture` to `clear` and copies it into `readback`, on the compute queue.
    static void clearAndCopy(const GpuTexture& texture, const VkClearColorValue& clear, const Readback& readback)
    {
        VkDevice vk = dev_->device();
        VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = dev_->info().computeFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        ASSERT_EQ(vkCreateCommandPool(vk, &pci, nullptr, &pool), VK_SUCCESS);
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateCommandBuffers(vk, &ai, &cmd), VK_SUCCESS);

        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS);
        imageBarrier(cmd, texture.image(), VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        imageBarrier(cmd, texture.image(), VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { texture.desc().width, texture.desc().height, 1 };
        vkCmdCopyImageToBuffer(cmd, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        VkMemoryBarrier2 hb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &dep);
        ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        ASSERT_EQ(vkCreateFence(vk, &fi, nullptr, &fence), VK_SUCCESS);
        VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        ci.commandBuffer = cmd;
        VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &ci;
        GpuStatus s = dev_->submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), fence);
        EXPECT_TRUE(s.ok()) << s.message;
        if (s.ok()) {
            EXPECT_EQ(vkWaitForFences(vk, 1, &fence, VK_TRUE, 30'000'000'000ull), VK_SUCCESS);
        }
        vkDestroyFence(vk, fence, nullptr);
        vkDestroyCommandPool(vk, pool, nullptr);
        EXPECT_EQ(vmaInvalidateAllocation(dev_->allocator(), readback.alloc, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    }
};

GPU_TEST_F(GpuRasterTest, CapabilityIsReported)
{
    const GpuDeviceInfo& info = dev_->info();
    const bool rgba = formatBlends(dev_->physicalDevice(), VK_FORMAT_R32G32B32A32_SFLOAT);
    const bool r = formatBlends(dev_->physicalDevice(), VK_FORMAT_R32_SFLOAT);
    std::printf("raster: dynamicRendering=%d rasterFloatBlend=%d (RGBA32F blend=%d, R32F blend=%d)\n",
                int(info.dynamicRendering), int(info.rasterFloatBlend), int(rgba), int(r));
    // Dynamic rendering is core in Vulkan 1.3, which every accepted device implements.
    EXPECT_TRUE(info.dynamicRendering);
    EXPECT_EQ(info.rasterFloatBlend, info.dynamicRendering && rgba && r);
}

GPU_TEST_F(GpuRasterTest, ClearedAttachmentCopiesBack)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    constexpr uint32_t kW = 37;
    constexpr uint32_t kH = 19;
    for (uint32_t channels : { 4u, 1u }) {
        SCOPED_TRACE(std::to_string(channels) + " channels");
        std::unique_ptr<GpuTexture> texture;
        GpuStatus s = GpuTexture::createAttachment(*dev_, kW, kH, channels, texture);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_TRUE(texture->isAttachment());
        EXPECT_EQ(texture->format(), channels == 4 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32_SFLOAT);
        EXPECT_EQ(texture->packedBytes(), VkDeviceSize(kW) * kH * channels * sizeof(float));

        Readback readback;
        makeReadback(texture->packedBytes(), readback);
        if (HasFatalFailure()) {
            return;
        }
        VkClearColorValue clear {};
        clear.float32[0] = 0.25f;
        clear.float32[1] = -1.5f;
        clear.float32[2] = 3.0e5f;
        clear.float32[3] = 1.0f;
        clearAndCopy(*texture, clear, readback);
        if (HasFatalFailure()) {
            return;
        }
        const float* texels = static_cast<const float*>(readback.mapped);
        size_t mismatches = 0;
        for (size_t i = 0; i < size_t(kW) * kH * channels; ++i) {
            if (texels[i] != clear.float32[i % channels]) {
                ++mismatches;
            }
        }
        EXPECT_EQ(mismatches, 0u);
    }
}

GPU_TEST_F(GpuRasterTest, AttachmentRejectsUnsupportedChannels)
{
    std::unique_ptr<GpuTexture> texture;
    EXPECT_FALSE(GpuTexture::createAttachment(*dev_, 8, 8, 3, texture).ok());
    EXPECT_FALSE(GpuTexture::createAttachment(*dev_, 0, 8, 4, texture).ok());
    EXPECT_EQ(texture, nullptr);
}

GPU_TEST_F(GpuRasterTest, RasterTestIsRegistered)
{
    GpuKernelRegistry registry(*dev_);
    const GpuRasterDesc* desc = registry.rasterDesc(GpuRasterId::raster_test);
    ASSERT_NE(desc, nullptr);
    EXPECT_GT(desc->vertexSpirvWords, 0u);
    EXPECT_GT(desc->fragmentSpirvWords, 0u);
    EXPECT_EQ(desc->storageBufferCount, 1u);
    EXPECT_EQ(desc->pushConstantBytes, 32u);
    EXPECT_EQ(registry.rasterDesc(GpuRasterId::Count), nullptr);

    const GpuRasterId added = registry.registerRaster(*desc);
    EXPECT_EQ(added, GpuRasterId::Count);
    const GpuRasterDesc* again = registry.rasterDesc(added);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->vertexSpirv, desc->vertexSpirv);
}

GPU_TEST_F(GpuRasterTest, PipelinesAreBuiltAndCached)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    const fs::path dir = fs::temp_directory_path() / ("natron-gpu-raster-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    {
        GpuKernelRegistry registry(*dev_);
        GpuPipelineCacheOptions o;
        o.directory = dir.string();
        GpuPipelineCache cache(*dev_, o);

        std::vector<std::shared_ptr<GpuRasterPipeline>> built;
        for (VkFormat format : { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT }) {
            for (GpuRasterBlend blend : { GpuRasterBlend::Additive, GpuRasterBlend::Replace }) {
                SCOPED_TRACE(std::string(format == VK_FORMAT_R32_SFLOAT ? "R32F " : "RGBA32F ")
                             + (blend == GpuRasterBlend::Additive ? "additive" : "replace"));
                std::shared_ptr<GpuRasterPipeline> p;
                GpuStatus s = cache.raster(registry, GpuRasterId::raster_test, format, blend, p);
                ASSERT_TRUE(s.ok()) << s.message;
                ASSERT_NE(p, nullptr);
                EXPECT_NE(p->pipeline(), VkPipeline(VK_NULL_HANDLE));
                EXPECT_EQ(p->colourFormat(), format);
                EXPECT_EQ(p->blend(), blend);
                for (const auto& other : built) {
                    EXPECT_NE(other.get(), p.get());
                }

                std::shared_ptr<GpuRasterPipeline> cached;
                s = cache.raster(registry, GpuRasterId::raster_test, format, blend, cached);
                ASSERT_TRUE(s.ok()) << s.message;
                EXPECT_EQ(cached.get(), p.get());
                built.push_back(p);
            }
        }
        EXPECT_EQ(cache.stats().rasterBuilds, 4u);

        std::shared_ptr<GpuRasterPipeline> p;
        EXPECT_EQ(cache.raster(registry, GpuRasterId::raster_test, VK_FORMAT_R16G16B16A16_SFLOAT,
                               GpuRasterBlend::Additive, p)
                      .result,
                  VK_ERROR_FORMAT_NOT_SUPPORTED);
        EXPECT_EQ(p, nullptr);
        EXPECT_FALSE(cache.raster(registry, GpuRasterId::Count, VK_FORMAT_R32_SFLOAT, GpuRasterBlend::Replace, p).ok());

        cache.clearMemory();
        built.clear();
        std::shared_ptr<GpuRasterPipeline> rebuilt;
        ASSERT_TRUE(cache.raster(registry, GpuRasterId::raster_test, VK_FORMAT_R32_SFLOAT, GpuRasterBlend::Replace,
                                 rebuilt)
                        .ok());
        EXPECT_EQ(cache.stats().rasterBuilds, 5u);
    }
    fs::remove_all(dir, ec);
}

} // namespace
