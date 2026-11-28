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

#ifndef Engine_Nodes_Scene_Utility_UtilityProfile_h
#define Engine_Nodes_Scene_Utility_UtilityProfile_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief Wall-clock stage timings of the Utility renderer's frames, written as one JSON line a frame to the path in
 * NATRON_UTILITY_PROFILE. With the variable unset every scope costs one test of a flag read once.
 *
 * Only the thread that opened a UtilityProfileFrame is timed. Stages nest and each one's time excludes the stages
 * opened inside it, so the stages of a frame add up to its time less what no stage covers. GPU work shows up where
 * the host waits for it: under the submit stages, not under the stage that recorded it. The device's own time for
 * the submissions, where it writes timestamps, goes to those stages' gpu_ms.
 **/
namespace utilityProfile {
enum class Stage : std::size_t {
    ShutterSnapshot,
    TracerUpdate,
    ShadeTables,
    Uploads,
    FilmBegin,
    Raster,
    RasterSubmitWait,
    RayGenIntersect,
    Resolve,
    Shade,
    Bounces,
    FilmAccumulate,
    Readback,
    GpuSubmitWait,
    WriteTile,
    PublishPass,
    // Outside a frame's draw, on the threads Hydra and its callers run.
    Clear,
    RenderCall,
    MapAov,
    ColorDownload,
    PickReadback,
    CpuFrame,
    CpuWindow,
    DecodeIds,
    SceneStageSync,
    Count
};

inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);

const char* stageName(Stage stage);

bool enabledFromEnvironment();

inline bool
enabled()
{
    static const bool on = enabledFromEnvironment();

    return on;
}

class Scope;

struct FrameState {
    using Clock = std::chrono::steady_clock;

    Clock::time_point start;
    std::array<double, kStageCount> ms {};
    std::array<double, kStageCount> inPassMs {};
    std::array<std::uint64_t, kStageCount> calls {};
    // Device time of the submissions waited for under each stage, from timestamps the device wrote around them.
    std::array<double, kStageCount> gpuMs {};
    std::vector<double> passMs;
    bool inPass = false;
    Clock::time_point passStart;
    Scope* open = nullptr;
};

FrameState* currentFrame();

// Adds ms of device time to stage of the frame open on this thread; nothing outside one.
void addGpuTime(Stage stage,
                double ms);

// The thread's state for scopes outside a frame's draw, which its outermost scope adds to the process-wide one.
FrameState* looseFrame();

// Adds a thread's loose state to the process-wide one and zeroes it.
void flushLoose(FrameState* loose);

class Scope {
public:
    explicit Scope(Stage stage)
        : _frame(enabled() ? currentFrame() : nullptr)
        , _stage(stage)
    {
        if (enabled() && !_frame) {
            _frame = looseFrame();
            _loose = true;
        }
        if (_frame) {
            _parent = _frame->open;
            _frame->open = this;
            _start = FrameState::Clock::now();
        }
    }

    ~Scope()
    {
        if (_frame) {
            close();
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    void close();

    FrameState* _frame;
    Stage _stage;
    bool _loose = false;
    Scope* _parent = nullptr;
    FrameState::Clock::time_point _start;
    double _childMs = 0.;
};

class Pass {
public:
    Pass();
    ~Pass();

    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;

private:
    FrameState* _frame;
};
} // namespace utilityProfile

/**
 * @brief Times one frame's draw on this thread, from construction until it is destroyed, and then appends its
 * record. Frames opened while one is already open on the thread are not timed.
 **/
class UtilityProfileFrame {
public:
    UtilityProfileFrame(const std::string& mode,
                        const std::string& device,
                        int width,
                        int height,
                        int passes);
    ~UtilityProfileFrame();

    void setFinished(bool finished) { _finished = finished; }

    UtilityProfileFrame(const UtilityProfileFrame&) = delete;
    UtilityProfileFrame& operator=(const UtilityProfileFrame&) = delete;

private:
    void write();

    bool _owner = false;
    bool _finished = false;
    std::string _mode;
    std::string _device;
    int _width;
    int _height;
    int _passes;
    utilityProfile::FrameState _state;
};

/**
 * @brief Times one HydraRender call, from construction until it is destroyed, and then appends a hydra_profile
 * record with the stages timed outside any frame's draw meanwhile, on any thread. Calls nested on the thread are
 * not timed; overlapping calls on other threads share those stages.
 **/
namespace utilityProfile {
/**
 * @brief Writes a "viewer_frame" record as a viewer shows a frame: from a root left on the device (deviceColor) or
 * from host pixels, through the viewer's hand-off rung, with fallback when a device root was drawn from a download
 * instead, and refining when the frame is a pass of a render still refining, which the viewer asks for again. It
 * carries the steady clock in mono_ms and the process's device download and pick readback counts, so a reader can
 * tell what each frame cost and line it up with the hydra_profile records. Does nothing unless enabled().
 **/
void writeViewerFrame(const std::string& viewer,
                      double time,
                      bool deviceColor,
                      bool fallback,
                      const std::string& rung,
                      bool refining);
} // namespace utilityProfile

namespace utilityProfile {
// Writes a "stamp" record: name at the steady clock's now, in mono_ms. Does nothing unless enabled().
void writeStamp(const char* name);

/**
 * @brief Writes a "span" record as it is destroyed: name, with the steady clock at construction in start_mono_ms
 * and at destruction in mono_ms, so that a reader can place the step between an edit and the frame it brings.
 * Costs one flag test unless enabled().
 **/
class Span {
public:
    explicit Span(const char* name);

    // With the "node" field nodeName() gives, called only when enabled().
    template <typename NodeNameFunc>
    Span(const char* name,
         NodeNameFunc&& nodeName)
        : Span(name)
    {
        if (_on) {
            _node = nodeName();
        }
    }

    ~Span();

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

private:
    const char* _name;
    bool _on;
    std::string _node;
    std::chrono::steady_clock::time_point _start;
};

/**
 * @brief Process-wide counts of what the caches around a render did, carried as running totals in the "counters"
 * object of the utility_profile, hydra_profile and viewer_frame records, so that a reader takes the difference
 * between two records for what happened between them.
 **/
enum class Counter : std::size_t {
    SettingsSummaryRead,
    SettingsSummaryReuse,
    SettingsSummaryJoin,
    SettingsSummaryEvaluation,
    RenderSetupRead,
    RenderSetupReuse,
    RenderSetupCameraRead,
    ShadeTablesUploaded,
    ShadeTableSetsKept,
    RenderCall,
    SceneEvaluation,
    Count
};

inline constexpr std::size_t kCounterCount = static_cast<std::size_t>(Counter::Count);

const char* counterName(Counter counter);

// Adds n to counter. Does nothing unless enabled().
void count(Counter counter,
           std::uint64_t n = 1);
} // namespace utilityProfile

class UtilityProfileHydraCall {
public:
    UtilityProfileHydraCall(const std::string& kind,
                            const std::string& renderer,
                            int width,
                            int height);
    ~UtilityProfileHydraCall();

    UtilityProfileHydraCall(const UtilityProfileHydraCall&) = delete;
    UtilityProfileHydraCall& operator=(const UtilityProfileHydraCall&) = delete;

private:
    void write();

    bool _owner = false;
    std::string _kind;
    std::string _renderer;
    int _width;
    int _height;
    std::chrono::steady_clock::time_point _start;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityProfile_h
