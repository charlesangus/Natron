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

#include <cfenv>
#include <thread>

#include "Global/FloatingPointExceptions.h"

#include <gtest/gtest.h>

#include "Engine/OSGLContext.h"

NATRON_NAMESPACE_USING

namespace fp = boost_adaptbx::floating_point;

namespace {

const int kAllTraps = fp::exception_trapping::division_by_zero | fp::exception_trapping::invalid | fp::exception_trapping::overflow;

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

#ifdef DEBUG
bool
anyTrapped(const TrapState& s)
{
    return s.divisionByZero || s.invalid || s.overflow;
}
#endif

bool
allTrapped(const TrapState& s)
{
    return s.divisionByZero && s.invalid && s.overflow;
}

// The switch is process-wide and the GPU tests in this binary may have set it.
class SoftwareRendererSwitch {
public:
    explicit SoftwareRendererSwitch(bool present)
        : _previous(fp::software_renderer_present().exchange(present))
    {
    }

    ~SoftwareRendererSwitch()
    {
        fp::software_renderer_present().store(_previous);
    }

    SoftwareRendererSwitch(const SoftwareRendererSwitch&) = delete;
    SoftwareRendererSwitch& operator=(const SoftwareRendererSwitch&) = delete;

private:
    bool _previous;
};

// Arms a thread of its own and returns its traps after it notes rendererName.
bool
noteOnArmedThread(const std::string& rendererName,
                  TrapState* after)
{
    bool software = false;
    std::thread t([&] {
        fp::exception_trapping armed(kAllTraps);
        software = OSGLContext::noteSoftwareRenderer(rendererName);
        *after = trapState();
    });
    t.join();

    return software;
}
}

// A debug Natron arms these traps for the whole process and, on a hardware renderer, disarms them only
// around driver calls, so a guard that outlived its scope would silence the traps for Natron's own code.
#ifdef DEBUG
TEST(FpTrapGuard, DisarmsInScopeAndRearmsAfter)
{
    const SoftwareRendererSwitch hardware(false);
    TrapState inside;
    TrapState after;
    std::thread t([&] {
        fp::exception_trapping armed(kAllTraps);
        {
            const fp::FpTrapGuard noTraps;
            inside = trapState();
        }
        after = trapState();
    });
    t.join();
    EXPECT_FALSE(anyTrapped(inside));
    EXPECT_TRUE(allTrapped(after));
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

// A flag a driver raised while the traps were off must not survive their re-arming, where the next
// float instruction would deliver it as SIGFPE.
TEST(FpTrapGuard, RearmingDropsFlagsRaisedWhileDisarmed)
{
    const SoftwareRendererSwitch hardware(false);
    TrapState armedAfter;
    int pendingAfter = -1;
    std::thread t([&] {
        fp::exception_trapping armed(kAllTraps);
        {
            fp::exception_trapping off(fp::exception_trapping::dont_trap);
            std::feraiseexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
        }
        pendingAfter = std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
        armedAfter = trapState();
    });
    t.join();
    EXPECT_EQ(pendingAfter, 0);
    EXPECT_TRUE(allTrapped(armedAfter));
}

TEST(FpTrapGuard, HardwareRendererLeavesTrapsArmed)
{
    const SoftwareRendererSwitch hardware(false);
    TrapState after;
    EXPECT_FALSE(noteOnArmedThread("AMD Radeon RX 6900 XT (radeonsi, navi21, LLVM 17.0.6, DRM 3.54)", &after));
    EXPECT_TRUE(allTrapped(after));
    EXPECT_FALSE(fp::software_renderer_present().load());
}

#ifdef DEBUG
TEST(FpTrapGuard, SoftwareRendererKeepsEveryThreadDisarmed)
{
    const SoftwareRendererSwitch hardware(false);
    TrapState noted;
    TrapState inheritedByChild;
    TrapState armedByChild;
    TrapState afterGuard;
    TrapState armedByOtherThread;
    bool software = false;
    std::thread t([&] {
        fp::exception_trapping armed(kAllTraps);
        software = OSGLContext::noteSoftwareRenderer("llvmpipe (LLVM 21.1.0, 256 bits)");
        noted = trapState();
        std::thread child([&] {
            inheritedByChild = trapState();
            fp::exception_trapping childArmed(kAllTraps);
            armedByChild = trapState();
        });
        child.join();
        {
            const fp::FpTrapGuard noTraps;
        }
        afterGuard = trapState();
    });
    t.join();
    std::thread other([&] {
        fp::exception_trapping armed(kAllTraps);
        armedByOtherThread = trapState();
    });
    other.join();

    EXPECT_TRUE(software);
    EXPECT_TRUE(fp::software_renderer_present().load());
    EXPECT_FALSE(anyTrapped(noted));
    EXPECT_FALSE(anyTrapped(inheritedByChild));
    EXPECT_FALSE(anyTrapped(armedByChild));
    EXPECT_FALSE(anyTrapped(afterGuard));
    EXPECT_FALSE(anyTrapped(armedByOtherThread));
}

TEST(FpTrapGuard, ThreadArmedBeforeTheSwitchFollowsIt)
{
    const SoftwareRendererSwitch hardware(false);
    TrapState before;
    TrapState after;
    std::thread t([&] {
        fp::exception_trapping armed(kAllTraps);
        before = trapState();
        fp::software_renderer_present().store(true);
        fp::follow_software_renderer();
        after = trapState();
    });
    t.join();
    EXPECT_TRUE(allTrapped(before));
    EXPECT_FALSE(anyTrapped(after));
}
#endif
