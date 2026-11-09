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

#include <list>
#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/VectorFollowTestSupport.h"

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/KnobTypes.h"
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

class VectorFollowBakeBase
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
        _generator = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1));
        setKnob<KnobChoice>(_generator, kVectorGeneratorParamQuality, (int)eFlowQualityMedium);
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    }

    // Medium solves at half resolution and undershoots this half-pixel motion by 0.03 px a
    // frame, more than kFollowTolerance by frame +20. Only the cases that assert a position pay
    // for Fine; the rest check keys, undo and aborts, which do not depend on the solve quality.
    void useFine()
    {
        setKnob<KnobChoice>(_generator, kVectorGeneratorParamQuality, (int)eFlowQualityFine);
    }

    RotoCurveContextPtr context() const
    {
        return _roto->getCurveContext();
    }

    BezierPtr makeSquare(double time,
                         double left = 90.,
                         double bottom = 70.) const
    {
        BezierPtr bezier = context()->makeBezier(left, bottom, kRotoBezierBaseName, time, false);
        bezier->addControlPoint(left + 40., bottom, time);
        bezier->addControlPoint(left + 40., bottom + 40., time);
        bezier->addControlPoint(left, bottom + 40., time);
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

// Two suites so each stays well inside the ctest timeout on the slower task-graph leg.
class VectorFollowBakeTest
    : public VectorFollowBakeBase {
};

class VectorFollowBlendTest
    : public VectorFollowBakeBase {
};

class VectorFollowSelectionTest
    : public VectorFollowBakeBase {
};

TEST_F(VectorFollowBakeTest, PerPointFollowsAndLandsOnTheVectors)
{
    useFine();
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
    useFine();
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
    useFine();
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
    useFine();
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
    EXPECT_TRUE(isUserStopVectorFollowReport(report));
    EXPECT_FALSE(isCleanVectorFollowReport(report));

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

TEST_F(VectorFollowBlendTest, AForwardFollowWithALaterKeyMeetsBothAndBlendsBetween)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, ABlendedFollowUndoesAsOneStep)
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

TEST_F(VectorFollowBlendTest, AnAbortMidBlendKeepsFinishedFramesAndTheLaterKey)
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

TEST_F(VectorFollowBlendTest, AFitLandsOnALaterKeyOfItsOwn)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, AForwardOnlySourceFollowsForward)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, ABackwardFollowOfAForwardOnlySourceReportsTheMissingLayer)
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

TEST_F(VectorFollowBlendTest, ATwoWayFollowOfAForwardOnlySourceFallsBackToForwardAndReportsIt)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, AForwardFollowWithOnlyALaterKeyTracksBackFromIt)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, AFitWithOnlyALaterKeyOfItsOwnTracksBackFromIt)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, AForwardFollowOfAForwardOnlySourceWithOnlyALaterKeyReportsTheMissingLayer)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, ABackwardFollowWithAnEarlierKeyMeetsBothAndBlendsBetween)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, ABackwardBlendUndoesAsOneStep)
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

TEST_F(VectorFollowBlendTest, AnAbortMidBackwardBlendKeepsFinishedFramesNextToTheCurrentFrame)
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

TEST_F(VectorFollowBlendTest, ABackwardFitLandsOnAnEarlierKeyOfItsOwn)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, ABackwardFollowWithOnlyAnEarlierKeyTracksForwardFromIt)
{
    useFine();
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

TEST_F(VectorFollowBlendTest, ARefusedSourceWritesNothing)
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

// Selected items have their knobs slaved to the context's. A write to one item's matrix that
// changes several dimensions at once used to echo between two selected items without end.
TEST_F(VectorFollowSelectionTest, TwoSelectedShapesTakeAMatrixWriteWithoutEchoing)
{
    BezierPtr first = makeSquare(kStart);
    BezierPtr second = makeSquare(kStart, 150.);
    const Transform::Matrix3x3 identity(1., 0., 0., 0., 1., 0., 0., 0., 1.);
    first->setExtraMatrix(true, kStart, identity);
    second->setExtraMatrix(true, kStart, identity);
    context()->select(std::list<RotoDrawableItemPtr> { first, second }, RotoItem::eSelectionReasonOther);

    const Transform::Matrix3x3 moved(1.1, 0.2, 7., -0.1, 0.9, -3., 0., 0., 1.);
    first->setExtraMatrix(true, kStart, moved);

    KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(first->getKnobByName("extraMatrix"));
    ASSERT_TRUE(bool(knob));
    EXPECT_DOUBLE_EQ(knob->getValueAtTime(kStart, 0), moved.a);
    EXPECT_DOUBLE_EQ(knob->getValueAtTime(kStart, 2), moved.c);
    EXPECT_DOUBLE_EQ(knob->getValueAtTime(kStart, 5), moved.f);
}

// The GUI sequence that overflowed the stack: a per-point follow of two selected shapes, undone and
// redone, then a fit follow of two new selected shapes, undone and run again.
TEST_F(VectorFollowSelectionTest, AFitFollowOfTwoSelectedShapesAfterAnUndo)
{
    const std::list<RotoDrawableItemPtr> perPoint { makeSquare(kStart), makeSquare(kStart, 150.) };
    context()->select(perPoint, RotoItem::eSelectionReasonOther);
    VectorFollowParams params = paramsFor(perPoint.front(), VectorFollowModel::ePerPoint);
    params.items = perPoint;
    UndoCommandPtr undo;
    VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_TRUE(bool(undo));
    undo->undo();
    undo->redo();
    context()->clearSelection(RotoItem::eSelectionReasonOther);
    for (const RotoDrawableItemPtr& item : perPoint) {
        context()->removeItem(item);
    }

    BezierPtr first = makeSquare(kStart);
    BezierPtr second = makeSquare(kStart, 150.);
    const FollowPoint firstStart = positionAt(first, 0, kStart);
    const FollowPoint secondStart = positionAt(second, 0, kStart);
    const std::list<RotoDrawableItemPtr> fitted { first, second };
    context()->select(fitted, RotoItem::eSelectionReasonOther);
    params = paramsFor(first, VectorFollowModel::eAffine);
    params.items = fitted;
    for (int run = 0; run < 2; ++run) {
        UndoCommandPtr fitUndo;
        report = context()->followVectors(params, &fitUndo);
        ASSERT_TRUE(report.error.empty()) << report.error;
        ASSERT_EQ(report.shapes.size(), 2u);
        ASSERT_TRUE(bool(fitUndo));

        // Medium undershoots this motion by about 0.6 px over the span; the case is about the
        // selection, not the solve.
        const double tolerance = 2.;
        const FollowPoint firstEnd = shownAt(first, firstStart, kStart + kSpan);
        const FollowPoint secondEnd = shownAt(second, secondStart, kStart + kSpan);
        EXPECT_NEAR(firstEnd.x, firstStart.x + kShiftX * kSpan, tolerance);
        EXPECT_NEAR(firstEnd.y, firstStart.y + kShiftY * kSpan, tolerance);
        EXPECT_NEAR(secondEnd.x, secondStart.x + kShiftX * kSpan, tolerance);
        EXPECT_NEAR(secondEnd.y, secondStart.y + kShiftY * kSpan, tolerance);

        fitUndo->undo();
        const FollowPoint firstBack = shownAt(first, firstStart, kStart + kSpan);
        const FollowPoint secondBack = shownAt(second, secondStart, kStart + kSpan);
        EXPECT_NEAR(firstBack.x, firstStart.x, 1e-9);
        EXPECT_NEAR(firstBack.y, firstStart.y, 1e-9);
        EXPECT_NEAR(secondBack.x, secondStart.x, 1e-9);
        EXPECT_NEAR(secondBack.y, secondStart.y, 1e-9);
    }
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
