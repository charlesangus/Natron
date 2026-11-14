#pragma once

#include "GpuBlur.h"

#include "Global/EdgeMode.h"

#include <array>
#include <cstdint>
#include <vector>

namespace gpu {

using NATRON_NAMESPACE::EdgeMode;

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
    // The matrix's six doubles: for the exact kernels (impulse, box, every clamped filter and every
    // filter on a minifying matrix) each as its low then its high 32 bits, for the others the six
    // high floats' bits and then the six low floats'.
    uint32_t matrix[12];
    int32_t lobes;
    float B;
    float C;
    uint32_t padding;
};
static_assert(sizeof(GpuResampleParams) == 128);

inline constexpr uint32_t kGpuResampleMaxChannels = 4;
inline constexpr uint32_t kGpuResampleFilterCount = 17;
// The kernel picks a blurred sample's matrix with 32-bit integer arithmetic that needs the count
// below 2^16.
inline constexpr uint32_t kGpuResampleMaxMatrices = 65535;

// Whether one Jacobian of the matrix is zero, which sends box to its nearest pixel.
struct GpuResampleJacobian {
    bool zero = true;
};

// One destination-to-source pixel matrix, rows (m[0] m[1] m[2]) and (m[3] m[4] m[5]) and the third
// row `perspective`, with its Jacobians indexed by which axes sample inside the source: bit 0 x,
// bit 1 y; entry 0 must be zero. A third row other than (0, 0, 1) makes the matrix projective, sampled
// as Resampler's samplePixel does: divided by z, zero where z <= 0, with the Jacobian and the kernel
// scales worked out per pixel, so `jacobians` and the scales are not read. scaleX and scaleY are
// Resampler::kernelScale of the two rows, carried as the CPU's doubles so that the exact kernels'
// tap ranges fall where the CPU's do. weight is only read by a weighted blur.
struct GpuResampleMatrix {
    std::array<double, 6> matrix {};
    std::array<double, 3> perspective { 0., 0., 1. };
    std::array<GpuResampleJacobian, 4> jacobians {};
    double scaleX = 1.;
    double scaleY = 1.;
    double weight = 1.;

    bool isProjective() const
    {
        return perspective[0] != 0. || perspective[1] != 0. || perspective[2] != 1.;
    }
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

// Resampler::resampleRow over `output` for affine or projective inverse matrices. One matrix resamples it
// directly; several are averaged per pixel as Resampler::motionBlurPixel does, with `blur`. filter,
// lobes, B and C are the Resampler::FilterSpec; clamp is only honoured by the filters that clamp on
// the CPU, which the caller decides. A source without a buffer, or with empty bounds, renders zeros.
struct GpuResample {
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    uint32_t channels = 0;
    uint32_t filter = 0;
    int32_t lobes = 4;
    double B = 1. / 3.;
    double C = 1. / 3.;
    bool clamp = false;
    EdgeMode edgeMode = EdgeMode::Black;
    std::vector<GpuResampleMatrix> matrices;
    GpuResampleBlur blur;
};

GpuStatus recordResample(GpuRecordContext& ctx, const GpuResample& resample);

// Mirrors OffsetParams in Kernels/offset.slang. The valid rect is the part of the source the
// dispatch can read, in thread coordinates, the source's offset addressing the pixel its first
// thread reads. It lies within one pixel of the dispatch, so a large offset cannot overflow the
// kernel's signed arithmetic: a Stretch source wholly beside the dispatch is cut down to the one
// row or column next to it.
struct GpuOffsetParams {
    uint32_t width;
    uint32_t height;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t validX1;
    int32_t validY1;
    int32_t validX2;
    int32_t validY2;
    uint32_t nComps;
    uint32_t flags;
    // Where the row entries start in the weight buffer, in floats.
    uint32_t rowWeights;
};
static_assert(sizeof(GpuOffsetParams) == 52);

// A whole-pixel translation over `output`: dst(x, y) = src(x - dx, y - dy), where that pixel lies
// outside src.bounds the nearest pixel of src.bounds for Stretch and zero for Black. Every channel
// is copied bit for bit. A source without a buffer, or with empty bounds, renders zeros.
//
// With a matte, one weight and one black flag per column and per row of `output` from its
// origin, the pixel is then weighed as runResampleCpu() weighs it: written 0 where its column or
// row is black, otherwise left where w = column * row is 1 or more and multiplied by w, 0
// included, elsewhere. The weights travel as two floats each, so that multiply matches the CPU's
// double one to within a float rounding, and the black and one cases match it exactly.
struct GpuOffset {
    GpuBlurImage src;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    uint32_t channels = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    EdgeMode edgeMode = EdgeMode::Black;
    // All empty, or as many as output's columns and rows; a nonzero black flag blackens.
    std::vector<double> columnWeights;
    std::vector<double> rowWeights;
    std::vector<unsigned char> blackColumns;
    std::vector<unsigned char> blackRows;
};

GpuStatus recordOffset(GpuRecordContext& ctx, const GpuOffset& offset);

} // namespace gpu
