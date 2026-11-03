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

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

LoopFrameCache::LoopFrameCache(std::size_t budget)
    : _mutex()
    , _budget(budget)
    , _bytes(0)
    , _clock(0)
    , _series()
{
}

std::size_t
LoopFrameCache::defaultBudget()
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (!settings) {
        return 0;
    }

    return (std::size_t)(settings->getRamMaximumPercent() * (double)getSystemTotalRAM_conditionnally() / 4.);
}

void
LoopFrameCache::setBudget(std::size_t budget)
{
    QMutexLocker k(&_mutex);

    _budget = budget;
    evict(LoopFrameSeries(), std::numeric_limits<int>::min());
}

std::size_t
LoopFrameCache::getBudget() const
{
    QMutexLocker k(&_mutex);

    return _budget;
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
    QMutexLocker k(&_mutex);
    Frames& frames = _series[series];
    if (frames.entries.empty()) {
        frames.startFrame = startFrame;
    }
    Entry& entry = frames.entries[frame];
    frames.bytes -= entry.bytes;
    _bytes -= entry.bytes;
    entry.image = image;
    entry.bytes = bytes;
    entry.lastUse = ++_clock;
    frames.bytes += bytes;
    _bytes += bytes;
    evict(series, frame);
}

ImagePtr
LoopFrameCache::lookup(const LoopFrameSeries& series,
                       int frame)
{
    QMutexLocker k(&_mutex);
    SeriesMap::iterator found = _series.find(series);

    if (found == _series.end()) {
        return ImagePtr();
    }
    std::map<int, Entry>::iterator entry = found->second.entries.find(frame);
    if (entry == found->second.entries.end()) {
        return ImagePtr();
    }
    entry->second.lastUse = ++_clock;

    return entry->second.image;
}

bool
LoopFrameCache::nearestBefore(const LoopFrameSeries& series,
                              int frame,
                              int* found,
                              ImagePtr* image)
{
    QMutexLocker k(&_mutex);
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
    after->second.lastUse = ++_clock;
    *found = after->first;
    *image = after->second.image;

    return true;
}

void
LoopFrameCache::purgeOtherGenerations(U64 hash)
{
    QMutexLocker k(&_mutex);

    for (SeriesMap::iterator it = _series.begin(); it != _series.end();) {
        if (it->first.hash != hash) {
            _bytes -= it->second.bytes;
            it = _series.erase(it);
        } else {
            ++it;
        }
    }
}

void
LoopFrameCache::clear()
{
    QMutexLocker k(&_mutex);

    _series.clear();
    _bytes = 0;
}

std::size_t
LoopFrameCache::bytes() const
{
    QMutexLocker k(&_mutex);

    return _bytes;
}

std::vector<int>
LoopFrameCache::frames(const LoopFrameSeries& series) const
{
    QMutexLocker k(&_mutex);
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
    QMutexLocker k(&_mutex);
    SeriesMap::const_iterator it = _series.find(series);

    return (it == _series.end()) ? 1 : intervalOf(it->second);
}

int
LoopFrameCache::intervalOf(const Frames& frames) const
{
    if (frames.entries.empty()) {
        return 1;
    }
    // Every series shares the quarter of the budget kept for checkpoints.
    const std::size_t share = _budget / 4 / std::max<std::size_t>(1, _series.size());
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

void
LoopFrameCache::evict(const LoopFrameSeries& kept,
                      int keptFrame)
{
    while (_bytes > _budget) {
        SeriesMap::iterator victimSeries = _series.end();
        std::map<int, Entry>::iterator victim;
        bool victimIsCheckpoint = true;

        for (SeriesMap::iterator s = _series.begin(); s != _series.end(); ++s) {
            const long long interval = intervalOf(s->second);
            const bool isKept = (s->first == kept);
            for (std::map<int, Entry>::iterator e = s->second.entries.begin(); e != s->second.entries.end(); ++e) {
                if (isKept && (e->first == keptFrame)) {
                    continue;
                }
                const long long offset = (long long)e->first - (long long)s->second.startFrame;
                const bool checkpoint = (offset >= 0) && (offset % interval == 0);
                const bool better = (victimSeries == _series.end()) || (victimIsCheckpoint && !checkpoint) || ((victimIsCheckpoint == checkpoint) && (e->second.lastUse < victim->second.lastUse));
                if (better) {
                    victimSeries = s;
                    victim = e;
                    victimIsCheckpoint = checkpoint;
                }
            }
        }
        if (victimSeries == _series.end()) {
            return;
        }
        victimSeries->second.bytes -= victim->second.bytes;
        _bytes -= victim->second.bytes;
        victimSeries->second.entries.erase(victim);
        if (victimSeries->second.entries.empty()) {
            _series.erase(victimSeries);
        }
    }
}

NATRON_NAMESPACE_EXIT
