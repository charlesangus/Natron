#pragma once

#include "GpuBlur.h"

#include <cstdint>
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
};
static_assert(sizeof(GpuMorphParams) == 64);

inline constexpr uint32_t kGpuMorphMaxRadius = 1024;
inline constexpr uint32_t kGpuMorphMaxChannels = 4;

// One min (or max with takeMax) filter along an axis over windows of 2 * half + 1 pixels.
struct GpuMorphPass {
    uint32_t half = 0;
    bool vertical = false;
    bool takeMax = false;
};

// The buffer the CPU's spatial filters run over for `output`: output grown by the halo and clipped
// to rod, or empty. Records it into a scratch buffer of 4 ints held by ctx as (x1, y1, x2, y2), all
// zero when empty, followed by a compute barrier.
GpuStatus recordFilterWindow(GpuRecordContext& ctx, const GpuBlurRect& output, const GpuBlurRect& rod, int32_t haloX,
                             int32_t haloY, GpuBuffer*& window);

// ErodeDilateKernels::filterLine over the lines of a buffer, as ErodeDilate::render() runs it. The
// buffer is output grown by the halo and clipped to rod; each line clamps its windows to the
// buffer, and output pixels outside the buffer are zero.
struct GpuMorphology {
    // Reads zero outside its bounds, which may be empty.
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    GpuBlurRect rod;
    int32_t haloX = 0;
    int32_t haloY = 0;
    // One or two passes, in order, on different axes.
    std::span<const GpuMorphPass> passes;
    uint32_t channels = 0;
};

// Records the passes into ctx, with the window and the intermediate lines in scratch buffers held
// by ctx and compute barriers between dependent dispatches. A half above kGpuMorphMaxRadius or
// more than kGpuMorphMaxChannels channels is refused.
GpuStatus recordMorphology(GpuRecordContext& ctx, const GpuMorphology& morph);

} // namespace gpu
