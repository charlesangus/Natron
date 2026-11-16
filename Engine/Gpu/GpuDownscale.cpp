#include "GpuDownscale.h"

#include "GpuBuffer.h"
#include "GpuRecord.h"

#include <algorithm>
#include <string>
#include <vector>

namespace gpu {

namespace {

    GpuStatus invalid(const std::string& what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, "mip downscale: " + what };
    }

    bool contains(const GpuDownscaleRect& outer, const GpuDownscaleRect& inner)
    {
        return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 && inner.y2 <= outer.y2;
    }

    // The kernel indexes with 32 bits.
    GpuStatus checkView(const GpuDownscaleView& v, const char* name)
    {
        if (!v.buffer || !*v.buffer) {
            return invalid(std::string(name) + " has no buffer");
        }
        if (v.bounds.empty()) {
            return {};
        }
        const uint64_t rowFloats = uint64_t(v.bounds.x2 - v.bounds.x1) * v.nComps;
        if (v.strideFloats < rowFloats) {
            return invalid(std::string(name) + " stride is shorter than a row");
        }
        const uint64_t rows = uint64_t(v.bounds.y2 - v.bounds.y1);
        const uint64_t endFloats = v.offsetFloats + (rows - 1) * v.strideFloats + rowFloats;
        if (endFloats > UINT32_MAX) {
            return invalid(std::string(name) + " exceeds 32-bit indexing");
        }
        if (endFloats * sizeof(float) > v.buffer->size()) {
            return invalid(std::string(name) + " overruns its buffer");
        }
        return {};
    }

} // namespace

namespace {

    // A pass's block is 2^levels pixels a side, which the kernel computes and multiplies with 32-bit ints.
    constexpr unsigned int kMaxPassLevels = 16;

    // The bounds at `levels` coarser of an image of `bounds`: floor and ceiling of the scaled edges.
    GpuDownscaleRect coarser(const GpuDownscaleRect& bounds, unsigned int levels)
    {
        return { bounds.x1 >> levels, bounds.y1 >> levels, -((-bounds.x2) >> levels), -((-bounds.y2) >> levels) };
    }

    // The pixels `levels` finer that rect's blocks cover, inside bounds. Computed in 64 bits, since a rect at the edge
    // of the 32-bit range scales past it before the intersection brings it back.
    GpuDownscaleRect finerWithin(const GpuDownscaleRect& rect, unsigned int levels, const GpuDownscaleRect& bounds)
    {
        const int64_t scale = int64_t(1) << levels;
        GpuDownscaleRect out;
        out.x1 = (int32_t)std::max<int64_t>(int64_t(rect.x1) * scale, bounds.x1);
        out.y1 = (int32_t)std::max<int64_t>(int64_t(rect.y1) * scale, bounds.y1);
        out.x2 = (int32_t)std::min<int64_t>(int64_t(rect.x2) * scale, bounds.x2);
        out.y2 = (int32_t)std::min<int64_t>(int64_t(rect.y2) * scale, bounds.y2);
        return out;
    }

    GpuStatus dispatchPass(GpuRecordContext& ctx, const GpuDownscaleView& source, const GpuDownscaleView& dest,
                           const GpuDownscaleRect& rect, unsigned int levels)
    {
        GpuMipDownscalePush push = {};
        push.levels = levels;
        push.nComps = source.nComps;
        push.srcOffset = (uint32_t)source.offsetFloats;
        push.srcStride = source.strideFloats;
        push.srcX1 = source.bounds.x1;
        push.srcY1 = source.bounds.y1;
        push.srcX2 = source.bounds.x2;
        push.srcY2 = source.bounds.y2;
        push.dstOffset = (uint32_t)(dest.offsetFloats + uint64_t(rect.y1 - dest.bounds.y1) * dest.strideFloats + uint64_t(rect.x1 - dest.bounds.x1) * dest.nComps);
        push.dstStride = dest.strideFloats;
        push.outX = rect.x1;
        push.outY = rect.y1;
        push.outWidth = uint32_t(rect.x2 - rect.x1);
        push.outHeight = uint32_t(rect.y2 - rect.y1);
        return ctx.dispatch(GpuKernelId::mip_downscale, { source.buffer, dest.buffer }, push,
                            { push.outWidth, push.outHeight, 1 });
    }

} // namespace

GpuStatus
recordMipDownscale(GpuRecordContext& ctx, const GpuDownscaleView& source, const GpuDownscaleView& dest,
                   const GpuDownscaleRect& rect, unsigned int levels)
{
    if (rect.empty()) {
        return {};
    }
    if (levels == 0) {
        return invalid("levels must be at least 1");
    }
    if (source.nComps == 0 || source.nComps > 4 || source.nComps != dest.nComps) {
        return invalid("source and destination must have the same 1 to 4 channels");
    }
    if (!contains(dest.bounds, rect)) {
        return invalid("the rect is outside the destination");
    }
    if (GpuStatus status = checkView(source, "source"); !status) {
        return status;
    }
    if (GpuStatus status = checkView(dest, "destination"); !status) {
        return status;
    }

    // Repeated halving composes, so passes of at most kMaxPassLevels each make what one pass would. Each intermediate
    // holds only the blocks of rect at its level: those cuts fall on block edges of every later pass, so no later
    // partial edge block is made up by them. The first pass takes the most levels, which keeps the intermediates small.
    std::vector<unsigned int> passes;
    for (unsigned int left = levels; left > 0;) {
        const unsigned int step = std::min(left, kMaxPassLevels);
        passes.push_back(step);
        left -= step;
    }
    // needed[i] is what pass i writes, at the level reached after it: rect for the last pass.
    std::vector<GpuDownscaleRect> needed(passes.size());
    std::vector<GpuDownscaleRect> reached(passes.size());
    {
        GpuDownscaleRect bounds = source.bounds;
        for (std::size_t i = 0; i < passes.size(); ++i) {
            bounds = source.bounds.empty() ? GpuDownscaleRect() : coarser(bounds, passes[i]);
            reached[i] = bounds;
        }
    }
    needed.back() = rect;
    for (std::size_t i = passes.size() - 1; i-- > 0;) {
        needed[i] = needed[i + 1].empty() ? GpuDownscaleRect() : finerWithin(needed[i + 1], passes[i + 1], reached[i]);
    }

    GpuDownscaleView from = source;
    for (std::size_t i = 0; i + 1 < passes.size(); ++i) {
        if (needed[i].empty()) {
            // None of rect's blocks meets the source, so the last pass writes zeros from no pixels.
            from.bounds = GpuDownscaleRect();
            break;
        }
        GpuDownscaleView to;
        to.bounds = needed[i];
        to.nComps = source.nComps;
        const uint64_t strideFloats = uint64_t(to.bounds.x2 - to.bounds.x1) * to.nComps;
        if (strideFloats > UINT32_MAX) {
            return invalid("an intermediate level exceeds 32-bit indexing");
        }
        to.strideFloats = (uint32_t)strideFloats;
        GpuBuffer* buffer = nullptr;
        if (GpuStatus status = ctx.scratch(strideFloats * uint64_t(to.bounds.y2 - to.bounds.y1) * sizeof(float), buffer); !status) {
            return status;
        }
        to.buffer = buffer;
        if (GpuStatus status = checkView(to, "intermediate level"); !status) {
            return status;
        }
        if (GpuStatus status = dispatchPass(ctx, from, to, to.bounds, passes[i]); !status) {
            return status;
        }
        ctx.computeBarrier();
        from = to;
    }
    return dispatchPass(ctx, from, dest, rect, passes.back());
}

} // namespace gpu
