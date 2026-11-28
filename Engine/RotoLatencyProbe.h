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
#include <optional>
#include <string>

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

/**
 * @brief Opt-in per-stage timing of roto mask and RotoPaint renders. When NATRON_ROTO_RENDER_PROFILE
 * names a file, each mask render (RotoDrawableItem::renderMaskFromStroke) and each RotoPaint render
 * appends one JSON line:
 *
 *   mask:      {"kind": "mask", "node", "item", "type": "bezier"|"stroke", "time", "mipmap", "hit",
 *               "pixels", "total_ms", "other_ms", then "<stage>_ms" and "<stage>_calls" for
 *               cache_lookup, allocate, polygon, feather_setup, fill, feather and conversion}
 *   rotopaint: {"kind": "rotopaint", "node", "time", "mipmap", "items", "pixels", "total_ms",
 *               "other_ms", then "<stage>_ms" and "<stage>_calls" for setup and tree}
 *
 * Every stage of the record's kind is written, with 0 for a stage the render never entered. Stage
 * times are exclusive: a stage nested in another is not counted in the outer one, and other_ms is
 * the time of the record spent in no stage. A record opened inside another on the same thread
 * stands on its own, so the masks a RotoPaint's tree renders on its thread are part of its tree_ms.
 * Times are milliseconds on std::chrono::steady_clock.
 *
 * Stage timers look at one thread-local pointer, so without the variable a site costs one branch.
 **/
class RotoRenderProfile {
public:
    enum KindEnum {
        eKindMask = 0,
        eKindRotoPaint
    };

    enum StageEnum {
        eStageCacheLookup = 0,
        eStageAllocate,
        eStagePolygon,
        eStageFeatherSetup,
        eStageFill,
        eStageFeather,
        eStageConversion,
        eStageSetup,
        eStageTree,
        eStageCount
    };

    struct Record {
        KindEnum kind;
        std::string node;
        std::string item;
        std::string type;
        double time;
        unsigned int mipmap;
        bool hit;
        long long pixels;
        int items;
        double startMs;
        double lastMs;
        int current;
        double stageMs[eStageCount];
        int stageCalls[eStageCount];
        double otherMs;
    };

    /**
     * @brief Opens a record on this thread when the profile runs, and writes it when it goes out of
     * scope. Without the profile every setter does nothing.
     **/
    class Scope {
    public:
        Scope(KindEnum kind, double time, unsigned int mipmap);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        bool active() const
        {
            return _record != nullptr;
        }

        void setNames(const std::string& node, const std::string& item, const std::string& type);
        void setHit(bool hit);
        void setPixels(long long pixels);
        void setItems(int items);

    private:
        RotoRenderProfile* _profile;
        Record* _record;
        Record* _previous;
        std::optional<Record> _storage;
    };

    /**
     * @brief Charges the time until it goes out of scope to one stage of this thread's open record.
     **/
    class StageTimer {
    public:
        explicit StageTimer(StageEnum stage)
            : _record(t_current)
            , _previous(-1)
        {
            if (_record) {
                begin(stage);
            }
        }

        ~StageTimer()
        {
            if (_record) {
                end();
            }
        }

        StageTimer(const StageTimer&) = delete;
        StageTimer& operator=(const StageTimer&) = delete;

    private:
        void begin(StageEnum stage);
        void end();

        Record* _record;
        int _previous;
    };

    static RotoRenderProfile* instance()
    {
        return s_instance.load(std::memory_order_acquire);
    }

    /**
     * @brief Starts the profile when NATRON_ROTO_RENDER_PROFILE names a path; returns whether it runs.
     **/
    static bool startFromEnvironment();

    /**
     * @brief Returns false when the profile already runs or logPath cannot be opened for appending.
     **/
    static bool start(const QString& logPath);

    /**
     * @brief Closes the log and stops the profile. No render may be running: one could still hold
     * the instance.
     **/
    static void stop();

private:
    explicit RotoRenderProfile(std::FILE* log);
    ~RotoRenderProfile();

    void writeRecord(const Record& record);

    std::FILE* _log;
    std::mutex _mutex;

    static std::atomic<RotoRenderProfile*> s_instance;
    static thread_local Record* t_current;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoLatencyProbe_h
