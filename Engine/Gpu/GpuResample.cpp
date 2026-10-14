#include "GpuResample.h"

#include "GpuRecord.h"

#include <climits>
#include <cmath>
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
    if (!r.jacobians[0].zero) {
        return invalid("the Jacobian with both axes outside must be zero");
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

    for (int k = 0; k < 6; ++k) {
        const double v = r.matrix[k];
        if (!std::isfinite(v)) {
            return invalid("the matrix is not finite");
        }
        p.hHi[k] = static_cast<float>(v);
        p.hLo[k] = static_cast<float>(v - static_cast<double>(p.hHi[k]));
    }
    for (uint32_t s = 0; s < 4; ++s) {
        const GpuResampleJacobian& j = r.jacobians[s];
        if (j.levelX > kGpuResampleMaxLevel || j.levelY > kGpuResampleMaxLevel) {
            return invalid("supersampling level out of range");
        }
        const uint32_t bits = j.levelX | (j.levelY << 3) | (j.zero ? 0x40u : 0u);
        p.states |= bits << (8 * s);
    }

    // An invalid source is never read, but the binding still needs a buffer.
    const GpuBuffer* src = srcValid ? r.src.buffer : r.dst.buffer;
    return ctx.dispatch(GpuKernelId::resample, { src, r.dst.buffer }, p, { p.width, p.height, 1 });
}

} // namespace gpu
