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

#include "Gui/Edge.h"

NATRON_NAMESPACE_USING

namespace {
EdgeNearbyCandidate
makeCandidate(const QPointF& p1,
              const QPointF& p2,
              bool isUnconnectedInput)
{
    EdgeNearbyCandidate candidate;

    candidate.line = QLineF(p1, p2);
    candidate.isUnconnectedInput = isUnconnectedInput;

    return candidate;
}
} // anonymous namespace

TEST(EdgeNearbyGrab, Dist2ToSegmentClampsToTheNearestEndpoint)
{
    QLineF line(QPointF(0, 0), QPointF(10, 0));

    EXPECT_DOUBLE_EQ(25., Edge::dist2ToSegment(line, QPointF(-5, 0)));
    EXPECT_DOUBLE_EQ(25., Edge::dist2ToSegment(line, QPointF(15, 0)));
}

TEST(EdgeNearbyGrab, Dist2ToSegmentIsThePerpendicularDistanceInTheMiddle)
{
    QLineF line(QPointF(0, 0), QPointF(10, 0));

    EXPECT_DOUBLE_EQ(9., Edge::dist2ToSegment(line, QPointF(5, 3)));
}

TEST(EdgeNearbyGrab, PicksTheNearerOfTwoEdgesWithinRadius)
{
    std::vector<EdgeNearbyCandidate> candidates;

    candidates.push_back(makeCandidate(QPointF(0, 10), QPointF(10, 10), false)); // 10 away
    candidates.push_back(makeCandidate(QPointF(0, 2), QPointF(10, 2), false)); // 2 away

    int nearest = pickNearestEdgeCandidate(QPointF(5, 0), candidates, 20., 20.);

    EXPECT_EQ(1, nearest);
}

TEST(EdgeNearbyGrab, SkipsAnEdgeOutsideItsOwnRadius)
{
    std::vector<EdgeNearbyCandidate> candidates;

    candidates.push_back(makeCandidate(QPointF(0, 30), QPointF(10, 30), false));

    int nearest = pickNearestEdgeCandidate(QPointF(5, 0), candidates, 20., 20.);

    EXPECT_EQ(-1, nearest);
}

TEST(EdgeNearbyGrab, AnUnconnectedInputGetsTheLargerRadius)
{
    std::vector<EdgeNearbyCandidate> candidates;

    // 15 away: beyond a connected pipe's radius, but within an unconnected input's.
    candidates.push_back(makeCandidate(QPointF(0, 15), QPointF(10, 15), true));

    EXPECT_EQ(-1, pickNearestEdgeCandidate(QPointF(5, 0), candidates, 10., 10.));
    EXPECT_EQ(0, pickNearestEdgeCandidate(QPointF(5, 0), candidates, 10., 18.));
}

TEST(EdgeNearbyGrab, NoCandidatesReturnsNone)
{
    std::vector<EdgeNearbyCandidate> candidates;

    EXPECT_EQ(-1, pickNearestEdgeCandidate(QPointF(5, 0), candidates, 20., 20.));
}
