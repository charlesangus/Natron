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

#include <vector>

#include <gtest/gtest.h>

#include "Gui/EdgeGeometry.h"

NATRON_NAMESPACE_USING

namespace {
std::vector<QPointF>
polyline(const QPointF& a,
         const QPointF& b)
{
    std::vector<QPointF> points;
    points.push_back(a);
    points.push_back(b);

    return points;
}
} // anonymous namespace

TEST(EdgeGeometry, CrossingSegmentsIntersect)
{
    const QLineF pipe(QPointF(0., 0.), QPointF(0., 100.));

    EXPECT_TRUE(polylineCrossesSegment(polyline(QPointF(-50., 50.), QPointF(50., 50.)), pipe));
    EXPECT_TRUE(polylineCrossesSegment(polyline(QPointF(-50., 0.), QPointF(50., 100.)), pipe));
}

TEST(EdgeGeometry, SegmentsThatMissDoNotIntersect)
{
    const QLineF pipe(QPointF(0., 0.), QPointF(0., 100.));

    // Short of the pipe, and past its end.
    EXPECT_FALSE(polylineCrossesSegment(polyline(QPointF(-50., 50.), QPointF(-1., 50.)), pipe));
    EXPECT_FALSE(polylineCrossesSegment(polyline(QPointF(-50., 101.), QPointF(50., 101.)), pipe));
}

TEST(EdgeGeometry, ParallelSegmentsDoNotIntersect)
{
    const QLineF pipe(QPointF(0., 0.), QPointF(100., 100.));

    EXPECT_FALSE(polylineCrossesSegment(polyline(QPointF(10., 0.), QPointF(110., 100.)), pipe));
}

TEST(EdgeGeometry, TouchingAnEndpointIntersects)
{
    const QLineF pipe(QPointF(0., 0.), QPointF(0., 100.));

    // The cut ends on the pipe, and the cut passes through the pipe's end.
    EXPECT_TRUE(polylineCrossesSegment(polyline(QPointF(-50., 50.), QPointF(0., 50.)), pipe));
    EXPECT_TRUE(polylineCrossesSegment(polyline(QPointF(-50., 100.), QPointF(50., 100.)), pipe));
    EXPECT_TRUE(segmentsIntersect(QLineF(QPointF(0., 100.), QPointF(50., 150.)), pipe));
}

TEST(EdgeGeometry, CollinearSegmentsIntersectOnlyWhenTheyOverlap)
{
    const QLineF pipe(QPointF(0., 0.), QPointF(100., 0.));

    EXPECT_TRUE(polylineCrossesSegment(polyline(QPointF(50., 0.), QPointF(150., 0.)), pipe));
    EXPECT_TRUE(polylineCrossesSegment(polyline(QPointF(-50., 0.), QPointF(0., 0.)), pipe));
    EXPECT_FALSE(polylineCrossesSegment(polyline(QPointF(101., 0.), QPointF(150., 0.)), pipe));
}

TEST(EdgeGeometry, PolylineWithSeveralSegments)
{
    const QLineF pipe(QPointF(100., 0.), QPointF(100., 100.));

    // A hook whose last segment alone reaches the pipe.
    std::vector<QPointF> hook;
    hook.push_back(QPointF(0., 0.));
    hook.push_back(QPointF(50., 50.));
    hook.push_back(QPointF(0., 100.));
    hook.push_back(QPointF(150., 80.));
    EXPECT_TRUE(polylineCrossesSegment(hook, pipe));

    hook.pop_back();
    EXPECT_FALSE(polylineCrossesSegment(hook, pipe));
}

TEST(EdgeGeometry, TooFewPointsCrossNothing)
{
    const QLineF pipe(QPointF(0., 0.), QPointF(0., 100.));

    EXPECT_FALSE(polylineCrossesSegment(std::vector<QPointF>(), pipe));
    EXPECT_FALSE(polylineCrossesSegment(std::vector<QPointF>(1, QPointF(0., 50.)), pipe));
}
