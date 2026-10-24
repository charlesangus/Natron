#pragma once

#include "GpuBlur.h"

#include <array>
#include <cstdint>
#include <vector>

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
// The kernel picks a blurred sample's matrix with 32-bit integer arithmetic that needs the count
// below 2^16.
inline constexpr uint32_t kGpuResampleMaxMatrices = 65535;

// What interpolateSuper does for one Jacobian of the matrix: nothing beyond the filter when it is
// zero, else 3^levelX x 3^levelY supersamples when minifying (both levels 0 for none).
struct GpuResampleJacobian {
    bool zero = true;
    uint32_t levelX = 0;
    uint32_t levelY = 0;
};

// One destination-to-source pixel matrix, rows (m[0] m[1] m[2]) and (m[3] m[4] m[5]), with its
// Jacobians indexed by which axes sample inside the source: bit 0 x, bit 1 y; entry 0 must be zero.
// weight is only read by a weighted blur.
struct GpuResampleMatrix {
    std::array<double, 6> matrix {};
    std::array<GpuResampleJacobian, 4> jacobians {};
    double weight = 1.;
};

// How Resampler::motionBlurPixel samples: the minimum and maximum samples per pixel, the standard
// error that stops sampling, the seed's offset unsigned(0x10000 * motionBlur), and whether the
// matrices' weights apply (a directional blur) or every matrix counts alike.
struct GpuResampleBlur {
    uint32_t minSamples = 0;
    uint32_t maxIterations = 0;
    double maxError = 0.;
    uint32_t seedOffset = 0;
    bool weighted = false;
};

// Resampler::resampleRow over `output` for affine inverse matrices. One matrix resamples it
// directly; several are averaged per pixel as Resampler::motionBlurPixel does, with `blur`. filter
// is the Resampler::FilterEnum value; clamp is only honoured by the filters that clamp on the CPU,
// which the caller decides. A source without a buffer, or with empty bounds, renders zeros.
struct GpuResample {
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    uint32_t channels = 0;
    uint32_t filter = 0;
    bool clamp = false;
    bool blackOutside = true;
    std::vector<GpuResampleMatrix> matrices;
    GpuResampleBlur blur;
};

GpuStatus recordResample(GpuRecordContext& ctx, const GpuResample& resample);

// Mirrors OffsetParams in Kernels/offset.slang. The valid rect is in thread coordinates, the
// source's offset addressing the pixel its first thread reads, so the kernel does no signed
// arithmetic that a large offset could overflow.
struct GpuOffsetParams {
    uint32_t width;
    uint32_t height;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t validX1;
    uint32_t validY1;
    uint32_t validX2;
    uint32_t validY2;
    uint32_t nComps;
};
static_assert(sizeof(GpuOffsetParams) == 44);

// A whole-pixel translation over `output`: dst(x, y) = src(x - dx, y - dy), zero where that pixel
// lies outside src.bounds. Every channel is copied bit for bit. A source without a buffer, or with
// empty bounds, renders zeros.
struct GpuOffset {
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    uint32_t channels = 0;
    int32_t dx = 0;
    int32_t dy = 0;
};

GpuStatus recordOffset(GpuRecordContext& ctx, const GpuOffset& offset);

} // namespace gpu
