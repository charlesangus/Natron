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
#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/NearestMemberLines.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::vector<std::pair<std::size_t, std::size_t>> Lines;

Lines
line(std::size_t a,
     std::size_t b)
{
    return Lines(1, std::make_pair(a, b));
}

Lines
lines(std::initializer_list<std::pair<std::size_t, std::size_t>> l)
{
    return Lines(l);
}

} // namespace

TEST(NearestMemberLines, FewerThanTwoMembersDrawNothing)
{
    EXPECT_TRUE(nearestMemberLines(std::vector<QPointF>()).empty());
    EXPECT_TRUE(nearestMemberLines(std::vector<QPointF>(1, QPointF(3., 4.))).empty());
}

TEST(NearestMemberLines, TwoMembersDrawOneLine)
{
    EXPECT_EQ(line(0, 1), nearestMemberLines({ QPointF(0., 0.), QPointF(50., 10.) }));
}

TEST(NearestMemberLines, FourMembersInARowDrawThreeLines)
{
    const std::vector<QPointF> row = { QPointF(0., 0.), QPointF(300., 0.), QPointF(600., 0.), QPointF(900., 0.) };

    EXPECT_EQ(lines({ { 0, 1 }, { 1, 2 }, { 2, 3 } }), nearestMemberLines(row));
}

TEST(NearestMemberLines, OrderOfTheRowDoesNotChangeTheLines)
{
    // The same row, its members listed out of order: still each to its neighbour.
    const std::vector<QPointF> row = { QPointF(600., 0.), QPointF(0., 0.), QPointF(900., 0.), QPointF(300., 0.) };

    EXPECT_EQ(lines({ { 0, 2 }, { 0, 3 }, { 1, 3 } }), nearestMemberLines(row));
}

TEST(NearestMemberLines, MovingAMemberRepicksItsNearest)
{
    std::vector<QPointF> row = { QPointF(0., 0.), QPointF(300., 0.), QPointF(600., 0.), QPointF(900., 0.) };
    row[3] = QPointF(-300., 0.);

    EXPECT_EQ(lines({ { 0, 1 }, { 0, 3 }, { 1, 2 } }), nearestMemberLines(row));
}

TEST(NearestMemberLines, EveryMemberIsJoinedToItsNearestMember)
{
    const std::vector<QPointF> centres = { QPointF(0., 0.), QPointF(10., 0.), QPointF(500., 0.), QPointF(520., 0.), QPointF(250., 400.) };
    const Lines drawn = nearestMemberLines(centres);

    ASSERT_EQ(centres.size() - 1, drawn.size());
    for (std::size_t i = 0; i < centres.size(); ++i) {
        std::size_t nearest = centres.size();
        double nearestDistance = 0.;
        for (std::size_t j = 0; j < centres.size(); ++j) {
            const QPointF d = centres[j] - centres[i];
            const double distance = d.x() * d.x() + d.y() * d.y();
            if ((j != i) && ((nearest == centres.size()) || (distance < nearestDistance))) {
                nearest = j;
                nearestDistance = distance;
            }
        }
        const std::pair<std::size_t, std::size_t> expected(std::min(i, nearest), std::max(i, nearest));
        EXPECT_NE(drawn.end(), std::find(drawn.begin(), drawn.end(), expected)) << "member " << i;
    }
}

TEST(NearestMemberLines, TwoClustersAreJoinedByOneLine)
{
    const std::vector<QPointF> centres = { QPointF(0., 0.), QPointF(0., 20.), QPointF(1000., 0.), QPointF(1000., 20.) };

    // Each pair is mutually nearest; one more line joins the pairs so the set reads as one.
    EXPECT_EQ(lines({ { 0, 1 }, { 0, 2 }, { 2, 3 } }), nearestMemberLines(centres));
}
