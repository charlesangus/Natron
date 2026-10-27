#pragma once

#include "GpuDevice.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>

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

// Device textures kept under a caller's key, so that texels many dispatches sample unchanged, such
// as a colour transform's LUTs, are uploaded once per device and shared by every caller: record
// contexts and the viewer alike. The key must identify the description and the texels, since a
// lookup of a key already held returns its texture whatever it is given.
//
// Once the cached textures take more than the budget, entries that nothing outside the cache
// holds go, least recently used first. A held entry is never released, so the cache may run over
// its budget while its textures are in use.
//
// Thread safe. The cache and every texture it handed out must be destroyed before its GpuDevice,
// once the recorded work sampling them is done.
class GpuTextureCache {
public:
    static constexpr uint64_t kDefaultBudgetBytes = 256ull << 20;

    explicit GpuTextureCache(GpuDevice& device, uint64_t budgetBytes = kDefaultBudgetBytes);
    GpuTextureCache(const GpuTextureCache&) = delete;
    GpuTextureCache& operator=(const GpuTextureCache&) = delete;

    // The texture under `key`, created from `desc` and `texels` as GpuTexture::create when the
    // cache does not hold it. `uploaded`, when given, tells whether this call created it.
    GpuStatus acquire(const std::string& key, const GpuTextureDesc& desc, std::span<const float> texels,
                      std::shared_ptr<const GpuTexture>& out, bool* uploaded = nullptr);

    // Textures created since the cache was, to tell a hit from an upload.
    size_t created() const;

    // Device bytes of the textures the cache holds.
    uint64_t bytes() const;

    size_t size() const;

private:
    struct Entry {
        std::shared_ptr<const GpuTexture> texture;
        uint64_t bytes = 0;
        uint64_t lastUse = 0;
    };

    void trimLocked();

    GpuDevice& device_;
    const uint64_t budget_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    uint64_t bytes_ = 0;
    uint64_t clock_ = 0;
    size_t created_ = 0;
};

} // namespace gpu
