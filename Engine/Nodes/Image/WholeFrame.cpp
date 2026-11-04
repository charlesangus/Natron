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

#include "WholeFrame.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "Engine/EffectInstance.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

bool
wholeFramePixels(const EffectInstance& effect,
                 int sourceInput,
                 double time,
                 ViewIdx view,
                 const RenderScale& scale,
                 RectI* pixels,
                 RectI* fullScalePixels)
{
    const EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();

    if (!input) {
        return false;
    }
    RectD rod;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    const NodeMetadataConstPtr metadata = effect.getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(sourceInput) : 1.;
    if (rod.isInfinite()) {
        if (!metadata) {
            return false;
        }
        rod = rod.intersect(PassExec::toCanonical(metadata->getOutputFormat(), RenderScale::identity, par));
    }
    if (rod.isNull()) {
        return false;
    }
    const RectI atScale = PassExec::toPixelEnclosing(rod, scale, par);
    const RectI full = PassExec::toPixelEnclosing(rod, RenderScale::identity, par);
    if (atScale.isNull() || full.isNull()) {
        return false;
    }
    *pixels = atScale;
    if (fullScalePixels) {
        *fullScalePixels = full;
    }

    return true;
}

namespace {
// How often a caller waiting on another's compute asks whether its own render was aborted.
constexpr std::chrono::milliseconds kCancelPoll(10);

void
appendExact(std::string* key,
            double value)
{
    // Hexadecimal, so no two values share a key.
    char buf[64];

    std::snprintf(buf, sizeof(buf), "%a|", value);
    *key += buf;
}
} // anonymous namespace

std::string
wholeFrameKey(const EffectInstance& effect,
              int sourceInput,
              double time,
              ViewIdx view,
              const RenderScale& scale,
              const RectI& frame,
              const std::string& layerId,
              int nComps,
              const std::string& extra)
{
    const EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();
    std::string key = std::to_string(effect.getRenderHash()) + '|' + std::to_string(input ? input->getRenderHash() : 0) + '|';

    appendExact(&key, time);
    key += std::to_string((int)view) + '|';
    appendExact(&key, scale.toOfxPointD().x);
    appendExact(&key, scale.toOfxPointD().y);
    key += std::to_string(frame.x1) + ',' + std::to_string(frame.y1) + ',' + std::to_string(frame.x2) + ',' + std::to_string(frame.y2) + '|';
    key += layerId + '|' + std::to_string(nComps) + '|' + extra;

    return key;
}

bool
wholeFrameFitsGpu(const RectI& frame,
                  int floatsPerPixel,
                  std::uint64_t maxScratchBytes)
{
    if (frame.isNull() || (floatsPerPixel <= 0)) {
        return true;
    }
    const std::uint64_t floats = (std::uint64_t)frame.width() * (std::uint64_t)frame.height() * (std::uint64_t)floatsPerPixel;

    return (floats <= UINT32_MAX) && (floats <= maxScratchBytes / sizeof(float));
}

WholeFrameCache::WholeFrameCache(const char* what,
                                 std::size_t capacity)
    : _what(what)
    , _capacity(std::max<std::size_t>(capacity, 1))
    , _mutex()
    , _finished()
    , _results()
    , _computing()
    , _generation(0)
{
}

WholeFrameCache::Result
WholeFrameCache::get(const std::string& key,
                     const std::function<bool()>& cancelled,
                     const Compute& compute)
{
    const auto isCancelled = [&cancelled]() { return cancelled && cancelled(); };

    if (isCancelled()) {
        return Result();
    }
    std::unique_lock<std::mutex> lock(_mutex);

    for (;;) {
        for (std::list<std::pair<std::string, Result>>::iterator it = _results.begin(); it != _results.end(); ++it) {
            if (it->first == key) {
                _results.splice(_results.begin(), _results, it);

                return _results.front().second;
            }
        }
        const std::map<std::string, std::shared_ptr<InFlight>>::const_iterator computing = _computing.find(key);
        if (computing == _computing.end()) {
            break;
        }
        const std::shared_ptr<InFlight> flight = computing->second;
        while (!_finished.wait_for(lock, kCancelPoll, [&flight]() { return flight->done; })) {
            // cancelled may take locks of its own, so it is not asked under the cache's.
            lock.unlock();
            const bool stop = isCancelled();
            lock.lock();
            if (stop) {
                return Result();
            }
        }
        if (flight->result) {
            return flight->result;
        }
        lock.unlock();
        const bool stop = isCancelled();
        lock.lock();
        if (stop) {
            return Result();
        }
    }
    const std::shared_ptr<InFlight> flight = std::make_shared<InFlight>();
    _computing[key] = flight;
    const std::uint64_t generation = _generation;
    lock.unlock();

    Result result;
    try {
        result = compute();
    } catch (...) {
        finish(key, flight, Result(), generation);
        throw;
    }
    RawNodeSupport::noteFrameComputed(_what, (U64)std::hash<std::string>()(key));
    finish(key, flight, result, generation);

    return result;
}

void
WholeFrameCache::finish(const std::string& key,
                        const std::shared_ptr<InFlight>& flight,
                        const Result& result,
                        std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (result && (generation == _generation)) {
            _results.emplace_front(key, result);
            while (_results.size() > _capacity) {
                _results.pop_back();
            }
        }
        flight->result = result;
        flight->done = true;
        const std::map<std::string, std::shared_ptr<InFlight>>::iterator it = _computing.find(key);
        if ((it != _computing.end()) && (it->second == flight)) {
            _computing.erase(it);
        }
    }
    _finished.notify_all();
}

void
WholeFrameCache::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _results.clear();
    ++_generation;
}

NATRON_NAMESPACE_EXIT
