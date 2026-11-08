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

#include "Global/FloatingPointExceptions.h"

#include <gtest/gtest.h>

namespace fp = boost_adaptbx::floating_point;

// A debug Natron arms these traps for the whole process and disarms them only around driver calls,
// so a guard that outlived its scope would silence the traps for Natron's own code.
#ifdef DEBUG
TEST(FpTrapGuard, DisarmsInScopeAndRearmsAfter)
{
    const int all = fp::exception_trapping::division_by_zero | fp::exception_trapping::invalid | fp::exception_trapping::overflow;
    fp::exception_trapping armed(all);
    ASSERT_TRUE(fp::is_division_by_zero_trapped());
    ASSERT_TRUE(fp::is_invalid_trapped());
    ASSERT_TRUE(fp::is_overflow_trapped());
    {
        const fp::FpTrapGuard noTraps;
        EXPECT_FALSE(fp::is_division_by_zero_trapped());
        EXPECT_FALSE(fp::is_invalid_trapped());
        EXPECT_FALSE(fp::is_overflow_trapped());
    }
    EXPECT_TRUE(fp::is_division_by_zero_trapped());
    EXPECT_TRUE(fp::is_invalid_trapped());
    EXPECT_TRUE(fp::is_overflow_trapped());
}
#else
TEST(FpTrapGuard, ReleaseGuardLeavesTrapsAlone)
{
    const bool before = fp::is_invalid_trapped();
    {
        const fp::FpTrapGuard noTraps;
        EXPECT_EQ(fp::is_invalid_trapped(), before);
    }
    EXPECT_EQ(fp::is_invalid_trapped(), before);
}
#endif
