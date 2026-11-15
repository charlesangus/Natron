#include "GpuDownscale.h"

#include "GpuBuffer.h"
#include "GpuRecord.h"

#include <string>

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

GpuStatus
recordMipDownscale(GpuRecordContext& ctx, const GpuDownscaleView& source, const GpuDownscaleView& dest,
                   const GpuDownscaleRect& rect, unsigned int levels)
{
    if (rect.empty()) {
        return {};
    }
    if (levels == 0 || levels > 16) {
        return invalid("levels must be 1 to 16");
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

} // namespace gpu
