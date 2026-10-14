#pragma once

#include "GpuBlur.h"

#include <array>
#include <cstdint>

namespace gpu {

class GpuRecordContext;

// Mirrors ResampleParams in Kernels/resample.slang, which has no padding between members.
struct GpuResampleParams {
    int32_t outX;
    int32_t outY;
    uint32_t width;
    uint32_t height;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t nComps;
    uint32_t filter;
    uint32_t flags;
    uint32_t states;
    float hHi[6];
    float hLo[6];
};
static_assert(sizeof(GpuResampleParams) == 112);

inline constexpr uint32_t kGpuResampleMaxChannels = 4;
inline constexpr uint32_t kGpuResampleFilterCount = 10;
inline constexpr uint32_t kGpuResampleMaxLevel = 4;

// What interpolateSuper does for one Jacobian of the matrix: nothing beyond the filter when it is
// zero, else 3^levelX x 3^levelY supersamples when minifying (both levels 0 for none).
struct GpuResampleJacobian {
    bool zero = true;
    uint32_t levelX = 0;
    uint32_t levelY = 0;
};

// Resampler::resampleRow over `output` for one affine inverse matrix (destination pixel to source
// pixel, rows (m[0] m[1] m[2]) and (m[3] m[4] m[5])) without motion blur. filter is the
// Resampler::FilterEnum value; clamp is only honoured by the filters that clamp on the CPU, which
// the caller decides. jacobians are indexed by which axes sample inside the source: bit 0 x,
// bit 1 y; entry 0 must be zero. A source without a buffer, or with empty bounds, renders zeros.
struct GpuResample {
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    uint32_t channels = 0;
    uint32_t filter = 0;
    bool clamp = false;
    bool blackOutside = true;
    std::array<double, 6> matrix {};
    std::array<GpuResampleJacobian, 4> jacobians {};
};

GpuStatus recordResample(GpuRecordContext& ctx, const GpuResample& resample);

} // namespace gpu
