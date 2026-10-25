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

#include "StageCache.h"

#include <algorithm>

#include "Engine/Nodes/Scene/SceneOps.h"

// How many composed stages the process-wide cache keeps: one per viewer input, with room for
// A/B and a few recently viewed stacks.
#define NATRON_STAGE_CACHE_CAPACITY 16

NATRON_NAMESPACE_ENTER

StageCache::StageCache(std::size_t capacity)
    : _mutex()
    , _capacity(std::max<std::size_t>(capacity, 1))
    , _entries()
    , _hits(0)
    , _misses(0)
{
}

StageCache::~StageCache()
{
}

StageCache&
StageCache::instance()
{
    // Leaked on purpose: USD's own registries are torn down at exit in an unspecified order
    // relative to this, and releasing a stage after them crashes.
    static StageCache* cache = new StageCache(NATRON_STAGE_CACHE_CAPACITY);

    return *cache;
}

PXR_NS::UsdStageRefPtr
StageCache::getStage(const ScenePayload& payload)
{
    if (payload.isEmpty()) {
        return PXR_NS::UsdStageRefPtr();
    }

    const U64 stackHash = payload.getStackHash();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::list<Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if (it->stackHash == stackHash) {
                _entries.splice(_entries.begin(), _entries, it);
                ++_hits;

                return _entries.front().stage;
            }
        }
        ++_misses;
    }

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    if (!stage) {
        return stage;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    // Another thread may have composed the same stack meanwhile: keep the first, so every
    // consumer shares one stage.
    for (std::list<Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->stackHash == stackHash) {
            _entries.splice(_entries.begin(), _entries, it);

            return _entries.front().stage;
        }
    }
    Entry entry;
    entry.stackHash = stackHash;
    entry.payload = payload;
    entry.stage = stage;
    _entries.push_front(entry);
    while (_entries.size() > _capacity) {
        _entries.pop_back();
    }

    return stage;
} // StageCache::getStage

void
StageCache::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _entries.clear();
}

std::size_t
StageCache::getNumEntries() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _entries.size();
}

U64
StageCache::getHitCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _hits;
}

U64
StageCache::getMissCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _misses;
}

NATRON_NAMESPACE_EXIT
