#ifndef Tests_Gpu_GpuFlowTestSupport_h
#define Tests_Gpu_GpuFlowTestSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Flow/DisFlow.h"
#include "Engine/Flow/FlowField.h"
#include "Engine/Flow/FlowPyramid.h"
#include "Engine/Flow/TvL1Flow.h"
#include "Engine/Flow/VariationalRefine.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuFlow.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Tests/Native/NativeGpuParity.h"

NATRON_NAMESPACE_ENTER

namespace GpuFlowTest {
// A software device takes far longer over a whole solve than a submit is otherwise allowed.
constexpr std::uint64_t kSubmitTimeoutNs = 280'000'000'000ull;

inline gpu::GpuFlowFormat
formatFor(int width,
          int height,
          int channels,
          bool luminance,
          int depth = 0)
{
    gpu::GpuFlowFormat format;
    format.width = (std::uint32_t)width;
    format.height = (std::uint32_t)height;
    format.channels = (std::uint32_t)channels;
    format.luminance = luminance;
    format.depth = (std::uint32_t)((depth > 0) ? depth : flowPyramidDepthForSize(width, height));
    const std::vector<float> weights = BlurKernels::LineFilter::firGaussianWeights(1.0);
    EXPECT_EQ(format.downsampleWeights.size(), weights.size());
    std::copy_n(weights.begin(), std::min(weights.size(), format.downsampleWeights.size()), format.downsampleWeights.begin());

    return format;
}

inline gpu::GpuFlowDisParams
gpuParams(const DisParams& params)
{
    const RefineParams refine;
    gpu::GpuFlowDisParams out;
    out.finestLevel = params.finestLevel;
    out.patchSize = params.patchSize;
    out.patchStride = params.patchStride;
    out.searchIterations = params.searchIterations;
    out.refineFixedPointIterations = params.refineFixedPointIterations;
    out.refineSorIterations = params.refineSorIterations;
    out.alpha = refine.alpha;
    out.gamma = refine.gamma;
    out.delta = refine.delta;
    out.omega = refine.omega;

    return out;
}

inline gpu::GpuFlowTvL1Params
gpuParams(const TvL1Params& params)
{
    gpu::GpuFlowTvL1Params out;
    out.finestLevel = params.finestLevel;
    out.warps = params.warps;
    out.iterations = params.iterations;
    out.lambda = params.lambda;
    out.theta = params.theta;
    out.tau = params.tau;

    return out;
}

/// The runtime, with the host side of what a test feeds it and reads back.
class Harness {
public:
    bool create()
    {
        return createRuntime(_runtime);
    }

    gpu::GpuRuntime& runtime()
    {
        return _runtime->runtime();
    }

    const gpu::GpuBuffer* upload(const std::vector<float>& floats)
    {
        gpu::GpuBuffer* buffer = _runtime->buffer(_nextSlot++, floats.size());
        std::vector<float> data = floats;
        if (!buffer || !transfer(*buffer, data, true)) {
            return nullptr;
        }

        return buffer;
    }

    gpu::GpuFlowImage image(const std::vector<float>& pixels,
                            int width,
                            int height,
                            int nComps)
    {
        gpu::GpuFlowImage result;
        result.buffer = upload(pixels);
        result.rowStrideFloats = (std::uint32_t)(width * nComps);
        result.pixelStride = (std::uint32_t)nComps;
        result.width = width;
        result.height = height;

        return result;
    }

    gpu::GpuFlowField uploadField(const FlowField& field)
    {
        std::vector<float> floats((std::size_t)field.width * field.height * 3);
        for (int y = 0; y < field.height; ++y) {
            for (int x = 0; x < field.width; ++x) {
                float* pixel = floats.data() + ((std::size_t)y * field.width + x) * 3;
                pixel[0] = field.u.at(x, y);
                pixel[1] = field.v.at(x, y);
                pixel[2] = field.confidence.at(x, y);
            }
        }
        gpu::GpuFlowField result;
        result.buffer = upload(floats);
        result.width = (std::uint32_t)field.width;
        result.height = (std::uint32_t)field.height;

        return result;
    }

    std::vector<float> read(const gpu::GpuBuffer* buffer,
                            std::size_t floats)
    {
        std::vector<float> data(floats);
        if (!buffer || !transfer(const_cast<gpu::GpuBuffer&>(*buffer), data, false)) {
            data.clear();
        }

        return data;
    }

    bool readField(const gpu::GpuFlowField& field,
                   FlowField* out)
    {
        const std::vector<float> floats = read(field.buffer, (std::size_t)field.width * field.height * 3);
        if (floats.empty()) {
            return false;
        }
        *out = FlowField((int)field.width, (int)field.height);
        for (int y = 0; y < out->height; ++y) {
            for (int x = 0; x < out->width; ++x) {
                const float* pixel = floats.data() + ((std::size_t)y * out->width + x) * 3;
                out->u.at(x, y) = pixel[0];
                out->v.at(x, y) = pixel[1];
                out->confidence.at(x, y) = pixel[2];
            }
        }

        return true;
    }

private:
    bool transfer(gpu::GpuBuffer& buffer,
                  std::vector<float>& data,
                  bool toDevice)
    {
        gpu::TimelineValue done;
        const gpu::HostRange range { data.data(), data.size() * sizeof(float) };
        gpu::GpuStatus status = toDevice ? runtime().upload(range, buffer, 0, done) : runtime().download(buffer, 0, range, done);
        if (status) {
            status = runtime().wait(done);
        }
        EXPECT_TRUE(bool(status)) << status.message;

        return bool(status);
    }

    std::unique_ptr<GpuParityRuntime> _runtime;
    int _nextSlot = 0;
};

/// The mean distance between two fields' vectors over the whole frame.
inline double
meanFieldDistance(const FlowField& a,
                  const FlowField& b)
{
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    if ((a.width != b.width) || (a.height != b.height) || (a.width == 0) || (a.height == 0)) {
        return 1e30;
    }
    double sum = 0.;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const double du = a.u.at(x, y) - b.u.at(x, y);
            const double dv = a.v.at(x, y) - b.v.at(x, y);
            sum += std::sqrt(du * du + dv * dv);
        }
    }

    return sum / ((double)a.width * a.height);
}

/// The largest difference between two fields' vectors and confidences.
inline double
largestFieldDifference(const FlowField& a,
                       const FlowField& b)
{
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    if ((a.width != b.width) || (a.height != b.height)) {
        return 1e30;
    }
    double worst = 0.;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            worst = std::max(worst, (double)std::abs(a.u.at(x, y) - b.u.at(x, y)));
            worst = std::max(worst, (double)std::abs(a.v.at(x, y) - b.v.at(x, y)));
            worst = std::max(worst, (double)std::abs(a.confidence.at(x, y) - b.confidence.at(x, y)));
        }
    }

    return worst;
}

/// Solves from a to b on the device, both single-component frames of the same size.
template <typename Params>
bool
solveOnDevice(Harness& harness,
              const std::vector<float>& a,
              const std::vector<float>& b,
              int width,
              int height,
              const Params& params,
              FlowField* out)
{
    const gpu::GpuFlowFormat format = formatFor(width, height, 1, false);
    const gpu::GpuFlowImage imageA = harness.image(a, width, height, 1);
    const gpu::GpuFlowImage imageB = harness.image(b, width, height, 1);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    EXPECT_TRUE(bool(status)) << status.message;
    if (!status) {
        return false;
    }
    gpu::GpuFlow flow;
    gpu::GpuFlowPyramid pyramidA;
    gpu::GpuFlowPyramid pyramidB;
    gpu::GpuFlowField field;
    status = flow.buildPyramid(*ctx, imageA, format, &pyramidA);
    if (status) {
        status = flow.buildPyramid(*ctx, imageB, format, &pyramidB);
    }
    if (status) {
        if constexpr (std::is_same_v<Params, DisParams>) {
            status = flow.solveDis(*ctx, pyramidA, pyramidB, nullptr, gpuParams(params), &field);
        } else {
            status = flow.solveTvL1(*ctx, pyramidA, pyramidB, nullptr, gpuParams(params), &field);
        }
    }
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    EXPECT_TRUE(bool(status)) << status.message;

    return bool(status) && harness.readField(field, out);
}
} // namespace GpuFlowTest

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Tests_Gpu_GpuFlowTestSupport_h
