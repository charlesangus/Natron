#pragma once

#include "GpuDevice.h"

#include <cstdint>
#include <vector>

namespace gpu {

class GpuBuffer;
class GpuRecordContext;

// The longest line Kernels/fft.slang transforms, its shared line holding that many complex samples.
inline constexpr uint32_t kGpuFftMaxSize = 4096;
inline constexpr uint32_t kGpuFftGroupSize = 256;
inline constexpr uint32_t kGpuFftSharedBytes = kGpuFftMaxSize * 2 * sizeof(float);

// Mirrors FftParams in Kernels/fft.slang, which has no padding between members.
struct GpuFftParams {
    uint32_t n;
    uint32_t log2n;
    uint32_t lines;
    uint32_t inverse;
    uint32_t realInput;
    uint32_t tableSize;
    float scale;
};
static_assert(sizeof(GpuFftParams) == 28);

// A 2-D transform of width x height samples stored row by row: src holds interleaved complex floats, or real ones
// with realInput; dst holds interleaved complex floats and may be src. The forward transform is unscaled and the
// inverse scaled by 1 / (width * height).
struct GpuFft2D {
    const GpuBuffer* src = nullptr;
    const GpuBuffer* dst = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool inverse = false;
    bool realInput = false;
};

// Powers of two from 1 to kGpuFftMaxSize along each axis.
bool gpuFftSupportsSize(uint32_t width, uint32_t height);

// exp(-2 pi i j / size) for j in [0, size), interleaved, as fft reads it from its twiddle binding.
std::vector<float> gpuFftTwiddles(uint32_t size);

// Records the row pass, which writes its lines transposed into a scratch buffer held by ctx, a compute barrier, and the
// column pass, which transposes them back into dst. Nothing orders the passes against the caller's own dispatches.
// Refused for a size gpuFftSupportsSize() rejects, buffers too small for the transform, or a device whose compute
// shared memory is below kGpuFftSharedBytes.
GpuStatus recordFft2D(GpuRecordContext& ctx, const GpuFft2D& fft);

} // namespace gpu
