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

#include <gtest/gtest.h>

#include "Gui/ViewerPixelGrid.h"

NATRON_NAMESPACE_USING

namespace {

double
fractionalPart(double v)
{
    return v - std::floor(v);
}

} // namespace

TEST(ViewerPixelGrid, OddViewerAtFullSizePutsTheImageOnWholePixels)
{
    // A 320x200 image centred in a 1137x319 viewer at 100%.
    double left = 160. - 1137. / 2.;
    double right = left + 1137.;
    double bottom = 100. - 319. / 2.;
    double top = bottom + 319.;
    snapProjectionToPixelGrid(&left, &right, &bottom, &top, 1137, 319);

    EXPECT_DOUBLE_EQ(1137., right - left);
    EXPECT_DOUBLE_EQ(319., top - bottom);
    EXPECT_DOUBLE_EQ(0., fractionalPart(left));
    EXPECT_DOUBLE_EQ(0., fractionalPart(bottom));
    EXPECT_LE(std::abs(left - (160. - 1137. / 2.)), 0.5);
    EXPECT_LE(std::abs(bottom - (100. - 319. / 2.)), 0.5);
}

TEST(ViewerPixelGrid, AlignedProjectionIsUnchanged)
{
    double left = -400.;
    double right = 600.;
    double bottom = -50.;
    double top = 250.;
    snapProjectionToPixelGrid(&left, &right, &bottom, &top, 1000, 300);

    EXPECT_DOUBLE_EQ(-400., left);
    EXPECT_DOUBLE_EQ(600., right);
    EXPECT_DOUBLE_EQ(-50., bottom);
    EXPECT_DOUBLE_EQ(250., top);
}

TEST(ViewerPixelGrid, SnapsInDevicePixelsAtAnyZoom)
{
    // 200% on a 2x device pixel ratio: four device pixels per canonical unit.
    const int viewportWidth = 1001 * 2;
    const int viewportHeight = 401 * 2;
    double left = -137.3;
    double right = left + 1001. / 2.;
    double bottom = 12.61;
    double top = bottom + 401. / 2.;
    snapProjectionToPixelGrid(&left, &right, &bottom, &top, viewportWidth, viewportHeight);

    const double ppuX = viewportWidth / (right - left);
    const double ppuY = viewportHeight / (top - bottom);
    EXPECT_NEAR(4., ppuX, 1e-9);
    EXPECT_NEAR(4., ppuY, 1e-9);
    EXPECT_NEAR(std::floor(left * ppuX + 0.5), left * ppuX, 1e-9);
    EXPECT_NEAR(std::floor(bottom * ppuY + 0.5), bottom * ppuY, 1e-9);
    EXPECT_LE(std::abs(left - -137.3) * ppuX, 0.5);
    EXPECT_LE(std::abs(bottom - 12.61) * ppuY, 0.5);
}

TEST(ViewerPixelGrid, EmptyViewportLeavesTheProjectionAlone)
{
    double left = 0.25;
    double right = 10.25;
    double bottom = 0.75;
    double top = 0.75;
    snapProjectionToPixelGrid(&left, &right, &bottom, &top, 0, 10);

    EXPECT_DOUBLE_EQ(0.25, left);
    EXPECT_DOUBLE_EQ(10.25, right);
    EXPECT_DOUBLE_EQ(0.75, bottom);
    EXPECT_DOUBLE_EQ(0.75, top);
}
