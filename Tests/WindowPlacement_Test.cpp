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

#include <vector>

#include <QPoint>
#include <QRect>
#include <QSize>

#include <gtest/gtest.h>

#include "Gui/WindowPlacement.h"

NATRON_NAMESPACE_USING

TEST(WindowPlacement, ScreenIndexPicksContainingScreenOfTwoSideBySide)
{
    const std::vector<QRect> geoms = { QRect(0, 0, 1920, 1080), QRect(1920, 0, 2560, 1440) };

    EXPECT_EQ(0, screenIndexAt(geoms, QPoint(100, 100), -1));
    EXPECT_EQ(1, screenIndexAt(geoms, QPoint(1920, 0), -1));
    EXPECT_EQ(1, screenIndexAt(geoms, QPoint(4000, 1400), -1));
}

TEST(WindowPlacement, ScreenIndexHandlesNegativeOrigin)
{
    const std::vector<QRect> geoms = { QRect(0, 0, 1920, 1080), QRect(-1280, -200, 1280, 1024) };

    EXPECT_EQ(1, screenIndexAt(geoms, QPoint(-10, 500), 0));
    EXPECT_EQ(0, screenIndexAt(geoms, QPoint(0, 0), 1));
}

TEST(WindowPlacement, ScreenIndexReturnsFallbackOutsideEveryScreen)
{
    const std::vector<QRect> geoms = { QRect(0, 0, 1920, 1080), QRect(1920, 0, 1920, 1080) };

    EXPECT_EQ(7, screenIndexAt(geoms, QPoint(100, 2000), 7));
    EXPECT_EQ(-1, screenIndexAt(std::vector<QRect>(), QPoint(0, 0), -1));
}

TEST(WindowPlacement, CentresWithinSecondScreen)
{
    const QRect avail(1920, 0, 2560, 1400);

    EXPECT_EQ(QPoint(1920 + 780, 300), centredTopLeft(avail, QSize(1000, 800)));
}

TEST(WindowPlacement, CentresOnScreenAtNegativeOrigin)
{
    const QRect avail(-1280, -200, 1280, 1000);

    EXPECT_EQ(QPoint(-1280 + 340, -200 + 200), centredTopLeft(avail, QSize(600, 600)));
}

TEST(WindowPlacement, OversizedWindowClampsToTopLeftOfArea)
{
    const QRect avail(-1280, -200, 800, 600);

    EXPECT_EQ(QPoint(-1280, -200), centredTopLeft(avail, QSize(1000, 900)));
    EXPECT_EQ(QPoint(-1280, -200 + 100), centredTopLeft(avail, QSize(1000, 400)));
}
