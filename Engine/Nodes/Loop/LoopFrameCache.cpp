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

#include "LoopFrameCache.h"

#include <algorithm>
#include <limits>
#include <mutex>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace {
std::mutex sharedBudgetMutex;
std::shared_ptr<LoopFrameBudget> sharedBudget;
} // anonymous namespace

LoopFrameBudget::LoopFrameBudget(std::size_t budget)
    : _mutex()
    , _budget(budget)
    , _bytes(0)
    , _clock(0)
    , _caches()
{
}

std::shared_ptr<LoopFrameBudget>
LoopFrameBudget::shared()
{
    std::lock_guard<std::mutex> l(sharedBudgetMutex);

    if (!sharedBudget) {
        sharedBudget = std::make_shared<LoopFrameBudget>(defaultBudget());
    }

    return sharedBudget;
}

void
LoopFrameBudget::refreshShared()
{
    std::shared_ptr<LoopFrameBudget> budget;
    {
        std::lock_guard<std::mutex> l(sharedBudgetMutex);
        budget = sharedBudget;
    }
    if (budget) {
        budget->setBudget(defaultBudget());
    }
}

std::size_t
LoopFrameBudget::defaultBudget()
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (!settings) {
        return 0;
    }

    return (std::size_t)(settings->getRamMaximumPercent() * (double)getSystemTotalRAM_conditionnally() / 4.);
}

void
LoopFrameBudget::setBudget(std::size_t budget)
{
    QMutexLocker k(&_mutex);

    _budget = budget;
    evict(0, LoopFrameSeries(), std::numeric_limits<int>::min());
}

std::size_t
LoopFrameBudget::getBudget() const
{
    QMutexLocker k(&_mutex);

    return _budget;
}

std::size_t
LoopFrameBudget::bytes() const
{
    QMutexLocker k(&_mutex);

    return _bytes;
}

std::size_t
LoopFrameBudget::seriesCount() const
{
    std::size_t count = 0;

    for (const LoopFrameCache* cache : _caches) {
        count += cache->_series.size();
    }

    return count;
}

void
LoopFrameBudget::evict(const LoopFrameCache* keptCache,
                       const LoopFrameSeries& kept,
                       int keptFrame)
{
    while (_bytes > _budget) {
        LoopFrameCache* victimCache = 0;
        LoopFrameCache::SeriesMap::iterator victimSeries;
        std::map<int, LoopFrameCache::Entry>::iterator victim;
        bool victimIsCheckpoint = true;

        for (LoopFrameCache* cache : _caches) {
            for (LoopFrameCache::SeriesMap::iterator s = cache->_series.begin(); s != cache->_series.end(); ++s) {
                const long long interval = cache->intervalOf(s->second);
                const bool isKept = (cache == keptCache) && (s->first == kept);
                for (std::map<int, LoopFrameCache::Entry>::iterator e = s->second.entries.begin(); e != s->second.entries.end(); ++e) {
                    if (isKept && (e->first == keptFrame)) {
                        continue;
                    }
                    const long long offset = (long long)e->first - (long long)s->second.startFrame;
                    const bool checkpoint = (offset >= 0) && (offset % interval == 0);
                    const bool better = !victimCache || (victimIsCheckpoint && !checkpoint) || ((victimIsCheckpoint == checkpoint) && (e->second.lastUse < victim->second.lastUse));
                    if (better) {
                        victimCache = cache;
                        victimSeries = s;
                        victim = e;
                        victimIsCheckpoint = checkpoint;
                    }
                }
            }
        }
        if (!victimCache) {
            return;
        }
        victimSeries->second.bytes -= victim->second.bytes;
        victimCache->_bytes -= victim->second.bytes;
        _bytes -= victim->second.bytes;
        victimSeries->second.entries.erase(victim);
        if (victimSeries->second.entries.empty()) {
            victimCache->_series.erase(victimSeries);
        }
    }
}

LoopFrameCache::LoopFrameCache(std::size_t budget)
    : LoopFrameCache(std::make_shared<LoopFrameBudget>(budget))
{
}

LoopFrameCache::LoopFrameCache(const std::shared_ptr<LoopFrameBudget>& budget)
    : _budget(budget)
    , _bytes(0)
    , _series()
{
    QMutexLocker k(&_budget->_mutex);

    _budget->_caches.push_back(this);
}

LoopFrameCache::~LoopFrameCache()
{
    QMutexLocker k(&_budget->_mutex);

    clearLocked();
    _budget->_caches.erase(std::remove(_budget->_caches.begin(), _budget->_caches.end(), this), _budget->_caches.end());
}

const std::shared_ptr<LoopFrameBudget>&
LoopFrameCache::budget() const
{
    return _budget;
}

void
LoopFrameCache::setBudget(std::size_t budget)
{
    _budget->setBudget(budget);
}

std::size_t
LoopFrameCache::getBudget() const
{
    return _budget->getBudget();
}

void
LoopFrameCache::insert(const LoopFrameSeries& series,
                       int startFrame,
                       int frame,
                       const ImagePtr& image,
                       std::size_t bytes)
{
    if (!image) {
        return;
    }
    if (bytes == 0) {
        bytes = image->size();
    }
    QMutexLocker k(&_budget->_mutex);
    Frames& frames = _series[series];
    if (frames.entries.empty()) {
        frames.startFrame = startFrame;
    }
    Entry& entry = frames.entries[frame];
    frames.bytes -= entry.bytes;
    _bytes -= entry.bytes;
    _budget->_bytes -= entry.bytes;
    entry.image = image;
    entry.bytes = bytes;
    entry.lastUse = ++_budget->_clock;
    frames.bytes += bytes;
    _bytes += bytes;
    _budget->_bytes += bytes;
    _budget->evict(this, series, frame);
}

ImagePtr
LoopFrameCache::lookup(const LoopFrameSeries& series,
                       int frame)
{
    QMutexLocker k(&_budget->_mutex);
    SeriesMap::iterator found = _series.find(series);

    if (found == _series.end()) {
        return ImagePtr();
    }
    std::map<int, Entry>::iterator entry = found->second.entries.find(frame);
    if (entry == found->second.entries.end()) {
        return ImagePtr();
    }
    entry->second.lastUse = ++_budget->_clock;

    return entry->second.image;
}

bool
LoopFrameCache::nearestBefore(const LoopFrameSeries& series,
                              int frame,
                              int* found,
                              ImagePtr* image)
{
    QMutexLocker k(&_budget->_mutex);
    SeriesMap::iterator it = _series.find(series);

    if (it == _series.end()) {
        return false;
    }
    std::map<int, Entry>& entries = it->second.entries;
    std::map<int, Entry>::iterator after = entries.lower_bound(frame);
    if (after == entries.begin()) {
        return false;
    }
    --after;
    after->second.lastUse = ++_budget->_clock;
    *found = after->first;
    *image = after->second.image;

    return true;
}

void
LoopFrameCache::purgeOtherGenerations(U64 hash)
{
    QMutexLocker k(&_budget->_mutex);

    for (SeriesMap::iterator it = _series.begin(); it != _series.end();) {
        if (it->first.hash != hash) {
            _bytes -= it->second.bytes;
            _budget->_bytes -= it->second.bytes;
            it = _series.erase(it);
        } else {
            ++it;
        }
    }
}

void
LoopFrameCache::clear()
{
    QMutexLocker k(&_budget->_mutex);

    clearLocked();
}

void
LoopFrameCache::clearLocked()
{
    _budget->_bytes -= _bytes;
    _bytes = 0;
    _series.clear();
}

std::size_t
LoopFrameCache::bytes() const
{
    QMutexLocker k(&_budget->_mutex);

    return _bytes;
}

std::vector<int>
LoopFrameCache::frames(const LoopFrameSeries& series) const
{
    QMutexLocker k(&_budget->_mutex);
    std::vector<int> result;
    SeriesMap::const_iterator it = _series.find(series);

    if (it != _series.end()) {
        for (std::map<int, Entry>::const_iterator entry = it->second.entries.begin(); entry != it->second.entries.end(); ++entry) {
            result.push_back(entry->first);
        }
    }

    return result;
}

int
LoopFrameCache::checkpointInterval(const LoopFrameSeries& series) const
{
    QMutexLocker k(&_budget->_mutex);
    SeriesMap::const_iterator it = _series.find(series);

    return (it == _series.end()) ? 1 : intervalOf(it->second);
}

int
LoopFrameCache::intervalOf(const Frames& frames) const
{
    if (frames.entries.empty()) {
        return 1;
    }
    // Every series of every cache on the budget shares the quarter of it kept for checkpoints.
    const std::size_t share = _budget->_budget / 4 / std::max<std::size_t>(1, _budget->seriesCount());
    const std::size_t average = std::max<std::size_t>(1, frames.bytes / frames.entries.size());
    const long long capacity = (long long)std::max<std::size_t>(1, share / average);
    const long long span = std::max(0LL, (long long)frames.entries.rbegin()->first - (long long)frames.startFrame);
    // A power of two, so the checkpoints at an interval are among those at half of it: frames
    // kept as checkpoints stay checkpoints when the interval doubles, or are the first to go.
    long long interval = 1;

    while ((span / interval + 1 > capacity) && (interval < (1LL << 30))) {
        interval *= 2;
    }

    return (int)interval;
}

NATRON_NAMESPACE_EXIT
