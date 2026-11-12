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
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/NodeGraphItemGrid.h"

NATRON_NAMESPACE_USING

namespace {

typedef NodeGraphItemGrid<int, int> Grid;

std::vector<int>
sortedQuery(Grid& grid,
            const QRectF& rect)
{
    std::vector<int> out;
    grid.query(rect, &out);
    std::sort(out.begin(), out.end());

    return out;
}

bool
overlapsInclusive(const QRectF& a,
                  const QRectF& b)
{
    return (a.left() <= b.right()) && (b.left() <= a.right()) && (a.top() <= b.bottom()) && (b.top() <= a.bottom());
}

} // namespace

TEST(NodeGraphItemGrid, FindsOnlyOverlappingRects)
{
    Grid grid(100.);
    grid.insertOrUpdate(1, 1, QRectF(0, 0, 50, 50));
    grid.insertOrUpdate(2, 2, QRectF(500, 500, 50, 50));
    grid.insertOrUpdate(3, 3, QRectF(-300, -300, 10, 10));

    EXPECT_EQ(std::vector<int>({ 1 }), sortedQuery(grid, QRectF(10, 10, 1, 1)));
    EXPECT_EQ(std::vector<int>({ 2 }), sortedQuery(grid, QRectF(540, 540, 100, 100)));
    EXPECT_EQ(std::vector<int>({ 3 }), sortedQuery(grid, QRectF(-295, -295, 1, 1)));
    EXPECT_TRUE(sortedQuery(grid, QRectF(200, 200, 10, 10)).empty());
}

TEST(NodeGraphItemGrid, TouchingBordersCount)
{
    Grid grid(100.);
    grid.insertOrUpdate(1, 1, QRectF(0, 0, 100, 100));

    EXPECT_EQ(std::vector<int>({ 1 }), sortedQuery(grid, QRectF(100, 100, 0, 0)));
    EXPECT_EQ(std::vector<int>({ 1 }), sortedQuery(grid, QRectF(100, 50, 10, 1)));
}

TEST(NodeGraphItemGrid, ItemsSpanningCellsAreReportedOnce)
{
    Grid grid(10.);
    grid.insertOrUpdate(7, 7, QRectF(0, 0, 55, 55));

    std::vector<int> out;
    grid.query(QRectF(-5, -5, 70, 70), &out);
    EXPECT_EQ(std::vector<int>({ 7 }), out);
}

TEST(NodeGraphItemGrid, UpdateMovesAndRemoveForgets)
{
    Grid grid(100.);
    grid.insertOrUpdate(1, 1, QRectF(0, 0, 10, 10));
    grid.insertOrUpdate(1, 11, QRectF(1000, 1000, 10, 10));

    EXPECT_EQ(1u, grid.size());
    EXPECT_TRUE(sortedQuery(grid, QRectF(0, 0, 10, 10)).empty());
    EXPECT_EQ(std::vector<int>({ 11 }), sortedQuery(grid, QRectF(1005, 1005, 1, 1)));

    EXPECT_TRUE(grid.remove(1));
    EXPECT_FALSE(grid.remove(1));
    EXPECT_FALSE(grid.contains(1));
    EXPECT_TRUE(sortedQuery(grid, QRectF(1005, 1005, 1, 1)).empty());
}

TEST(NodeGraphItemGrid, OversizedRectsAreAlwaysTested)
{
    Grid grid(10., 4);
    grid.insertOrUpdate(1, 1, QRectF(0, 0, 1000, 1000));
    const double huge = std::numeric_limits<double>::max() / 4.;
    grid.insertOrUpdate(2, 2, QRectF(-huge, -huge, 2. * huge, 2. * huge));
    grid.insertOrUpdate(3, 3, QRectF(5000, 5000, 1, 1));

    EXPECT_EQ(std::vector<int>({ 1, 2 }), sortedQuery(grid, QRectF(500, 500, 1, 1)));
    EXPECT_EQ(std::vector<int>({ 2, 3 }), sortedQuery(grid, QRectF(5000, 5000, 1, 1)));

    grid.insertOrUpdate(1, 1, QRectF(5000, 5000, 2, 2));
    EXPECT_EQ(std::vector<int>({ 1, 2, 3 }), sortedQuery(grid, QRectF(5000, 5000, 1, 1)));
    EXPECT_EQ(std::vector<int>({ 2 }), sortedQuery(grid, QRectF(500, 500, 1, 1)));
}

TEST(NodeGraphItemGrid, MatchesBruteForceOnRandomRects)
{
    std::mt19937 rng(118);
    std::uniform_real_distribution<double> pos(-5000., 5000.);
    std::uniform_real_distribution<double> extent(0., 600.);
    Grid grid(256., 64);
    std::vector<QRectF> rects(400);

    for (int i = 0; i < (int)rects.size(); ++i) {
        rects[i] = QRectF(pos(rng), pos(rng), extent(rng), extent(rng));
        grid.insertOrUpdate(i, i, rects[i]);
    }
    for (int i = 0; i < (int)rects.size(); i += 3) {
        rects[i] = QRectF(pos(rng), pos(rng), extent(rng) * 10., extent(rng));
        grid.insertOrUpdate(i, i, rects[i]);
    }
    for (int q = 0; q < 200; ++q) {
        const QRectF query(pos(rng), pos(rng), extent(rng) * (q % 2 ? 1. : 20.), extent(rng));
        std::vector<int> expected;
        for (int i = 0; i < (int)rects.size(); ++i) {
            if (overlapsInclusive(rects[i], query)) {
                expected.push_back(i);
            }
        }
        EXPECT_EQ(expected, sortedQuery(grid, query)) << "query " << q;
    }
}
