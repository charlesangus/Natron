#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    std::printf("raster: dynamicRendering=%d rasterFloatBlend=%d (RGBA32F blend=%d, R32F blend=%d, graphics queue=%d)\n",
                int(info.dynamicRendering), int(info.rasterFloatBlend), int(rgba), int(r), int(info.graphicsQueue));
    // Dynamic rendering is core in Vulkan 1.3, which every accepted device implements.
    EXPECT_TRUE(info.dynamicRendering);
    EXPECT_EQ(info.rasterFloatBlend, info.dynamicRendering && rgba && r && info.graphicsQueue);
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

GPU_TEST_F(GpuRasterTest, OverlappingTrianglesAddAndFollowTheTopLeftRule)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 4;
    // Every edge runs through pixel centres: a centre on a left or top edge is covered, one on a right or
    // bottom edge is not, and the diagonal each rectangle's two triangles share is covered once.
    const uint32_t firstColumns[2] = { 0, 2 };
    const uint32_t endColumns[2] = { 5, 7 };
    constexpr uint32_t kEndRow = 2;
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
            hostBuffer(rect(firstColumns[i] + 0.5f, 0.5f, endColumns[i] + 0.5f, kEndRow + 0.5f),
                       kGpuBufferDefaultUsage, positions[i]);
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
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                float expected = 0.0f;
                for (int i = 0; i < 2; ++i) {
                    if (y < kEndRow && x >= firstColumns[i] && x < endColumns[i]) {
                        expected += 1.0f;
                    }
                }
                for (uint32_t c = 0; c < channels; ++c) {
                    const float got = texels[(size_t(y) * kW + x) * channels + c];
                    if (got != expected) {
                        ADD_FAILURE() << "pixel (" << x << ", " << y << ") channel " << c << ": " << got
                                      << ", expected " << expected;
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(mismatches, 0u);
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

    // Once aborted, the context neither records nor submits, nor asks again.
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

GPU_TEST_F(GpuRasterTest, ComputeResultsCrossIntoARasterContext)
{
    if (!dev_->info().rasterFloatBlend) {
        GPUTEST_SKIP_TEST() << "the device cannot blend float colour attachments";
    }
    // A compute context writes the commands and hands them over; the raster context waits on its signal.
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

} // namespace
