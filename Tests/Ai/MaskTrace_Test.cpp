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
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Ai/MaskTrace.h"

NATRON_NAMESPACE_USING

namespace {

const double kPi = 3.14159265358979323846;

/*
 * A mask whose 0.5 iso-line is the circle (cx, cy, radius) exactly, up to the linear interpolation
 * between pixel centres: the value falls linearly with the distance across a two-pixel band.
 */
std::vector<float>
ringMask(int width,
         int height,
         double cx,
         double cy,
         double outer,
         double inner)
{
    std::vector<float> mask((size_t)width * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double out = std::min(1., std::max(0., (outer - d) * 0.5 + 0.5));
            const double in = (inner > 0.) ? std::min(1., std::max(0., (d - inner) * 0.5 + 0.5)) : 1.;
            mask[(size_t)y * width + x] = (float)std::min(out, in);
        }
    }

    return mask;
}

MaskTrace::Point
cubicAt(const MaskTrace::CurvePoint& a,
        const MaskTrace::CurvePoint& b,
        double t)
{
    const double m = 1. - t;
    MaskTrace::Point p;

    p.x = m * m * m * a.position.x + 3. * m * m * t * a.right.x + 3. * m * t * t * b.left.x + t * t * t * b.position.x;
    p.y = m * m * m * a.position.y + 3. * m * m * t * a.right.y + 3. * m * t * t * b.left.y + t * t * t * b.position.y;

    return p;
}

std::vector<MaskTrace::Point>
sampleContour(const MaskTrace::Contour& contour,
              int perSegment = 32)
{
    std::vector<MaskTrace::Point> samples;
    const std::size_t n = contour.points.size();

    for (std::size_t k = 0; k < n; ++k) {
        for (int s = 0; s < perSegment; ++s) {
            samples.push_back(cubicAt(contour.points[k], contour.points[(k + 1) % n], (double)s / perSegment));
        }
    }

    return samples;
}

// The Hausdorff distance between the traced curve and the circle.
double
hausdorffToCircle(const MaskTrace::Contour& contour,
                  double cx,
                  double cy,
                  double radius)
{
    // Dense enough that sample spacing adds little to the circle-to-curve distance.
    const std::vector<MaskTrace::Point> samples = sampleContour(contour, 512);
    double curveToCircle = 0.;

    for (const MaskTrace::Point& p : samples) {
        curveToCircle = std::max(curveToCircle, std::fabs(std::hypot(p.x - cx, p.y - cy) - radius));
    }
    double circleToCurve = 0.;
    for (int i = 0; i < 720; ++i) {
        const double a = 2. * kPi * i / 720.;
        const double qx = cx + radius * std::cos(a);
        const double qy = cy + radius * std::sin(a);
        double nearest = 1e300;
        for (const MaskTrace::Point& p : samples) {
            nearest = std::min(nearest, std::hypot(p.x - qx, p.y - qy));
        }
        circleToCurve = std::max(circleToCurve, nearest);
    }

    return std::max(curveToCircle, circleToCurve);
}

double
curveSignedArea(const MaskTrace::Contour& contour)
{
    MaskTrace::Polygon polygon;

    polygon.points = sampleContour(contour);

    return MaskTrace::signedArea(polygon);
}

} // namespace

TEST(MaskTrace, DiscTracesWithinHalfAPixel)
{
    const double cx = 48.3, cy = 47.6, radius = 30.;
    const std::vector<float> mask = ringMask(96, 96, cx, cy, radius, 0.);
    MaskTrace::Options options;

    options.simplify = 0.3;
    const std::vector<MaskTrace::Contour> contours = MaskTrace::trace(mask, 96, 96, options);
    ASSERT_EQ(1u, contours.size());
    EXPECT_FALSE(contours[0].isHole);
    EXPECT_EQ(0, contours[0].depth);
    EXPECT_GE(contours[0].points.size(), 3u);
    EXPECT_NEAR(kPi * radius * radius, contours[0].area, 0.01 * kPi * radius * radius);
    EXPECT_LT(hausdorffToCircle(contours[0], cx, cy, radius), 0.5);
    EXPECT_GT(curveSignedArea(contours[0]), 0.);
}

TEST(MaskTrace, CoarserSimplifyStillFitsWithFewerPoints)
{
    const double cx = 48.3, cy = 47.6, radius = 30.;
    const std::vector<float> mask = ringMask(96, 96, cx, cy, radius, 0.);
    MaskTrace::Options fine, coarse;

    fine.simplify = 0.3;
    coarse.simplify = 1.5;
    const std::vector<MaskTrace::Contour> a = MaskTrace::trace(mask, 96, 96, fine);
    const std::vector<MaskTrace::Contour> b = MaskTrace::trace(mask, 96, 96, coarse);
    ASSERT_EQ(1u, a.size());
    ASSERT_EQ(1u, b.size());
    EXPECT_LE(b[0].points.size(), a[0].points.size());
    EXPECT_LT(hausdorffToCircle(b[0], cx, cy, radius), 1.5);
}

TEST(MaskTrace, RingGivesAnOutlineAndAHole)
{
    const double cx = 50., cy = 44., outer = 34., inner = 14.;
    const std::vector<float> mask = ringMask(100, 90, cx, cy, outer, inner);
    MaskTrace::Options options;

    options.simplify = 0.3;
    const std::vector<MaskTrace::Contour> contours = MaskTrace::trace(mask, 100, 90, options);
    ASSERT_EQ(2u, contours.size());
    // Enclosing shapes come first.
    EXPECT_FALSE(contours[0].isHole);
    EXPECT_EQ(0, contours[0].depth);
    EXPECT_TRUE(contours[1].isHole);
    EXPECT_EQ(1, contours[1].depth);
    EXPECT_LT(hausdorffToCircle(contours[0], cx, cy, outer), 0.5);
    EXPECT_LT(hausdorffToCircle(contours[1], cx, cy, inner), 0.5);
    EXPECT_GT(curveSignedArea(contours[0]), 0.);
    EXPECT_LT(curveSignedArea(contours[1]), 0.);

    options.keepHoles = false;
    const std::vector<MaskTrace::Contour> noHoles = MaskTrace::trace(mask, 100, 90, options);
    ASSERT_EQ(1u, noHoles.size());
    EXPECT_FALSE(noHoles[0].isHole);
}

TEST(MaskTrace, SmallBlobsAreDropped)
{
    std::vector<float> mask = ringMask(96, 96, 48., 48., 20., 0.);

    // A 3x3 speck, far from the disc.
    for (int y = 4; y < 7; ++y) {
        for (int x = 4; x < 7; ++x) {
            mask[(size_t)y * 96 + x] = 1.f;
        }
    }
    MaskTrace::Options options;
    options.minArea = 64.;
    EXPECT_EQ(1u, MaskTrace::trace(mask, 96, 96, options).size());

    options.minArea = 1.;
    EXPECT_EQ(2u, MaskTrace::trace(mask, 96, 96, options).size());
}

TEST(MaskTrace, StraightModeKeepsCorners)
{
    // A filled rectangle: without smoothing, four corners and straight sides.
    const int w = 40, h = 30;
    std::vector<float> mask((size_t)w * h, 0.f);

    for (int y = 5; y < 25; ++y) {
        for (int x = 8; x < 32; ++x) {
            mask[(size_t)y * w + x] = 1.f;
        }
    }
    MaskTrace::Options options;
    options.smooth = false;
    options.simplify = 0.5;
    const std::vector<MaskTrace::Contour> contours = MaskTrace::trace(mask, w, h, options);
    ASSERT_EQ(1u, contours.size());
    // Marching squares cuts each corner with a short diagonal.
    EXPECT_LE(contours[0].points.size(), 8u);
    for (const MaskTrace::CurvePoint& p : contours[0].points) {
        EXPECT_EQ(p.position.x, p.left.x);
        EXPECT_EQ(p.position.y, p.right.y);
        EXPECT_GE(p.position.x, 7.9);
        EXPECT_LE(p.position.x, 32.1);
        EXPECT_GE(p.position.y, 4.9);
        EXPECT_LE(p.position.y, 25.1);
    }
}

TEST(MaskTrace, EmptyMaskHasNoContours)
{
    const std::vector<float> mask((size_t)32 * 32, 0.f);

    EXPECT_TRUE(MaskTrace::trace(mask, 32, 32, MaskTrace::Options()).empty());
}
