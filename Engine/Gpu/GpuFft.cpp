#include "GpuFft.h"

#include "GpuRecord.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <span>
#include <string>

namespace gpu {

namespace {

    GpuStatus refused(const std::string& what)
    {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "recordFft2D: " + what };
    }

    GpuStatus dispatchLines(GpuRecordContext& ctx, const GpuBuffer* src, const GpuBuffer* dst, const GpuBuffer* twiddles,
                            const GpuFftParams& p)
    {
        return ctx.dispatch(GpuKernelId::fft, { src, dst, twiddles }, p, { p.lines * kGpuFftGroupSize, 1, 1 });
    }

} // namespace

bool
gpuFftSupportsSize(uint32_t width, uint32_t height)
{
    return width >= 1 && height >= 1 && width <= kGpuFftMaxSize && height <= kGpuFftMaxSize && std::has_single_bit(width)
        && std::has_single_bit(height);
}

std::vector<float>
gpuFftTwiddles(uint32_t size)
{
    std::vector<float> out(size_t(size) * 2);
    for (uint32_t j = 0; j < size; ++j) {
        const double angle = -2. * std::numbers::pi * double(j) / double(size);
        out[2 * j] = float(std::cos(angle));
        out[2 * j + 1] = float(std::sin(angle));
    }
    return out;
}

GpuStatus
recordFft2D(GpuRecordContext& ctx, const GpuFft2D& fft)
{
    if (!fft.src || !fft.dst) {
        return { VK_ERROR_INITIALIZATION_FAILED, "recordFft2D: missing source or destination buffer" };
    }
    if (!gpuFftSupportsSize(fft.width, fft.height)) {
        return refused(std::to_string(fft.width) + "x" + std::to_string(fft.height) + " is not a power of two up to "
                       + std::to_string(kGpuFftMaxSize) + " along each axis");
    }
    const uint64_t samples = uint64_t(fft.width) * fft.height;
    const uint64_t complexBytes = samples * 2 * sizeof(float);
    const uint64_t srcBytes = fft.realInput ? samples * sizeof(float) : complexBytes;
    if (fft.src->size() < srcBytes || fft.dst->size() < complexBytes) {
        return { VK_ERROR_INITIALIZATION_FAILED, "recordFft2D: buffers too small for the transform" };
    }
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(ctx.device().physicalDevice(), &props);
    if (props.limits.maxComputeSharedMemorySize < kGpuFftSharedBytes) {
        return refused("the device's compute shared memory is below " + std::to_string(kGpuFftSharedBytes) + " bytes");
    }

    // One table serves both axes: a line of n samples reads every (tableSize / n)-th entry.
    const uint32_t tableSize = std::max(fft.width, fft.height);
    const std::vector<float> table = gpuFftTwiddles(tableSize);
    GpuBuffer* twiddles = nullptr;
    if (GpuStatus s = ctx.scratch(std::span(table).size_bytes(), twiddles); !s) {
        return s;
    }
    if (GpuStatus s = ctx.updateBuffer(*twiddles, 0, std::as_bytes(std::span(table))); !s) {
        return s;
    }
    GpuBuffer* transposed = nullptr;
    if (GpuStatus s = ctx.scratch(complexBytes, transposed); !s) {
        return s;
    }

    const uint32_t inverse = fft.inverse ? 1u : 0u;
    GpuFftParams rows {};
    rows.n = fft.width;
    rows.log2n = uint32_t(std::countr_zero(fft.width));
    rows.lines = fft.height;
    rows.inverse = inverse;
    rows.realInput = fft.realInput ? 1u : 0u;
    rows.tableSize = tableSize;
    rows.scale = fft.inverse ? 1.f / float(fft.width) : 1.f;
    if (GpuStatus s = dispatchLines(ctx, fft.src, transposed, twiddles, rows); !s) {
        return s;
    }
    ctx.computeBarrier();

    GpuFftParams columns = rows;
    columns.n = fft.height;
    columns.log2n = uint32_t(std::countr_zero(fft.height));
    columns.lines = fft.width;
    columns.realInput = 0;
    columns.scale = fft.inverse ? 1.f / float(fft.height) : 1.f;
    return dispatchLines(ctx, transposed, fft.dst, twiddles, columns);
}

} // namespace gpu
