#include "GpuAperture.h"

#include "GpuBuffer.h"
#include "GpuRecord.h"

#include "Engine/Optics/Aperture.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace gpu {

namespace {

    namespace optics = NATRON_NAMESPACE::Optics;

    // Kernels/aperture.slang spells out the header's indices.
    static_assert(optics::kApertureNdcScaleX == 2 && optics::kApertureBlades == 6);
    static_assert(optics::kApertureGratingStrength == 12 && optics::kApertureGratingBound == 18);
    static_assert(optics::kApertureScratchesStrength == 19 && optics::kApertureScratchesSin == 27);
    static_assert(optics::kApertureDustStrength == 28 && optics::kApertureDustBound == 34);
    static_assert(optics::kApertureImageStrength == 35 && optics::kApertureImageWhite == 43);
    static_assert(optics::kApertureBladeOffset == 44 && optics::kApertureDustOffset == 47);
    static_assert(optics::kApertureHeaderWords == 48);
    static_assert(optics::kApertureBladeWords == 2 && optics::kApertureGratingWords == 4);
    static_assert(optics::kApertureScratchWords == 6 && optics::kApertureDustWords == 2);
    static_assert(optics::kApertureSamples == 8);
    // The kernel's per-pixel survivor masks hold one bit per entry, sized for these maxima.
    static_assert(optics::kApertureMaxGratingBars <= 12 * 32);
    static_assert(optics::kApertureMaxScratches <= 32 * 32 && optics::kApertureMaxDust <= 32 * 32);

    constexpr std::size_t kUpdateChunkBytes = 65536;

    GpuStatus invalid(const std::string& what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, "aperture: " + what };
    }

} // namespace

GpuStatus
recordAperture(GpuRecordContext& ctx, std::span<const uint32_t> tables, const GpuBuffer* image,
               const GpuApertureView& dest)
{
    if (tables.size() < optics::kApertureHeaderWords) {
        return {};
    }
    if (!optics::apertureTablesValid(tables.data(), tables.size())) {
        return invalid("the tables' sections overrun them or exceed their maxima");
    }
    const uint32_t width = tables[optics::kApertureWidth];
    const uint32_t height = tables[optics::kApertureHeight];
    if (width == 0 || height == 0) {
        return {};
    }
    if (!dest.buffer || !*dest.buffer) {
        return invalid("the destination has no buffer");
    }
    if (dest.strideFloats < width) {
        return invalid("the destination stride is shorter than a row");
    }
    const uint64_t endFloats = dest.offsetFloats + uint64_t(height - 1) * dest.strideFloats + width;
    if (endFloats > UINT32_MAX) {
        return invalid("the destination exceeds 32-bit indexing");
    }
    if (endFloats * sizeof(float) > dest.buffer->size()) {
        return invalid("the destination overruns its buffer");
    }
    const uint64_t imageFloats = uint64_t(tables[optics::kApertureImageWidth]) * tables[optics::kApertureImageHeight];
    if (imageFloats > 0) {
        if (!image || !*image) {
            return invalid("the tables have an image layer but no image was given");
        }
        if (imageFloats > UINT32_MAX || imageFloats * sizeof(float) > image->size()) {
            return invalid("the image is smaller than the tables say");
        }
    }

    const std::span<const std::byte> bytes = std::as_bytes(tables);
    GpuBuffer* tableBuffer = nullptr;
    if (GpuStatus s = ctx.scratch(bytes.size(), tableBuffer); !s) {
        return s;
    }
    for (std::size_t offset = 0; offset < bytes.size(); offset += kUpdateChunkBytes) {
        const std::size_t n = std::min(kUpdateChunkBytes, bytes.size() - offset);
        if (GpuStatus s = ctx.updateBuffer(*tableBuffer, offset, bytes.subspan(offset, n)); !s) {
            return s;
        }
    }
    // The kernel reads no image without an image layer, but every binding needs a buffer.
    const GpuBuffer* imageBuffer = imageFloats > 0 ? image : nullptr;
    if (!imageBuffer) {
        GpuBuffer* dummy = nullptr;
        if (GpuStatus s = ctx.scratch(sizeof(float), dummy); !s) {
            return s;
        }
        imageBuffer = dummy;
    }

    GpuAperturePush push = {};
    push.dstOffset = (uint32_t)dest.offsetFloats;
    push.dstStride = dest.strideFloats;
    return ctx.dispatch(GpuKernelId::aperture, { tableBuffer, imageBuffer, dest.buffer }, push, { width, height, 1 });
}

} // namespace gpu
