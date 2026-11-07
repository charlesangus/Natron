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

#include "Engine/HostImageMemory.h"

NATRON_NAMESPACE_ENTER

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
