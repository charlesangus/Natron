#pragma once

#include "GpuKernel.h"

#include <array>
#include <cstdint>

namespace gpu {

// Host side of kernels/blur.slang (entry blurPass): validates a pass against the kernel's
// fixed-size shared tile and picks the dispatch shape for the path the kernel will take.
struct GpuBlurPass
{
    uint32_t width = 0;
    uint32_t height = 0;
    // Interleaved float channels per pixel.
    uint32_t channels = 0;
    // Taps run from -radius to +radius; the weights buffer holds 2 * radius + 1 floats.
    uint32_t radius = 0;
    bool vertical = false;
    // Clamp to the edge pixel (true) or read zero outside the image (false).
    bool neumann = true;
};

inline constexpr uint32_t kGpuBlurMaxRadius = 512;
inline constexpr uint32_t kGpuBlurMaxChannels = 4;

GpuStatus validateBlurPass(const GpuBlurPass& pass);

// Groups to dispatch for `pass`; only meaningful for a pass validateBlurPass accepts.
std::array<uint32_t, 3> blurGroupCount(const GpuBlurPass& pass);

// Records one pass of `blur`, a GpuKernel built from blurPass with bindings {src, dst, weights}.
// A radius above kGpuBlurMaxRadius or more than kGpuBlurMaxChannels channels is refused, since
// the kernel's shared tile is sized for those limits.
GpuStatus recordBlurPass(GpuKernel& blur, VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkBuffer weights,
                         const GpuBlurPass& pass);

} // namespace gpu
