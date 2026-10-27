#include "GpuTexture.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace gpu {

namespace {

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
    }

    constexpr uint64_t kUploadTimeoutNs = 30'000'000'000ull;

    VkImageType imageType(uint32_t dimensions)
    {
        return dimensions == 1 ? VK_IMAGE_TYPE_1D : dimensions == 2 ? VK_IMAGE_TYPE_2D
                                                                    : VK_IMAGE_TYPE_3D;
    }

    uint32_t storedChannels(uint32_t channels)
    {
        return channels == 3 ? 4 : channels;
    }

    struct StagingBuffer {
        VmaAllocator allocator = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        ~StagingBuffer()
        {
            if (allocation) {
                vmaDestroyBuffer(allocator, buffer, allocation);
            }
        }
    };

} // namespace

VkFormat
GpuTexture::formatFor(uint32_t channels)
{
    switch (channels) {
    case 1:
        return VK_FORMAT_R32_SFLOAT;
    case 2:
        return VK_FORMAT_R32G32_SFLOAT;
    case 3:
    case 4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool
GpuTexture::linearFilterSupported(GpuDevice& device, uint32_t channels)
{
    const VkFormat format = formatFor(channels);
    if (format == VK_FORMAT_UNDEFINED) {
        return false;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT
        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

VkImageViewType
GpuTexture::viewType() const
{
    return desc_.dimensions == 1 ? VK_IMAGE_VIEW_TYPE_1D
        : desc_.dimensions == 2  ? VK_IMAGE_VIEW_TYPE_2D
                                 : VK_IMAGE_VIEW_TYPE_3D;
}

GpuStatus
GpuTexture::create(GpuDevice& device, const GpuTextureDesc& desc, std::span<const float> texels,
                   std::unique_ptr<GpuTexture>& out)
{
    if (device.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "GpuTexture::create: device lost");
    }
    const VkFormat format = formatFor(desc.channels);
    if (desc.dimensions < 1 || desc.dimensions > 3 || format == VK_FORMAT_UNDEFINED || desc.width == 0
        || desc.height == 0 || desc.depth == 0 || (desc.dimensions < 2 && desc.height != 1)
        || (desc.dimensions < 3 && desc.depth != 1)) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuTexture::create: invalid texture description");
    }
    const uint64_t texelCount = uint64_t(desc.width) * desc.height * desc.depth;
    if (texels.size() != texelCount * desc.channels) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuTexture::create: " + std::to_string(texels.size()) + " floats for " + std::to_string(texelCount) + " texels of " + std::to_string(desc.channels) + " channels");
    }

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format, &fp);
    const VkFormatFeatureFlags base = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((fp.optimalTilingFeatures & base) != base) {
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "GpuTexture::create: the device cannot sample " + std::to_string(desc.channels) + "-channel float textures");
    }
    if (desc.filter == GpuTextureFilter::Linear
        && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "GpuTexture::create: the device cannot filter " + std::to_string(desc.channels) + "-channel float textures linearly");
    }
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties ifp {};
    const VkResult fr = vkGetPhysicalDeviceImageFormatProperties(device.physicalDevice(), format,
                                                                 imageType(desc.dimensions), VK_IMAGE_TILING_OPTIMAL,
                                                                 usage, 0, &ifp);
    if (fr != VK_SUCCESS || desc.width > ifp.maxExtent.width || desc.height > ifp.maxExtent.height
        || desc.depth > ifp.maxExtent.depth) {
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "GpuTexture::create: the device does not support a " + std::to_string(desc.dimensions) + "D texture of " + std::to_string(desc.width) + "x" + std::to_string(desc.height) + "x" + std::to_string(desc.depth));
    }

    std::unique_ptr<GpuTexture> t(new GpuTexture(device, desc));
    t->format_ = format;
    const VkDevice dev = device.device();

    VkImageCreateInfo ici { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = imageType(desc.dimensions);
    ici.format = format;
    ici.extent = { desc.width, desc.height, desc.depth };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (GpuStatus s = device.check(vmaCreateImage(device.allocator(), &ici, &aci, &t->image_, &t->allocation_, nullptr),
                                   "vmaCreateImage");
        !s) {
        return s;
    }

    VkImageViewCreateInfo vci { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t->image_;
    vci.viewType = t->viewType();
    vci.format = format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (GpuStatus s = device.check(vkCreateImageView(dev, &vci, nullptr, &t->view_), "vkCreateImageView"); !s) {
        return s;
    }

    const VkFilter filter = desc.filter == GpuTextureFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = filter;
    sci.minFilter = filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (GpuStatus s = device.check(vkCreateSampler(dev, &sci, nullptr, &t->sampler_), "vkCreateSampler"); !s) {
        return s;
    }

    if (GpuStatus s = t->upload(texels); !s) {
        return s;
    }
    out = std::move(t);
    return {};
}

GpuStatus
GpuTexture::upload(std::span<const float> texels)
{
    const VkDevice dev = device_.device();
    const uint32_t stored = storedChannels(desc_.channels);
    const uint64_t texelCount = uint64_t(desc_.width) * desc_.height * desc_.depth;
    const VkDeviceSize bytes = texelCount * stored * sizeof(float);

    std::vector<float> expanded;
    std::span<const float> src = texels;
    if (stored != desc_.channels) {
        expanded.resize(texelCount * stored);
        for (uint64_t i = 0; i < texelCount; ++i) {
            expanded[i * 4 + 0] = texels[i * 3 + 0];
            expanded[i * 4 + 1] = texels[i * 3 + 1];
            expanded[i * 4 + 2] = texels[i * 3 + 2];
            expanded[i * 4 + 3] = 1.0f;
        }
        src = expanded;
    }

    StagingBuffer staging;
    staging.allocator = device_.allocator();
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (GpuStatus s = device_.check(vmaCreateBuffer(staging.allocator, &bci, &aci, &staging.buffer, &staging.allocation, nullptr),
                                    "vmaCreateBuffer");
        !s) {
        return s;
    }
    if (GpuStatus s = device_.check(vmaCopyMemoryToAllocation(staging.allocator, src.data(), staging.allocation, 0, bytes),
                                    "vmaCopyMemoryToAllocation");
        !s) {
        return s;
    }

    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = device_.queueFamily(QueueKind::Compute);
    VkCommandPool pool = VK_NULL_HANDLE;
    if (GpuStatus s = device_.check(vkCreateCommandPool(dev, &pci, nullptr, &pool), "vkCreateCommandPool"); !s) {
        return s;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBufferBeginInfo cbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GpuStatus s = device_.check(vkAllocateCommandBuffers(dev, &cai, &cmd), "vkAllocateCommandBuffers");
    if (s) {
        s = device_.check(vkBeginCommandBuffer(cmd, &cbi), "vkBeginCommandBuffer");
    }
    if (!s) {
        vkDestroyCommandPool(dev, pool, nullptr);
        return s;
    }

    VkImageMemoryBarrier2 toDst { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image_;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toDst;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { desc_.width, desc_.height, desc_.depth };
    vkCmdCopyBufferToImage(cmd, staging.buffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Later dispatches on the compute queue are ordered after this barrier by submission order,
    // so they need no barrier of their own.
    VkImageMemoryBarrier2 toRead = toDst;
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dep.pImageMemoryBarriers = &toRead;
    vkCmdPipelineBarrier2(cmd, &dep);

    if (s = device_.check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"); !s) {
        vkDestroyCommandPool(dev, pool, nullptr);
        return s;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (s = device_.check(vkCreateFence(dev, &fi, nullptr, &fence), "vkCreateFence"); !s) {
        vkDestroyCommandPool(dev, pool, nullptr);
        return s;
    }
    VkCommandBufferSubmitInfo csi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    csi.commandBuffer = cmd;
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &csi;
    s = device_.submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), fence);
    bool mayBePending = (s.result == VK_ERROR_DEVICE_LOST);
    if (s) {
        VkResult waited = vkWaitForFences(dev, 1, &fence, VK_TRUE, kUploadTimeoutNs);
        if (waited == VK_TIMEOUT) {
            std::fprintf(stderr, "GpuTexture::create: the upload did not finish within %llu ms; the GPU device is treated as lost\n",
                         static_cast<unsigned long long>(kUploadTimeoutNs / 1000000));
            device_.abandonInFlightWork("vkWaitForFences");
            // The copy may still run, so the staging buffer, the fence and the command pool are leaked.
            staging.allocation = nullptr;
            return fail(VK_ERROR_DEVICE_LOST, "GpuTexture::create: the upload timed out");
        }
        if (waited != VK_SUCCESS) {
            s = device_.check(waited, "vkWaitForFences");
            mayBePending = true;
        }
    }
    vkDestroyFence(dev, fence, nullptr);
    // A pool whose command buffer may still be pending cannot be destroyed; it is leaked on a lost device.
    if (!mayBePending) {
        vkDestroyCommandPool(dev, pool, nullptr);
    }
    return s;
}

GpuTexture::~GpuTexture()
{
    if (device_.hasAbandonedWork()) {
        return;
    }
    const VkDevice dev = device_.device();
    if (sampler_) {
        vkDestroySampler(dev, sampler_, nullptr);
    }
    if (view_) {
        vkDestroyImageView(dev, view_, nullptr);
    }
    if (allocation_) {
        vmaDestroyImage(device_.allocator(), image_, allocation_);
    }
}

GpuTextureCache::GpuTextureCache(GpuDevice& device, uint64_t budgetBytes)
    : device_(device)
    , budget_(budgetBytes)
{
}

GpuStatus
GpuTextureCache::acquire(const std::string& key, const GpuTextureDesc& desc, std::span<const float> texels,
                         std::shared_ptr<const GpuTexture>& out, bool* uploaded)
{
    out.reset();
    if (uploaded) {
        *uploaded = false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        found->second.lastUse = ++clock_;
        out = found->second.texture;
        return {};
    }
    // Created under the lock, so that callers asking for the same key at once upload it once.
    std::unique_ptr<GpuTexture> created;
    if (GpuStatus s = GpuTexture::create(device_, desc, texels, created); !s) {
        return s;
    }
    Entry entry;
    entry.texture = std::shared_ptr<const GpuTexture>(std::move(created));
    entry.bytes = uint64_t(desc.width) * desc.height * desc.depth * storedChannels(desc.channels) * sizeof(float);
    entry.lastUse = ++clock_;
    out = entry.texture;
    bytes_ += entry.bytes;
    ++created_;
    if (uploaded) {
        *uploaded = true;
    }
    entries_.emplace(key, std::move(entry));
    trimLocked();
    return {};
}

void
GpuTextureCache::trimLocked()
{
    while (bytes_ > budget_) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if ((it->second.texture.use_count() == 1) && ((oldest == entries_.end()) || (it->second.lastUse < oldest->second.lastUse))) {
                oldest = it;
            }
        }
        if (oldest == entries_.end()) {
            return;
        }
        bytes_ -= oldest->second.bytes;
        entries_.erase(oldest);
    }
}

size_t
GpuTextureCache::created() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return created_;
}

uint64_t
GpuTextureCache::bytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

size_t
GpuTextureCache::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace gpu
