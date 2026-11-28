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

#include <cstdlib>

#include <sys/syscall.h>
#include <unistd.h>

#include "TestWatchdog.h"

// These definitions replace glibc's for the whole process: a call from this executable binds to
// them at link time, and one from a shared library binds through its PLT to the first definition in
// global scope, which is the executable's as long as the link exports them
// (--export-dynamic-symbol in CMakeLists.txt). glibc's own exit() and abort() call a hidden
// internal _exit that no definition here can replace; exit() is reported by the on_exit handler.

namespace {

[[noreturn]] void
endProcess(int status)
{
    for (;;) {
        syscall(SYS_exit_group, status);
    }
}

} // namespace

extern "C" void
_exit(int status)
{
    TestWatchdog::reportDirectExit(status);
    endProcess(status);
}

extern "C" void
_Exit(int status) noexcept
{
    TestWatchdog::reportDirectExit(status);
    endProcess(status);
}
