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

#include <cmath>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTwoPi = 6.283185307179586;
const int kPoints = 8;
const double kKeys[] = { 1., 11., 21., 31. };

void
expectSameKeyFrames(const Curve& expected,
                    const Curve& actual,
                    const std::string& what)
{
    const KeyFrameSet e = expected.getKeyFrames_mt_safe();
    const KeyFrameSet a = actual.getKeyFrames_mt_safe();

    ASSERT_EQ(e.size(), a.size()) << what;
    KeyFrameSet::const_iterator ai = a.begin();
    for (KeyFrameSet::const_iterator ei = e.begin(); ei != e.end(); ++ei, ++ai) {
        EXPECT_EQ(ei->getTime(), ai->getTime()) << what;
        EXPECT_EQ(ei->getValue(), ai->getValue()) << what << " at " << ei->getTime();
        EXPECT_EQ(ei->getLeftDerivative(), ai->getLeftDerivative()) << what << " at " << ei->getTime();
        EXPECT_EQ(ei->getRightDerivative(), ai->getRightDerivative()) << what << " at " << ei->getTime();
        EXPECT_EQ(ei->getInterpolation(), ai->getInterpolation()) << what << " at " << ei->getTime();
    }
}

std::vector<double>
sampleTimes()
{
    std::vector<double> times;

    for (double t = -4.; t <= 40.; t += 0.5) {
        times.push_back(t);
    }

    return times;
}

// The values `point` gives on its GUI (`useGui`) or internal curves over sampleTimes().
std::vector<double>
sampleValues(const BezierCP& point,
             bool useGui)
{
    const std::vector<double> times = sampleTimes();
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

int
countGuiMismatches(const std::list<BezierCPPtr>& points)
{
    int mismatches = 0;

    for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it) {
        std::list<std::pair<double, KeyframeTypeEnum>> guiKeys, keys;
        (*it)->getKeyFrames(true, &guiKeys);
        (*it)->getKeyFrames(false, &keys);
        if (guiKeys != keys) {
            ++mismatches;
        }
        const std::vector<double> gui = sampleValues(**it, true);
        const std::vector<double> internal = sampleValues(**it, false);
        for (std::size_t i = 0; i < gui.size(); ++i) {
            if (gui[i] != internal[i]) {
                ++mismatches;
            }
        }
    }

    return mismatches;
}

// Every orientation the shape caches, internal and GUI, against what a clone that never cached
// one computes over sampleTimes(). The GUI copy must equal the internal curves.
int
countOrientationMismatches(const RotoContextPtr& context,
                           const BezierPtr& shape,
                           double* firstTime)
{
    BezierPtr fresh = std::make_shared<Bezier>(context, "fresh", RotoLayerPtr(), false);

    fresh->clone(shape.get());
    const std::vector<double> times = sampleTimes();
    int mismatches = 0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        const bool expected = fresh->isFeatherPolygonClockwiseOriented(false, times[i]);
        const int wrong = (shape->isFeatherPolygonClockwiseOriented(false, times[i]) != expected ? 1 : 0) + (shape->isFeatherPolygonClockwiseOriented(true, times[i]) != expected ? 1 : 0);
        if (wrong && (mismatches == 0)) {
            *firstTime = times[i];
        }
        mismatches += wrong;
    }

    return mismatches;
}

// Reads the orientation at every sample time on both sides, so stale entries would be cached.
void
readOrientations(const BezierPtr& shape)
{
    const std::vector<double> times = sampleTimes();

    for (std::size_t i = 0; i < times.size(); ++i) {
        (void)shape->isFeatherPolygonClockwiseOriented(false, times[i]);
        (void)shape->isFeatherPolygonClockwiseOriented(true, times[i]);
    }
}

} // namespace

class BezierIncrementalCopyTest
    : public BaseTest {
protected:
    // Counter-clockwise in the y-up sense the orientation uses, unless clockwise is set.
    BezierPtr makeKeyedShape(const RotoContextPtr& context,
                             bool clockwise = false)
    {
        const double turn = clockwise ? -1. : 1.;
        BezierPtr shape = context->makeBezier(60., 40., "Bezier", kKeys[0], false);
        if (!shape) {
            return shape;
        }
        for (int i = 1; i < kPoints; ++i) {
            const double a = kTwoPi * i / kPoints;
            shape->addControlPoint(40. + 20. * std::cos(a), 40. + turn * 20. * std::sin(a), kKeys[0]);
        }
        shape->setCurveFinished(true);
        for (std::size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); ++k) {
            for (int i = 0; i < kPoints; ++i) {
                const double a = kTwoPi * i / kPoints;
                const double r = 20. + 2. * std::sin(0.5 * kKeys[k] + i);
                const double x = 40. + r * std::cos(a);
                const double y = 40. + turn * r * std::sin(a);
                shape->setPointAtIndex(false, i, kKeys[k], x, y, x - 1., y - 2., x + 1., y + 2.);
                shape->setPointAtIndex(true, i, kKeys[k], x * 1.1, y * 1.1, x * 1.1 - 1., y * 1.1 - 2., x * 1.1 + 1., y * 1.1 + 2.);
            }
        }

        return shape;
    }
};

// A per-time copy brings the keyframe at that time and the neighbours whose derivatives it
// refreshed to exactly what a full clone gives.
TEST_F(BezierIncrementalCopyTest, CurvePerTimeCopyMatchesAFullClone)
{
    Curve source;
    for (std::size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); ++k) {
        source.addKeyFrame(KeyFrame(kKeys[k], std::sin(kKeys[k]) * 10.));
    }
    Curve copy;
    copy.clone(source);
    expectSameKeyFrames(source, copy, "the initial clone");

    source.addKeyFrame(KeyFrame(16., 7.));
    copy.copyKeyFrameFrom(source, 16.);
    expectSameKeyFrames(source, copy, "after adding a keyframe between two");

    source.addKeyFrame(KeyFrame(11., -3.));
    copy.copyKeyFrameFrom(source, 11.);
    expectSameKeyFrames(source, copy, "after replacing a keyframe");

    source.removeKeyFrameWithTime(21.);
    copy.copyKeyFrameFrom(source, 21.);
    expectSameKeyFrames(source, copy, "after removing a keyframe");

    source.addKeyFrame(KeyFrame(-5., 2.));
    copy.copyKeyFrameFrom(source, -5.);
    expectSameKeyFrames(source, copy, "after adding a first keyframe");

    source.addKeyFrame(KeyFrame(45., 4.));
    copy.copyKeyFrameFrom(source, 45.);
    expectSameKeyFrames(source, copy, "after adding a last keyframe");

    copy.copyKeyFrameFrom(source, 26.);
    expectSameKeyFrames(source, copy, "after copying a time with no keyframe");

    source.removeKeyFrameWithTime(-5.);
    source.removeKeyFrameWithTime(45.);
    source.removeKeyFrameWithTime(1.);
    copy.copyKeyFrameFrom(source, -5.);
    copy.copyKeyFrameFrom(source, 45.);
    copy.copyKeyFrameFrom(source, 1.);
    expectSameKeyFrames(source, copy, "after removing the first and last keyframes");
}

TEST_F(BezierIncrementalCopyTest, PointPerTimeCopyMatchesAFullClone)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr shape = makeKeyedShape(context);
    ASSERT_TRUE(bool(shape));

    std::list<BezierCPPtr> points = shape->getControlPoints_mt_safe();
    std::list<BezierCPPtr>::iterator it = points.begin();
    std::advance(it, 2);
    BezierCPPtr point = *it;
    ASSERT_EQ(sampleValues(*point, false), sampleValues(*point, true));

    // setLeftBezierPointAtTime(false, ...) writes the internal curves only.
    point->setLeftBezierPointAtTime(false, 16., 33., 41.);
    point->cloneInternalCurvesToGuiCurvesAtTime(16.);
    BezierCP full;
    full.clone(*point);
    EXPECT_EQ(sampleValues(full, true), sampleValues(*point, true)) << "after a new internal keyframe";
    EXPECT_EQ(sampleValues(*point, false), sampleValues(*point, true)) << "after a new internal keyframe";

    point->getLeftXCurve()->removeKeyFrameWithTime(21.);
    point->getLeftYCurve()->removeKeyFrameWithTime(21.);
    point->cloneInternalCurvesToGuiCurvesAtTime(21.);
    full.clone(*point);
    EXPECT_EQ(sampleValues(full, true), sampleValues(*point, true)) << "after removing an internal keyframe";

    point->setPositionAtTime(true, 26., 50., 45.);
    point->setLeftBezierPointAtTime(true, 26., 49., 44.);
    point->setRightBezierPointAtTime(true, 26., 51., 46.);
    point->cloneGuiCurvesToInternalCurvesAtTime(26.);
    EXPECT_EQ(sampleValues(*point, true), sampleValues(*point, false)) << "after writing a GUI keyframe back";

    getApp()->getProject()->reset(false, true);
}

// While the node renders, edits reach the GUI curves only, so dequeueGuiActions() has to bring
// every internal curve they touched up to its GUI copy.
TEST_F(BezierIncrementalCopyTest, EditsDuringARenderAreWrittenBack)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr shape = makeKeyedShape(context);
    ASSERT_TRUE(bool(shape));
    context->setAutoKeyingEnabled(true);
    context->setRippleEditEnabled(false);

    {
        RenderingFlagSetter rendering(roto);
        ASSERT_TRUE(roto->isNodeRendering());

        shape->setPointAtIndex(false, 1, 11., 55., 47., 54., 46., 56., 48.);
        shape->setPointAtIndex(true, 4, 16., 18., 41., 17., 40., 19., 42.);
        shape->movePointByIndex(6, 21., 1., -1.);
        shape->moveFeatherByIndex(3, 26., -0.5, 0.5);
        context->setRippleEditEnabled(true);
        shape->movePointByIndex(2, 31., 0.5, 0.5);
        context->setRippleEditEnabled(false);
        std::list<BezierCPPtr> points = shape->getControlPoints_mt_safe();
        Transform::Matrix3x3 matrix = Transform::matScale(1.1, 0.9);
        shape->transformPoint(points.back(), 11., &matrix);
        shape->smoothPointAtIndex(5, 21., std::make_pair(1., 1.));
        shape->addControlPointAfterIndex(0, 0.5);
    }
    ASSERT_FALSE(roto->isNodeRendering());
    shape->dequeueGuiActions();

    EXPECT_EQ(0, countGuiMismatches(shape->getControlPoints_mt_safe()));
    EXPECT_EQ(0, countGuiMismatches(shape->getFeatherPoints_mt_safe()));

    getApp()->getProject()->reset(false, true);
}

// Mirrors every point and feather point of the shape at time, which reverses its orientation there.
static void
mirrorShapeAt(const BezierPtr& shape,
              double time)
{
    std::list<BezierCPPtr> cps = shape->getControlPoints_mt_safe();
    std::list<BezierCPPtr> fps = shape->getFeatherPoints_mt_safe();
    std::list<BezierCPPtr>::iterator fp = fps.begin();
    int i = 0;
    for (std::list<BezierCPPtr>::iterator cp = cps.begin(); cp != cps.end(); ++cp, ++fp, ++i) {
        double x, y, lx, ly, rx, ry;
        (*cp)->getPositionAtTime(false, time, ViewIdx(0), &x, &y);
        (*cp)->getLeftBezierPointAtTime(false, time, ViewIdx(0), &lx, &ly);
        (*cp)->getRightBezierPointAtTime(false, time, ViewIdx(0), &rx, &ry);
        shape->setPointAtIndex(false, i, time, x, 80. - y, lx, 80. - ly, rx, 80. - ry);
        (*fp)->getPositionAtTime(false, time, ViewIdx(0), &x, &y);
        (*fp)->getLeftBezierPointAtTime(false, time, ViewIdx(0), &lx, &ly);
        (*fp)->getRightBezierPointAtTime(false, time, ViewIdx(0), &rx, &ry);
        shape->setPointAtIndex(true, i, time, x, 80. - y, lx, 80. - ly, rx, 80. - ry);
    }
}

// Orientation is cached per time as it is read, so every edit has to forget the times it reshaped,
// between keyframes too, on the side it edited.
TEST_F(BezierIncrementalCopyTest, EditsForgetTheOrientationTheyReshape)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    context->setRippleEditEnabled(false);
    BezierPtr shape = makeKeyedShape(context);
    ASSERT_TRUE(bool(shape));
    double first = 0.;

    readOrientations(shape);
    EXPECT_FALSE(shape->isFeatherPolygonClockwiseOriented(false, 21.));
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after building, first at " << first;

    readOrientations(shape);
    mirrorShapeAt(shape, 21.);
    EXPECT_TRUE(shape->isFeatherPolygonClockwiseOriented(false, 21.));
    EXPECT_TRUE(shape->isFeatherPolygonClockwiseOriented(true, 21.));
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after mirroring a keyframe, first at " << first;

    readOrientations(shape);
    mirrorShapeAt(shape, 16.);
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after mirroring at a new time, first at " << first;

    readOrientations(shape);
    shape->setKeyframe(26.);
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after setKeyframe, first at " << first;

    readOrientations(shape);
    shape->removeKeyframe(21.);
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after removeKeyframe, first at " << first;

    readOrientations(shape);
    context->setRippleEditEnabled(true);
    shape->movePointByIndex(2, 11., 30., -30.);
    context->setRippleEditEnabled(false);
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after a ripple edit, first at " << first;

    readOrientations(shape);
    shape->addControlPointAfterIndex(3, 0.5);
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after addControlPointAfterIndex, first at " << first;

    readOrientations(shape);
    shape->removeControlPointByIndex(0);
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after removeControlPointByIndex, first at " << first;

    getApp()->getProject()->reset(false, true);
}

// moveKeyframe() carries the orientation with the key it moves.
TEST_F(BezierIncrementalCopyTest, MoveKeyframeKeepsTheOrientation)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    context->setRippleEditEnabled(false);

    for (int clockwise = 0; clockwise < 2; ++clockwise) {
        BezierPtr shape = makeKeyedShape(context, clockwise != 0);
        ASSERT_TRUE(bool(shape));
        readOrientations(shape);
        ASSERT_EQ(clockwise != 0, shape->isFeatherPolygonClockwiseOriented(false, 31.));

        shape->moveKeyframe(31., 36.);
        EXPECT_EQ(clockwise != 0, shape->isFeatherPolygonClockwiseOriented(false, 36.)) << "clockwise " << clockwise;
        EXPECT_EQ(clockwise != 0, shape->isFeatherPolygonClockwiseOriented(true, 36.)) << "clockwise " << clockwise;
        double first = 0.;
        EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "clockwise " << clockwise << ", first at " << first;
        context->removeItem(shape);
    }

    getApp()->getProject()->reset(false, true);
}

// During a render, edits reshape the GUI polygon only: the orientation the render reads stays
// the internal one until dequeueGuiActions() writes the edits back.
TEST_F(BezierIncrementalCopyTest, OrientationDuringARenderFollowsEachSide)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    context->setRippleEditEnabled(false);
    BezierPtr shape = makeKeyedShape(context);
    ASSERT_TRUE(bool(shape));
    readOrientations(shape);

    {
        RenderingFlagSetter rendering(roto);
        ASSERT_TRUE(roto->isNodeRendering());

        mirrorShapeAt(shape, 21.);
        EXPECT_TRUE(shape->isFeatherPolygonClockwiseOriented(true, 21.));
        EXPECT_FALSE(shape->isFeatherPolygonClockwiseOriented(false, 21.));
        readOrientations(shape);

        context->setRippleEditEnabled(true);
        shape->movePointByIndex(5, 11., -25., 20.);
        context->setRippleEditEnabled(false);
        readOrientations(shape);
    }
    ASSERT_FALSE(roto->isNodeRendering());
    shape->dequeueGuiActions();

    EXPECT_TRUE(shape->isFeatherPolygonClockwiseOriented(false, 21.) == shape->isFeatherPolygonClockwiseOriented(true, 21.));
    double first = 0.;
    EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "after the write-back, first at " << first;

    getApp()->getProject()->reset(false, true);
}

// A shape without keyframes keeps one orientation, computed when first read.
TEST_F(BezierIncrementalCopyTest, UnkeyedShapeOrientation)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(false);

    for (int clockwise = 0; clockwise < 2; ++clockwise) {
        const double turn = clockwise ? -1. : 1.;
        BezierPtr shape = context->makeBezier(60., 40., "Bezier", 1., false);
        ASSERT_TRUE(bool(shape));
        for (int i = 1; i < kPoints; ++i) {
            const double a = kTwoPi * i / kPoints;
            shape->addControlPoint(40. + 20. * std::cos(a), 40. + turn * 20. * std::sin(a), 1.);
        }
        shape->setCurveFinished(true);
        ASSERT_EQ(0, shape->getKeyframesCount());

        EXPECT_EQ(clockwise != 0, shape->isFeatherPolygonClockwiseOriented(false, 5.)) << "clockwise " << clockwise;
        EXPECT_EQ(clockwise != 0, shape->isFeatherPolygonClockwiseOriented(true, 5.)) << "clockwise " << clockwise;

        // Removing a point keeps the winding but must not leave a stale value behind.
        shape->removeControlPointByIndex(2);
        double first = 0.;
        EXPECT_EQ(0, countOrientationMismatches(context, shape, &first)) << "clockwise " << clockwise << ", first at " << first;
        context->removeItem(shape);
    }

    context->setAutoKeyingEnabled(true);
    getApp()->getProject()->reset(false, true);
}
