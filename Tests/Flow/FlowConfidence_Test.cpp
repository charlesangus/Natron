/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Flow/FlowConfidence.h"
#include "Engine/Flow/FlowEstimate.h"
#include "Tests/Flow/FlowTestPlate.h"

NATRON_NAMESPACE_USING
using namespace FlowTest;

namespace {
const int kWidth = 192;
const int kHeight = 144;
const int kSquareX = 64;
const int kSquareY = 40;
const int kSquareSize = 64;
// The estimators resolve a motion boundary to a few pixels, so the uncovered strip must be wider
// than that for its middle to show the round trip failing.
const int kSpeed = 14;

bool
inSquare(double x,
         double y,
         int time)
{
    const int left = kSquareX + time * kSpeed;
    return x >= left && x < left + kSquareSize && y >= kSquareY && y < kSquareY + kSquareSize;
}

/// A static background with a square of its own texture moving right by kSpeed per frame.
Frame
squareFrame(int time)
{
    const NoiseTexture background(101);
    const NoiseTexture foreground(202);
    return Frame(kWidth, kHeight, [&](double x, double y) {
        return inSquare(x, y, time) ? foreground(x - time * kSpeed, y) : background(x, y);
    });
}

Frame
squareMatte(int time)
{
    return Frame(kWidth, kHeight, [&](double x, double y) { return inSquare(x, y, time) ? 1.f : 0.f; });
}

FlowFrame
frameOf(const Frame& frame,
        const Frame* matte = nullptr)
{
    FlowFrame result;
    result.image = frame.view();
    if (matte) {
        result.matte = matte->pixels.data();
        result.matteRowElements = matte->width;
    }
    return result;
}

FlowEstimateParams
paramsFor(FlowMethod method,
          FlowQuality quality)
{
    FlowEstimateParams params;
    params.method = method;
    params.quality = quality;
    return params;
}

FlowEstimateResult
estimateSquare(const FlowEstimateParams& params,
               bool useMatte)
{
    const Frame previous = squareFrame(-1);
    const Frame current = squareFrame(0);
    const Frame next = squareFrame(1);
    const Frame previousMatte = squareMatte(-1);
    const Frame currentMatte = squareMatte(0);
    const Frame nextMatte = squareMatte(1);
    const FlowFrame p = frameOf(previous, useMatte ? &previousMatte : nullptr);
    const FlowFrame c = frameOf(current, useMatte ? &currentMatte : nullptr);
    const FlowFrame n = frameOf(next, useMatte ? &nextMatte : nullptr);
    FlowEstimateResult result;
    EXPECT_EQ(estimateFlow(&p, c, &n, params, FlowContext(), result), eFlowEstimateOK);
    return result;
}

/// Mean confidence of the backward field in the strip the square left between t-1 and t. The vertical
/// inset and one column at the far side are the boundary the estimators cannot place exactly.
double
uncoveredStripConfidence(const FlowEstimateResult& result)
{
    double sum = 0.;
    int count = 0;
    for (int y = kSquareY + 4; y < kSquareY + kSquareSize - 4; ++y) {
        for (int x = kSquareX - kSpeed + 1; x < kSquareX; ++x) {
            sum += result.backward.confidence.at(x, y);
            ++count;
        }
    }
    return sum / count;
}

/// The lowest backward confidence outside the square and the strip, by more than kGuard pixels.
float
lowestConfidenceElsewhere(const FlowEstimateResult& result)
{
    const int kGuard = 4;
    float lowest = 1.f;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const bool nearby = x >= kSquareX - kSpeed - kGuard && x < kSquareX + kSquareSize + kGuard
                && y >= kSquareY - kGuard && y < kSquareY + kSquareSize + kGuard;
            if (!nearby) {
                lowest = std::min(lowest, result.backward.confidence.at(x, y));
            }
        }
    }
    return lowest;
}

/// Mean endpoint error over the background within 4 pixels of the square.
double
backgroundBandError(const FlowField& flow)
{
    return meanEndpointError(flow, translation(0.f, 0.f), [](int x, int y) {
        const bool nearby = x >= kSquareX - 4 && x < kSquareX + kSquareSize + 4 && y >= kSquareY - 4 && y < kSquareY + kSquareSize + 4;
        return nearby && !inSquare(x, y, 0);
    });
}

FlowField
constantField(int width,
              int height,
              float u,
              float v)
{
    FlowField field(width, height);
    std::fill(field.u.data.begin(), field.u.data.end(), u);
    std::fill(field.v.data.begin(), field.v.data.end(), v);
    return field;
}
} // anonymous namespace

TEST(FlowConfidence, RoundTripFormula)
{
    FlowField there = constantField(8, 8, 2.f, 0.f);
    const FlowField agreeing = constantField(8, 8, -2.f, 0.f);
    flowComputeConfidence(agreeing, 2.f, FlowContext(), there);
    EXPECT_FLOAT_EQ(there.confidence.at(1, 4), 1.f);

    const FlowField missing = constantField(8, 8, 0.f, 0.f);
    flowComputeConfidence(missing, 2.f, FlowContext(), there);
    EXPECT_FLOAT_EQ(there.confidence.at(1, 4), std::exp(-1.f));
}

TEST(FlowConfidence, ZeroWhereTheVectorLeavesTheFrame)
{
    FlowField there = constantField(8, 8, 2.f, 0.f);
    const FlowField back = constantField(8, 8, -2.f, 0.f);
    flowComputeConfidence(back, 2.f, FlowContext(), there);
    EXPECT_FLOAT_EQ(there.confidence.at(5, 0), 1.f);
    EXPECT_FLOAT_EQ(there.confidence.at(6, 0), 0.f);
    EXPECT_FLOAT_EQ(there.confidence.at(7, 0), 0.f);
}

TEST(FlowConfidence, ResidualAtTheBorderIsNotLeavingTheFrame)
{
    FlowField there = constantField(8, 8, -0.01f, 0.01f);
    const FlowField back = constantField(8, 8, 0.01f, -0.01f);
    flowComputeConfidence(back, 2.f, FlowContext(), there);
    EXPECT_FLOAT_EQ(there.confidence.at(0, 7), 1.f);
}

TEST(FlowConfidence, DisFlagsTheUncoveredStripAndNothingElse)
{
    const FlowEstimateResult result = estimateSquare(paramsFor(eFlowMethodDIS, eFlowQualityFine), false);
    EXPECT_LT(uncoveredStripConfidence(result), 0.2);
    EXPECT_GT(lowestConfidenceElsewhere(result), 0.9);
}

TEST(FlowConfidence, TvL1FlagsTheUncoveredStripAndNothingElse)
{
    const FlowEstimateResult result = estimateSquare(paramsFor(eFlowMethodTVL1, eFlowQualityMedium), false);
    EXPECT_LT(uncoveredStripConfidence(result), 0.2);
    EXPECT_GT(lowestConfidenceElsewhere(result), 0.9);
}

TEST(FlowConfidence, DisAtMediumStillFlagsTheUncoveredStrip)
{
    // Measured 0.27: Medium's coarser patch grid leaves a wider unreliable edge than Fine's.
    const FlowEstimateResult result = estimateSquare(paramsFor(eFlowMethodDIS, eFlowQualityMedium), false);
    EXPECT_LT(uncoveredStripConfidence(result), 0.35);
}

TEST(FlowConfidence, MatteSeparatesTheBackgroundAtTheSquareWithDis)
{
    const FlowEstimateParams params = paramsFor(eFlowMethodDIS, eFlowQualityMedium);
    const FlowEstimateResult plain = estimateSquare(params, false);
    const FlowEstimateResult matted = estimateSquare(params, true);
    // Measured ratios of matted to plain error: 0.50 forward, 0.72 backward.
    EXPECT_LT(backgroundBandError(matted.forward), 0.7 * backgroundBandError(plain.forward));
    EXPECT_LT(backgroundBandError(matted.backward), 0.85 * backgroundBandError(plain.backward));
}

TEST(FlowConfidence, MatteSeparatesTheBackgroundAtTheSquareWithTvL1)
{
    const FlowEstimateParams params = paramsFor(eFlowMethodTVL1, eFlowQualityFast);
    const FlowEstimateResult plain = estimateSquare(params, false);
    const FlowEstimateResult matted = estimateSquare(params, true);
    // Measured ratios of matted to plain error: 0.21 forward, 0.30 backward.
    EXPECT_LT(backgroundBandError(matted.forward), 0.4 * backgroundBandError(plain.forward));
    EXPECT_LT(backgroundBandError(matted.backward), 0.5 * backgroundBandError(plain.backward));
}

TEST(FlowConfidence, MatteThatSeparatesNothingChangesNothing)
{
    const Frame current = squareFrame(0);
    const Frame next = squareFrame(1);
    const Frame empty(kWidth, kHeight, [](double, double) { return 0.f; });
    const Frame full(kWidth, kHeight, [](double, double) { return 1.f; });
    const FlowEstimateParams params = paramsFor(eFlowMethodDIS, eFlowQualityFast);

    const FlowFrame plainCurrent = frameOf(current);
    const FlowFrame plainNext = frameOf(next);
    FlowEstimateResult plain;
    ASSERT_EQ(estimateFlow(nullptr, plainCurrent, &plainNext, params, FlowContext(), plain), eFlowEstimateOK);

    for (const Frame* matte : { &empty, &full }) {
        const FlowFrame mattedCurrent = frameOf(current, matte);
        const FlowFrame mattedNext = frameOf(next, matte);
        FlowEstimateResult matted;
        ASSERT_EQ(estimateFlow(nullptr, mattedCurrent, &mattedNext, params, FlowContext(), matted), eFlowEstimateOK);
        EXPECT_EQ(matted.forward.u.data, plain.forward.u.data);
        EXPECT_EQ(matted.forward.v.data, plain.forward.v.data);
        EXPECT_EQ(matted.forward.confidence.data, plain.forward.confidence.data);
    }
}

TEST(FlowConfidence, NonFiniteMatteSamplesCountAsZero)
{
    const Frame current = squareFrame(0);
    const Frame next = squareFrame(1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Frame matte(kWidth, kHeight, [&](double, double) { return nan; });
    const FlowEstimateParams params = paramsFor(eFlowMethodDIS, eFlowQualityFast);

    const FlowFrame plainCurrent = frameOf(current);
    const FlowFrame plainNext = frameOf(next);
    FlowEstimateResult plain;
    ASSERT_EQ(estimateFlow(nullptr, plainCurrent, &plainNext, params, FlowContext(), plain), eFlowEstimateOK);

    const FlowFrame mattedCurrent = frameOf(current, &matte);
    const FlowFrame mattedNext = frameOf(next, &matte);
    FlowEstimateResult matted;
    ASSERT_EQ(estimateFlow(nullptr, mattedCurrent, &mattedNext, params, FlowContext(), matted), eFlowEstimateOK);
    EXPECT_EQ(matted.forward.u.data, plain.forward.u.data);
}

TEST(FlowConfidence, MissingNeighbourLeavesItsDirectionEmpty)
{
    const Frame current = squareFrame(0);
    const Frame next = squareFrame(1);
    const FlowFrame c = frameOf(current);
    const FlowFrame n = frameOf(next);
    FlowEstimateResult result;
    ASSERT_EQ(estimateFlow(nullptr, c, &n, paramsFor(eFlowMethodDIS, eFlowQualityFast), FlowContext(), result), eFlowEstimateOK);
    ASSERT_EQ(result.backward.width, kWidth);
    ASSERT_EQ(result.backward.height, kHeight);
    for (std::size_t i = 0; i < result.backward.u.data.size(); ++i) {
        ASSERT_EQ(result.backward.u.data[i], 0.f);
        ASSERT_EQ(result.backward.v.data[i], 0.f);
        ASSERT_EQ(result.backward.confidence.data[i], 0.f);
    }
    EXPECT_GT(*std::max_element(result.forward.confidence.data.begin(), result.forward.confidence.data.end()), 0.9f);
}

TEST(FlowConfidence, IdenticalFramesAreStillAndFullyConfident)
{
    const Frame frame = squareFrame(0);
    const FlowFrame f = frameOf(frame);
    FlowEstimateResult result;
    ASSERT_EQ(estimateFlow(&f, f, &f, paramsFor(eFlowMethodDIS, eFlowQualityFast), FlowContext(), result), eFlowEstimateOK);
    EXPECT_LT(meanEndpointError(result.forward, translation(0.f, 0.f), 0), 1e-3);
    EXPECT_LT(meanEndpointError(result.backward, translation(0.f, 0.f), 0), 1e-3);
    EXPECT_GT(*std::min_element(result.forward.confidence.data.begin(), result.forward.confidence.data.end()), 0.99f);
    EXPECT_GT(*std::min_element(result.backward.confidence.data.begin(), result.backward.confidence.data.end()), 0.99f);
}

TEST(FlowConfidence, ReportsWhichNeighbourHasAnotherSize)
{
    const Frame frame = squareFrame(0);
    const Frame small(64, 48, [](double, double) { return 0.f; });
    const FlowFrame f = frameOf(frame);
    const FlowFrame s = frameOf(small);
    FlowEstimateResult result;
    EXPECT_EQ(estimateFlow(nullptr, f, &s, FlowEstimateParams(), FlowContext(), result), eFlowEstimateFormatMismatch);
    EXPECT_EQ(result.mismatchOffset, 1);
    EXPECT_EQ(estimateFlow(&s, f, nullptr, FlowEstimateParams(), FlowContext(), result), eFlowEstimateFormatMismatch);
    EXPECT_EQ(result.mismatchOffset, -1);
}

TEST(FlowConfidence, AbortIsReported)
{
    const Frame frame = squareFrame(0);
    const FlowFrame f = frameOf(frame);
    FlowContext context;
    context.aborted = []() { return true; };
    FlowEstimateResult result;
    EXPECT_EQ(estimateFlow(&f, f, &f, FlowEstimateParams(), context, result), eFlowEstimateAborted);
}
