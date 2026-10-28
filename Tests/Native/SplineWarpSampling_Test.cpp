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
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SplineWarpHostTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"
#include "Engine/Project.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {
const double kTime = 1.;
// Arc-length pairing reads a piecewise-linear table, so positions along a curve are near, not exact.
const double kAlongTolerance = 0.25;

typedef std::vector<std::pair<double, double>> Points;

Points
line(double x0,
     double y0,
     double x1,
     double y1)
{
    Points p;

    p.push_back(std::make_pair(x0, y0));
    p.push_back(std::make_pair(x1, y1));

    return p;
}

Points
square(double x0,
       double y0,
       double size)
{
    Points p;

    p.push_back(std::make_pair(x0, y0));
    p.push_back(std::make_pair(x0 + size, y0));
    p.push_back(std::make_pair(x0 + size, y0 + size));
    p.push_back(std::make_pair(x0, y0 + size));

    return p;
}
} // anonymous namespace

class SplineWarpSamplingTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        _host = createNode(QString::fromUtf8(kTestPluginIDSplineWarpHost));
        if (_host) {
            _ctx = std::dynamic_pointer_cast<SplineWarpContext>(_host->getCurveContext());
        }
    }

    virtual void TearDown() OVERRIDE
    {
        _ctx.reset();
        _host.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    BezierPtr makeCurve(const Points& points,
                        bool closed)
    {
        BezierPtr bezier = _ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
        if (!bezier) {
            return bezier;
        }
        for (std::size_t i = 1; i < points.size(); ++i) {
            bezier->addControlPoint(points[i].first, points[i].second, kTime);
        }
        if (closed) {
            bezier->setCurveFinished(true);
        }

        return bezier;
    }

    // A source line at y = 0 and a destination line at y = 10, both from x = 0 to 100, linked.
    SplineWarpLinkPtr makeHorizontalLink(const Points& destination)
    {
        BezierPtr src = makeCurve(line(0., 0., 100., 0.), false);
        BezierPtr dst = makeCurve(destination, false);
        if (!src || !dst) {
            return SplineWarpLinkPtr();
        }

        return _ctx->linkWithAlignment(src, dst, 0., false);
    }

    static SplineWarpSamplingSettings settings(double spacing)
    {
        SplineWarpSamplingSettings s;

        s.time = kTime;
        s.spacing = spacing;
        s.boundarySpacing = spacing;

        return s;
    }

    NodePtr _host;
    SplineWarpContextPtr _ctx;
};

TEST_F(SplineWarpSamplingTest, AStraightLinkPairsByArcLength)
{
    ASSERT_TRUE(bool(_ctx));
    Points destination;
    destination.push_back(std::make_pair(0., 10.));
    destination.push_back(std::make_pair(30., 10.));
    destination.push_back(std::make_pair(100., 10.));
    ASSERT_TRUE(bool(makeHorizontalLink(destination)));

    const SplineWarpSampleSet set = sampleSplineWarp(*_ctx, settings(10.));

    // Ten spans of 10 px plus the open end.
    ASSERT_EQ(11u, set.pairs.size());
    EXPECT_EQ(11, set.linkPairCount);
    EXPECT_FALSE(set.hasBoundaries());
    for (std::size_t k = 0; k < set.pairs.size(); ++k) {
        const SplineWarpSamplePair& p = set.pairs[k];
        EXPECT_NEAR(10. * k, p.destination.x, kAlongTolerance) << k;
        EXPECT_NEAR(10., p.destination.y, 1e-9) << k;
        EXPECT_NEAR(10. * k, p.source.x, kAlongTolerance) << k;
        EXPECT_NEAR(0., p.source.y, 1e-9) << k;
    }
}

TEST_F(SplineWarpSamplingTest, ACorrespondenceShiftsThePairing)
{
    ASSERT_TRUE(bool(_ctx));
    Points destination;
    destination.push_back(std::make_pair(0., 10.));
    destination.push_back(std::make_pair(70., 10.));
    destination.push_back(std::make_pair(100., 10.));
    SplineWarpLinkPtr link = makeHorizontalLink(destination);
    ASSERT_TRUE(bool(link));
    // The source's midpoint (x = 50) to the destination's middle point (x = 70).
    _ctx->addCorrespondence(link, 0.5, 1.);

    const SplineWarpSampleSet set = sampleSplineWarp(*_ctx, settings(10.));

    // Spans of 70 and 50 px on their longer sides: 7 and 5 samples, plus the open end.
    ASSERT_EQ(13u, set.pairs.size());
    EXPECT_NEAR(70., set.pairs[7].destination.x, kAlongTolerance);
    EXPECT_NEAR(50., set.pairs[7].source.x, kAlongTolerance);
    EXPECT_NEAR(30., set.pairs[3].destination.x, kAlongTolerance);
    EXPECT_NEAR(50. * 3. / 7., set.pairs[3].source.x, kAlongTolerance);
    EXPECT_NEAR(100., set.pairs[12].destination.x, kAlongTolerance);
    EXPECT_NEAR(100., set.pairs[12].source.x, kAlongTolerance);

    _ctx->removeCorrespondence(link, 0);
    const SplineWarpSampleSet plain = sampleSplineWarp(*_ctx, settings(10.));
    ASSERT_EQ(11u, plain.pairs.size());
    EXPECT_NEAR(70., plain.pairs[7].destination.x, kAlongTolerance);
    EXPECT_NEAR(70., plain.pairs[7].source.x, kAlongTolerance);
}

TEST_F(SplineWarpSamplingTest, AmountLerpsTheDestinationTowardTheSource)
{
    ASSERT_TRUE(bool(_ctx));
    SplineWarpLinkPtr link = makeHorizontalLink(line(0., 10., 100., 10.));
    ASSERT_TRUE(bool(link));

    _ctx->setLinkAmount(link, 0.5, kTime);
    const SplineWarpSampleSet half = sampleSplineWarp(*_ctx, settings(10.));
    ASSERT_EQ(11u, half.pairs.size());
    for (std::size_t k = 0; k < half.pairs.size(); ++k) {
        EXPECT_NEAR(5., half.pairs[k].destination.y, 1e-9) << k;
        EXPECT_NEAR(-5., half.pairs[k].source.y - half.pairs[k].destination.y, 1e-9) << k;
        EXPECT_NEAR(half.pairs[k].source.x, half.pairs[k].destination.x, 1e-9) << k;
    }

    // The global amount multiplies the link's.
    _ctx->setLinkAmount(link, 1., kTime);
    SplineWarpSamplingSettings global = settings(10.);
    global.globalAmount = 0.5;
    const SplineWarpSampleSet globalHalf = sampleSplineWarp(*_ctx, global);
    ASSERT_EQ(half.pairs.size(), globalHalf.pairs.size());
    for (std::size_t k = 0; k < half.pairs.size(); ++k) {
        EXPECT_NEAR(half.pairs[k].destination.y, globalHalf.pairs[k].destination.y, 1e-12) << k;
    }
    EXPECT_EQ(half.hash, globalHalf.hash);

    // At amount 0 every pair is a fixed point.
    _ctx->setLinkAmount(link, 0., kTime);
    const SplineWarpSampleSet none = sampleSplineWarp(*_ctx, settings(10.));
    for (std::size_t k = 0; k < none.pairs.size(); ++k) {
        EXPECT_EQ(none.pairs[k].source.x, none.pairs[k].destination.x) << k;
        EXPECT_EQ(none.pairs[k].source.y, none.pairs[k].destination.y) << k;
    }
    EXPECT_NE(none.hash, half.hash);
}

TEST_F(SplineWarpSamplingTest, InvertSwapsTheSides)
{
    ASSERT_TRUE(bool(_ctx));
    ASSERT_TRUE(bool(makeHorizontalLink(line(0., 10., 100., 10.))));

    SplineWarpSamplingSettings inverted = settings(10.);
    inverted.invert = true;
    const SplineWarpSampleSet set = sampleSplineWarp(*_ctx, inverted);

    ASSERT_EQ(11u, set.pairs.size());
    for (std::size_t k = 0; k < set.pairs.size(); ++k) {
        EXPECT_NEAR(0., set.pairs[k].destination.y, 1e-9) << k;
        EXPECT_NEAR(10., set.pairs[k].source.y, 1e-9) << k;
    }
}

TEST_F(SplineWarpSamplingTest, ASinglePointLinkGivesOnePair)
{
    ASSERT_TRUE(bool(_ctx));
    Points a;
    a.push_back(std::make_pair(20., 30.));
    Points b;
    b.push_back(std::make_pair(25., 45.));
    BezierPtr src = makeCurve(a, false);
    BezierPtr dst = makeCurve(b, false);
    ASSERT_TRUE(bool(src));
    ASSERT_TRUE(bool(dst));
    _ctx->link(src, dst);

    const SplineWarpSampleSet set = sampleSplineWarp(*_ctx, settings(1.));

    ASSERT_EQ(1u, set.pairs.size());
    EXPECT_DOUBLE_EQ(25., set.pairs[0].destination.x);
    EXPECT_DOUBLE_EQ(45., set.pairs[0].destination.y);
    EXPECT_DOUBLE_EQ(20., set.pairs[0].source.x);
    EXPECT_DOUBLE_EQ(30., set.pairs[0].source.y);
}

TEST_F(SplineWarpSamplingTest, BoundaryPairsHaveNoDisplacement)
{
    ASSERT_TRUE(bool(_ctx));
    BezierPtr boundary = makeCurve(square(0., 0., 100.), true);
    ASSERT_TRUE(bool(boundary));
    boundary->setFeatherDistance(0., kTime);
    _ctx->setBoundary(boundary, true);

    const SplineWarpSampleSet bare = sampleSplineWarp(*_ctx, settings(10.));
    ASSERT_EQ(1u, bare.boundaries.size());
    EXPECT_EQ(0, bare.linkPairCount);
    EXPECT_EQ(40u, bare.pairs.size());
    for (std::size_t k = 0; k < bare.pairs.size(); ++k) {
        const SplineWarpSamplePair& p = bare.pairs[k];
        EXPECT_EQ(p.source.x, p.destination.x) << k;
        EXPECT_EQ(p.source.y, p.destination.y) << k;
        // On the curve itself.
        EXPECT_NEAR(50., std::max(std::fabs(p.destination.x - 50.), std::fabs(p.destination.y - 50.)), 1e-6) << k;
    }

    // With a feather the pairs sit on the feather, outside the curve.
    boundary->setFeatherDistance(5., kTime);
    const SplineWarpSampleSet feathered = sampleSplineWarp(*_ctx, settings(10.));
    ASSERT_FALSE(feathered.pairs.empty());
    for (std::size_t k = 0; k < feathered.pairs.size(); ++k) {
        const SplineWarpSamplePair& p = feathered.pairs[k];
        EXPECT_EQ(p.source.x, p.destination.x) << k;
        EXPECT_EQ(p.source.y, p.destination.y) << k;
        const double reach = std::max(std::fabs(p.destination.x - 50.), std::fabs(p.destination.y - 50.));
        EXPECT_GE(reach, 53.) << k;
        EXPECT_LE(reach, 55. + 1e-6) << k;
    }
    EXPECT_NE(bare.hash, feathered.hash);
}

TEST_F(SplineWarpSamplingTest, LinkSamplesInsideBoundariesIsReported)
{
    ASSERT_TRUE(bool(_ctx));
    BezierPtr boundary = makeCurve(square(-50., -50., 50.), true);
    ASSERT_TRUE(bool(boundary));
    boundary->setFeatherDistance(0., kTime);
    _ctx->setBoundary(boundary, true);
    ASSERT_TRUE(bool(makeHorizontalLink(line(0., 10., 100., 10.))));

    const SplineWarpSampleSet outside = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_FALSE(outside.linkSamplesInsideBoundaries);

    BezierPtr around = makeCurve(square(-20., -20., 200.), true);
    ASSERT_TRUE(bool(around));
    _ctx->setBoundary(around, true);
    const SplineWarpSampleSet inside = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_TRUE(inside.linkSamplesInsideBoundaries);
    EXPECT_EQ(2u, inside.boundaries.size());
}

TEST_F(SplineWarpSamplingTest, CoincidentDestinationsMerge)
{
    ASSERT_TRUE(bool(_ctx));
    ASSERT_TRUE(bool(makeHorizontalLink(line(0., 10., 100., 10.))));
    // A second link onto the same destination positions, from a source at y = 20.
    BezierPtr src = makeCurve(line(0., 20., 100., 20.), false);
    BezierPtr dst = makeCurve(line(0., 10., 100., 10.), false);
    ASSERT_TRUE(bool(src));
    ASSERT_TRUE(bool(dst));
    SplineWarpLinkPtr second = _ctx->linkWithAlignment(src, dst, 0., false);

    const SplineWarpSampleSet merged = sampleSplineWarp(*_ctx, settings(10.));
    ASSERT_EQ(11u, merged.pairs.size());
    EXPECT_EQ(11, merged.linkPairCount);
    for (std::size_t k = 0; k < merged.pairs.size(); ++k) {
        // The sources average to y = 10: no displacement.
        EXPECT_NEAR(10., merged.pairs[k].source.y, 1e-9) << k;
    }

    // A hundredth of a pixel apart they stay separate.
    _ctx->removeLink(second);
    BezierPtr dstApart = makeCurve(line(0., 10.01, 100., 10.01), false);
    BezierPtr srcApart = makeCurve(line(0., 20., 100., 20.), false);
    ASSERT_TRUE(bool(dstApart));
    ASSERT_TRUE(bool(srcApart));
    _ctx->linkWithAlignment(srcApart, dstApart, 0., false);
    const SplineWarpSampleSet apart = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_EQ(22u, apart.pairs.size());
}

TEST_F(SplineWarpSamplingTest, TheBudgetDecimatesUniformly)
{
    ASSERT_TRUE(bool(_ctx));
    BezierPtr src = makeCurve(line(0., 0., 1000., 0.), false);
    BezierPtr dst = makeCurve(line(0., 10., 1000., 10.), false);
    ASSERT_TRUE(bool(src));
    ASSERT_TRUE(bool(dst));
    _ctx->linkWithAlignment(src, dst, 0., false);

    SplineWarpSamplingSettings dense = settings(1.);
    dense.budget = 0;
    const SplineWarpSampleSet unbounded = sampleSplineWarp(*_ctx, dense);
    EXPECT_EQ(1001u, unbounded.pairs.size());

    dense.budget = 100;
    const SplineWarpSampleSet bounded = sampleSplineWarp(*_ctx, dense);
    ASSERT_LE(bounded.pairs.size(), 100u);
    ASSERT_GE(bounded.pairs.size(), 90u);
    const double step = 1000. / (bounded.pairs.size() - 1);
    for (std::size_t k = 1; k < bounded.pairs.size(); ++k) {
        EXPECT_NEAR(step, bounded.pairs[k].destination.x - bounded.pairs[k - 1].destination.x, 2. * kAlongTolerance) << k;
    }
}

TEST_F(SplineWarpSamplingTest, DeactivatedCurvesAndLayersAreIgnored)
{
    ASSERT_TRUE(bool(_ctx));
    SplineWarpLinkPtr link = makeHorizontalLink(line(0., 10., 100., 10.));
    ASSERT_TRUE(bool(link));
    BezierPtr destination = link->getDestination();
    ASSERT_TRUE(bool(destination));
    EXPECT_GT(sampleSplineWarp(*_ctx, settings(10.)).linkPairCount, 0);

    destination->setActivated(false, kTime);
    const SplineWarpSampleSet curveOff = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_EQ(0, curveOff.linkPairCount);
    EXPECT_TRUE(curveOff.pairs.empty());
    EXPECT_TRUE(SplineWarpTps::solve(curveOff.pairs, 0.).isIdentity());
    destination->setActivated(true, kTime);

    RotoLayerPtr layer = _ctx->addLayer();
    ASSERT_TRUE(bool(layer));
    layer->addItem(destination);
    EXPECT_GT(sampleSplineWarp(*_ctx, settings(10.)).linkPairCount, 0);
    layer->setGloballyActivated(false, false);
    EXPECT_TRUE(destination->isDeactivatedRecursive());
    const SplineWarpSampleSet layerOff = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_EQ(0, layerOff.linkPairCount);
    EXPECT_TRUE(layerOff.pairs.empty());
    layer->setGloballyActivated(true, false);

    // A deactivated boundary neither pins nor masks.
    BezierPtr boundary = makeCurve(square(200., 200., 50.), true);
    ASSERT_TRUE(bool(boundary));
    _ctx->setBoundary(boundary, true);
    EXPECT_EQ(1u, sampleSplineWarp(*_ctx, settings(10.)).boundaries.size());
    boundary->setActivated(false, kTime);
    const SplineWarpSampleSet boundaryOff = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_TRUE(boundaryOff.boundaries.empty());
    EXPECT_EQ((std::size_t)boundaryOff.linkPairCount, boundaryOff.pairs.size());
}

TEST_F(SplineWarpSamplingTest, ASoftBoundaryPinsItsCurveAndMasksNothing)
{
    ASSERT_TRUE(bool(_ctx));
    ASSERT_TRUE(bool(makeHorizontalLink(line(0., 10., 100., 10.))));
    BezierPtr soft = makeCurve(line(0., 60., 100., 60.), false);
    ASSERT_TRUE(bool(soft));
    _ctx->setSoftBoundary(soft, true);

    const SplineWarpSampleSet set = sampleSplineWarp(*_ctx, settings(10.));
    EXPECT_TRUE(set.boundaries.empty());
    ASSERT_EQ(2u, set.polylines.size());
    EXPECT_FALSE(set.polylines[1].closed);
    // About ten spans along the open curve, and its end.
    ASSERT_GE(set.pairs.size(), (std::size_t)set.linkPairCount + 11u);
    ASSERT_LE(set.pairs.size(), (std::size_t)set.linkPairCount + 12u);
    const SplineWarpTps tps = SplineWarpTps::solve(set.pairs, 0.);
    for (std::size_t k = set.linkPairCount; k < set.pairs.size(); ++k) {
        const SplineWarpSamplePair& p = set.pairs[k];
        EXPECT_EQ(p.source.x, p.destination.x) << k;
        EXPECT_EQ(p.source.y, p.destination.y) << k;
        EXPECT_NEAR(60., p.destination.y, 1e-6) << k;
        const Point s = tps.evaluate(p.destination.x, p.destination.y);
        EXPECT_NEAR(p.destination.x, s.x, 1e-6) << k;
        EXPECT_NEAR(p.destination.y, s.y, 1e-6) << k;
    }
    // Between the link and the curve the warp eases out; past the curve it still moves.
    const Point between = tps.evaluate(50., 35.);
    EXPECT_GT(std::fabs(between.y - 35.), 1.);
    EXPECT_LT(std::fabs(between.y - 35.), 10.);
    const Point beyond = tps.evaluate(50., 110.);
    EXPECT_GT(std::fabs(beyond.y - 110.), 1.);

    // A soft boundary cannot be linked, and becoming a hard boundary clears it.
    BezierPtr other = makeCurve(line(0., 80., 100., 80.), false);
    ASSERT_TRUE(bool(other));
    EXPECT_THROW(_ctx->linkWithAlignment(soft, other, 0., false), std::invalid_argument);
    BezierPtr loop = makeCurve(square(150., 150., 20.), true);
    ASSERT_TRUE(bool(loop));
    _ctx->setSoftBoundary(loop, true);
    _ctx->setBoundary(loop, true);
    EXPECT_FALSE(_ctx->isSoftBoundary(loop));
    EXPECT_TRUE(_ctx->isBoundary(loop));
}

TEST_F(SplineWarpSamplingTest, TheFormatRectanglePinsTheCorners)
{
    ASSERT_TRUE(bool(_ctx));
    ASSERT_TRUE(bool(makeHorizontalLink(line(20., 50., 80., 50.))));
    const double corners[4][2] = { { 0., 0. }, { 100., 0. }, { 100., 80. }, { 0., 80. } };

    const SplineWarpSampleSet off = sampleSplineWarp(*_ctx, settings(10.));
    const SplineWarpTps free = SplineWarpTps::solve(off.pairs, 0.);
    for (int c = 0; c < 4; ++c) {
        const Point s = free.evaluate(corners[c][0], corners[c][1]);
        EXPECT_GT(std::fabs(s.x - corners[c][0]) + std::fabs(s.y - corners[c][1]), 1.) << c;
    }

    const std::vector<SplineWarpSoftBoundarySamplingInput> format(1, splineWarpRectangleBoundary(0., 0., 100., 80.));
    const SplineWarpSampleSet on = sampleSplineWarp(*_ctx, settings(10.), format);
    EXPECT_TRUE(on.boundaries.empty());
    // The perimeter, 360, at 10 apart.
    EXPECT_EQ((std::size_t)off.linkPairCount + 36u, on.pairs.size());
    EXPECT_NE(off.hash, on.hash);
    const SplineWarpTps pinned = SplineWarpTps::solve(on.pairs, 0.);
    for (int c = 0; c < 4; ++c) {
        const Point s = pinned.evaluate(corners[c][0], corners[c][1]);
        EXPECT_NEAR(corners[c][0], s.x, 1e-6) << c;
        EXPECT_NEAR(corners[c][1], s.y, 1e-6) << c;
    }
}

TEST_F(SplineWarpSamplingTest, BoundarySamplesFollowTheBoundarySpacing)
{
    ASSERT_TRUE(bool(_ctx));
    BezierPtr hard = makeCurve(square(0., 0., 100.), true);
    ASSERT_TRUE(bool(hard));
    hard->setFeatherDistance(0., kTime);
    _ctx->setBoundary(hard, true);
    // Apart from the hard boundary, so no samples merge.
    const std::vector<SplineWarpSoftBoundarySamplingInput> format(1, splineWarpRectangleBoundary(200., 0., 300., 80.));

    SplineWarpSamplingSettings s = settings(1.);
    s.boundarySpacing = 10.;
    // 400 / 10 along the hard boundary, 360 / 10 along the rectangle; the link spacing plays no part.
    EXPECT_EQ(40u + 36u, sampleSplineWarp(*_ctx, s, format).pairs.size());
    s.boundarySpacing = 20.;
    EXPECT_EQ(20u + 18u, sampleSplineWarp(*_ctx, s, format).pairs.size());
}
