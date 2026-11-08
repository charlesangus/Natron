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

#include <thread>

#include "Global/FloatingPointExceptions.h"

#include <gtest/gtest.h>

#include "Engine/OSGLContext.h"

NATRON_NAMESPACE_USING

namespace fp = boost_adaptbx::floating_point;

// A debug Natron arms these traps for the whole process and, on a hardware renderer, disarms them only
// around driver calls, so a guard that outlived its scope would silence the traps for Natron's own code.
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

namespace {

struct TrapState {
    bool divisionByZero = false;
    bool invalid = false;
    bool overflow = false;
};

TrapState
trapState()
{
    return TrapState { fp::is_division_by_zero_trapped(), fp::is_invalid_trapped(), fp::is_overflow_trapped() };
}

// Runs on a thread of its own, as the disarm lasts for the rest of the calling thread's life.
bool
disarmOnArmedThread(const std::string& rendererName, TrapState* after)
{
    bool software = false;
    std::thread t([&] {
        fp::exception_trapping armed(fp::exception_trapping::division_by_zero | fp::exception_trapping::invalid | fp::exception_trapping::overflow);
        software = OSGLContext::disarmFpTrapsForSoftwareRenderer(rendererName);
        *after = trapState();
    });
    t.join();

    return software;
}
}

TEST(FpTrapGuard, SoftwareRendererDisarmsTheCallingThreadOnly)
{
    const TrapState before = trapState();
    TrapState after;
    EXPECT_TRUE(disarmOnArmedThread("llvmpipe (LLVM 21.1.0, 256 bits)", &after));
#ifdef DEBUG
    EXPECT_FALSE(after.divisionByZero);
    EXPECT_FALSE(after.invalid);
    EXPECT_FALSE(after.overflow);
#endif
    const TrapState here = trapState();
    EXPECT_EQ(here.divisionByZero, before.divisionByZero);
    EXPECT_EQ(here.invalid, before.invalid);
    EXPECT_EQ(here.overflow, before.overflow);
}

TEST(FpTrapGuard, HardwareRendererLeavesTrapsArmed)
{
    TrapState after;
    EXPECT_FALSE(disarmOnArmedThread("AMD Radeon RX 6900 XT (radeonsi, navi21, LLVM 17.0.6, DRM 3.54)", &after));
    EXPECT_TRUE(after.divisionByZero);
    EXPECT_TRUE(after.invalid);
    EXPECT_TRUE(after.overflow);
}
