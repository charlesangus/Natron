#pragma once

#include "GpuKernelRegistry.h"

#include <array>
#include <cstdint>
#include <span>

namespace gpu {

class GpuBuffer;
class GpuRecordContext;

// Mirrors WrapParams in Kernels/pixelwrap.slang, which has no padding between members.
struct GpuWrapParams {
    int32_t outX;
    int32_t outY;
    uint32_t width;
    uint32_t height;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t maskOffset;
    uint32_t maskStride;
    int32_t maskX1;
    int32_t maskY1;
    int32_t maskX2;
    int32_t maskY2;
    uint32_t auxOffset;
    uint32_t auxStride;
    uint32_t nComps;
    uint32_t channelMask;
    uint32_t maskChannel;
    uint32_t maskComps;
    int32_t divisorChannel;
    int32_t skipChannel;
    float mix;
    uint32_t flags;
};
static_assert(sizeof(GpuWrapParams) == 112);
static_assert(sizeof(GpuWrapParams) <= GpuKernel::kMaxPushConstantBytes);

inline constexpr uint32_t kGpuWrapFlagMask = 1;
inline constexpr uint32_t kGpuWrapFlagMaskInvert = 2;

// Bindings of every kernel built on pixelwrap.slang: source, destination, mask, then the op's
// parameter block (the filtered result for wrap_finish).
inline constexpr uint32_t kGpuWrapBindings = 4;

// Half-open pixel rect in image coordinates.
struct GpuWrapRect {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;

    bool empty() const { return x1 >= x2 || y1 >= y2; }
};

// Interleaved float pixels of `bounds` in a buffer, the first at offsetFloats, rows strideFloats apart.
struct GpuWrapView {
    const GpuBuffer* buffer = nullptr;
    uint64_t offsetFloats = 0;
    GpuWrapRect bounds;
    uint32_t strideFloats = 0;
    uint32_t nComps = 0;
};

// What the CPU point-op wrapping does around a point op, over `output`. The source reads zero
// outside its bounds, which may be empty; it has the destination's nComps.
struct GpuWrap {
    GpuWrapView src;
    // Must contain output.
    GpuWrapView dst;
    GpuWrapRect output;
    // 1 to 4.
    uint32_t nComps = 0;
    // Processed channels in the bit space of pixelKernelChannelBit: a one-channel pixel is alpha, bit 3.
    uint32_t channelMask = 0xF;
    // The mask reads zero outside its bounds, which may be empty.
    bool applyMask = false;
    GpuWrapView mask;
    uint32_t maskChannel = 0;
    bool maskInvert = false;
    float mix = 1.0f;
    // The "(Un)premult by" channel as a component of the source pixel, -1 for none; outside the
    // source it reads 1.
    int32_t divisorChannel = -1;
    // The component that is the divisor itself, left undivided, or -1.
    int32_t skipChannel = -1;
};

// An alpha a Composite step reads, in the output's channels by channel bit: component `channel` of
// `view` for every channel or, with channel -1, `view` read per channel as a step's input is read.
// It reads zero outside the view's bounds, which may be empty.
struct GpuWrapAlpha {
    GpuWrapView view;
    int32_t channel = -1;
};

// One step of a Composite op's fold over `output`: `result`, the result so far, combined with
// `input` into `dst`. Both read zero outside their bounds, which may be empty. `result` has nComps
// channels; `input` has 1 to 4 and is read in the output's channels by channel bit. The step's
// GpuWrapParams describe `result` as the source and `input` in the mask fields, maskComps being its
// channel count, with no mask flag.
//
// With `alpha`, the step also reads resultAlpha, the result's alpha, and inputAlpha, the input's,
// and writes the new result's alpha to dstAlpha, which has nComps channels and must contain output;
// its kernel is built with runCompositeAlphaStep (Kernels/compositealpha.slang).
struct GpuWrapStep {
    GpuWrapView result;
    GpuWrapView input;
    // Must contain output.
    GpuWrapView dst;
    GpuWrapRect output;
    // 1 to 4.
    uint32_t nComps = 0;
    uint32_t channelMask = 0xF;
    bool alpha = false;
    GpuWrapAlpha resultAlpha;
    GpuWrapAlpha inputAlpha;
    GpuWrapView dstAlpha;
};

// Bindings of every kernel built on compositestep.slang: those of pixelwrap.slang, with the mask
// binding unused, then the step's input.
inline constexpr uint32_t kGpuWrapStepBindings = kGpuWrapBindings + 1;

// Bindings of every kernel built on compositealpha.slang: those of compositestep.slang, then the
// alpha geometry, the result's alpha, the input's alpha and the destination alpha.
inline constexpr uint32_t kGpuWrapAlphaStepBindings = kGpuWrapStepBindings + 4;

// The alpha geometry block of compositealpha.slang, in words: per alpha view, the result's then the
// input's, its offset, row stride, x1, y1, x2, y2, channel count (0 reads zero) and channel
// (kGpuWrapAlphaPerChannel to read it per channel), then the destination alpha's offset and row
// stride from the output's origin.
inline constexpr uint32_t kGpuWrapAlphaViewWords = 8;
inline constexpr uint32_t kGpuWrapAlphaGeometryWords = 2 * kGpuWrapAlphaViewWords + 2;
inline constexpr uint32_t kGpuWrapAlphaPerChannel = 0xFFFFFFFFu;
using GpuWrapAlphaGeometry = std::array<uint32_t, kGpuWrapAlphaGeometryWords>;

// Validates the geometry of `wrap` and packs it for a dispatch over its output; buffers are only
// checked when recording. `filtered`, wrap_finish's input, must contain the output and have
// wrap.nComps channels.
GpuStatus wrapParams(const GpuWrap& wrap, GpuWrapParams& out, const GpuWrapView* filtered = nullptr);

// Validates the geometry of `step` and packs it for a dispatch over its output, as GpuWrapStep
// describes.
GpuStatus wrapStepParams(const GpuWrapStep& step, GpuWrapParams& out);

// Validates the alphas of `step`, which must have `alpha`, and packs their geometry.
GpuStatus wrapStepAlphaGeometry(const GpuWrapStep& step, GpuWrapAlphaGeometry& out);

// Records `kernel`, a point op entry built with runWrapped, reading its parameters from `opParams`.
// Without opParams the binding gets a scratch buffer.
GpuStatus recordWrapped(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap,
                        const GpuBuffer* opParams = nullptr);

// recordWrapped() for an entry that also reads `inputs`, bound in order after the parameter block.
// An input with empty bounds gets a stand-in buffer, so the kernel must read none of its pixels.
GpuStatus recordWrappedInputs(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap,
                              const GpuBuffer* opParams, std::span<const GpuWrapView> inputs);

// Records one dispatch of `kernel`, a Composite step entry built with runCompositeStep, or with
// runCompositeAlphaStep when step.alpha is set, reading its parameters from `opParams`.
GpuStatus recordWrapStep(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrapStep& step,
                         const GpuBuffer* opParams = nullptr);

// Records a Composite op's fold and its finish into wrap.dst over wrap.output: the result starts as
// `base` and `kernel` folds each of `inputs` into it in order, one dispatch per input, alternating
// between two scratch buffers of the output's size; wrap_finish then blends the result with
// wrap.src, the blend input, by mask x mix and passes unprocessed channels through from it. With no
// input the result is `base`. `base` has wrap.nComps channels and every view reads zero outside its
// bounds. No input is divided, so wrap.divisorChannel must be -1.
//
// With `alphas`, the base's alpha then one per input, the fold carries the result's alpha: `kernel`
// is built with runCompositeAlphaStep, the result's alpha starts as the base's, and each step reads
// it from and writes it to two more scratch buffers.
GpuStatus recordWrapFold(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuWrapView& base,
                         std::span<const GpuWrapView> inputs, const GpuBuffer* opParams = nullptr,
                         std::span<const GpuWrapAlpha> alphas = {});

// Writes the source over wrap.output into wrap.dst, divided by the divisor where it is usable, as
// a spatial filter's input. Channel mask, mask and mix are ignored.
GpuStatus recordWrapDivide(GpuRecordContext& ctx, const GpuWrap& wrap);

// Writes wrap.dst over wrap.output from `filtered`, the spatial filter's result over the same rect,
// multiplied back, blended with the undivided source and with unprocessed channels passed through.
// Pixels outside a filter's processed window pass through with mix 0 and no mask.
GpuStatus recordWrapFinish(GpuRecordContext& ctx, const GpuWrap& wrap, const GpuWrapView& filtered);

} // namespace gpu
