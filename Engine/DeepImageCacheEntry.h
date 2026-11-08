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

#include <atomic>
#include <string>

#include "Engine/CacheEntry.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct CacheDiskFileInfo;
class CacheDiskSource;

/**
 * @brief The deep payload of the node cache, next to Image. DeepImage itself stays a plain COW
 * value with no knowledge of the cache; this wrapper is what the cache stores, finds and evicts.
 *
 * A deep image's size is known only after it has rendered, whereas the cache seals an entry before
 * that. So the generic buffer of CacheEntryHelper stays unallocated (its storage info says
 * eStorageModeNone), the entry costs nothing while it renders, and allocateMemory() below reports
 * the real, aliasing-blind cost of the payload. Populate the DeepImage completely -- sample table
 * and channels -- before calling it: that call is the one point where the cache captures this
 * entry's byte cost, and mutating the DeepImage afterwards does not update it.
 *
 * Like Image, this class must call deallocate() from its own destructor body, because by the time
 * the base destructor runs, virtual dispatch no longer reaches the override below and the cache
 * would never get this entry's bytes back.
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

    /**
     * @brief The entry's image, under a pointer that shares ownership with the entry. The cache
     * evicts what nothing but the cache holds, so a consumer must hold this one for the entry to
     * stay pinned the way an ImagePtr pins an Image. Never store it in the entry: that is a cycle.
     **/
    static DeepImagePtr pinnedDeepImage(const DeepImageCacheEntryPtr& entry)
    {
        return DeepImagePtr(entry, entry->getDeepImage().get());
    }

    const CacheEntryHeaderPtr& getEntryHeader() const
    {
        return _header;
    }

    virtual CacheEntryHeader* evictionHeader() const OVERRIDE FINAL
    {
        return _header.get();
    }

    typedef void DiskPayloadTag;

    virtual bool diskIdentity(CacheDiskIdentity* identity) const OVERRIDE FINAL;
    virtual bool diskSnapshot(CacheDiskSnapshot* snapshot) const OVERRIDE FINAL;
    virtual bool diskWritePayload(const CacheDiskSnapshot& snapshot, CacheDiskSink& sink) const OVERRIDE FINAL;

    static CacheDiskIdentity diskIdentityOfKey(const DeepImageKey& key);
    static std::string diskParamsBytes(const DeepImageParams& params);
    static DeepImageParamsPtr diskParamsFromBytes(const std::string& bytes, const RectI& bounds);

    // False for a file that contradicts itself, which is corrupt. Throws std::bad_alloc.
    bool diskRestore(const CacheDiskFileInfo& info, CacheDiskSource& source);

    // False from construction until renderDeepRoI() has actually populated the sample table and
    // channels and called allocateMemory(): a cache lookup that finds this entry before then would
    // otherwise hand out an empty payload that just happens to have the right bounds.
    bool isFullyRendered() const
    {
        return _sizeAccounted;
    }

    // Hides CacheEntryHelper::allocateMemory(), which has no buffer to allocate here. The cache
    // never allocates an entry itself and every caller holds the typed pointer.
    void allocateMemory();

    virtual void deallocate() OVERRIDE FINAL;

    // The cache adds this entry's cost exactly once, from within allocateMemory(), and later
    // subtracts this during deallocate(). A snapshot rather than a live measure keeps those two
    // numbers equal even if the payload is mutated in between, which would otherwise desync the
    // cache's running total and, via the clamp on underflow, silently disable eviction. It also
    // makes this safe to call while the entry renders, as cache statistics and eviction do.
    virtual size_t size() const OVERRIDE
    {
        return _sizeAccounted ? _accountedSize : 0;
    }

private:
    CacheEntryHeaderPtr _header;
    DeepImagePtr _deepImage;
    std::size_t _accountedSize;
    std::atomic<bool> _sizeAccounted;
};

NATRON_NAMESPACE_EXIT

#endif // DEEPIMAGECACHEENTRY_H
