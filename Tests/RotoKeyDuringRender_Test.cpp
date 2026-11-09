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

#include <atomic>
#include <chrono>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/TimeLine.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTwoPi = 6.283185307179586;
const int kPoints = 8;
const double kKeys[] = { 1., 11., 21., 31. };
const double kTolerance = 1e-9;
// The project file stores values as text.
const double kSaveTolerance = 1e-6;
const char* const kComponents[] = { "x", "y", "left x", "left y", "right x", "right y" };
const double kEdit[] = { 31., 22., 30., 20., 32., 24. };

std::vector<double>
sampleTimes()
{
    static const double times[] = { 1., 3.5, 6., 8.5, 11., 16., 21., 26., 31., 36. };

    return std::vector<double>(times, times + sizeof(times) / sizeof(times[0]));
}

std::vector<double>
keyFrameTimes()
{
    return std::vector<double>(kKeys, kKeys + sizeof(kKeys) / sizeof(kKeys[0]));
}

std::vector<double>
sampleValues(const BezierCP& point,
             bool useGui,
             const std::vector<double>& times)
{
    std::vector<double> values;

    for (std::size_t i = 0; i < times.size(); ++i) {
        double v[6];
        point.getPositionAtTime(useGui, times[i], ViewIdx(0), &v[0], &v[1]);
        point.getLeftBezierPointAtTime(useGui, times[i], ViewIdx(0), &v[2], &v[3]);
        point.getRightBezierPointAtTime(useGui, times[i], ViewIdx(0), &v[4], &v[5]);
        values.insert(values.end(), v, v + 6);
    }

    return values;
}

struct ShapeValues {
    std::vector<double> times;
    std::vector<std::vector<double>> cps, fps;
};

ShapeValues
recordShape(const BezierPtr& shape,
            bool useGui,
            const std::vector<double>& times = sampleTimes())
{
    ShapeValues values;
    values.times = times;
    const std::list<BezierCPPtr> cps = shape->getControlPoints_mt_safe();
    const std::list<BezierCPPtr> fps = shape->getFeatherPoints_mt_safe();

    for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it) {
        values.cps.push_back(sampleValues(**it, useGui, times));
    }
    for (std::list<BezierCPPtr>::const_iterator it = fps.begin(); it != fps.end(); ++it) {
        values.fps.push_back(sampleValues(**it, useGui, times));
    }

    return values;
}

void
describePoint(const std::string& what,
              const std::vector<double>& expected,
              const std::vector<double>& actual,
              double tolerance,
              const std::vector<double>& times,
              std::ostringstream& out)
{
    for (std::size_t i = 0; i + 1 < actual.size(); i += 6) {
        if ((actual[i] == 0.) && (actual[i + 1] == 0.)) {
            out << what << " sits at (0,0) at frame " << times[i / 6] << "\n";
            break;
        }
    }
    for (std::size_t i = 0; i < expected.size() && i < actual.size(); ++i) {
        if (std::fabs(expected[i] - actual[i]) > tolerance) {
            out << what << " " << kComponents[i % 6] << " at frame " << times[i / 6] << ": expected " << expected[i] << ", got " << actual[i] << "\n";

            return;
        }
    }
}

std::set<double>
keyTimes(const CurvePtr& curve)
{
    std::set<double> times;
    const KeyFrameSet keys = curve->getKeyFrames_mt_safe();

    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        times.insert(it->getTime());
    }

    return times;
}

// The render reads the internal curves. Where x is keyed and y is not, that read leaves y unset
// (its assert is compiled out), so every x and y pair must be keyed at the same frames.
void
describeHalfWrittenKeys(const std::string& what,
                        const BezierCP& point,
                        std::ostringstream& out)
{
    const char* const names[] = { "position", "left tangent", "right tangent" };
    const CurvePtr xs[] = { point.getXCurve(), point.getLeftXCurve(), point.getRightXCurve() };
    const CurvePtr ys[] = { point.getYCurve(), point.getLeftYCurve(), point.getRightYCurve() };

    for (int i = 0; i < 3; ++i) {
        if (keyTimes(xs[i]) != keyTimes(ys[i])) {
            out << what << " " << names[i] << " has x and y keyed at different frames\n";
        }
    }
}

// Every way the shape differs from `expected`, on its internal and on its GUI curves; empty when
// both sides match.
std::string
describeDifferences(const ShapeValues& expected,
                    const BezierPtr& shape,
                    double tolerance)
{
    std::ostringstream out;

    for (int gui = 0; gui < 2; ++gui) {
        const ShapeValues actual = recordShape(shape, gui != 0, expected.times);
        const std::string side = gui ? "GUI" : "internal";
        if ((actual.cps.size() != expected.cps.size()) || (actual.fps.size() != expected.fps.size())) {
            out << side << ": " << actual.cps.size() << " points and " << actual.fps.size() << " feather points, expected " << expected.cps.size() << " and " << expected.fps.size() << "\n";
            continue;
        }
        for (std::size_t i = 0; i < expected.cps.size(); ++i) {
            std::ostringstream what;
            what << side << " point " << i;
            describePoint(what.str(), expected.cps[i], actual.cps[i], tolerance, expected.times, out);
        }
        for (std::size_t i = 0; i < expected.fps.size(); ++i) {
            std::ostringstream what;
            what << side << " feather point " << i;
            describePoint(what.str(), expected.fps[i], actual.fps[i], tolerance, expected.times, out);
        }
    }

    const std::list<BezierCPPtr> cps = shape->getControlPoints_mt_safe();
    const std::list<BezierCPPtr> fps = shape->getFeatherPoints_mt_safe();
    int i = 0;
    for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it, ++i) {
        std::ostringstream what;
        what << "point " << i;
        describeHalfWrittenKeys(what.str(), **it, out);
    }
    i = 0;
    for (std::list<BezierCPPtr>::const_iterator it = fps.begin(); it != fps.end(); ++it, ++i) {
        std::ostringstream what;
        what << "feather point " << i;
        describeHalfWrittenKeys(what.str(), **it, out);
    }

    return out.str();
}

void
editPointFive(const BezierPtr& shape,
              double time)
{
    shape->setPointAtIndex(false, 5, time, kEdit[0], kEdit[1], kEdit[2], kEdit[3], kEdit[4], kEdit[5]);
}

} // namespace

class RotoKeyDuringRenderTest
    : public BaseTest {
protected:
    NodePtr _roto;
    RotoContextPtr _context;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        seek(1);
    }

    virtual void TearDown() OVERRIDE
    {
        _context.reset();
        _roto.reset();
        getApp()->getProject()->reset(false, true);
        seek(1);
        BaseTest::TearDown();
    }

    void seek(int frame)
    {
        getApp()->getTimeLine()->seekFrame(frame, false, NULL, eTimelineChangeReasonOtherSeek);
    }

    bool makeRoto(bool autoKey)
    {
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        if (!_roto) {
            return false;
        }
        _context = _roto->getRotoContext();
        if (!_context) {
            return false;
        }
        _context->setAutoKeyingEnabled(autoKey);
        _context->setRippleEditEnabled(false);

        return true;
    }

    RotoPaintInteractPtr interact() const
    {
        RotoPaint* effect = dynamic_cast<RotoPaint*>(_roto->getEffectInstance().get());

        return effect ? effect->getInteract() : RotoPaintInteractPtr();
    }

    // A track keyed on every frame of [1, 10].
    TrackMarkerPtr makeTrack()
    {
        NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        if (!tracker || !tracker->getTrackerContext()) {
            return TrackMarkerPtr();
        }
        TrackMarkerPtr marker = tracker->getTrackerContext()->createMarker();
        if (!marker) {
            return marker;
        }
        KnobDoublePtr centre = marker->getCenterKnob();
        for (int f = 1; f <= 10; ++f) {
            centre->setValueAtTime(f, 50. + 2. * f, ViewSpec::all(), 0);
            centre->setValueAtTime(f, 30. + f, ViewSpec::all(), 1);
        }

        return marker;
    }

    // Built with auto-key on whatever the test's setting: with it off, makeBezier and
    // setPointAtIndex leave an unkeyed shape at its creation pose.
    BezierPtr makeKeyedShape(const std::string& name)
    {
        const bool autoKey = _context->isAutoKeyingEnabled();
        _context->setAutoKeyingEnabled(true);
        BezierPtr shape = buildKeyedShape(name);
        _context->setAutoKeyingEnabled(autoKey);
        if (shape && (shape->getKeyframesCount() != (int)(sizeof(kKeys) / sizeof(kKeys[0])))) {
            return BezierPtr();
        }

        return shape;
    }

    BezierPtr buildKeyedShape(const std::string& name)
    {
        BezierPtr shape = _context->makeBezier(60., 40., name, kKeys[0], false);
        if (!shape) {
            return shape;
        }
        for (int i = 1; i < kPoints; ++i) {
            const double a = kTwoPi * i / kPoints;
            shape->addControlPoint(40. + 20. * std::cos(a), 40. + 20. * std::sin(a), kKeys[0]);
        }
        shape->setCurveFinished(true);
        for (std::size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); ++k) {
            for (int i = 0; i < kPoints; ++i) {
                const double a = kTwoPi * i / kPoints;
                const double r = 20. + 2. * std::sin(0.5 * kKeys[k] + i);
                const double x = 40. + r * std::cos(a);
                const double y = 40. + r * std::sin(a);
                shape->setPointAtIndex(false, i, kKeys[k], x, y, x - 1., y - 2., x + 1., y + 2.);
                shape->setPointAtIndex(true, i, kKeys[k], x * 1.1, y * 1.1, x * 1.1 - 1., y * 1.1 - 2., x * 1.1 + 1., y * 1.1 + 2.);
            }
        }

        return shape;
    }

    // Applies `edit` to `reference` with no render running and to `shape` while the node renders,
    // then writes the render-time edits back.
    template <typename Edit>
    void editBothWays(const BezierPtr& reference,
                      const BezierPtr& shape,
                      Edit edit)
    {
        edit(reference);
        {
            RenderingFlagSetter rendering(_roto);
            EXPECT_TRUE(_roto->isNodeRendering());
            edit(shape);
        }
        EXPECT_FALSE(_roto->isNodeRendering());
        shape->dequeueGuiActions();
    }
};

// Auto-keying the frame of a point inserted during a render keys the new point where it was
// inserted, not at the origin.
TEST_F(RotoKeyDuringRenderTest, AutoKeyedInsertDuringARenderKeepsTheNewPoint)
{
    ASSERT_TRUE(makeRoto(true));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);
    seek(6);

    editBothWays(reference, shape, [](const BezierPtr& s) { s->addControlPointAfterIndex(0, 0.5); });

    std::set<double> keys;
    shape->getKeyframeTimes(&keys);
    EXPECT_TRUE(keys.count(6.)) << "the insert auto-keyed frame 6";
    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, AutoKeyedMoveDuringARenderKeepsTheMove)
{
    ASSERT_TRUE(makeRoto(true));
    seek(6);

    for (int ripple = 0; ripple < 2; ++ripple) {
        _context->setRippleEditEnabled(ripple != 0);
        BezierPtr reference = makeKeyedShape("Reference");
        BezierPtr shape = makeKeyedShape("Edited");
        ASSERT_TRUE(reference && shape);

        editBothWays(reference, shape, [](const BezierPtr& s) { s->movePointByIndex(3, 6., 4., -3.); });

        const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
        EXPECT_TRUE(differences.empty()) << "ripple " << ripple << "\n"
                                         << differences;
        _context->removeItem(reference);
        _context->removeItem(shape);
    }
}

TEST_F(RotoKeyDuringRenderTest, AutoKeyedSetPointAtIndexDuringARenderKeepsTheValues)
{
    ASSERT_TRUE(makeRoto(true));
    seek(6);

    for (int feather = 0; feather < 2; ++feather) {
        BezierPtr reference = makeKeyedShape("Reference");
        BezierPtr shape = makeKeyedShape("Edited");
        ASSERT_TRUE(reference && shape);
        const bool isFeather = feather != 0;

        editBothWays(reference, shape, [isFeather](const BezierPtr& s) {
            s->setPointAtIndex(isFeather, 5, 6., kEdit[0], kEdit[1], kEdit[2], kEdit[3], kEdit[4], kEdit[5]);
        });

        const BezierCPPtr point = isFeather ? shape->getFeatherPointAtIndex(5) : shape->getControlPointAtIndex(5);
        ASSERT_TRUE(bool(point));
        for (int gui = 0; gui < 2; ++gui) {
            double x = 0., y = 0.;
            point->getPositionAtTime(gui != 0, 6., ViewIdx(0), &x, &y);
            EXPECT_EQ(kEdit[0], x) << "feather " << feather << ", gui " << gui;
            EXPECT_EQ(kEdit[1], y) << "feather " << feather << ", gui " << gui;
        }
        const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
        EXPECT_TRUE(differences.empty()) << "feather " << feather << "\n"
                                         << differences;
        _context->removeItem(reference);
        _context->removeItem(shape);
    }
}

TEST_F(RotoKeyDuringRenderTest, SetKeyframeDuringARenderKeepsAnEditAtTheNextKey)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);

    editBothWays(reference, shape, [](const BezierPtr& s) {
        editPointFive(s, 11.);
        s->setKeyframe(6.);
    });

    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, RemoveKeyframeDuringARenderKeepsAnEditAtThePreviousKey)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);

    editBothWays(reference, shape, [](const BezierPtr& s) {
        editPointFive(s, 11.);
        s->removeKeyframe(21.);
    });

    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, MoveKeyframeDuringARenderMovesTheEditedValues)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);

    editBothWays(reference, shape, [](const BezierPtr& s) {
        editPointFive(s, 31.);
        s->moveKeyframe(31., 36.);
    });

    double x = 0., y = 0.;
    shape->getControlPointAtIndex(5)->getPositionAtTime(false, 36., ViewIdx(0), &x, &y);
    EXPECT_EQ(kEdit[0], x);
    EXPECT_EQ(kEdit[1], y);
    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, RemoveAnimationDuringARenderKeepsTheEditedPose)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);
    seek(11);

    editBothWays(reference, shape, [](const BezierPtr& s) {
        editPointFive(s, 11.);
        s->removeAnimation();
    });

    double x = 0., y = 0.;
    shape->getControlPointAtIndex(5)->getPositionAtTime(false, 1., ViewIdx(0), &x, &y);
    EXPECT_EQ(kEdit[0], x);
    EXPECT_EQ(kEdit[1], y);
    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, RemoveAnimationOfAnUnkeyedShapeKeepsItsPose)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = _context->makeBezier(60., 40., "Reference", kKeys[0], false);
    BezierPtr shape = _context->makeBezier(60., 40., "Edited", kKeys[0], false);
    ASSERT_TRUE(reference && shape);
    for (int i = 1; i < kPoints; ++i) {
        const double a = kTwoPi * i / kPoints;
        reference->addControlPoint(40. + 20. * std::cos(a), 40. + 20. * std::sin(a), kKeys[0]);
        shape->addControlPoint(40. + 20. * std::cos(a), 40. + 20. * std::sin(a), kKeys[0]);
    }
    reference->setCurveFinished(true);
    shape->setCurveFinished(true);
    ASSERT_EQ(0, shape->getKeyframesCount());
    const ShapeValues pose = recordShape(shape, false);
    seek(11);

    editBothWays(reference, shape, [](const BezierPtr& s) { s->removeAnimation(); });

    const std::string referenceDifferences = describeDifferences(pose, reference, kTolerance);
    EXPECT_TRUE(referenceDifferences.empty()) << "no render\n"
                                              << referenceDifferences;
    const std::string differences = describeDifferences(pose, shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << "during a render\n"
                                     << differences;
}

TEST_F(RotoKeyDuringRenderTest, TrackUnlinkBakeDuringARenderKeepsAnEdit)
{
    ASSERT_TRUE(makeRoto(false));
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);
    reference->linkPointToTrack(2, marker, 1.);
    shape->linkPointToTrack(2, marker, 1.);

    editBothWays(reference, shape, [](const BezierPtr& s) {
        editPointFive(s, 11.);
        s->unlinkPoint(2, true);
    });

    EXPECT_FALSE(shape->getControlPointAtIndex(2)->isLinkedToTrack());
    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

// A dope sheet or curve editor edit made after a render ends, before its edits are written back,
// keeps them.
TEST_F(RotoKeyDuringRenderTest, PointEditAtAKeyframeAfterARenderKeepsTheRenderTimeEdit)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);

    editPointFive(reference, 11.);
    ASSERT_TRUE(reference->setPointPositionAtKeyframe(5, 21., 35., 18.));
    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());
        editPointFive(shape, 11.);
    }
    ASSERT_FALSE(_roto->isNodeRendering());
    ASSERT_TRUE(shape->setPointPositionAtKeyframe(5, 21., 35., 18.));
    shape->dequeueGuiActions();

    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, PointPositionForEditAfterARenderReadsTheRenderTimeEdit)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));

    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());
        editPointFive(shape, 11.);
    }
    ASSERT_FALSE(_roto->isNodeRendering());
    double x = 0., y = 0.;
    ASSERT_TRUE(shape->getPointPositionForEdit(5, 11., &x, &y));
    EXPECT_EQ(kEdit[0], x);
    EXPECT_EQ(kEdit[1], y);
}

TEST_F(RotoKeyDuringRenderTest, SmoothUndoAfterAnInsertDuringARenderRestoresTheNewPoint)
{
    ASSERT_TRUE(makeRoto(false));
    RotoPaintInteractPtr ui = interact();
    ASSERT_TRUE(bool(ui));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));
    seek(11);

    ShapeValues inserted;
    std::shared_ptr<SmoothCuspUndoCommand> cmd;
    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());
        ASSERT_TRUE(bool(shape->addControlPointAfterIndex(0, 0.5)));
        inserted = recordShape(shape, true);
        SmoothCuspUndoCommand::SmoothCuspCurveData data;
        data.curve = shape;
        data.newPoints.push_back(std::make_pair(shape->getControlPointAtIndex(1), shape->getFeatherPointAtIndex(1)));
        cmd = std::make_shared<SmoothCuspUndoCommand>(ui, std::list<SmoothCuspUndoCommand::SmoothCuspCurveData>(1, data), 11., false, std::make_pair(1., 1.));
        cmd->redo();
    }
    shape->dequeueGuiActions();
    cmd->undo();

    const std::string differences = describeDifferences(inserted, shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, RemoveFeatherUndoAfterAnInsertDuringARenderRestoresTheFeather)
{
    ASSERT_TRUE(makeRoto(false));
    RotoPaintInteractPtr ui = interact();
    ASSERT_TRUE(bool(ui));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));
    seek(11);

    ShapeValues inserted;
    std::shared_ptr<RemoveFeatherUndoCommand> cmd;
    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());
        ASSERT_TRUE(bool(shape->addControlPointAfterIndex(0, 0.5)));
        inserted = recordShape(shape, true);
        RemoveFeatherUndoCommand::RemoveFeatherData data;
        data.curve = shape;
        data.newPoints.push_back(shape->getFeatherPointAtIndex(1));
        cmd = std::make_shared<RemoveFeatherUndoCommand>(ui, std::list<RemoveFeatherUndoCommand::RemoveFeatherData>(1, data));
        cmd->redo();
    }
    shape->dequeueGuiActions();
    cmd->undo();

    const std::string differences = describeDifferences(inserted, shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, TrackLinkUndoAfterAnInsertDuringARenderRestoresTheNewPoint)
{
    ASSERT_TRUE(makeRoto(false));
    RotoPaintInteractPtr ui = interact();
    ASSERT_TRUE(bool(ui));
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));
    seek(11);

    ShapeValues inserted;
    std::shared_ptr<TrackLinkUndoCommand> cmd;
    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());
        ASSERT_TRUE(bool(shape->addControlPointAfterIndex(0, 0.5)));
        inserted = recordShape(shape, true);
        std::list<TrackLinkUndoCommand::PointRef> points(1, std::make_pair(shape, 3));
        cmd = std::make_shared<TrackLinkUndoCommand>(ui, points, TrackLinkUndoCommand::eActionLink, marker, 11.);
        cmd->redo();
    }
    shape->dequeueGuiActions();
    EXPECT_TRUE(shape->getControlPointAtIndex(3)->isLinkedToTrack());
    cmd->undo();

    EXPECT_FALSE(shape->getControlPointAtIndex(3)->isLinkedToTrack());
    const std::string differences = describeDifferences(inserted, shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, RemoveFeatherAtIndexDuringARenderCopiesTheEditedPoint)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);

    editBothWays(reference, shape, [](const BezierPtr& s) {
        s->setPointAtIndex(false, 3, 11., kEdit[0], kEdit[1], kEdit[2], kEdit[3], kEdit[4], kEdit[5]);
        s->removeFeatherAtIndex(3);
    });

    double x = 0., y = 0.;
    shape->getFeatherPointAtIndex(3)->getPositionAtTime(false, 11., ViewIdx(0), &x, &y);
    EXPECT_EQ(kEdit[0], x);
    EXPECT_EQ(kEdit[1], y);
    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, SaveDuringARenderKeepsAnInsertedPoint)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr shape = makeKeyedShape("Saved");
    ASSERT_TRUE(bool(shape));
    seek(11);
    const std::string curveName = shape->getScriptName();
    const std::string rotoName = _roto->getFullyQualifiedName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QString savedPath;
    ProjectPtr project = getApp()->getProject();
    ShapeValues inserted;
    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());
        ASSERT_TRUE(bool(shape->addControlPointAfterIndex(0, 0.5)));
        // In-between frames are left out because reloading recomputes Smooth slopes in time order.
        inserted = recordShape(shape, true, keyFrameTimes());
        ASSERT_TRUE(project->saveProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("keyDuringRender.ntp"), &savedPath));
    }
    shape->dequeueGuiActions();
    shape.reset();
    _context.reset();
    _roto.reset();

    project->reset(false, true);
    const QFileInfo info(savedPath);
    ASSERT_TRUE(project->loadProject(info.absolutePath() + QLatin1Char('/'), info.fileName()));
    NodePtr loadedRoto = getApp()->getNodeByFullySpecifiedName(rotoName);
    ASSERT_TRUE(bool(loadedRoto));
    BezierPtr loaded = std::dynamic_pointer_cast<Bezier>(loadedRoto->getCurveContext()->getItemByName(curveName));
    ASSERT_TRUE(bool(loaded));

    const std::string differences = describeDifferences(inserted, loaded, kSaveTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

// A render on another thread reads the internal curves of whatever points the list holds while
// the main thread edits: inserts, moves, keys, interpolation changes, links and unlinks.
TEST_F(RotoKeyDuringRenderTest, RenderThreadReadsNoEditMadeDuringItsRender)
{
    ASSERT_TRUE(makeRoto(true));
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));
    seek(6);
    shape->linkPointToTrack(2, marker, 1.);
    shape->dequeueGuiActions();

    const std::vector<double> times = sampleTimes();
    std::map<const BezierCP*, std::vector<double>> before;
    {
        const std::list<BezierCPPtr> cps = shape->getControlPoints_mt_safe();
        const std::list<BezierCPPtr> fps = shape->getFeatherPoints_mt_safe();
        for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it) {
            before[it->get()] = sampleValues(**it, false, times);
        }
        for (std::list<BezierCPPtr>::const_iterator it = fps.begin(); it != fps.end(); ++it) {
            before[it->get()] = sampleValues(**it, false, times);
        }
    }

    std::atomic<bool> stop(false);
    std::atomic<int> passes(0);
    std::mutex problemsMutex;
    std::set<std::string> problems;
    ShapeValues edited;
    {
        RenderingFlagSetter rendering(_roto);
        ASSERT_TRUE(_roto->isNodeRendering());

        std::thread reader([&]() {
            while (!stop) {
                std::list<BezierCPPtr> points = shape->getControlPoints_mt_safe();
                const std::list<BezierCPPtr> fps = shape->getFeatherPoints_mt_safe();
                points.insert(points.end(), fps.begin(), fps.end());
                std::ostringstream out;
                for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it) {
                    const std::vector<double> values = sampleValues(**it, false, times);
                    std::map<const BezierCP*, std::vector<double>>::const_iterator found = before.find(it->get());
                    if (found == before.end()) {
                        describePoint("inserted point", values, values, kTolerance, times, out);
                    } else {
                        describePoint("existing point", found->second, values, kTolerance, times, out);
                    }
                    describeHalfWrittenKeys("point", **it, out);
                }
                if (!out.str().empty()) {
                    std::lock_guard<std::mutex> l(problemsMutex);
                    problems.insert(out.str());
                }
                ++passes;
            }
        });

        // Two passes after an edit, so one began after it returned.
        auto letReaderSee = [&]() {
            const int start = passes;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while ((passes < start + 2) && (std::chrono::steady_clock::now() < deadline)) {
                std::this_thread::yield();
            }
        };

        letReaderSee();
        ASSERT_TRUE(bool(shape->addControlPointAfterIndex(0, 0.5)));
        letReaderSee();
        shape->movePointByIndex(4, 6., 4., -3.);
        letReaderSee();
        editPointFive(shape, 6.);
        letReaderSee();
        shape->setKeyframe(16.);
        letReaderSee();
        shape->setKeyFrameInterpolation(eKeyframeTypeConstant, 1);
        letReaderSee();
        shape->linkPointToTrack(6, marker, 6.);
        letReaderSee();
        shape->unlinkPoint(3, true);
        letReaderSee();

        stop = true;
        reader.join();
        EXPECT_GT(passes.load(), 14);
        edited = recordShape(shape, true);
    }
    shape->dequeueGuiActions();

    for (std::set<std::string>::const_iterator it = problems.begin(); it != problems.end(); ++it) {
        ADD_FAILURE() << *it;
    }
    EXPECT_TRUE(shape->getControlPointAtIndex(6)->isLinkedToTrack(false));
    EXPECT_FALSE(shape->getControlPointAtIndex(3)->isLinkedToTrack(false));
    const std::string differences = describeDifferences(edited, shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

// A link made during a render reaches the render's side only on write-back, and so does an unlink.
TEST_F(RotoKeyDuringRenderTest, TrackLinkChangesDuringARenderWaitForTheWriteBack)
{
    ASSERT_TRUE(makeRoto(false));
    TrackMarkerPtr marker = makeTrack();
    ASSERT_TRUE(bool(marker));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));
    seek(6);
    const ShapeValues unlinked = recordShape(shape, false);

    {
        RenderingFlagSetter rendering(_roto);
        shape->linkPointToTrack(2, marker, 1.);
        EXPECT_TRUE(shape->getControlPointAtIndex(2)->isLinkedToTrack(true));
        EXPECT_FALSE(shape->getControlPointAtIndex(2)->isLinkedToTrack(false));
        EXPECT_FALSE(shape->getFeatherPointAtIndex(2)->isLinkedToTrack(false));
        const ShapeValues internal = recordShape(shape, false);
        EXPECT_TRUE(internal.cps == unlinked.cps);
        EXPECT_TRUE(internal.fps == unlinked.fps);
    }
    shape->dequeueGuiActions();
    EXPECT_TRUE(shape->getControlPointAtIndex(2)->isLinkedToTrack(false));
    EXPECT_TRUE(shape->getFeatherPointAtIndex(2)->isLinkedToTrack(false));

    const ShapeValues linked = recordShape(shape, false);
    {
        RenderingFlagSetter rendering(_roto);
        shape->unlinkPoint(2, true);
        EXPECT_FALSE(shape->getControlPointAtIndex(2)->isLinkedToTrack(true));
        EXPECT_TRUE(shape->getControlPointAtIndex(2)->isLinkedToTrack(false));
        const ShapeValues internal = recordShape(shape, false);
        EXPECT_TRUE(internal.cps == linked.cps);
        EXPECT_TRUE(internal.fps == linked.fps);
    }
    shape->dequeueGuiActions();
    EXPECT_FALSE(shape->getControlPointAtIndex(2)->isLinkedToTrack(false));
    EXPECT_FALSE(shape->hasTrackLinks());
}

// Changing interpolation during a render keys the GUI side at its own keyframe and leaves the
// render's side alone until write-back.
TEST_F(RotoKeyDuringRenderTest, KeyFrameInterpolationDuringARenderWaitsForTheWriteBack)
{
    ASSERT_TRUE(makeRoto(false));
    BezierPtr reference = makeKeyedShape("Reference");
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(reference && shape);
    seek(6);
    const ShapeValues original = recordShape(shape, false);

    reference->setKeyFrameInterpolation(eKeyframeTypeConstant, 1);
    {
        RenderingFlagSetter rendering(_roto);
        shape->setKeyFrameInterpolation(eKeyframeTypeConstant, 1);
        const ShapeValues internal = recordShape(shape, false);
        EXPECT_TRUE(internal.cps == original.cps);
        EXPECT_TRUE(internal.fps == original.fps);
    }
    shape->dequeueGuiActions();

    const std::string differences = describeDifferences(recordShape(reference, false), shape, kTolerance);
    EXPECT_TRUE(differences.empty()) << differences;
}

TEST_F(RotoKeyDuringRenderTest, HeldRenderStartsWaitForTheEditOnOtherThreadsOnly)
{
    ASSERT_TRUE(makeRoto(false));

    {
        RenderingFlagSetter rendering(_roto);
        EXPECT_FALSE(_roto->holdRenderStartsForEdit());
    }

    ASSERT_TRUE(_roto->holdRenderStartsForEdit());
    {
        RenderingFlagSetter rendering(_roto);
        EXPECT_TRUE(_roto->isNodeRendering());
    }

    std::atomic<bool> started(false);
    std::atomic<bool> finish(false);
    std::thread render([&]() {
        RenderingFlagSetter rendering(_roto);
        started = true;
        while (!finish) {
            std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(started.load());
    EXPECT_FALSE(_roto->isNodeRendering());

    _roto->releaseRenderStartsForEdit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!started && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(started.load());
    EXPECT_TRUE(_roto->isNodeRendering());
    finish = true;
    render.join();
    EXPECT_FALSE(_roto->isNodeRendering());
}

// A render starting on another thread between an edit's choice of side and its write would see
// a half-made edit: two reads in one render must agree.
TEST_F(RotoKeyDuringRenderTest, RenderOnAnotherThreadNeverSeesAnEditInProgress)
{
    ASSERT_TRUE(makeRoto(true));
    BezierPtr shape = makeKeyedShape("Edited");
    ASSERT_TRUE(bool(shape));
    seek(6);

    std::atomic<bool> stop(false);
    std::atomic<int> renders(0);
    std::atomic<int> torn(0);
    std::thread render([&]() {
        const std::vector<double> times(1, 6.);
        while (!stop) {
            RenderingFlagSetter rendering(_roto);
            const ShapeValues first = recordShape(shape, false, times);
            std::this_thread::yield();
            const ShapeValues second = recordShape(shape, false, times);
            if ((first.cps != second.cps) || (first.fps != second.fps)) {
                ++torn;
            }
            ++renders;
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int i = 0; (renders < 200) && (std::chrono::steady_clock::now() < deadline); ++i) {
        shape->movePointByIndex(i % kPoints, 6., (i % 2) ? 1. : -1., 0.5);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop = true;
    render.join();
    shape->dequeueGuiActions();

    EXPECT_GT(renders.load(), 0);
    EXPECT_EQ(0, torn.load());
}
