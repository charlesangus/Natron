// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/Gpu/GpuFlowTestSupport.h"

#include "Engine/Flow/DisFlow.h"

NATRON_NAMESPACE_USING
#ifdef NATRON_HAVE_GPU
using namespace FlowTest;
using namespace GpuFlowTest;

namespace {
const int kWidth = 256;
const int kHeight = 192;

// The ceilings of Tests/Flow/DisFlow_Test.cpp, which the device has to meet as well.
const double kSearchTranslationEpe = 0.06;
const double kSearchRotationEpe = 0.125;
const double kSearchLargeTranslationEpe = 0.05;
const double kRefinedTranslationEpe = 0.05;
const double kRefinedRotationEpe = 0.095;
const double kRefinedLargeTranslationEpe = 0.04;
const double kFromCpu = 0.05;

DisParams
searchOnly()
{
    DisParams params = disParamsForQuality(eFlowQualityMedium);
    params.refineFixedPointIterations = 0;

    return params;
}

// Solves a to b both ways and checks the field against the truth and the CPU's.
void
expectSolves(const Frame& a,
             const Frame& b,
             const DisParams& params,
             const TrueFlow& truth,
             int margin,
             double ceiling)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    FlowField cpu;
    ASSERT_TRUE(computeDisFlow(a.pyramid(), b.pyramid(), params, nullptr, FlowContext(), cpu));
    FlowField field;
    ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, a.width, a.height, params, &field));
    EXPECT_LT(meanEndpointError(field, truth, margin), ceiling);
    EXPECT_LT(meanFieldDistance(field, cpu), kFromCpu);
}

void
expectSubpixel(const DisParams& params,
               double ceiling)
{
    const NoiseTexture texture(1234);
    expectSolves(translatedFrame(texture, kWidth, kHeight, 0., 0.), translatedFrame(texture, kWidth, kHeight, 3.5, -2.25), params, translation(3.5f, -2.25f), 8, ceiling);
}

void
expectLarge(const DisParams& params,
            double ceiling)
{
    const NoiseTexture texture(1234);
    expectSolves(translatedFrame(texture, kWidth, kHeight, 0., 0.), translatedFrame(texture, kWidth, kHeight, 20., 0.25), params, translation(20.f, 0.25f), 28, ceiling);
}

void
expectRotation(const DisParams& params,
               double ceiling)
{
    const NoiseTexture texture(1234);
    expectSolves(translatedFrame(texture, kWidth, kHeight, 0., 0.), rotatedFrame(texture, kWidth, kHeight, 2.), params, rotation(kWidth, kHeight, 2.), 12, ceiling);
}
} // anonymous namespace

TEST(GpuDisFlow, SearchFindsASubpixelTranslation)
{
    expectSubpixel(searchOnly(), kSearchTranslationEpe);
}

TEST(GpuDisFlow, SearchFindsARotation)
{
    expectRotation(searchOnly(), kSearchRotationEpe);
}

TEST(GpuDisFlow, SearchReachesALargeTranslationThroughThePyramid)
{
    expectLarge(searchOnly(), kSearchLargeTranslationEpe);
}

TEST(GpuDisFlow, RefinedFindsASubpixelTranslation)
{
    expectSubpixel(disParamsForQuality(eFlowQualityMedium), kRefinedTranslationEpe);
}

TEST(GpuDisFlow, RefinedFindsARotation)
{
    expectRotation(disParamsForQuality(eFlowQualityMedium), kRefinedRotationEpe);
}

TEST(GpuDisFlow, RefinedReachesALargeTranslationThroughThePyramid)
{
    expectLarge(disParamsForQuality(eFlowQualityMedium), kRefinedLargeTranslationEpe);
}

// Every quality stops at its own level and upsamples from there.
TEST(GpuDisFlow, EveryQualityMatchesTheCpu)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const Frame b = translatedFrame(texture, kWidth, kHeight, 3.5, -2.25);
    for (FlowQuality quality : { eFlowQualityFast, eFlowQualityFine }) {
        const DisParams params = disParamsForQuality(quality);
        Harness harness;
        if (!harness.create()) {
            return;
        }
        FlowField cpu;
        ASSERT_TRUE(computeDisFlow(a.pyramid(), b.pyramid(), params, nullptr, FlowContext(), cpu));
        FlowField field;
        ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, kWidth, kHeight, params, &field));
        EXPECT_LT(meanFieldDistance(field, cpu), kFromCpu) << "quality " << (int)quality;
    }
}

TEST(GpuDisFlow, OddSizesMatchTheCpu)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, 257, 129, 0., 0.);
    const Frame b = translatedFrame(texture, 257, 129, 3.5, -2.25);
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const DisParams params = disParamsForQuality(eFlowQualityMedium);
    FlowField cpu;
    ASSERT_TRUE(computeDisFlow(a.pyramid(), b.pyramid(), params, nullptr, FlowContext(), cpu));
    FlowField field;
    ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, 257, 129, params, &field));
    EXPECT_LT(meanFieldDistance(field, cpu), kFromCpu);
}

TEST(GpuDisFlow, RgbChannelsMatchTheCpu)
{
    const NoiseTexture red(11);
    const NoiseTexture green(22);
    const NoiseTexture blue(33);
    const std::vector<float> first = [&]() {
        std::vector<float> pixels((std::size_t)kWidth * kHeight * 3);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                float* pixel = pixels.data() + ((std::size_t)y * kWidth + x) * 3;
                pixel[0] = red(x, y);
                pixel[1] = green(x, y);
                pixel[2] = blue(x, y);
            }
        }

        return pixels;
    }();
    const std::vector<float> second = [&]() {
        std::vector<float> pixels((std::size_t)kWidth * kHeight * 3);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                float* pixel = pixels.data() + ((std::size_t)y * kWidth + x) * 3;
                pixel[0] = red(x - 3.5, y + 2.25);
                pixel[1] = green(x - 3.5, y + 2.25);
                pixel[2] = blue(x - 3.5, y + 2.25);
            }
        }

        return pixels;
    }();
    const auto viewOf = [](const std::vector<float>& pixels) {
        FlowImageView view;
        view.pixels = pixels.data();
        view.width = kWidth;
        view.height = kHeight;
        view.nComps = 3;
        view.rowElements = (std::ptrdiff_t)kWidth * 3;

        return view;
    };
    FlowPyramid cpuFirst;
    FlowPyramid cpuSecond;
    ASSERT_TRUE(cpuFirst.build(viewOf(first), eFlowChannelsRGB, 0, FlowContext()));
    ASSERT_TRUE(cpuSecond.build(viewOf(second), eFlowChannelsRGB, 0, FlowContext()));
    const DisParams params = disParamsForQuality(eFlowQualityMedium);
    FlowField cpu;
    ASSERT_TRUE(computeDisFlow(cpuFirst, cpuSecond, params, nullptr, FlowContext(), cpu));

    Harness harness;
    if (!harness.create()) {
        return;
    }
    const gpu::GpuFlowFormat format = formatFor(kWidth, kHeight, 3, false);
    const gpu::GpuFlowImage imageFirst = harness.image(first, kWidth, kHeight, 3);
    const gpu::GpuFlowImage imageSecond = harness.image(second, kWidth, kHeight, 3);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowPyramid pyramidFirst;
    gpu::GpuFlowPyramid pyramidSecond;
    gpu::GpuFlowField field;
    status = flow.buildPyramid(*ctx, imageFirst, format, &pyramidFirst);
    if (status) {
        status = flow.buildPyramid(*ctx, imageSecond, format, &pyramidSecond);
    }
    if (status) {
        status = flow.solveDis(*ctx, pyramidFirst, pyramidSecond, nullptr, gpuParams(params), &field);
    }
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    ASSERT_TRUE(bool(status)) << status.message;
    FlowField got;
    ASSERT_TRUE(harness.readField(field, &got));
    EXPECT_LT(meanFieldDistance(got, cpu), kFromCpu);
    EXPECT_LT(meanEndpointError(got, translation(3.5f, -2.25f), 8), kRefinedTranslationEpe);
}

// An abort between batches leaves the solve failed before anything is submitted.
TEST(GpuDisFlow, AnAbortStopsTheRecording)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const gpu::GpuFlowImage image = harness.image(a.pixels, kWidth, kHeight, 1);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow([]() { return true; });
    gpu::GpuFlowPyramid pyramid;
    status = flow.buildPyramid(*ctx, image, formatFor(kWidth, kHeight, 1, false), &pyramid);
    EXPECT_FALSE(bool(status));
    EXPECT_EQ(VK_INCOMPLETE, status.result);
}
#endif // NATRON_HAVE_GPU
