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
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/LensAberration.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const RectI kHd(0, 0, 1920, 1080);

LensAberrationParams
catEye(double amount,
       CatEyeOrientation orientation = CatEyeOrientation::Radial)
{
    LensAberrationParams params;

    params.catEye = amount;
    params.catEyeOrientation = orientation;

    return params;
}

bool
clips(const BokehClip& clip)
{
    return std::isfinite(clip.radius);
}

} // anonymous namespace

TEST(LensAberration, DefaultsChangeNothing)
{
    const LensAberration lens(LensAberrationParams(), kHd, 1., 1., 1.);

    EXPECT_FALSE(lens.hasCatEye());
    EXPECT_FALSE(lens.hasChromatic());
    EXPECT_EQ(lens.groupCount(), 1);
    EXPECT_EQ(lens.maxScale(), 1.);
    const RectI window(-100, -100, 3000, 2000);
    const LensCells cells = lens.cells(window);
    EXPECT_EQ(cells.columns * cells.rows, 1);
    EXPECT_FALSE(clips(lens.clip(lens.cellColumn(0), lens.cellRow(0))));
    const std::vector<LensBand> bands = lens.bands(window, 32);
    ASSERT_EQ(bands.size(), 1u);
    EXPECT_EQ(bands[0].y1, window.y1);
    EXPECT_EQ(bands[0].y2, window.y2);
    int calls = 0;
    lens.forEachSegment(window.x1, window.x2, [&](int xa, int xb, int i) {
        EXPECT_EQ(xa, window.x1);
        EXPECT_EQ(xb, window.x2);
        EXPECT_EQ(i, 0);
        ++calls;
    });
    EXPECT_EQ(calls, 1);
}

TEST(LensAberration, CentreCellIsUnclipped)
{
    const LensAberration lens(catEye(1.), kHd, 1., 1., 1.);

    ASSERT_TRUE(lens.hasCatEye());
    const int i = lens.cellColumn(960);
    const int j = lens.cellRow(540);
    EXPECT_EQ(i, 0);
    EXPECT_EQ(j, 0);
    EXPECT_FALSE(clips(lens.clip(i, j)));
    // The centre cell is centred on the format's centre.
    EXPECT_EQ(lens.columnStart(0), 960 - 16);
    EXPECT_EQ(lens.columnStart(1), 960 + 16);
}

TEST(LensAberration, CornerCellsClipRadially)
{
    const LensAberration lens(catEye(1.), kHd, 1., 1., 1.);
    const int corners[4][2] = { { 0, 0 }, { 1919, 0 }, { 0, 1079 }, { 1919, 1079 } };

    for (const auto& corner : corners) {
        const int i = lens.cellColumn(corner[0]);
        const int j = lens.cellRow(corner[1]);
        double px = 0.;
        double py = 0.;
        lens.cellPosition(i, j, &px, &py);
        EXPECT_GT(std::hypot(px, py), 0.9);
        const BokehClip clip = lens.clip(i, j);
        ASSERT_TRUE(clips(clip));
        EXPECT_EQ(clip.radius, 1.);
        // Towards the centre: the offset points from the corner back at the format's centre.
        const double dx = 960. - corner[0];
        const double dy = 540. - corner[1];
        EXPECT_GT(clip.offsetX * dx + clip.offsetY * dy, 0.);
        EXPECT_NEAR(clip.offsetX * dy - clip.offsetY * dx, 0., 0.05 * std::hypot(dx, dy));
        EXPECT_NEAR(std::hypot(clip.offsetX, clip.offsetY), std::hypot(px, py), 1e-12);
    }
}

TEST(LensAberration, SwirlClipsTangentially)
{
    const LensAberration radial(catEye(0.7), kHd, 1., 1., 1.);
    const LensAberration swirl(catEye(0.7, CatEyeOrientation::Tangential), kHd, 1., 1., 1.);

    for (int i : { -20, -3, 5, 29 }) {
        for (int j : { -16, 2, 11 }) {
            const BokehClip r = radial.clip(i, j);
            const BokehClip t = swirl.clip(i, j);
            ASSERT_TRUE(clips(t));
            // A quarter turn counter-clockwise of the radial offset.
            EXPECT_DOUBLE_EQ(t.offsetX, -r.offsetY);
            EXPECT_DOUBLE_EQ(t.offsetY, r.offsetX);
        }
    }
}

TEST(LensAberration, OffsetGrowsWithDistance)
{
    const LensAberration lens(catEye(1.2), kHd, 1., 1., 1.);
    double last = 0.;

    for (int i = 1; i < 40; ++i) {
        const BokehClip clip = lens.clip(i, 0);
        const double offset = std::hypot(clip.offsetX, clip.offsetY);
        EXPECT_GE(offset, last);
        EXPECT_LE(offset, 1.2 + 1e-12);
        last = offset;
    }
    // Beyond the corners the position is clamped.
    EXPECT_NEAR(last, 1.2, 1e-12);
}

TEST(LensAberration, GridIsIndependentOfTheWindow)
{
    const LensAberration lens(catEye(1.), kHd, 1., 1., 1.);
    const RectI windows[] = { RectI(0, 0, 1920, 1080), RectI(300, 200, 557, 457), RectI(-50, -70, 20, 30) };

    for (const RectI& window : windows) {
        const LensCells cells = lens.cells(window);
        for (int y = window.y1; y < window.y2; y += 37) {
            int count = 0;
            int x = window.x1;
            lens.forEachSegment(window.x1, window.x2, [&](int xa, int xb, int i) {
                EXPECT_EQ(xa, x);
                EXPECT_LT(xa, xb);
                EXPECT_GE(i, cells.i1);
                EXPECT_LT(i, cells.i1 + cells.columns);
                for (int xx = xa; xx < xb; ++xx) {
                    EXPECT_EQ(lens.cellColumn(xx), i);
                }
                x = xb;
                ++count;
            });
            EXPECT_EQ(x, window.x2);
            EXPECT_GE(count, 1);
        }
        // The bands tile the window's rows in whole cell rows.
        const std::vector<LensBand> bands = lens.bands(window, 48);
        ASSERT_FALSE(bands.empty());
        EXPECT_EQ(bands.front().y1, window.y1);
        EXPECT_EQ(bands.front().j1, cells.j1);
        EXPECT_EQ(bands.back().y2, window.y2);
        EXPECT_EQ(bands.back().j2, cells.j1 + cells.rows);
        for (std::size_t b = 0; b < bands.size(); ++b) {
            if (b > 0) {
                EXPECT_EQ(bands[b].y1, bands[b - 1].y2);
                EXPECT_EQ(bands[b].j1, bands[b - 1].j2);
            }
            if (b + 1 < bands.size()) {
                EXPECT_GE(bands[b].y2 - bands[b].y1, 48);
            }
            for (int y = bands[b].y1; y < bands[b].y2; ++y) {
                EXPECT_GE(lens.rowOf(y), bands[b].j1);
                EXPECT_LT(lens.rowOf(y), bands[b].j2);
            }
        }
    }
    // Every pixel's cell is that of its full-resolution pixel, whatever the window.
    for (int x = -40; x < 2000; ++x) {
        const int i = lens.cellColumn(x);
        EXPECT_LE(lens.columnStart(i), x);
        EXPECT_GT(lens.columnStart(i + 1), x);
    }
}

TEST(LensAberration, SpreadItemsCoversEachOnce)
{
    for (int nLines : { 1, 7, 100 }) {
        for (std::size_t nItems : { (std::size_t)0, (std::size_t)3, (std::size_t)100, (std::size_t)250 }) {
            std::size_t next = 0;
            for (int line = 0; line < nLines; ++line) {
                std::size_t first = 0;
                std::size_t end = 0;
                spreadItems(line, nLines, nItems, &first, &end);
                EXPECT_EQ(first, next);
                EXPECT_LE(end - first, (nItems + nLines - 1) / nLines);
                next = end;
            }
            EXPECT_EQ(next, nItems);
        }
    }
}

TEST(LensAberration, GridScalesWithTheRender)
{
    const LensAberration full(catEye(1.), kHd, 1., 1., 1.);
    const LensAberration half(catEye(1.), RectI(0, 0, 1920, 1080), 1., 0.5, 0.5);

    for (int i = -31; i <= 31; ++i) {
        EXPECT_EQ(half.columnStart(i), (int)std::ceil((full.columnStart(i) + 0.5) * 0.5 - 0.5)) << i;
        double fx = 0.;
        double fy = 0.;
        double hx = 0.;
        double hy = 0.;
        full.cellPosition(i, 3, &fx, &fy);
        half.cellPosition(i, 3, &hx, &hy);
        EXPECT_EQ(fx, hx);
        EXPECT_EQ(fy, hy);
    }
}

TEST(LensAberration, PositionIsCanonical)
{
    // Pixels of aspect 2: 960 pixels span the canonical width of 1920.
    const LensAberration square(catEye(1.), kHd, 1., 1., 1.);
    const LensAberration wide(catEye(1.), RectI(0, 0, 960, 1080), 2., 1., 1.);
    double sx = 0.;
    double sy = 0.;
    double wx = 0.;
    double wy = 0.;

    square.cellPosition(10, 4, &sx, &sy);
    wide.cellPosition(5, 4, &wx, &wy);
    EXPECT_NEAR(sx, wx, 1e-12);
    EXPECT_NEAR(sy, wy, 1e-12);
}

TEST(LensAberration, ChromaticScales)
{
    LensAberrationParams params;

    params.chromaticAberration = 0.25;
    const LensAberration lens(params, kHd, 1., 1., 1.);
    EXPECT_TRUE(lens.hasChromatic());
    EXPECT_FALSE(lens.hasCatEye());
    EXPECT_DOUBLE_EQ(lens.channelScale(0), 1.25);
    EXPECT_DOUBLE_EQ(lens.channelScale(1), 1.);
    EXPECT_DOUBLE_EQ(lens.channelScale(2), 0.75);
    // The alpha and any other channel take the green's.
    EXPECT_DOUBLE_EQ(lens.channelScale(3), 1.);
    EXPECT_EQ(lens.groupOf(3), lens.groupOf(1));
    EXPECT_EQ(lens.groupCount(), 3);
    EXPECT_DOUBLE_EQ(lens.maxScale(), 1.25);
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(lens.groupScale(lens.groupOf(c)), lens.channelScale(c));
    }

    // Channels of one scale share a group; no scale is negative.
    params.chromaticAberration = 2.;
    params.chromaticOffset[0] = 0.;
    params.chromaticOffset[1] = 0.;
    params.chromaticOffset[2] = -1.;
    const LensAberration shared(params, kHd, 1., 1., 1.);
    EXPECT_EQ(shared.groupCount(), 2);
    EXPECT_EQ(shared.groupOf(0), shared.groupOf(1));
    EXPECT_EQ(shared.channelScale(2), 0.);
}
