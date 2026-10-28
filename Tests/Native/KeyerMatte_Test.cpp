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

#include "Global/Macros.h"

#include <gtest/gtest.h>

#include "Engine/Nodes/Keyer/KeyerMatte.h"

NATRON_NAMESPACE_USING

TEST(KeyerMatte, RampsAndPlateau)
{
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(-1., 0., 0.2, 0.6, 1.));
    EXPECT_FLOAT_EQ(0.5f, trapezoidMatte(0.1, 0., 0.2, 0.6, 1.));
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(0.4, 0., 0.2, 0.6, 1.));
    EXPECT_FLOAT_EQ(0.5f, trapezoidMatte(0.8, 0., 0.2, 0.6, 1.));
    EXPECT_FLOAT_EQ(0.25f, trapezoidMatte(0.9, 0., 0.2, 0.6, 1.));
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(2., 0., 0.2, 0.6, 1.));
}

TEST(KeyerMatte, EachBreakpoint)
{
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(0., 0., 0.1, 0.9, 1.));
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(0.1, 0., 0.1, 0.9, 1.));
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(0.9, 0., 0.1, 0.9, 1.));
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(1., 0., 0.1, 0.9, 1.));
}

TEST(KeyerMatte, HardEdges)
{
    // a == b: a step up at a; c == d: a step down at d.
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(0.2999, 0.3, 0.3, 0.7, 0.7));
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(0.3, 0.3, 0.3, 0.7, 0.7));
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(0.7, 0.3, 0.3, 0.7, 0.7));
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(0.7001, 0.3, 0.3, 0.7, 0.7));
}

TEST(KeyerMatte, OneSidedRampsOutsideTheUnitRange)
{
    // Scene-linear keys: no clamp to [0, 1] on either the key or the breakpoints.
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(5., 0., 1., 10., 20.));
    EXPECT_FLOAT_EQ(0.5f, trapezoidMatte(15., 0., 1., 10., 20.));
    EXPECT_FLOAT_EQ(0.5f, trapezoidMatte(-1.5, -2., -1., 0., 0.));
}

TEST(KeyerMatte, AllEqualTrapezoid)
{
    EXPECT_FLOAT_EQ(1.f, trapezoidMatte(0.5, 0.5, 0.5, 0.5, 0.5));
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(0.4999, 0.5, 0.5, 0.5, 0.5));
    EXPECT_FLOAT_EQ(0.f, trapezoidMatte(0.5001, 0.5, 0.5, 0.5, 0.5));
}

TEST(KeyerMatte, SortedTrapezoid)
{
    const KeyerTrapezoid ordered = sortedTrapezoid(0., 0.1, 0.9, 1.);
    EXPECT_EQ(0., ordered.a);
    EXPECT_EQ(0.1, ordered.b);
    EXPECT_EQ(0.9, ordered.c);
    EXPECT_EQ(1., ordered.d);

    const KeyerTrapezoid raised = sortedTrapezoid(0.5, 0.2, 0.1, 0.8);
    EXPECT_EQ(0.5, raised.a);
    EXPECT_EQ(0.5, raised.b);
    EXPECT_EQ(0.5, raised.c);
    EXPECT_EQ(0.8, raised.d);
}

TEST(KeyerMatte, PushNeighbours)
{
    // B dragged below A pushes A down.
    double below[4] = { 0.2, 0.1, 0.9, 1. };
    pushTrapezoidNeighbours(below, 1);
    EXPECT_EQ(0.1, below[0]);
    EXPECT_EQ(0.1, below[1]);
    EXPECT_EQ(0.9, below[2]);
    EXPECT_EQ(1., below[3]);

    // B dragged past C and D pushes both up.
    double above[4] = { 0., 1.5, 0.9, 1. };
    pushTrapezoidNeighbours(above, 1);
    EXPECT_EQ(0., above[0]);
    EXPECT_EQ(1.5, above[1]);
    EXPECT_EQ(1.5, above[2]);
    EXPECT_EQ(1.5, above[3]);

    // A dragged above everything pushes the whole trapezoid up.
    double first[4] = { 2., 0.1, 0.9, 1. };
    pushTrapezoidNeighbours(first, 0);
    EXPECT_EQ(2., first[1]);
    EXPECT_EQ(2., first[2]);
    EXPECT_EQ(2., first[3]);

    // D dragged below everything pushes the whole trapezoid down.
    double last[4] = { 0., 0.1, 0.9, -1. };
    pushTrapezoidNeighbours(last, 3);
    EXPECT_EQ(-1., last[0]);
    EXPECT_EQ(-1., last[1]);
    EXPECT_EQ(-1., last[2]);
}
