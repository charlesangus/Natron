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

#include <algorithm>
#include <cmath>
#include <list>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/RenderBothWays.h"
#include "Tests/VectorFollowTestSupport.h"

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"
#include "Engine/RectI.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/UndoCommand.h"
#include "Engine/VectorFollowBake.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace VectorFollowTest;

constexpr double kFollowTolerance = 0.5;
constexpr int kStart = 5;
constexpr int kSpan = 10;
constexpr double kSourceLeft = 90.;
constexpr double kDestinationLeft = 150.;
constexpr double kBottom = 70.;
// On the bottom edge of both squares, which the motion carries along that edge for the whole span.
constexpr double kCorrespondenceParam = 0.2;

// SplineWarp over a translating texture, followed through the vectors VectorGenerator solves from it.
class SplineWarpFollowVectorsTest
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
        NodePtr texture = createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1);
        _vectors = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, texture);
        // Medium solves at half resolution and undershoots this half-pixel motion.
        setKnob<KnobChoice>(_vectors, kVectorGeneratorParamQuality, (int)eFlowQualityFine);
        _warpNode = createOver(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, texture);
        ASSERT_TRUE(bool(_warpNode));
        _warp = std::dynamic_pointer_cast<SplineWarpContext>(_warpNode->getCurveContext());
        ASSERT_TRUE(bool(_warp));
        _source = makeSquare(kSourceLeft);
        _destination = makeSquare(kDestinationLeft);
        _link = _warp->link(_source, _destination);
        ASSERT_TRUE(bool(_link));
        _warp->clearSelection(RotoItem::eSelectionReasonOther);
    }

    BezierPtr makeSquare(double left) const
    {
        BezierPtr bezier = _warp->makeBezier(left, kBottom, kRotoBezierBaseName, kStart, false);
        bezier->addControlPoint(left + 40., kBottom, kStart);
        bezier->addControlPoint(left + 40., kBottom + 40., kStart);
        bezier->addControlPoint(left, kBottom + 40., kStart);
        bezier->setCurveFinished(true);

        return bezier;
    }

    VectorFollowParams params() const
    {
        VectorFollowParams params;
        params.source = _vectors;
        params.first = kStart;
        params.last = kStart + kSpan;
        params.model = VectorFollowModel::ePerPoint;

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

    static std::size_t keyCount(const BezierPtr& bezier)
    {
        std::set<double> keys;
        bezier->getKeyframeTimes(&keys);

        return keys.size();
    }

    // Where each end of correspondence 0 lies on its curve at time.
    void correspondenceAt(double time,
                          Point* source,
                          Point* destination) const
    {
        double s = 0.;
        double d = 0.;
        ASSERT_TRUE(_link->getCorrespondence(0, time, &s, &d));
        *source = SplineWarpCurveShape::fromBezier(*_source, time).pointAt(s);
        *destination = SplineWarpCurveShape::fromBezier(*_destination, time).pointAt(d);
    }

    void expectTheLinkSurvives() const
    {
        ASSERT_EQ(_warp->getLinks().size(), 1u);
        EXPECT_EQ(_warp->getLink(_source), _link);
        EXPECT_EQ(_warp->getLink(_destination), _link);
        EXPECT_EQ(_link->getSource(), _source);
        EXPECT_EQ(_link->getDestination(), _destination);
    }

    void expectFollowed(const BezierPtr& bezier,
                        const FollowPoint& start) const
    {
        const FollowPoint end = positionAt(bezier, 0, kStart + kSpan);
        EXPECT_NEAR(end.x, start.x + kShiftX * kSpan, kFollowTolerance) << bezier->getScriptName();
        EXPECT_NEAR(end.y, start.y + kShiftY * kSpan, kFollowTolerance) << bezier->getScriptName();
        EXPECT_EQ(keyCount(bezier), (std::size_t)kSpan + 1) << bezier->getScriptName();
    }

    void expectStill(const BezierPtr& bezier,
                     double left) const
    {
        EXPECT_EQ(keyCount(bezier), 1u) << bezier->getScriptName();
        const FollowPoint end = positionAt(bezier, 0, kStart + kSpan);
        EXPECT_DOUBLE_EQ(end.x, left) << bezier->getScriptName();
        EXPECT_DOUBLE_EQ(end.y, kBottom) << bezier->getScriptName();
    }

    std::vector<float> renderAt(double time) const
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = renderNodePlanesDirect(_warpNode, time, ViewIdx(0), 0, RectI(70, 40, 210, 130), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(ok) << error;
        if (!ok || (planes.size() != 1)) {
            return std::vector<float>();
        }

        return planes[0].pixels;
    }

    static double largestDifference(const std::vector<float>& a,
                                    const std::vector<float>& b)
    {
        EXPECT_EQ(a.size(), b.size());
        double largest = 0.;
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            largest = std::max(largest, (double)std::abs(a[i] - b[i]));
        }

        return largest;
    }

    NodePtr _vectors;
    NodePtr _warpNode;
    SplineWarpContextPtr _warp;
    BezierPtr _source;
    BezierPtr _destination;
    SplineWarpLinkPtr _link;
};

TEST_F(SplineWarpFollowVectorsTest, ASourceCurveFollowsWhileItsDestinationStays)
{
    const FollowPoint start = positionAt(_source, 0, kStart);
    VectorFollowParams follow = params();
    follow.items.push_back(_source);

    UndoCommandPtr undo;
    const VectorFollowReport report = _warp->followVectors(follow, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    EXPECT_TRUE(bool(undo));

    expectFollowed(_source, start);
    expectStill(_destination, kDestinationLeft);
    expectTheLinkSurvives();
}

TEST_F(SplineWarpFollowVectorsTest, ADestinationCurveFollowsWhileItsSourceStays)
{
    const FollowPoint start = positionAt(_destination, 0, kStart);
    VectorFollowParams follow = params();
    follow.items.push_back(_destination);

    UndoCommandPtr undo;
    const VectorFollowReport report = _warp->followVectors(follow, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);

    expectFollowed(_destination, start);
    expectStill(_source, kSourceLeft);
    expectTheLinkSurvives();
}

// Each end moves with the picture under it and is keyed where its curve passes nearest.
TEST_F(SplineWarpFollowVectorsTest, ACorrespondencePointFollowsAlongItsCurves)
{
    ASSERT_EQ(_warp->addCorrespondence(_link, kCorrespondenceParam, kCorrespondenceParam), 0);
    Point sourceStart;
    Point destinationStart;
    correspondenceAt(kStart, &sourceStart, &destinationStart);
    ASSERT_NEAR(sourceStart.y, kBottom, 1e-9);
    ASSERT_NEAR(destinationStart.y, kBottom, 1e-9);

    VectorFollowParams follow = params();
    VectorFollowPointsPtr points = _warp->makeCorrespondenceFollowPoints(_link, 0);
    ASSERT_TRUE(bool(points));
    follow.points.push_back(points);
    EXPECT_FALSE(bool(_warp->makeCorrespondenceFollowPoints(_link, 1)));

    UndoCommandPtr undo;
    const VectorFollowReport report = _warp->followVectors(follow, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 1u);
    EXPECT_FALSE(report.shapes[0].stopped);
    ASSERT_TRUE(bool(undo));

    Point sourceEnd;
    Point destinationEnd;
    correspondenceAt(kStart + kSpan, &sourceEnd, &destinationEnd);
    EXPECT_NEAR(sourceEnd.x, sourceStart.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(sourceEnd.y, kBottom, 1e-6);
    EXPECT_NEAR(destinationEnd.x, destinationStart.x + kShiftX * kSpan, kFollowTolerance);
    EXPECT_NEAR(destinationEnd.y, kBottom, 1e-6);

    const std::vector<SplineWarpCorrespondence> all = _link->getCorrespondences();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].source.getKeyFrameTimes().size(), (std::size_t)kSpan + 1);
    EXPECT_EQ(all[0].destination.getKeyFrameTimes().size(), (std::size_t)kSpan + 1);

    expectStill(_source, kSourceLeft);
    expectStill(_destination, kDestinationLeft);
    expectTheLinkSurvives();

    undo->undo();
    const std::vector<SplineWarpCorrespondence> back = _link->getCorrespondences();
    ASSERT_EQ(back.size(), 1u);
    EXPECT_FALSE(back[0].source.isAnimated());
    EXPECT_FALSE(back[0].destination.isAnimated());
    EXPECT_DOUBLE_EQ(back[0].source.getStaticValue(), kCorrespondenceParam);
    EXPECT_DOUBLE_EQ(back[0].destination.getStaticValue(), kCorrespondenceParam);
}

// The warp field is cached by the node's hash, which the follow's keys must move.
TEST_F(SplineWarpFollowVectorsTest, ARenderAfterTheFollowShowsTheNewKeys)
{
    const double last = kStart + kSpan;
    const std::vector<float> before = renderAt(last);
    ASSERT_FALSE(before.empty());

    VectorFollowParams follow = params();
    follow.items.push_back(_source);
    UndoCommandPtr undo;
    const VectorFollowReport report = _warp->followVectors(follow, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_TRUE(bool(undo));

    const std::vector<float> after = renderAt(last);
    EXPECT_GT(largestDifference(after, before), 1e-3);

    undo->undo();
    EXPECT_LT(largestDifference(renderAt(last), before), 1e-6);
}

// Only the correspondence moves here, so a changed render can only come from its keys.
TEST_F(SplineWarpFollowVectorsTest, ARenderAfterACorrespondenceOnlyFollowShowsTheNewKeys)
{
    ASSERT_EQ(_warp->addCorrespondence(_link, kCorrespondenceParam, kCorrespondenceParam), 0);
    const double last = kStart + kSpan;
    const std::vector<float> before = renderAt(last);
    ASSERT_FALSE(before.empty());

    VectorFollowParams follow = params();
    follow.points.push_back(_warp->makeCorrespondenceFollowPoints(_link, 0));
    UndoCommandPtr undo;
    const VectorFollowReport report = _warp->followVectors(follow, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_TRUE(bool(undo));
    EXPECT_EQ(keyCount(_source), 1u);
    EXPECT_EQ(keyCount(_destination), 1u);
    EXPECT_TRUE(_link->getCorrespondences()[0].source.isAnimated());

    const std::vector<float> after = renderAt(last);
    EXPECT_GT(largestDifference(after, before), 1e-3);

    undo->undo();
    EXPECT_LT(largestDifference(renderAt(last), before), 1e-6);
}

TEST_F(SplineWarpFollowVectorsTest, OneUndoRevertsTheCurvesAndTheCorrespondence)
{
    ASSERT_EQ(_warp->addCorrespondence(_link, kCorrespondenceParam, kCorrespondenceParam), 0);
    const FollowPoint sourceStart = positionAt(_source, 0, kStart + kSpan);
    const FollowPoint destinationStart = positionAt(_destination, 0, kStart + kSpan);

    VectorFollowParams follow = params();
    follow.items.push_back(_source);
    follow.items.push_back(_destination);
    follow.points.push_back(_warp->makeCorrespondenceFollowPoints(_link, 0));

    UndoCommandPtr undo;
    const VectorFollowReport report = _warp->followVectors(follow, &undo);
    ASSERT_TRUE(report.error.empty()) << report.error;
    ASSERT_EQ(report.shapes.size(), 3u);
    ASSERT_TRUE(bool(undo));
    EXPECT_GT(keyCount(_source), 1u);
    EXPECT_GT(keyCount(_destination), 1u);
    double followedSource = 0.;
    double followedDestination = 0.;
    ASSERT_TRUE(_link->getCorrespondence(0, kStart + kSpan, &followedSource, &followedDestination));
    EXPECT_TRUE(_link->getCorrespondences()[0].source.isAnimated());

    undo->undo();
    EXPECT_EQ(keyCount(_source), 1u);
    EXPECT_EQ(keyCount(_destination), 1u);
    EXPECT_DOUBLE_EQ(positionAt(_source, 0, kStart + kSpan).x, sourceStart.x);
    EXPECT_DOUBLE_EQ(positionAt(_source, 0, kStart + kSpan).y, sourceStart.y);
    EXPECT_DOUBLE_EQ(positionAt(_destination, 0, kStart + kSpan).x, destinationStart.x);
    EXPECT_DOUBLE_EQ(positionAt(_destination, 0, kStart + kSpan).y, destinationStart.y);
    const std::vector<SplineWarpCorrespondence> back = _link->getCorrespondences();
    ASSERT_EQ(back.size(), 1u);
    EXPECT_FALSE(back[0].source.isAnimated());
    EXPECT_FALSE(back[0].destination.isAnimated());
    expectTheLinkSurvives();

    undo->redo();
    EXPECT_EQ(keyCount(_source), (std::size_t)kSpan + 1);
    double redoneSource = 0.;
    double redoneDestination = 0.;
    ASSERT_TRUE(_link->getCorrespondence(0, kStart + kSpan, &redoneSource, &redoneDestination));
    EXPECT_DOUBLE_EQ(redoneSource, followedSource);
    EXPECT_DOUBLE_EQ(redoneDestination, followedDestination);
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
