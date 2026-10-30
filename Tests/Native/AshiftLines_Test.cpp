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

#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/AshiftFit.h"
#include "Engine/Nodes/Transform/AshiftLines.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 320;
const int kHeight = 240;

void
mapPoint(const TransformMath::Mat3& m,
         double x,
         double y,
         double* ox,
         double* oy)
{
    const double w = m.m[6] * x + m.m[7] * y + m.m[8];

    *ox = (m.m[0] * x + m.m[1] * y + m.m[2]) / w;
    *oy = (m.m[3] * x + m.m[4] * y + m.m[5]) / w;
}

// The guide line from (x1, y1) to (x2, y2) of the corrected image, mapped back into the input.
Ashift::DrawnLine
inputLine(const TransformMath::Mat3& inverse,
          double x1,
          double y1,
          double x2,
          double y2,
          bool vertical)
{
    Ashift::DrawnLine line;

    mapPoint(inverse, x1, y1, &line.x1, &line.y1);
    mapPoint(inverse, x2, y2, &line.x2, &line.y2);
    line.vertical = vertical;

    return line;
}

// Four verticals and three horizontals that model makes axis-aligned, as the input sees them.
std::vector<Ashift::DrawnLine>
gridLines(const Ashift::Model& model)
{
    const TransformMath::Mat3 forward = Ashift::homography(model, kWidth, kHeight, false);
    const TransformMath::Mat3 inverse = Ashift::homography(model, kWidth, kHeight, true);
    double cx = 0., cy = 0.;

    mapPoint(forward, kWidth / 2., kHeight / 2., &cx, &cy);
    std::vector<Ashift::DrawnLine> lines;
    for (int k = -2; k <= 1; ++k) {
        lines.push_back(inputLine(inverse, cx + 50. * k + 25., cy - 80., cx + 50. * k + 25., cy + 80., true));
    }
    for (int k = -1; k <= 1; ++k) {
        lines.push_back(inputLine(inverse, cx - 110., cy + 40. * k, cx + 110., cy + 40. * k, false));
    }

    return lines;
}

int
countType(const Ashift::LineSet& set,
          int type)
{
    int n = 0;

    for (const Ashift::Line& line : set.lines) {
        n += ((line.type & Ashift::eLineMask) == type) ? 1 : 0;
    }

    return n;
}
} // namespace

// makeLine(): darktable's homogeneous line through both end points, normalised, weight 1.
TEST(AshiftLines, MakeLineIsTheNormalisedCrossProduct)
{
    const Ashift::Line line = Ashift::makeLine(10., 20., 10., 120., Ashift::eLineVerticalSelected);

    EXPECT_DOUBLE_EQ(10., line.p1[0]);
    EXPECT_DOUBLE_EQ(120., line.p2[1]);
    EXPECT_DOUBLE_EQ(1., line.p1[2]);
    EXPECT_DOUBLE_EQ(100., line.length);
    EXPECT_DOUBLE_EQ(1., line.weight);
    EXPECT_EQ(Ashift::eLineVerticalSelected, line.type);
    // Both end points lie on the line.
    EXPECT_NEAR(0., line.L[0] * line.p1[0] + line.L[1] * line.p1[1] + line.L[2], 1e-9);
    EXPECT_NEAR(0., line.L[0] * line.p2[0] + line.L[1] * line.p2[1] + line.L[2], 1e-9);
}

// drawnLines() keeps each drawn line's tag, counts and weighs them per direction and drops lines
// of zero length.
TEST(AshiftLines, DrawnLinesKeepTheirTags)
{
    std::vector<Ashift::DrawnLine> drawn = gridLines(Ashift::Model());
    Ashift::DrawnLine degenerate;
    degenerate.x1 = degenerate.x2 = 40.;
    degenerate.y1 = degenerate.y2 = 50.;
    drawn.push_back(degenerate);
    // A steep line tagged horizontal stays horizontal: the tag, not the direction, decides.
    Ashift::DrawnLine steep;
    steep.x1 = 30.;
    steep.y1 = 20.;
    steep.x2 = 34.;
    steep.y2 = 200.;
    steep.vertical = false;
    drawn.push_back(steep);

    const Ashift::LineSet set = Ashift::drawnLines(drawn, kWidth, kHeight);

    EXPECT_EQ(kWidth, set.width);
    EXPECT_EQ(kHeight, set.height);
    ASSERT_EQ(8u, set.lines.size());
    EXPECT_EQ(4, countType(set, Ashift::eLineVerticalSelected));
    EXPECT_EQ(4, countType(set, Ashift::eLineHorizontalSelected));
    EXPECT_EQ(4, set.verticalCount);
    EXPECT_EQ(4, set.horizontalCount);
    EXPECT_DOUBLE_EQ(4., set.verticalWeight);
    EXPECT_DOUBLE_EQ(4., set.horizontalWeight);
    EXPECT_EQ(Ashift::eLineHorizontalSelected, set.lines.back().type);
}

// Drawn lines that a 3 degree rotation makes axis-aligned fit back within a tenth of a degree,
// and a keystone with rotation fits back from the drawn lines alone.
TEST(AshiftLines, DrawnLinesFitRecoversTheModel)
{
    {
        Ashift::Model truth;
        truth.rotation = 3.;
        const Ashift::LineSet set = Ashift::drawnLines(gridLines(truth), kWidth, kHeight);
        Ashift::Model fitted;

        ASSERT_EQ(Ashift::eNmsSuccess, Ashift::fit(set, Ashift::eFitRotationBothLines, &fitted));
        EXPECT_NEAR(truth.rotation, fitted.rotation, 0.1);
    }
    {
        Ashift::Model truth;
        truth.rotation = -2.;
        truth.lensShiftV = 0.25;
        truth.lensShiftH = 0.1;
        const Ashift::LineSet set = Ashift::drawnLines(gridLines(truth), kWidth, kHeight);
        Ashift::Model fitted;

        ASSERT_EQ(Ashift::eNmsSuccess, Ashift::fit(set, Ashift::eFitBoth, &fitted));
        EXPECT_NEAR(truth.rotation, fitted.rotation, 0.05);
        EXPECT_NEAR(truth.lensShiftV, fitted.lensShiftV, 0.01);
        EXPECT_NEAR(truth.lensShiftH, fitted.lensShiftH, 0.01);
    }
}

// Too few lines in a fitted direction: the fit says so and leaves the model alone.
TEST(AshiftLines, TooFewDrawnLinesFail)
{
    std::vector<Ashift::DrawnLine> drawn = gridLines(Ashift::Model());
    drawn.resize(1);
    const Ashift::LineSet set = Ashift::drawnLines(drawn, kWidth, kHeight);
    Ashift::Model fitted;

    EXPECT_EQ(Ashift::eNmsNotEnoughLines, Ashift::fit(set, Ashift::eFitVertically, &fitted));
    EXPECT_EQ(0., fitted.rotation);
    EXPECT_EQ(0., fitted.lensShiftV);
}

// darktable's rectangle mode: two verticals (left, right) then two horizontals (top, bottom).
TEST(AshiftLines, QuadLinesTypesItsSides)
{
    // Top left, bottom left, top right, bottom right.
    const double pts[8] = { 10., 12., 14., 200., 300., 8., 296., 204. };
    const Ashift::LineSet set = Ashift::quadLines(pts, kWidth, kHeight);

    ASSERT_EQ(4u, set.lines.size());
    EXPECT_EQ(Ashift::eLineVerticalSelected, set.lines[0].type);
    EXPECT_EQ(Ashift::eLineVerticalSelected, set.lines[1].type);
    EXPECT_EQ(Ashift::eLineHorizontalSelected, set.lines[2].type);
    EXPECT_EQ(Ashift::eLineHorizontalSelected, set.lines[3].type);
    EXPECT_EQ(2, set.verticalCount);
    EXPECT_EQ(2, set.horizontalCount);
    EXPECT_DOUBLE_EQ(2., set.verticalWeight);
    EXPECT_DOUBLE_EQ(2., set.horizontalWeight);
    EXPECT_EQ(kWidth, set.width);
    EXPECT_EQ(kHeight, set.height);
}

// A quadrilateral that a known model makes rectangular: the fit of rotation and both lens shifts
// recovers the model.
TEST(AshiftLines, QuadFitRecoversTheModel)
{
    Ashift::Model truth;
    truth.rotation = -2.5;
    truth.lensShiftV = 0.2;
    truth.lensShiftH = -0.1;
    const TransformMath::Mat3 forward = Ashift::homography(truth, kWidth, kHeight, false);
    const TransformMath::Mat3 inverse = Ashift::homography(truth, kWidth, kHeight, true);
    double cx = 0., cy = 0.;
    mapPoint(forward, kWidth / 2., kHeight / 2., &cx, &cy);
    const double corners[4][2] = { { cx - 90., cy - 70. }, { cx - 90., cy + 70. }, { cx + 90., cy - 70. }, { cx + 90., cy + 70. } };
    double pts[8];
    for (int i = 0; i < 4; ++i) {
        mapPoint(inverse, corners[i][0], corners[i][1], &pts[2 * i], &pts[2 * i + 1]);
    }
    const Ashift::LineSet set = Ashift::quadLines(pts, kWidth, kHeight);
    Ashift::Model fitted;

    ASSERT_EQ(Ashift::eNmsSuccess, Ashift::fit(set, Ashift::eFitBoth, &fitted));
    EXPECT_NEAR(truth.rotation, fitted.rotation, 0.05);
    EXPECT_NEAR(truth.lensShiftV, fitted.lensShiftV, 0.01);
    EXPECT_NEAR(truth.lensShiftH, fitted.lensShiftH, 0.01);
}
