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

#ifndef Engine_RotoLatencyProbe_h
#define Engine_RotoLatencyProbe_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>

#include <QString>

NATRON_NAMESPACE_ENTER

/**
 * @brief Opt-in pen-to-pixel latency log for roto painting. When NATRON_ROTO_LATENCY_LOG names a
 * file, each stroke point gets one JSON line once its pixels have been painted in a viewer:
 * {"seq", "event_ms", "append_ms", "request_ms", "render_start_ms", "render_end_ms", "upload_ms",
 * "paint_ms"}, with null for a stage the point never went through.
 *
 * Times are milliseconds on std::chrono::steady_clock, the clock of StallMonitor and of Python's
 * time.monotonic(). A render covers every point appended before it was requested: the mark that
 * noteRenderRequested() returns travels with the render to its start and end.
 *
 * Callers test instance() first, so a session without the variable pays one branch per site.
 **/
class RotoLatencyProbe {
public:
    static RotoLatencyProbe* instance()
    {
        return s_instance.load(std::memory_order_acquire);
    }

    /**
     * @brief Starts the probe when NATRON_ROTO_LATENCY_LOG names a path; returns whether it runs.
     **/
    static bool startFromEnvironment();

    /**
     * @brief Returns false when the probe already runs or logPath cannot be opened for appending.
     **/
    static bool start(const QString& logPath);

    /**
     * @brief Writes the points still waiting for a paint, closes the log and stops the probe.
     * No render may be running: a render thread could still hold the instance.
     **/
    static void stop();

    static double nowMs();

    // Main thread: a pen or mouse event reached the viewer.
    void noteInputEvent();

    // Main thread: the stroke took a new point.
    void notePointAppended();

    // Returns the mark to hand to noteRenderStarted() and noteRenderFinished().
    std::uint64_t noteRenderRequested();

    void noteRenderStarted(std::uint64_t mark);

    void noteRenderFinished(std::uint64_t mark);

    // Main thread: a rendered frame reached the viewer's texture.
    void noteTextureUploaded();

    // Main thread: the viewer finished painting.
    void noteFramePainted();

private:
    enum StageEnum {
        eStageEvent = 0,
        eStageAppend,
        eStageRequest,
        eStageRenderStart,
        eStageRenderEnd,
        eStageUpload,
        eStagePaint,
        eStageCount
    };

    struct PointRecord {
        std::uint64_t seq;
        double stamps[eStageCount];
    };

    explicit RotoLatencyProbe(std::FILE* log);
    ~RotoLatencyProbe();

    void writeRecord(const PointRecord& record);

    std::FILE* _log;
    std::mutex _mutex;
    std::deque<PointRecord> _pending;
    std::uint64_t _lastSeq;
    double _pendingEventMs;

    static std::atomic<RotoLatencyProbe*> s_instance;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoLatencyProbe_h
