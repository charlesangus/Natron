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

class VectorFollowBakeTest
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
        _generator = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1));
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
