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
#include <set>
#include <string>
#include <vector>

#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const double kTolerance = 1e-9;
const int kFirstKey = 1;
const int kLastKey = 10;

// The track's centre at a key frame: a straight line, so the move between two frames does not
// depend on where the offset was taken.
double
trackX(double frame)
{
    return 20. + frame;
}

double
trackY(double frame)
{
    return 15. + 0.5 * frame;
}

struct PointState {
    double x, y, lx, ly, rx, ry;
};

PointState
pointAt(const BezierCPPtr& cp,
        double time)
{
    PointState s;
    cp->getPositionAtTime(false, time, ViewIdx(0), &s.x, &s.y);
    cp->getLeftBezierPointAtTime(false, time, ViewIdx(0), &s.lx, &s.ly, false);
    cp->getRightBezierPointAtTime(false, time, ViewIdx(0), &s.rx, &s.ry, false);

    return s;
}

std::size_t
countDiffering(const std::vector<RenderedPlane>& a,
               const std::vector<RenderedPlane>& b)
{
    if ((a.size() != 1) || (b.size() != 1) || (a[0].pixels.size() != b[0].pixels.size())) {
        return std::size_t(-1);
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < a[0].pixels.size(); ++i) {
        if (a[0].pixels[i] != b[0].pixels[i]) {
            ++n;
        }
    }

    return n;
}

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

const char* const kPythonHelpers = "def raises(f):\n"
                                   "    try:\n"
                                   "        f()\n"
                                   "    except ValueError:\n"
                                   "        return True\n"
                                   "    return False\n";

} // namespace

class CurveTrackLinkTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "curveTrackLink64x48", 1.));
    }

    // A Tracker with one track keyed on every frame of [kFirstKey, kLastKey].
    TrackMarkerPtr makeTrack(NodePtr* trackerNode = 0)
    {
        NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        EXPECT_TRUE(bool(tracker));
        if (!tracker) {
            return TrackMarkerPtr();
        }
        TrackerContextPtr context = tracker->getTrackerContext();
        EXPECT_TRUE(bool(context));
        if (!context) {
            return TrackMarkerPtr();
        }
        TrackMarkerPtr marker = context->createMarker();
        EXPECT_TRUE(bool(marker));
        if (!marker) {
            return marker;
        }
        KnobDoublePtr centre = marker->getCenterKnob();
        for (int f = kFirstKey; f <= kLastKey; ++f) {
            centre->setValueAtTime(f, trackX(f), ViewSpec::all(), 0);
            centre->setValueAtTime(f, trackY(f), ViewSpec::all(), 1);
        }
        if (trackerNode) {
            *trackerNode = tracker;
        }

        return marker;
    }

    void centreAt(const TrackMarkerPtr& marker,
                  double time,
                  double* x,
                  double* y)
    {
        KnobDoublePtr centre = marker->getCenterKnob();
        *x = centre->getValueAtTime(time, 0);
        *y = centre->getValueAtTime(time, 1);
    }

    std::string pythonName(const NodePtr& node)
    {
        return getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
    }
};

// A linked point is the track's centre plus the offset it had at the link frame, at key frames
// and between them; its tangents and feather point keep their offsets from it, and the other
// points do not move.
TEST_F(CurveTrackLinkTest, PointFollowsAKeyedTrackWithItsOffsetAndTangents)
{
    resetProject();
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoCurveContextPtr context = roto->getCurveContext();
    ASSERT_TRUE(bool(context));
    const double t0 = context->getTimelineCurrentTime();
    BezierPtr square = context->makeSquare(10., 10., 12., t0);
    ASSERT_TRUE(bool(square));
    // Pull the feather out so it is distinct from its point.
    square->moveFeatherByIndex(0, t0, -2., -3.);

    BezierCPPtr cp = square->getControlPointAtIndex(0);
    BezierCPPtr fp = square->getFeatherPointAtIndex(0);
    BezierCPPtr other = square->getControlPointAtIndex(1);
    ASSERT_TRUE(cp && fp && other);
    const PointState before = pointAt(cp, t0);
    const PointState featherBefore = pointAt(fp, t0);
    const PointState otherBefore = pointAt(other, t0);
    double c0x, c0y;
    centreAt(marker, t0, &c0x, &c0y);

    square->linkPointToTrack(0, marker, t0);
    EXPECT_TRUE(cp->isLinkedToTrack());
    EXPECT_TRUE(fp->isLinkedToTrack());
    EXPECT_FALSE(other->isLinkedToTrack());
    EXPECT_EQ(marker, cp->getResolvedTrack());
    EXPECT_EQ(marker->getScriptName_mt_safe(), cp->getTrackLinkTrackName());

    const PointState atLink = pointAt(cp, t0);
    EXPECT_NEAR(before.x, atLink.x, kTolerance);
    EXPECT_NEAR(before.y, atLink.y, kTolerance);

    const double frames[] = { 1., 3., 4.5, 7., 10., 12. };
    for (double f : frames) {
        double cx, cy;
        centreAt(marker, f, &cx, &cy);
        const PointState now = pointAt(cp, f);
        EXPECT_NEAR(cx + before.x - c0x, now.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(cy + before.y - c0y, now.y, kTolerance) << "frame " << f;
        EXPECT_NEAR(before.lx - before.x, now.lx - now.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(before.ly - before.y, now.ly - now.y, kTolerance) << "frame " << f;
        EXPECT_NEAR(before.rx - before.x, now.rx - now.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(before.ry - before.y, now.ry - now.y, kTolerance) << "frame " << f;

        const PointState feather = pointAt(fp, f);
        EXPECT_NEAR(featherBefore.x - before.x, feather.x - now.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(featherBefore.y - before.y, feather.y - now.y, kTolerance) << "frame " << f;

        const PointState still = pointAt(other, f);
        EXPECT_NEAR(otherBefore.x, still.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(otherBefore.y, still.y, kTolerance) << "frame " << f;
    }
}

// Linking an animated point takes the offset at the link frame and ignores its keys; Unlink brings
// them back, and Unlink and Bake replaces them with the followed path over the track's key range.
TEST_F(CurveTrackLinkTest, UnlinkRestoresKeysAndBakeReplacesThem)
{
    resetProject();
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoCurveContextPtr context = roto->getCurveContext();
    ASSERT_TRUE(bool(context));
    ASSERT_TRUE(context->isAutoKeyingEnabled());
    const double t0 = context->getTimelineCurrentTime();
    const double tKey = t0 + 4.;
    BezierPtr square = context->makeSquare(10., 10., 12., t0);
    ASSERT_TRUE(bool(square));
    square->setKeyframe(tKey);
    square->movePointByIndex(0, tKey, 5., -4.);

    BezierCPPtr cp = square->getControlPointAtIndex(0);
    ASSERT_TRUE(bool(cp));
    const PointState ownAtLink = pointAt(cp, t0);
    const PointState ownAtKey = pointAt(cp, tKey);
    ASSERT_GT(std::abs(ownAtKey.x - ownAtLink.x), 1.);

    square->linkPointToTrack(0, marker, t0);
    double c0x, c0y, ckx, cky;
    centreAt(marker, t0, &c0x, &c0y);
    centreAt(marker, tKey, &ckx, &cky);
    const PointState linkedAtKey = pointAt(cp, tKey);
    EXPECT_NEAR(ckx + ownAtLink.x - c0x, linkedAtKey.x, kTolerance);
    EXPECT_NEAR(cky + ownAtLink.y - c0y, linkedAtKey.y, kTolerance);

    square->unlinkPoint(0, false);
    EXPECT_FALSE(cp->isLinkedToTrack());
    const PointState restored = pointAt(cp, tKey);
    EXPECT_NEAR(ownAtKey.x, restored.x, kTolerance);
    EXPECT_NEAR(ownAtKey.y, restored.y, kTolerance);

    square->linkPointToTrack(0, marker, t0);
    std::vector<PointState> followed;
    for (int f = kFirstKey; f <= kLastKey; ++f) {
        followed.push_back(pointAt(cp, f));
    }
    square->unlinkPoint(0, true);
    EXPECT_FALSE(cp->isLinkedToTrack());
    std::set<double> keys;
    square->getKeyframeTimes(&keys);
    for (int f = kFirstKey; f <= kLastKey; ++f) {
        EXPECT_TRUE(keys.count(f)) << "no baked key at frame " << f;
        const PointState baked = pointAt(cp, f);
        const PointState& expected = followed[f - kFirstKey];
        EXPECT_NEAR(expected.x, baked.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(expected.y, baked.y, kTolerance) << "frame " << f;
        EXPECT_NEAR(expected.lx, baked.lx, kTolerance) << "frame " << f;
        EXPECT_NEAR(expected.ry, baked.ry, kTolerance) << "frame " << f;
    }
}

// A saved link is restored on load, resolved by name, and follows the track again.
TEST_F(CurveTrackLinkTest, LinkSurvivesSaveAndLoad)
{
    resetProject();
    NodePtr tracker;
    TrackMarkerPtr marker = makeTrack(&tracker);
    ASSERT_TRUE(marker && tracker);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoCurveContextPtr context = roto->getCurveContext();
    ASSERT_TRUE(bool(context));
    const double t0 = context->getTimelineCurrentTime();
    BezierPtr square = context->makeSquare(10., 10., 12., t0);
    ASSERT_TRUE(bool(square));
    const std::string curveName = square->getScriptName();
    const std::string rotoName = roto->getFullyQualifiedName();
    const std::string trackerName = tracker->getFullyQualifiedName();
    const std::string trackName = marker->getScriptName_mt_safe();
    square->linkPointToTrack(2, marker, t0);
    std::vector<PointState> saved;
    for (int f = kFirstKey; f <= kLastKey; ++f) {
        saved.push_back(pointAt(square->getControlPointAtIndex(2), f));
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QString savedPath;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(project->saveProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("trackLink.ntp"), &savedPath));

    project->reset(false, true);
    const QFileInfo info(savedPath);
    ASSERT_TRUE(project->loadProject(info.absolutePath() + QLatin1Char('/'), info.fileName()));
    const QStringList warnings = project->getLoadWarnings();
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();

    NodePtr loadedRoto = getApp()->getNodeByFullySpecifiedName(rotoName);
    NodePtr loadedTracker = getApp()->getNodeByFullySpecifiedName(trackerName);
    ASSERT_TRUE(loadedRoto && loadedTracker);
    BezierPtr loaded = std::dynamic_pointer_cast<Bezier>(loadedRoto->getCurveContext()->getItemByName(curveName));
    ASSERT_TRUE(bool(loaded));
    BezierCPPtr cp = loaded->getControlPointAtIndex(2);
    BezierCPPtr fp = loaded->getFeatherPointAtIndex(2);
    ASSERT_TRUE(cp && fp);
    EXPECT_TRUE(cp->isLinkedToTrack());
    EXPECT_TRUE(fp->isLinkedToTrack());
    EXPECT_FALSE(loaded->getControlPointAtIndex(0)->isLinkedToTrack());
    TrackMarkerPtr loadedMarker = loadedTracker->getTrackerContext()->getMarkerByName(trackName);
    ASSERT_TRUE(bool(loadedMarker));
    EXPECT_EQ(loadedMarker, cp->getResolvedTrack());
    EXPECT_EQ(trackerName, cp->getTrackLinkTrackerName());
    EXPECT_FALSE(loadedRoto->hasPersistentMessage());
    for (int f = kFirstKey; f <= kLastKey; ++f) {
        const PointState now = pointAt(cp, f);
        const PointState& expected = saved[f - kFirstKey];
        EXPECT_NEAR(expected.x, now.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(expected.y, now.y, kTolerance) << "frame " << f;
        EXPECT_NEAR(expected.lx, now.lx, kTolerance) << "frame " << f;
        EXPECT_NEAR(expected.ry, now.ry, kTolerance) << "frame " << f;
    }

    // Moving the loaded track moves the loaded point: the link is live, not a copy of the path.
    loadedMarker->getCenterKnob()->setValueAtTime(5., trackX(5.) + 7., ViewSpec::all(), 0);
    EXPECT_NEAR(saved[5 - kFirstKey].x + 7., pointAt(cp, 5.).x, kTolerance);

    project->reset(false, true);
}

// A renamed track is still followed; a removed one leaves the point where it was at the current
// frame, and the node warns with the track's name.
TEST_F(CurveTrackLinkTest, RemovedTrackLeavesThePointStaticWithAWarning)
{
    resetProject();
    NodePtr tracker;
    TrackMarkerPtr marker = makeTrack(&tracker);
    ASSERT_TRUE(marker && tracker);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoCurveContextPtr context = roto->getCurveContext();
    ASSERT_TRUE(bool(context));
    const double t0 = context->getTimelineCurrentTime();
    BezierPtr square = context->makeSquare(10., 10., 12., t0);
    ASSERT_TRUE(bool(square));
    BezierCPPtr cp = square->getControlPointAtIndex(0);
    ASSERT_TRUE(bool(cp));
    const PointState before = pointAt(cp, t0);
    square->linkPointToTrack(0, marker, t0);

    ASSERT_TRUE(marker->setScriptName("renamedTrack"));
    EXPECT_EQ(std::string("renamedTrack"), cp->getTrackLinkTrackName());
    EXPECT_EQ(marker, cp->getResolvedTrack());
    EXPECT_FALSE(roto->hasPersistentMessage());

    const U64 linkedHash = context->getContentHash();
    tracker->getTrackerContext()->removeMarker(marker);
    EXPECT_TRUE(cp->isLinkedToTrack());
    EXPECT_FALSE(bool(cp->getResolvedTrack()));
    EXPECT_NE(linkedHash, context->getContentHash());
    for (int f = kFirstKey; f <= kLastKey; ++f) {
        const PointState now = pointAt(cp, f);
        EXPECT_NEAR(before.x, now.x, kTolerance) << "frame " << f;
        EXPECT_NEAR(before.y, now.y, kTolerance) << "frame " << f;
        EXPECT_NEAR(before.lx, now.lx, kTolerance) << "frame " << f;
    }

    ASSERT_TRUE(roto->hasPersistentMessage());
    QString message;
    int type = 0;
    roto->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("renamedTrack"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(tracker->getFullyQualifiedName().c_str()))) << message.toStdString();

    // Unlinking the lost point clears the warning.
    square->unlinkPoint(0, false);
    EXPECT_FALSE(roto->hasPersistentMessage());
}

// Moving a key of a followed track changes the Roto's and the SplineWarp's hashes and renders.
TEST_F(CurveTrackLinkTest, MovingATrackKeyInvalidatesRotoAndSplineWarp)
{
    resetProject();
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    connectNodes(source, roto, 0, true);
    RotoCurveContextPtr rotoContext = roto->getCurveContext();
    ASSERT_TRUE(bool(rotoContext));
    const double t0 = rotoContext->getTimelineCurrentTime();
    BezierPtr square = rotoContext->makeSquare(16., 12., 14., t0);
    ASSERT_TRUE(bool(square));
    square->linkPointToTrack(0, marker, t0);

    NodePtr warp = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
    ASSERT_TRUE(bool(warp));
    connectNodes(source, warp, 0, true);
    SplineWarpContextPtr warpContext = std::dynamic_pointer_cast<SplineWarpContext>(warp->getCurveContext());
    ASSERT_TRUE(bool(warpContext));
    BezierPtr from = makeQuad(warpContext, 20., 15., 10., t0);
    BezierPtr to = makeQuad(warpContext, 20., 15., 10., t0);
    ASSERT_TRUE(from && to);
    ASSERT_TRUE(bool(warpContext->link(from, to)));
    to->linkPointToTrack(1, marker, t0);

    const double frame = 5.;
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::string error;

    roto->refreshHashNow();
    warp->refreshHashNow();
    const U64 rotoHash = roto->getContentHash();
    const U64 warpHash = warp->getContentHash();
    const U64 rotoContextHash = rotoContext->getContentHash();
    const U64 warpContextHash = warpContext->getContentHash();
    std::vector<RenderedPlane> rotoBefore, warpBefore;
    ASSERT_TRUE(renderNodePlanesDirect(roto, frame, ViewIdx(0), 0, window, layers, &rotoBefore, &error, false)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(warp, frame, ViewIdx(0), 0, window, layers, &warpBefore, &error, false)) << error;

    marker->getCenterKnob()->setValueAtTime(frame, trackX(frame) + 6., ViewSpec::all(), 0);
    marker->getCenterKnob()->setValueAtTime(frame, trackY(frame) - 4., ViewSpec::all(), 1);

    EXPECT_NE(rotoContextHash, rotoContext->getContentHash());
    EXPECT_NE(warpContextHash, warpContext->getContentHash());
    roto->refreshHashNow();
    warp->refreshHashNow();
    EXPECT_NE(rotoHash, roto->getContentHash());
    EXPECT_NE(warpHash, warp->getContentHash());

    std::vector<RenderedPlane> rotoAfter, warpAfter;
    ASSERT_TRUE(renderNodePlanesDirect(roto, frame, ViewIdx(0), 0, window, layers, &rotoAfter, &error, false)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(warp, frame, ViewIdx(0), 0, window, layers, &warpAfter, &error, false)) << error;
    const std::size_t rotoDiff = countDiffering(rotoBefore, rotoAfter);
    const std::size_t warpDiff = countDiffering(warpBefore, warpAfter);
    EXPECT_NE(std::size_t(-1), rotoDiff);
    EXPECT_NE(std::size_t(-1), warpDiff);
    EXPECT_GT(rotoDiff, 0u);
    EXPECT_GT(warpDiff, 0u);

    // Unlinking stops listening: the track no longer reaches the hash.
    square->unlinkPoint(0, false);
    to->unlinkPoint(1, false);
    const U64 rotoUnlinked = rotoContext->getContentHash();
    const U64 warpUnlinked = warpContext->getContentHash();
    marker->getCenterKnob()->setValueAtTime(frame, trackX(frame) - 3., ViewSpec::all(), 0);
    EXPECT_EQ(rotoUnlinked, rotoContext->getContentHash());
    EXPECT_EQ(warpUnlinked, warpContext->getContentHash());
}

// BezierCurve.linkPointToTrack() and unlinkPoint() from Python link, bake and unlink, and raise
// ValueError for a bad index or no track.
TEST_F(CurveTrackLinkTest, PythonLinksBakesAndUnlinks)
{
    resetProject();
    NodePtr tracker;
    TrackMarkerPtr marker = makeTrack(&tracker);
    ASSERT_TRUE(marker && tracker);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));

    const std::string script = std::string(kPythonHelpers) + "r = " + pythonName(roto) + ".getRotoContext()\n"
                                                                                         "t = "
        + pythonName(tracker) + ".getTrackerContext().getTrackByName(\"" + marker->getScriptName_mt_safe() + "\")\n"
                                                                                                             "assert t is not None\n"
                                                                                                             "c = r.createBezier(10., 10., 1.)\n"
                                                                                                             "c.addControlPoint(30., 10.)\n"
                                                                                                             "c.addControlPoint(30., 30.)\n"
                                                                                                             "c.addControlPoint(10., 30.)\n"
                                                                                                             "c.setCurveFinished(True)\n"
                                                                                                             "c.setScriptName(\"linked\")\n"
                                                                                                             "assert raises(lambda: c.linkPointToTrack(99, t))\n"
                                                                                                             "assert raises(lambda: c.linkPointToTrack(0, None))\n"
                                                                                                             "assert raises(lambda: c.unlinkPoint(99, False))\n"
                                                                                                             "c.linkPointToTrack(0, t)\n"
                                                                                                             "c.linkPointToTrack(1, t)\n"
                                                                                                             "p2 = c.getControlPointPosition(0, 2.)\n"
                                                                                                             "p6 = c.getControlPointPosition(0, 6.)\n"
                                                                                                             "assert abs(p6[0] - p2[0] - 4.) < 1e-9, (p2, p6)\n"
                                                                                                             "assert abs(p6[1] - p2[1] - 2.) < 1e-9, (p2, p6)\n"
                                                                                                             "c.unlinkPoint(0, True)\n"
                                                                                                             "keys = c.getKeyframes()\n"
                                                                                                             "assert all(float(f) in keys for f in range(1, 11)), keys\n"
                                                                                                             "b6 = c.getControlPointPosition(0, 6.)\n"
                                                                                                             "assert abs(b6[0] - p6[0]) < 1e-9 and abs(b6[1] - p6[1]) < 1e-9, (b6, p6)\n"
                                                                                                             "c.unlinkPoint(1, False)\n";
    std::string error, output;
    EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << error << "\n"
                                                                << script;

    BezierPtr curve = std::dynamic_pointer_cast<Bezier>(roto->getCurveContext()->getItemByName("linked"));
    ASSERT_TRUE(bool(curve));
    EXPECT_FALSE(curve->hasTrackLinks());
    EXPECT_FALSE(curve->getControlPointAtIndex(0)->isLinkedToTrack());
    EXPECT_FALSE(curve->getControlPointAtIndex(1)->isLinkedToTrack());
}
