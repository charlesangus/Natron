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

#include <list>
#include <string>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTolerance = 1e-9;

BezierPtr
makeQuad(const RotoCurveContextPtr& ctx,
         double x,
         double y,
         double size,
         double time)
{
    BezierPtr bezier = ctx->makeBezier(x, y, kRotoBezierBaseName, time, false);
    if (!bezier) {
        return bezier;
    }
    bezier->addControlPoint(x + size, y, time);
    bezier->addControlPoint(x + size * 1.2, y + size, time);
    bezier->addControlPoint(x - size * 0.1, y + size * 0.8, time);
    bezier->setCurveFinished(true);

    return bezier;
}

BezierPtr
asCurve(const RotoItemPtr& item)
{
    return std::dynamic_pointer_cast<Bezier>(item);
}

// Same points, tangents and feathers at time.
void
expectSameShape(const BezierPtr& a,
                const BezierPtr& b,
                double time)
{
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->getControlPointsCount(), b->getControlPointsCount());
    for (int i = 0; i < a->getControlPointsCount(); ++i) {
        BezierCPPtr pa = a->getControlPointAtIndex(i);
        BezierCPPtr pb = b->getControlPointAtIndex(i);
        ASSERT_TRUE(pa && pb);
        double ax, ay, bx, by;
        pa->getPositionAtTime(false, time, ViewIdx(0), &ax, &ay);
        pb->getPositionAtTime(false, time, ViewIdx(0), &bx, &by);
        EXPECT_NEAR(ax, bx, kTolerance) << "point " << i;
        EXPECT_NEAR(ay, by, kTolerance) << "point " << i;
        pa->getLeftBezierPointAtTime(false, time, ViewIdx(0), &ax, &ay, false);
        pb->getLeftBezierPointAtTime(false, time, ViewIdx(0), &bx, &by, false);
        EXPECT_NEAR(ax, bx, kTolerance) << "left tangent " << i;
        EXPECT_NEAR(ay, by, kTolerance) << "left tangent " << i;
        BezierCPPtr fa = a->getFeatherPointAtIndex(i);
        BezierCPPtr fb = b->getFeatherPointAtIndex(i);
        ASSERT_TRUE(fa && fb);
        fa->getPositionAtTime(false, time, ViewIdx(0), &ax, &ay);
        fb->getPositionAtTime(false, time, ViewIdx(0), &bx, &by);
        EXPECT_NEAR(ax, bx, kTolerance) << "feather " << i;
        EXPECT_NEAR(ay, by, kTolerance) << "feather " << i;
    }
    EXPECT_EQ(a->isCurveFinished(), b->isCurveFinished());
}

} // namespace

class CurveClipboardTest
    : public BaseTest {
protected:
    void resetProject()
    {
        getApp()->getProject()->reset(false, true);
    }

    SplineWarpContextPtr makeSplineWarp(NodePtr* node = 0)
    {
        NodePtr warp = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
        EXPECT_TRUE(bool(warp));
        if (!warp) {
            return SplineWarpContextPtr();
        }
        if (node) {
            *node = warp;
        }

        return std::dynamic_pointer_cast<SplineWarpContext>(warp->getCurveContext());
    }

    RotoCurveContextPtr makeRoto()
    {
        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        EXPECT_TRUE(bool(roto));

        return roto ? roto->getCurveContext() : RotoCurveContextPtr();
    }
};

// A Roto shape pastes into a SplineWarp as an unlinked curve of the same shape, and its track
// link follows it, found again by tracker and track name.
TEST_F(CurveClipboardTest, RotoShapePastesIntoSplineWarpUnlinked)
{
    resetProject();
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));
    TrackMarkerPtr marker = tracker->getTrackerContext()->createMarker();
    ASSERT_TRUE(bool(marker));
    marker->getCenterKnob()->setValueAtTime(1., 30., ViewSpec::all(), 0);
    marker->getCenterKnob()->setValueAtTime(1., 20., ViewSpec::all(), 1);
    marker->getCenterKnob()->setValueAtTime(9., 38., ViewSpec::all(), 0);
    marker->getCenterKnob()->setValueAtTime(9., 24., ViewSpec::all(), 1);

    RotoCurveContextPtr roto = makeRoto();
    ASSERT_TRUE(bool(roto));
    const double t0 = roto->getTimelineCurrentTime();
    BezierPtr square = roto->makeSquare(10., 10., 12., t0);
    ASSERT_TRUE(bool(square));
    square->moveFeatherByIndex(1, t0, 2., -3.);
    square->linkPointToTrack(2, marker, t0);

    const std::string blob = roto->serializeItems(std::list<RotoItemPtr>(1, square));
    ASSERT_FALSE(blob.empty());

    SplineWarpContextPtr warp = makeSplineWarp();
    ASSERT_TRUE(bool(warp));
    EXPECT_EQ(1, warp->countPastableItems(blob));
    std::list<RotoItemPtr> pasted = warp->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(1u, pasted.size());
    BezierPtr copy = asCurve(pasted.front());
    ASSERT_TRUE(bool(copy));
    EXPECT_EQ(square->getScriptName(), copy->getScriptName());
    EXPECT_EQ(copy, asCurve(warp->getItemByName(copy->getScriptName())));
    EXPECT_EQ(eSplineWarpCurveRoleUnlinked, warp->getRole(copy));
    EXPECT_TRUE(warp->getLinks().empty());
    EXPECT_TRUE(warp->getBoundaries().empty());

    expectSameShape(square, copy, t0);
    expectSameShape(square, copy, 5.);
    BezierCPPtr linked = copy->getControlPointAtIndex(2);
    ASSERT_TRUE(bool(linked));
    EXPECT_TRUE(linked->isLinkedToTrack());
    EXPECT_EQ(marker, linked->getResolvedTrack());

    // A second paste into the same host takes new names.
    std::list<RotoItemPtr> again = warp->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(1u, again.size());
    EXPECT_NE(copy->getScriptName(), again.front()->getScriptName());
}

// A linked SplineWarp pair pastes into another SplineWarp with its link, alignment and amount,
// and into the same SplineWarp as a second linked pair under new names.
TEST_F(CurveClipboardTest, SplineWarpPairPastesIntoSplineWarpLinked)
{
    resetProject();
    SplineWarpContextPtr from = makeSplineWarp();
    ASSERT_TRUE(bool(from));
    const double t0 = from->getTimelineCurrentTime();
    BezierPtr source = makeQuad(from, 20., 15., 10., t0);
    BezierPtr destination = makeQuad(from, 24., 18., 10., t0);
    BezierPtr boundary = makeQuad(from, 2., 2., 40., t0);
    ASSERT_TRUE(source && destination && boundary);
    SplineWarpLinkPtr link = from->link(source, destination);
    ASSERT_TRUE(bool(link));
    from->setLinkAmount(link, 0.25, t0);
    from->setBoundary(boundary, true);

    std::list<RotoItemPtr> items;
    items.push_back(source);
    items.push_back(destination);
    items.push_back(boundary);
    const std::string blob = from->serializeItems(items);

    SplineWarpContextPtr to = makeSplineWarp();
    ASSERT_TRUE(bool(to));
    std::list<RotoItemPtr> pasted = to->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(3u, pasted.size());
    BezierPtr pastedSource = asCurve(to->getItemByName(source->getScriptName()));
    BezierPtr pastedDestination = asCurve(to->getItemByName(destination->getScriptName()));
    BezierPtr pastedBoundary = asCurve(to->getItemByName(boundary->getScriptName()));
    ASSERT_TRUE(pastedSource && pastedDestination && pastedBoundary);
    expectSameShape(source, pastedSource, t0);
    expectSameShape(destination, pastedDestination, t0);

    ASSERT_EQ(1u, to->getLinks().size());
    SplineWarpLinkPtr pastedLink = to->getLinks().front();
    EXPECT_EQ(pastedSource, pastedLink->getSource());
    EXPECT_EQ(pastedDestination, pastedLink->getDestination());
    EXPECT_NEAR(link->getStartOffset(), pastedLink->getStartOffset(), kTolerance);
    EXPECT_EQ(link->isReversed(), pastedLink->isReversed());
    EXPECT_NEAR(0.25, pastedLink->getAmount(t0), kTolerance);
    EXPECT_TRUE(to->isBoundary(pastedBoundary));

    // Into the source host: the copies are renamed and linked to each other, the originals keep
    // their own link.
    std::list<RotoItemPtr> copies = from->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(3u, copies.size());
    ASSERT_EQ(2u, from->getLinks().size());
    EXPECT_EQ(link, from->getLink(source));
    BezierPtr copySource = asCurve(copies.front());
    ASSERT_TRUE(bool(copySource));
    EXPECT_NE(source, copySource);
    EXPECT_NE(source->getScriptName(), copySource->getScriptName());
    SplineWarpLinkPtr copyLink = from->getLink(copySource);
    ASSERT_TRUE(bool(copyLink));
    EXPECT_NE(link, copyLink);
    EXPECT_EQ(copySource, copyLink->getSource());
    EXPECT_NE(destination, copyLink->getDestination());
    EXPECT_EQ(2u, from->getBoundaries().size());
}

// SplineWarp curves paste into a Roto as plain shapes: the Roto keeps no warp data.
TEST_F(CurveClipboardTest, SplineWarpPairPastesIntoRotoWithoutWarpData)
{
    resetProject();
    SplineWarpContextPtr from = makeSplineWarp();
    ASSERT_TRUE(bool(from));
    const double t0 = from->getTimelineCurrentTime();
    BezierPtr source = makeQuad(from, 20., 15., 10., t0);
    BezierPtr destination = makeQuad(from, 24., 18., 10., t0);
    ASSERT_TRUE(source && destination);
    ASSERT_TRUE(bool(from->link(source, destination)));

    std::list<RotoItemPtr> items;
    items.push_back(source);
    items.push_back(destination);
    const std::string blob = from->serializeItems(items);

    RotoCurveContextPtr roto = makeRoto();
    ASSERT_TRUE(bool(roto));
    EXPECT_FALSE(bool(std::dynamic_pointer_cast<SplineWarpContext>(roto)));
    const U64 hashBefore = roto->getContentHash();
    std::list<RotoItemPtr> pasted = roto->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(2u, pasted.size());
    EXPECT_NE(hashBefore, roto->getContentHash());
    expectSameShape(source, asCurve(roto->getItemByName(source->getScriptName())), t0);
    expectSameShape(destination, asCurve(roto->getItemByName(destination->getScriptName())), t0);

    // Copied back out of the Roto, the curves carry no link.
    std::list<RotoItemPtr> back;
    back.push_back(roto->getItemByName(source->getScriptName()));
    back.push_back(roto->getItemByName(destination->getScriptName()));
    SplineWarpContextPtr to = makeSplineWarp();
    ASSERT_TRUE(bool(to));
    ASSERT_EQ(2u, to->pasteItems(roto->serializeItems(back), RotoLayerPtr()).size());
    EXPECT_TRUE(to->getLinks().empty());
}

// A curve copied without its link partner pastes unlinked.
TEST_F(CurveClipboardTest, OneEndedLinkPastesUnlinked)
{
    resetProject();
    SplineWarpContextPtr from = makeSplineWarp();
    ASSERT_TRUE(bool(from));
    const double t0 = from->getTimelineCurrentTime();
    BezierPtr source = makeQuad(from, 20., 15., 10., t0);
    BezierPtr destination = makeQuad(from, 24., 18., 10., t0);
    ASSERT_TRUE(source && destination);
    ASSERT_TRUE(bool(from->link(source, destination)));

    const std::string blob = from->serializeItems(std::list<RotoItemPtr>(1, destination));

    SplineWarpContextPtr to = makeSplineWarp();
    ASSERT_TRUE(bool(to));
    std::list<RotoItemPtr> pasted = to->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(1u, pasted.size());
    BezierPtr copy = asCurve(pasted.front());
    ASSERT_TRUE(bool(copy));
    EXPECT_EQ(eSplineWarpCurveRoleUnlinked, to->getRole(copy));
    EXPECT_TRUE(to->getLinks().empty());
    expectSameShape(destination, copy, t0);

    // Pasted back into its own host, the copy is unlinked and the original pair is untouched.
    std::list<RotoItemPtr> again = from->pasteItems(blob, RotoLayerPtr());
    ASSERT_EQ(1u, again.size());
    EXPECT_EQ(1u, from->getLinks().size());
    EXPECT_EQ(eSplineWarpCurveRoleUnlinked, from->getRole(asCurve(again.front())));
    EXPECT_EQ(eSplineWarpCurveRoleLinked, from->getRole(destination));
}

// Text that is not a curves clipboard is rejected.
TEST_F(CurveClipboardTest, UnreadableTextIsRejected)
{
    resetProject();
    RotoCurveContextPtr roto = makeRoto();
    ASSERT_TRUE(bool(roto));
    EXPECT_EQ(0, roto->countPastableItems("{not: [curves"));
    EXPECT_THROW(roto->pasteItems("{not: [curves", RotoLayerPtr()), std::invalid_argument);
}
