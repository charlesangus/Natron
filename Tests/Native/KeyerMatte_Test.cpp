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

#include "KeyerMatteReference.h"

NATRON_NAMESPACE_USING

namespace {

void
expectMatte(float expected,
            double k,
            double a,
            double b,
            double c,
            double d)
{
    const float range[4] = { (float)a, (float)b, (float)c, (float)d };

    EXPECT_FLOAT_EQ(expected, trapezoidMatte(k, a, b, c, d)) << "reference at key " << k;
    EXPECT_FLOAT_EQ(expected, trapezoidMatteFloat((float)k, range)) << "render at key " << k;
}

} // namespace

TEST(KeyerMatte, RampsAndPlateau)
{
    expectMatte(0.f, -1., 0., 0.2, 0.6, 1.);
    expectMatte(0.5f, 0.1, 0., 0.2, 0.6, 1.);
    expectMatte(1.f, 0.4, 0., 0.2, 0.6, 1.);
    expectMatte(0.5f, 0.8, 0., 0.2, 0.6, 1.);
    expectMatte(0.25f, 0.9, 0., 0.2, 0.6, 1.);
    expectMatte(0.f, 2., 0., 0.2, 0.6, 1.);
}

TEST(KeyerMatte, EachBreakpoint)
{
    expectMatte(0.f, 0., 0., 0.1, 0.9, 1.);
    expectMatte(1.f, 0.1, 0., 0.1, 0.9, 1.);
    expectMatte(1.f, 0.9, 0., 0.1, 0.9, 1.);
    expectMatte(0.f, 1., 0., 0.1, 0.9, 1.);
}

TEST(KeyerMatte, HardEdges)
{
    // a == b: a step up at a; c == d: a step down at d.
    expectMatte(0.f, 0.2999, 0.3, 0.3, 0.7, 0.7);
    expectMatte(1.f, 0.3, 0.3, 0.3, 0.7, 0.7);
    expectMatte(1.f, 0.7, 0.3, 0.3, 0.7, 0.7);
    expectMatte(0.f, 0.7001, 0.3, 0.3, 0.7, 0.7);
}

TEST(KeyerMatte, OneSidedRampsOutsideTheUnitRange)
{
    // Scene-linear keys: no clamp to [0, 1] on either the key or the breakpoints.
    expectMatte(1.f, 5., 0., 1., 10., 20.);
    expectMatte(0.5f, 15., 0., 1., 10., 20.);
    expectMatte(0.5f, -1.5, -2., -1., 0., 0.);
}

TEST(KeyerMatte, AllEqualTrapezoid)
{
    expectMatte(1.f, 0.5, 0.5, 0.5, 0.5, 0.5);
    expectMatte(0.f, 0.4999, 0.5, 0.5, 0.5, 0.5);
    expectMatte(0.f, 0.5001, 0.5, 0.5, 0.5, 0.5);
}
