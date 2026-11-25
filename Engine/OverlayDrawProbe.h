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

#ifndef Engine_OverlayDrawProbe_h
#define Engine_OverlayDrawProbe_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdio>

#include <QString>

NATRON_NAMESPACE_ENTER

/**
 * @brief Times each viewer overlay pass for the GUI bench. When NATRON_OVERLAY_DRAW_LOG names a
 * path, every pass appends one JSON line: start_ms and end_ms on std::chrono::steady_clock, and
 * duration_ms between them. Main thread only.
 **/
class OverlayDrawProbe {
public:
    static OverlayDrawProbe* instance()
    {
        return s_instance;
    }

    /**
     * @brief Starts the probe when NATRON_OVERLAY_DRAW_LOG names a path; returns whether it runs.
     **/
    static bool startFromEnvironment();

    /**
     * @brief Returns false when the probe already runs or logPath cannot be opened for appending.
     **/
    static bool start(const QString& logPath);

    static void stop();

    static double nowMs();

    void notePass(double startMs, double endMs);

private:
    explicit OverlayDrawProbe(std::FILE* log);

    ~OverlayDrawProbe();

    static OverlayDrawProbe* s_instance;

    std::FILE* _log;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayDrawProbe_h
