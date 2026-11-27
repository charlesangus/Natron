#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

#include "GpuHarness.h"
#include "GpuKernelRegistry.h"
#include "GpuPipelineCache.h"
#include "GpuRaster.h"
#include "GpuRecord.h"
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

    struct RasterParams {
        float viewport[2];
        float pad[2];
        float colour[4];
    };
    static_assert(sizeof(RasterParams) == 32);

    struct IndirectParams {
        uint32_t commands;
        uint32_t drawn;
    };

    static RasterParams rasterParams(uint32_t width, uint32_t height, float value)
    {
        return RasterParams { { float(width), float(height) }, { 0.0f, 0.0f }, { value, value, value, value } };
    }

    static std::span<const std::byte> bytesOf(const RasterParams& p) { return std::as_bytes(std::span(&p, 1)); }

    // A host-visible buffer holding `data`, which dispatches and draws bind directly.
    template <typename T>
    static void hostBuffer(const std::vector<T>& data, VkBufferUsageFlags usage, GpuBuffer& out)
    {
        GpuStatus s = GpuBuffer::createStaging(*dev_, data.size() * sizeof(T), usage, out);
        ASSERT_TRUE(s.ok()) << s.message;
        std::memcpy(out.mapped(), data.data(), data.size() * sizeof(T));
    }

    // Two triangles covering the rectangle, split along its diagonal.
    static std::vector<float> rect(float x0, float y0, float x1, float y1)
    {
        return { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
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

    struct DrawIndirectCap {
        explicit DrawIndirectCap(uint32_t count) { GpuRecordContext::setMaxDrawIndirectCountForTests(count); }
        ~DrawIndirectCap() { GpuRecordContext::setMaxDrawIndirectCountForTests(0); }
        DrawIndirectCap(const DrawIndirectCap&) = delete;
        DrawIndirectCap& operator=(const DrawIndirectCap&) = delete;
    };

    static void makeTimeline(uint64_t initial, VkSemaphore& out)
    {
        GpuStatus s = dev_->createTimelineSemaphore(initial, out);
        ASSERT_TRUE(s.ok()) << s.message;
    }

    static VkResult waitFor(VkSemaphore semaphore, uint64_t value)
    {
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &semaphore;
        wi.pValues = &value;
        return vkWaitSemaphores(dev_->device(), &wi, 30'000'000'000ull);
    }

    // A one-row attachment of 2 * commands pixels drawn by one drawIndexedIndirect of `commands` commands, of
    // which a dispatch makes the first `drawn` draw triangle k over the centre of pixel 2k and the rest nothing.
    // `drawStatus` is the draw's status; `texels` is filled only when the work ran.
    static void indirectRow(uint32_t commands, uint32_t drawn, const std::function<bool()>& abort,
                            GpuStatus& drawStatus, std::vector<float>& texels)
    {
        const uint32_t width = 2 * commands;
        std::vector<float> xy;
        std::vector<uint32_t> indices;
        for (uint32_t k = 0; k < commands; ++k) {
            const float x = 2.0f * k;
            for (float v : { x + 0.1f, -1.0f, x + 0.9f, -1.0f, x + 0.5f, 2.0f }) {
                xy.push_back(v);
            }
            for (uint32_t j = 0; j < 3; ++j) {
                indices.push_back(3 * k + j);
            }
        }
        GpuKernelRegistry registry(*dev_);
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
        ASSERT_TRUE(s.ok()) << s.message;
        std::unique_ptr<GpuTexture> texture;
        s = GpuTexture::createAttachment(*dev_, width, 1, 1, texture);
        ASSERT_TRUE(s.ok()) << s.message;
        GpuBuffer positions;
        GpuBuffer indexBuffer;
        hostBuffer(xy, kGpuBufferDefaultUsage, positions);
        hostBuffer(indices, kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer);
        GpuBuffer commandBuffer;
        s = GpuBuffer::createDeviceLocal(*dev_, commands * sizeof(VkDrawIndexedIndirectCommand),
                                         kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, commandBuffer);
        ASSERT_TRUE(s.ok()) << s.message;
        GpuBuffer readback;
        s = GpuBuffer::createStaging(*dev_, texture->packedBytes(), kGpuBufferDefaultUsage, readback);
        ASSERT_TRUE(s.ok()) << s.message;
        if (HasFatalFailure()) {
            return;
        }

        s = ctx->dispatch(GpuKernelId::raster_test_indirect, { &commandBuffer }, IndirectParams { commands, drawn },
                          { commands, 1, 1 });
        ASSERT_TRUE(s.ok()) << s.message;
        ctx->setAbortCheck(abort);
        const RasterParams params = rasterParams(width, 1, 1.0f);
        s = ctx->beginRaster(*texture, { 0.0f, 0.0f, 0.0f, 0.0f });
        ASSERT_TRUE(s.ok()) << s.message;
        drawStatus = ctx->drawIndexedIndirect(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions },
                                              bytesOf(params), indexBuffer, commandBuffer, commands);
        if (!drawStatus.ok()) {
            return;
        }
        ASSERT_TRUE(ctx->endRaster().ok());
        ASSERT_TRUE(ctx->copyToImage(readback).ok());
        s = ctx->submitAndWait();
        ASSERT_TRUE(s.ok()) << s.message;
        const float* mapped = static_cast<const float*>(readback.mapped());
        texels.assign(mapped, mapped + width);
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
    std::printf("raster: dynamicRendering=%d rasterFloatBlend=%d (RGBA32F blend=%d, R32F blend=%d, graphics queue=%d, "
                "vertexPipelineStoresAndAtomics=%d, fragmentStoresAndAtomics=%d, multiDrawIndirect=%d, "
                "maxDrawIndirectCount=%u, graphics family %s the compute family)\n",
                int(info.dynamicRendering), int(info.rasterFloatBlend), int(rgba), int(r), int(info.graphicsQueue),
                int(info.vertexPipelineStores), int(info.fragmentStores), int(info.multiDrawIndirect),
                info.maxDrawIndirectCount, info.graphicsFamily == info.computeFamily ? "is" : "is not");
    // Dynamic rendering is core in Vulkan 1.3, which every accepted device implements.
    EXPECT_TRUE(info.dynamicRendering);
    EXPECT_EQ(info.rasterFloatBlend, info.dynamicRendering && rgba && r && info.graphicsQueue && info.vertexPipelineStores && info.fragmentStores);
    if (!info.multiDrawIndirect) {
        EXPECT_EQ(info.maxDrawIndirectCount, 1u);
    }
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

GPU_TEST_F(GpuRasterTest, LoadOpClearCopiesBackThroughARasterContext)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    constexpr uint32_t kW = 37;
    constexpr uint32_t kH = 19;
    // Two passes into one attachment, so the second clear also has to wait for the first copy.
    const std::array<float, 4> clears[2] = { { 0.25f, -1.5f, 3.0e5f, 1.0f }, { -7.0f, 0.125f, 1.0e-3f, 42.0f } };
    for (uint32_t channels : { 4u, 1u }) {
        SCOPED_TRACE(std::to_string(channels) + " channels");
        GpuKernelRegistry registry(*dev_);
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
        ASSERT_TRUE(s.ok()) << s.message;
        std::unique_ptr<GpuTexture> texture;
        s = GpuTexture::createAttachment(*dev_, kW, kH, channels, texture);
        ASSERT_TRUE(s.ok()) << s.message;
        GpuBuffer readbacks[2];
        for (GpuBuffer& readback : readbacks) {
            s = GpuBuffer::createStaging(*dev_, texture->packedBytes(), kGpuBufferDefaultUsage, readback);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        for (int pass = 0; pass < 2; ++pass) {
            s = ctx->beginRaster(*texture, clears[pass]);
            ASSERT_TRUE(s.ok()) << s.message;
            s = ctx->endRaster();
            ASSERT_TRUE(s.ok()) << s.message;
            s = ctx->copyToImage(readbacks[pass]);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        s = ctx->submitAndWait();
        ASSERT_TRUE(s.ok()) << s.message;

        for (int pass = 0; pass < 2; ++pass) {
            SCOPED_TRACE("pass " + std::to_string(pass));
            const float* texels = static_cast<const float*>(readbacks[pass].mapped());
            size_t mismatches = 0;
            for (size_t i = 0; i < size_t(kW) * kH * channels; ++i) {
                if (texels[i] != clears[pass][i % channels]) {
                    ++mismatches;
                }
            }
            EXPECT_EQ(mismatches, 0u);
        }
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

GPU_TEST_F(GpuRasterTest, RegisteredRastersOfTwoRegistriesGetTheirOwnPipelines)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    const fs::path dir = fs::temp_directory_path() / ("natron-gpu-raster-registries-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    {
        GpuKernelRegistry first(*dev_);
        GpuKernelRegistry second(*dev_);
        EXPECT_NE(first.serial(), second.serial());
        const GpuRasterDesc* desc = first.rasterDesc(GpuRasterId::raster_test);
        ASSERT_NE(desc, nullptr);
        const GpuRasterId a = first.registerRaster(*desc);
        const GpuRasterId b = second.registerRaster(*desc);
        ASSERT_EQ(a, b);

        GpuPipelineCacheOptions o;
        o.directory = dir.string();
        GpuPipelineCache cache(*dev_, o);
        std::shared_ptr<GpuRasterPipeline> fromFirst;
        std::shared_ptr<GpuRasterPipeline> fromSecond;
        GpuStatus s = cache.raster(first, a, VK_FORMAT_R32_SFLOAT, GpuRasterBlend::Additive, fromFirst);
        ASSERT_TRUE(s.ok()) << s.message;
        s = cache.raster(second, b, VK_FORMAT_R32_SFLOAT, GpuRasterBlend::Additive, fromSecond);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_NE(fromFirst.get(), fromSecond.get());
        EXPECT_EQ(cache.stats().rasterBuilds, 2u);

        // A generated raster is one table entry in every registry, so its pipeline is shared.
        std::shared_ptr<GpuRasterPipeline> generatedFirst;
        std::shared_ptr<GpuRasterPipeline> generatedSecond;
        ASSERT_TRUE(cache.raster(first, GpuRasterId::raster_test, VK_FORMAT_R32_SFLOAT, GpuRasterBlend::Additive,
                                 generatedFirst)
                        .ok());
        ASSERT_TRUE(cache.raster(second, GpuRasterId::raster_test, VK_FORMAT_R32_SFLOAT, GpuRasterBlend::Additive,
                                 generatedSecond)
                        .ok());
        EXPECT_EQ(generatedFirst.get(), generatedSecond.get());
        EXPECT_EQ(cache.stats().rasterBuilds, 3u);
    }
    fs::remove_all(dir, ec);
}

GPU_TEST_F(GpuRasterTest, RasterContextNeedsTheCapability)
{
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    const GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
    if (!dev_->info().rasterFloatBlend) {
        EXPECT_EQ(s.result, VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(ctx, nullptr);
        return;
    }
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_TRUE(ctx->isRaster());
}

GPU_TEST_F(GpuRasterTest, ComputeContextRejectsRaster)
{
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    GpuStatus s = GpuRecordContext::create(registry, ctx);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_FALSE(ctx->isRaster());

    if (dev_->info().rasterFloatBlend) {
        std::unique_ptr<GpuTexture> texture;
        s = GpuTexture::createAttachment(*dev_, 4, 4, 1, texture);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_FALSE(ctx->beginRaster(*texture, { 0.0f, 0.0f, 0.0f, 0.0f }).ok());
    }
    GpuBuffer positions;
    hostBuffer(std::vector<float>(6, 0.0f), kGpuBufferDefaultUsage, positions);
    if (HasFatalFailure()) {
        return;
    }
    const RasterParams params = rasterParams(4, 4, 1.0f);
    EXPECT_FALSE(ctx->draw(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions }, bytesOf(params), 3).ok());
    EXPECT_FALSE(ctx->drawIndexedIndirect(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions },
                                          bytesOf(params), positions, positions, 1)
                     .ok());
    EXPECT_FALSE(ctx->endRaster().ok());
    EXPECT_FALSE(ctx->copyToImage(positions).ok());
    // The refusals leave the context usable.
    s = ctx->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;
}

GPU_TEST_F(GpuRasterTest, OverlappingTrianglesAddAndCoverSharedEdgesOnce)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    constexpr uint32_t kW = 9;
    constexpr uint32_t kH = 7;
    // Each rectangle's two triangles share a 45-degree diagonal through pixel centres, which Vulkan covers
    // exactly once whatever its tie-break rule. Which outer edges through pixel centres are covered is up to
    // the implementation, so a centre on one may take either value.
    struct Rect {
        float x0, y0, x1, y1;
    };
    const Rect rects[2] = { { 0.5f, 0.5f, 5.5f, 5.5f }, { 2.5f, 0.5f, 7.5f, 5.5f } };
    for (uint32_t channels : { 1u, 4u }) {
        SCOPED_TRACE(std::to_string(channels) + " channels");
        GpuKernelRegistry registry(*dev_);
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
        ASSERT_TRUE(s.ok()) << s.message;
        std::unique_ptr<GpuTexture> texture;
        s = GpuTexture::createAttachment(*dev_, kW, kH, channels, texture);
        ASSERT_TRUE(s.ok()) << s.message;
        GpuBuffer positions[2];
        for (int i = 0; i < 2; ++i) {
            hostBuffer(rect(rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1), kGpuBufferDefaultUsage, positions[i]);
        }
        GpuBuffer readback;
        s = GpuBuffer::createStaging(*dev_, texture->packedBytes(), kGpuBufferDefaultUsage, readback);
        ASSERT_TRUE(s.ok()) << s.message;
        if (HasFatalFailure()) {
            return;
        }

        const RasterParams params = rasterParams(kW, kH, 1.0f);
        s = ctx->beginRaster(*texture, { 0.0f, 0.0f, 0.0f, 0.0f });
        ASSERT_TRUE(s.ok()) << s.message;
        for (const GpuBuffer& p : positions) {
            s = ctx->draw(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &p }, bytesOf(params), 6);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        s = ctx->endRaster();
        ASSERT_TRUE(s.ok()) << s.message;
        s = ctx->copyToImage(readback);
        ASSERT_TRUE(s.ok()) << s.message;
        s = ctx->submitAndWait();
        ASSERT_TRUE(s.ok()) << s.message;

        const float* texels = static_cast<const float*>(readback.mapped());
        size_t mismatches = 0;
        size_t diagonalSamples = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const float cx = x + 0.5f;
                const float cy = y + 0.5f;
                float inside = 0.0f;
                float onOuterEdge = 0.0f;
                for (const Rect& r : rects) {
                    if (cx > r.x0 && cx < r.x1 && cy > r.y0 && cy < r.y1) {
                        inside += 1.0f;
                        if (cx - r.x0 == cy - r.y0) {
                            ++diagonalSamples;
                        }
                    } else if (cx >= r.x0 && cx <= r.x1 && cy >= r.y0 && cy <= r.y1) {
                        onOuterEdge += 1.0f;
                    }
                }
                for (uint32_t c = 0; c < channels; ++c) {
                    const float got = texels[(size_t(y) * kW + x) * channels + c];
                    if (got < inside || got > inside + onOuterEdge || got != float(int(got))) {
                        ADD_FAILURE() << "pixel (" << x << ", " << y << ") channel " << c << ": " << got
                                      << ", expected " << inside << " plus at most " << onOuterEdge;
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(mismatches, 0u);
        EXPECT_EQ(diagonalSamples, 8u);
    }
}

GPU_TEST_F(GpuRasterTest, IndirectDrawCountComesFromADispatch)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    // Command k draws triangle k, which covers only the centre of pixel 2k of a one-row attachment; the
    // dispatch makes the first kDrawn commands draw three indices and the rest none.
    constexpr uint32_t kCommands = 12;
    constexpr uint32_t kDrawn = 7;
    constexpr uint32_t kW = 2 * kCommands;
    constexpr uint32_t kH = 1;
    std::vector<float> xy;
    std::vector<uint32_t> indices;
    for (uint32_t k = 0; k < kCommands; ++k) {
        const float x = 2.0f * k;
        for (float v : { x + 0.1f, -1.0f, x + 0.9f, -1.0f, x + 0.5f, 2.0f }) {
            xy.push_back(v);
        }
        for (uint32_t j = 0; j < 3; ++j) {
            indices.push_back(3 * k + j);
        }
    }

    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
    ASSERT_TRUE(s.ok()) << s.message;
    std::unique_ptr<GpuTexture> texture;
    s = GpuTexture::createAttachment(*dev_, kW, kH, 1, texture);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuBuffer positions;
    GpuBuffer indexBuffer;
    hostBuffer(xy, kGpuBufferDefaultUsage, positions);
    hostBuffer(indices, kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer);
    GpuBuffer commands;
    s = GpuBuffer::createDeviceLocal(*dev_, kCommands * sizeof(VkDrawIndexedIndirectCommand),
                                     kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, commands);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuBuffer readback;
    s = GpuBuffer::createStaging(*dev_, texture->packedBytes(), kGpuBufferDefaultUsage, readback);
    ASSERT_TRUE(s.ok()) << s.message;
    if (HasFatalFailure()) {
        return;
    }

    s = ctx->dispatch(GpuKernelId::raster_test_indirect, { &commands }, IndirectParams { kCommands, kDrawn },
                      { kCommands, 1, 1 });
    ASSERT_TRUE(s.ok()) << s.message;
    const RasterParams params = rasterParams(kW, kH, 1.0f);
    s = ctx->beginRaster(*texture, { 0.0f, 0.0f, 0.0f, 0.0f });
    ASSERT_TRUE(s.ok()) << s.message;
    s = ctx->drawIndexedIndirect(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions }, bytesOf(params),
                                 indexBuffer, commands, kCommands);
    ASSERT_TRUE(s.ok()) << s.message;
    // More commands than the buffer holds are refused.
    EXPECT_FALSE(ctx->drawIndexedIndirect(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions },
                                          bytesOf(params), indexBuffer, commands, kCommands + 1)
                     .ok());
    s = ctx->endRaster();
    ASSERT_TRUE(s.ok()) << s.message;
    s = ctx->copyToImage(readback);
    ASSERT_TRUE(s.ok()) << s.message;
    s = ctx->submitAndWait();
    ASSERT_TRUE(s.ok()) << s.message;

    const float* texels = static_cast<const float*>(readback.mapped());
    uint32_t drawn = 0;
    for (uint32_t x = 0; x < kW; ++x) {
        const float expected = (x % 2 == 0 && x / 2 < kDrawn) ? 1.0f : 0.0f;
        EXPECT_EQ(texels[x], expected) << "pixel " << x;
        drawn += texels[x] == 1.0f ? 1 : 0;
    }
    EXPECT_EQ(drawn, kDrawn);
}

GPU_TEST_F(GpuRasterTest, ChunkedIndirectDrawsMatchOneCall)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    constexpr uint32_t kCommands = 12;
    constexpr uint32_t kDrawn = 7;
    GpuStatus drawn;
    std::vector<float> whole;
    indirectRow(kCommands, kDrawn, {}, drawn, whole);
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_TRUE(drawn.ok()) << drawn.message;
    ASSERT_EQ(whole.size(), size_t(2 * kCommands));

    // A cap of 1 is what a device without multiDrawIndirect records: one draw per command.
    for (uint32_t cap : { 5u, 1u }) {
        SCOPED_TRACE("at most " + std::to_string(cap) + " commands per draw");
        DrawIndirectCap capped(cap);
        {
            GpuKernelRegistry registry(*dev_);
            std::unique_ptr<GpuRecordContext> ctx;
            ASSERT_TRUE(GpuRecordContext::create(registry, ctx).ok());
            EXPECT_EQ(ctx->maxDrawIndirectCount(), std::min(cap, dev_->info().maxDrawIndirectCount));
        }
        std::vector<float> chunked;
        indirectRow(kCommands, kDrawn, {}, drawn, chunked);
        if (HasFatalFailure()) {
            return;
        }
        ASSERT_TRUE(drawn.ok()) << drawn.message;
        EXPECT_EQ(chunked, whole);
    }
}

GPU_TEST_F(GpuRasterTest, AbortLandsBetweenIndirectChunks)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    DrawIndirectCap capped(5);
    int checks = 0;
    GpuStatus drawn;
    std::vector<float> texels;
    // The draw's own check passes; the check before its second chunk aborts.
    indirectRow(12, 12, [&checks] { return ++checks > 1; }, drawn, texels);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_FALSE(drawn.ok());
    EXPECT_TRUE(drawn.aborted) << drawn.message;
    EXPECT_EQ(checks, 2);
    EXPECT_TRUE(texels.empty());
    EXPECT_FALSE(dev_->isLost());
}

GPU_TEST_F(GpuRasterTest, AbortBetweenDrawsStopsRecording)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    constexpr int kAbortAfter = 3;
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
    ASSERT_TRUE(s.ok()) << s.message;
    std::unique_ptr<GpuTexture> texture;
    s = GpuTexture::createAttachment(*dev_, 16, 16, 1, texture);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuBuffer positions;
    hostBuffer(rect(0.0f, 0.0f, 16.0f, 16.0f), kGpuBufferDefaultUsage, positions);
    if (HasFatalFailure()) {
        return;
    }

    int checks = 0;
    ctx->setAbortCheck([&checks] { return ++checks > kAbortAfter; });
    const RasterParams params = rasterParams(16, 16, 1.0f);
    s = ctx->beginRaster(*texture, { 0.0f, 0.0f, 0.0f, 0.0f });
    ASSERT_TRUE(s.ok()) << s.message;
    int recorded = 0;
    for (int i = 0; i < 1000; ++i) {
        s = ctx->draw(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions }, bytesOf(params), 6);
        if (!s.ok()) {
            break;
        }
        ++recorded;
    }
    EXPECT_EQ(recorded, kAbortAfter);
    EXPECT_TRUE(s.aborted) << s.message;
    EXPECT_EQ(s.result, VK_INCOMPLETE);
    EXPECT_EQ(checks, kAbortAfter + 1);

    // Once aborted, the context neither records nor runs commands, nor asks again.
    s = ctx->draw(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions }, bytesOf(params), 6);
    EXPECT_TRUE(s.aborted);
    EXPECT_EQ(checks, kAbortAfter + 1);
    EXPECT_TRUE(ctx->endRaster().ok());
    s = ctx->submitAndWait();
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.aborted) << s.message;
}

GPU_TEST_F(GpuRasterTest, AbortCheckAlsoStopsDispatches)
{
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    GpuStatus s = GpuRecordContext::create(registry, ctx);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuBuffer commands;
    s = GpuBuffer::createDeviceLocal(*dev_, 4 * sizeof(VkDrawIndexedIndirectCommand), kGpuBufferDefaultUsage, commands);
    ASSERT_TRUE(s.ok()) << s.message;
    bool abort = false;
    ctx->setAbortCheck([&abort] { return abort; });
    s = ctx->dispatch(GpuKernelId::raster_test_indirect, { &commands }, IndirectParams { 4, 4 }, { 4, 1, 1 });
    ASSERT_TRUE(s.ok()) << s.message;
    abort = true;
    s = ctx->dispatch(GpuKernelId::raster_test_indirect, { &commands }, IndirectParams { 4, 4 }, { 4, 1, 1 });
    EXPECT_TRUE(s.aborted) << s.message;
    EXPECT_FALSE(ctx->submitAndWait().ok());
}

GPU_TEST_F(GpuRasterTest, AbortedSubmitStillSignals)
{
    GpuKernelRegistry registry(*dev_);
    GpuBuffer commands;
    GpuStatus s = GpuBuffer::createDeviceLocal(*dev_, 4 * sizeof(VkDrawIndexedIndirectCommand), kGpuBufferDefaultUsage,
                                               commands);
    ASSERT_TRUE(s.ok()) << s.message;
    VkSemaphore sem = VK_NULL_HANDLE;
    makeTimeline(0, sem);
    if (HasFatalFailure()) {
        return;
    }
    {
        std::unique_ptr<GpuRecordContext> first;
        ASSERT_TRUE(GpuRecordContext::create(registry, first).ok());
        s = first->dispatch(GpuKernelId::raster_test_indirect, { &commands }, IndirectParams { 4, 4 }, { 4, 1, 1 });
        ASSERT_TRUE(s.ok()) << s.message;
        s = first->submit({}, TimelineValue { sem, 1 });
        ASSERT_TRUE(s.ok()) << s.message;

        // The aborted context's waits still order its signal after the first context's.
        std::unique_ptr<GpuRecordContext> aborted;
        ASSERT_TRUE(GpuRecordContext::create(registry, aborted).ok());
        aborted->setAbortCheck([] { return true; });
        s = aborted->dispatch(GpuKernelId::raster_test_indirect, { &commands }, IndirectParams { 4, 4 }, { 4, 1, 1 });
        EXPECT_TRUE(s.aborted) << s.message;
        const TimelineValue wait { sem, 1 };
        s = aborted->submit(std::span<const TimelineValue>(&wait, 1), TimelineValue { sem, 2 });
        EXPECT_FALSE(s.ok());
        EXPECT_TRUE(s.aborted) << s.message;
        EXPECT_EQ(waitFor(sem, 2), VK_SUCCESS);
        EXPECT_FALSE(aborted->submit({}, TimelineValue { sem, 3 }).ok());

        std::unique_ptr<GpuRecordContext> next;
        ASSERT_TRUE(GpuRecordContext::create(registry, next).ok());
        const TimelineValue afterAbort { sem, 2 };
        s = next->submit(std::span<const TimelineValue>(&afterAbort, 1), TimelineValue { sem, 3 });
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_EQ(waitFor(sem, 3), VK_SUCCESS);
    }
    EXPECT_FALSE(dev_->isLost());
    vkDestroySemaphore(dev_->device(), sem, nullptr);
}

GPU_TEST_F(GpuRasterTest, RasterContextOutsideTheComputeFamilyTakesNoTextures)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    const GpuDeviceInfo& info = dev_->info();
    const bool crossFamily = info.graphicsFamily != info.computeFamily;
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuTextureDesc desc;
    desc.width = 2;
    desc.height = 2;
    desc.channels = 1;
    desc.filter = GpuTextureFilter::Nearest;
    const std::vector<float> texels(4, 0.5f);
    const GpuTexture* texture = nullptr;
    s = ctx->texture(desc, texels, texture);
    EXPECT_EQ(s.ok(), !crossFamily) << s.message;
    const GpuTexture* cached = nullptr;
    s = ctx->cachedTexture("raster-family-test", desc, texels, cached);
    EXPECT_EQ(s.ok(), !crossFamily) << s.message;
    if (crossFamily) {
        EXPECT_EQ(texture, nullptr);
        EXPECT_EQ(cached, nullptr);
        GpuBinding binding;
        binding.kind = GpuBinding::Kind::Texture;
        EXPECT_FALSE(ctx->dispatchBindings(GpuKernelId::raster_test_indirect, std::span<const GpuBinding>(&binding, 1),
                                           {}, { 1, 1, 1 })
                         .ok());
    } else {
        std::printf("raster: the graphics family is the compute family, so the texture refusals are not exercised\n");
    }
    s = ctx->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;
}

GPU_TEST_F(GpuRasterTest, ComputeResultsCrossIntoARasterContext)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    // A compute context writes the commands and hands them over; the raster context waits on its signal.
    const bool crossFamily = dev_->info().graphicsFamily != dev_->info().computeFamily;
    RecordProperty("cross_family", crossFamily ? "1" : "0");
    if (!crossFamily) {
        std::printf("raster: the graphics family is the compute family, so no ownership transfer is recorded and this "
                    "pass does not cover a cross-family handoff\n");
    }
    constexpr uint32_t kCommands = 4;
    constexpr uint32_t kDrawn = 3;
    constexpr uint32_t kW = 2 * kCommands;
    std::vector<float> xy;
    std::vector<uint32_t> indices;
    for (uint32_t k = 0; k < kCommands; ++k) {
        const float x = 2.0f * k;
        for (float v : { x + 0.1f, -1.0f, x + 0.9f, -1.0f, x + 0.5f, 2.0f }) {
            xy.push_back(v);
        }
        for (uint32_t j = 0; j < 3; ++j) {
            indices.push_back(3 * k + j);
        }
    }
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuTexture> texture;
    GpuStatus s = GpuTexture::createAttachment(*dev_, kW, 1, 1, texture);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuBuffer positions;
    GpuBuffer indexBuffer;
    hostBuffer(xy, kGpuBufferDefaultUsage, positions);
    hostBuffer(indices, kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer);
    GpuBuffer commands;
    const VkDeviceSize commandBytes = kCommands * sizeof(VkDrawIndexedIndirectCommand);
    s = GpuBuffer::createDeviceLocal(*dev_, commandBytes, kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                     commands);
    ASSERT_TRUE(s.ok()) << s.message;
    GpuBuffer readback;
    s = GpuBuffer::createStaging(*dev_, texture->packedBytes(), kGpuBufferDefaultUsage, readback);
    ASSERT_TRUE(s.ok()) << s.message;
    if (HasFatalFailure()) {
        return;
    }

    VkSemaphoreTypeCreateInfo ti { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    si.pNext = &ti;
    VkSemaphore timeline = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateSemaphore(dev_->device(), &si, nullptr, &timeline), VK_SUCCESS);
    {
        std::unique_ptr<GpuRecordContext> compute;
        s = GpuRecordContext::create(registry, compute);
        ASSERT_TRUE(s.ok()) << s.message;
        s = compute->dispatch(GpuKernelId::raster_test_indirect, { &commands }, IndirectParams { kCommands, kDrawn },
                              { kCommands, 1, 1 });
        ASSERT_TRUE(s.ok()) << s.message;
        s = compute->releaseToOtherQueue(commands, 0, commandBytes);
        ASSERT_TRUE(s.ok()) << s.message;
        s = compute->submit({}, TimelineValue { timeline, 1 });
        ASSERT_TRUE(s.ok()) << s.message;

        std::unique_ptr<GpuRecordContext> raster;
        s = GpuRecordContext::createRaster(registry, raster);
        ASSERT_TRUE(s.ok()) << s.message;
        s = raster->acquireFromOtherQueue(commands, 0, commandBytes);
        ASSERT_TRUE(s.ok()) << s.message;
        const RasterParams params = rasterParams(kW, 1, 1.0f);
        ASSERT_TRUE(raster->beginRaster(*texture, { 0.0f, 0.0f, 0.0f, 0.0f }).ok());
        s = raster->drawIndexedIndirect(GpuRasterId::raster_test, GpuRasterBlend::Additive, { &positions },
                                        bytesOf(params), indexBuffer, commands, kCommands);
        ASSERT_TRUE(s.ok()) << s.message;
        ASSERT_TRUE(raster->endRaster().ok());
        ASSERT_TRUE(raster->copyToImage(readback).ok());
        const TimelineValue wait { timeline, 1 };
        s = raster->submit(std::span<const TimelineValue>(&wait, 1), TimelineValue { timeline, 2 });
        ASSERT_TRUE(s.ok()) << s.message;
        uint64_t value = 2;
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &value;
        EXPECT_EQ(vkWaitSemaphores(dev_->device(), &wi, 30'000'000'000ull), VK_SUCCESS);
    }
    vkDestroySemaphore(dev_->device(), timeline, nullptr);

    const float* texels = static_cast<const float*>(readback.mapped());
    for (uint32_t x = 0; x < kW; ++x) {
        EXPECT_EQ(texels[x], (x % 2 == 0 && x / 2 < kDrawn) ? 1.0f : 0.0f) << "pixel " << x;
    }
}

bool
formatHas(VkPhysicalDevice pd, VkFormat format, VkFormatFeatureFlags need)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, format, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

GPU_TEST_F(GpuRasterTest, VisibilityCapabilityIsReported)
{
    const GpuDeviceInfo& info = dev_->info();
    const VkPhysicalDevice pd = dev_->physicalDevice();
    const VkFormatFeatureFlags idTarget = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const bool depth = formatHas(pd, VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    const bool rg = formatHas(pd, VK_FORMAT_R32G32_UINT, idTarget);
    const bool rgba = formatHas(pd, VK_FORMAT_R32G32B32A32_UINT, idTarget);
    std::printf("raster: rasterVisibility=%d (D32F depth=%d, RG32UI=%d, RGBA32UI=%d, geometryShader=%d, graphics "
                "queue=%d, vertexPipelineStoresAndAtomics=%d, fragmentStoresAndAtomics=%d)\n",
                int(info.rasterVisibility), int(depth), int(rg), int(rgba), int(info.geometryShader),
                int(info.graphicsQueue), int(info.vertexPipelineStores), int(info.fragmentStores));
    EXPECT_EQ(info.rasterVisibility, info.graphicsQueue && info.dynamicRendering && info.vertexPipelineStores && info.fragmentStores && info.geometryShader && depth && rg && rgba);
}

GPU_TEST_F(GpuRasterTest, UintAttachmentsClearAndCopyBackExactly)
{
    if (!dev_->info().rasterVisibility) {
        GPUTEST_SKIP_TEST() << "the device cannot raster uint ids with a depth test";
    }
    constexpr uint32_t kW = 13;
    constexpr uint32_t kH = 5;
    const std::array<uint32_t, 4> clear { 7u, 0xffffffffu, 0x80000001u, 9u };
    for (VkFormat format : { VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT }) {
        const uint32_t channels = format == VK_FORMAT_R32G32_UINT ? 2 : 4;
        SCOPED_TRACE(std::to_string(channels) + " channels");
        GpuKernelRegistry registry(*dev_);
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
        ASSERT_TRUE(s.ok()) << s.message;
        std::unique_ptr<GpuTexture> texture;
        s = GpuTexture::createAttachment(*dev_, kW, kH, format, texture);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_TRUE(texture->isAttachment());
        EXPECT_FALSE(texture->isDepth());
        EXPECT_EQ(texture->format(), format);
        EXPECT_EQ(texture->packedBytes(), VkDeviceSize(kW) * kH * channels * sizeof(uint32_t));
        std::unique_ptr<GpuTexture> depth;
        s = GpuTexture::createDepthAttachment(*dev_, kW, kH, depth);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_TRUE(depth->isDepth());
        EXPECT_FALSE(depth->isAttachment());
        GpuBuffer readback;
        s = GpuBuffer::createStaging(*dev_, texture->packedBytes(), kGpuBufferDefaultUsage, readback);
        ASSERT_TRUE(s.ok()) << s.message;

        s = ctx->beginRasterUint(*texture, clear, depth.get());
        ASSERT_TRUE(s.ok()) << s.message;
        s = ctx->endRaster();
        ASSERT_TRUE(s.ok()) << s.message;
        s = ctx->copyToImage(readback);
        ASSERT_TRUE(s.ok()) << s.message;
        s = ctx->submitAndWait();
        ASSERT_TRUE(s.ok()) << s.message;

        const uint32_t* texels = static_cast<const uint32_t*>(readback.mapped());
        size_t mismatches = 0;
        for (size_t i = 0; i < size_t(kW) * kH * channels; ++i) {
            if (texels[i] != clear[i % channels]) {
                ++mismatches;
            }
        }
        EXPECT_EQ(mismatches, 0u);
    }
}

// A triangle over the whole attachment and a nearer one over part of it, drawn in one call so that their
// primitive ids are their places in the buffer. With the depth test the nearer one's ids win wherever it
// covers a pixel centre, whichever comes first; without it the later one wins everywhere it covers.
GPU_TEST_F(GpuRasterTest, DepthKeepsTheNearerTrianglesIdsInEitherDrawOrder)
{
    if (!dev_->info().rasterVisibility) {
        GPUTEST_SKIP_TEST() << "the device cannot raster uint ids with a depth test";
    }
    constexpr uint32_t kW = 9;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kFarTag = 100;
    constexpr uint32_t kNearTag = 200;
    // The long edge, x + y = 8.2, passes through no pixel centre, whose coordinates sum to a whole number.
    const std::vector<float> nearTriangle { 1.0f, 1.0f, 0.25f, 7.2f, 1.0f, 0.25f, 1.0f, 7.2f, 0.25f };
    const std::vector<float> farTriangle { -1.0f, -1.0f, 0.75f, 3.0f * kW, -1.0f, 0.75f, -1.0f, 3.0f * kH, 0.75f };
    const auto insideNear = [](float cx, float cy) { return cx > 1.0f && cy > 1.0f && cx + cy < 8.2f; };

    struct RasterIdsParams {
        float viewport[2];
        float pad[2];
    };
    const RasterIdsParams params { { float(kW), float(kH) }, { 0.0f, 0.0f } };
    const std::span<const std::byte> push = std::as_bytes(std::span(&params, 1));

    for (const bool nearFirst : { false, true }) {
        for (const bool depthTest : { true, false }) {
            SCOPED_TRACE(std::string(nearFirst ? "near first" : "far first") + (depthTest ? ", depth" : ", no depth"));
            std::vector<float> vertices = nearFirst ? nearTriangle : farTriangle;
            const std::vector<float>& second = nearFirst ? farTriangle : nearTriangle;
            vertices.insert(vertices.end(), second.begin(), second.end());
            // float3 is a 16-byte element in a structured buffer.
            std::vector<float> padded;
            for (size_t v = 0; v < 6; ++v) {
                padded.insert(padded.end(), { vertices[3 * v], vertices[3 * v + 1], vertices[3 * v + 2], 0.0f });
            }
            const std::vector<uint32_t> tags = nearFirst ? std::vector<uint32_t> { kNearTag, kFarTag }
                                                         : std::vector<uint32_t> { kFarTag, kNearTag };

            GpuKernelRegistry registry(*dev_);
            std::unique_ptr<GpuRecordContext> ctx;
            GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
            ASSERT_TRUE(s.ok()) << s.message;
            std::unique_ptr<GpuTexture> ids;
            s = GpuTexture::createAttachment(*dev_, kW, kH, VK_FORMAT_R32G32_UINT, ids);
            ASSERT_TRUE(s.ok()) << s.message;
            std::unique_ptr<GpuTexture> depth;
            s = GpuTexture::createDepthAttachment(*dev_, kW, kH, depth);
            ASSERT_TRUE(s.ok()) << s.message;
            GpuBuffer vertexBuffer;
            GpuBuffer tagBuffer;
            hostBuffer(padded, kGpuBufferDefaultUsage, vertexBuffer);
            hostBuffer(tags, kGpuBufferDefaultUsage, tagBuffer);
            GpuBuffer readback;
            s = GpuBuffer::createStaging(*dev_, ids->packedBytes(), kGpuBufferDefaultUsage, readback);
            ASSERT_TRUE(s.ok()) << s.message;
            if (HasFatalFailure()) {
                return;
            }

            s = ctx->beginRasterUint(*ids, { 0xffffffffu, 0xffffffffu, 0u, 0u }, depthTest ? depth.get() : nullptr);
            ASSERT_TRUE(s.ok()) << s.message;
            s = ctx->draw(GpuRasterId::raster_test_ids, GpuRasterBlend::Replace, { &vertexBuffer, &tagBuffer }, push, 6);
            ASSERT_TRUE(s.ok()) << s.message;
            s = ctx->endRaster();
            ASSERT_TRUE(s.ok()) << s.message;
            s = ctx->copyToImage(readback);
            ASSERT_TRUE(s.ok()) << s.message;
            s = ctx->submitAndWait();
            ASSERT_TRUE(s.ok()) << s.message;

            const uint32_t nearPrimitive = nearFirst ? 0 : 1;
            const uint32_t* texels = static_cast<const uint32_t*>(readback.mapped());
            size_t mismatches = 0;
            size_t nearPixels = 0;
            for (uint32_t y = 0; y < kH; ++y) {
                for (uint32_t x = 0; x < kW; ++x) {
                    const bool near = insideNear(x + 0.5f, y + 0.5f);
                    nearPixels += near ? 1 : 0;
                    const bool nearWins = near && (depthTest || !nearFirst);
                    const uint32_t tag = nearWins ? kNearTag : kFarTag;
                    const uint32_t primitive = nearWins ? nearPrimitive : 1 - nearPrimitive;
                    const uint32_t* got = texels + 2 * (size_t(y) * kW + x);
                    if (got[0] != tag || got[1] != primitive) {
                        ADD_FAILURE() << "pixel (" << x << ", " << y << "): (" << got[0] << ", " << got[1]
                                      << "), expected (" << tag << ", " << primitive << ")";
                        ++mismatches;
                    }
                }
            }
            EXPECT_EQ(mismatches, 0u);
            EXPECT_GT(nearPixels, 10u);
        }
    }
}

GPU_TEST_F(GpuRasterTest, VisibilityPassesRefuseMismatchedTargets)
{
    if (!dev_->info().rasterVisibility) {
        GPUTEST_SKIP_TEST() << "the device cannot raster uint ids with a depth test";
    }
    GpuKernelRegistry registry(*dev_);
    std::unique_ptr<GpuRecordContext> ctx;
    GpuStatus s = GpuRecordContext::createRaster(registry, ctx);
    ASSERT_TRUE(s.ok()) << s.message;
    std::unique_ptr<GpuTexture> ids;
    s = GpuTexture::createAttachment(*dev_, 4, 4, VK_FORMAT_R32G32_UINT, ids);
    ASSERT_TRUE(s.ok()) << s.message;
    std::unique_ptr<GpuTexture> depth;
    s = GpuTexture::createDepthAttachment(*dev_, 4, 4, depth);
    ASSERT_TRUE(s.ok()) << s.message;
    std::unique_ptr<GpuTexture> smallDepth;
    s = GpuTexture::createDepthAttachment(*dev_, 2, 4, smallDepth);
    ASSERT_TRUE(s.ok()) << s.message;
    std::unique_ptr<GpuTexture> unsupported;
    EXPECT_FALSE(GpuTexture::createAttachment(*dev_, 4, 4, VK_FORMAT_R32G32_SFLOAT, unsupported).ok());
    EXPECT_FALSE(GpuTexture::createDepthAttachment(*dev_, 0, 4, unsupported).ok());
    EXPECT_EQ(unsupported, nullptr);

    EXPECT_FALSE(ctx->beginRaster(*ids, { 0.0f, 0.0f, 0.0f, 0.0f }).ok()) << "a uint attachment takes uint clears";
    EXPECT_FALSE(ctx->beginRasterUint(*depth, { 0u, 0u, 0u, 0u }).ok()) << "a depth texture is no colour attachment";
    EXPECT_FALSE(ctx->beginRasterUint(*ids, { 0u, 0u, 0u, 0u }, ids.get()).ok()) << "ids are no depth";
    EXPECT_FALSE(ctx->beginRasterUint(*ids, { 0u, 0u, 0u, 0u }, smallDepth.get()).ok()) << "the extents differ";
    if (dev_->info().rasterFloatBlend) {
        std::unique_ptr<GpuTexture> values;
        s = GpuTexture::createAttachment(*dev_, 4, 4, 1, values);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_FALSE(ctx->beginRasterUint(*values, { 0u, 0u, 0u, 0u }).ok()) << "a float attachment takes float clears";
    }

    GpuBuffer vertexBuffer;
    GpuBuffer tagBuffer;
    hostBuffer(std::vector<float>(24, 0.0f), kGpuBufferDefaultUsage, vertexBuffer);
    hostBuffer(std::vector<uint32_t>(2, 0u), kGpuBufferDefaultUsage, tagBuffer);
    if (HasFatalFailure()) {
        return;
    }
    const float params[4] = { 4.0f, 4.0f, 0.0f, 0.0f };
    const std::span<const std::byte> push = std::as_bytes(std::span(params));
    s = ctx->beginRasterUint(*ids, { 0u, 0u, 0u, 0u }, depth.get());
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_FALSE(ctx->draw(GpuRasterId::raster_test_ids, GpuRasterBlend::Additive, { &vertexBuffer, &tagBuffer }, push, 3)
                     .ok())
        << "ids cannot blend";
    s = ctx->draw(GpuRasterId::raster_test_ids, GpuRasterBlend::Replace, { &vertexBuffer, &tagBuffer }, push, 3);
    EXPECT_TRUE(s.ok()) << s.message;
    s = ctx->endRaster();
    ASSERT_TRUE(s.ok()) << s.message;
    s = ctx->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;
}

GPU_TEST_F(GpuRasterTest, PipelineCacheKeysTheDepthTest)
{
    if (!dev_->info().rasterVisibility) {
        GPUTEST_SKIP_TEST() << "the device cannot raster uint ids with a depth test";
    }
    const fs::path dir = fs::temp_directory_path() / ("natron-gpu-raster-depth-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    {
        GpuKernelRegistry registry(*dev_);
        GpuPipelineCacheOptions o;
        o.directory = dir.string();
        GpuPipelineCache cache(*dev_, o);
        std::shared_ptr<GpuRasterPipeline> plain;
        std::shared_ptr<GpuRasterPipeline> tested;
        std::shared_ptr<GpuRasterPipeline> again;
        GpuStatus s = cache.raster(registry, GpuRasterId::raster_test_ids, VK_FORMAT_R32G32_UINT, GpuRasterBlend::Replace,
                                   plain);
        ASSERT_TRUE(s.ok()) << s.message;
        s = cache.raster(registry, GpuRasterId::raster_test_ids, VK_FORMAT_R32G32_UINT, GpuRasterBlend::Replace, tested,
                         GpuRasterDepth::Less);
        ASSERT_TRUE(s.ok()) << s.message;
        s = cache.raster(registry, GpuRasterId::raster_test_ids, VK_FORMAT_R32G32_UINT, GpuRasterBlend::Replace, again,
                         GpuRasterDepth::Less);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_NE(plain, tested);
        EXPECT_EQ(tested, again);
        EXPECT_EQ(plain->depth(), GpuRasterDepth::None);
        EXPECT_EQ(tested->depth(), GpuRasterDepth::Less);
        EXPECT_EQ(cache.stats().rasterBuilds, 2u);
        std::shared_ptr<GpuRasterPipeline> refused;
        EXPECT_EQ(cache.raster(registry, GpuRasterId::raster_test_ids, VK_FORMAT_R32G32B32A32_UINT,
                               GpuRasterBlend::Additive, refused, GpuRasterDepth::Less)
                      .result,
                  VK_ERROR_FORMAT_NOT_SUPPORTED);
        EXPECT_EQ(refused, nullptr);
    }
    fs::remove_all(dir, ec);
}

} // namespace
