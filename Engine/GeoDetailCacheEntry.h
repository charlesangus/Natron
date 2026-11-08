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

#ifndef GEODETAILCACHEENTRY_H
#define GEODETAILCACHEENTRY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>

#include "Engine/CacheEntry.h"
#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/GeoDetailKey.h"
#include "Engine/NonKeyParams.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Non-identifying data for a GeoDetailCacheEntry. There is none: the payload is the
 * detail itself. Its storage info is a fixed 1-byte RAM allocation so that CacheEntryHelper's
 * allocate-once/deallocate bookkeeping works normally, as for DeepImageParams.
 **/
class GeoDetailParams
    : public NonKeyParams {
public:
    GeoDetailParams();

    GeoDetailParams(const GeoDetailParams& other);

    virtual ~GeoDetailParams()
    {
    }

    bool operator==(const GeoDetailParams&) const
    {
        return true;
    }

    bool operator!=(const GeoDetailParams& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief The Cache<GeoDetailCacheEntry> entry type. GeoDetail stays a plain COW value that knows
 * nothing about the cache.
 *
 * The cost is GeoDetail::getSizeInBytes(), snapshotted when allocateMemory() runs, so the amount
 * added to the cache's running total and the amount subtracted on deallocate() are the same
 * number. Call setGeoDetail() before allocateMemory(); the detail is const from then on.
 *
 * The destructor calls deallocate() itself because by the time ~CacheEntryHelper() runs, virtual
 * dispatch no longer reaches this class's size().
 **/
class GeoDetailCacheEntry
    : public CacheEntryHelper<char, GeoDetailKey, GeoDetailParams> {
public:
    GeoDetailCacheEntry(const GeoDetailKey& key,
                        const GeoDetailParamsPtr& params,
                        const CacheAPI* cache);

    virtual ~GeoDetailCacheEntry();

    /** @brief Null until setGeoDetail(). */
    const GeoDetailConstPtr& getGeoDetail() const
    {
        return _detail;
    }

    /** @brief Has no effect once the entry's memory has been allocated. */
    void setGeoDetail(const GeoDetailConstPtr& detail);

    /** @brief False until the detail has been set and allocateMemory() has run. */
    bool isFullyRendered() const
    {
        return _sizeAccounted;
    }

    virtual void onMemoryAllocated(bool /*diskRestoration*/) OVERRIDE FINAL
    {
        _accountedSize = _detail ? _detail->getSizeInBytes() : 0;
        _sizeAccounted = true;
    }

    virtual size_t size() const OVERRIDE
    {
        if (_sizeAccounted) {
            return _accountedSize;
        }
        return _detail ? _detail->getSizeInBytes() : 0;
    }

private:
    GeoDetailConstPtr _detail;
    std::size_t _accountedSize;
    bool _sizeAccounted;
};

NATRON_NAMESPACE_EXIT

#endif // GEODETAILCACHEENTRY_H
