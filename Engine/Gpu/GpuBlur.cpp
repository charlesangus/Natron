#include "GpuBlur.h"

#include "GpuRecord.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace gpu {

namespace {

    // Must match kGroup, kNarrowColumns, kNarrowRows and kDirectMaxRadius in Kernels/blur.slang.
    constexpr uint32_t kGroup = kGpuBlurGroupSize;
    constexpr uint32_t kNarrowColumns = 8;
    constexpr uint32_t kNarrowRows = 64;
    constexpr uint32_t kDirectMaxRadius = 32;

    uint32_t divUp(uint32_t v, uint32_t d)
    {
        return static_cast<uint32_t>((uint64_t(v) + d - 1) / d);
    }

    bool contains(const GpuBlurRect& outer, const GpuBlurRect& inner)
    {
        return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 && inner.y2 <= outer.y2;
    }

    GpuStatus invalid(const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string("recordSeparableBlur: ") + what };
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

    GpuStatus dispatchPass(GpuRecordContext& ctx, const GpuBuffer* src, const GpuBuffer* dst, const GpuBuffer* weights,
                           const GpuBlurPass& pass)
    {
        if (GpuStatus s = validateBlurPass(pass); !s) {
            return s;
        }
        const GpuBlurParams p = blurParams(pass);
        const std::array<uint32_t, 3> groups = blurGroupCount(pass);
        return ctx.dispatch(GpuKernelId::blur, { src, dst, weights }, p, { groups[0] * kGroup, groups[1], groups[2] });
    }

    GpuStatus uploadWeights(GpuRecordContext& ctx, std::span<const float> w, GpuBuffer*& out)
    {
        if (w.empty() || w.size() > kGpuBlurMaxRadius + 1) {
            return { VK_ERROR_FEATURE_NOT_PRESENT, "recordSeparableBlur: blur radius exceeds the GPU blur limit of " + std::to_string(kGpuBlurMaxRadius) };
        }
        if (GpuStatus s = ctx.scratch(w.size_bytes(), out); !s) {
            return s;
        }
        return ctx.updateBuffer(*out, 0, std::as_bytes(w));
    }

    // Must match numthreads in Kernels/blur_lines.slang.
    constexpr uint32_t kLinesGroup = 64;

    GpuStatus invalidLines(const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string("recordLineBlur: ") + what };
    }

    void pushTwoFloat(std::vector<float>& out, double v)
    {
        const float hi = static_cast<float>(v);
        out.push_back(hi);
        out.push_back(static_cast<float>(v - static_cast<double>(hi)));
    }

    // The words blur_lines reads from coefBuf, derived from f with the expressions and operand types
    // of LineFilter::applyVanVliet and applyBox, and the box's half window.
    std::vector<float> lineCoefficients(const GpuLineFilter& f, int32_t& half)
    {
        std::vector<float> c;
        half = 0;
        switch (f.kind) {
        case GpuLineFilterKind::deriche:
            for (double v : f.deriche) {
                pushTwoFloat(c, v);
            }
            break;
        case GpuLineFilterKind::vanVliet: {
            const double sumsq = f.vanVliet[0], sum = sumsq * sumsq, a1 = f.vanVliet[1], a2 = f.vanVliet[2], a3 = f.vanVliet[3];
            pushTwoFloat(c, sumsq);
            pushTwoFloat(c, sum);
            pushTwoFloat(c, a1);
            pushTwoFloat(c, a2);
            pushTwoFloat(c, a3);
            pushTwoFloat(c, 1. - a1 - a2 - a3);
            for (double v : f.triggs) {
                pushTwoFloat(c, v);
            }
            break;
        }
        case GpuLineFilterKind::box: {
            const float boxsize = f.boxSize;
            const int w2 = (int)(boxsize - 1) / 2;
            const unsigned int winsize = 2 * w2 + 1U;
            const double frac = (boxsize - winsize) / 2.;
            pushTwoFloat(c, frac);
            pushTwoFloat(c, boxsize);
            half = w2;
            break;
        }
        case GpuLineFilterKind::identity:
            pushTwoFloat(c, 0.);
            break;
        }
        return c;
    }

    // A box of LineFilter::box() that smooths, as opposed to one that only takes the difference.
    bool boxSmooths(const GpuLineFilter& f)
    {
        return f.boxSize > 1.f && f.iterations > 0;
    }

    bool validLineFilter(const GpuLineFilter& f)
    {
        if (f.order > 1) {
            return false;
        }
        if (f.kind != GpuLineFilterKind::box) {
            return true;
        }
        if (!(f.boxSize > 0.f)) {
            return false;
        }
        return boxSmooths(f) ? f.iterations <= 3 : f.order == 1;
    }

    // One pass of blur_lines with its coefficients, ring and stored lines, the scratch ones from ctx.
    GpuStatus dispatchLines(GpuRecordContext& ctx, const GpuLineFilter& f, GpuLineBlurParams p, const GpuBuffer* src,
                            const GpuBuffer* dst, const GpuBuffer* lines, uint64_t storedFloats, const GpuBuffer* window)
    {
        int32_t half = 0;
        const std::vector<float> words = lineCoefficients(f, half);
        GpuBuffer* coefficients = nullptr;
        if (GpuStatus s = ctx.scratch(words.size() * sizeof(float), coefficients); !s) {
            return s;
        }
        if (GpuStatus s = ctx.updateBuffer(*coefficients, 0, std::as_bytes(std::span(words))); !s) {
            return s;
        }
        p.kind = static_cast<uint32_t>(f.kind);
        p.half = half;
        const bool box = f.kind == GpuLineFilterKind::box;
        p.iterations = box && boxSmooths(f) ? f.iterations : 0;
        p.order = f.order;
        p.neumann = f.neumann ? 1u : 0u;

        // A box keeps its line when a later pass or the difference reads what a pass wrote.
        const bool stores = f.kind == GpuLineFilterKind::vanVliet || (box && (p.iterations > 1 || (p.iterations > 0 && p.order > 0)));
        if (stores && !lines) {
            if (storedFloats > UINT32_MAX) {
                return invalidLines("stored lines exceed 32-bit indexing");
            }
            GpuBuffer* stored = nullptr;
            if (GpuStatus s = ctx.scratch(std::max<uint64_t>(storedFloats, 1) * sizeof(float), stored); !s) {
                return s;
            }
            lines = stored;
        }
        const GpuBuffer* ring = window;
        if (f.kind == GpuLineFilterKind::box) {
            const uint64_t pixels = uint64_t(2 * half + 1) * p.lineCount;
            if (pixels * 4 > UINT32_MAX) {
                return invalidLines("box window exceeds 32-bit indexing");
            }
            GpuBuffer* r = nullptr;
            if (GpuStatus s = ctx.scratch(pixels * 4 * sizeof(float), r); !s) {
                return s;
            }
            ring = r;
        }
        return ctx.dispatch(GpuKernelId::blur_lines, { src, dst, lines ? lines : dst, ring, coefficients, window }, p,
                            { p.lineCount, 1, 1 });
    }

} // namespace

GpuStatus
validateBlurPass(const GpuBlurPass& pass)
{
    if (pass.radius > kGpuBlurMaxRadius) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "blur radius " + std::to_string(pass.radius) + " exceeds the GPU blur limit of " + std::to_string(kGpuBlurMaxRadius) };
    }
    if (pass.channels == 0 || pass.channels > kGpuBlurMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "GPU blur supports 1 to " + std::to_string(kGpuBlurMaxChannels) + " channels, got " + std::to_string(pass.channels) };
    }
    return {};
}

GpuBlurParams
blurParams(const GpuBlurPass& pass)
{
    GpuBlurRegion r;
    if (pass.region) {
        r = *pass.region;
    } else {
        const int32_t w = static_cast<int32_t>(pass.width);
        const int32_t h = static_cast<int32_t>(pass.height);
        r.dstStride = pass.width * pass.channels;
        r.srcX2 = w;
        r.srcY2 = h;
        r.srcStride = r.dstStride;
        r.lineHi = pass.vertical ? h : w;
    }
    return { pass.width, pass.height, pass.channels, pass.radius, pass.vertical ? 1u : 0u,
             r.outX, r.outY, r.dstOffset, r.dstStride, r.srcX1, r.srcY1, r.srcX2, r.srcY2, r.srcOffset, r.srcStride,
             r.lineLo, r.lineHi };
}

std::array<uint32_t, 3>
blurGroupCount(const GpuBlurPass& pass)
{
    const uint32_t len = pass.vertical ? pass.height : pass.width;
    const uint32_t lines = pass.vertical ? pass.width : pass.height;
    if (pass.vertical && pass.radius <= kDirectMaxRadius) {
        return { divUp(len, kNarrowRows), divUp(lines, kNarrowColumns), 1 };
    }
    if (pass.vertical) {
        return { lines, divUp(len, kGroup), 1 };
    }
    return { divUp(len, kGroup), lines, 1 };
}

GpuStatus
recordBlurPass(GpuKernel& blur, VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkBuffer weights,
               const GpuBlurPass& pass)
{
    if (GpuStatus s = validateBlurPass(pass); !s) {
        return s;
    }
    const GpuKernelDesc& d = blur.desc();
    if (d.groupSize[0] != kGroup || d.groupSize[1] != 1 || d.groupSize[2] != 1 || d.storageBufferCount != 3
        || d.pushConstantBytes != sizeof(GpuBlurParams)) {
        return { VK_ERROR_INITIALIZATION_FAILED, "recordBlurPass: kernel is not blurPass" };
    }
    const GpuBlurParams p = blurParams(pass);
    const VkBuffer buffers[] = { src, dst, weights };
    return blur.recordGroups(cmd, buffers, std::as_bytes(std::span(&p, 1)), blurGroupCount(pass));
}

GpuStatus
recordSeparableBlur(GpuRecordContext& ctx, const GpuSeparableBlur& b)
{
    const uint32_t nc = b.channels;
    if (nc == 0 || nc > kGpuBlurMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "recordSeparableBlur: unsupported channel count" };
    }
    if (!b.src.buffer || !b.dst.buffer || b.output.empty() || !contains(b.dst.bounds, b.output)
        || (!b.srcValid.empty() && !contains(b.src.bounds, b.srcValid))) {
        return invalid("invalid source or destination window");
    }

    GpuBuffer* wx = nullptr;
    GpuBuffer* wy = nullptr;
    if (GpuStatus s = uploadWeights(ctx, b.weightsX, wx); !s) {
        return s;
    }
    if (GpuStatus s = uploadWeights(ctx, b.weightsY, wy); !s) {
        return s;
    }

    // The horizontal pass covers the output's columns over every row the vertical pass reads.
    GpuBlurRect mid { b.output.x1, b.domain.y1, b.output.x2, b.domain.y2 };
    if (mid.y2 < mid.y1) {
        mid.y2 = mid.y1;
    }
    const uint32_t midWidth = uint32_t(mid.x2 - mid.x1);
    const uint32_t midHeight = uint32_t(mid.y2 - mid.y1);
    const uint64_t midFloats = uint64_t(midWidth) * midHeight * nc;
    if (midFloats > UINT32_MAX) {
        return invalid("intermediate rows exceed 32-bit indexing");
    }
    GpuBuffer* scratch = nullptr;
    if (GpuStatus s = ctx.scratch(std::max<uint64_t>(midFloats, 1) * sizeof(float), scratch); !s) {
        return s;
    }

    if (midHeight > 0) {
        GpuBlurRegion h;
        h.outX = mid.x1;
        h.outY = mid.y1;
        h.dstStride = midWidth * nc;
        if (!b.srcValid.empty()) {
            h.srcX1 = b.srcValid.x1;
            h.srcY1 = b.srcValid.y1;
            h.srcX2 = b.srcValid.x2;
            h.srcY2 = b.srcValid.y2;
            h.srcStride = b.src.strideFloats;
            if (!floatIndex(b.src, b.srcValid.x1, b.srcValid.y1, nc, h.srcOffset)) {
                return invalid("source exceeds 32-bit indexing");
            }
        }
        h.lineLo = b.domain.x1;
        h.lineHi = b.domain.x2;
        const GpuBlurPass pass { midWidth, midHeight, nc, uint32_t(b.weightsX.size() - 1), false, h };
        if (GpuStatus s = dispatchPass(ctx, b.src.buffer, scratch, wx, pass); !s) {
            return s;
        }
        ctx.computeBarrier();
    }

    GpuBlurRegion v;
    v.outX = b.output.x1;
    v.outY = b.output.y1;
    v.dstStride = b.dst.strideFloats;
    if (!floatIndex(b.dst, b.output.x1, b.output.y1, nc, v.dstOffset)) {
        return invalid("destination exceeds 32-bit indexing");
    }
    v.srcX1 = mid.x1;
    v.srcY1 = mid.y1;
    v.srcX2 = mid.x2;
    v.srcY2 = mid.y2;
    v.srcStride = midWidth * nc;
    v.lineLo = b.domain.y1;
    v.lineHi = b.domain.y2;
    const GpuBlurPass pass { uint32_t(b.output.x2 - b.output.x1), uint32_t(b.output.y2 - b.output.y1), nc,
                             uint32_t(b.weightsY.size() - 1), true, v };
    return dispatchPass(ctx, scratch, b.dst.buffer, wy, pass);
}

GpuStatus
recordLineBlur(GpuRecordContext& ctx, const GpuLineBlur& b)
{
    const uint32_t nc = b.channels;
    if (nc == 0 || nc > kGpuBlurMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "recordLineBlur: unsupported channel count" };
    }
    if (!b.src.buffer || !b.dst.buffer || !b.window || b.output.empty() || !contains(b.dst.bounds, b.output)
        || (!b.srcValid.empty() && !contains(b.src.bounds, b.srcValid))) {
        return invalidLines("invalid source, destination or window");
    }
    if (!validLineFilter(b.filterX) || !validLineFilter(b.filterY)) {
        return invalidLines("unsupported line filter");
    }

    // The row pass covers the output's columns over every row of the domain, which holds every row
    // the column pass reads.
    GpuBlurRect mid { b.output.x1, b.domain.y1, b.output.x2, b.domain.y2 };
    if (b.domain.empty()) {
        mid.y2 = mid.y1;
    }
    const uint32_t midWidth = uint32_t(mid.x2 - mid.x1);
    const uint32_t midHeight = uint32_t(mid.y2 - mid.y1);
    const uint64_t midFloats = uint64_t(midWidth) * midHeight * nc;
    if (midFloats > UINT32_MAX) {
        return invalidLines("intermediate rows exceed 32-bit indexing");
    }
    GpuBuffer* midBuffer = nullptr;
    if (GpuStatus s = ctx.scratch(std::max<uint64_t>(midFloats, 4) * sizeof(float), midBuffer); !s) {
        return s;
    }

    if (midHeight > 0) {
        const uint32_t domainWidth = uint32_t(b.domain.x2 - b.domain.x1);
        GpuLineBlurParams p {};
        p.vertical = 0;
        p.channels = nc;
        p.lineCount = midHeight;
        p.lineOrigin = mid.y1;
        p.outLo = b.output.x1;
        p.outHi = b.output.x2;
        p.dstPosStep = nc;
        p.dstLineStep = midWidth * nc;
        // Position-major, so that the threads of a wave, on adjacent rows, touch adjacent pixels.
        p.storeLo = b.domain.x1;
        p.storePosStep = midHeight * nc;
        p.storeLineStep = nc;
        p.fromSource = 1;
        if (!b.srcValid.empty()) {
            p.srcX1 = b.srcValid.x1;
            p.srcY1 = b.srcValid.y1;
            p.srcX2 = b.srcValid.x2;
            p.srcY2 = b.srcValid.y2;
            p.srcStride = b.src.strideFloats;
            if (!floatIndex(b.src, b.srcValid.x1, b.srcValid.y1, nc, p.srcOffset)) {
                return invalidLines("source exceeds 32-bit indexing");
            }
        }
        const uint64_t storedFloats = uint64_t(domainWidth) * midHeight * nc;
        if (GpuStatus s = dispatchLines(ctx, b.filterX, p, b.src.buffer, midBuffer, nullptr, storedFloats, b.window); !s) {
            return s;
        }
        ctx.computeBarrier();
    }

    GpuLineBlurParams p {};
    p.vertical = 1;
    p.channels = nc;
    p.lineCount = uint32_t(b.output.x2 - b.output.x1);
    p.lineOrigin = b.output.x1;
    p.outLo = b.output.y1;
    p.outHi = b.output.y2;
    if (!floatIndex(b.dst, b.output.x1, b.output.y1, nc, p.dstOffset)) {
        return invalidLines("destination exceeds 32-bit indexing");
    }
    p.dstPosStep = b.dst.strideFloats;
    p.dstLineStep = nc;
    p.storeLo = mid.y1;
    p.storePosStep = midWidth * nc;
    p.storeLineStep = nc;
    p.fromSource = 0;
    return dispatchLines(ctx, b.filterY, p, midBuffer, b.dst.buffer, midBuffer, midFloats, b.window);
}

} // namespace gpu
