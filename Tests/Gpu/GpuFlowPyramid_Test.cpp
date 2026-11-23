// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/Gpu/GpuFlowTestSupport.h"

#include "Engine/Flow/FlowPyramid.h"
#include "Engine/Nodes/Time/VectorGenerator.h"

NATRON_NAMESPACE_USING
#ifdef NATRON_HAVE_GPU
using namespace FlowTest;
using namespace GpuFlowTest;

namespace {
const double kTolerance = 1e-5;

std::vector<float>
texturedImage(int width,
              int height,
              int nComps)
{
    const NoiseTexture texture(1234);
    std::vector<float> pixels((std::size_t)width * height * nComps);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < nComps; ++c) {
                pixels[((std::size_t)y * width + x) * nComps + c] = texture(x + 7.5 * c, y - 3.25 * c);
            }
        }
    }

    return pixels;
}

// Matches every level of the device's pyramid against FlowPyramid: values and gradients.
void
expectMatchesCpu(const std::vector<float>& pixels,
                 int width,
                 int height,
                 int nComps,
                 FlowChannels channels,
                 int depth)
{
    FlowImageView view;
    view.pixels = pixels.data();
    view.width = width;
    view.height = height;
    view.nComps = nComps;
    view.rowElements = (std::ptrdiff_t)width * nComps;
    FlowPyramid cpu;
    ASSERT_TRUE(cpu.build(view, channels, depth, FlowContext()));

    Harness harness;
    if (!harness.create()) {
        return;
    }
    const int matched = flowMatchedChannelCount(nComps, channels);
    gpu::GpuFlowFormat format = formatFor(width, height, matched, (nComps >= 3) && (channels == eFlowChannelsLuminance), depth);
    const gpu::GpuFlowImage image = harness.image(pixels, width, height, nComps);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowPyramid pyramid;
    status = flow.buildPyramid(*ctx, image, format, &pyramid);
    ASSERT_TRUE(bool(status)) << status.message;
    status = ctx->submitAndWait(kSubmitTimeoutNs);
    ASSERT_TRUE(bool(status)) << status.message;

    ASSERT_EQ((std::size_t)cpu.depth(), pyramid.levels.size());
    ASSERT_EQ((std::uint32_t)matched, pyramid.channels);
    const gpu::GpuFlowLevel& last = pyramid.levels.back();
    const std::size_t total = last.offsetFloats + (std::size_t)last.width * last.height * matched * 3;
    const std::vector<float> floats = harness.read(pyramid.buffer, total);
    ASSERT_EQ(total, floats.size());
    for (int l = 0; l < cpu.depth(); ++l) {
        const FlowPyramidLevel& expected = cpu.level(l);
        const gpu::GpuFlowLevel& level = pyramid.levels[l];
        ASSERT_EQ((std::uint32_t)expected.width, level.width) << "level " << l;
        ASSERT_EQ((std::uint32_t)expected.height, level.height) << "level " << l;
        double worst = 0.;
        for (int y = 0; y < expected.height; ++y) {
            for (int x = 0; x < expected.width; ++x) {
                for (int c = 0; c < matched; ++c) {
                    const float* got = floats.data() + level.offsetFloats + ((std::size_t)y * expected.width + x) * matched * 3 + c * 3;
                    worst = std::max(worst, (double)std::abs(got[0] - expected.channels[c].at(x, y)));
                    worst = std::max(worst, (double)std::abs(got[1] - expected.gradX[c].at(x, y)));
                    worst = std::max(worst, (double)std::abs(got[2] - expected.gradY[c].at(x, y)));
                }
            }
        }
        EXPECT_LE(worst, kTolerance) << "level " << l << " of " << width << "x" << height;
    }
}
} // anonymous namespace

TEST(GpuFlowPyramid, MatchesTheCpuAtOddSizes)
{
    expectMatchesCpu(texturedImage(257, 129, 1), 257, 129, 1, eFlowChannelsLuminance, 0);
    expectMatchesCpu(texturedImage(101, 67, 1), 101, 67, 1, eFlowChannelsLuminance, 4);
}

TEST(GpuFlowPyramid, MatchesTheCpuAtADefaultFrame)
{
    expectMatchesCpu(texturedImage(256, 192, 1), 256, 192, 1, eFlowChannelsLuminance, 0);
}

TEST(GpuFlowPyramid, MatchesTheCpuDownToOnePixel)
{
    expectMatchesCpu(texturedImage(5, 3, 1), 5, 3, 1, eFlowChannelsLuminance, 50);
}

TEST(GpuFlowPyramid, LuminanceMatchesTheCpu)
{
    expectMatchesCpu(texturedImage(133, 71, 4), 133, 71, 4, eFlowChannelsLuminance, 3);
    expectMatchesCpu(texturedImage(133, 71, 3), 133, 71, 3, eFlowChannelsLuminance, 3);
}

TEST(GpuFlowPyramid, RgbMatchesTheCpu)
{
    expectMatchesCpu(texturedImage(133, 71, 3), 133, 71, 3, eFlowChannelsRGB, 3);
}

TEST(GpuFlowPyramid, NonFiniteSamplesCountAsZero)
{
    std::vector<float> pixels = texturedImage(65, 33, 1);
    pixels[100] = std::numeric_limits<float>::quiet_NaN();
    pixels[200] = std::numeric_limits<float>::infinity();
    pixels[300] = -std::numeric_limits<float>::infinity();
    expectMatchesCpu(pixels, 65, 33, 1, eFlowChannelsLuminance, 2);
}

// The source's position in the window: what it does not cover reads as 0, and it may start outside.
TEST(GpuFlowPyramid, AnImageOffsetInTheWindowPadsWithBlack)
{
    const int width = 32;
    const int height = 16;
    const std::vector<float> pixels = texturedImage(width, height, 1);
    Harness harness;
    if (!harness.create()) {
        return;
    }
    gpu::GpuFlowImage image = harness.image(pixels, width, height, 1);
    image.x0 = 5;
    image.y0 = -3;
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowPyramid pyramid;
    status = flow.buildPyramid(*ctx, image, formatFor(width, height, 1, false, 1), &pyramid);
    ASSERT_TRUE(bool(status)) << status.message;
    status = ctx->submitAndWait(kSubmitTimeoutNs);
    ASSERT_TRUE(bool(status)) << status.message;
    const std::vector<float> floats = harness.read(pyramid.buffer, (std::size_t)width * height * 3);
    ASSERT_EQ((std::size_t)width * height * 3, floats.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float got = floats[((std::size_t)y * width + x) * 3];
            const int sx = x - 5;
            const int sy = y + 3;
            const bool covered = (sx >= 0) && (sy < height);
            const float expected = covered ? pixels[(std::size_t)sy * width + sx] : 0.f;
            ASSERT_FLOAT_EQ(expected, got) << "at " << x << ", " << y;
        }
    }
}

TEST(GpuFlowPyramid, MatchingCurveMatchesTheNodes)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::vector<float> samples = { -1.f, 0.f, 1e-30f, 1e-7f, 1e-3f, 0.02f, 0.05f, 0.18f, 0.5f, 1.f, 2.f, 40.f, 1e8f, 1e38f, nan, infinity, -infinity };
    const int width = (int)samples.size();
    Harness harness;
    if (!harness.create()) {
        return;
    }
    gpu::GpuFlowFormat format = formatFor(width, 1, 1, false, 1);
    format.matchingCurve = true;
    const gpu::GpuFlowImage image = harness.image(samples, width, 1, 1);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowPyramid pyramid;
    status = flow.buildPyramid(*ctx, image, format, &pyramid);
    ASSERT_TRUE(bool(status)) << status.message;
    status = ctx->submitAndWait(kSubmitTimeoutNs);
    ASSERT_TRUE(bool(status)) << status.message;
    const std::vector<float> floats = harness.read(pyramid.buffer, samples.size() * 3);
    ASSERT_EQ(samples.size() * 3, floats.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        EXPECT_NEAR(vectorGeneratorMatchingValue(samples[i]), floats[i * 3], kTolerance) << "sample " << samples[i];
    }
}
#endif // NATRON_HAVE_GPU
