/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "Global/Macros.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/VectorFollow.h"

NATRON_NAMESPACE_ENTER

namespace {

// Each frame rotates about the origin by exactly kAngle, the vectors being the chord to the next
// and previous position.
class RotationField
    : public VectorField {
public:
    explicit RotationField(double angle)
        : _cos(std::cos(angle))
        , _sin(std::sin(angle))
    {
    }

    virtual bool prepareFrame(int /*frame*/) OVERRIDE FINAL
    {
        return true;
    }

    virtual VectorSample sample(int /*frame*/,
                                double x,
                                double y) OVERRIDE FINAL
    {
        VectorSample s;
        s.forwardX = _cos * x - _sin * y - x;
        s.forwardY = _sin * x + _cos * y - y;
        s.backwardX = _cos * x + _sin * y - x;
        s.backwardY = -_sin * x + _cos * y - y;

        return s;
    }

private:
    const double _cos;
    const double _sin;
};

// A picture standing still, read through an estimate with a constant forward error.
class BiasedForwardField
    : public VectorField {
public:
    explicit BiasedForwardField(double bias)
        : _bias(bias)
    {
    }

    virtual bool prepareFrame(int /*frame*/) OVERRIDE FINAL
    {
        return true;
    }

    virtual VectorSample sample(int /*frame*/,
                                double /*x*/,
                                double /*y*/) OVERRIDE FINAL
    {
        VectorSample s;
        s.forwardX = _bias;

        return s;
    }

private:
    const double _bias;
};

class ConstantField
    : public VectorField {
public:
    ConstantField(double dx,
                  double dy)
        : _dx(dx)
        , _dy(dy)
    {
    }

    int unreadableFrom = 1 << 30;
    int lowConfidenceFrom = 1 << 30;

    virtual bool prepareFrame(int frame) OVERRIDE FINAL
    {
        return frame < unreadableFrom;
    }

    virtual VectorSample sample(int frame,
                                double /*x*/,
                                double /*y*/) OVERRIDE FINAL
    {
        VectorSample s;
        s.forwardX = _dx;
        s.forwardY = _dy;
        s.backwardX = -_dx;
        s.backwardY = -_dy;
        if (frame >= lowConfidenceFrom) {
            s.forwardConfidence = 0.;
            s.backwardConfidence = 0.;
        }

        return s;
    }

private:
    const double _dx;
    const double _dy;
};

std::vector<FollowPoint>
squarePoints()
{
    std::vector<FollowPoint> points;
    points.push_back(FollowPoint(10., 10.));
    points.push_back(FollowPoint(30., 10.));
    points.push_back(FollowPoint(30., 30.));
    points.push_back(FollowPoint(10., 30.));

    return points;
}

VectorFollowShapeSetup
perPointSetup(int first,
              int last,
              const std::vector<int>& anchorFrames,
              double dx,
              double dy)
{
    VectorFollowShapeSetup setup;
    setup.first = first;
    setup.last = last;
    for (int frame : anchorFrames) {
        VectorFollowAnchor anchor;
        anchor.frame = frame;
        for (const FollowPoint& p : squarePoints()) {
            anchor.points.push_back(FollowPoint(p.x + dx * frame, p.y + dy * frame));
        }
        anchor.samples = anchor.points;
        setup.anchors.push_back(anchor);
    }

    return setup;
}

std::vector<VectorFollowFrame>
runFollow(VectorField& field,
          const VectorFollowShapeSetup& setup,
          VectorFollowShapeOutcome* outcome = nullptr)
{
    std::vector<VectorFollowFrame> frames;
    const VectorFollowShapeOutcome result = followVectorShape(
        field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });
    if (outcome) {
        *outcome = result;
    }

    return frames;
}

const VectorFollowFrame*
frameOf(const std::vector<VectorFollowFrame>& frames,
        int frame)
{
    for (const VectorFollowFrame& f : frames) {
        if (f.frame == frame) {
            return &f;
        }
    }

    return nullptr;
}

} // anonymous namespace

TEST(VectorFollow, MatrixInverseAndCompose)
{
    FollowMatrix a;
    a.m[0] = 2.;
    a.m[1] = 0.5;
    a.m[2] = 3.;
    a.m[3] = -0.25;
    a.m[4] = 1.5;
    a.m[5] = -7.;
    a.m[6] = 0.001;
    a.m[7] = 0.002;
    FollowMatrix inverse;
    ASSERT_TRUE(a.inverted(&inverse));
    const FollowMatrix identity = a * inverse;
    const FollowMatrix expected;
    for (int i = 0; i < 9; ++i) {
        EXPECT_NEAR(identity.m[i], expected.m[i], 1e-12);
    }
    FollowMatrix singular;
    singular.m[0] = 0.;
    singular.m[1] = 0.;
    singular.m[2] = 0.;
    EXPECT_FALSE(singular.inverted(&inverse));
}

TEST(VectorFollow, AnalyticRotationStaysOnItsCircle)
{
    const double total = M_PI / 4.;
    const int frames = 50;
    RotationField field(total / frames);

    const FollowPoint start(100., 40.);
    FollowTrajectory trajectory;
    ASSERT_TRUE(integrateFollowPoints(field, std::vector<FollowPoint>(1, start), 0, frames, &trajectory));

    const FollowPoint end = trajectory.pointsAt(frames)[0];
    const double expectedX = std::cos(total) * start.x - std::sin(total) * start.y;
    const double expectedY = std::sin(total) * start.x + std::cos(total) * start.y;
    EXPECT_NEAR(end.x, expectedX, 0.25);
    EXPECT_NEAR(end.y, expectedY, 0.25);
}

TEST(VectorFollow, BackwardIntegrationGoesDown)
{
    ConstantField field(2., 1.);
    FollowTrajectory trajectory;
    ASSERT_TRUE(integrateFollowPoints(field, std::vector<FollowPoint>(1, FollowPoint(50., 50.)), 10, 4, &trajectory));
    EXPECT_EQ(trajectory.firstFrame, 4);
    EXPECT_DOUBLE_EQ(trajectory.pointsAt(4)[0].x, 38.);
    EXPECT_DOUBLE_EQ(trajectory.pointsAt(4)[0].y, 44.);
    EXPECT_DOUBLE_EQ(trajectory.pointsAt(10)[0].x, 50.);
}

TEST(VectorFollow, BridgeWeightIsASmoothstep)
{
    EXPECT_DOUBLE_EQ(followBridgeWeight(0, 0, 10), 0.);
    EXPECT_DOUBLE_EQ(followBridgeWeight(5, 0, 10), 0.5);
    EXPECT_DOUBLE_EQ(followBridgeWeight(10, 0, 10), 1.);
    EXPECT_LT(followBridgeWeight(1, 0, 10), 0.1);
    EXPECT_GT(followBridgeWeight(9, 0, 10), 0.9);
}

TEST(VectorFollow, BridgeLandsOnBothAnchorsAndHalvesTheMidGapError)
{
    const double bias = 0.5;
    const int gap = 20;
    BiasedForwardField field(bias);

    const std::vector<FollowPoint> anchor(1, FollowPoint(50., 50.));

    FollowTrajectory oneWay;
    ASSERT_TRUE(integrateFollowPoints(field, anchor, 0, gap, &oneWay));
    const double oneWayError = std::fabs(oneWay.pointsAt(gap / 2)[0].x - 50.);
    EXPECT_NEAR(oneWayError, bias * gap / 2, 1e-12);

    FollowTrajectory bridge;
    ASSERT_TRUE(bridgeFollowPoints(field, anchor, anchor, 0, gap, &bridge));
    EXPECT_DOUBLE_EQ(bridge.pointsAt(0)[0].x, anchor[0].x);
    EXPECT_DOUBLE_EQ(bridge.pointsAt(0)[0].y, anchor[0].y);
    EXPECT_DOUBLE_EQ(bridge.pointsAt(gap)[0].x, anchor[0].x);
    EXPECT_DOUBLE_EQ(bridge.pointsAt(gap)[0].y, anchor[0].y);

    const double bridgeError = std::fabs(bridge.pointsAt(gap / 2)[0].x - 50.);
    EXPECT_NEAR(bridgeError, oneWayError / 2., 1e-12);
}

TEST(VectorFollow, ShapeSeedFollowsForwardAndEmitsTheSeed)
{
    ConstantField field(1., 0.5);
    VectorFollowShapeSetup setup = perPointSetup(0, 5, std::vector<int>(1, 0), 1., 0.5);
    setup.seedIsNew = true;

    VectorFollowShapeOutcome outcome;
    const std::vector<VectorFollowFrame> frames = runFollow(field, setup, &outcome);
    ASSERT_EQ(frames.size(), 6u);
    EXPECT_EQ(frames.front().frame, 0);
    EXPECT_EQ(frames.back().frame, 5);
    EXPECT_DOUBLE_EQ(frames.back().points[0].x, 10. + 5.);
    EXPECT_DOUBLE_EQ(frames.back().points[0].y, 10. + 2.5);
    EXPECT_FALSE(outcome.stopped);
    EXPECT_TRUE(outcome.inferred.empty());
}

TEST(VectorFollow, ExistingAnchorIsNotEmittedAndEveryFrameOnceAroundIt)
{
    ConstantField field(1., 0.);
    const VectorFollowShapeSetup setup = perPointSetup(0, 6, std::vector<int>(1, 3), 1., 0.);

    const std::vector<VectorFollowFrame> frames = runFollow(field, setup);
    EXPECT_EQ(frames.size(), 6u);
    EXPECT_EQ(frameOf(frames, 3), nullptr);
    ASSERT_NE(frameOf(frames, 0), nullptr);
    ASSERT_NE(frameOf(frames, 6), nullptr);
    EXPECT_DOUBLE_EQ(frameOf(frames, 0)->points[0].x, 10.);
    EXPECT_DOUBLE_EQ(frameOf(frames, 6)->points[0].x, 16.);
}

TEST(VectorFollow, ShapeBridgeBetweenKeysBakesInTheDirectionOfTheFollow)
{
    ConstantField field(1., 0.);
    std::vector<int> anchorFrames;
    anchorFrames.push_back(2);
    anchorFrames.push_back(8);

    VectorFollowShapeSetup forward = perPointSetup(2, 8, anchorFrames, 1., 0.);
    std::vector<VectorFollowFrame> frames = runFollow(field, forward);
    ASSERT_EQ(frames.size(), 5u);
    EXPECT_EQ(frames.front().frame, 3);
    EXPECT_EQ(frames.back().frame, 7);
    EXPECT_DOUBLE_EQ(frames[2].points[0].x, 10. + 5.);

    VectorFollowShapeSetup backward = forward;
    backward.direction = VectorFollowDirection::eBackward;
    frames = runFollow(field, backward);
    ASSERT_EQ(frames.size(), 5u);
    EXPECT_EQ(frames.front().frame, 7);
    EXPECT_EQ(frames.back().frame, 3);
}

TEST(VectorFollow, StopRuleHoldsTheLastStateAndReportsTheFrame)
{
    ConstantField field(1., 0.);
    field.lowConfidenceFrom = 3;
    VectorFollowShapeSetup setup = perPointSetup(0, 10, std::vector<int>(1, 0), 1., 0.);
    setup.seedIsNew = true;

    VectorFollowShapeOutcome outcome;
    const std::vector<VectorFollowFrame> frames = runFollow(field, setup, &outcome);
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(outcome.stopFrame, 3);
    EXPECT_EQ(frames.back().frame, 3);
    EXPECT_DOUBLE_EQ(frames.back().points[0].x, 13.);
}

TEST(VectorFollow, AnAbortKeepsOnlyWhatWasEmitted)
{
    ConstantField field(1., 0.);
    VectorFollowShapeSetup setup = perPointSetup(0, 10, std::vector<int>(1, 0), 1., 0.);
    setup.seedIsNew = true;

    int checks = 0;
    std::vector<VectorFollowFrame> frames;
    const VectorFollowShapeOutcome outcome = followVectorShape(
        field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, [&checks]() { return ++checks > 4; });
    EXPECT_TRUE(outcome.aborted);
    EXPECT_FALSE(outcome.stopped);
    EXPECT_EQ(frames.back().frame, 4);
}

TEST(VectorFollow, AnUnreadableFrameEndsTheFollowAsAnAbort)
{
    ConstantField field(1., 0.);
    field.unreadableFrom = 2;
    VectorFollowShapeSetup setup = perPointSetup(0, 10, std::vector<int>(1, 0), 1., 0.);

    VectorFollowShapeOutcome outcome;
    const std::vector<VectorFollowFrame> frames = runFollow(field, setup, &outcome);
    EXPECT_TRUE(outcome.aborted);
    EXPECT_EQ(frames.back().frame, 2);
}

TEST(VectorFollow, ReportNamesInferredRangesAndTheStop)
{
    VectorFollowReport report;
    VectorFollowShapeReport shape;
    shape.name = "Bezier1";
    VectorFollowFrameRange a;
    a.first = 4;
    a.last = 7;
    VectorFollowFrameRange b;
    b.first = 10;
    b.last = 10;
    shape.inferred.push_back(a);
    shape.inferred.push_back(b);
    shape.stopped = true;
    shape.stopFrame = 12;
    report.shapes.push_back(shape);

    const std::string text = formatVectorFollowReport(report);
    EXPECT_NE(text.find("Bezier1"), std::string::npos);
    EXPECT_NE(text.find("4-7, 10"), std::string::npos);
    EXPECT_NE(text.find("stopped at frame 12"), std::string::npos);
}

NATRON_NAMESPACE_EXIT
