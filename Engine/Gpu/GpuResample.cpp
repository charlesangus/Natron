#include "GpuResample.h"

#include "GpuRecord.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace gpu {

namespace {

    constexpr uint32_t kFlagClamp = 1;
    constexpr uint32_t kFlagBlackOutside = 2;
    constexpr uint32_t kFlagSrcValid = 4;

    GpuStatus invalid(const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string("recordResample: ") + what };
    }

    bool contains(const GpuBlurRect& outer, const GpuBlurRect& inner)
    {
        return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 && inner.y2 <= outer.y2;
    }

    // Float index of pixel (x, y) of image, or false when the image does not fit 32-bit indices.
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

    // Mirrors kBlurHeaderWords and kBlurMatrixWords in Kernels/resample.slang.
    constexpr uint32_t kBlurHeaderWords = 8;
    constexpr uint32_t kBlurMatrixWords = 16;

    // updateBuffer's limit per call.
    constexpr std::size_t kUpdateChunkBytes = 65536;

    void splitDouble(double v, float& hi, float& lo)
    {
        hi = static_cast<float>(v);
        lo = static_cast<float>(v - static_cast<double>(hi));
    }

    uint32_t floatBits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    void doubleBits(double v, uint32_t* words)
    {
        uint64_t u;
        std::memcpy(&u, &v, sizeof(u));
        words[0] = static_cast<uint32_t>(u);
        words[1] = static_cast<uint32_t>(u >> 32);
    }

    uint32_t packStates(const std::array<GpuResampleJacobian, 4>& jacobians)
    {
        uint32_t states = 0;
        for (uint32_t s = 0; s < 4; ++s) {
            const GpuResampleJacobian& j = jacobians[s];
            const uint32_t bits = j.levelX | (j.levelY << 3) | (j.zero ? 0x40u : 0u);
            states |= bits << (8 * s);
        }
        return states;
    }

    GpuStatus validateMatrix(const GpuResampleMatrix& m, bool weighted)
    {
        if (!m.jacobians[0].zero) {
            return invalid("the Jacobian with both axes outside must be zero");
        }
        for (double v : m.matrix) {
            if (!std::isfinite(v)) {
                return invalid("the matrix is not finite");
            }
        }
        for (const GpuResampleJacobian& j : m.jacobians) {
            if (j.levelX > kGpuResampleMaxLevel || j.levelY > kGpuResampleMaxLevel) {
                return invalid("supersampling level out of range");
            }
        }
        if (weighted && !std::isfinite(m.weight)) {
            return invalid("a matrix weight is not finite");
        }
        return {};
    }

    GpuStatus uploadBlurMatrices(GpuRecordContext& ctx, const GpuResample& r, GpuBuffer*& out)
    {
        const GpuResampleBlur& b = r.blur;
        std::vector<uint32_t> words(kBlurHeaderWords + r.matrices.size() * kBlurMatrixWords, 0u);
        words[0] = static_cast<uint32_t>(r.matrices.size());
        words[1] = b.minSamples;
        words[2] = b.maxIterations;
        words[3] = b.seedOffset;
        float hi, lo;
        splitDouble(b.maxError * b.maxError, hi, lo);
        words[4] = floatBits(hi);
        words[5] = floatBits(lo);
        words[6] = b.weighted ? 1u : 0u;
        for (std::size_t i = 0; i < r.matrices.size(); ++i) {
            const GpuResampleMatrix& m = r.matrices[i];
            uint32_t* w = &words[kBlurHeaderWords + i * kBlurMatrixWords];
            for (int k = 0; k < 6; ++k) {
                doubleBits(m.matrix[k], &w[2 * k]);
            }
            w[12] = packStates(m.jacobians);
            splitDouble(m.weight, hi, lo);
            w[13] = floatBits(hi);
            w[14] = floatBits(lo);
        }
        const std::span<const std::byte> bytes = std::as_bytes(std::span<const uint32_t>(words));
        if (GpuStatus s = ctx.scratch(bytes.size(), out); !s) {
            return s;
        }
        for (std::size_t offset = 0; offset < bytes.size(); offset += kUpdateChunkBytes) {
            const std::size_t n = std::min(kUpdateChunkBytes, bytes.size() - offset);
            if (GpuStatus s = ctx.updateBuffer(*out, offset, bytes.subspan(offset, n)); !s) {
                return s;
            }
        }
        return {};
    }

} // namespace

GpuStatus
recordResample(GpuRecordContext& ctx, const GpuResample& r)
{
    if (r.channels == 0 || r.channels > kGpuResampleMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "recordResample: unsupported channel count" };
    }
    if (r.filter >= kGpuResampleFilterCount) {
        return invalid("unknown filter");
    }
    if (!r.dst.buffer || r.output.empty() || !contains(r.dst.bounds, r.output)) {
        return invalid("invalid destination window");
    }
    if (r.matrices.empty() || r.matrices.size() > kGpuResampleMaxMatrices) {
        return invalid("the matrix count is out of range");
    }
    const bool blurred = r.matrices.size() > 1;
    for (const GpuResampleMatrix& m : r.matrices) {
        if (GpuStatus s = validateMatrix(m, blurred && r.blur.weighted); !s) {
            return s;
        }
    }
    if (blurred && (r.blur.minSamples == 0 || !std::isfinite(r.blur.maxError))) {
        return invalid("invalid motion blur sampling");
    }

    GpuResampleParams p {};
    p.outX = r.output.x1;
    p.outY = r.output.y1;
    p.width = uint32_t(r.output.x2 - r.output.x1);
    p.height = uint32_t(r.output.y2 - r.output.y1);
    p.dstStride = r.dst.strideFloats;
    if (!floatIndex(r.dst, r.output.x1, r.output.y1, r.channels, p.dstOffset)) {
        return invalid("destination exceeds 32-bit indexing");
    }
    p.nComps = r.channels;
    p.filter = r.filter;
    p.flags = (r.clamp ? kFlagClamp : 0u) | (r.blackOutside ? kFlagBlackOutside : 0u);

    const bool srcValid = r.src.buffer && !r.src.bounds.empty();
    if (srcValid) {
        p.srcX1 = r.src.bounds.x1;
        p.srcY1 = r.src.bounds.y1;
        p.srcX2 = r.src.bounds.x2;
        p.srcY2 = r.src.bounds.y2;
        p.srcStride = r.src.strideFloats;
        if (!floatIndex(r.src, r.src.bounds.x1, r.src.bounds.y1, r.channels, p.srcOffset)) {
            return invalid("source exceeds 32-bit indexing");
        }
        p.flags |= kFlagSrcValid;
    }

    // An invalid source is never read, but the binding still needs a buffer, and so does the
    // matrices' binding when they travel in the push constants.
    const GpuBuffer* src = srcValid ? r.src.buffer : r.dst.buffer;
    if (!blurred) {
        const GpuResampleMatrix& m = r.matrices[0];
        for (int k = 0; k < 6; ++k) {
            doubleBits(m.matrix[k], &p.hBits[2 * k]);
        }
        p.states = packStates(m.jacobians);
        return ctx.dispatch(GpuKernelId::resample, { src, r.dst.buffer, r.dst.buffer }, p, { p.width, p.height, 1 });
    }

    GpuBuffer* matrices = nullptr;
    if (GpuStatus s = uploadBlurMatrices(ctx, r, matrices); !s) {
        return s;
    }
    return ctx.dispatch(GpuKernelId::resample_blur, { src, r.dst.buffer, matrices }, p, { p.width, p.height, 1 });
}

GpuStatus
recordOffset(GpuRecordContext& ctx, const GpuOffset& o)
{
    if (o.channels == 0 || o.channels > kGpuResampleMaxChannels) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "recordOffset: unsupported channel count" };
    }
    if (!o.dst.buffer || o.output.empty() || !contains(o.dst.bounds, o.output)) {
        return { VK_ERROR_INITIALIZATION_FAILED, "recordOffset: invalid destination window" };
    }

    GpuOffsetParams p {};
    p.width = uint32_t(o.output.x2 - o.output.x1);
    p.height = uint32_t(o.output.y2 - o.output.y1);
    p.dstStride = o.dst.strideFloats;
    p.nComps = o.channels;
    if (!floatIndex(o.dst, o.output.x1, o.output.y1, o.channels, p.dstOffset)) {
        return { VK_ERROR_INITIALIZATION_FAILED, "recordOffset: destination exceeds 32-bit indexing" };
    }

    bool srcValid = o.src.buffer && !o.src.bounds.empty();
    if (srcValid) {
        const int64_t sx0 = int64_t(o.output.x1) - o.dx;
        const int64_t sy0 = int64_t(o.output.y1) - o.dy;
        const auto threads = [](int64_t v, uint32_t n) { return uint32_t(std::clamp<int64_t>(v, 0, int64_t(n))); };
        p.validX1 = threads(int64_t(o.src.bounds.x1) - sx0, p.width);
        p.validX2 = threads(int64_t(o.src.bounds.x2) - sx0, p.width);
        p.validY1 = threads(int64_t(o.src.bounds.y1) - sy0, p.height);
        p.validY2 = threads(int64_t(o.src.bounds.y2) - sy0, p.height);
        srcValid = p.validX1 < p.validX2 && p.validY1 < p.validY2;
        if (srcValid) {
            p.srcStride = o.src.strideFloats;
            if (!floatIndex(o.src, int32_t(sx0 + p.validX1), int32_t(sy0 + p.validY1), o.channels, p.srcOffset)) {
                return { VK_ERROR_INITIALIZATION_FAILED, "recordOffset: source exceeds 32-bit indexing" };
            }
        } else {
            p.validX1 = p.validX2 = p.validY1 = p.validY2 = 0;
        }
    }

    // An invalid source is never read, but the binding still needs a buffer.
    const GpuBuffer* src = srcValid ? o.src.buffer : o.dst.buffer;
    return ctx.dispatch(GpuKernelId::offset, { src, o.dst.buffer }, p, { p.width, p.height, 1 });
}

} // namespace gpu
