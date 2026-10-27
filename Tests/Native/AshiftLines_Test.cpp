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
#include <string>
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

// The segment from (x1, y1) to (x2, y2) of the corrected image, mapped back into the input.
Ashift::Segment
inputSegment(const TransformMath::Mat3& inverse,
             double x1,
             double y1,
             double x2,
             double y2)
{
    Ashift::Segment s;

    mapPoint(inverse, x1, y1, &s.x1, &s.y1);
    mapPoint(inverse, x2, y2, &s.x2, &s.y2);
    s.width = 1.5;
    s.p = 0.125;
    s.logNfa = 10.;

    return s;
}

// A grid of 7 verticals and 5 horizontals that model makes axis-aligned, as the input sees it,
// with three diagonals and, when withOutlier, one vertical tilted by 10 degrees.
std::vector<Ashift::Segment>
gridSegments(const Ashift::Model& model,
             bool withOutlier)
{
    const TransformMath::Mat3 forward = Ashift::homography(model, kWidth, kHeight, false);
    const TransformMath::Mat3 inverse = Ashift::homography(model, kWidth, kHeight, true);
    double cx = 0., cy = 0.;

    mapPoint(forward, kWidth / 2., kHeight / 2., &cx, &cy);
    std::vector<Ashift::Segment> segments;
    for (int k = -3; k <= 3; ++k) {
        segments.push_back(inputSegment(inverse, cx + 40. * k, cy - 80., cx + 40. * k, cy + 80.));
    }
    for (int k = -2; k <= 2; ++k) {
        segments.push_back(inputSegment(inverse, cx - 110., cy + 30. * k, cx + 110., cy + 30. * k));
    }
    for (int k = 0; k < 3; ++k) {
        Ashift::Segment s;
        s.x1 = 40. + 60. * k;
        s.y1 = 50.;
        s.x2 = s.x1 + 40.;
        s.y2 = s.y1 + 40.;
        s.p = 0.125;
        segments.push_back(s);
    }
    if (withOutlier) {
        const double tilt = 10. * M_PI / 180.;
        Ashift::Segment s;
        s.x1 = 300.;
        s.y1 = 40.;
        s.x2 = s.x1 + 150. * std::sin(tilt);
        s.y2 = s.y1 + 150. * std::cos(tilt);
        s.width = 1.5;
        s.p = 0.125;
        segments.push_back(s);
    }

    return segments;
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

// The detector a test passes stands in for LSD: detectLines() classifies its segments, the
// outlier removal keeps the grid and the rotation fit recovers the 3 degree tilt.
TEST(AshiftLines, RotatedGridIsClassifiedAndFitWithinATenthOfADegree)
{
    Ashift::Model truth;
    truth.rotation = 3.;
    const std::vector<Ashift::Segment> segments = gridSegments(truth, true);
    const Ashift::SegmentDetector detector = [&segments](const std::vector<double>& grey, int width, int height) {
        EXPECT_EQ((std::size_t)width * height, grey.size());

        return segments;
    };
    std::vector<float> rgba((std::size_t)kWidth * kHeight * 4, 0.5f);
    Ashift::LineSet set;
    std::string error;

    ASSERT_TRUE(Ashift::detectLines(rgba, kWidth, kHeight, 1., Ashift::eEnhanceNone, detector, &set, &error)) << error;
    EXPECT_EQ(kWidth, set.width);
    EXPECT_EQ(kHeight, set.height);
    EXPECT_EQ(8, set.verticalCount);
    EXPECT_EQ(5, set.horizontalCount);
    EXPECT_EQ(3, countType(set, Ashift::eLineIrrelevant));
    EXPECT_EQ(8, countType(set, Ashift::eLineVerticalSelected));

    Ashift::removeOutliers(&set, 1);
    EXPECT_EQ(7, countType(set, Ashift::eLineVerticalSelected));
    EXPECT_EQ(5, countType(set, Ashift::eLineHorizontalSelected));
    EXPECT_EQ(Ashift::eLineVerticalNotSelected, set.lines.back().type & Ashift::eLineMask);

    Ashift::Model fitted;
    ASSERT_EQ(Ashift::eNmsSuccess, Ashift::fit(set, Ashift::eFitRotationBothLines, &fitted));
    EXPECT_NEAR(truth.rotation, fitted.rotation, 0.1);
}

// detectLines() scales x by the pixel aspect ratio and widens the line set to match.
TEST(AshiftLines, PixelAspectRatioScalesTheSegments)
{
    std::vector<Ashift::Segment> segments(1);
    segments[0].x1 = 50.;
    segments[0].y1 = 20.;
    segments[0].x2 = 50.;
    segments[0].y2 = 120.;
    segments[0].p = 0.125;
    const Ashift::SegmentDetector detector = [&segments](const std::vector<double>&, int, int) {
        return segments;
    };
    std::vector<float> rgba((std::size_t)kWidth * kHeight * 4, 0.5f);
    Ashift::LineSet set;
    std::string error;

    ASSERT_TRUE(Ashift::detectLines(rgba, kWidth, kHeight, 2., Ashift::eEnhanceNone, detector, &set, &error)) << error;
    EXPECT_EQ(2 * kWidth, set.width);
    ASSERT_EQ(1u, set.lines.size());
    EXPECT_DOUBLE_EQ(100., set.lines[0].p1[0] / set.lines[0].p1[2]);
    EXPECT_EQ(Ashift::eLineVerticalSelected, set.lines[0].type & Ashift::eLineMask);
}

// Segments hugging the image border are dropped, as darktable drops them; an empty result fails.
TEST(AshiftLines, BorderSegmentsAreDroppedAndNothingFails)
{
    std::vector<Ashift::Segment> segments(2);
    segments[0].x1 = segments[0].x2 = 0.5;
    segments[0].y1 = 10.;
    segments[0].y2 = 200.;
    segments[1].x1 = 10.;
    segments[1].x2 = 300.;
    segments[1].y1 = segments[1].y2 = kHeight - 1.;
    const Ashift::SegmentDetector detector = [&segments](const std::vector<double>&, int, int) {
        return segments;
    };
    std::vector<float> rgba((std::size_t)kWidth * kHeight * 4, 0.5f);
    Ashift::LineSet set;
    std::string error;

    EXPECT_FALSE(Ashift::detectLines(rgba, kWidth, kHeight, 1., Ashift::eEnhanceEdges | Ashift::eEnhanceDetail, detector, &set, &error));
    EXPECT_EQ("could not detect structural data in image", error);
}

TEST(AshiftLines, NoDetectorFails)
{
    if (Ashift::hasSegmentDetector()) {
        return;
    }
    std::vector<float> rgba((std::size_t)16 * 16 * 4, 0.5f);
    Ashift::LineSet set;
    std::string error;

    EXPECT_FALSE(Ashift::detectLines(rgba, 16, 16, 1., Ashift::eEnhanceNone, Ashift::SegmentDetector(), &set, &error));
    EXPECT_EQ("no line segment detector is available", error);
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

TEST(AshiftLines, GammaClampsNegativesAndLeavesAlpha)
{
    std::vector<float> rgba = { 0.25f, -1.f, 1.f, 0.3f };

    Ashift::gammaCorrect(&rgba);
    EXPECT_NEAR(std::pow(0.25, 0.45), rgba[0], 1e-6);
    EXPECT_EQ(0.f, rgba[1]);
    EXPECT_NEAR(1., rgba[2], 1e-6);
    EXPECT_EQ(0.3f, rgba[3]);
}

TEST(AshiftLines, GreyIsLumaTimes256)
{
    const std::vector<float> rgba = { 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f };
    const std::vector<double> grey = Ashift::rgbToGrey256(rgba, 2, 1);

    ASSERT_EQ(2u, grey.size());
    EXPECT_NEAR(256., grey[0], 1e-4);
    EXPECT_NEAR(0.3 * 256., grey[1], 1e-4);
}

// The gradient magnitude is zero on flat areas and peaks on a step.
TEST(AshiftLines, EdgeEnhanceFindsAStep)
{
    const int w = 16, h = 12;
    std::vector<double> grey((std::size_t)w * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            grey[(std::size_t)y * w + x] = (x < w / 2) ? 10. : 200.;
        }
    }
    Ashift::edgeEnhance(&grey, w, h);
    for (int y = 2; y < h - 2; ++y) {
        EXPECT_NEAR(0., grey[(std::size_t)y * w + 3], 1e-9);
        EXPECT_NEAR(0., grey[(std::size_t)y * w + w - 4], 1e-9);
        EXPECT_GT(std::max(grey[(std::size_t)y * w + w / 2 - 1], grey[(std::size_t)y * w + w / 2]), 50.);
    }
}

// The detail enhancement leaves a flat colour as it is.
TEST(AshiftLines, DetailEnhanceKeepsAFlatColour)
{
    const int w = 24, h = 20;
    std::vector<float> rgba((std::size_t)w * h * 4);
    for (std::size_t p = 0; p < (std::size_t)w * h; ++p) {
        rgba[4 * p] = 0.4f;
        rgba[4 * p + 1] = 0.3f;
        rgba[4 * p + 2] = 0.2f;
        rgba[4 * p + 3] = 1.f;
    }
    Ashift::detailEnhance(&rgba, w, h);
    for (std::size_t p = 0; p < (std::size_t)w * h; ++p) {
        EXPECT_NEAR(0.4, rgba[4 * p], 2e-3);
        EXPECT_NEAR(0.3, rgba[4 * p + 1], 2e-3);
        EXPECT_NEAR(0.2, rgba[4 * p + 2], 2e-3);
    }
}
