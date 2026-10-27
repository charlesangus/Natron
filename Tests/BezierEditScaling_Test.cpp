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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <list>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"

NATRON_NAMESPACE_USING

namespace {

const double kTwoPi = 6.283185307179586;
const int kScalingPoints = 24;
const int kScalingKeyCounts[] = { 25, 50, 100, 200 };

typedef std::chrono::steady_clock Clock;

} // namespace

// Not a pass/fail check on timing: prints how the cost of keying a shape grows with its keyframe
// count, so a per-edit cost that grows with points x keys shows up as a slope of about 2.
class BezierEditScalingTest
    : public BaseTest {
protected:
    RotoContextPtr createRotoContext()
    {
        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        if (!roto) {
            return RotoContextPtr();
        }

        return roto->getRotoContext();
    }

    // A finished, feathered circle of kScalingPoints points keyed at frame 1.
    BezierPtr makeShape(const RotoContextPtr& context)
    {
        const double cx = 500.;
        const double cy = 400.;
        const double radius = 200.;
        BezierPtr shape = context->makeBezier(cx + radius, cy, "Bezier", 1., false);
        if (!shape) {
            return shape;
        }
        for (int i = 1; i < kScalingPoints; ++i) {
            const double a = kTwoPi * i / kScalingPoints;
            shape->addControlPoint(cx + radius * std::cos(a), cy + radius * std::sin(a), 1.);
        }
        shape->setCurveFinished(true);

        return shape;
    }

    // Keys every point and its feather point at frames 1..keyCount. Each setPointAtIndex() call
    // that `perEditMs` is given is timed into it.
    void keyShape(const BezierPtr& shape,
                  int keyCount,
                  std::vector<double>* perEditMs)
    {
        const double cx = 500.;
        const double cy = 400.;
        for (int k = 0; k < keyCount; ++k) {
            const double time = 1. + k;
            for (int i = 0; i < kScalingPoints; ++i) {
                const double a = kTwoPi * i / kScalingPoints;
                const double radius = 200. + 20. * std::sin(0.3 * k + i);
                const double x = cx + radius * std::cos(a);
                const double y = cy + radius * std::sin(a);
                const double tx = -15. * std::sin(a);
                const double ty = 15. * std::cos(a);
                for (int feather = 0; feather < 2; ++feather) {
                    const double grow = feather ? 1.1 : 1.;
                    const double fx = cx + (x - cx) * grow;
                    const double fy = cy + (y - cy) * grow;
                    const Clock::time_point start = Clock::now();
                    shape->setPointAtIndex(feather != 0, i, time, fx, fy, fx - tx, fy - ty, fx + tx, fy + ty);
                    if (perEditMs) {
                        perEditMs->push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    }
                }
            }
        }
    }
};

TEST_F(BezierEditScalingTest, KeyingCostAgainstKeyframeCount)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    context->setRippleEditEnabled(false);

    const int rows = sizeof(kScalingKeyCounts) / sizeof(kScalingKeyCounts[0]);
    std::vector<double> totalsMs;
    std::vector<double> lastPerEditMs;
    for (int r = 0; r < rows; ++r) {
        const int keyCount = kScalingKeyCounts[r];
        BezierPtr shape = makeShape(context);
        ASSERT_TRUE(bool(shape));
        ASSERT_EQ(kScalingPoints, shape->getControlPointsCount());
        ASSERT_EQ(shape->getControlPoints().size(), shape->getFeatherPoints().size());

        std::vector<double>* perEdit = (r == rows - 1) ? &lastPerEditMs : NULL;
        const Clock::time_point start = Clock::now();
        keyShape(shape, keyCount, perEdit);
        const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        totalsMs.push_back(totalMs);
        EXPECT_EQ(keyCount, shape->getKeyframesCount());

        std::printf("[ BezierEditScaling ] %d points x %d keys: %.3f ms\n", kScalingPoints, keyCount, totalMs);
        context->removeItem(shape);
    }

    ASSERT_FALSE(lastPerEditMs.empty());
    std::vector<double> sorted = lastPerEditMs;
    std::sort(sorted.begin(), sorted.end());
    std::printf("[ BezierEditScaling ] median setPointAtIndex at %d keys: %.4f ms over %d edits\n",
                kScalingKeyCounts[rows - 1], sorted[sorted.size() / 2], (int)sorted.size());

    // Least-squares slope of log(total) against log(keys).
    double sx = 0., sy = 0., sxx = 0., sxy = 0.;
    for (int r = 0; r < rows; ++r) {
        const double lx = std::log((double)kScalingKeyCounts[r]);
        const double ly = std::log(std::max(totalsMs[r], 1e-9));
        sx += lx;
        sy += ly;
        sxx += lx * lx;
        sxy += lx * ly;
    }
    const double slope = (rows * sxy - sx * sy) / (rows * sxx - sx * sx);
    std::printf("[ BezierEditScaling ] log-log slope of total time against keys: %.3f\n", slope);

    getApp()->getProject()->reset(false, true);
}
