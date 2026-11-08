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

#ifndef NATRON_GUI_SCENEDRAWTIMING_H
#define NATRON_GUI_SCENEDRAWTIMING_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <QtCore/QVariant>

NATRON_NAMESPACE_ENTER

/**
 * @brief Where the time of a draw state change goes, from the call that makes it to the image that
 * shows it, for the heavy scene bench. Exists only while NATRON_DRAW_STATE_TIMING is set to 1.
 *
 * Every frame that takes in a change gets one record: the main thread's start of the change, the
 * render thread's wait for its job, the filter's apply, the engine's render calls, the wait for the
 * GPU to finish them, the frame's arrival on the main thread and its presentation. Changes applied
 * before the same frame share its record. Steady-clock spans only, unless a trace is requested: then
 * the next record also runs TraceCollector and HdPerfLog from its apply to the end of its frame and
 * splits the time by Hydra and Storm scope. A traced record's own times include that overhead.
 *
 * The main thread stamps a change and its presentation; everything else runs on the render thread,
 * and the mutex is never held across Hydra or GL work.
 **/
class SceneDrawTiming {
public:
    using Clock = std::chrono::steady_clock;

    static bool enabledByEnvironment();

    SceneDrawTiming();
    ~SceneDrawTiming();

    SceneDrawTiming(const SceneDrawTiming&) = delete;
    SceneDrawTiming& operator=(const SceneDrawTiming&) = delete;

    // Main thread: the next frame that takes in a change is traced, or no longer.
    void setTraceNext(bool trace);

    // Render thread, before a non-empty apply: starts the trace if one is due and no record is open.
    void applyStarting();

    // Render thread: an apply sent changes firstPosted to lastPosted, the earliest of them started at
    // startedAt; firstPosted is past lastPosted for a reveal step alone. hiddenSent and shownSent are
    // what the filter sent during it.
    void applied(unsigned long long firstPosted,
                 unsigned long long lastPosted,
                 Clock::time_point startedAt,
                 Clock::time_point applyStart,
                 Clock::time_point applyEnd,
                 unsigned long long hiddenSent,
                 unsigned long long shownSent,
                 bool revealStep);

    // Render thread, before a frame's render calls: whether a record is waiting for this frame.
    bool frameStarting(Clock::time_point renderStart);

    // Render thread, after the render calls of a frame frameStarting() said yes to. gpuTailMs is
    // negative when the frame did not wait for the GPU.
    void frameEnded(Clock::time_point renderEnd,
                    int renderPasses,
                    double gpuTailMs,
                    bool onPaint);

    // Main thread: a background frame reached the main thread.
    void frameDone(Clock::time_point at);

    // Main thread: a paint showed every change up to absorbed. True if a record was completed.
    bool presented(unsigned long long absorbed,
                   Clock::time_point at);

    // Main thread: the completed records, oldest first, as maps for scripts.
    QVariantList records() const;

private:
    struct Record;

    void startTrace();
    void finishTrace(Record* record);

    mutable std::mutex _mutex;
    std::unique_ptr<Record> _open;
    std::deque<std::unique_ptr<Record>> _rendered;
    std::deque<std::unique_ptr<Record>> _completed;
    std::atomic<bool> _traceNext { false };
    bool _tracing = false;
    std::map<std::string, double> _countersAtStart;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWTIMING_H
