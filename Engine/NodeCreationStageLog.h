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

#ifndef Engine_NodeCreationStageLog_h
#define Engine_NodeCreationStageLog_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>

NATRON_NAMESPACE_ENTER

/**
 * @brief Opt-in stage timing of interactive node creation. When NATRON_NODE_CREATION_STAGE_LOG names
 * a file, each Scope appends one JSON line {"stage", "start_ms", "end_ms"} when it ends.
 *
 * Times are milliseconds on std::chrono::steady_clock, the clock of StallMonitor and of Python's
 * time.monotonic(). Scopes nest: a stage's span includes the stages inside it.
 *
 * A session without the variable pays one relaxed load per Scope.
 **/
class NodeCreationStageLog {
public:
    static bool enabled()
    {
        return s_enabled.load(std::memory_order_relaxed);
    }

    /**
     * @brief Starts logging when NATRON_NODE_CREATION_STAGE_LOG names a path; returns whether it runs.
     **/
    static bool startFromEnvironment();

    static double nowMs();

    class Scope {
    public:
        explicit Scope(const char* stage)
            : _stage(enabled() ? stage : nullptr)
            , _startMs(_stage ? nowMs() : 0.)
        {
        }

        ~Scope()
        {
            if (_stage) {
                write(_stage, _startMs, nowMs());
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        const char* _stage;
        double _startMs;
    };

private:
    static void write(const char* stage, double startMs, double endMs);

    static std::atomic<bool> s_enabled;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_NodeCreationStageLog_h
