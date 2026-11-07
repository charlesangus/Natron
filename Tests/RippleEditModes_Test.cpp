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

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RippleEditKeys.h"
#include "Engine/RotoContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kKeyCount = 10;
const double kEditTime = 5.;
const double kDx = 10.;

struct ModeCase {
    const char* name;
    bool enabled;
    RippleEditModeEnum mode;
    double rangeFirst, rangeLast;
    // Keys expected to move, frames 1..10.
    std::set<double> moved;
};

std::set<double>
frames(int first,
       int last)
{
    std::set<double> s;
    for (int f = first; f <= last; ++f) {
        s.insert(f);
    }

    return s;
}

std::vector<ModeCase>
modeCases()
{
    return {
        { "off", false, eRippleEditModeAll, 0., 0., { kEditTime } },
        { "all", true, eRippleEditModeAll, 0., 0., frames(1, 10) },
        { "fromStart", true, eRippleEditModeFromStart, 0., 0., frames(1, 5) },
        { "toEnd", true, eRippleEditModeToEnd, 0., 0., frames(5, 10) },
        // The range is given backwards on purpose: either order selects the same keys.
        { "range", true, eRippleEditModeRange, 7., 3., frames(3, 7) },
    };
}

} // namespace

class RippleEditModesTest
    : public BaseTest {
protected:
    // A square keyed at frames 1..10, every key with the same points.
    BezierPtr makeKeyedSquare(const RotoContextPtr& context)
    {
        BezierPtr shape = context->makeBezier(100., 100., "Bezier", 1., false);
        if (!shape) {
            return shape;
        }
        shape->addControlPoint(300., 100., 1.);
        shape->addControlPoint(300., 300., 1.);
        shape->addControlPoint(100., 300., 1.);
        shape->setCurveFinished(true);
        for (int k = 2; k <= kKeyCount; ++k) {
            shape->setKeyframe(k);
        }

        return shape;
    }

    void applyMode(const RotoContextPtr& context,
                   const ModeCase& c)
    {
        context->setRippleEditEnabled(c.enabled);
        context->setRippleEditMode(c.mode);
        context->setRippleEditRange(c.rangeFirst, c.rangeLast);
    }
};

TEST(RippleEditKeys, IncludesFollowsTheMode)
{
    RippleEditKeys keys;
    EXPECT_FALSE(keys.includes(5., 5.));
    keys.enabled = true;
    EXPECT_TRUE(keys.includes(1., 5.));
    EXPECT_TRUE(keys.includes(9., 5.));
    keys.mode = eRippleEditModeFromStart;
    EXPECT_TRUE(keys.includes(5., 5.));
    EXPECT_TRUE(keys.includes(1., 5.));
    EXPECT_FALSE(keys.includes(6., 5.));
    keys.mode = eRippleEditModeToEnd;
    EXPECT_TRUE(keys.includes(5., 5.));
    EXPECT_FALSE(keys.includes(4., 5.));
    EXPECT_TRUE(keys.includes(6., 5.));
    keys.mode = eRippleEditModeRange;
    keys.rangeFirst = 3.;
    keys.rangeLast = 7.;
    EXPECT_FALSE(keys.includes(2., 5.));
    EXPECT_TRUE(keys.includes(3., 5.));
    EXPECT_TRUE(keys.includes(7., 5.));
    EXPECT_FALSE(keys.includes(8., 5.));
}

// Moves a point at frame 5 of a 10-key shape in each mode and checks which keys moved with it.
TEST_F(RippleEditModesTest, PointMoveReachesTheModesKeys)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);

    for (const ModeCase& c : modeCases()) {
        SCOPED_TRACE(c.name);
        BezierPtr shape = makeKeyedSquare(context);
        ASSERT_TRUE(bool(shape));
        ASSERT_EQ(kKeyCount, shape->getKeyframesCount());
        BezierCPPtr cp = shape->getControlPointAtIndex(1);
        ASSERT_TRUE(cp != nullptr);

        std::map<double, double> before;
        for (int k = 1; k <= kKeyCount; ++k) {
            double x, y;
            cp->getPositionAtTime(false, k, ViewIdx(0), &x, &y);
            before[k] = x;
        }

        applyMode(context, c);
        shape->movePointByIndex(1, kEditTime, kDx, 0.);

        for (int k = 1; k <= kKeyCount; ++k) {
            double x, y;
            cp->getPositionAtTime(false, k, ViewIdx(0), &x, &y);
            const double expected = before[k] + (c.moved.count(k) ? kDx : 0.);
            EXPECT_DOUBLE_EQ(expected, x) << "key " << k;
        }
        EXPECT_EQ(kKeyCount, shape->getKeyframesCount());

        context->removeItem(shape);
    }
    context->setRippleEditEnabled(false);
    getApp()->getProject()->reset(false, true);
}

// The same for a tangent drag, which goes through the other ripple loop.
TEST_F(RippleEditModesTest, TangentMoveReachesTheModesKeys)
{
    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);

    for (const ModeCase& c : modeCases()) {
        SCOPED_TRACE(c.name);
        BezierPtr shape = makeKeyedSquare(context);
        ASSERT_TRUE(bool(shape));
        BezierCPPtr cp = shape->getControlPointAtIndex(2);
        ASSERT_TRUE(cp != nullptr);

        std::map<double, double> before;
        for (int k = 1; k <= kKeyCount; ++k) {
            double x, y;
            cp->getLeftBezierPointAtTime(false, k, ViewIdx(0), &x, &y, false);
            before[k] = x;
        }

        applyMode(context, c);
        shape->moveLeftBezierPoint(2, kEditTime, kDx, 0.);

        for (int k = 1; k <= kKeyCount; ++k) {
            double x, y;
            cp->getLeftBezierPointAtTime(false, k, ViewIdx(0), &x, &y, false);
            const double expected = before[k] + (c.moved.count(k) ? kDx : 0.);
            EXPECT_DOUBLE_EQ(expected, x) << "key " << k;
        }

        context->removeItem(shape);
    }
    context->setRippleEditEnabled(false);
    getApp()->getProject()->reset(false, true);
}
