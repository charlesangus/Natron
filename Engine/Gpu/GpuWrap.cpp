#include "GpuWrap.h"

#include "GpuBuffer.h"
#include "GpuRecord.h"

#include <span>
#include <string>

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
wrapSourceBlock(const GpuWrap& wrap, const GpuWrapSources& sources, GpuWrapSourceBlock& out)
{
    out = {};
    if (sources.count > kGpuWrapMaxSources) {
        return invalid("at most " + std::to_string(kGpuWrapMaxSources) + " sources are supported, got "
                       + std::to_string(sources.count));
    }
    if (wrap.divisorChannel >= 0) {
        return invalid("an op reading several sources divides none of them");
    }
    out.count = sources.count;
    for (uint32_t i = 0; i < sources.count; ++i) {
        const GpuWrapSource& source = sources.list[i];
        GpuWrapSourceView& v = out.views[i];
        v.channel = source.channel < 0 ? -1 : source.channel;
        if (source.view.bounds.empty()) {
            continue;
        }
        const std::string name = "source " + std::to_string(i);
        if (GpuStatus s = checkGeometry(source.view, name.c_str()); !s) {
            return s;
        }
        if (source.channel >= int32_t(source.view.nComps)) {
            return invalid(name + " channel out of range");
        }
        v.offset = static_cast<uint32_t>(source.view.offsetFloats);
        v.stride = source.view.strideFloats;
        v.x1 = source.view.bounds.x1;
        v.y1 = source.view.bounds.y1;
        v.x2 = source.view.bounds.x2;
        v.y2 = source.view.bounds.y2;
        v.comps = source.view.nComps;
    }
    return {};
}

GpuStatus
recordWrapped(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuBuffer* opParams)
{
    return record(ctx, kernel, wrap, opParams, nullptr);
}

GpuStatus
recordWrappedSources(GpuRecordContext& ctx, GpuKernelId kernel, const GpuWrap& wrap, const GpuWrapSources& sources,
                     const GpuBuffer* opParams)
{
    GpuWrapParams p;
    if (GpuStatus s = wrapParams(wrap, p); !s) {
        return s;
    }
    GpuWrapSourceBlock block;
    if (GpuStatus s = wrapSourceBlock(wrap, sources, block); !s) {
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
    const GpuBuffer* src[kGpuWrapMaxSources] = {};
    for (uint32_t i = 0; i < sources.count; ++i) {
        const GpuWrapView& view = sources.list[i].view;
        if (view.bounds.empty()) {
            continue;
        }
        const std::string name = "source " + std::to_string(i);
        if (GpuStatus s = checkBuffer(view, name.c_str()); !s) {
            return s;
        }
        src[i] = view.buffer;
    }
    for (const GpuBuffer*& buffer : src) {
        if (!buffer) {
            if (GpuStatus s = standIn(ctx, unused); !s) {
                return s;
            }
            buffer = unused;
        }
    }
    GpuBuffer* views = nullptr;
    if (GpuStatus s = ctx.scratch(sizeof(block), views); !s) {
        return s;
    }
    if (GpuStatus s = ctx.updateBuffer(*views, 0, std::as_bytes(std::span(&block, 1))); !s) {
        return s;
    }
    return ctx.dispatch(kernel, { b[0], b[1], b[2], b[3], src[0], src[1], src[2], src[3], views }, p,
                        { p.width, p.height, 1 });
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
