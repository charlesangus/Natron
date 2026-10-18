#pragma once

#include "GpuDevice.h"

#include <cstdint>
#include <memory>
#include <span>

namespace gpu {

enum class GpuTextureFilter {
    Nearest,
    Linear,
};

struct GpuTextureDesc {
    // 1, 2 or 3; the unused extents must be 1.
    uint32_t dimensions = 2;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    // Channels per texel of the source data, 1 to 4. Three-channel float formats are not
    // sampleable everywhere, so three channels are stored as four with alpha 1.
    uint32_t channels = 4;
    GpuTextureFilter filter = GpuTextureFilter::Linear;
};

// An immutable, sampled float texture with its sampler (clamp to edge, no mipmaps, normalized
// coordinates), in SHADER_READ_ONLY_OPTIMAL on the compute queue family. It must outlive the
// recorded work that samples it and be destroyed before its GpuDevice.
class GpuTexture {
public:
    // R32_SFLOAT, R32G32_SFLOAT or R32G32B32A32_SFLOAT; VK_FORMAT_UNDEFINED outside 1..4.
    static VkFormat formatFor(uint32_t channels);

    // Whether a texture of `channels` can be created with GpuTextureFilter::Linear, so that a
    // caller can pick a fallback before creating one.
    static bool linearFilterSupported(GpuDevice& device, uint32_t channels);

    // Uploads `texels` (x fastest, then y, then z; `channels` floats per texel) through a staging
    // buffer and blocks until the copy is done. Fails with VK_ERROR_FORMAT_NOT_SUPPORTED when the
    // device cannot sample the format, cannot filter it linearly while Linear is asked for, or
    // does not support the extent.
    static GpuStatus create(GpuDevice& device, const GpuTextureDesc& desc, std::span<const float> texels,
                            std::unique_ptr<GpuTexture>& out);

    ~GpuTexture();
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    const GpuTextureDesc& desc() const { return desc_; }
    VkFormat format() const { return format_; }
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    VkImageViewType viewType() const;

private:
    GpuTexture(GpuDevice& device, const GpuTextureDesc& desc)
        : device_(device)
        , desc_(desc)
    {
    }

    GpuStatus upload(std::span<const float> texels);

    GpuDevice& device_;
    GpuTextureDesc desc_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace gpu
