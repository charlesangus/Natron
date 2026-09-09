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

#ifndef DEEPIMAGECACHEENTRY_H
#define DEEPIMAGECACHEENTRY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/CacheEntry.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Cache<DeepImageCacheEntry> entry type. DeepImage itself stays a plain COW value
 * with no knowledge of the cache; this wrapper is what implements CacheEntryHelper's contract,
 * holding a DeepImage alongside the generic (and here unused-for-payload) buffer that
 * CacheEntryHelper manages internally.
 *
 * size() overrides CacheEntryHelper's default (which measures that generic buffer) to report
 * getDeepImage()->getSizeInBytes() instead -- the real, aliasing-blind cost of the payload. This
 * override must still run through the ordinary allocateMemory()/deallocate() path so the cache's
 * running byte total stays in sync (see DeepImageParams for why that path is RAM-backed at all
 * despite the real payload not living in it). Populate the DeepImage completely -- sample table
 * and channels -- before calling allocateMemory(): that call is the one point where the cache
 * captures this entry's byte cost, and mutating the DeepImage afterwards does not update it.
 *
 * Like Image, this class must call deallocate() from its own destructor body (not rely on
 * ~CacheEntryHelper() to do it), because by the time the base destructor runs, virtual dispatch
 * no longer reaches this class's size() override, and the byte count reported to the cache would
 * silently drop to whatever the untouched generic buffer measures.
 **/
class DeepImageCacheEntry
    : public CacheEntryHelper<char, DeepImageKey, DeepImageParams> {
public:
    DeepImageCacheEntry(const DeepImageKey& key,
                        const DeepImageParamsPtr& params,
                        const CacheAPI* cache);

    virtual ~DeepImageCacheEntry();

    const DeepImagePtr& getDeepImage() const
    {
        return _deepImage;
    }

    // The cache adds this entry's cost exactly once, from within allocateMemory(), and later
    // subtracts whatever size() reports at that same instant during deallocate(). Snapshotting
    // here (rather than measuring the DeepImage live in size()) keeps those two numbers equal
    // even if the payload is mutated in between, which would otherwise desync the cache's running
    // total and, via the clamp on underflow, silently disable eviction.
    virtual void onMemoryAllocated(bool /*diskRestoration*/) OVERRIDE FINAL
    {
        _accountedSize = _deepImage ? _deepImage->getSizeInBytes() : 0;
        _sizeAccounted = true;
    }

    virtual size_t size() const OVERRIDE
    {
        if (_sizeAccounted) {
            return _accountedSize;
        }
        return _deepImage ? _deepImage->getSizeInBytes() : 0;
    }

private:
    DeepImagePtr _deepImage;
    std::size_t _accountedSize;
    bool _sizeAccounted;
};

NATRON_NAMESPACE_EXIT

#endif // DEEPIMAGECACHEENTRY_H
