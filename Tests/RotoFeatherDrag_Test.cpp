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

#include <gtest/gtest.h>

#include <cmath>

#include "Engine/RotoFeatherDrag.h"

NATRON_NAMESPACE_USING

namespace {
const double kEps = 1e-9;

Point
pt(double x,
   double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

double
length(const Point& p)
{
    return std::sqrt(p.x * p.x + p.y * p.y);
}
}

TEST(RotoFeatherDrag, BarDirectionIsUnitVectorFromControlPointToFeather)
{
    const Point dir = featherDragDirection(pt(10, 10), pt(13, 14), pt(0, 0), pt(0, 0), true);

    EXPECT_NEAR(dir.x, 0.6, kEps);
    EXPECT_NEAR(dir.y, 0.8, kEps);
    EXPECT_FALSE(featherSitsOnControlPoint(pt(10, 10), pt(13, 14)));
}

TEST(RotoFeatherDrag, BarDirectionIgnoresOrientation)
{
    const Point cw = featherDragDirection(pt(0, 0), pt(0, 5), pt(1, 0), pt(1, 0), true);
    const Point ccw = featherDragDirection(pt(0, 0), pt(0, 5), pt(1, 0), pt(1, 0), false);

    EXPECT_NEAR(cw.x, ccw.x, kEps);
    EXPECT_NEAR(cw.y, ccw.y, kEps);
}

TEST(RotoFeatherDrag, DragAlongDirectionGivesThatDistance)
{
    const Point dir = featherDragDirection(pt(0, 0), pt(3, 4), pt(0, 0), pt(0, 0), true);

    EXPECT_NEAR(featherDragDistance(dir, 6, 8), 10., kEps);
    EXPECT_NEAR(featherDragDistance(dir, -3, -4), -5., kEps);
}

TEST(RotoFeatherDrag, PerpendicularDragGivesZeroDistance)
{
    const Point dir = featherDragDirection(pt(0, 0), pt(3, 4), pt(0, 0), pt(0, 0), true);

    EXPECT_NEAR(featherDragDistance(dir, -4, 3), 0., kEps);
}

TEST(RotoFeatherDrag, ApplyMovesAlongDirectionBySignedDistance)
{
    const Point moved = applyFeatherDistance(pt(1, 2), pt(0.6, 0.8), 5.);

    EXPECT_NEAR(moved.x, 4., kEps);
    EXPECT_NEAR(moved.y, 6., kEps);

    const Point back = applyFeatherDistance(moved, pt(0.6, 0.8), -5.);
    EXPECT_NEAR(back.x, 1., kEps);
    EXPECT_NEAR(back.y, 2., kEps);
}

TEST(RotoFeatherDrag, ThreePointsWithDifferentNormalsEachMoveBySameDistance)
{
    const Point cps[3] = { pt(0, 0), pt(10, 0), pt(5, 8) };
    const Point fps[3] = { pt(-2, -2), pt(13, 0), pt(5, 12) };

    const Point grabbedDir = featherDragDirection(cps[0], fps[0], pt(0, 0), pt(0, 0), true);
    const double s = featherDragDistance(grabbedDir, -3, -3);
    EXPECT_NEAR(s, 3. * std::sqrt(2.), kEps);

    for (int i = 0; i < 3; ++i) {
        const Point dir = featherDragDirection(cps[i], fps[i], pt(0, 0), pt(0, 0), true);
        EXPECT_NEAR(length(dir), 1., kEps);

        const Point moved = applyFeatherDistance(fps[i], dir, s);
        const Point step = pt(moved.x - fps[i].x, moved.y - fps[i].y);
        EXPECT_NEAR(length(step), s, kEps);
        EXPECT_NEAR(step.x * dir.y - step.y * dir.x, 0., kEps);
        EXPECT_GT(step.x * dir.x + step.y * dir.y, 0.);

        const double before = length(pt(fps[i].x - cps[i].x, fps[i].y - cps[i].y));
        const double after = length(pt(moved.x - cps[i].x, moved.y - cps[i].y));
        EXPECT_NEAR(after - before, s, kEps);
    }
}

TEST(RotoFeatherDrag, FeatherOnPointUsesDerivativeNormal)
{
    const Point p = pt(5, 5);

    EXPECT_TRUE(featherSitsOnControlPoint(p, p));

    const Point left = pt(-1, 0);
    const Point right = pt(1, 0);
    const Point cw = featherDragDirection(p, p, left, right, true);
    EXPECT_NEAR(cw.x, 0., kEps);
    EXPECT_NEAR(cw.y, 1., kEps);

    const Point ccw = featherDragDirection(p, p, left, right, false);
    EXPECT_NEAR(ccw.x, 0., kEps);
    EXPECT_NEAR(ccw.y, -1., kEps);

    EXPECT_NEAR(featherDragDistance(cw, 7, 4), 4., kEps);
}

TEST(RotoFeatherDrag, EqualDerivativesFallBackToLeftDerivative)
{
    const Point p = pt(0, 0);
    const Point d = pt(2, 0);
    const Point dir = featherDragDirection(p, p, d, d, true);

    EXPECT_NEAR(dir.x, 0., kEps);
    EXPECT_NEAR(dir.y, 1., kEps);
}

TEST(RotoFeatherDrag, DegenerateDerivativesGiveNoMovement)
{
    const Point p = pt(3, 3);
    const Point dir = featherDragDirection(p, p, p, p, true);

    EXPECT_EQ(dir.x, 0.);
    EXPECT_EQ(dir.y, 0.);
    EXPECT_EQ(featherDragDistance(dir, 10, -4), 0.);
}
