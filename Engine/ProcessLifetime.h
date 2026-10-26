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

#ifndef Natron_Engine_ProcessLifetime_h
#define Natron_Engine_ProcessLifetime_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Turns termination signals into a call on an ordinary thread.
 *
 * Almost nothing is safe inside a signal handler: tearing an app down there runs Qt, Python
 * and render code on top of whatever the interrupted thread was doing. The handler installed
 * here only writes the signal number to a pipe. A dedicated thread reads it and runs the
 * callback, which may lock, log and post to the main thread.
 *
 * The first signal reaches the callback. A second one of the same set terminates the process
 * with the default action, so a shutdown that stalls can still be ended with another Ctrl-C
 * or kill.
 */
class ShutdownSignalDispatcher {
public:
    /**
     * @brief Installs the handler for every signal in @p signalIds and starts the reader thread.
     * Returns false when already installed or when the pipe or a handler can't be set up.
     */
    static bool install(const std::vector<int>& signalIds,
                        const std::function<void(int)>& onSignal);

    /**
     * @brief Restores the handlers that were in place before install(), then stops and joins
     * the reader thread. Once it returns the callback no longer runs. Safe when not installed.
     */
    static void uninstall();

    static bool isInstalled();

    /**
     * @brief Drops the state a fork() child inherits, without touching the parent's thread, so
     * the child can install a dispatcher of its own. Only for a single-threaded forked child.
     */
    static void resetInForkedChildForTesting();
};

/**
 * @brief Marks every open descriptor from @p firstFd up as close-on-exec.
 *
 * A launcher can hand Natron descriptors it means only for Natron itself. The AppImage runtime
 * leaves the read end of the pipe that keeps its mount alive open across exec, for example. A
 * browser, a background render or any other program Natron spawns would inherit them and keep
 * the launcher alive after Natron exits. The descriptors stay open in Natron.
 *
 * Returns the number of descriptors it changed.
 */
int markInheritedDescriptorsCloseOnExec(int firstFd = 3);

NATRON_NAMESPACE_EXIT

#endif // Natron_Engine_ProcessLifetime_h
