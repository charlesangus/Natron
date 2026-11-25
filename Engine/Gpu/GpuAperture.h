#pragma once

#include "GpuKernel.h"

#include <cstdint>
#include <span>

namespace gpu {

class GpuRecordContext;

// A single-channel float mask in a buffer, the first pixel at offsetFloats, rows strideFloats apart.
struct GpuApertureView {
    const GpuBuffer* buffer = nullptr;
    uint64_t offsetFloats = 0;
    uint32_t strideFloats = 0;
};

// Mirrors AperturePush in Kernels/aperture.slang.
struct GpuAperturePush {
    uint32_t dstOffset;
    uint32_t dstStride;
};
static_assert(sizeof(GpuAperturePush) == 8);
static_assert(sizeof(GpuAperturePush) <= GpuKernel::kMaxPushConstantBytes);

// Records the aperture mask that tables (from Optics::buildApertureTables()) describe into dest: what
// Optics::rasteriseApertureRows() makes on the CPU. image holds the image layer's width x height floats
// when the tables have one, and may be null when they do not. The tables are uploaded through a scratch
// buffer of ctx's pool. No barrier is recorded after the dispatch.
GpuStatus recordAperture(GpuRecordContext& ctx, std::span<const uint32_t> tables, const GpuBuffer* image,
                         const GpuApertureView& dest);

} // namespace gpu
