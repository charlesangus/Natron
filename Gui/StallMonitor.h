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

#ifndef Gui_StallMonitor_h
#define Gui_StallMonitor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include <QString>
#include <QTimer>

NATRON_NAMESPACE_ENTER

/**
 * @brief Logs main-thread event-loop stalls as JSON lines, one {"start_ms", "duration_ms"} object
 * per stall. A main-thread timer stamps a heartbeat; a watcher thread opens a stall when the
 * heartbeat is older than the threshold and writes it once the heartbeat advances again.
 * Times are milliseconds on std::chrono::steady_clock (CLOCK_MONOTONIC on Linux), the clock
 * behind Python's time.monotonic(), so a script can match stalls to its own timestamps.
 * Must be created and destroyed on the main thread.
 **/
class StallMonitor {
public:
    static const int kDefaultThresholdMs = 50;
    static const int kHeartbeatIntervalMs = 10;

    /**
     * @brief Returns a running monitor when NATRON_STALL_LOG names a path (NATRON_STALL_THRESHOLD_MS
     * overrides the threshold), or null when it is unset or the log cannot be opened.
     **/
    static std::unique_ptr<StallMonitor> createFromEnvironment();

    /**
     * @brief Returns null when logPath cannot be opened for appending.
     **/
    static std::unique_ptr<StallMonitor> create(const QString& logPath, int thresholdMs);

    ~StallMonitor();

    /**
     * @brief Number of live monitors, each owning one watcher thread.
     **/
    static int activeCount();

    static double nowMs();

private:
    StallMonitor(std::FILE* log, int thresholdMs);

    void watch();
    void writeStall(double startMs, double durationMs);

    std::FILE* _log;
    const double _thresholdMs;
    QTimer _heartbeatTimer;
    std::atomic<double> _lastBeatMs;
    std::mutex _stopMutex;
    std::condition_variable _stopCond;
    bool _stop;
    std::thread _watcher;

    static std::atomic<int> s_active;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_StallMonitor_h
