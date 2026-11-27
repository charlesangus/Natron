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

#include <cstddef>
#include <iterator>

#include <gtest/gtest.h>

#include "Gui/ViewerZoomLadder.h"

NATRON_NAMESPACE_USING

namespace {

const double kMin = 0.01;
const double kMax = 1024.;

double
step(double percent,
     int steps,
     double minFactor = kMin,
     double maxFactor = kMax)
{
    return 100. * stepZoomPreset(percent / 100., steps, minFactor, maxFactor);
}

} // namespace

TEST(ViewerZoomLadder, PresetsMatchTheZoomMenu)
{
    const int expected[] = { 10, 25, 50, 75, 100, 125, 150, 200, 400, 800, 1600, 2400, 3200, 6400 };

    ASSERT_EQ(std::size(expected), std::size(kViewerZoomPresetsPercent));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        EXPECT_EQ(expected[i], kViewerZoomPresetsPercent[i]);
    }
}

TEST(ViewerZoomLadder, StepsUpAndBackAlongThePresets)
{
    const double up[] = { 100., 125., 150., 200., 400. };

    double level = up[0];
    for (std::size_t i = 1; i < std::size(up); ++i) {
        level = step(level, 1);
        EXPECT_DOUBLE_EQ(up[i], level);
    }
    for (std::size_t i = std::size(up) - 1; i > 0; --i) {
        level = step(level, -1);
        EXPECT_DOUBLE_EQ(up[i - 1], level);
    }
}

TEST(ViewerZoomLadder, OffLadderStepsToTheNextPresetInTheStepDirection)
{
    EXPECT_DOUBLE_EQ(100., step(87., 1));
    EXPECT_DOUBLE_EQ(75., step(87., -1));
    EXPECT_DOUBLE_EQ(125., step(112., 1));
    EXPECT_DOUBLE_EQ(100., step(112., -1));
}

TEST(ViewerZoomLadder, NearAPresetCountsAsOnIt)
{
    EXPECT_DOUBLE_EQ(125., step(99.8, 1));
    EXPECT_DOUBLE_EQ(75., step(99.8, -1));
    EXPECT_DOUBLE_EQ(125., step(100.4, 1));
    EXPECT_DOUBLE_EQ(75., step(100.4, -1));
    EXPECT_DOUBLE_EQ(50., step(25. * (1. + 1e-12), 1));
    EXPECT_DOUBLE_EQ(10., step(25. * (1. - 1e-12), -1));
}

TEST(ViewerZoomLadder, HalvesBelowTheFirstPreset)
{
    EXPECT_DOUBLE_EQ(5., step(10., -1));
    EXPECT_DOUBLE_EQ(2.5, step(5., -1));
    EXPECT_DOUBLE_EQ(1.25, step(2.5, -1));
    EXPECT_DOUBLE_EQ(2.5, step(1.25, 1));
    EXPECT_DOUBLE_EQ(10., step(5., 1));
    EXPECT_DOUBLE_EQ(5., step(7., -1));
    EXPECT_DOUBLE_EQ(10., step(7., 1));
}

TEST(ViewerZoomLadder, DoublesAboveTheLastPreset)
{
    EXPECT_DOUBLE_EQ(12800., step(6400., 1));
    EXPECT_DOUBLE_EQ(25600., step(12800., 1));
    EXPECT_DOUBLE_EQ(6400., step(12800., -1));
    EXPECT_DOUBLE_EQ(12800., step(9000., 1));
    EXPECT_DOUBLE_EQ(6400., step(9000., -1));
}

TEST(ViewerZoomLadder, ClampsToTheZoomBounds)
{
    EXPECT_DOUBLE_EQ(1., step(1.25, -1));
    EXPECT_DOUBLE_EQ(1., step(1., -1));
    EXPECT_DOUBLE_EQ(1.25, step(1., 1));
    EXPECT_DOUBLE_EQ(100. * kMax, step(100. * kMax, 1));
    EXPECT_DOUBLE_EQ(100. * kMax, step(100. * kMax, 5));
    EXPECT_DOUBLE_EQ(100. * kMax / 2., step(100. * kMax, -1));
    EXPECT_DOUBLE_EQ(200., step(100., 3, kMin, 2.));
    EXPECT_DOUBLE_EQ(50., step(100., -3, 0.5, kMax));
}

TEST(ViewerZoomLadder, StepsSeveralPresetsAtOnce)
{
    EXPECT_DOUBLE_EQ(400., step(100., 4));
    EXPECT_DOUBLE_EQ(10., step(100., -4));
    EXPECT_DOUBLE_EQ(150., step(87., 3));
    EXPECT_DOUBLE_EQ(50., step(87., -2));
    EXPECT_DOUBLE_EQ(1600., step(400., 2));
    EXPECT_DOUBLE_EQ(25600., step(3200., 3));
    EXPECT_DOUBLE_EQ(2.5, step(25., -3));
}

TEST(ViewerZoomLadder, ZeroStepsLeavesTheLevel)
{
    EXPECT_DOUBLE_EQ(87., step(87., 0));
}

namespace {

ViewerWheelZoom
wheelSnap(double rawPercent,
          double scale,
          double minFactor = kMin,
          double maxFactor = kMax)
{
    ViewerWheelZoom z = wheelZoomSnap(rawPercent / 100., scale, minFactor, maxFactor);
    z.raw *= 100.;
    z.factor *= 100.;

    return z;
}

} // namespace

TEST(ViewerZoomLadder, WheelLandsOnAPresetWithinItsBand)
{
    EXPECT_DOUBLE_EQ(100., wheelSnap(95., 1.02).factor);
    EXPECT_NEAR(96.9, wheelSnap(95., 1.02).raw, 1e-9);
    EXPECT_DOUBLE_EQ(75., wheelSnap(80., 0.95).factor);
    EXPECT_DOUBLE_EQ(125., wheelSnap(110., 1.1).factor);
}

TEST(ViewerZoomLadder, WheelZoomsSmoothlyBetweenPresets)
{
    EXPECT_NEAR(168., wheelSnap(160., 1.05).factor, 1e-9);
    EXPECT_NEAR(168., wheelSnap(160., 1.05).raw, 1e-9);
    EXPECT_NEAR(300., wheelSnap(250., 1.2).factor, 1e-9);
    EXPECT_NEAR(83.6, wheelSnap(88., 0.95).factor, 1e-9);
}

TEST(ViewerZoomLadder, LeavingAPresetTakesItsBand)
{
    ViewerWheelZoom z = wheelSnap(100., 1.05);
    EXPECT_DOUBLE_EQ(100., z.factor);
    z = wheelSnap(z.raw, 1.05);
    EXPECT_NEAR(110.25, z.factor, 1e-9);
    z = wheelSnap(100., 0.95);
    EXPECT_DOUBLE_EQ(100., z.factor);
    z = wheelSnap(z.raw, 0.95);
    EXPECT_NEAR(90.25, z.factor, 1e-9);
}

TEST(ViewerZoomLadder, WheelNotchesFromAPreset)
{
    const double expected[] = { 125., 150., 172.8, 200. };
    ViewerWheelZoom z { 1., 1. };

    for (double e : expected) {
        z = wheelZoomSnap(z.raw, 1.2, kMin, kMax);
        EXPECT_NEAR(e, 100. * z.factor, 1e-9);
    }
}

TEST(ViewerZoomLadder, AStepThatJumpsABandLandsOnItsPreset)
{
    ViewerWheelZoom z = wheelSnap(90., 1.25);
    EXPECT_DOUBLE_EQ(100., z.factor);
    EXPECT_DOUBLE_EQ(100., z.raw);
    z = wheelSnap(110., 0.8);
    EXPECT_DOUBLE_EQ(100., z.factor);
    EXPECT_DOUBLE_EQ(75., wheelSnap(30., 3.).factor);
    EXPECT_DOUBLE_EQ(100., wheelSnap(30., 3.5).factor);
}

TEST(ViewerZoomLadder, WheelClampsToTheZoomBounds)
{
    EXPECT_DOUBLE_EQ(100. * kMax, wheelSnap(100000., 2.).factor);
    EXPECT_DOUBLE_EQ(100. * kMin, wheelSnap(1.5, 0.5).factor);
    EXPECT_DOUBLE_EQ(100. * kMin, wheelSnap(1., 0.5).factor);
}
