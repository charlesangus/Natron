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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/VectorFollowTestSupport.h"

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Transform.h"
#include "Engine/UndoCommand.h"
#include "Engine/VectorFollowBake.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace VectorFollowTest;

constexpr double kFollowTolerance = 0.5;
constexpr int kStart = 5;
constexpr int kSpan = 20;
// A key before kStart that holds the same pose as the shape's other key, so a follow from kStart
// starts from what kStart shows.
constexpr int kEarlier = kStart - 2;

class VectorFollowBakeTest
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
        _generator = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1));
        // Medium solves at half resolution and undershoots this half-pixel motion by 0.03 px a
        // frame, more than kFollowTolerance by frame +20. These tests measure the follower.
        setKnob<KnobChoice>(_generator, kVectorGeneratorParamQuality, (int)eFlowQualityFine);
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    }

    RotoCurveContextPtr context() const
    {
        return _roto->getCurveContext();
    }

    BezierPtr makeSquare(double time) const
    {
        BezierPtr bezier = context()->makeBezier(90., 70., kRotoBezierBaseName, time, false);
        bezier->addControlPoint(130., 70., time);
        bezier->addControlPoint(130., 110., time);
        bezier->addControlPoint(90., 110., time);
        bezier->setCurveFinished(true);

        return bezier;
    }

    VectorFollowParams paramsFor(const RotoDrawableItemPtr& item,
                                 VectorFollowModel model) const
    {
        VectorFollowParams params;
        params.items.push_back(item);
        params.source = _generator;
        params.first = kStart;
        params.last = kStart + kSpan;
        params.model = model;

        return params;
    }

    static FollowPoint positionAt(const BezierPtr& bezier,
                                  int index,
                                  double time)
    {
        FollowPoint p;
        bezier->getControlPointAtIndex(index)->getPositionAtTime(false, time, ViewIdx(0), &p.x, &p.y);

        return p;
    }

    static FollowPoint shownAt(const RotoDrawableItemPtr& item,
                               const FollowPoint& p,
                               double time)
    {
        Transform::Matrix3x3 m;
        item->getTransformAtTime(time, &m);
        Transform::Point3D out(p.x, p.y, 1.);
        out = Transform::matApply(m, out);

        return FollowPoint(out.x / out.z, out.y / out.z);
    }

    NodePtr _generator;
    NodePtr _roto;
};

TEST_F(VectorFollowBakeTest, PerPointFollowsAndLandsOnTheVectors)
{
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);

    const FollowPoint end = positionAt(bezier, 0, kStart + kSpan);
    EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance);

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
}

TEST_F(VectorFollowBakeTest, UndoPutsBackThePointsAndRedoTheFollow)
{
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 2, kStart + kSpan);

    UndoCommandPtr undo;
    context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(bool(undo));
    const FollowPoint followed = positionAt(bezier, 2, kStart + kSpan);

    undo->undo();
    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 2, kStart + kSpan).x, start.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 2, kStart + kSpan).y, start.y);

    undo->redo();
    EXPECT_DOUBLE_EQ(positionAt(bezier, 2, kStart + kSpan).x, followed.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 2, kStart + kSpan).y, followed.y);
}

TEST_F(VectorFollowBakeTest, AnExistingKeyIsAnAnchorAndStaysExact)
{
    BezierPtr bezier = makeSquare(kStart);
    const int later = kStart + 10;
    bezier->setKeyframe(later);
    const FollowPoint atStart = positionAt(bezier, 1, kStart);
    const FollowPoint atLater = positionAt(bezier, 1, later);

    UndoCommandPtr undo;
    context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);

    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).x, atStart.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).y, atStart.y);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, later).x, atLater.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, later).y, atLater.y);
}

TEST_F(VectorFollowBakeTest, SelectedPointsMoveAndTheRestStaysPut)
{
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint moved = positionAt(bezier, 0, kStart);
    const FollowPoint still = positionAt(bezier, 2, kStart);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::eTranslate);
    params.selectedPoints[bezier.get()].insert(0);
    UndoCommandPtr undo;
    context()->followVectors(params, &undo);

    EXPECT_NEAR(positionAt(bezier, 0, kStart + kSpan).x, moved.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 2, kStart + kSpan).x, still.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 2, kStart + kSpan).y, still.y);
}

TEST_F(VectorFollowBakeTest, FitsKeyTheTransformAndLeaveThePointKeysAlone)
{
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine }) {
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(paramsFor(bezier, model), &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;

        std::set<double> keys;
        bezier->getKeyframeTimes(&keys);
        EXPECT_EQ(keys.size(), 1u);

        const FollowPoint end = shownAt(bezier, start, kStart + kSpan);
        EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance);
        EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance);

        undo->undo();
        const FollowPoint back = shownAt(bezier, start, kStart + kSpan);
        EXPECT_NEAR(back.x, start.x, 1e-9);
        EXPECT_NEAR(back.y, start.y, 1e-9);
    }
}

TEST_F(VectorFollowBakeTest, AStrokeFollowsByItsTransform)
{
    // Detached and without internal nodes: only its points and transform take part.
    RotoStrokeItemPtr stroke = std::make_shared<RotoStrokeItem>(eRotoStrokeTypeSolid, context(), std::string("stroke"), RotoLayerPtr());
    CurvePtr xCurve = std::make_shared<Curve>();
    CurvePtr yCurve = std::make_shared<Curve>();
    CurvePtr pressureCurve = std::make_shared<Curve>();
    for (int i = 0; i < 12; ++i) {
        xCurve->addKeyFrame(KeyFrame(i, 90. + 4. * i));
        yCurve->addKeyFrame(KeyFrame(i, 70. + 3. * i));
        pressureCurve->addKeyFrame(KeyFrame(i, 1.));
    }
    stroke->addStroke(xCurve, yCurve, pressureCurve);

    const FollowPoint start(110., 90.);
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(stroke, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;

    const FollowPoint end = shownAt(stroke, start, kStart + kSpan);
    EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance);
}

TEST_F(VectorFollowBakeTest, AnAbortKeepsTheFramesAlreadyDone)
{
    BezierPtr bezier = makeSquare(kStart);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    int calls = 0;
    params.shouldAbort = [&calls]() { return ++calls > 6; };
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    EXPECT_TRUE(report.aborted);

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_GT(keys.size(), 1u);
    EXPECT_LT(keys.size(), (std::size_t)kSpan + 1);
    for (double t = kStart; t <= *keys.rbegin(); t += 1.) {
        EXPECT_EQ(keys.count(t), 1u) << t;
    }

    ASSERT_TRUE(bool(undo));
    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);
}

TEST_F(VectorFollowBakeTest, ReducesInsideTheSameUndoStep)
{
    BezierPtr bezier = makeSquare(kStart);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.reduce = true;
    params.tolerance = 0.5;
    UndoCommandPtr undo;
    context()->followVectors(params, &undo);

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_LT(keys.size(), (std::size_t)kSpan);
    EXPECT_EQ(keys.count(kStart), 1u);

    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);
}

TEST_F(VectorFollowBakeTest, AForwardFollowWithALaterKeyMeetsBothAndBlendsBetween)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kEarlier);
    const FollowPoint held = positionAt(bezier, 1, later);
    const FollowPoint shown = positionAt(bezier, 1, kStart);
    EXPECT_NEAR(shown.x, held.x, 1e-9);
    EXPECT_NEAR(shown.y, held.y, 1e-9);

    UndoCommandPtr undo;
    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = later;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_TRUE(report.shapes[0].passStops.empty());

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 12u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).x, shown.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).y, shown.y);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, later).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, later).y, held.y);

    // Both anchors hold the same pose on a moving picture, so at each frame one pass is ahead by the
    // frames since the start and the other behind by the frames to the key.
    for (int frame : { kStart + 3, kStart + 5, kStart + 8 }) {
        const double weight = followBridgeWeight(frame, kStart, later);
        const double offset = (frame - kStart) - weight * 10.;
        EXPECT_NEAR(positionAt(bezier, 1, frame).x, held.x + kShiftX * offset, kFollowTolerance) << frame;
        EXPECT_NEAR(positionAt(bezier, 1, frame).y, held.y + kShiftY * offset, kFollowTolerance) << frame;
    }
}

TEST_F(VectorFollowBakeTest, ABlendedFollowUndoesAsOneStep)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kEarlier);
    const FollowPoint held = positionAt(bezier, 0, later);
    const FollowPoint shown = positionAt(bezier, 0, kStart);

    UndoCommandPtr undo;
    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = later;
    context()->followVectors(params, &undo);
    ASSERT_TRUE(bool(undo));
    const FollowPoint followed = positionAt(bezier, 0, kStart + 3);

    undo->undo();
    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, (std::set<double> { (double)kEarlier, (double)later }));
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).x, shown.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).x, held.x);

    undo->redo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 12u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 3).x, followed.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 3).y, followed.y);
}

TEST_F(VectorFollowBakeTest, AnAbortMidBlendKeepsFinishedFramesAndTheLaterKey)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kEarlier);
    const FollowPoint held = positionAt(bezier, 0, later);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = later;
    int total = 0;
    params.shouldAbort = [&total]() {
        ++total;

        return false;
    };
    UndoCommandPtr undo;
    context()->followVectors(params, &undo);
    ASSERT_TRUE(bool(undo));
    std::map<int, FollowPoint> whole;
    for (int frame = kStart; frame <= later; ++frame) {
        whole[frame] = positionAt(bezier, 0, frame);
    }
    undo->undo();

    // The held pass takes the first half of the checks, so this one stops inside the baking pass.
    int calls = 0;
    const int allowed = (total * 3) / 4;
    params.shouldAbort = [&calls, allowed]() { return ++calls > allowed; };
    const VectorFollowReport report = context()->followVectors(params, &undo);
    EXPECT_TRUE(report.aborted);
    EXPECT_TRUE(report.error.empty()) << report.error;

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    keys.erase((double)later);
    keys.erase((double)kEarlier);
    ASSERT_GT(keys.size(), 1u);
    EXPECT_LT(keys.size(), 10u);
    int expected = kStart;
    for (double t : keys) {
        EXPECT_EQ((int)t, expected++);
        EXPECT_NEAR(positionAt(bezier, 0, t).x, whole[(int)t].x, 1e-6) << t;
        EXPECT_NEAR(positionAt(bezier, 0, t).y, whole[(int)t].y, 1e-6) << t;
    }
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).x, held.x);

    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 2u);
}

TEST_F(VectorFollowBakeTest, AFitLandsOnALaterKeyOfItsOwn)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);
    bezier->setTranslateKeyframe(kEarlier, 0., 0.);
    bezier->setTranslateKeyframe(later, 0., 0.);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine }) {
        VectorFollowParams params = paramsFor(bezier, model);
        params.last = later;
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(params, &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;

        for (int frame : { kStart, later }) {
            const FollowPoint shown = shownAt(bezier, start, frame);
            EXPECT_NEAR(shown.x, start.x, 1e-9) << frame;
            EXPECT_NEAR(shown.y, start.y, 1e-9) << frame;
        }
        // Ahead by five frames from one side and behind by five from the other, weighted evenly.
        const FollowPoint middle = shownAt(bezier, start, kStart + 5);
        EXPECT_NEAR(middle.x, start.x, kFollowTolerance);
        EXPECT_NEAR(middle.y, start.y, kFollowTolerance);

        undo->undo();
    }
}

TEST_F(VectorFollowBakeTest, AForwardOnlySourceFollowsForward)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    EXPECT_TRUE(report.shapes[0].passStops.empty());

    const FollowPoint end = positionAt(bezier, 0, kStart + kSpan);
    EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance);
}

TEST_F(VectorFollowBakeTest, ABackwardFollowOfAForwardOnlySourceReportsTheMissingLayer)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    BezierPtr bezier = makeSquare(kStart + kSpan);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.direction = VectorFollowDirection::eBackward;
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    EXPECT_NE(report.error.find("no backward motion layer"), std::string::npos) << report.error;
    EXPECT_FALSE(bool(undo));

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);
}

TEST_F(VectorFollowBakeTest, ATwoWayFollowOfAForwardOnlySourceFallsBackToForwardAndReportsIt)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kEarlier);
    const FollowPoint held = positionAt(bezier, 0, later);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = later;
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    ASSERT_EQ(report.shapes[0].passStops.size(), 1u);
    EXPECT_EQ(report.shapes[0].passStops[0].reason, VectorFollowPassEnd::eNoLayer);
    EXPECT_EQ(report.shapes[0].passStops[0].direction, VectorFollowDirection::eBackward);
    EXPECT_EQ(report.shapes[0].passStops[0].otherOnly.first, kStart + 1);
    EXPECT_EQ(report.shapes[0].passStops[0].otherOnly.last, later - 1);

    EXPECT_NEAR(positionAt(bezier, 0, later - 1).x, held.x + kShiftX * 9, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, later - 1).y, held.y + kShiftY * 9, kFollowTolerance);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).y, held.y);
}

TEST_F(VectorFollowBakeTest, AForwardFollowWithOnlyALaterKeyTracksBackFromIt)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    const FollowPoint held = positionAt(bezier, 0, later);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    EXPECT_TRUE(report.shapes[0].passStops.empty());
    EXPECT_TRUE(report.shapes[0].unfollowed.empty());

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).y, held.y);
    // Not the held pose: the current frame is where the vectors bring the key back to.
    EXPECT_NEAR(positionAt(bezier, 0, kStart).x, held.x - kShiftX * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart).y, held.y - kShiftY * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart + kSpan).x, held.x + kShiftX * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart + kSpan).y, held.y + kShiftY * 10, kFollowTolerance);

    ASSERT_TRUE(bool(undo));
    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, std::set<double> { (double)later });
}

TEST_F(VectorFollowBakeTest, AFitWithOnlyALaterKeyOfItsOwnTracksBackFromIt)
{
    const int later = kStart + 10;
    // The point key on the start frame is no key to a fit, which keys the transform.
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);
    bezier->setTranslateKeyframe(later, 0., 0.);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine }) {
        VectorFollowParams params = paramsFor(bezier, model);
        params.last = later;
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(params, &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;
        ASSERT_EQ(report.shapes.size(), 1u);
        EXPECT_TRUE(report.shapes[0].passStops.empty());

        const FollowPoint atLater = shownAt(bezier, start, later);
        EXPECT_NEAR(atLater.x, start.x, 1e-9);
        EXPECT_NEAR(atLater.y, start.y, 1e-9);
        const FollowPoint atStart = shownAt(bezier, start, kStart);
        EXPECT_NEAR(atStart.x, start.x - kShiftX * 10, kFollowTolerance);
        EXPECT_NEAR(atStart.y, start.y - kShiftY * 10, kFollowTolerance);

        ASSERT_TRUE(bool(undo));
        undo->undo();
    }
}

TEST_F(VectorFollowBakeTest, AForwardFollowOfAForwardOnlySourceWithOnlyALaterKeyReportsTheMissingLayer)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    const FollowPoint held = positionAt(bezier, 0, later);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    ASSERT_EQ(report.shapes[0].passStops.size(), 1u);
    EXPECT_EQ(report.shapes[0].passStops[0].direction, VectorFollowDirection::eBackward);
    EXPECT_EQ(report.shapes[0].passStops[0].reason, VectorFollowPassEnd::eNoLayer);
    EXPECT_EQ(report.shapes[0].passStops[0].frame, later);
    EXPECT_TRUE(report.shapes[0].passStops[0].otherOnly.empty());

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(*keys.begin(), (double)later);
    EXPECT_EQ(keys.size(), (std::size_t)(kStart + kSpan - later + 1));
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).x, held.x);
    EXPECT_NEAR(positionAt(bezier, 0, kStart + kSpan).x, held.x + kShiftX * 10, kFollowTolerance);
}

TEST_F(VectorFollowBakeTest, ABackwardFollowWithAnEarlierKeyMeetsBothAndBlendsBetween)
{
    const int current = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    bezier->setKeyframe(current + 2);
    const FollowPoint held = positionAt(bezier, 1, kStart);
    const FollowPoint shown = positionAt(bezier, 1, current);
    EXPECT_NEAR(shown.x, held.x, 1e-9);
    EXPECT_NEAR(shown.y, held.y, 1e-9);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = current;
    params.direction = VectorFollowDirection::eBackward;
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    EXPECT_TRUE(report.shapes[0].passStops.empty());
    EXPECT_TRUE(report.shapes[0].unfollowed.empty());

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 12u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, current).x, shown.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, current).y, shown.y);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).y, held.y);

    // As forward: one pass ahead by the frames since the earlier key, the other behind by the frames
    // to the current frame.
    for (int frame : { kStart + 2, kStart + 5, kStart + 7 }) {
        const double weight = followBridgeWeight(frame, kStart, current);
        const double offset = (frame - kStart) - weight * 10.;
        EXPECT_NEAR(positionAt(bezier, 1, frame).x, held.x + kShiftX * offset, kFollowTolerance) << frame;
        EXPECT_NEAR(positionAt(bezier, 1, frame).y, held.y + kShiftY * offset, kFollowTolerance) << frame;
    }
}

TEST_F(VectorFollowBakeTest, ABackwardBlendUndoesAsOneStep)
{
    const int current = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    bezier->setKeyframe(current + 2);
    const FollowPoint shown = positionAt(bezier, 0, current);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = current;
    params.direction = VectorFollowDirection::eBackward;
    UndoCommandPtr undo;
    context()->followVectors(params, &undo);
    ASSERT_TRUE(bool(undo));
    const FollowPoint followed = positionAt(bezier, 0, kStart + 7);

    undo->undo();
    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, (std::set<double> { (double)kStart, (double)(current + 2) }));
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, current).x, shown.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, current).y, shown.y);

    undo->redo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 12u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 7).x, followed.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 7).y, followed.y);
}

TEST_F(VectorFollowBakeTest, AnAbortMidBackwardBlendKeepsFinishedFramesNextToTheCurrentFrame)
{
    const int current = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    bezier->setKeyframe(current + 2);
    const FollowPoint held = positionAt(bezier, 0, kStart);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.last = current;
    params.direction = VectorFollowDirection::eBackward;
    int total = 0;
    params.shouldAbort = [&total]() {
        ++total;

        return false;
    };
    UndoCommandPtr undo;
    context()->followVectors(params, &undo);
    ASSERT_TRUE(bool(undo));
    std::map<int, FollowPoint> whole;
    for (int frame = kStart; frame <= current; ++frame) {
        whole[frame] = positionAt(bezier, 0, frame);
    }
    undo->undo();

    // The held pass takes the first half of the checks, so this one stops inside the baking pass.
    int calls = 0;
    const int allowed = (total * 3) / 4;
    params.shouldAbort = [&calls, allowed]() { return ++calls > allowed; };
    const VectorFollowReport report = context()->followVectors(params, &undo);
    EXPECT_TRUE(report.aborted);
    EXPECT_TRUE(report.error.empty()) << report.error;

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    keys.erase((double)kStart);
    keys.erase((double)(current + 2));
    ASSERT_GT(keys.size(), 1u);
    EXPECT_LT(keys.size(), 10u);
    int expected = current;
    for (std::set<double>::const_reverse_iterator it = keys.rbegin(); it != keys.rend(); ++it) {
        const double t = *it;
        EXPECT_EQ((int)t, expected--);
        EXPECT_NEAR(positionAt(bezier, 0, t).x, whole[(int)t].x, 1e-6) << t;
        EXPECT_NEAR(positionAt(bezier, 0, t).y, whole[(int)t].y, 1e-6) << t;
    }
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).x, held.x);

    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 2u);
}

TEST_F(VectorFollowBakeTest, ABackwardFitLandsOnAnEarlierKeyOfItsOwn)
{
    const int current = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);
    bezier->setTranslateKeyframe(kStart, 0., 0.);
    bezier->setTranslateKeyframe(current + 2, 0., 0.);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine }) {
        VectorFollowParams params = paramsFor(bezier, model);
        params.last = current;
        params.direction = VectorFollowDirection::eBackward;
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(params, &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;

        for (int frame : { kStart, current }) {
            const FollowPoint shown = shownAt(bezier, start, frame);
            EXPECT_NEAR(shown.x, start.x, 1e-9) << frame;
            EXPECT_NEAR(shown.y, start.y, 1e-9) << frame;
        }
        const FollowPoint middle = shownAt(bezier, start, kStart + 5);
        EXPECT_NEAR(middle.x, start.x, kFollowTolerance);
        EXPECT_NEAR(middle.y, start.y, kFollowTolerance);

        ASSERT_TRUE(bool(undo));
        undo->undo();
    }
}

TEST_F(VectorFollowBakeTest, ABackwardFollowWithOnlyAnEarlierKeyTracksForwardFromIt)
{
    const int earlier = kStart + 10;
    BezierPtr bezier = makeSquare(earlier);
    const FollowPoint held = positionAt(bezier, 0, earlier);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.direction = VectorFollowDirection::eBackward;
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    EXPECT_TRUE(report.shapes[0].passStops.empty());
    EXPECT_TRUE(report.shapes[0].unfollowed.empty());

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, earlier).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, earlier).y, held.y);
    // Not the held pose: the current frame is where the vectors bring the key forward to.
    EXPECT_NEAR(positionAt(bezier, 0, kStart + kSpan).x, held.x + kShiftX * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart + kSpan).y, held.y + kShiftY * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart).x, held.x - kShiftX * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart).y, held.y - kShiftY * 10, kFollowTolerance);

    ASSERT_TRUE(bool(undo));
    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, std::set<double> { (double)earlier });
}

TEST_F(VectorFollowBakeTest, ARefusedSourceWritesNothing)
{
    BezierPtr bezier = makeSquare(kStart);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.source = createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1);
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    EXPECT_FALSE(report.error.empty());
    EXPECT_FALSE(bool(undo));

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
