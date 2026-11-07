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
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Flow/FlowEstimate.h"
#include "Engine/Flow/FlowTemporal.h"
#include "Tests/Flow/FlowTestPlate.h"

NATRON_NAMESPACE_USING
using namespace FlowTest;

namespace {
const int kWidth = 128;
const int kHeight = 96;
const int kFrames = 9;
const int kMargin = 10;
const float kSpeedX = 2.5f;
const float kSpeedY = -1.25f;

std::vector<Frame>
noisySequence(float noise)
{
    const NoiseTexture texture(77);
    std::mt19937 engine(4242);
    std::vector<Frame> frames;
    for (int t = 0; t < kFrames; ++t) {
        Frame frame = translatedFrame(texture, kWidth, kHeight, kSpeedX * t, kSpeedY * t);
        for (float& value : frame.pixels) {
            value += noise * (static_cast<float>(engine()) / 4294967296.f - 0.5f);
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

FlowFrame
frameOf(const Frame& frame)
{
    FlowFrame result;
    result.image = frame.view();
    return result;
}

FlowField
constantField(float u,
              float v,
              float confidence)
{
    FlowField field(8, 8);
    std::fill(field.u.data.begin(), field.u.data.end(), u);
    std::fill(field.v.data.begin(), field.v.data.end(), v);
    std::fill(field.confidence.data.begin(), field.confidence.data.end(), confidence);
    return field;
}

/// The forward field of every frame with two frames either side.
std::vector<FlowField>
forwardFields(const std::vector<Frame>& sequence,
              float strength)
{
    std::vector<FlowField> fields;
    for (int t = 2; t < kFrames - 2; ++t) {
        std::vector<FlowFrame> frames;
        for (int k = -2; k <= 2; ++k) {
            frames.push_back(frameOf(sequence[t + k]));
        }
        const FlowFrame* const pointers[] = { &frames[0], &frames[1], &frames[2], &frames[3], &frames[4] };
        FlowEstimateResult result;
        EXPECT_EQ(estimateFlowSmoothed(pointers, strength, FlowEstimateParams(), FlowContext(), result), eFlowEstimateOK);
        fields.push_back(result.forward);
    }
    return fields;
}

double
meanError(const std::vector<FlowField>& fields)
{
    double sum = 0.;
    for (const FlowField& field : fields) {
        sum += meanEndpointError(field, translation(kSpeedX, kSpeedY), kMargin);
    }
    return sum / static_cast<double>(fields.size());
}

/// Each pixel's vector variance over the frames, averaged over the pixels.
double
frameToFrameVariance(const std::vector<FlowField>& fields)
{
    const double count = static_cast<double>(fields.size());
    double total = 0.;
    long pixels = 0;
    for (int y = kMargin; y < kHeight - kMargin; ++y) {
        for (int x = kMargin; x < kWidth - kMargin; ++x) {
            double sumU = 0., sumV = 0., sumSquares = 0.;
            for (const FlowField& field : fields) {
                const double u = field.u.at(x, y);
                const double v = field.v.at(x, y);
                sumU += u;
                sumV += v;
                sumSquares += u * u + v * v;
            }
            total += sumSquares / count - (sumU * sumU + sumV * sumV) / (count * count);
            ++pixels;
        }
    }
    return total / static_cast<double>(pixels);
}
} // anonymous namespace

TEST(FlowTemporal, ConstantVelocityVectorsSteadyWithoutLosingAccuracy)
{
    const std::vector<Frame> sequence = noisySequence(0.03f);
    const std::vector<FlowField> raw = forwardFields(sequence, 0.f);
    const std::vector<FlowField> smoothed = forwardFields(sequence, 1.f);
    // Measured: the variance falls from 0.00120 to 0.00019, a ratio of 0.16.
    EXPECT_LT(frameToFrameVariance(smoothed), 0.4 * frameToFrameVariance(raw));
    EXPECT_LE(meanError(smoothed), meanError(raw));
}

TEST(FlowTemporal, StrengthZeroGivesTheCurrentField)
{
    const FlowField current = constantField(1.f, 2.f, 0.5f);
    const FlowField neighbour = constantField(5.f, 6.f, 1.f);
    const FlowField through = constantField(0.f, 0.f, 1.f);
    const FlowTemporalNeighbour neighbours[] = { { &neighbour, &through } };
    FlowField out;
    flowSmoothTemporal(current, neighbours, 1, 0.f, FlowContext(), out);
    EXPECT_EQ(out.u.data, current.u.data);
    EXPECT_EQ(out.v.data, current.v.data);
}

TEST(FlowTemporal, ConfidentFieldsCountEquallyAtFullStrength)
{
    const FlowField current = constantField(3.f, 0.f, 1.f);
    const FlowField earlier = constantField(0.f, 3.f, 1.f);
    const FlowField later = constantField(6.f, 3.f, 1.f);
    const FlowField through = constantField(0.f, 0.f, 1.f);
    const FlowTemporalNeighbour neighbours[] = { { &earlier, &through }, { &later, &through } };
    FlowField out;
    flowSmoothTemporal(current, neighbours, 2, 1.f, FlowContext(), out);
    EXPECT_FLOAT_EQ(out.u.at(4, 4), 3.f);
    EXPECT_FLOAT_EQ(out.v.at(4, 4), 2.f);
    EXPECT_FLOAT_EQ(out.confidence.at(4, 4), 1.f);
}

TEST(FlowTemporal, UnconfidentNeighboursAndCarriersCountForNothing)
{
    const FlowField current = constantField(1.f, 2.f, 1.f);
    const FlowField unconfident = constantField(50.f, 50.f, 0.f);
    const FlowField confident = constantField(50.f, 50.f, 1.f);
    const FlowField through = constantField(0.f, 0.f, 1.f);
    const FlowField badThrough = constantField(0.f, 0.f, 0.f);
    const FlowTemporalNeighbour neighbours[] = { { &unconfident, &through }, { &confident, &badThrough } };
    FlowField out;
    flowSmoothTemporal(current, neighbours, 2, 1.f, FlowContext(), out);
    EXPECT_FLOAT_EQ(out.u.at(4, 4), 1.f);
    EXPECT_FLOAT_EQ(out.v.at(4, 4), 2.f);
}

TEST(FlowTemporal, NeighboursAreCarriedAlongTheFlow)
{
    const FlowField current = constantField(0.f, 0.f, 1.f);
    FlowField neighbour = constantField(0.f, 0.f, 1.f);
    neighbour.u.at(6, 4) = 9.f;
    const FlowField through = constantField(2.f, 0.f, 1.f);
    const FlowTemporalNeighbour neighbours[] = { { &neighbour, &through } };
    FlowField out;
    flowSmoothTemporal(current, neighbours, 1, 1.f, FlowContext(), out);
    EXPECT_FLOAT_EQ(out.u.at(4, 4), 4.5f);
    EXPECT_FLOAT_EQ(out.u.at(6, 4), 0.f);
}

TEST(FlowTemporal, EdgeOfTheSequenceLeavesTheMissingDirectionEmpty)
{
    const std::vector<Frame> sequence = noisySequence(0.f);
    const FlowFrame current = frameOf(sequence[0]);
    const FlowFrame next = frameOf(sequence[1]);
    const FlowFrame afterNext = frameOf(sequence[2]);
    const FlowFrame* const pointers[] = { nullptr, nullptr, &current, &next, &afterNext };
    FlowEstimateResult result;
    ASSERT_EQ(estimateFlowSmoothed(pointers, 1.f, FlowEstimateParams(), FlowContext(), result), eFlowEstimateOK);
    for (std::size_t i = 0; i < result.backward.u.data.size(); ++i) {
        ASSERT_EQ(result.backward.u.data[i], 0.f);
        ASSERT_EQ(result.backward.v.data[i], 0.f);
        ASSERT_EQ(result.backward.confidence.data[i], 0.f);
    }
    EXPECT_LT(meanEndpointError(result.forward, translation(kSpeedX, kSpeedY), kMargin), 0.1);
}

TEST(FlowTemporal, ReportsAMismatchedFrame)
{
    const std::vector<Frame> sequence = noisySequence(0.f);
    const Frame other(64, 48, [](double, double) { return 0.f; });
    const FlowFrame current = frameOf(sequence[0]);
    const FlowFrame wrong = frameOf(other);
    const FlowFrame* const pointers[] = { &wrong, nullptr, &current, nullptr, nullptr };
    FlowEstimateResult result;
    EXPECT_EQ(estimateFlowSmoothed(pointers, 1.f, FlowEstimateParams(), FlowContext(), result), eFlowEstimateFormatMismatch);
    EXPECT_EQ(result.mismatchOffset, -2);
}

TEST(FlowTemporal, AbortStopsTheEstimate)
{
    const std::vector<Frame> sequence = noisySequence(0.f);
    const FlowFrame current = frameOf(sequence[0]);
    const FlowFrame next = frameOf(sequence[1]);
    const FlowFrame* const pointers[] = { nullptr, nullptr, &current, &next, nullptr };
    FlowContext context;
    context.aborted = []() { return true; };
    FlowEstimateResult result;
    EXPECT_EQ(estimateFlowSmoothed(pointers, 1.f, FlowEstimateParams(), context, result), eFlowEstimateAborted);
}
