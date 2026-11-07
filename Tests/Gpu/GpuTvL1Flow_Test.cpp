// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <gtest/gtest.h>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/Gpu/GpuFlowTestSupport.h"

#include "Engine/Flow/TvL1Flow.h"

NATRON_NAMESPACE_USING
#ifdef NATRON_HAVE_GPU
using namespace FlowTest;
using namespace GpuFlowTest;

namespace {
const int kWidth = 256;
const int kHeight = 192;

// The ceilings of Tests/Flow/TvL1Flow_Test.cpp, which the device has to meet as well.
const double kTranslationEpe = 0.015;
const double kRotationEpe = 0.045;
const double kLargeTranslationEpe = 0.012;
const double kFromCpu = 0.05;

void
expectSolves(const Frame& a,
             const Frame& b,
             const TrueFlow& truth,
             int margin,
             double ceiling)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const TvL1Params params = tvL1ParamsForQuality(eFlowQualityMedium);
    FlowField cpu;
    ASSERT_TRUE(computeTvL1Flow(a.pyramid(), b.pyramid(), params, nullptr, FlowContext(), cpu));
    FlowField field;
    ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, a.width, a.height, params, &field));
    EXPECT_LT(meanEndpointError(field, truth, margin), ceiling);
    EXPECT_LT(meanFieldDistance(field, cpu), kFromCpu);
}
} // anonymous namespace

TEST(GpuTvL1Flow, FindsASubpixelTranslation)
{
    const NoiseTexture texture(1234);
    expectSolves(translatedFrame(texture, kWidth, kHeight, 0., 0.), translatedFrame(texture, kWidth, kHeight, 3.5, -2.25), translation(3.5f, -2.25f), 8, kTranslationEpe);
}

TEST(GpuTvL1Flow, FindsARotation)
{
    const NoiseTexture texture(1234);
    expectSolves(translatedFrame(texture, kWidth, kHeight, 0., 0.), rotatedFrame(texture, kWidth, kHeight, 2.), rotation(kWidth, kHeight, 2.), 12, kRotationEpe);
}

TEST(GpuTvL1Flow, ReachesALargeTranslationThroughThePyramid)
{
    const NoiseTexture texture(1234);
    expectSolves(translatedFrame(texture, kWidth, kHeight, 0., 0.), translatedFrame(texture, kWidth, kHeight, 20., 0.25), translation(20.f, 0.25f), 28, kLargeTranslationEpe);
}

// Fast stops above the finest level and upsamples from there.
TEST(GpuTvL1Flow, FastMatchesTheCpuAtOddSizes)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, 257, 129, 0., 0.);
    const Frame b = translatedFrame(texture, 257, 129, 3.5, -2.25);
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const TvL1Params params = tvL1ParamsForQuality(eFlowQualityFast);
    FlowField cpu;
    ASSERT_TRUE(computeTvL1Flow(a.pyramid(), b.pyramid(), params, nullptr, FlowContext(), cpu));
    FlowField field;
    ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, 257, 129, params, &field));
    EXPECT_LT(meanFieldDistance(field, cpu), kFromCpu);
}
#endif // NATRON_HAVE_GPU
