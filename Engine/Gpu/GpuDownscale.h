#pragma once

#include "GpuKernel.h"

#include <cstdint>

namespace gpu {

class GpuRecordContext;

struct GpuDownscaleRect {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;

    bool empty() const { return x1 >= x2 || y1 >= y2; }
};

// Interleaved float pixels of `bounds` in a buffer, the first at offsetFloats, rows strideFloats apart.
struct GpuDownscaleView {
    const GpuBuffer* buffer = nullptr;
    uint64_t offsetFloats = 0;
    GpuDownscaleRect bounds;
    uint32_t strideFloats = 0;
    uint32_t nComps = 0;
};

// Mirrors MipDownscaleParams in Kernels/mipdownscale.slang, which has no padding between members.
struct GpuMipDownscalePush {
    uint32_t levels;
    uint32_t nComps;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t dstOffset;
    uint32_t dstStride;
    int32_t outX;
    int32_t outY;
    uint32_t outWidth;
    uint32_t outHeight;
};
static_assert(sizeof(GpuMipDownscalePush) == 56);
static_assert(sizeof(GpuMipDownscalePush) <= GpuKernel::kMaxPushConstantBytes);

// Records the box downscale of `source`, a level-0 image, into the `rect` pixels of `dest`, an image `levels` levels
// coarser: what the CPU's repeated halving of `source` makes, in which each halving averages the pixels of a 2x2 block
// that lie inside the previous level, so a partial edge block averages what it holds and an output pixel whose square
// misses `source` is 0. Source and destination have the same nComps. Any number of levels is recorded: more than one
// pass's worth runs as several passes, through scratch buffers of ctx's pool holding only what the next pass reads, with
// a barrier between passes. No barrier is recorded after the last dispatch.
GpuStatus recordMipDownscale(GpuRecordContext& ctx, const GpuDownscaleView& source, const GpuDownscaleView& dest,
                             const GpuDownscaleRect& rect, unsigned int levels);

} // namespace gpu
