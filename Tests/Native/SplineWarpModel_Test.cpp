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

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Serialization/ProjectBundleTestHelpers.h"
#include "SplineWarpHostTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/SplineWarpSerialization.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {
const double kTime = 1.;
} // anonymous namespace

class SplineWarpModelTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    SplineWarpContextPtr makeHost(NodePtr* host)
    {
        *host = createNode(QString::fromUtf8(kTestPluginIDSplineWarpHost));
        if (!*host) {
            return SplineWarpContextPtr();
        }

        return std::dynamic_pointer_cast<SplineWarpContext>((*host)->getCurveContext());
    }

    // A Bezier through points, keyed at kTime; closed when asked, open (unfinished) otherwise.
    static BezierPtr makeCurve(const SplineWarpContextPtr& ctx,
                               const std::vector<std::pair<double, double>>& points,
                               bool closed)
    {
        BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
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

    static BezierPtr makeQuad(const SplineWarpContextPtr& ctx,
                              double x,
                              double y,
                              double size)
    {
        std::vector<std::pair<double, double>> points;
        points.push_back(std::make_pair(x, y));
        points.push_back(std::make_pair(x + size, y));
        points.push_back(std::make_pair(x + size * 1.2, y + size));
        points.push_back(std::make_pair(x - size * 0.1, y + size * 0.8));

        return makeCurve(ctx, points, true);
    }

    static BezierPtr makeOpenLine(const SplineWarpContextPtr& ctx,
                                  double x,
                                  double y)
    {
        std::vector<std::pair<double, double>> points;
        points.push_back(std::make_pair(x, y));
        points.push_back(std::make_pair(x + 50., y + 20.));
        points.push_back(std::make_pair(x + 100., y - 10.));

        return makeCurve(ctx, points, false);
    }

    static Point positionAt(const BezierPtr& curve,
                            double u)
    {
        return SplineWarpCurveShape::fromBezier(*curve, kTime).pointAt(u);
    }
};

TEST_F(SplineWarpModelTest, TheHostGetsASplineWarpContext)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);

    ASSERT_TRUE(bool(host));
    ASSERT_TRUE(bool(ctx));
}

TEST_F(SplineWarpModelTest, LinkingTwoCurvesCreatesALink)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(b));
    EXPECT_EQ(eSplineWarpCurveRoleUnlinked, ctx->getRole(a));

    SplineWarpLinkPtr link = ctx->link(a, b, true);
    ASSERT_TRUE(bool(link));

    EXPECT_EQ(a, link->getSource());
    EXPECT_EQ(b, link->getDestination());
    EXPECT_EQ(0., link->getStartOffset());
    EXPECT_FALSE(link->isReversed());
    EXPECT_EQ(1., link->getAmount(kTime));
    EXPECT_EQ(0, link->getCorrespondenceCount());
    EXPECT_EQ(eSplineWarpCurveRoleLinked, ctx->getRole(a));
    EXPECT_EQ(eSplineWarpCurveRoleLinked, ctx->getRole(b));
    EXPECT_EQ(link, ctx->getLink(a));
    EXPECT_EQ(link, ctx->getLink(b));
    ASSERT_EQ(std::size_t(1), ctx->getLinks().size());
}

TEST_F(SplineWarpModelTest, ASecondLinkOnACurveIsRefused)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    BezierPtr c = makeQuad(ctx, 200., 0., 80.);
    ASSERT_TRUE(bool(a) && bool(b) && bool(c));
    ASSERT_TRUE(bool(ctx->link(a, b, true)));

    EXPECT_THROW(ctx->link(a, c, true), std::invalid_argument);
    EXPECT_THROW(ctx->link(c, b, true), std::invalid_argument);
    EXPECT_THROW(ctx->link(c, c, true), std::invalid_argument);
    EXPECT_EQ(std::size_t(1), ctx->getLinks().size());
    EXPECT_EQ(eSplineWarpCurveRoleUnlinked, ctx->getRole(c));
}

TEST_F(SplineWarpModelTest, ABoundaryMustBeClosedAndUnlinked)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr open = makeOpenLine(ctx, 0., 0.);
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    BezierPtr boundary = makeQuad(ctx, -50., -50., 300.);
    ASSERT_TRUE(bool(open) && bool(a) && bool(b) && bool(boundary));
    ASSERT_TRUE(bool(ctx->link(a, b, true)));

    EXPECT_THROW(ctx->setBoundary(open, true), std::invalid_argument);
    EXPECT_FALSE(ctx->isBoundary(open));
    EXPECT_THROW(ctx->setBoundary(a, true), std::invalid_argument);

    ctx->setBoundary(boundary, true);
    EXPECT_TRUE(ctx->isBoundary(boundary));
    EXPECT_EQ(eSplineWarpCurveRoleBoundary, ctx->getRole(boundary));
    EXPECT_THROW(ctx->link(boundary, open, true), std::invalid_argument);

    // Opening a boundary clears its role.
    boundary->setCurveFinished(false);
    EXPECT_FALSE(ctx->isBoundary(boundary));
}

TEST_F(SplineWarpModelTest, InsertingAPointKeepsCorrespondencesInPlace)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    ASSERT_TRUE(bool(a) && bool(b));
    SplineWarpLinkPtr link = ctx->linkWithAlignment(a, b, 3.5, false);
    ASSERT_TRUE(bool(link));
    // One correspondence on each side of the split point, one on the split segment's far side,
    // and one on the closing segment.
    ASSERT_EQ(0, ctx->addCorrespondence(link, 1.3, 2.2));
    ASSERT_EQ(1, ctx->addCorrespondence(link, 0.5, 2.8));
    ASSERT_EQ(2, ctx->addCorrespondence(link, 3.6, 0.4));
    ASSERT_EQ(3, ctx->addCorrespondence(link, 2.0, 3.9));

    std::vector<Point> sourceBefore, destinationBefore;
    for (int i = 0; i < link->getCorrespondenceCount(); ++i) {
        double s = 0., d = 0.;
        ASSERT_TRUE(link->getCorrespondence(i, kTime, &s, &d));
        sourceBefore.push_back(positionAt(a, s));
        destinationBefore.push_back(positionAt(b, d));
    }
    const Point offsetBefore = positionAt(b, link->getStartOffset());

    b->addControlPointAfterIndex(2, 0.4);
    b->addControlPointAfterIndex(-1, 0.3);
    a->addControlPointAfterIndex(3, 0.7);
    a->addControlPointAfterIndex(0, 0.25);
    ASSERT_EQ(6, a->getControlPointsCount());
    ASSERT_EQ(6, b->getControlPointsCount());

    for (int i = 0; i < link->getCorrespondenceCount(); ++i) {
        double s = 0., d = 0.;
        ASSERT_TRUE(link->getCorrespondence(i, kTime, &s, &d));
        const Point sp = positionAt(a, s);
        const Point dp = positionAt(b, d);
        EXPECT_NEAR(sourceBefore[i].x, sp.x, 1e-6) << "correspondence " << i;
        EXPECT_NEAR(sourceBefore[i].y, sp.y, 1e-6) << "correspondence " << i;
        EXPECT_NEAR(destinationBefore[i].x, dp.x, 1e-6) << "correspondence " << i;
        EXPECT_NEAR(destinationBefore[i].y, dp.y, 1e-6) << "correspondence " << i;
    }
    const Point offsetAfter = positionAt(b, link->getStartOffset());
    EXPECT_NEAR(offsetBefore.x, offsetAfter.x, 1e-6);
    EXPECT_NEAR(offsetBefore.y, offsetAfter.y, 1e-6);
}

TEST_F(SplineWarpModelTest, RemovingAPointKeepsParametersOnTheirSegments)
{
    // Removal is not exact (the two segments merge), but points away from the removed one keep
    // their place.
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    ASSERT_TRUE(bool(a) && bool(b));
    SplineWarpLinkPtr link = ctx->link(a, b, true);
    ASSERT_TRUE(bool(link));
    ASSERT_EQ(0, ctx->addCorrespondence(link, 0.5, 0.5));
    ASSERT_EQ(1, ctx->addCorrespondence(link, 3.5, 3.5));

    double s = 0., d = 0.;
    ASSERT_TRUE(link->getCorrespondence(0, kTime, &s, &d));
    const Point before = positionAt(b, d);

    b->removeControlPointByIndex(2);
    ASSERT_EQ(3, b->getControlPointsCount());

    ASSERT_TRUE(link->getCorrespondence(0, kTime, &s, &d));
    EXPECT_DOUBLE_EQ(0.5, d);
    const Point after = positionAt(b, d);
    EXPECT_NEAR(before.x, after.x, 1e-6);
    EXPECT_NEAR(before.y, after.y, 1e-6);
    ASSERT_TRUE(link->getCorrespondence(1, kTime, &s, &d));
    EXPECT_DOUBLE_EQ(2.5, d);
    EXPECT_DOUBLE_EQ(3.5, s);
}

TEST_F(SplineWarpModelTest, ParameterRemapsFollowTheSplitAndMergedSegments)
{
    // Open, 4 points: split segment 1 at t = 0.5.
    EXPECT_DOUBLE_EQ(0.5, SplineWarpContext::paramAfterInsertion(0.5, 4, false, 1, 0.5));
    EXPECT_DOUBLE_EQ(1.5, SplineWarpContext::paramAfterInsertion(1.25, 4, false, 1, 0.5));
    EXPECT_DOUBLE_EQ(2.5, SplineWarpContext::paramAfterInsertion(1.75, 4, false, 1, 0.5));
    EXPECT_DOUBLE_EQ(3.5, SplineWarpContext::paramAfterInsertion(2.5, 4, false, 1, 0.5));
    EXPECT_DOUBLE_EQ(4., SplineWarpContext::paramAfterInsertion(3., 4, false, 1, 0.5));
    // Closed, 4 points: a point at the front splits the closing segment 3.
    EXPECT_DOUBLE_EQ(1.5, SplineWarpContext::paramAfterInsertion(0.5, 4, true, -1, 0.5));
    EXPECT_DOUBLE_EQ(4.5, SplineWarpContext::paramAfterInsertion(3.25, 4, true, -1, 0.5));
    EXPECT_DOUBLE_EQ(0.5, SplineWarpContext::paramAfterInsertion(3.75, 4, true, -1, 0.5));
    // Closed, 4 points: removing point 0 merges segments 3 and 0 into segment 2.
    EXPECT_DOUBLE_EQ(2.25, SplineWarpContext::paramAfterRemoval(3.5, 4, true, 0, 0.5));
    EXPECT_DOUBLE_EQ(2.75, SplineWarpContext::paramAfterRemoval(0.5, 4, true, 0, 0.5));
    EXPECT_DOUBLE_EQ(0.5, SplineWarpContext::paramAfterRemoval(1.5, 4, true, 0, 0.5));
    // Open, 4 points: removing an end point collapses its segment onto the new end.
    EXPECT_DOUBLE_EQ(0., SplineWarpContext::paramAfterRemoval(0.5, 4, false, 0, 0.5));
    EXPECT_DOUBLE_EQ(2., SplineWarpContext::paramAfterRemoval(2.5, 4, false, 3, 0.5));
}

TEST_F(SplineWarpModelTest, DeletingACurveRemovesItsLinkAndRole)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    BezierPtr boundary = makeQuad(ctx, -50., -50., 300.);
    ASSERT_TRUE(bool(a) && bool(b) && bool(boundary));
    ASSERT_TRUE(bool(ctx->link(a, b, true)));
    ctx->setBoundary(boundary, true);

    ctx->removeItem(b);
    EXPECT_TRUE(ctx->getLinks().empty());
    EXPECT_EQ(eSplineWarpCurveRoleUnlinked, ctx->getRole(a));

    ctx->removeItem(boundary);
    EXPECT_TRUE(ctx->getBoundaries().empty());
}

TEST_F(SplineWarpModelTest, EveryEditChangesTheHash)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    BezierPtr boundary = makeQuad(ctx, -50., -50., 300.);
    ASSERT_TRUE(bool(a) && bool(b) && bool(boundary));

    std::vector<U64> contextHashes;
    std::vector<U64> nodeHashes;
    const auto record = [&]() {
        contextHashes.push_back(ctx->getContentHash());
        nodeHashes.push_back(host->getHashValue());
    };

    record();
    SplineWarpLinkPtr link = ctx->link(a, b, true);
    ASSERT_TRUE(bool(link));
    record();
    ctx->setLinkAlignment(link, 1.5, false);
    record();
    ctx->setLinkAlignment(link, 1.5, true);
    record();
    ctx->setLinkAmount(link, 0.5, kTime);
    record();
    ctx->setLinkAmountAtTime(link, 10., 0.25);
    record();
    const std::size_t beforeCorrespondence = contextHashes.size() - 1;
    ASSERT_EQ(0, ctx->addCorrespondence(link, 1., 2.));
    record();
    ctx->setCorrespondence(link, 0, 1.5, 2., kTime);
    record();
    ctx->setCorrespondenceAtTime(link, 0, 5., 1.5, 2.5);
    record();
    ctx->removeCorrespondence(link, 0);
    record();
    const std::size_t beforeBoundary = contextHashes.size() - 1;
    ctx->setBoundary(boundary, true);
    record();
    ctx->setBoundary(boundary, false);
    record();
    ctx->unlink(a);
    record();

    for (std::size_t i = 1; i < contextHashes.size(); ++i) {
        EXPECT_NE(contextHashes[i - 1], contextHashes[i]) << "edit " << i;
        EXPECT_NE(nodeHashes[i - 1], nodeHashes[i]) << "edit " << i;
    }
    // Undoing an edit restores the earlier warp model, so it restores the earlier hash.
    EXPECT_EQ(contextHashes[beforeCorrespondence], contextHashes[beforeBoundary]);
    EXPECT_EQ(contextHashes[beforeBoundary], contextHashes[beforeBoundary + 2]);
    EXPECT_EQ(contextHashes.front(), contextHashes.back());
    EXPECT_EQ(nodeHashes[beforeCorrespondence], nodeHashes[beforeBoundary]);
    EXPECT_EQ(nodeHashes[beforeBoundary], nodeHashes[beforeBoundary + 2]);
    EXPECT_EQ(nodeHashes.front(), nodeHashes.back());
    EXPECT_EQ(std::size_t(10), std::set<U64>(contextHashes.begin(), contextHashes.end()).size());
}

TEST_F(SplineWarpModelTest, SaveAndLoadRoundTripsTheModel)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    BezierPtr c = makeOpenLine(ctx, 0., 200.);
    BezierPtr d = makeOpenLine(ctx, 10., 230.);
    std::vector<std::pair<double, double>> single;
    single.push_back(std::make_pair(400., 400.));
    BezierPtr pinSource = makeCurve(ctx, single, false);
    single[0] = std::make_pair(420., 390.);
    BezierPtr pinDestination = makeCurve(ctx, single, false);
    BezierPtr boundary = makeQuad(ctx, -50., -50., 600.);
    BezierPtr soft = makeOpenLine(ctx, 0., 300.);
    ASSERT_TRUE(bool(a) && bool(b) && bool(c) && bool(d) && bool(pinSource) && bool(pinDestination) && bool(boundary) && bool(soft));

    SplineWarpLinkPtr closedLink = ctx->linkWithAlignment(a, b, 1.5, true);
    ASSERT_TRUE(bool(closedLink));
    ctx->setLinkAmountAtTime(closedLink, 1., 0.25);
    ctx->setLinkAmountAtTime(closedLink, 20., 0.75);
    ASSERT_EQ(0, ctx->addCorrespondence(closedLink, 0.5, 1.));
    ctx->setCorrespondenceAtTime(closedLink, 0, 1., 0.5, 1.);
    ctx->setCorrespondenceAtTime(closedLink, 0, 10., 0.75, 1.5);
    ASSERT_EQ(1, ctx->addCorrespondence(closedLink, 2.25, 3.5));

    SplineWarpLinkPtr openLink = ctx->linkWithAlignment(c, d, 0., true);
    ASSERT_TRUE(bool(openLink));
    ctx->setLinkAmount(openLink, 0.6, kTime);

    SplineWarpLinkPtr pinLink = ctx->link(pinSource, pinDestination, false);
    ASSERT_TRUE(bool(pinLink));
    ASSERT_EQ(0, ctx->addCorrespondence(pinLink, 0., 0.));

    ctx->setBoundary(boundary, true);
    ctx->setSoftBoundary(soft, true);

    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(bool(layer));
    layer->addItem(d);
    ctx->setLayerAmountAtTime(layer, 1., 0.25);
    ctx->setLayerAmountAtTime(layer, 20., 0.75);

    Serialization::RotoContextSerialization saved;
    ctx->toSerialization(&saved);
    ASSERT_TRUE(saved.hasSplineWarp);
    ASSERT_EQ(std::size_t(3), saved.splineWarp.links.size());
    ASSERT_EQ(std::size_t(1), saved.splineWarp.boundaries.size());
    ASSERT_EQ(std::size_t(1), saved.splineWarp.softBoundaries.size());
    ASSERT_EQ(std::size_t(1), saved.splineWarp.layerAmounts.size());

    const std::string hostName = host->getScriptName();
    const U64 savedHash = ctx->getContentHash();
    const std::string aName = a->getScriptName(), bName = b->getScriptName(), cName = c->getScriptName(), dName = d->getScriptName();
    const std::string pinSourceName = pinSource->getScriptName(), pinDestinationName = pinDestination->getScriptName();
    const std::string boundaryName = boundary->getScriptName();
    const std::string softName = soft->getScriptName(), layerName = layer->getScriptName();
    struct SavedCorrespondence {
        double time, source, destination;
    };
    std::vector<SavedCorrespondence> closedCorrespondences;
    for (double t = 1.; t <= 10.; t += 3.) {
        double s = 0., dd = 0.;
        ASSERT_TRUE(closedLink->getCorrespondence(0, t, &s, &dd));
        closedCorrespondences.push_back({ t, s, dd });
    }

    std::string yaml;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("splinewarp.ntpz"), &yaml));
    EXPECT_NE(std::string::npos, yaml.find("splineWarp"));
    host.reset();
    ctx.reset();
    a.reset();
    b.reset();
    c.reset();
    d.reset();
    pinSource.reset();
    pinDestination.reset();
    boundary.reset();
    soft.reset();
    layer.reset();
    closedLink.reset();
    openLink.reset();
    pinLink.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("splinewarp.ntpz")));

    NodePtr loaded = project->getNodeByName(hostName);
    ASSERT_TRUE(bool(loaded));
    SplineWarpContextPtr loadedCtx = std::dynamic_pointer_cast<SplineWarpContext>(loaded->getCurveContext());
    ASSERT_TRUE(bool(loadedCtx));

    Serialization::RotoContextSerialization reloaded;
    loadedCtx->toSerialization(&reloaded);
    EXPECT_TRUE(reloaded.splineWarp == saved.splineWarp);
    EXPECT_EQ(savedHash, loadedCtx->getContentHash());

    const auto curveNamed = [&](const std::string& name) {
        return std::dynamic_pointer_cast<Bezier>(loadedCtx->getItemByName(name));
    };

    const std::vector<SplineWarpLinkPtr> links = loadedCtx->getLinks();
    ASSERT_EQ(std::size_t(3), links.size());

    const SplineWarpLinkPtr& lc = links[0];
    ASSERT_TRUE(bool(lc->getSource()) && bool(lc->getDestination()));
    EXPECT_EQ(aName, lc->getSource()->getScriptName());
    EXPECT_EQ(bName, lc->getDestination()->getScriptName());
    EXPECT_EQ(1.5, lc->getStartOffset());
    EXPECT_TRUE(lc->isReversed());
    EXPECT_TRUE(lc->getAmountValue().isAnimated());
    EXPECT_DOUBLE_EQ(0.25, lc->getAmount(1.));
    EXPECT_DOUBLE_EQ(0.75, lc->getAmount(20.));
    ASSERT_EQ(2, lc->getCorrespondenceCount());
    for (std::size_t i = 0; i < closedCorrespondences.size(); ++i) {
        double s = 0., dd = 0.;
        ASSERT_TRUE(lc->getCorrespondence(0, closedCorrespondences[i].time, &s, &dd));
        EXPECT_DOUBLE_EQ(closedCorrespondences[i].source, s);
        EXPECT_DOUBLE_EQ(closedCorrespondences[i].destination, dd);
    }
    {
        double s = 0., dd = 0.;
        ASSERT_TRUE(lc->getCorrespondence(1, kTime, &s, &dd));
        EXPECT_EQ(2.25, s);
        EXPECT_EQ(3.5, dd);
        EXPECT_FALSE(lc->getCorrespondences()[1].source.isAnimated());
    }

    const SplineWarpLinkPtr& lo = links[1];
    ASSERT_TRUE(bool(lo->getSource()) && bool(lo->getDestination()));
    EXPECT_EQ(cName, lo->getSource()->getScriptName());
    EXPECT_EQ(dName, lo->getDestination()->getScriptName());
    EXPECT_EQ(0., lo->getStartOffset());
    EXPECT_TRUE(lo->isReversed());
    EXPECT_FALSE(lo->getAmountValue().isAnimated());
    EXPECT_EQ(0.6, lo->getAmount(kTime));
    EXPECT_EQ(0, lo->getCorrespondenceCount());

    const SplineWarpLinkPtr& lp = links[2];
    ASSERT_TRUE(bool(lp->getSource()) && bool(lp->getDestination()));
    EXPECT_EQ(pinSourceName, lp->getSource()->getScriptName());
    EXPECT_EQ(pinDestinationName, lp->getDestination()->getScriptName());
    EXPECT_EQ(1, lp->getSource()->getControlPointsCount());
    EXPECT_EQ(1, lp->getCorrespondenceCount());

    BezierPtr loadedBoundary = curveNamed(boundaryName);
    ASSERT_TRUE(bool(loadedBoundary));
    EXPECT_TRUE(loadedCtx->isBoundary(loadedBoundary));
    EXPECT_EQ(eSplineWarpCurveRoleLinked, loadedCtx->getRole(curveNamed(aName)));
    BezierPtr loadedSoft = curveNamed(softName);
    ASSERT_TRUE(bool(loadedSoft));
    EXPECT_TRUE(loadedCtx->isSoftBoundary(loadedSoft));
    EXPECT_EQ(eSplineWarpCurveRoleSoftBoundary, loadedCtx->getRole(loadedSoft));

    RotoLayerPtr loadedLayer = std::dynamic_pointer_cast<RotoLayer>(loadedCtx->getItemByName(layerName));
    ASSERT_TRUE(bool(loadedLayer));
    EXPECT_TRUE(loadedCtx->getLayerAmountValue(loadedLayer).isAnimated());
    EXPECT_DOUBLE_EQ(0.25, loadedCtx->getLayerAmount(loadedLayer, 1.));
    EXPECT_DOUBLE_EQ(0.75, loadedCtx->getLayerAmount(loadedLayer, 20.));
    EXPECT_DOUBLE_EQ(0.25, loadedCtx->getInheritedLayerAmount(curveNamed(dName), 1.));
}

// Each pair's destination, for comparing sample sets.
static std::vector<Point>
destinationsOf(const SplineWarpSampleSet& set)
{
    std::vector<Point> points;

    for (std::size_t i = 0; i < set.pairs.size(); ++i) {
        points.push_back(set.pairs[i].destination);
    }

    return points;
}

static void
expectSameDestinations(const SplineWarpSampleSet& a,
                       const SplineWarpSampleSet& b)
{
    ASSERT_EQ(a.pairs.size(), b.pairs.size());
    const std::vector<Point> pa = destinationsOf(a), pb = destinationsOf(b);
    for (std::size_t i = 0; i < pa.size(); ++i) {
        EXPECT_NEAR(pa[i].x, pb[i].x, 1e-9) << i;
        EXPECT_NEAR(pa[i].y, pb[i].y, 1e-9) << i;
    }
}

TEST_F(SplineWarpModelTest, LayerAmountsScaleTheirLinksAndNestingMultiplies)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr source = makeOpenLine(ctx, 0., 0.);
    BezierPtr destination = makeOpenLine(ctx, 0., 40.);
    ASSERT_TRUE(source && destination);
    SplineWarpLinkPtr link = ctx->linkWithAlignment(source, destination, 0., false);
    ASSERT_TRUE(bool(link));
    SplineWarpSamplingSettings settings;
    settings.time = kTime;

    RotoLayerPtr outer = ctx->addLayer();
    RotoLayerPtr inner = ctx->addLayer();
    ASSERT_TRUE(outer && inner);
    outer->addItem(inner);
    inner->addItem(destination);
    EXPECT_EQ(1., ctx->getLayerAmount(inner, kTime));
    const U64 hashBefore = ctx->getContentHash();

    ctx->setLinkAmount(link, 0.5, kTime);
    const SplineWarpSampleSet byLinkHalf = sampleSplineWarp(*ctx, settings);
    ctx->setLinkAmount(link, 0.25, kTime);
    const SplineWarpSampleSet byLinkQuarter = sampleSplineWarp(*ctx, settings);
    ctx->setLinkAmount(link, 1., kTime);
    EXPECT_EQ(hashBefore, ctx->getContentHash());

    ctx->setLayerAmount(inner, 0.5, kTime);
    EXPECT_NE(hashBefore, ctx->getContentHash());
    EXPECT_DOUBLE_EQ(0.5, ctx->getInheritedLayerAmount(destination, kTime));
    EXPECT_DOUBLE_EQ(1., ctx->getInheritedLayerAmount(source, kTime));
    expectSameDestinations(byLinkHalf, sampleSplineWarp(*ctx, settings));

    ctx->setLayerAmount(outer, 0.5, kTime);
    EXPECT_DOUBLE_EQ(0.25, ctx->getInheritedLayerAmount(destination, kTime));
    expectSameDestinations(byLinkQuarter, sampleSplineWarp(*ctx, settings));

    // Amounts are clamped to 0..1, and a layer of another node is refused.
    ctx->setLayerAmount(outer, 2., kTime);
    EXPECT_EQ(1., ctx->getLayerAmount(outer, kTime));
    NodePtr otherHost;
    SplineWarpContextPtr other = makeHost(&otherHost);
    ASSERT_TRUE(bool(other));
    RotoLayerPtr foreign = other->getOrCreateBaseLayer();
    ASSERT_TRUE(bool(foreign));
    EXPECT_THROW(ctx->setLayerAmount(foreign, 0.5, kTime), std::invalid_argument);

    // Removing the layer drops its amount.
    ctx->removeItem(inner, RotoItem::eSelectionReasonOther);
    Serialization::RotoContextSerialization s;
    ctx->toSerialization(&s);
    for (std::list<Serialization::SplineWarpLayerAmountSerialization>::const_iterator it = s.splineWarp.layerAmounts.begin(); it != s.splineWarp.layerAmounts.end(); ++it) {
        EXPECT_NE(inner->getScriptName(), it->layer);
    }
}

TEST_F(SplineWarpModelTest, TheAmountKnobsFollowTheSelection)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext());
    KnobDoublePtr layerAmount = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamLayerAmount));
    KnobDoublePtr linkAmount = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamLinkAmount));
    ASSERT_TRUE(ctx && layerAmount && linkAmount);

    BezierPtr a = makeOpenLine(ctx, 0., 0.);
    BezierPtr b = makeOpenLine(ctx, 0., 40.);
    BezierPtr loose = makeOpenLine(ctx, 0., 80.);
    ASSERT_TRUE(a && b && loose);
    SplineWarpLinkPtr link = ctx->linkWithAlignment(a, b, 0., false);
    ASSERT_TRUE(bool(link));
    ctx->setLinkAmount(link, 0.4, kTime);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(bool(layer));
    layer->addItem(b);
    ctx->setLayerAmount(layer, 0.6, kTime);

    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(layerAmount->isEnabled(0));
    EXPECT_FALSE(linkAmount->isEnabled(0));

    // A linked curve: Link Amount shows its link's amount.
    ctx->select(b, RotoItem::eSelectionReasonOther);
    EXPECT_TRUE(linkAmount->isEnabled(0));
    EXPECT_DOUBLE_EQ(0.4, linkAmount->getValue());
    EXPECT_FALSE(layerAmount->isEnabled(0));
    // An unlinked curve too: disabled.
    ctx->select(loose, RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(linkAmount->isEnabled(0));
    ctx->deselect(loose, RotoItem::eSelectionReasonOther);
    EXPECT_TRUE(linkAmount->isEnabled(0));

    // An edit keys the link.
    linkAmount->setValueAtTime(5., 0.8, ViewSpec::all(), 0);
    EXPECT_TRUE(link->getAmountValue().isAnimated());
    EXPECT_DOUBLE_EQ(0.8, link->getAmount(5.));

    // Selecting the layer selects the curve inside it: Layer Amount shows the layer's amount.
    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    ctx->select(layer, RotoItem::eSelectionReasonOther);
    EXPECT_TRUE(layerAmount->isEnabled(0));
    EXPECT_DOUBLE_EQ(0.6, layerAmount->getValue());
    layerAmount->setValue(0.3);
    EXPECT_DOUBLE_EQ(0.3, ctx->getLayerAmount(layer, kTime));
    EXPECT_FALSE(layerAmount->getIsPersistent());

    // A curve outside the layer disables it.
    ctx->select(loose, RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(layerAmount->isEnabled(0));
}

TEST_F(SplineWarpModelTest, LoadDropsALinkWhoseCurveIsMissing)
{
    NodePtr host;
    SplineWarpContextPtr ctx = makeHost(&host);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 0., 0., 100.);
    BezierPtr b = makeQuad(ctx, 30., 20., 90.);
    BezierPtr c = makeQuad(ctx, 200., 0., 80.);
    BezierPtr e = makeQuad(ctx, 300., 0., 80.);
    ASSERT_TRUE(bool(a) && bool(b) && bool(c) && bool(e));
    ASSERT_TRUE(bool(ctx->link(a, b, true)));
    ASSERT_TRUE(bool(ctx->link(c, e, true)));

    Serialization::RotoContextSerialization s;
    ctx->toSerialization(&s);
    ASSERT_EQ(std::size_t(2), s.splineWarp.links.size());
    s.splineWarp.links.front().destination = "NoSuchCurve";

    // Loaded into a second host, whose context holds its base layer only, as a loading node's does.
    NodePtr other;
    SplineWarpContextPtr otherCtx = makeHost(&other);
    ASSERT_TRUE(bool(otherCtx));
    otherCtx->fromSerialization(s);

    const std::vector<SplineWarpLinkPtr> links = otherCtx->getLinks();
    ASSERT_EQ(std::size_t(1), links.size());
    ASSERT_TRUE(bool(links[0]->getSource()));
    EXPECT_EQ(c->getScriptName(), links[0]->getSource()->getScriptName());
    EXPECT_NE(c, links[0]->getSource());
}
