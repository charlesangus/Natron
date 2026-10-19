#pragma once

#include "GpuKernel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace gpu {

class GpuBuffer;
class GpuRecordContext;

// Where a pass reads and writes, in image pixel coordinates. Offsets and strides count floats.
struct GpuBlurRegion {
    // Image position of the first output pixel, stored at dstOffset in the destination.
    int32_t outX = 0;
    int32_t outY = 0;
    uint32_t dstOffset = 0;
    uint32_t dstStride = 0;
    // The source pixels that exist, the first stored at srcOffset; samples outside are black.
    int32_t srcX1 = 0;
    int32_t srcY1 = 0;
    int32_t srcX2 = 0;
    int32_t srcY2 = 0;
    uint32_t srcOffset = 0;
    uint32_t srcStride = 0;
    // The extent of the filtered line along the blur axis: taps and outputs beyond it are zero.
    int32_t lineLo = 0;
    int32_t lineHi = 0;
};

// Host side of Kernels/blur.slang (entry blurPass): validates a pass against the kernel's
// fixed-size shared tile and picks the dispatch shape for the path the kernel will take.
struct GpuBlurPass {
    // The output rect's size.
    uint32_t width = 0;
    uint32_t height = 0;
    // Interleaved float channels per pixel.
    uint32_t channels = 0;
    // Taps run from -radius to +radius; the weights buffer holds the radius + 1 weights of offsets
    // 0 to radius, the kernel being symmetric.
    uint32_t radius = 0;
    bool vertical = false;
    // Unset, source and destination are both the whole tightly packed width x height image.
    std::optional<GpuBlurRegion> region;
};

// Mirrors BlurParams in Kernels/blur.slang, which has no padding between members.
struct GpuBlurParams {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t radius;
    uint32_t vertical;
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
    int32_t lineLo;
    int32_t lineHi;
};

inline constexpr uint32_t kGpuBlurMaxRadius = 512;
inline constexpr uint32_t kGpuBlurMaxChannels = 4;
inline constexpr uint32_t kGpuBlurGroupSize = 128;

GpuStatus validateBlurPass(const GpuBlurPass& pass);

GpuBlurParams blurParams(const GpuBlurPass& pass);

// Groups to dispatch for `pass`; only meaningful for a pass validateBlurPass accepts.
std::array<uint32_t, 3> blurGroupCount(const GpuBlurPass& pass);

// Records one pass of `blur`, a GpuKernel built from blurPass with bindings {src, dst, weights}.
// A radius above kGpuBlurMaxRadius or more than kGpuBlurMaxChannels channels is refused, since
// the kernel's shared tile is sized for those limits.
GpuStatus recordBlurPass(GpuKernel& blur, VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkBuffer weights,
                         const GpuBlurPass& pass);

// Half-open pixel rect in image coordinates.
struct GpuBlurRect {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;

    bool empty() const { return x1 >= x2 || y1 >= y2; }
};

// Interleaved float pixels of `bounds` in a buffer, the first at offsetFloats, rows strideFloats apart.
struct GpuBlurImage {
    const GpuBuffer* buffer = nullptr;
    uint64_t offsetFloats = 0;
    GpuBlurRect bounds;
    uint32_t strideFloats = 0;
};

// A separable FIR blur of `output`, computed as a horizontal then a vertical pass over the lines of
// `domain`. Samples outside srcValid and taps beyond `domain` are black, and so are output pixels
// outside `domain`.
struct GpuSeparableBlur {
    GpuBlurImage src;
    // Within src.bounds, or empty when the source has no pixels.
    GpuBlurRect srcValid;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    GpuBlurRect domain;
    uint32_t channels = 0;
    // Symmetric half kernels: the weights of offsets 0 to radius.
    std::span<const float> weightsX;
    std::span<const float> weightsY;
};

// Records both passes into ctx, with the intermediate rows and the weights in scratch buffers
// held by ctx and a compute barrier between the passes.
GpuStatus recordSeparableBlur(GpuRecordContext& ctx, const GpuSeparableBlur& blur);

// The recursive line filters of Kernels/blur_lines.slang, the CPU's LineFilter kinds at order 0.
enum class GpuLineFilterKind : uint32_t {
    identity = 0,
    deriche = 1,
    vanVliet = 2,
    box = 3,
};

// One axis of a GpuLineBlur, with the coefficients LineFilter computed for it, in double.
struct GpuLineFilter {
    GpuLineFilterKind kind = GpuLineFilterKind::identity;
    // deriche: a0, a1, a2, a3, b1, b2, coefp, coefn.
    std::array<double, 8> deriche {};
    // vanVliet: [B, -b1, -b2, -b3] and the 3x3 Triggs matrix row by row.
    std::array<double, 4> vanVliet {};
    std::array<double, 9> triggs {};
    // box: the width (above 1) and the number of passes (1 to 3).
    float boxSize = 0.f;
    uint32_t iterations = 0;
};

// Mirrors LinesParams in Kernels/blur_lines.slang, which has no padding between members.
struct GpuLineBlurParams {
    uint32_t kind;
    uint32_t vertical;
    uint32_t channels;
    uint32_t lineCount;
    int32_t lineOrigin;
    int32_t outLo;
    int32_t outHi;
    uint32_t dstOffset;
    uint32_t dstPosStep;
    uint32_t dstLineStep;
    int32_t storeLo;
    uint32_t storePosStep;
    uint32_t storeLineStep;
    uint32_t fromSource;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t half;
    uint32_t iterations;
};
static_assert(sizeof(GpuLineBlurParams) == 88);

// A separable recursive blur of `output`, as Blur's CPU passes run LineFilter: every row of the
// filtered rect, then every column, each over the whole rect along its axis, with samples outside
// it black. Samples outside srcValid are black too, and so are output pixels outside the filtered
// rect.
struct GpuLineBlur {
    GpuBlurImage src;
    // Within src.bounds, or empty when the source has no pixels.
    GpuBlurRect srcValid;
    GpuBlurImage dst;
    // Within dst.bounds.
    GpuBlurRect output;
    // Holds every filtered rect the window may give.
    GpuBlurRect domain;
    // From recordFilterWindow(): the filtered rect, within domain.
    const GpuBuffer* window = nullptr;
    uint32_t channels = 0;
    GpuLineFilter filterX;
    GpuLineFilter filterY;
};

// Records the row pass, a compute barrier and the column pass into ctx, with the intermediate
// rows, the stored lines and the coefficients in scratch buffers held by ctx.
GpuStatus recordLineBlur(GpuRecordContext& ctx, const GpuLineBlur& blur);

} // namespace gpu
