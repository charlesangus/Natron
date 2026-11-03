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
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QString>

#include "BaseTest.h"
#include "Native/NativeGolden.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Format.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

#include "Serialization/CurveSerialization.h"
#include "Serialization/RotoSerialization.h"

NATRON_NAMESPACE_USING

// Every edit path of a Bezier runs over one keyed, feathered shape. After each step the GUI copy
// of every point must equal the internal one at every keyframe and between keyframes, and the
// orientation the step refreshed must equal the one a fresh clone computes. The golden case
// records the shape the script leaves, as text, and the mask it renders at three frames, so a
// change to how edits reach the internal curves shows up against the recorded reference. Record
// it with NATRON_GOLDEN_RECORD_DIR set to an empty directory, then copy its BezierEdit directory
// into Tests/fixtures/native-golden.

namespace {

const double kTwoPi = 6.283185307179586;
const int kShapePoints = 8;
const double kCenterX = 32.;
const double kCenterY = 24.;
const double kRadius = 14.;
const double kShapeKeys[] = { 1., 11., 21. };
const double kRenderTimes[] = { 1., 16., 26. };
const char* const kGoldenGroup = "BezierEdit";
const char* const kGoldenShapeFile = "shape.txt";

typedef std::function<void(const std::string& step, double time, bool checkOrientation)> StepCheck;

void
pointAt(int i,
        double time,
        double grow,
        double* x,
        double* y,
        double* tx,
        double* ty)
{
    const double a = kTwoPi * i / kShapePoints;
    const double radius = kRadius * grow * (1. + 0.1 * std::sin(0.4 * time + i));

    *x = kCenterX + radius * std::cos(a);
    *y = kCenterY + radius * std::sin(a);
    *tx = -3. * std::sin(a);
    *ty = 3. * std::cos(a);
}

void
keyPoint(const BezierPtr& shape,
         int i,
         double time,
         bool feather)
{
    double x, y, tx, ty;

    pointAt(i, time, feather ? 1.15 : 1., &x, &y, &tx, &ty);
    shape->setPointAtIndex(feather, i, time, x, y, x - tx, y - ty, x + tx, y + ty);
}

std::vector<double>
sampleTimes(const BezierPtr& shape)
{
    std::set<double> keys;

    shape->getKeyframeTimes(&keys);
    std::vector<double> times;
    if (keys.empty()) {
        times.push_back(1.);

        return times;
    }
    times.push_back(*keys.begin() - 3.);
    for (std::set<double>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        times.push_back(*it);
        std::set<double>::const_iterator next = it;
        ++next;
        if (next != keys.end()) {
            times.push_back(0.5 * (*it + *next));
            times.push_back(*it + 0.25 * (*next - *it));
        }
    }
    times.push_back(*keys.rbegin() + 3.);

    return times;
}

// Counts the samples where the GUI copy of a point differs from the internal one, describing
// the first in `first`.
int
countGuiMismatches(const std::list<BezierCPPtr>& points,
                   const std::vector<double>& times,
                   const char* kind,
                   std::string* first)
{
    int mismatches = 0;
    int index = 0;

    for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it, ++index) {
        std::list<std::pair<double, KeyframeTypeEnum>> guiKeys, keys;
        (*it)->getKeyFrames(true, &guiKeys);
        (*it)->getKeyFrames(false, &keys);
        if (guiKeys != keys) {
            if (mismatches == 0) {
                std::ostringstream os;
                os << kind << " " << index << ": " << guiKeys.size() << " GUI keyframes, " << keys.size() << " internal";
                *first = os.str();
            }
            ++mismatches;
        }
        for (std::size_t t = 0; t < times.size(); ++t) {
            double g[6], v[6];
            (*it)->getPositionAtTime(true, times[t], ViewIdx(0), &g[0], &g[1]);
            (*it)->getLeftBezierPointAtTime(true, times[t], ViewIdx(0), &g[2], &g[3]);
            (*it)->getRightBezierPointAtTime(true, times[t], ViewIdx(0), &g[4], &g[5]);
            (*it)->getPositionAtTime(false, times[t], ViewIdx(0), &v[0], &v[1]);
            (*it)->getLeftBezierPointAtTime(false, times[t], ViewIdx(0), &v[2], &v[3]);
            (*it)->getRightBezierPointAtTime(false, times[t], ViewIdx(0), &v[4], &v[5]);
            for (int c = 0; c < 6; ++c) {
                if (g[c] != v[c]) {
                    if (mismatches == 0) {
                        std::ostringstream os;
                        os.precision(17);
                        os << kind << " " << index << " at time " << times[t] << ", value " << c << ": GUI " << g[c] << ", internal " << v[c];
                        *first = os.str();
                    }
                    ++mismatches;
                }
            }
        }
    }

    return mismatches;
}

void
expectGuiMatchesInternal(const BezierPtr& shape,
                         const std::string& step)
{
    const std::vector<double> times = sampleTimes(shape);
    std::string first;
    int mismatches = countGuiMismatches(shape->getControlPoints_mt_safe(), times, "control point", &first);
    std::string firstFeather;

    mismatches += countGuiMismatches(shape->getFeatherPoints_mt_safe(), times, "feather point", first.empty() ? &first : &firstFeather);
    EXPECT_EQ(0, mismatches) << "after " << step << ", first: " << first;
}

void
expectOrientationOfAFreshClone(const RotoContextPtr& context,
                               const BezierPtr& shape,
                               double time,
                               const std::string& step)
{
    BezierPtr fresh = std::make_shared<Bezier>(context, "fresh", RotoLayerPtr(), false);

    fresh->clone(shape.get());
    EXPECT_EQ(fresh->isFeatherPolygonClockwiseOriented(false, time), shape->isFeatherPolygonClockwiseOriented(false, time))
        << "after " << step << " at time " << time;
}

void
seek(const AppInstancePtr& app,
     int frame)
{
    app->getTimeLine()->seekFrame(frame, false, NULL, eTimelineChangeReasonOtherSeek);
}

std::string
hex(double v)
{
    char buf[64];

    std::snprintf(buf, sizeof(buf), "%a", v);

    return std::string(buf);
}

void
dumpCurve(std::ostringstream& os,
          const char* name,
          const Serialization::CurveSerialization& curve)
{
    os << "    " << name << " " << curve.keyframes.size() << "\n";
    for (std::size_t i = 0; i < curve.keyframes.size(); ++i) {
        const Serialization::KeyFrameSerialization& k = curve.keyframes[i];
        os << "      " << hex(k.time) << " " << hex(k.value) << " " << k.interpolation << " " << hex(k.leftDerivative) << " "
           << hex(k.rightDerivative) << " " << hex(k.leftWeight) << " " << hex(k.rightWeight) << "\n";
    }
}

void
dumpPoints(std::ostringstream& os,
           const char* kind,
           const std::list<Serialization::BezierCPSerialization>& points)
{
    int index = 0;

    for (std::list<Serialization::BezierCPSerialization>::const_iterator it = points.begin(); it != points.end(); ++it, ++index) {
        os << kind << " " << index << " static " << hex(it->x) << " " << hex(it->y) << " " << hex(it->leftX) << " " << hex(it->leftY) << " "
           << hex(it->rightX) << " " << hex(it->rightY) << " broken " << it->broken << "\n";
        dumpCurve(os, "x", it->xCurve);
        dumpCurve(os, "y", it->yCurve);
        dumpCurve(os, "leftX", it->leftXCurve);
        dumpCurve(os, "leftY", it->leftYCurve);
        dumpCurve(os, "rightX", it->rightXCurve);
        dumpCurve(os, "rightY", it->rightYCurve);
    }
}

// The points, their curves and the orientation the renderer reads at each keyframe.
std::string
dumpShape(const BezierPtr& shape)
{
    Serialization::BezierSerialization s;

    shape->toSerialization(&s);
    std::ostringstream os;
    os << "closed " << s.closed << " open " << s.isOpenBezier << "\n";
    dumpPoints(os, "cp", s.controlPoints);
    dumpPoints(os, "fp", s.featherPoints);
    std::set<double> keys;
    shape->getKeyframeTimes(&keys);
    for (std::set<double>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        os << "clockwise " << hex(*it) << " " << shape->isFeatherPolygonClockwiseOriented(false, *it) << "\n";
    }

    return os.str();
}

bool
readFile(const std::string& path,
         std::string* text)
{
    QFile file(QString::fromStdString(path));

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    *text = QString::fromUtf8(file.readAll()).toStdString();

    return true;
}

bool
writeFile(const std::string& path,
          const std::string& text)
{
    QFile file(QString::fromStdString(path));

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes = QString::fromStdString(text).toUtf8();

    return file.write(bytes) == bytes.size();
}

} // namespace

class BezierGuiCopyEquivalenceTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kGoldenWidth, kGoldenHeight, "bezierEdit64x48", 1.));
        seek(getApp(), 1);
    }

    void TearDown() OVERRIDE
    {
        seek(getApp(), 1);
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A finished, feathered shape whose points are keyed at every kShapeKeys time.
    BezierPtr makeKeyedShape(const RotoContextPtr& context)
    {
        double x, y, tx, ty;

        pointAt(0, kShapeKeys[0], 1., &x, &y, &tx, &ty);
        BezierPtr shape = context->makeBezier(x, y, "Bezier", kShapeKeys[0], false);
        if (!shape) {
            return shape;
        }
        for (int i = 1; i < kShapePoints; ++i) {
            pointAt(i, kShapeKeys[0], 1., &x, &y, &tx, &ty);
            shape->addControlPoint(x, y, kShapeKeys[0]);
        }
        shape->setCurveFinished(true);
        for (std::size_t k = 0; k < sizeof(kShapeKeys) / sizeof(kShapeKeys[0]); ++k) {
            for (int i = 0; i < kShapePoints; ++i) {
                keyPoint(shape, i, kShapeKeys[k], false);
                keyPoint(shape, i, kShapeKeys[k], true);
            }
        }

        return shape;
    }

    // Runs every edit path once, calling `check` after each step with the time it edited and
    // whether that step refreshed the orientation there.
    void runEditScript(const RotoContextPtr& context,
                       const BezierPtr& shape,
                       const StepCheck& check)
    {
        const std::pair<double, double> pixelScale(1., 1.);

        context->setAutoKeyingEnabled(true);
        context->setRippleEditEnabled(false);
        context->setFeatherLinkEnabled(true);

        shape->setPointAtIndex(false, 3, 11., 40., 30., 38., 29., 42., 31.);
        check("setPointAtIndex on a keyframe", 11., true);
        shape->setPointAtIndex(true, 5, 16., 20., 14., 19., 13., 21., 15.);
        check("setPointAtIndex (feather) at a new time", 16., true);
        shape->setPointAtIndex(false, 0, 6., 47., 25., 46., 23., 48., 27.);
        check("setPointAtIndex of point 0 at a new time", 6., true);
        shape->movePointByIndex(2, 21., 1.5, -0.75);
        check("movePointByIndex on a keyframe", 21., true);
        shape->moveFeatherByIndex(4, 8., -1., 0.5);
        check("moveFeatherByIndex at a new time", 8., true);
        shape->moveLeftBezierPoint(1, 11., 0.5, 0.5);
        check("moveLeftBezierPoint on a keyframe", 11., true);
        shape->moveRightBezierPoint(6, 26., -0.5, 1.);
        check("moveRightBezierPoint after the last keyframe", 26., true);
        {
            std::list<BezierCPPtr> cps = shape->getControlPoints_mt_safe();
            std::list<BezierCPPtr> fps = shape->getFeatherPoints_mt_safe();
            std::list<BezierCPPtr>::iterator cp = cps.begin();
            std::list<BezierCPPtr>::iterator fp = fps.begin();
            std::advance(cp, 7);
            std::advance(fp, 7);
            shape->movePointLeftAndRightIndex(**cp, **fp, 21., 0.25, -0.25, -0.25, 0.25, 0.25, -0.25, -0.25, 0.25, false, false);
            check("movePointLeftAndRightIndex on a keyframe", 21., true);
            shape->movePointLeftAndRightIndex(**cp, **fp, 13., 0.5, 0., 0., 0.5, 0.5, 0., 0., 0.5, true, false);
            check("movePointLeftAndRightIndex breaking tangents at a new time", 13., true);

            Transform::Matrix3x3 matrix = Transform::matScale(1.05, 0.95);
            shape->transformPoint(*cp, 11., &matrix);
            check("transformPoint on a keyframe", 11., true);
        }
        shape->smoothPointAtIndex(3, 16., pixelScale);
        check("smoothPointAtIndex", 16., true);
        shape->cuspPointAtIndex(4, 21., pixelScale);
        check("cuspPointAtIndex", 21., true);

        context->setRippleEditEnabled(true);
        shape->setPointAtIndex(false, 2, 11., 39., 18., 38., 17., 40., 19.);
        check("ripple setPointAtIndex", 11., true);
        shape->movePointByIndex(5, 21., 0.5, 0.5);
        check("ripple movePointByIndex", 21., true);
        shape->moveRightBezierPoint(1, 16., 0.25, -0.25);
        check("ripple moveRightBezierPoint", 16., true);
        shape->smoothPointAtIndex(6, 11., pixelScale);
        check("ripple smoothPointAtIndex", 11., true);
        context->setRippleEditEnabled(false);

        context->setAutoKeyingEnabled(false);
        shape->setPointAtIndex(false, 1, 21., 41., 33., 40., 32., 42., 34.);
        check("setPointAtIndex on a keyframe without auto-key", 21., true);
        shape->movePointByIndex(3, 31., 1., 1.);
        check("movePointByIndex at a new time without auto-key", 31., false);
        context->setAutoKeyingEnabled(true);

        shape->setKeyframe(31.);
        check("setKeyframe", 31., false);
        shape->removeKeyframe(16.);
        check("removeKeyframe", 16., false);
        shape->moveKeyframe(26., 36.);
        check("moveKeyframe", 36., false);

        seek(getApp(), 18);
        shape->addControlPointAfterIndex(2, 0.5);
        check("addControlPointAfterIndex at an unkeyed frame", 18., false);
        seek(getApp(), 1);
        shape->addControlPointAfterIndex(5, 0.25);
        check("addControlPointAfterIndex on a keyframe", 1., false);
        shape->removeControlPointByIndex(0);
        check("removeControlPointByIndex", 11., true);
        shape->setCurveFinished(true);
        check("setCurveFinished", 21., true);
    }
};

TEST_F(BezierGuiCopyEquivalenceTest, EveryEditKeepsTheGuiCopyEqual)
{
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));

    BezierPtr shape = makeKeyedShape(context);
    ASSERT_TRUE(bool(shape));
    ASSERT_EQ(kShapePoints, shape->getControlPointsCount());
    ASSERT_EQ((int)(sizeof(kShapeKeys) / sizeof(kShapeKeys[0])), shape->getKeyframesCount());
    expectGuiMatchesInternal(shape, "building the shape");

    runEditScript(context, shape, [&](const std::string& step, double time, bool checkOrientation) {
        expectGuiMatchesInternal(shape, step);
        if (checkOrientation) {
            expectOrientationOfAFreshClone(context, shape, time, step);
        }
    });
}

TEST_F(BezierGuiCopyEquivalenceTest, EditScriptMatchesTheGolden)
{
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));

    BezierPtr shape = makeKeyedShape(context);
    ASSERT_TRUE(bool(shape));
    runEditScript(context, shape, [](const std::string&, double, bool) { });

    const std::string text = dumpShape(shape);
    const std::string recordDir = goldenRecordDir();
    const std::string groupDir = (recordDir.empty() ? goldenReferenceDir() : recordDir) + "/" + kGoldenGroup;
    const std::string shapePath = groupDir + "/" + kGoldenShapeFile;
    if (!recordDir.empty()) {
        QDir().mkpath(QString::fromStdString(groupDir));
        ASSERT_TRUE(writeFile(shapePath, text)) << "cannot write " << shapePath;
    } else {
        std::string reference;
        ASSERT_TRUE(readFile(shapePath, &reference)) << "no reference " << shapePath;
        EXPECT_EQ(reference, text);
    }

    for (std::size_t i = 0; i < sizeof(kRenderTimes) / sizeof(kRenderTimes[0]); ++i) {
        const std::string caseName = "mask_f" + std::to_string((int)kRenderTimes[i]);
        EXPECT_TRUE(checkGolden(roto, kGoldenGroup, caseName, 0, kRenderTimes[i]));
    }
}
