#include "GpuWrap.h"

#include "GpuBuffer.h"
#include "GpuRecord.h"

#include <span>
#include <string>
#include <vector>

namespace gpu {

namespace {

    GpuStatus invalid(const std::string& what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, "pixel wrap: " + what };
    }

    bool contains(const GpuWrapRect& outer, const GpuWrapRect& inner)
    {
        return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 && inner.y2 <= outer.y2;
    }

    uint64_t rowFloats(const GpuWrapView& v)
    {
        return uint64_t(v.bounds.x2 - v.bounds.x1) * v.nComps;
    }

    // The kernels index with 32 bits.
    GpuStatus checkGeometry(const GpuWrapView& v, const char* name)
    {
        if (v.nComps == 0 || v.nComps > 4) {
            return invalid(std::string(name) + " must have 1 to 4 channels");
        }
        if (v.bounds.empty()) {
            return {};
        }
        if (v.strideFloats < rowFloats(v)) {
            return invalid(std::string(name) + " stride is shorter than a row");
        }
        const uint64_t rows = uint64_t(v.bounds.y2 - v.bounds.y1);
        if (v.offsetFloats + rows * v.strideFloats > UINT32_MAX) {
            return invalid(std::string(name) + " exceeds 32-bit indexing");
        }
        return {};
    }

    GpuStatus checkBuffer(const GpuWrapView& v, const char* name)
    {
        if (!v.buffer || !*v.buffer) {
            return invalid(std::string(name) + " has no buffer");
        }
        const uint64_t rows = uint64_t(v.bounds.y2 - v.bounds.y1);
        const uint64_t endFloats = v.offsetFloats + (rows - 1) * v.strideFloats + rowFloats(v);
        if (endFloats * sizeof(float) > v.buffer->size()) {
            return invalid(std::string(name) + " overruns its buffer");
        }
        return {};
    }

    uint32_t indexAt(const GpuWrapView& v, int32_t x, int32_t y)
    {
        return static_cast<uint32_t>(v.offsetFloats + uint64_t(y - v.bounds.y1) * v.strideFloats
                                     + uint64_t(x - v.bounds.x1) * v.nComps);
    }

    // A view that must hold the whole output, addressed from the output's origin.
    GpuStatus outputWindow(const GpuWrapView& v, const GpuWrapRect& output, uint32_t nComps, const char* name,
                           uint32_t& offset, uint32_t& stride)
    {
        if (GpuStatus s = checkGeometry(v, name); !s) {
            return s;
        }
        if (v.nComps != nComps) {
            return invalid(std::string(name) + " channel count differs from the output's");
        }
        if (!contains(v.bounds, output)) {
            return invalid(std::string(name) + " does not contain the output");
        }
        offset = indexAt(v, output.x1, output.y1);
        stride = v.strideFloats;
        return {};
    }

    // A buffer for a binding the kernel reads nothing from, requested once per dispatch.
    GpuStatus standIn(GpuRecordContext& ctx, const GpuBuffer*& unused)
    {
        if (unused) {
            return {};
        }
        GpuBuffer* scratch = nullptr;
        if (GpuStatus s = ctx.scratch(16, scratch); !s) {
            return s;
        }
        unused = scratch;
        return {};
    }

    // The source, destination, mask and parameter buffers of a dispatch of `wrap`.
    GpuStatus wrapBuffers(GpuRecordContext& ctx, const GpuWrap& wrap, const GpuBuffer* aux,
                          const GpuWrapView* filtered, const GpuBuffer*& unused,
                          const GpuBuffer* (&out)[kGpuWrapBindings])
    {
        if (GpuStatus s = checkBuffer(wrap.dst, "destination"); !s) {
            return s;
        }
        if (filtered) {
            if (GpuStatus s = checkBuffer(*filtered, "filtered result"); !s) {
                return s;
            }
        }
        const GpuBuffer* src = nullptr;
        if (!wrap.src.bounds.empty()) {
            if (GpuStatus s = checkBuffer(wrap.src, "source"); !s) {
                return s;
            }
            src = wrap.src.buffer;
        }
        const GpuBuffer* mask = nullptr;
        if (wrap.applyMask && !wrap.mask.bounds.empty()) {
            if (GpuStatus s = checkBuffer(wrap.mask, "mask"); !s) {
                return s;
            }
            mask = wrap.mask.buffer;
        }
        if (aux && !*aux) {
            return invalid("empty parameter buffer");
        }
        if (!src || !mask || !aux) {
            if (GpuStatus s = standIn(ctx, unused); !s) {
                return s;
            }
        }
        out[0] = src ? src : unused;
        out[1] = wrap.dst.buffer;
        out[2] = mask ? mask : unused;
        out[3] = aux ? aux : unused;
        return {};
    }

    GpuStatus record(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuBuffer* aux,
                     const GpuWrapView* filtered)
    {
        GpuWrapParams p;
        if (GpuStatus s = wrapParams(wrap, p, filtered); !s) {
            return s;
        }
        if (p.width == 0 || p.height == 0) {
            return {};
        }
        const GpuBuffer* unused = nullptr;
        const GpuBuffer* b[kGpuWrapBindings];
        if (GpuStatus s = wrapBuffers(ctx, wrap, aux, filtered, unused, b); !s) {
            return s;
        }
        return ctx.dispatch(kernel, { b[0], b[1], b[2], b[3] }, p, { p.width, p.height, 1 });
    }

} // namespace

GpuStatus
wrapParams(const GpuWrap& wrap, GpuWrapParams& out, const GpuWrapView* filtered)
{
    out = {};
    const uint32_t n = wrap.nComps;
    if (n == 0 || n > 4) {
        return invalid("1 to 4 channels are supported, got " + std::to_string(n));
    }
    if (wrap.divisorChannel >= int32_t(n) || wrap.skipChannel >= int32_t(n)) {
        return invalid("divisor or skip channel out of range");
    }
    if (wrap.output.empty()) {
        return {};
    }
    out.outX = wrap.output.x1;
    out.outY = wrap.output.y1;
    out.width = uint32_t(wrap.output.x2 - wrap.output.x1);
    out.height = uint32_t(wrap.output.y2 - wrap.output.y1);
    out.nComps = n;
    out.channelMask = wrap.channelMask & 0xFu;
    out.mix = wrap.mix;
    out.divisorChannel = wrap.divisorChannel < 0 ? -1 : wrap.divisorChannel;
    out.skipChannel = wrap.skipChannel < 0 ? -1 : wrap.skipChannel;

    if (GpuStatus s = outputWindow(wrap.dst, wrap.output, n, "destination", out.dstOffset, out.dstStride); !s) {
        return s;
    }
    if (filtered) {
        if (GpuStatus s = outputWindow(*filtered, wrap.output, n, "filtered result", out.auxOffset, out.auxStride); !s) {
            return s;
        }
    }

    if (!wrap.src.bounds.empty()) {
        if (GpuStatus s = checkGeometry(wrap.src, "source"); !s) {
            return s;
        }
        if (wrap.src.nComps != n) {
            return invalid("source channel count differs from the output's");
        }
        out.srcOffset = static_cast<uint32_t>(wrap.src.offsetFloats);
        out.srcStride = wrap.src.strideFloats;
        out.srcX1 = wrap.src.bounds.x1;
        out.srcY1 = wrap.src.bounds.y1;
        out.srcX2 = wrap.src.bounds.x2;
        out.srcY2 = wrap.src.bounds.y2;
    }

    if (wrap.applyMask) {
        if (GpuStatus s = checkGeometry(wrap.mask, "mask"); !s) {
            return s;
        }
        if (wrap.maskChannel >= wrap.mask.nComps) {
            return invalid("mask channel out of range");
        }
        out.flags |= kGpuWrapFlagMask;
        if (wrap.maskInvert) {
            out.flags |= kGpuWrapFlagMaskInvert;
        }
        out.maskChannel = wrap.maskChannel;
        out.maskComps = wrap.mask.nComps;
        if (!wrap.mask.bounds.empty()) {
            out.maskOffset = static_cast<uint32_t>(wrap.mask.offsetFloats);
            out.maskStride = wrap.mask.strideFloats;
            out.maskX1 = wrap.mask.bounds.x1;
            out.maskY1 = wrap.mask.bounds.y1;
            out.maskX2 = wrap.mask.bounds.x2;
            out.maskY2 = wrap.mask.bounds.y2;
        }
    }
    return {};
}

GpuStatus
wrapStepParams(const GpuWrapStep& step, GpuWrapParams& out)
{
    GpuWrap wrap;
    wrap.src = step.result;
    wrap.dst = step.dst;
    wrap.output = step.output;
    wrap.nComps = step.nComps;
    wrap.channelMask = step.channelMask;
    if (GpuStatus s = wrapParams(wrap, out); !s) {
        return s;
    }
    if (step.output.empty() || step.input.bounds.empty()) {
        return {};
    }
    if (GpuStatus s = checkGeometry(step.input, "step input"); !s) {
        return s;
    }
    out.maskOffset = static_cast<uint32_t>(step.input.offsetFloats);
    out.maskStride = step.input.strideFloats;
    out.maskX1 = step.input.bounds.x1;
    out.maskY1 = step.input.bounds.y1;
    out.maskX2 = step.input.bounds.x2;
    out.maskY2 = step.input.bounds.y2;
    out.maskComps = step.input.nComps;
    return {};
}

namespace {

    // Packs `alpha` at word `at` of `out`.
    GpuStatus packAlpha(const GpuWrapAlpha& alpha, const char* name, GpuWrapAlphaGeometry& out, uint32_t at)
    {
        for (uint32_t i = 0; i < kGpuWrapAlphaViewWords; ++i) {
            out[at + i] = 0;
        }
        if (alpha.view.bounds.empty() || alpha.view.nComps == 0) {
            return {};
        }
        if (GpuStatus s = checkGeometry(alpha.view, name); !s) {
            return s;
        }
        if (alpha.channel >= 0 && static_cast<uint32_t>(alpha.channel) >= alpha.view.nComps) {
            return invalid(std::string(name) + " channel is not one of its view's");
        }
        out[at] = static_cast<uint32_t>(alpha.view.offsetFloats);
        out[at + 1] = alpha.view.strideFloats;
        out[at + 2] = static_cast<uint32_t>(alpha.view.bounds.x1);
        out[at + 3] = static_cast<uint32_t>(alpha.view.bounds.y1);
        out[at + 4] = static_cast<uint32_t>(alpha.view.bounds.x2);
        out[at + 5] = static_cast<uint32_t>(alpha.view.bounds.y2);
        out[at + 6] = alpha.view.nComps;
        out[at + 7] = alpha.channel >= 0 ? static_cast<uint32_t>(alpha.channel) : kGpuWrapAlphaPerChannel;
        return {};
    }

    // The buffer of `alpha`, or a stand-in when it reads zero.
    GpuStatus alphaBuffer(GpuRecordContext& ctx, const GpuWrapAlpha& alpha, const char* name, const GpuBuffer*& unused,
                          const GpuBuffer*& out)
    {
        if (alpha.view.bounds.empty() || alpha.view.nComps == 0) {
            if (GpuStatus s = standIn(ctx, unused); !s) {
                return s;
            }
            out = unused;
            return {};
        }
        if (GpuStatus s = checkBuffer(alpha.view, name); !s) {
            return s;
        }
        out = alpha.view.buffer;
        return {};
    }

} // namespace

GpuStatus
wrapStepAlphaGeometry(const GpuWrapStep& step, GpuWrapAlphaGeometry& out)
{
    if (!step.alpha) {
        return invalid("the step carries no alpha");
    }
    if (GpuStatus s = packAlpha(step.resultAlpha, "result alpha", out, 0); !s) {
        return s;
    }
    if (GpuStatus s = packAlpha(step.inputAlpha, "input alpha", out, kGpuWrapAlphaViewWords); !s) {
        return s;
    }
    uint32_t offset = 0;
    uint32_t stride = 0;
    if (!step.output.empty()) {
        if (GpuStatus s = outputWindow(step.dstAlpha, step.output, step.nComps, "destination alpha", offset, stride); !s) {
            return s;
        }
    }
    out[2 * kGpuWrapAlphaViewWords] = offset;
    out[2 * kGpuWrapAlphaViewWords + 1] = stride;
    return {};
}

GpuStatus
recordWrapped(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuBuffer* opParams)
{
    return record(ctx, kernel, wrap, opParams, nullptr);
}

GpuStatus
recordWrappedInputs(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuBuffer* opParams,
                    std::span<const GpuWrapView> inputs)
{
    GpuWrapParams p;
    if (GpuStatus s = wrapParams(wrap, p); !s) {
        return s;
    }
    if (p.width == 0 || p.height == 0) {
        return {};
    }
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* b[kGpuWrapBindings];
    if (GpuStatus s = wrapBuffers(ctx, wrap, opParams, nullptr, unused, b); !s) {
        return s;
    }
    std::vector<const GpuBuffer*> buffers(b, b + kGpuWrapBindings);
    for (const GpuWrapView& input : inputs) {
        if (input.bounds.empty()) {
            if (GpuStatus s = standIn(ctx, unused); !s) {
                return s;
            }
            buffers.push_back(unused);
            continue;
        }
        if (GpuStatus s = checkGeometry(input, "input"); !s) {
            return s;
        }
        if (GpuStatus s = checkBuffer(input, "input"); !s) {
            return s;
        }
        buffers.push_back(input.buffer);
    }
    return ctx.dispatch(kernel, std::span<const GpuBuffer* const>(buffers.data(), buffers.size()),
                        std::as_bytes(std::span(&p, 1)), { p.width, p.height, 1 });
}

GpuStatus
recordWrapStep(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrapStep& step, const GpuBuffer* opParams)
{
    GpuWrapParams p;
    if (GpuStatus s = wrapStepParams(step, p); !s) {
        return s;
    }
    if (p.width == 0 || p.height == 0) {
        return {};
    }
    GpuWrap wrap;
    wrap.src = step.result;
    wrap.dst = step.dst;
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* b[kGpuWrapBindings];
    if (GpuStatus s = wrapBuffers(ctx, wrap, opParams, nullptr, unused, b); !s) {
        return s;
    }
    const GpuBuffer* input = nullptr;
    if (!step.input.bounds.empty()) {
        if (GpuStatus s = checkBuffer(step.input, "step input"); !s) {
            return s;
        }
        input = step.input.buffer;
    } else {
        if (GpuStatus s = standIn(ctx, unused); !s) {
            return s;
        }
        input = unused;
    }
    if (!step.alpha) {
        return ctx.dispatch(kernel, { b[0], b[1], b[2], b[3], input }, p, { p.width, p.height, 1 });
    }

    GpuWrapAlphaGeometry geometry;
    if (GpuStatus s = wrapStepAlphaGeometry(step, geometry); !s) {
        return s;
    }
    const GpuBuffer* resultAlpha = nullptr;
    const GpuBuffer* inputAlpha = nullptr;
    if (GpuStatus s = alphaBuffer(ctx, step.resultAlpha, "result alpha", unused, resultAlpha); !s) {
        return s;
    }
    if (GpuStatus s = alphaBuffer(ctx, step.inputAlpha, "input alpha", unused, inputAlpha); !s) {
        return s;
    }
    if (GpuStatus s = checkBuffer(step.dstAlpha, "destination alpha"); !s) {
        return s;
    }
    GpuBuffer* geometryBuffer = nullptr;
    if (GpuStatus s = ctx.scratch(sizeof(geometry), geometryBuffer); !s) {
        return s;
    }
    if (GpuStatus s = ctx.updateBuffer(*geometryBuffer, 0, std::as_bytes(std::span<const uint32_t>(geometry))); !s) {
        return s;
    }
    return ctx.dispatch(kernel, { b[0], b[1], b[2], b[3], input, geometryBuffer, resultAlpha, inputAlpha, step.dstAlpha.buffer },
                        p, { p.width, p.height, 1 });
}

GpuStatus
recordWrapFold(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuWrapView& base,
               std::span<const GpuWrapView> inputs, const GpuBuffer* opParams, std::span<const GpuWrapAlpha> alphas,
               bool resultIsAlpha)
{
    if (wrap.divisorChannel >= 0) {
        return invalid("a fold divides none of its inputs");
    }
    const bool carriesAlpha = !alphas.empty();
    if (carriesAlpha && alphas.size() != inputs.size() + 1) {
        return invalid("a fold carrying alpha has one alpha per input and the base's");
    }
    if (resultIsAlpha && (!carriesAlpha || inputs.empty())) {
        return invalid("a fold whose result is its alpha carries one and has an input");
    }
    GpuWrapParams p;
    if (GpuStatus s = wrapParams(wrap, p); !s) {
        return s;
    }
    if (p.width == 0 || p.height == 0) {
        return {};
    }
    const uint32_t n = wrap.nComps;
    if (!base.bounds.empty()) {
        if (GpuStatus s = checkGeometry(base, "base"); !s) {
            return s;
        }
        if (base.nComps != n) {
            return invalid("base channel count differs from the output's");
        }
    }

    const VkDeviceSize bytes = VkDeviceSize(p.width) * p.height * n * sizeof(float);
    GpuWrapView scratch[2];
    const int nScratch = inputs.size() > 1 ? 2 : 1;
    for (int i = 0; i < nScratch; ++i) {
        GpuBuffer* buffer = nullptr;
        if (GpuStatus s = ctx.scratch(bytes, buffer); !s) {
            return s;
        }
        scratch[i] = GpuWrapView { buffer, 0, wrap.output, p.width * n, n };
    }
    // The result's alpha, in the result's layout, beside each scratch result.
    GpuWrapView alphaScratch[2];
    if (carriesAlpha && !inputs.empty()) {
        for (int i = 0; i < nScratch; ++i) {
            GpuBuffer* buffer = nullptr;
            if (GpuStatus s = ctx.scratch(bytes, buffer); !s) {
                return s;
            }
            alphaScratch[i] = GpuWrapView { buffer, 0, wrap.output, p.width * n, n };
        }
    }

    if (inputs.empty()) {
        GpuWrap copy;
        copy.src = base;
        copy.dst = scratch[0];
        copy.output = wrap.output;
        copy.nComps = n;
        if (GpuStatus s = record(ctx, GpuKernelId::wrap_divide, copy, nullptr, nullptr); !s) {
            return s;
        }
        ctx.computeBarrier();
        return recordWrapFinish(ctx, wrap, scratch[0]);
    }

    GpuWrapStep step;
    step.output = wrap.output;
    step.nComps = n;
    step.channelMask = wrap.channelMask;
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        step.result = (k == 0) ? base : scratch[(k - 1) & 1];
        step.input = inputs[k];
        step.dst = scratch[k & 1];
        if (carriesAlpha) {
            step.alpha = true;
            step.resultAlpha = (k == 0) ? alphas[0] : GpuWrapAlpha { alphaScratch[(k - 1) & 1], -1 };
            step.inputAlpha = alphas[k + 1];
            step.dstAlpha = alphaScratch[k & 1];
        }
        if (GpuStatus s = recordWrapStep(ctx, kernel, step, opParams); !s) {
            return s;
        }
        ctx.computeBarrier();
    }
    const std::size_t last = (inputs.size() - 1) & 1;
    return recordWrapFinish(ctx, wrap, resultIsAlpha ? alphaScratch[last] : scratch[last]);
}

GpuStatus
recordWrapDivide(GpuRecordContext& ctx, const GpuWrap& wrap)
{
    return record(ctx, GpuKernelId::wrap_divide, wrap, nullptr, nullptr);
}

GpuStatus
recordWrapFinish(GpuRecordContext& ctx, const GpuWrap& wrap, const GpuWrapView& filtered)
{
    if (!filtered.buffer) {
        return invalid("filtered result has no buffer");
    }
    return record(ctx, GpuKernelId::wrap_finish, wrap, filtered.buffer, &filtered);
}

} // namespace gpu
