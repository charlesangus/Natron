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

#ifndef Natron_Engine_AbortWatchdog_h
#define Natron_Engine_AbortWatchdog_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <string>

#include <QCoreApplication>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One app-wide watch over aborted renders, running on its own thread.
 *
 * AbortableRenderInfo::setAborted() hands the render to watch(), from any thread. While at least
 * one aborted render is watched, the watchdog thread scans them; it sleeps otherwise. Having its
 * own thread, it keeps working while the main thread is blocked, as it is during a command-line
 * render or a script's app.render(). A render still alive once the abort timeout has passed is
 * checked once: if some of its threads are still inside an action of a plug-in that is neither a
 * reader nor a writer, it is reported as stalled. Every render found stalled by one scan goes into
 * a single report: in background mode a log line written by the watchdog thread, otherwise a
 * question dialog, shown on the main thread, offering to kill the threads. While a dialog is open,
 * scans are deferred so dialogs never stack.
 **/
class AbortWatchdog {
    Q_DECLARE_TR_FUNCTIONS(AbortWatchdog)

public:
    typedef std::function<void(const std::string& report)> StallReportHandler;

    // Starts watching a render that was just aborted. Thread-safe. Does nothing after shutdown().
    static void watch(const AbortableRenderInfoWPtr& info);

    // Stops the watchdog thread and waits for it. Called once, while the application shuts down.
    static void shutdown();

    // How long an aborted render may keep threads inside an action before it is reported.
    static int getTimeoutMs();

    // Overrides the abort timeout; 0 restores the default.
    static void setTimeoutForTesting(int milliseconds);

    // While set, stall reports go to the handler, on the watchdog thread, instead of a dialog or the
    // log. Setting it waits for a report in flight.
    static void setStallReportHandlerForTesting(const StallReportHandler& handler);

    // Number of aborted renders still listed: not checked yet, being checked and reported, or
    // destroyed but not pruned yet.
    static std::size_t getNumWatchedForTesting();

    static bool isWatchingForTesting(const AbortableRenderInfoPtr& info);
};

NATRON_NAMESPACE_EXIT

#endif // Natron_Engine_AbortWatchdog_h
