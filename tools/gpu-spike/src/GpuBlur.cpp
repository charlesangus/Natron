#include "GpuBlur.h"

#include <span>
#include <string>

namespace gpu {

namespace {

    // Must match kGroup, kColumns and kDirectMaxRadius in kernels/blur.slang.
    constexpr uint32_t kGroup = 128;
    constexpr uint32_t kColumns = 32;
    constexpr uint32_t kDirectMaxRadius = 32;

    // Mirrors BlurParams in blur.slang, which has no padding between members.
    struct BlurParams {
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        uint32_t radius;
        uint32_t vertical;
        uint32_t neumann;
    };

    uint32_t divUp(uint32_t v, uint32_t d)
    {
        return static_cast<uint32_t>((uint64_t(v) + d - 1) / d);
    }

} // namespace

GpuStatus
validateBlurPass(const GpuBlurPass& pass)
{
    if (pass.radius > kGpuBlurMaxRadius) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "blur radius " + std::to_string(pass.radius) + " exceeds the GPU blur limit of " + std::to_string(kGpuBlurMaxRadius) };
    }
    if (pass.channels == 0 || pass.channels > kGpuBlurMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "GPU blur supports 1 to " + std::to_string(kGpuBlurMaxChannels) + " channels, got " + std::to_string(pass.channels) };
    }
    return {};
}

std::array<uint32_t, 3>
blurGroupCount(const GpuBlurPass& pass)
{
    const uint32_t len = pass.vertical ? pass.height : pass.width;
    const uint32_t lines = pass.vertical ? pass.width : pass.height;
    if (pass.vertical && pass.radius <= kDirectMaxRadius) {
        return { divUp(len, kGroup), divUp(lines, kColumns), 1 };
    }
    return { divUp(len, kGroup), lines, 1 };
}

GpuStatus
recordBlurPass(GpuKernel& blur, VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkBuffer weights,
               const GpuBlurPass& pass)
{
    if (GpuStatus s = validateBlurPass(pass); !s) {
        return s;
    }
    const GpuKernelDesc& d = blur.desc();
    if (d.groupSize[0] != kGroup || d.groupSize[1] != 1 || d.groupSize[2] != 1 || d.storageBufferCount != 3
        || d.pushConstantBytes != sizeof(BlurParams)) {
        return { VK_ERROR_INITIALIZATION_FAILED, "recordBlurPass: kernel is not blurPass" };
    }
    const BlurParams p { pass.width, pass.height, pass.channels, pass.radius, pass.vertical ? 1u : 0u,
                         pass.neumann ? 1u : 0u };
    const VkBuffer buffers[] = { src, dst, weights };
    return blur.recordGroups(cmd, buffers, std::as_bytes(std::span(&p, 1)), blurGroupCount(pass));
}

} // namespace gpu
