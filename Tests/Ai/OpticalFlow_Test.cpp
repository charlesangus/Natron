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

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/OpticalFlow.h"
#include "Tests/Ai/AiTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

// flow-constant.onnx's flow, from tools/ai/make-test-models.py.
const float kFlowDx = 2.5f;
const float kFlowDy = -1.5f;

typedef std::vector<std::vector<int64_t>> Shapes;

std::shared_ptr<InferenceSession>
openFlow()
{
    return InferenceSession::open(AiTest::modelPath("flow-constant.onnx"), AiTest::cpuOptions(), AiTest::loader());
}

OpticalFlowFrame
makeFrame(int width,
          int height)
{
    OpticalFlowFrame frame;

    for (int c = 0; c < 3; ++c) {
        frame.channel[c] = FlowPlane(width, height, 0.25f * (float)(c + 1));
    }

    return frame;
}

// Records the shape of every run's first input.
class ShapeRecorder {
public:
    ShapeRecorder()
        : _guard([this](const InferenceSession&, const Shapes& shapes) {
            _runs.push_back(shapes.at(0));

            return std::string();
        })
    {
    }

    const Shapes& runs() const
    {
        return _runs;
    }

private:
    Shapes _runs;
    AiTest::InjectorGuard _guard;
};

void
expectField(const FlowField& field,
            int width,
            int height,
            float dx,
            float dy)
{
    ASSERT_EQ(width, field.width);
    ASSERT_EQ(height, field.height);
    ASSERT_EQ(width, field.u.width);
    ASSERT_EQ(height, field.v.height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            ASSERT_NEAR(dx, field.u.at(x, y), 1e-4f) << x << "," << y;
            ASSERT_NEAR(dy, field.v.at(x, y), 1e-4f) << x << "," << y;
        }
    }
}

class AiOpticalFlow
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(AiTest::loader()->available()) << AiTest::loader()->unavailableReason();
    }
};

} // namespace

TEST(AiOpticalFlowRange, WrapIsOddMonotoneAndInvertible)
{
    EXPECT_FLOAT_EQ(0.5f, opticalFlowWrapRange(0.5f));
    EXPECT_FLOAT_EQ(-1.f, opticalFlowWrapRange(-1.f));
    float previous = opticalFlowWrapRange(-1e4f);
    for (float v : { -100.f, -2.f, -0.3f, 0.f, 0.7f, 1.f, 1.5f, 10.f, 1e4f }) {
        const float wrapped = opticalFlowWrapRange(v);
        EXPECT_GT(wrapped, previous);
        previous = wrapped;
        EXPECT_NEAR(v, opticalFlowUnwrapRange(wrapped), std::fabs(v) * 1e-5f + 1e-6f);
        EXPECT_FLOAT_EQ(-wrapped, opticalFlowWrapRange(-v));
    }
}

TEST(AiOpticalFlowRange, GeometryDownscalesThenPads)
{
    OpticalFlowSpec spec;
    spec.maxInferenceSize = 1024;

    OpticalFlowGeometry geo = opticalFlowGeometry(2048, 1000, spec);
    EXPECT_EQ(1024, geo.inferenceWidth);
    EXPECT_EQ(500, geo.inferenceHeight);
    EXPECT_EQ(1024, geo.paddedWidth);
    EXPECT_EQ(504, geo.paddedHeight);

    geo = opticalFlowGeometry(1920, 1080, spec);
    EXPECT_EQ(1024, geo.inferenceWidth);
    EXPECT_EQ(576, geo.inferenceHeight);

    geo = opticalFlowGeometry(1001, 513, spec);
    EXPECT_EQ(1001, geo.inferenceWidth);
    EXPECT_EQ(1008, geo.paddedWidth);
    EXPECT_EQ(520, geo.paddedHeight);

    geo = opticalFlowGeometry(20, 12, spec);
    EXPECT_EQ(20, geo.inferenceWidth);
    EXPECT_EQ(128, geo.paddedWidth);
    EXPECT_EQ(128, geo.paddedHeight);

    spec.maxInferenceSize = 0;
    geo = opticalFlowGeometry(4000, 3000, spec);
    EXPECT_EQ(4000, geo.inferenceWidth);
}

TEST_F(AiOpticalFlow, AConstantFlowComesBackExactlyAtFullSize)
{
    OpticalFlowSpec spec;
    spec.minSize = 8;
    ShapeRecorder recorder;

    const FlowField field = estimateOpticalFlow(openFlow(), makeFrame(64, 40), makeFrame(64, 40), spec);

    expectField(field, 64, 40, kFlowDx, kFlowDy);
    ASSERT_EQ(1u, recorder.runs().size());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 40, 64 }), recorder.runs()[0]);
}

TEST_F(AiOpticalFlow, PaddingIsCroppedOff)
{
    OpticalFlowSpec spec;
    spec.minSize = 8;
    ShapeRecorder recorder;

    const FlowField field = estimateOpticalFlow(openFlow(), makeFrame(37, 21), makeFrame(37, 21), spec);

    expectField(field, 37, 21, kFlowDx, kFlowDy);
    ASSERT_EQ(1u, recorder.runs().size());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 24, 40 }), recorder.runs()[0]);
}

TEST_F(AiOpticalFlow, FramesBelowTheModelMinimumArePadded)
{
    OpticalFlowSpec spec;
    spec.minSize = 128;
    ShapeRecorder recorder;

    const FlowField field = estimateOpticalFlow(openFlow(), makeFrame(20, 12), makeFrame(20, 12), spec);

    expectField(field, 20, 12, kFlowDx, kFlowDy);
    ASSERT_EQ(1u, recorder.runs().size());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 128, 128 }), recorder.runs()[0]);
}

TEST_F(AiOpticalFlow, DownscaledVectorsAreRescaledToFullScalePixels)
{
    OpticalFlowSpec spec;
    spec.maxInferenceSize = 100;
    spec.minSize = 8;
    ShapeRecorder recorder;

    const FlowField field = estimateOpticalFlow(openFlow(), makeFrame(400, 200), makeFrame(400, 200), spec);

    expectField(field, 400, 200, kFlowDx * 4.f, kFlowDy * 4.f);
    ASSERT_EQ(1u, recorder.runs().size());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 56, 104 }), recorder.runs()[0]);
}

TEST_F(AiOpticalFlow, AnUnevenDownscaleRescalesEachAxisByItsOwnFactor)
{
    OpticalFlowSpec spec;
    spec.maxInferenceSize = 100;
    spec.minSize = 8;

    const FlowField field = estimateOpticalFlow(openFlow(), makeFrame(300, 100), makeFrame(300, 100), spec);
    const OpticalFlowGeometry geo = opticalFlowGeometry(300, 100, spec);

    ASSERT_EQ(100, geo.inferenceWidth);
    ASSERT_EQ(33, geo.inferenceHeight);
    expectField(field, 300, 100, kFlowDx * 3.f, kFlowDy * 100.f / 33.f);
}

TEST_F(AiOpticalFlow, ThePairRunsTwiceAndBothDirectionsReturn)
{
    OpticalFlowSpec spec;
    spec.minSize = 8;
    ShapeRecorder recorder;

    const OpticalFlowPair pair = estimateOpticalFlowPair(openFlow(), makeFrame(48, 32), makeFrame(48, 32), spec);

    expectField(pair.forward, 48, 32, kFlowDx, kFlowDy);
    expectField(pair.backward, 48, 32, kFlowDx, kFlowDy);
    EXPECT_EQ(2u, recorder.runs().size());
}

TEST_F(AiOpticalFlow, FramesOfDifferentSizesAreRefused)
{
    EXPECT_THROW(estimateOpticalFlow(openFlow(), makeFrame(32, 32), makeFrame(32, 16), OpticalFlowSpec()), AiError);
}

TEST_F(AiOpticalFlow, AnAbortBeforeTheRunThrowsAndRunsNothing)
{
    ShapeRecorder recorder;

    EXPECT_THROW(estimateOpticalFlowPair(openFlow(), makeFrame(32, 32), makeFrame(32, 32), OpticalFlowSpec(), []() { return true; }), AiAborted);
    EXPECT_TRUE(recorder.runs().empty());
}

TEST_F(AiOpticalFlow, AnAbortDuringTheFirstRunSkipsTheBackwardRun)
{
    std::atomic<bool> abort(false);
    size_t runs = 0;
    AiTest::InjectorGuard guard([&abort, &runs](const InferenceSession&, const Shapes&) {
        ++runs;
        abort = true;

        return std::string();
    });

    EXPECT_THROW(estimateOpticalFlowPair(openFlow(), makeFrame(32, 32), makeFrame(32, 32), OpticalFlowSpec(), [&abort]() { return abort.load(); }), AiAborted);
    EXPECT_EQ(1u, runs);
}
