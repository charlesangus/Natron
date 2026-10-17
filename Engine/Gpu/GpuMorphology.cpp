#include "GpuMorphology.h"

#include "GpuRecord.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace gpu {

namespace {

    // Must match kGroup, kColumns, kChunk and kDirectMaxRadius in Kernels/morphology.slang.
    constexpr uint32_t kGroup = 128;
    constexpr uint32_t kColumns = 32;
    constexpr uint32_t kChunk = 16;
    constexpr uint32_t kDirectMaxRadius = 16;

    uint32_t divUp(uint32_t v, uint32_t d)
    {
        return static_cast<uint32_t>((uint64_t(v) + d - 1) / d);
    }

    bool contains(const GpuBlurRect& outer, const GpuBlurRect& inner)
    {
        return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 && inner.y2 <= outer.y2;
    }

    GpuBlurRect intersect(const GpuBlurRect& a, const GpuBlurRect& b)
    {
        GpuBlurRect r { std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2) };
        if (r.empty()) {
            return GpuBlurRect();
        }
        return r;
    }

    GpuStatus invalid(const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string("recordMorphology: ") + what };
    }

    // Float index of pixel (x, y) of image, or false when it does not fit the kernel's 32-bit indices.
    bool floatIndex(const GpuBlurImage& image, int32_t x, int32_t y, uint32_t channels, uint32_t& out)
    {
        const uint64_t index = image.offsetFloats + uint64_t(y - image.bounds.y1) * image.strideFloats
            + uint64_t(x - image.bounds.x1) * channels;
        const uint64_t rows = uint64_t(image.bounds.y2 - image.bounds.y1);
        if (index > UINT32_MAX || image.offsetFloats + rows * image.strideFloats > UINT32_MAX) {
            return false;
        }
        out = static_cast<uint32_t>(index);
        return true;
    }

    // A params block whose source fields address all of `src`.
    bool baseParams(const GpuBlurImage& src, uint32_t channels, GpuMorphParams& p)
    {
        p = GpuMorphParams {};
        p.channels = channels;
        if (!src.bounds.empty()) {
            p.srcX1 = src.bounds.x1;
            p.srcY1 = src.bounds.y1;
            p.srcX2 = src.bounds.x2;
            p.srcY2 = src.bounds.y2;
            p.srcStride = src.strideFloats;
            if (!floatIndex(src, src.bounds.x1, src.bounds.y1, channels, p.srcOffset)) {
                return false;
            }
        }
        return true;
    }

    GpuStatus dispatchPass(GpuRecordContext& ctx, const GpuMorphology& m, const GpuMorphPass& pass,
                           const GpuBlurImage& src, const GpuBlurImage& dst, const GpuBlurRect& out,
                           const GpuBuffer* window)
    {
        GpuMorphParams p;
        if (!baseParams(src, m.channels, p)) {
            return invalid("source exceeds 32-bit indexing");
        }
        p.width = uint32_t(out.x2 - out.x1);
        p.height = uint32_t(out.y2 - out.y1);
        p.half = pass.half;
        p.vertical = pass.vertical ? 1u : 0u;
        p.takeMax = pass.takeMax ? 1u : 0u;
        p.outX = out.x1;
        p.outY = out.y1;
        p.dstStride = dst.strideFloats;
        if (!floatIndex(dst, out.x1, out.y1, m.channels, p.dstOffset)) {
            return invalid("destination exceeds 32-bit indexing");
        }
        const uint32_t len = pass.vertical ? p.height : p.width;
        const uint32_t lines = pass.vertical ? p.width : p.height;
        const bool direct = pass.half <= kDirectMaxRadius;
        const GpuBuffer* srcBuffer = src.buffer ? src.buffer : window;
        if (!direct) {
            const std::array<uint32_t, 3> threads = pass.vertical ? std::array<uint32_t, 3> { divUp(lines, kGroup) * kGroup, divUp(len, kChunk), 1 }
                                                                  : std::array<uint32_t, 3> { divUp(divUp(len, kChunk), kGroup) * kGroup, lines, 1 };
            return ctx.dispatch(GpuKernelId::morph_wide, { srcBuffer, dst.buffer, window }, p, threads);
        }
        return ctx.dispatch(GpuKernelId::morph_direct, { srcBuffer, dst.buffer, window }, p,
                            { divUp(len, kGroup) * kGroup, pass.vertical ? divUp(lines, kColumns) : lines, 1 });
    }

    GpuStatus writeWindow(GpuRecordContext& ctx, const GpuBuffer& window, const std::array<int32_t, 4>& values)
    {
        return ctx.updateBuffer(window, 0, std::as_bytes(std::span(values)));
    }

} // namespace

GpuStatus
recordFilterWindow(GpuRecordContext& ctx, const GpuBlurRect& output, const GpuBlurRect& rod, int32_t haloX, int32_t haloY,
                   GpuBuffer*& window)
{
    window = nullptr;
    if (haloX < 0 || haloY < 0) {
        return invalid("negative halo");
    }
    if (GpuStatus s = ctx.scratch(4 * sizeof(int32_t), window); !s) {
        return s;
    }
    const GpuBlurRect grown { output.x1 - haloX, output.y1 - haloY, output.x2 + haloX, output.y2 + haloY };
    const GpuBlurRect domain = output.empty() ? GpuBlurRect() : intersect(grown, rod);
    if (GpuStatus s = writeWindow(ctx, *window, { domain.x1, domain.y1, domain.x2, domain.y2 }); !s) {
        return s;
    }
    ctx.computeBarrier();
    return {};
}

GpuStatus
recordMorphology(GpuRecordContext& ctx, const GpuMorphology& m)
{
    const uint32_t nc = m.channels;
    if (nc == 0 || nc > kGpuMorphMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "recordMorphology: unsupported channel count" };
    }
    if (m.passes.empty() || m.passes.size() > 2 || (m.passes.size() == 2 && m.passes[0].vertical == m.passes[1].vertical)) {
        return invalid("one or two passes on different axes are required");
    }
    for (const GpuMorphPass& pass : m.passes) {
        if (pass.half == 0 || pass.half > kGpuMorphMaxRadius) {
            return { VK_ERROR_FEATURE_NOT_PRESENT, "recordMorphology: half width " + std::to_string(pass.half) + " outside 1 to " + std::to_string(kGpuMorphMaxRadius) };
        }
    }
    if (m.haloX < 0 || m.haloY < 0) {
        return invalid("negative halo");
    }
    if (!m.dst.buffer || m.output.empty() || !contains(m.dst.bounds, m.output)) {
        return invalid("invalid destination window");
    }
    const GpuBlurRect grown { m.output.x1 - m.haloX, m.output.y1 - m.haloY, m.output.x2 + m.haloX, m.output.y2 + m.haloY };
    const GpuBlurRect domain = intersect(grown, m.rod);
    if (!m.src.buffer && !m.src.bounds.empty()) {
        return invalid("a source without a buffer must be empty");
    }

    GpuBuffer* window = nullptr;
    if (GpuStatus s = recordFilterWindow(ctx, m.output, m.rod, m.haloX, m.haloY, window); !s) {
        return s;
    }

    if (m.passes.size() == 1) {
        return dispatchPass(ctx, m, m.passes[0], m.src, m.dst, m.output, window);
    }

    // The first pass covers the output's extent along the second pass's lines and every position
    // along its axis that the second pass reads.
    GpuBlurRect mid = m.passes[0].vertical ? GpuBlurRect { domain.x1, m.output.y1, domain.x2, m.output.y2 }
                                           : GpuBlurRect { m.output.x1, domain.y1, m.output.x2, domain.y2 };
    if (domain.empty()) {
        mid = GpuBlurRect();
    }
    const uint64_t midFloats = uint64_t(mid.x2 - mid.x1) * uint64_t(mid.y2 - mid.y1) * nc;
    if (midFloats > UINT32_MAX) {
        return invalid("intermediate lines exceed 32-bit indexing");
    }
    GpuBuffer* midBuffer = nullptr;
    if (GpuStatus s = ctx.scratch(std::max<uint64_t>(midFloats, 1) * sizeof(float), midBuffer); !s) {
        return s;
    }
    GpuBlurImage midImage;
    midImage.buffer = midBuffer;
    midImage.bounds = mid;
    midImage.strideFloats = uint32_t(mid.x2 - mid.x1) * nc;
    if (!mid.empty()) {
        if (GpuStatus s = dispatchPass(ctx, m, m.passes[0], m.src, midImage, mid, window); !s) {
            return s;
        }
        ctx.computeBarrier();
    }
    return dispatchPass(ctx, m, m.passes[1], midImage, m.dst, m.output, window);
}

} // namespace gpu
