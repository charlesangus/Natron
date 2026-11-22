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

#include "CacheTier.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Engine/HostImageMemory.h"

NATRON_NAMESPACE_ENTER

namespace {

// How far a partial re-render moves the stored cost: its own scaling is noisier than a whole render's.
const double kPartialRenderWeight = 0.5;

// A measurement too short for the clock must still not read as no measurement.
const double kMinimumCostNs = 1000.;

thread_local std::uint64_t s_nestedRenderNs = 0;
thread_local CacheRecomputeStopwatch* s_currentStopwatch = NULL;

std::uint64_t
monotonicNs()
{
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

namespace CacheEviction {

std::uint64_t
nowNs()
{
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

double
score(double priority,
      float recomputeCostNs,
      std::uint32_t reuseCount,
      std::uint64_t ageNs,
      std::size_t bytes)
{
    if (bytes == 0) {
        return 0.;
    }

    const double cost = recomputeCostNs > 0.f ? recomputeCostNs : kUnmeasuredCostNs;
    const double reuseMultiplier = (1. + std::log2(1. + (double)reuseCount)) * std::pow(2., -(double)ageNs / kReuseHalfLifeNs);

    return priority * cost * reuseMultiplier / (double)bytes;
}

float
costAfterRender(float previousNs,
                double ownNs,
                double wholePixels,
                double renderedPixels)
{
    const double scale = renderedPixels > 0. ? std::max(1., wholePixels / renderedPixels) : 1.;
    const double cost = std::max(kMinimumCostNs, ownNs * scale);

    if (previousNs <= 0.f) {
        return (float)cost;
    }

    return (float)((double)previousNs + kPartialRenderWeight * (cost - (double)previousNs));
}

} // namespace CacheEviction

CacheRecomputeStopwatch::CacheRecomputeStopwatch()
    : _startNs(monotonicNs())
    , _nestedAtStart(s_nestedRenderNs)
    , _workerNestedNs(0)
    , _enclosing(s_currentStopwatch)
{
    s_currentStopwatch = this;
}

CacheRecomputeStopwatch::~CacheRecomputeStopwatch()
{
    // Replaces rather than adds: this scope's wall time already contains whatever nested in it.
    s_nestedRenderNs = _nestedAtStart + (monotonicNs() - _startNs);
    s_currentStopwatch = _enclosing;
}

double
CacheRecomputeStopwatch::ownNs() const
{
    const std::uint64_t wall = monotonicNs() - _startNs;
    const std::uint64_t nested = std::max(s_nestedRenderNs - _nestedAtStart, _workerNestedNs.load());

    return wall > nested ? (double)(wall - nested) : 0.;
}

CacheRecomputeStopwatch*
CacheRecomputeStopwatch::current()
{
    return s_currentStopwatch;
}

CacheRecomputeWorkerScope::CacheRecomputeWorkerScope(CacheRecomputeStopwatch* watch)
    : _watch(watch)
    , _nestedAtStart(s_nestedRenderNs)
{
}

CacheRecomputeWorkerScope::~CacheRecomputeWorkerScope()
{
    if (!_watch) {
        return;
    }

    const std::uint64_t nested = s_nestedRenderNs - _nestedAtStart;
    std::uint64_t longest = _watch->_workerNestedNs.load();

    while (nested > longest && !_watch->_workerNestedNs.compare_exchange_weak(longest, nested)) {
    }
}

CacheHostTier::CacheHostTier(const CacheHostStorage* storage,
                             std::size_t limit)
    : CacheTier()
    , _storage(storage)
    , _liveBytes(0)
    , _limit(limit)
    , _systemHeadroom(kSystemMemoryUnknown)
{
}

std::size_t
CacheHostTier::budget() const
{
    const std::size_t limit = _limit.load();
    const std::int64_t headroom = _systemHeadroom.load();

    if (headroom == kSystemMemoryUnknown) {
        return limit;
    }

    const std::size_t held = used();
    if (headroom >= 0) {
        const std::size_t room = (std::size_t)headroom;

        return room > limit || held > limit - room ? limit : held + room;
    }

    const std::size_t shortfall = (std::size_t)(-headroom);

    return std::min(limit, held > shortfall ? held - shortfall : 0);
}

std::size_t
CacheHostTier::used() const
{
    return _liveBytes.load() + HostImageMemory::pooledBytes();
}

bool
CacheHostTier::demote(const CacheEntryPtr& entry)
{
    if (!entry || !_storage) {
        return false;
    }

    return _storage->dropFromHost(entry);
}

bool
CacheHostTier::promote(const CacheEntryPtr& /*entry*/)
{
    return false;
}

void
CacheHostTier::removeBytes(std::size_t bytes)
{
    std::size_t current = _liveBytes.load();

    while (!_liveBytes.compare_exchange_weak(current, current > bytes ? current - bytes : 0)) {
    }
}

void
CacheHostTier::setSystemMemory(std::size_t availableBytes,
                               std::size_t keepFreeBytes)
{
    _systemHeadroom.store((std::int64_t)availableBytes - (std::int64_t)keepFreeBytes);
}

void
CacheHostTier::trimIdlePool() const
{
    const std::size_t mark = (std::size_t)(std::max((std::size_t)1, _limit.load()) * NATRON_CACHE_LIMIT_PERCENT);
    const std::size_t live = _liveBytes.load();

    HostImageMemory::trimTo(mark > live ? mark - live : 0);
}

CacheEntryPtr
CacheHostTier::demotionCandidate() const
{
    return _storage ? _storage->demotionCandidate() : CacheEntryPtr();
}

NATRON_NAMESPACE_EXIT
