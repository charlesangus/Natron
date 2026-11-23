// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/Gpu/GpuFlowTestSupport.h"

#include "Engine/Flow/DisFlow.h"
#include "Engine/Flow/FlowConfidence.h"
#include "Engine/Flow/FlowEstimate.h"
#include "Engine/Flow/FlowTemporal.h"

NATRON_NAMESPACE_USING
#ifdef NATRON_HAVE_GPU
using namespace FlowTest;
using namespace GpuFlowTest;

namespace {
const double kTolerance = 1e-3;
const double kFromCpu = 0.05;
const float kSigma = 2.f;

// The moving square of Tests/Flow/FlowConfidence_Test.cpp.
const int kSquareWidth = 192;
const int kSquareHeight = 144;
const int kSquareX = 64;
const int kSquareY = 40;
const int kSquareSize = 64;
const int kSquareSpeed = 14;

// The constant motion of Tests/Flow/FlowTemporal_Test.cpp.
const int kSteadyWidth = 192;
const int kSteadyHeight = 144;
const double kSteadyX = 2.5;
const double kSteadyY = -1.25;

bool
inSquare(double x,
         double y,
         int time)
{
    const int left = kSquareX + time * kSquareSpeed;

    return (x >= left) && (x < left + kSquareSize) && (y >= kSquareY) && (y < kSquareY + kSquareSize);
}

Frame
squareFrame(int time)
{
    const NoiseTexture background(101);
    const NoiseTexture foreground(202);

    return Frame(kSquareWidth, kSquareHeight, [&](double x, double y) {
        return inSquare(x, y, time) ? foreground(x - time * kSquareSpeed, y) : background(x, y);
    });
}

Frame
squareMatte(int time)
{
    return Frame(kSquareWidth, kSquareHeight, [&](double x, double y) { return inSquare(x, y, time) ? 1.f : 0.f; });
}

Frame
steadyFrame(int time)
{
    const NoiseTexture texture(77);
    std::mt19937 engine(4242u + (unsigned)time);
    Frame frame = translatedFrame(texture, kSteadyWidth, kSteadyHeight, kSteadyX * time, kSteadyY * time);
    for (float& value : frame.pixels) {
        value += 0.03f * (static_cast<float>(engine()) / 4294967296.f - 0.5f);
    }

    return frame;
}

FlowField
cpuFlow(const Frame& a,
        const Frame& b)
{
    FlowField flow;
    EXPECT_TRUE(computeDisFlow(a.pyramid(), b.pyramid(), disParamsForQuality(eFlowQualityMedium), nullptr, FlowContext(), flow));

    return flow;
}

// The round trip of the CPU's own fields, so the difference is the kernel's alone.
void
expectConfidenceMatches(const Frame& a,
                        const Frame& b)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    FlowField there = cpuFlow(a, b);
    const FlowField back = cpuFlow(b, a);
    const gpu::GpuFlowField deviceThere = harness.uploadField(there);
    const gpu::GpuFlowField deviceBack = harness.uploadField(back);
    flowComputeConfidence(back, kSigma, FlowContext(), there);

    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    status = flow.confidence(*ctx, deviceBack, kSigma, deviceThere);
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    ASSERT_TRUE(bool(status)) << status.message;
    FlowField got;
    ASSERT_TRUE(harness.readField(deviceThere, &got));
    EXPECT_LE(largestFieldDifference(got, there), kTolerance);
}

void
expectTemporalMatches(int neighbourCount,
                      float strength)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const Frame frames[] = { steadyFrame(0), steadyFrame(1), steadyFrame(2), steadyFrame(3) };
    FlowField current = cpuFlow(frames[1], frames[2]);
    const FlowField ahead = cpuFlow(frames[2], frames[3]);
    const FlowField behind = cpuFlow(frames[0], frames[1]);
    const FlowField back = cpuFlow(frames[1], frames[0]);
    flowComputeConfidence(cpuFlow(frames[2], frames[1]), kSigma, FlowContext(), current);
    FlowField aheadConfident = ahead;
    FlowField behindConfident = behind;
    FlowField backConfident = back;
    flowComputeConfidence(cpuFlow(frames[3], frames[2]), kSigma, FlowContext(), aheadConfident);
    flowComputeConfidence(back, kSigma, FlowContext(), behindConfident);
    flowComputeConfidence(behind, kSigma, FlowContext(), backConfident);

    const FlowTemporalNeighbour neighbours[] = { { &aheadConfident, &current }, { &behindConfident, &backConfident } };
    FlowField expected;
    flowSmoothTemporal(current, neighbours, neighbourCount, strength, FlowContext(), expected);

    const gpu::GpuFlowField deviceCurrent = harness.uploadField(current);
    const gpu::GpuFlowField deviceAhead = harness.uploadField(aheadConfident);
    const gpu::GpuFlowField deviceBehind = harness.uploadField(behindConfident);
    const gpu::GpuFlowField deviceBack = harness.uploadField(backConfident);
    std::vector<gpu::GpuFlowTemporalNeighbour> deviceNeighbours;
    const gpu::GpuFlowTemporalNeighbour all[] = { { deviceAhead, deviceCurrent }, { deviceBehind, deviceBack } };
    deviceNeighbours.assign(all, all + neighbourCount);

    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowField out;
    status = flow.smoothTemporal(*ctx, deviceCurrent, deviceNeighbours, strength, &out);
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    ASSERT_TRUE(bool(status)) << status.message;
    FlowField got;
    ASSERT_TRUE(harness.readField(out, &got));
    EXPECT_LE(largestFieldDifference(got, expected), kTolerance) << neighbourCount << " neighbours at strength " << strength;
}

// A matte through buildMatte(), every level against the CPU's pyramid, with both passes' weights.
void
expectMatteMatches(const Frame& matte,
                   bool separates)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const gpu::GpuFlowFormat format = formatFor(matte.width, matte.height, 1, false);
    FlowPlane base(matte.width, matte.height);
    base.data = matte.pixels;
    const std::vector<FlowPlane> levels = flowBuildPlanePyramid(base, (int)format.depth, FlowContext());

    const gpu::GpuFlowImage image = harness.image(matte.pixels, matte.width, matte.height, 1);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowMatte device;
    status = flow.buildMatte(*ctx, image, format, &device);
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    ASSERT_TRUE(bool(status)) << status.message;
    ASSERT_EQ(levels.size(), device.matte.levels.size());
    const gpu::GpuFlowLevel& last = device.matte.levels.back();
    const std::size_t total = last.offsetFloats + (std::size_t)last.width * last.height;
    const std::vector<float> base0 = harness.read(device.matte.buffer, total);
    const std::vector<float> inside = harness.read(device.inside.buffer, total);
    const std::vector<float> outside = harness.read(device.outside.buffer, total);
    ASSERT_EQ(total, base0.size());
    ASSERT_EQ(total, inside.size());
    ASSERT_EQ(total, outside.size());
    for (std::size_t l = 0; l < levels.size(); ++l) {
        const FlowPlane& level = levels[l];
        const std::size_t offset = device.matte.levels[l].offsetFloats;
        double worst = 0.;
        for (int y = 0; y < level.height; ++y) {
            for (int x = 0; x < level.width; ++x) {
                const std::size_t i = offset + (std::size_t)y * level.width + x;
                const float m = level.at(x, y);
                worst = std::max(worst, (double)std::abs(base0[i] - m));
                worst = std::max(worst, (double)std::abs(inside[i] - (separates ? m : 1.f)));
                worst = std::max(worst, (double)std::abs(outside[i] - (separates ? 1.f - m : 1.f)));
            }
        }
        EXPECT_LE(worst, kTolerance) << "level " << l;
    }
}

// Runs the whole estimate and compares it with the CPU's, with the moving square's matte when asked.
void
expectEstimateMatches(bool useMatte,
                      FlowMethod method)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const Frame previous = squareFrame(-1);
    const Frame current = squareFrame(0);
    const Frame next = squareFrame(1);
    const Frame previousMatte = squareMatte(-1);
    const Frame currentMatte = squareMatte(0);
    const Frame nextMatte = squareMatte(1);
    const auto frameOf = [useMatte](const Frame& frame, const Frame& matte) {
        FlowFrame result;
        result.image = frame.view();
        if (useMatte) {
            result.matte = matte.pixels.data();
            result.matteRowElements = matte.width;
        }

        return result;
    };
    const FlowFrame cpuPrevious = frameOf(previous, previousMatte);
    const FlowFrame cpuCurrent = frameOf(current, currentMatte);
    const FlowFrame cpuNext = frameOf(next, nextMatte);
    FlowEstimateParams cpuParams;
    cpuParams.method = method;
    cpuParams.quality = eFlowQualityMedium;
    FlowEstimateResult cpu;
    ASSERT_EQ(eFlowEstimateOK, estimateFlow(&cpuPrevious, cpuCurrent, &cpuNext, cpuParams, FlowContext(), cpu));

    gpu::GpuFlowEstimateParams params;
    params.format = formatFor(kSquareWidth, kSquareHeight, 1, false);
    params.tvL1 = (method == eFlowMethodTVL1);
    params.dis = gpuParams(disParamsForQuality(eFlowQualityMedium));
    params.tvL1Params = gpuParams(tvL1ParamsForQuality(eFlowQualityMedium));
    params.confidenceSigma = cpuParams.confidenceSigma;
    gpu::GpuFlowEstimate estimate(params, std::function<bool()>());
    const Frame* frames[] = { &previous, &current, &next };
    const Frame* mattes[] = { &previousMatte, &currentMatte, &nextMatte };
    for (int i = 0; i < 3; ++i) {
        const gpu::GpuFlowImage image = harness.image(frames[i]->pixels, kSquareWidth, kSquareHeight, 1);
        const gpu::GpuFlowImage matte = useMatte ? harness.image(mattes[i]->pixels, kSquareWidth, kSquareHeight, 1) : gpu::GpuFlowImage();
        estimate.setFrame(gpu::GpuFlowEstimate::kCurrent - 1 + i, image, useMatte ? &matte : nullptr);
    }

    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlowField forward;
    gpu::GpuFlowField backward;
    status = estimate.direction(*ctx, gpu::GpuFlowEstimate::kCurrent, gpu::GpuFlowEstimate::kCurrent + 1, true, &forward);
    if (status) {
        status = estimate.direction(*ctx, gpu::GpuFlowEstimate::kCurrent, gpu::GpuFlowEstimate::kCurrent - 1, true, &backward);
    }
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    ASSERT_TRUE(bool(status)) << status.message;
    FlowField gotForward;
    FlowField gotBackward;
    ASSERT_TRUE(harness.readField(forward, &gotForward));
    ASSERT_TRUE(harness.readField(backward, &gotBackward));
    EXPECT_LT(meanFieldDistance(gotForward, cpu.forward), kFromCpu);
    EXPECT_LT(meanFieldDistance(gotBackward, cpu.backward), kFromCpu);
}
} // anonymous namespace

TEST(GpuFlowConfidence, RoundTripMatchesTheCpuOnTheMovingSquare)
{
    expectConfidenceMatches(squareFrame(0), squareFrame(1));
}

TEST(GpuFlowConfidence, RoundTripMatchesTheCpuOnConstantVelocity)
{
    expectConfidenceMatches(steadyFrame(0), steadyFrame(1));
}

TEST(GpuFlowConfidence, TemporalBlendMatchesTheCpuOnConstantVelocity)
{
    expectTemporalMatches(2, 1.f);
    expectTemporalMatches(2, 0.5f);
    expectTemporalMatches(1, 1.f);
    expectTemporalMatches(0, 1.f);
}

TEST(GpuFlowConfidence, StrengthZeroGivesTheCurrentField)
{
    expectTemporalMatches(2, 0.f);
}

TEST(GpuFlowConfidence, MatteWeightsMatchTheCpuOnTheMovingSquare)
{
    expectMatteMatches(squareMatte(0), true);
}

TEST(GpuFlowConfidence, AMatteThatSeparatesNothingWeighsEveryPixelOne)
{
    expectMatteMatches(Frame(kSquareWidth, kSquareHeight, [](double, double) { return 0.f; }), false);
    expectMatteMatches(Frame(kSquareWidth, kSquareHeight, [](double, double) { return 1.f; }), false);
}

TEST(GpuFlowConfidence, CompositeMixesThePassesByTheMatte)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const Frame matte = squareMatte(0);
    const gpu::GpuFlowFormat format = formatFor(kSquareWidth, kSquareHeight, 1, false);
    FlowField inside = cpuFlow(squareFrame(0), squareFrame(1));
    FlowField outside = cpuFlow(squareFrame(1), squareFrame(0));
    flowComputeConfidence(outside, kSigma, FlowContext(), inside);
    flowComputeConfidence(inside, kSigma, FlowContext(), outside);
    FlowField expected(kSquareWidth, kSquareHeight);
    for (int y = 0; y < kSquareHeight; ++y) {
        for (int x = 0; x < kSquareWidth; ++x) {
            const float m = matte.pixels[(std::size_t)y * kSquareWidth + x];
            expected.u.at(x, y) = outside.u.at(x, y) + m * (inside.u.at(x, y) - outside.u.at(x, y));
            expected.v.at(x, y) = outside.v.at(x, y) + m * (inside.v.at(x, y) - outside.v.at(x, y));
            expected.confidence.at(x, y) = outside.confidence.at(x, y) + m * (inside.confidence.at(x, y) - outside.confidence.at(x, y));
        }
    }
    const gpu::GpuFlowField deviceInside = harness.uploadField(inside);
    const gpu::GpuFlowField deviceOutside = harness.uploadField(outside);
    const gpu::GpuFlowImage image = harness.image(matte.pixels, kSquareWidth, kSquareHeight, 1);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    gpu::GpuFlow flow;
    gpu::GpuFlowMatte deviceMatte;
    gpu::GpuFlowField out;
    status = flow.buildMatte(*ctx, image, format, &deviceMatte);
    if (status) {
        status = flow.composite(*ctx, deviceInside, deviceOutside, deviceMatte, &out);
    }
    if (status) {
        status = ctx->submitAndWait(kSubmitTimeoutNs);
    }
    ASSERT_TRUE(bool(status)) << status.message;
    FlowField got;
    ASSERT_TRUE(harness.readField(out, &got));
    EXPECT_LE(largestFieldDifference(got, expected), kTolerance);
}

TEST(GpuFlowConfidence, EstimateMatchesTheCpuWithoutAMatte)
{
    expectEstimateMatches(false, eFlowMethodDIS);
}

TEST(GpuFlowConfidence, EstimateMatchesTheCpuWithAMatte)
{
    expectEstimateMatches(true, eFlowMethodDIS);
}

TEST(GpuFlowConfidence, TvL1EstimateMatchesTheCpuWithAMatte)
{
    expectEstimateMatches(true, eFlowMethodTVL1);
}
#endif // NATRON_HAVE_GPU
