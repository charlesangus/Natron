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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "UtilityProfile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>

#include <unistd.h>

NATRON_NAMESPACE_ENTER

namespace utilityProfile {
namespace {
    thread_local FrameState* tFrame = nullptr;

    const char*
    profilePath()
    {
        static const std::string path = []() {
            const char* env = std::getenv("NATRON_UTILITY_PROFILE");

            return std::string(env ? env : "");
        }();

        return path.c_str();
    }

    double
    millisecondsBetween(FrameState::Clock::time_point from,
                        FrameState::Clock::time_point to)
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    std::string
    number(double value)
    {
        char text[64];

        std::snprintf(text, sizeof(text), "%.4f", value);

        return text;
    }

    std::string
    jsonString(const std::string& text)
    {
        std::string out = "\"";

        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];

                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out += escaped;
            } else {
                out += c;
            }
        }

        return out + "\"";
    }
} // anon namespace

const char*
stageName(Stage stage)
{
    switch (stage) {
    case Stage::ShutterSnapshot:
        return "shutter_snapshot";
    case Stage::TracerUpdate:
        return "tracer_update";
    case Stage::ShadeTables:
        return "shade_tables";
    case Stage::Uploads:
        return "uploads";
    case Stage::FilmBegin:
        return "film_begin";
    case Stage::Raster:
        return "raster";
    case Stage::RasterSubmitWait:
        return "raster_submit_wait";
    case Stage::RayGenIntersect:
        return "raygen_intersect";
    case Stage::Resolve:
        return "resolve";
    case Stage::Shade:
        return "shade";
    case Stage::Bounces:
        return "bounces";
    case Stage::FilmAccumulate:
        return "film_accumulate";
    case Stage::Readback:
        return "readback";
    case Stage::GpuSubmitWait:
        return "gpu_submit_wait";
    case Stage::WriteTile:
        return "write_tile";
    case Stage::PublishPass:
        return "publish_pass";
    case Stage::Count:
        break;
    }

    return "unknown";
}

bool
enabledFromEnvironment()
{
    return profilePath()[0] != '\0';
}

FrameState*
currentFrame()
{
    return tFrame;
}

void
Scope::close()
{
    const double elapsed = millisecondsBetween(_start, FrameState::Clock::now());
    const double own = std::max(0., elapsed - _childMs);
    const std::size_t index = static_cast<std::size_t>(_stage);

    _frame->ms[index] += own;
    if (_frame->inPass) {
        _frame->inPassMs[index] += own;
    }
    ++_frame->calls[index];
    _frame->open = _parent;
    if (_parent) {
        _parent->_childMs += elapsed;
    }
}

Pass::Pass()
    : _frame(enabled() ? currentFrame() : nullptr)
{
    if (_frame && _frame->inPass) {
        _frame = nullptr;
    }
    if (_frame) {
        _frame->inPass = true;
        _frame->passStart = FrameState::Clock::now();
    }
}

Pass::~Pass()
{
    if (_frame) {
        _frame->passMs.push_back(millisecondsBetween(_frame->passStart, FrameState::Clock::now()));
        _frame->inPass = false;
    }
}
} // namespace utilityProfile

UtilityProfileFrame::UtilityProfileFrame(const std::string& mode,
                                         const std::string& device,
                                         int width,
                                         int height,
                                         int passes)
    : _width(width)
    , _height(height)
    , _passes(passes)
{
    if (!utilityProfile::enabled() || utilityProfile::tFrame) {
        return;
    }
    _owner = true;
    _mode = mode;
    _device = device;
    _state.start = utilityProfile::FrameState::Clock::now();
    utilityProfile::tFrame = &_state;
}

UtilityProfileFrame::~UtilityProfileFrame()
{
    if (!_owner) {
        return;
    }
    utilityProfile::tFrame = nullptr;
    try {
        write();
    } catch (...) {
    }
}

void
UtilityProfileFrame::write()
{
    using namespace utilityProfile;

    static std::atomic<std::uint64_t> sequence { 0 };
    static std::mutex fileMutex;

    const double total = std::chrono::duration<double, std::milli>(FrameState::Clock::now() - _state.start).count();
    const std::vector<double>& passes = _state.passMs;
    const double passSum = std::accumulate(passes.begin(), passes.end(), 0.);
    const double staged = std::accumulate(_state.ms.begin(), _state.ms.end(), 0.);
    const double drawn = passes.empty() ? 1. : static_cast<double>(passes.size());
    const std::uint64_t submits = _state.calls[static_cast<std::size_t>(Stage::GpuSubmitWait)] + _state.calls[static_cast<std::size_t>(Stage::RasterSubmitWait)];
    std::string line = "{\"record\":\"utility_profile\"";

    line += ",\"sequence\":" + std::to_string(sequence.fetch_add(1));
    line += ",\"pid\":" + std::to_string(static_cast<long>(::getpid()));
    line += ",\"mode\":" + jsonString(_mode);
    line += ",\"device\":" + jsonString(_device);
    line += ",\"width\":" + std::to_string(_width);
    line += ",\"height\":" + std::to_string(_height);
    line += ",\"passes_wanted\":" + std::to_string(_passes);
    line += ",\"passes\":" + std::to_string(passes.size());
    line += std::string(",\"finished\":") + (_finished ? "true" : "false");
    line += ",\"timing\":\"cpu_wall\"";
    line += ",\"total_ms\":" + number(total);
    line += ",\"outside_passes_ms\":" + number(total - passSum);
    line += ",\"unstaged_ms\":" + number(total - staged);
    line += ",\"submits\":" + std::to_string(submits);
    line += ",\"pass_ms\":{\"sum\":" + number(passSum) + ",\"mean\":" + number(passes.empty() ? 0. : passSum / drawn);
    line += ",\"min\":" + number(passes.empty() ? 0. : *std::min_element(passes.begin(), passes.end()));
    line += ",\"max\":" + number(passes.empty() ? 0. : *std::max_element(passes.begin(), passes.end())) + "}";
    line += ",\"pass_times_ms\":[";
    for (std::size_t i = 0; i < passes.size(); ++i) {
        line += (i ? "," : "") + number(passes[i]);
    }
    line += "],\"stages\":{";

    bool first = true;

    for (std::size_t i = 0; i < kStageCount; ++i) {
        if (_state.calls[i] == 0) {
            continue;
        }
        line += first ? "" : ",";
        first = false;
        line += jsonString(stageName(static_cast<Stage>(i)));
        line += ":{\"ms\":" + number(_state.ms[i]);
        line += ",\"calls\":" + std::to_string(_state.calls[i]);
        line += ",\"in_passes_ms\":" + number(_state.inPassMs[i]);
        line += ",\"per_pass_ms\":" + number(_state.inPassMs[i] / drawn) + "}";
    }
    line += "}}\n";

    const std::lock_guard<std::mutex> l(fileMutex);

    if (std::FILE* f = std::fopen(profilePath(), "a")) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
}

NATRON_NAMESPACE_EXIT
