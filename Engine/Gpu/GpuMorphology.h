#pragma once

#include "GpuBlur.h"

#include <cstdint>
#include <optional>
#include <span>

namespace gpu {

class GpuRecordContext;

// Mirrors MorphParams in Kernels/morphology.slang, which has no padding between members.
struct GpuMorphParams {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t half;
    uint32_t vertical;
    uint32_t takeMax;
    int32_t outX;
    int32_t outY;
    uint32_t dstOffset;
    uint32_t dstStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t rodX1;
    int32_t rodY1;
    int32_t rodX2;
    int32_t rodY2;
    int32_t haloX;
    int32_t haloY;
    uint32_t maskChannel;
    uint32_t maskInvert;
};
static_assert(sizeof(GpuMorphParams) == 96);

inline constexpr uint32_t kGpuMorphMaxRadius = 1024;
inline constexpr uint32_t kGpuMorphMaxChannels = 4;

// One min (or max with takeMax) filter along an axis over windows of 2 * half + 1 pixels.
struct GpuMorphPass {
    uint32_t half = 0;
    bool vertical = false;
    bool takeMax = false;
};

// The mask the CPU shrinks its processed window by: one float channel of a view.
struct GpuMorphMask {
    GpuBlurImage image;
    uint32_t nComps = 0;
    uint32_t channel = 0;
    bool invert = false;
};

// The buffer the CPU's spatial filters run over for `output`: output, or with a mask the part of it
// that shrinkToMask() keeps, grown by the halo and clipped to rod, or empty. Records its computation
// into a scratch buffer of 8 ints held by ctx, whose [4, 8) receive the rect as (x1, y1, x2, y2),
// all zero when empty, followed by a compute barrier.
GpuStatus recordFilterWindow(GpuRecordContext& ctx, const GpuBlurRect& output, const GpuBlurRect& rod, int32_t haloX,
                             int32_t haloY, const std::optional<GpuMorphMask>& mask, GpuBuffer*& window);

// ErodeDilateKernels::filterLine over the lines of a buffer, as ErodeDilate::render() runs it. The
// buffer is output, or with a mask the part of output that shrinkToMask() keeps, grown by the
// halo and clipped to rod; each line clamps its windows to the buffer, and output pixels outside
// the buffer are zero.
struct GpuMorphology {
    // Reads zero outside its bounds, which may be empty.
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    GpuBlurRect rod;
    int32_t haloX = 0;
    int32_t haloY = 0;
    std::optional<GpuMorphMask> mask;
    // One or two passes, in order, on different axes.
    std::span<const GpuMorphPass> passes;
    uint32_t channels = 0;
};

// Records the passes into ctx, with the window and the intermediate lines in scratch buffers held
// by ctx and compute barriers between dependent dispatches. A half above kGpuMorphMaxRadius or
// more than kGpuMorphMaxChannels channels is refused.
GpuStatus recordMorphology(GpuRecordContext& ctx, const GpuMorphology& morph);

} // namespace gpu
