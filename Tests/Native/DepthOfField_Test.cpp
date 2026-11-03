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
#include <limits>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/DepthOfField.h"

NATRON_NAMESPACE_USING

namespace {

DepthOfFieldParams
manualParams(DepthUnits units)
{
    DepthOfFieldParams params;

    params.units = units;
    params.mode = DepthOfFieldMode::Manual;
    params.size = 20.;
    params.maxSize = 50.;

    return params;
}

// 50 mm at f/2 on a 36 mm film back, focused at 2 m, depth in metres, 1920 pixels wide.
DepthOfFieldParams
lensParams()
{
    DepthOfFieldParams params;

    params.units = DepthUnits::Distance;
    params.mode = DepthOfFieldMode::Lens;
    params.focalPlane = 2.;
    params.focalLength = 50.;
    params.fStop = 2.;
    params.filmBackWidth = 36.;
    params.imageWidth = 1920.;
    params.millimetresPerUnit = 1000.;
    params.maxSize = 500.;

    return params;
}

// f^2 / (N (S - f)) * |D - S| / D millimetres on the film back, times 1920 / 36 pixels per mm.
double
handLensCoc(double distanceMm,
            double fStop)
{
    const double f = 50.;
    const double s = 2000.;
    const double mm = std::isinf(distanceMm) ? f * f / (fStop * (s - f)) : f * f / (fStop * (s - f)) * std::abs(distanceMm - s) / distanceMm;

    return mm * 1920. / 36.;
}
} // anonymous namespace

TEST(DepthOfField, InvalidDepthIsInfinitelyFar)
{
    const double inf = std::numeric_limits<double>::infinity();

    for (DepthUnits units : { DepthUnits::Distance, DepthUnits::InverseDistance }) {
        EXPECT_TRUE(std::isinf(depthDistance(0., units)));
        EXPECT_TRUE(std::isinf(depthDistance(-1., units)));
        EXPECT_TRUE(std::isinf(depthDistance(std::numeric_limits<double>::quiet_NaN(), units)));
        EXPECT_TRUE(std::isinf(depthDistance(inf, units)));
    }
    EXPECT_DOUBLE_EQ(depthDistance(4., DepthUnits::Distance), 4.);
    EXPECT_DOUBLE_EQ(depthDistance(4., DepthUnits::InverseDistance), 0.25);
}

TEST(DepthOfField, ManualIsZeroInsideTheDepthOfField)
{
    DepthOfFieldParams params = manualParams(DepthUnits::Distance);
    params.focalPlane = 10.;
    params.depthOfField = 4.;
    const DepthOfField dof(params);

    for (double z : { 8., 9., 10., 11.5, 12. }) {
        EXPECT_EQ(dof.coc(z), 0.) << z;
    }
    EXPECT_LT(dof.coc(7.9), 0.);
    EXPECT_GT(dof.coc(12.1), 0.);
    // Size is the diameter infinitely far behind; invalid depth is infinitely far.
    EXPECT_NEAR(dof.coc(0.), 20., 1e-9);
    EXPECT_NEAR(dof.coc(-3.), 20., 1e-9);
    EXPECT_NEAR(dof.backMax(), 20., 1e-9);
    EXPECT_NEAR(dof.frontMax(), 50., 1e-9);
    // Monotone on each side, and clamped to the maximum close to the camera.
    EXPECT_LT(dof.coc(4.), dof.coc(6.));
    EXPECT_LT(dof.coc(20.), dof.coc(40.));
    EXPECT_NEAR(dof.coc(0.01), -50., 1e-9);
}

TEST(DepthOfField, ManualInverseDistance)
{
    DepthOfFieldParams params = manualParams(DepthUnits::InverseDistance);
    // In focus from 1 / 0.625 to 1 / 0.375 distance units.
    params.focalPlane = 0.5;
    params.depthOfField = 0.25;
    const DepthOfField dof(params);

    for (double z : { 0.375, 0.45, 0.5, 0.625 }) {
        EXPECT_EQ(dof.coc(z), 0.) << z;
    }
    // A larger inverse distance is nearer.
    EXPECT_LT(dof.coc(0.8), 0.);
    EXPECT_GT(dof.coc(0.2), 0.);
    EXPECT_GT(dof.coc(0.1), dof.coc(0.2));
    EXPECT_NEAR(dof.coc(0.), 20., 1e-9);
    // Behind the slice the diameter is linear in inverse distance: halfway to infinity, half Size.
    EXPECT_NEAR(dof.coc(0.1875), 10., 1e-9);
}

TEST(DepthOfField, ManualSizeZeroIsSharp)
{
    DepthOfFieldParams params = manualParams(DepthUnits::Distance);
    params.size = 0.;
    params.focalPlane = 5.;
    const DepthOfField dof(params);

    EXPECT_EQ(dof.coc(1.), 0.);
    EXPECT_EQ(dof.coc(100.), 0.);
    EXPECT_EQ(dof.frontMax(), 0.);
    EXPECT_EQ(dof.backMax(), 0.);
}

TEST(DepthOfField, LensMatchesHandComputedCircles)
{
    const DepthOfField dof(lensParams());

    EXPECT_EQ(dof.coc(2.), 0.);
    // 1282.05 mm^2 * |1/2000 - 1/4000| = 0.3205 mm, 17.09 pixels.
    EXPECT_NEAR(dof.coc(4.), handLensCoc(4000., 2.), 1e-9);
    EXPECT_NEAR(dof.coc(4.), 17.0940, 1e-3);
    EXPECT_NEAR(dof.coc(1.), -handLensCoc(1000., 2.), 1e-9);
    EXPECT_NEAR(dof.coc(1.), -34.1880, 1e-3);
    EXPECT_NEAR(dof.coc(3.), handLensCoc(3000., 2.), 1e-9);
    EXPECT_NEAR(dof.coc(0.), handLensCoc(std::numeric_limits<double>::infinity(), 2.), 1e-9);
    EXPECT_NEAR(dof.backMax(), 34.1880, 1e-3);
    EXPECT_NEAR(dof.frontMax(), 500., 1e-9);
    // Closer than the focal length clamps to the maximum.
    EXPECT_EQ(dof.coc(0.04), -500.);
}

TEST(DepthOfField, LensInverseDistanceMatchesDistance)
{
    DepthOfFieldParams params = lensParams();
    const DepthOfField distance(params);
    params.units = DepthUnits::InverseDistance;
    params.focalPlane = 0.5;
    const DepthOfField inverse(params);

    for (double d : { 0.5, 1., 1.5, 2., 3., 7., 100. }) {
        EXPECT_NEAR(inverse.coc(1. / d), distance.coc(d), 1e-9) << d;
    }
}

TEST(DepthOfField, DoublingTheFStopHalvesTheCircle)
{
    DepthOfFieldParams params = lensParams();
    const DepthOfField f2(params);
    params.fStop = 4.;
    const DepthOfField f4(params);

    for (double d : { 0.5, 1., 3., 4., 10. }) {
        EXPECT_NEAR(f4.coc(d), f2.coc(d) / 2., 1e-9) << d;
    }
    EXPECT_NEAR(f4.backMax(), f2.backMax() / 2., 1e-9);
}

TEST(DepthOfField, LensFocusedAtInfinity)
{
    DepthOfFieldParams params = lensParams();
    params.focalPlane = 0.;
    const DepthOfField dof(params);

    // f^2 / (N D) on the film back in front, nothing behind.
    EXPECT_EQ(dof.coc(0.), 0.);
    EXPECT_NEAR(dof.coc(10.), -2500. / (2. * 10000.) * 1920. / 36., 1e-9);
    EXPECT_EQ(dof.backMax(), 0.);
}
