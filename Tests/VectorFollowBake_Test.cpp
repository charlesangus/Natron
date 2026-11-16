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

#include <functional>
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
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoKeyframeReduce.h"
#include "Engine/RotoLayer.h"
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
// A frame before the follow range.
constexpr int kEarlier = kStart - 2;

class VectorFollowBakeBase
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
        _source = createNode(QString::fromUtf8(vectorsPluginID()), 1);
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    }

    virtual const char* vectorsPluginID() const
    {
        return kTestPluginIDFollowVectors;
    }

    void forwardOnly()
    {
        setKnob<KnobBool>(_source, kFollowVectorsParamBackward, false);
    }

    RotoCurveContextPtr context() const
    {
        return _roto->getCurveContext();
    }

    BezierPtr makeSquare(double time,
                         double left = 90.,
                         double bottom = 70.) const
    {
        return makeSquareIn(context(), time, left, bottom);
    }

    static BezierPtr makeSquareIn(const RotoCurveContextPtr& context,
                                  double time,
                                  double left = 90.,
                                  double bottom = 70.)
    {
        BezierPtr bezier = context->makeBezier(left, bottom, kRotoBezierBaseName, time, false);
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
        params.source = _source;
        params.first = kStart;
        params.last = kStart + kSpan;
        params.model = model;

        return params;
    }

    VectorFollowParams blendParamsFor(const RotoDrawableItemPtr& item,
                                      VectorFollowModel model) const
    {
        VectorFollowParams params = paramsFor(item, model);
        params.mode = VectorFollowMode::eBlendBetweenKeys;

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

    // Per dimension of each transform knob an item reads, its keys then its value at a few times.
    using TransformState = std::vector<std::vector<std::pair<double, double>>>;

    static TransformState transformOf(const RotoDrawableItemPtr& item)
    {
        TransformState state;
        for (const char* name : { "translate", "rotate", "scale", "center", "extraMatrix" }) {
            KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(item->getKnobByName(name));
            if (!knob) {
                ADD_FAILURE() << "no " << name << " knob";
                continue;
            }
            for (int d = 0; d < knob->getDimension(); ++d) {
                std::vector<std::pair<double, double>> dim;
                CurvePtr curve = knob->getCurve(ViewIdx(0), d);
                if (curve) {
                    for (const KeyFrame& key : curve->getKeyFrames_mt_safe()) {
                        dim.push_back(std::make_pair(key.getTime(), key.getValue()));
                    }
                }
                for (double time : { (double)kEarlier, (double)kStart, (double)kStart + kSpan / 2, (double)kStart + kSpan }) {
                    dim.push_back(std::make_pair(-time, knob->getValueAtTime(time, d)));
                }
                state.push_back(dim);
            }
        }

        return state;
    }

    static void expectSameTransform(const TransformState& got,
                                    const TransformState& expected,
                                    double tolerance,
                                    const char* what)
    {
        ASSERT_EQ(got.size(), expected.size()) << what;
        for (std::size_t d = 0; d < got.size(); ++d) {
            ASSERT_EQ(got[d].size(), expected[d].size()) << what << ", dimension " << d;
            for (std::size_t k = 0; k < got[d].size(); ++k) {
                EXPECT_EQ(got[d][k].first, expected[d][k].first) << what << ", dimension " << d << ", entry " << k;
                EXPECT_NEAR(got[d][k].second, expected[d][k].second, tolerance) << what << ", dimension " << d << ", entry " << k;
            }
        }
    }

    // A per-point follow of a square over the whole span lands on the source's motion.
    void expectAPerPointFollowLandsOnTheVectors()
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

    NodePtr _source;
    NodePtr _roto;
};

class VectorFollowBakeTest
    : public VectorFollowBakeBase {
};

class VectorFollowBlendTest
    : public VectorFollowBakeBase {
};

class VectorFollowSelectionTest
    : public VectorFollowBakeBase {
};

class RotoReduceKeyframesTest
    : public VectorFollowBakeBase {
};

class VectorFollowZoomTest
    : public VectorFollowBakeBase {
public:
    const char* vectorsPluginID() const override
    {
        return kTestPluginIDFollowZoomVectors;
    }
};

class VectorFollowSplineWarpTest
    : public VectorFollowBakeBase {
public:
    void SetUp() override
    {
        VectorFollowBakeBase::SetUp();
        _warp = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
    }

    SplineWarpContextPtr warpContext() const
    {
        return std::dynamic_pointer_cast<SplineWarpContext>(_warp->getCurveContext());
    }

    NodePtr _warp;
};

// The follow over vectors solved from the moving texture, rather than its exact ones.
class VectorFollowGeneratorTest
    : public VectorFollowBakeBase {
public:
    void SetUp() override
    {
        VectorFollowBakeBase::SetUp();
        _source = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1));
        // Medium solves at half resolution and undershoots this half-pixel motion by 0.03 px a
        // frame, more than kFollowTolerance by frame +20.
        setKnob<KnobChoice>(_source, kVectorGeneratorParamQuality, (int)eFlowQualityFine);
    }
};

TEST_F(VectorFollowGeneratorTest, PerPointFollowsAndLandsOnTheSolvedVectors)
{
    expectAPerPointFollowLandsOnTheVectors();
}

TEST_F(VectorFollowBakeTest, PerPointFollowsAndLandsOnTheVectors)
{
    expectAPerPointFollowLandsOnTheVectors();
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

TEST_F(VectorFollowBakeTest, AFollowKeysOverTheKeysOnItsFrames)
{
    BezierPtr bezier = makeSquare(kStart);
    const int later = kStart + 10;
    bezier->setKeyframe(later);
    const FollowPoint start = positionAt(bezier, 1, kStart);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_TRUE(report.shapes[0].passStops.empty());
    EXPECT_TRUE(report.shapes[0].unfollowed.empty());

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).x, start.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 1, kStart).y, start.y);
    EXPECT_NEAR(positionAt(bezier, 1, later).x, start.x + kShiftX * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 1, later).y, start.y + kShiftY * 10, kFollowTolerance);
}

// A shape keyed on every frame, corrected on one and followed again from there.
TEST_F(VectorFollowBakeTest, ReFollowingFromACorrectedFrameOverwritesTheFramesAfterIt)
{
    BezierPtr bezier = makeSquare(kStart);
    UndoCommandPtr undo;
    context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(bool(undo));
    std::map<int, FollowPoint> followed;
    for (int frame = kStart; frame <= kStart + kSpan; ++frame) {
        followed[frame] = positionAt(bezier, 0, frame);
    }

    const int corrected = kStart + 8;
    for (int i = 0; i < 4; ++i) {
        bezier->movePointByIndex(i, corrected, 6., -4.);
    }
    const FollowPoint fixed = positionAt(bezier, 0, corrected);
    ASSERT_NEAR(fixed.x, followed[corrected].x + 6., 1e-9);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.first = corrected;
    UndoCommandPtr refollow;
    const VectorFollowReport report = context()->followVectors(params, &refollow);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_TRUE(bool(refollow));

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
    for (int frame = kStart; frame < corrected; ++frame) {
        EXPECT_DOUBLE_EQ(positionAt(bezier, 0, frame).x, followed[frame].x) << frame;
        EXPECT_DOUBLE_EQ(positionAt(bezier, 0, frame).y, followed[frame].y) << frame;
    }
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, corrected).x, fixed.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, corrected).y, fixed.y);
    for (int frame = corrected + 1; frame <= kStart + kSpan; ++frame) {
        EXPECT_NEAR(positionAt(bezier, 0, frame).x, fixed.x + kShiftX * (frame - corrected), kFollowTolerance) << frame;
        EXPECT_NEAR(positionAt(bezier, 0, frame).y, fixed.y + kShiftY * (frame - corrected), kFollowTolerance) << frame;
    }

    refollow->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, corrected).x, fixed.x);
    for (int frame = corrected + 1; frame <= kStart + kSpan; ++frame) {
        EXPECT_DOUBLE_EQ(positionAt(bezier, 0, frame).x, followed[frame].x) << frame;
        EXPECT_DOUBLE_EQ(positionAt(bezier, 0, frame).y, followed[frame].y) << frame;
    }
}

TEST_F(VectorFollowBakeTest, KeysOutsideTheFollowedFramesStayAsTheyAre)
{
    BezierPtr bezier = makeSquare(kStart);
    bezier->setKeyframe(kEarlier);
    const int after = kStart + kSpan + 3;
    bezier->setKeyframe(after);
    for (int i = 0; i < 4; ++i) {
        bezier->movePointByIndex(i, after, -5., 2.);
    }
    const FollowPoint atEarlier = positionAt(bezier, 0, kEarlier);
    const FollowPoint atAfter = positionAt(bezier, 0, after);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 3);
    EXPECT_EQ(keys.count((double)kEarlier), 1u);
    EXPECT_EQ(keys.count((double)after), 1u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kEarlier).x, atEarlier.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kEarlier).y, atEarlier.y);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, after).x, atAfter.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, after).y, atAfter.y);
}

TEST_F(VectorFollowBakeTest, AFollowStartsFromWhatTheStartFrameShows)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    const FollowPoint held = positionAt(bezier, 0, later);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(paramsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).y, held.y);
    EXPECT_NEAR(positionAt(bezier, 0, later).x, held.x + kShiftX * 10, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, later).y, held.y + kShiftY * 10, kFollowTolerance);

    ASSERT_TRUE(bool(undo));
    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, std::set<double> { (double)later });
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).x, held.x);
}

TEST_F(VectorFollowBakeTest, AFitFollowOverwritesALaterKeyOfItsOwn)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);
    bezier->setTranslateKeyframe(later, 0., 0.);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine }) {
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(paramsFor(bezier, model), &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;

        const FollowPoint atStart = shownAt(bezier, start, kStart);
        EXPECT_NEAR(atStart.x, start.x, 1e-9);
        EXPECT_NEAR(atStart.y, start.y, 1e-9);
        const FollowPoint atLater = shownAt(bezier, start, later);
        EXPECT_NEAR(atLater.x, start.x + kShiftX * 10, kFollowTolerance);
        EXPECT_NEAR(atLater.y, start.y + kShiftY * 10, kFollowTolerance);

        ASSERT_TRUE(bool(undo));
        undo->undo();
        const FollowPoint back = shownAt(bezier, start, later);
        EXPECT_NEAR(back.x, start.x, 1e-9);
        EXPECT_NEAR(back.y, start.y, 1e-9);
    }
}

TEST_F(VectorFollowBakeTest, ABackwardFollowOverwritesTheFramesBeforeTheStart)
{
    const int current = kStart + kSpan;
    BezierPtr bezier = makeSquare(current);
    bezier->setKeyframe(kStart);
    bezier->setKeyframe(kEarlier);
    const FollowPoint held = positionAt(bezier, 0, current);

    VectorFollowParams params = paramsFor(bezier, VectorFollowModel::ePerPoint);
    params.direction = VectorFollowDirection::eBackward;
    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)kSpan + 2);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, current).x, held.x);
    EXPECT_NEAR(positionAt(bezier, 0, kStart).x, held.x - kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(positionAt(bezier, 0, kStart).y, held.y - kShiftY * kSpan, kFollowTolerance);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kEarlier).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kEarlier).y, held.y);

    ASSERT_TRUE(bool(undo));
    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, (std::set<double> { (double)kEarlier, (double)kStart, (double)current }));
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).x, held.x);
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

TEST_F(VectorFollowBakeTest, OnlyAStopTheUserAskedForThatKeptItsSourceIsAUserStop)
{
    VectorFollowReport report;
    report.aborted = true;
    report.shapes.resize(1);
    report.shapes[0].passStops.resize(1);
    report.shapes[0].stopped = true;
    EXPECT_FALSE(isUserStopVectorFollowReport(report));

    report.userStopped = true;
    report.error = "Aborted.";
    EXPECT_TRUE(isUserStopVectorFollowReport(report));

    report.sourceLost = true;
    EXPECT_FALSE(isUserStopVectorFollowReport(report));
}

TEST_F(VectorFollowBlendTest, ABlendMeetsBothKeysAndBlendsBetween)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kStart);
    const FollowPoint held = positionAt(bezier, 1, later);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(blendParamsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_TRUE(report.shapes[0].passStops.empty());
    EXPECT_EQ(report.action, std::string(kVectorFollowBlendActionName));

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 11u);
    EXPECT_EQ(*keys.begin(), (double)kStart);
    EXPECT_EQ(*keys.rbegin(), (double)later);
    for (int frame : { kStart, later, later + 5 }) {
        EXPECT_DOUBLE_EQ(positionAt(bezier, 1, frame).x, held.x) << frame;
        EXPECT_DOUBLE_EQ(positionAt(bezier, 1, frame).y, held.y) << frame;
    }

    // Both keys hold the same pose on a moving picture, so at each frame one pass is ahead by the
    // frames since the earlier key and the other behind by the frames to the later one.
    for (int frame : { kStart + 3, kStart + 5, kStart + 8 }) {
        const double weight = followBridgeWeight(frame, kStart, later);
        const double offset = (frame - kStart) - weight * 10.;
        EXPECT_NEAR(positionAt(bezier, 1, frame).x, held.x + kShiftX * offset, kFollowTolerance) << frame;
        EXPECT_NEAR(positionAt(bezier, 1, frame).y, held.y + kShiftY * offset, kFollowTolerance) << frame;
    }
}

TEST_F(VectorFollowBlendTest, ABlendUndoesAsOneStep)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kStart);
    const FollowPoint held = positionAt(bezier, 0, later);

    UndoCommandPtr undo;
    context()->followVectors(blendParamsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(bool(undo));
    const FollowPoint blended = positionAt(bezier, 0, kStart + 3);

    undo->undo();
    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys, (std::set<double> { (double)kStart, (double)later }));
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 3).x, held.x);

    undo->redo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 11u);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 3).x, blended.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart + 3).y, blended.y);
}

TEST_F(VectorFollowBlendTest, AnAbortMidBlendKeepsFinishedFramesAndTheKeys)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kStart);
    const FollowPoint held = positionAt(bezier, 0, later);

    VectorFollowParams params = blendParamsFor(bezier, VectorFollowModel::ePerPoint);
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
    keys.erase((double)kStart);
    ASSERT_GT(keys.size(), 0u);
    EXPECT_LT(keys.size(), 9u);
    int expected = kStart + 1;
    for (double t : keys) {
        EXPECT_EQ((int)t, expected++);
        EXPECT_NEAR(positionAt(bezier, 0, t).x, whole[(int)t].x, 1e-6) << t;
        EXPECT_NEAR(positionAt(bezier, 0, t).y, whole[(int)t].y, 1e-6) << t;
    }
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, kStart).x, held.x);
    EXPECT_DOUBLE_EQ(positionAt(bezier, 0, later).x, held.x);

    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 2u);
}

TEST_F(VectorFollowBlendTest, AFitBlendLandsOnItsOwnKeys)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);
    bezier->setTranslateKeyframe(kStart, 0., 0.);
    bezier->setTranslateKeyframe(later, 0., 0.);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine }) {
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(blendParamsFor(bezier, model), &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;

        for (int frame : { kStart, later, later + 5 }) {
            const FollowPoint shown = shownAt(bezier, start, frame);
            EXPECT_NEAR(shown.x, start.x, 1e-9) << frame;
            EXPECT_NEAR(shown.y, start.y, 1e-9) << frame;
        }
        // Ahead by five frames from one side and behind by five from the other, weighted evenly.
        const FollowPoint middle = shownAt(bezier, start, kStart + 5);
        EXPECT_NEAR(middle.x, start.x, kFollowTolerance);
        EXPECT_NEAR(middle.y, start.y, kFollowTolerance);

        ASSERT_TRUE(bool(undo));
        undo->undo();
    }
}

TEST_F(VectorFollowBlendTest, ABlendOverAForwardOnlySourceFallsBackToForwardAndReportsIt)
{
    forwardOnly();
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kStart);
    const FollowPoint held = positionAt(bezier, 0, later);

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(blendParamsFor(bezier, VectorFollowModel::ePerPoint), &undo);
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

// The range picks the keys: one before it is no key to blend from.
TEST_F(VectorFollowBlendTest, OnlyTheKeysWithinTheRangeCount)
{
    const int later = kStart + 10;
    BezierPtr bezier = makeSquare(later);
    bezier->setKeyframe(kStart);
    const int before = kStart - 6;
    bezier->setKeyframe(before);
    for (int i = 0; i < 4; ++i) {
        bezier->movePointByIndex(i, before, -8., 0.);
    }
    std::map<int, FollowPoint> outside;
    for (int frame : { before, kStart }) {
        outside[frame] = positionAt(bezier, 0, frame);
    }

    UndoCommandPtr undo;
    const VectorFollowReport report = context()->followVectors(blendParamsFor(bezier, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;

    // Smooth interpolation still bends the frames between the two earlier keys towards the new
    // ones after them, so only the keys themselves are compared.
    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    std::set<double> expected { (double)before };
    for (int frame = kStart; frame <= later; ++frame) {
        expected.insert((double)frame);
    }
    EXPECT_EQ(keys, expected);
    for (int frame : { before, kStart }) {
        EXPECT_DOUBLE_EQ(positionAt(bezier, 0, frame).x, outside[frame].x) << frame;
        EXPECT_DOUBLE_EQ(positionAt(bezier, 0, frame).y, outside[frame].y) << frame;
    }
}

TEST_F(VectorFollowBlendTest, AShapeWithFewerThanTwoKeysInTheRangeIsLeftAlone)
{
    BezierPtr single = makeSquare(kStart);
    UndoCommandPtr undo;
    VectorFollowReport report = context()->followVectors(blendParamsFor(single, VectorFollowModel::ePerPoint), &undo);
    EXPECT_FALSE(report.error.empty());
    EXPECT_FALSE(bool(undo));
    std::set<double> keys;
    single->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);

    const int later = kStart + 10;
    BezierPtr pair = makeSquare(later, 150.);
    pair->setKeyframe(kStart);
    VectorFollowParams params = blendParamsFor(single, VectorFollowModel::ePerPoint);
    params.items.push_back(pair);
    report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_EQ(report.shapes[0].name, pair->getScriptName());
    ASSERT_EQ(report.refused.size(), 1u);
    EXPECT_EQ(report.refused[0].name, single->getScriptName());
    keys.clear();
    single->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 1u);
    keys.clear();
    pair->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), 11u);
}

// The first shape's backward pass has no layer to run on; the user stops the blend during the second.
TEST_F(VectorFollowBlendTest, AUserStopAfterAPassStopIsStillAUserStop)
{
    forwardOnly();
    const int later = kStart + 10;
    BezierPtr bridged = makeSquare(later);
    bridged->setKeyframe(kStart);
    BezierPtr second = makeSquare(later, 150.);
    second->setKeyframe(kStart);

    VectorFollowParams params = blendParamsFor(bridged, VectorFollowModel::ePerPoint);
    int firstShapeChecks = 0;
    params.shouldAbort = [&firstShapeChecks]() {
        ++firstShapeChecks;

        return false;
    };
    UndoCommandPtr undo;
    VectorFollowReport report = context()->followVectors(params, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    ASSERT_EQ(report.shapes[0].passStops.size(), 1u);
    EXPECT_FALSE(report.userStopped);
    EXPECT_FALSE(isUserStopVectorFollowReport(report));
    ASSERT_TRUE(bool(undo));
    undo->undo();

    params.items = std::list<RotoDrawableItemPtr> { bridged, second };
    int calls = 0;
    const int allowed = firstShapeChecks + 6;
    params.shouldAbort = [&calls, allowed]() { return ++calls > allowed; };
    report = context()->followVectors(params, &undo);
    EXPECT_TRUE(report.aborted);
    EXPECT_TRUE(report.userStopped);
    ASSERT_EQ(report.shapes.size(), 2u);
    EXPECT_EQ(report.shapes[0].passStops.size(), 1u);
    EXPECT_TRUE(isUserStopVectorFollowReport(report));
    EXPECT_FALSE(isCleanVectorFollowReport(report));

    std::set<double> keys;
    second->getKeyframeTimes(&keys);
    EXPECT_GT(keys.size(), 2u);
    EXPECT_LT(keys.size(), 11u);
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

// A square keyed by hand on every frame from kStart to kStart + steps, its points moved by step on each.
static BezierPtr
keyEveryFrame(const BezierPtr& bezier,
              int steps,
              const std::function<FollowPoint(int)>& step)
{
    for (int frame = kStart + 1; frame <= kStart + steps; ++frame) {
        bezier->setKeyframe(frame);
        const FollowPoint by = step(frame);
        for (int i = 0; i < 4; ++i) {
            bezier->movePointByIndex(i, frame, by.x, by.y);
        }
    }

    return bezier;
}

using PointsByFrame = std::map<int, std::vector<FollowPoint>>;

static PointsByFrame
pointsOver(const BezierPtr& bezier,
           int first,
           int last)
{
    PointsByFrame points;
    for (int frame = first; frame <= last; ++frame) {
        for (int i = 0; i < 4; ++i) {
            FollowPoint p;
            bezier->getControlPointAtIndex(i)->getPositionAtTime(false, frame, ViewIdx(0), &p.x, &p.y);
            points[frame].push_back(p);
        }
    }

    return points;
}

static void
expectPointsWithin(const PointsByFrame& got,
                   const PointsByFrame& expected,
                   double tolerance)
{
    ASSERT_EQ(got.size(), expected.size());
    for (const auto& frame : expected) {
        const std::vector<FollowPoint>& points = got.at(frame.first);
        for (std::size_t i = 0; i < points.size(); ++i) {
            EXPECT_NEAR(points[i].x, frame.second[i].x, tolerance) << "frame " << frame.first << ", point " << i;
            EXPECT_NEAR(points[i].y, frame.second[i].y, tolerance) << "frame " << frame.first << ", point " << i;
        }
    }
}

// Reduction needs no vectors, so these shapes are keyed by hand on a Roto node with nothing connected.
TEST_F(RotoReduceKeyframesTest, HandKeysReduceWithinTheToleranceAsOneUndoStep)
{
    const int steps = 10;
    BezierPtr bezier = keyEveryFrame(makeSquare(kStart), steps, [](int) { return FollowPoint(1.5, 0.5); });
    const PointsByFrame before = pointsOver(bezier, kStart - 2, kStart + steps + 2);

    UndoCommandPtr undo;
    const std::vector<RotoReducedItem> reduced = reduceRotoKeyframes(context(), { bezier }, kStart, kStart + steps, kFollowTolerance, &undo);
    ASSERT_EQ(reduced.size(), 1u);
    EXPECT_EQ(reduced[0].item, bezier);
    ASSERT_FALSE(reduced[0].removed.empty());
    ASSERT_TRUE(bool(undo));

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)steps + 1 - reduced[0].removed.size());
    for (int frame : reduced[0].removed) {
        EXPECT_EQ(keys.count((double)frame), 0u) << frame;
    }
    EXPECT_EQ(keys.count((double)kStart), 1u);
    EXPECT_EQ(keys.count((double)kStart + steps), 1u);
    expectPointsWithin(pointsOver(bezier, kStart - 2, kStart + steps + 2), before, kFollowTolerance + 1e-9);

    undo->undo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)steps + 1);
    expectPointsWithin(pointsOver(bezier, kStart - 2, kStart + steps + 2), before, 1e-9);

    undo->redo();
    keys.clear();
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)steps + 1 - reduced[0].removed.size());
}

TEST_F(RotoReduceKeyframesTest, KeysThatShapeTheMotionStayUnderASmallTolerance)
{
    const int steps = 10;
    BezierPtr bezier = keyEveryFrame(makeSquare(kStart), steps, [](int frame) {
        return FollowPoint((frame % 2) ? 4. : -4., 0.);
    });
    const PointsByFrame before = pointsOver(bezier, kStart, kStart + steps);

    UndoCommandPtr undo;
    std::vector<RotoReducedItem> reduced = reduceRotoKeyframes(context(), { bezier }, kStart, kStart + steps, kFollowTolerance, &undo);
    EXPECT_TRUE(reduced.empty());
    EXPECT_FALSE(bool(undo));
    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    EXPECT_EQ(keys.size(), (std::size_t)steps + 1);

    const double loose = 10.;
    reduced = reduceRotoKeyframes(context(), { bezier }, kStart, kStart + steps, loose, &undo);
    ASSERT_EQ(reduced.size(), 1u);
    EXPECT_FALSE(reduced[0].removed.empty());
    expectPointsWithin(pointsOver(bezier, kStart, kStart + steps), before, loose + 1e-9);
}

TEST_F(RotoReduceKeyframesTest, OnlyKeysWithinTheRangeGo)
{
    const int steps = 10;
    BezierPtr bezier = keyEveryFrame(makeSquare(kStart), steps, [](int) { return FollowPoint(1.5, 0.5); });
    const int first = kStart + 5;

    UndoCommandPtr undo;
    const std::vector<RotoReducedItem> reduced = reduceRotoKeyframes(context(), { bezier }, first, kStart + steps, 100., &undo);
    ASSERT_EQ(reduced.size(), 1u);
    ASSERT_FALSE(reduced[0].removed.empty());
    EXPECT_GE(*reduced[0].removed.begin(), first);
    EXPECT_LE(*reduced[0].removed.rbegin(), kStart + steps);

    std::set<double> keys;
    bezier->getKeyframeTimes(&keys);
    for (int frame = kStart; frame < first; ++frame) {
        EXPECT_EQ(keys.count((double)frame), 1u) << frame;
    }
}

TEST_F(RotoReduceKeyframesTest, TransformKeysReduceToo)
{
    const int steps = 10;
    BezierPtr bezier = makeSquare(kStart);
    const FollowPoint start = positionAt(bezier, 0, kStart);
    for (int frame = kStart; frame <= kStart + steps; ++frame) {
        bezier->setTranslateKeyframe(frame, 2. * (frame - kStart), -1. * (frame - kStart));
    }
    std::map<int, FollowPoint> shown;
    for (int frame = kStart; frame <= kStart + steps; ++frame) {
        shown[frame] = shownAt(bezier, start, frame);
    }
    const TransformState transform = transformOf(bezier);

    UndoCommandPtr undo;
    const std::vector<RotoReducedItem> reduced = reduceRotoKeyframes(context(), { bezier }, kStart, kStart + steps, kFollowTolerance, &undo);
    ASSERT_EQ(reduced.size(), 1u);
    EXPECT_FALSE(reduced[0].removed.empty());

    KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(bezier->getKnobByName("translate"));
    ASSERT_TRUE(bool(translate));
    EXPECT_LT(translate->getCurve(ViewIdx(0), 0)->getKeyFramesCount(), steps + 1);
    for (int frame = kStart; frame <= kStart + steps; ++frame) {
        const FollowPoint now = shownAt(bezier, start, frame);
        EXPECT_NEAR(now.x, shown[frame].x, kFollowTolerance + 1e-9) << frame;
        EXPECT_NEAR(now.y, shown[frame].y, kFollowTolerance + 1e-9) << frame;
    }

    ASSERT_TRUE(bool(undo));
    undo->undo();
    expectSameTransform(transformOf(bezier), transform, 1e-9, "after undo");
}

// A write that changes several dimensions of one selected shape's matrix at once reaches that shape
// alone, and returns.
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

    KnobDoublePtr other = std::dynamic_pointer_cast<KnobDouble>(second->getKnobByName("extraMatrix"));
    ASSERT_TRUE(bool(other));
    EXPECT_DOUBLE_EQ(other->getValueAtTime(kStart, 0), 1.);
    EXPECT_DOUBLE_EQ(other->getValueAtTime(kStart, 2), 0.);
    EXPECT_DOUBLE_EQ(other->getValueAtTime(kStart, 5), 0.);
}

TEST_F(VectorFollowSelectionTest, ChangingTheSelectionLeavesEveryShapesTransformAlone)
{
    BezierPtr a = makeSquare(kStart);
    BezierPtr b = makeSquare(kStart, 150.);
    // makeBezier() leaves the new shape selected.
    context()->clearSelection(RotoItem::eSelectionReasonOther);
    a->setTranslateKeyframe(kStart, 5., 3.);
    a->setTranslateKeyframe(kStart + kSpan, 9., -2.);
    a->setExtraMatrix(true, kStart, Transform::Matrix3x3(1.1, 0.2, 7., -0.1, 0.9, -3., 0., 0., 1.));
    b->setTranslateKeyframe(kStart + 4, -4., 8.);
    b->setExtraMatrix(true, kStart + kSpan, Transform::Matrix3x3(0.8, -0.3, -2., 0.1, 1.2, 6., 0., 0., 1.));
    std::dynamic_pointer_cast<KnobDouble>(b->getKnobByName("rotate"))->setValue(12.);
    const TransformState aBefore = transformOf(a);
    const TransformState bBefore = transformOf(b);
    const RotoCurveContext::TransformKnobs page = context()->getTransformKnobs();
    ASSERT_TRUE(bool(page.translate));

    context()->select(a, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(context()->getTransformTiedItem(), RotoDrawableItemPtr(a));
    EXPECT_TRUE(page.translate->isEnabled(0));
    EXPECT_DOUBLE_EQ(page.translate->getValueAtTime(kStart, 0), 5.);
    expectSameTransform(transformOf(a), aBefore, 0., "a, selected alone");

    context()->select(b, RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(bool(context()->getTransformTiedItem()));
    EXPECT_FALSE(page.translate->isEnabled(0));
    EXPECT_FALSE(page.extraMatrix->isEnabled(0));
    expectSameTransform(transformOf(a), aBefore, 0., "a, after selecting b");
    expectSameTransform(transformOf(b), bBefore, 0., "b, after selecting it");

    context()->deselect(a, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(context()->getTransformTiedItem(), RotoDrawableItemPtr(b));
    EXPECT_TRUE(page.translate->isEnabled(0));
    EXPECT_DOUBLE_EQ(page.rotate->getValue(), 12.);
    expectSameTransform(transformOf(a), aBefore, 0., "a, after deselecting it");
    expectSameTransform(transformOf(b), bBefore, 0., "b, after deselecting a");

    context()->clearSelection(RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(bool(context()->getTransformTiedItem()));
    EXPECT_FALSE(page.translate->isEnabled(0));
    expectSameTransform(transformOf(a), aBefore, 0., "a, after clearing the selection");
    expectSameTransform(transformOf(b), bBefore, 0., "b, after clearing the selection");

    // A page edit of the one selected shape reaches it and not the other.
    context()->select(a, RotoItem::eSelectionReasonOther);
    page.rotate->setValue(30.);
    EXPECT_DOUBLE_EQ(std::dynamic_pointer_cast<KnobDouble>(a->getKnobByName("rotate"))->getValue(), 30.);
    context()->select(b, RotoItem::eSelectionReasonOther);
    context()->deselect(a, RotoItem::eSelectionReasonOther);
    EXPECT_DOUBLE_EQ(std::dynamic_pointer_cast<KnobDouble>(a->getKnobByName("rotate"))->getValue(), 30.);
    expectSameTransform(transformOf(b), bBefore, 0., "b, after a page edit of a");
}

// The zoom moves the left and right shapes opposite ways, so a fit shared between them would show.
TEST_F(VectorFollowZoomTest, TwoSelectedShapesEachGetTheirOwnFit)
{
    const double left = 50.;
    const double right = 166.;
    BezierPtr leftShape = makeSquare(kStart, left);
    BezierPtr rightShape = makeSquare(kStart, right);
    BezierPtr leftAlone = makeSquare(kStart, left);
    BezierPtr rightAlone = makeSquare(kStart, right);
    const TransformState leftUntouched = transformOf(leftShape);
    const TransformState rightUntouched = transformOf(rightShape);
    const FollowPoint leftCentre(left + 20., 90.);
    const FollowPoint rightCentre(right + 20., 90.);

    for (VectorFollowModel model : { VectorFollowModel::eAffine, VectorFollowModel::eTranslate }) {
        SCOPED_TRACE((int)model);
        std::vector<UndoCommandPtr> aloneUndos;
        for (const BezierPtr& alone : { leftAlone, rightAlone }) {
            context()->clearSelection(RotoItem::eSelectionReasonOther);
            context()->select(alone, RotoItem::eSelectionReasonOther);
            UndoCommandPtr undo;
            const VectorFollowReport report = context()->followVectors(paramsFor(alone, model), &undo);
            ASSERT_TRUE(report.error.empty()) << report.error;
            ASSERT_TRUE(bool(undo));
            aloneUndos.push_back(undo);
        }

        const std::list<RotoDrawableItemPtr> both { leftShape, rightShape };
        context()->clearSelection(RotoItem::eSelectionReasonOther);
        context()->select(both, RotoItem::eSelectionReasonOther);
        VectorFollowParams params = paramsFor(leftShape, model);
        params.items = both;
        UndoCommandPtr undo;
        const VectorFollowReport report = context()->followVectors(params, &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;
        ASSERT_EQ(report.shapes.size(), 2u);
        ASSERT_TRUE(bool(undo));

        const TransformState leftFollowed = transformOf(leftShape);
        const TransformState rightFollowed = transformOf(rightShape);
        expectSameTransform(leftFollowed, transformOf(leftAlone), 1e-6, "left");
        expectSameTransform(rightFollowed, transformOf(rightAlone), 1e-6, "right");

        const double leftMove = shownAt(leftShape, leftCentre, kStart + kSpan).x - leftCentre.x;
        const double rightMove = shownAt(rightShape, rightCentre, kStart + kSpan).x - rightCentre.x;
        EXPECT_LT(leftMove, -5.);
        EXPECT_GT(rightMove, 5.);

        undo->undo();
        expectSameTransform(transformOf(leftShape), leftUntouched, 0., "left, undone");
        expectSameTransform(transformOf(rightShape), rightUntouched, 0., "right, undone");
        undo->redo();
        expectSameTransform(transformOf(leftShape), leftFollowed, 0., "left, redone");
        expectSameTransform(transformOf(rightShape), rightFollowed, 0., "right, redone");
        undo->undo();
        expectSameTransform(transformOf(leftShape), leftUntouched, 0., "left, undone again");
        expectSameTransform(transformOf(rightShape), rightUntouched, 0., "right, undone again");

        for (const UndoCommandPtr& aloneUndo : aloneUndos) {
            aloneUndo->undo();
        }
    }
}

// A per-point follow of two selected shapes, undone and redone, then a fit follow of two new
// selected shapes, undone and run again.
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

        const double tolerance = kFollowTolerance;
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

// A curve linked to another takes that one's transform, which a fit on its behalf would move for
// every curve linked to it, and its own transform is unused: the fit is keyed on its points.
TEST_F(VectorFollowSplineWarpTest, AFitOnATransformLinkedCurveKeysItsOwnPointsOnly)
{
    SplineWarpContextPtr warp = warpContext();
    ASSERT_TRUE(bool(warp));
    BezierPtr linked = makeSquareIn(warp, kStart);
    BezierPtr target = makeSquareIn(warp, kStart, 150.);
    BezierPtr otherLinked = makeSquareIn(warp, kStart, 90., 140.);
    warp->linkTransform(linked, target);
    warp->linkTransform(otherLinked, target);
    warp->clearSelection(RotoItem::eSelectionReasonOther);
    const TransformState linkedBefore = transformOf(linked);
    const TransformState targetBefore = transformOf(target);
    const TransformState otherBefore = transformOf(otherLinked);
    const FollowPoint start = shownAt(linked, positionAt(linked, 0, kStart), kStart);
    const FollowPoint targetEnd = positionAt(target, 0, kStart + kSpan);
    const FollowPoint otherEnd = positionAt(otherLinked, 0, kStart + kSpan);

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine, VectorFollowModel::ePerspective }) {
        SCOPED_TRACE((int)model);
        UndoCommandPtr undo;
        const VectorFollowReport report = warp->followVectors(paramsFor(linked, model), &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;
        EXPECT_TRUE(report.refused.empty());
        ASSERT_EQ(report.shapes.size(), 1u);
        EXPECT_FALSE(report.shapes[0].stopped);
        ASSERT_TRUE(bool(undo));

        const FollowPoint end = shownAt(linked, positionAt(linked, 0, kStart + kSpan), kStart + kSpan);
        EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance);
        EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance);
        std::set<double> keys;
        linked->getKeyframeTimes(&keys);
        EXPECT_EQ(keys.size(), (std::size_t)kSpan + 1);

        EXPECT_EQ(warp->getTransformLink(linked), RotoItemPtr(target));
        EXPECT_EQ(warp->getTransformLink(otherLinked), RotoItemPtr(target));
        expectSameTransform(transformOf(linked), linkedBefore, 0., "linked");
        expectSameTransform(transformOf(target), targetBefore, 0., "target");
        expectSameTransform(transformOf(otherLinked), otherBefore, 0., "other linked");
        EXPECT_DOUBLE_EQ(positionAt(target, 0, kStart + kSpan).x, targetEnd.x);
        EXPECT_DOUBLE_EQ(positionAt(target, 0, kStart + kSpan).y, targetEnd.y);
        EXPECT_DOUBLE_EQ(positionAt(otherLinked, 0, kStart + kSpan).x, otherEnd.x);
        EXPECT_DOUBLE_EQ(positionAt(otherLinked, 0, kStart + kSpan).y, otherEnd.y);
        for (const BezierPtr& untouched : { target, otherLinked }) {
            std::set<double> untouchedKeys;
            untouched->getKeyframeTimes(&untouchedKeys);
            EXPECT_EQ(untouchedKeys.size(), 1u) << untouched->getScriptName();
        }

        undo->undo();
        keys.clear();
        linked->getKeyframeTimes(&keys);
        EXPECT_EQ(keys.size(), 1u);
        const FollowPoint back = shownAt(linked, positionAt(linked, 0, kStart + kSpan), kStart + kSpan);
        EXPECT_NEAR(back.x, start.x, 1e-9);
        EXPECT_NEAR(back.y, start.y, 1e-9);
    }
}

// Selected before its link target, a linked curve still moves with the vectors once, rather than
// again with the transform its target is keyed with.
TEST_F(VectorFollowSplineWarpTest, ALinkedCurveSelectedBeforeItsTargetMovesOnce)
{
    SplineWarpContextPtr warp = warpContext();
    ASSERT_TRUE(bool(warp));
    BezierPtr linked = makeSquareIn(warp, kStart);
    BezierPtr target = makeSquareIn(warp, kStart, 150.);
    warp->linkTransform(linked, target);
    warp->clearSelection(RotoItem::eSelectionReasonOther);
    const FollowPoint linkedStart = shownAt(linked, positionAt(linked, 0, kStart), kStart);
    const FollowPoint targetStart = shownAt(target, positionAt(target, 0, kStart), kStart);
    const double end = kStart + kSpan;

    for (VectorFollowModel model : { VectorFollowModel::eTranslate, VectorFollowModel::eAffine, VectorFollowModel::ePerspective }) {
        SCOPED_TRACE((int)model);
        VectorFollowParams params = paramsFor(linked, model);
        params.items = std::list<RotoDrawableItemPtr> { linked, target };
        UndoCommandPtr undo;
        const VectorFollowReport report = warp->followVectors(params, &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;
        EXPECT_TRUE(report.refused.empty());
        ASSERT_EQ(report.shapes.size(), 2u);
        ASSERT_TRUE(bool(undo));

        const FollowPoint targetEnd = shownAt(target, positionAt(target, 0, end), end);
        EXPECT_NEAR(targetEnd.x, targetStart.x + kShiftX * kSpan, kFollowTolerance);
        EXPECT_NEAR(targetEnd.y, targetStart.y + kShiftY * kSpan, kFollowTolerance);
        const FollowPoint linkedEnd = shownAt(linked, positionAt(linked, 0, end), end);
        EXPECT_NEAR(linkedEnd.x, linkedStart.x + kShiftX * kSpan, kFollowTolerance);
        EXPECT_NEAR(linkedEnd.y, linkedStart.y + kShiftY * kSpan, kFollowTolerance);

        undo->undo();
        const FollowPoint linkedBack = shownAt(linked, positionAt(linked, 0, end), end);
        EXPECT_NEAR(linkedBack.x, linkedStart.x, 1e-9);
        EXPECT_NEAR(linkedBack.y, linkedStart.y, 1e-9);
        const FollowPoint targetBack = shownAt(target, positionAt(target, 0, end), end);
        EXPECT_NEAR(targetBack.x, targetStart.x, 1e-9);
        EXPECT_NEAR(targetBack.y, targetStart.y, 1e-9);
    }
}

// A per-point follow writes the linked curve's own points only.
TEST_F(VectorFollowSplineWarpTest, APerPointFollowMovesALinkedCurvesPointsOnly)
{
    SplineWarpContextPtr warp = warpContext();
    ASSERT_TRUE(bool(warp));
    BezierPtr linked = makeSquareIn(warp, kStart);
    BezierPtr target = makeSquareIn(warp, kStart, 150.);
    warp->linkTransform(linked, target);
    const TransformState linkedBefore = transformOf(linked);
    const TransformState targetBefore = transformOf(target);
    const FollowPoint start = positionAt(linked, 0, kStart);
    const FollowPoint targetStart = positionAt(target, 0, kStart + kSpan);

    UndoCommandPtr undo;
    const VectorFollowReport report = warp->followVectors(paramsFor(linked, VectorFollowModel::ePerPoint), &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    EXPECT_TRUE(report.refused.empty());
    const FollowPoint end = positionAt(linked, 0, kStart + kSpan);
    EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance);
    expectSameTransform(transformOf(linked), linkedBefore, 0., "linked");
    expectSameTransform(transformOf(target), targetBefore, 0., "target");
    EXPECT_DOUBLE_EQ(positionAt(target, 0, kStart + kSpan).x, targetStart.x);
    EXPECT_DOUBLE_EQ(positionAt(target, 0, kStart + kSpan).y, targetStart.y);
}

// A parent layer turns and scales the curve's own translate on its way to where the curve shows, so
// a translate fit keyed as the shown motion would move the curve off the vectors.
TEST_F(VectorFollowSplineWarpTest, ATranslateFitUnderATurnedOrScaledLayerMovesWithTheVectors)
{
    SplineWarpContextPtr warp = warpContext();
    ASSERT_TRUE(bool(warp));
    // About the square's centre, so the shown square stays inside the vectors' picture.
    const double centreX = 110.;
    const double centreY = 90.;
    struct LayerCase {
        const char* name;
        double rotate;
        double scale;
    };
    for (const LayerCase& layerCase : { LayerCase { "turned", 90., 1. }, LayerCase { "scaled", 0., 1.5 }, LayerCase { "turned and scaled", -30., 0.75 } }) {
        SCOPED_TRACE(layerCase.name);
        BezierPtr bezier = makeSquareIn(warp, kStart);
        warp->clearSelection(RotoItem::eSelectionReasonOther);
        RotoLayerPtr layer = warp->addLayer();
        ASSERT_TRUE(bool(layer));
        layer->addItem(bezier);
        SplineWarpTransform transform;
        int dims = 0;
        transform.value(SplineWarpTransform::indexOf("rotate", &dims)).setValue(layerCase.rotate, kStart);
        const int scale = SplineWarpTransform::indexOf("scale", &dims);
        transform.value(scale).setValue(layerCase.scale, kStart);
        transform.value(scale + 1).setValue(layerCase.scale, kStart);
        const int center = SplineWarpTransform::indexOf("center", &dims);
        transform.value(center).setValue(centreX, kStart);
        transform.value(center + 1).setValue(centreY, kStart);
        warp->setLayerTransform(layer, transform);
        warp->clearSelection(RotoItem::eSelectionReasonOther);

        const FollowPoint start = shownAt(bezier, positionAt(bezier, 0, kStart), kStart);
        UndoCommandPtr undo;
        const VectorFollowReport report = warp->followVectors(paramsFor(bezier, VectorFollowModel::eTranslate), &undo);
        ASSERT_TRUE(report.error.empty()) << report.error;
        EXPECT_TRUE(report.refused.empty());
        ASSERT_EQ(report.shapes.size(), 1u);
        EXPECT_FALSE(report.shapes[0].stopped);
        ASSERT_TRUE(bool(undo));

        for (int offset : { kSpan / 2, kSpan }) {
            const FollowPoint shown = shownAt(bezier, positionAt(bezier, 0, kStart + offset), kStart + offset);
            EXPECT_NEAR(shown.x, start.x + kShiftX * offset, kFollowTolerance) << "frame +" << offset;
            EXPECT_NEAR(shown.y, start.y + kShiftY * offset, kFollowTolerance) << "frame +" << offset;
        }
        std::set<double> keys;
        bezier->getKeyframeTimes(&keys);
        EXPECT_EQ(keys.size(), 1u);

        undo->undo();
        const FollowPoint back = shownAt(bezier, positionAt(bezier, 0, kStart + kSpan), kStart + kSpan);
        EXPECT_NEAR(back.x, start.x, 1e-9);
        EXPECT_NEAR(back.y, start.y, 1e-9);
    }
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
