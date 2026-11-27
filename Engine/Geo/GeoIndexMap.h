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

#ifndef Engine_Geo_GeoIndexMap_h
#define Engine_Geo_GeoIndexMap_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

// An offset addresses attribute storage and never moves until a defragment; an index is the dense element number
// (point number, primitive number, vertex number) and shifts down whenever an earlier element is deleted.
using GeoOffset = std::int64_t;
using GeoIndex = std::int64_t;

struct GeoDefragmentResult;

/**
 * @brief How a defragment moved offsets. New offsets are dense, so the runs tile [0, newOffsetSize()) in order;
 * their old offsets increase too. Attribute storage applies this per destination page.
 */
class GeoIndexRemap {
public:
    struct Run {
        GeoOffset newOffset = 0;
        GeoOffset oldOffset = 0;
        GeoOffset count = 0;
    };

    GeoIndexRemap() = default;
    GeoIndexRemap(GeoOffset oldOffsetSize,
                  GeoOffset newOffsetSize,
                  std::vector<Run> runs);

    GeoOffset oldOffsetSize() const { return _oldOffsetSize; }

    GeoOffset newOffsetSize() const { return _newOffsetSize; }

    const std::vector<Run>& runs() const { return _runs; }

    bool isIdentity() const;

    GeoOffset oldFromNew(GeoOffset newOffset) const;

    // Returns -1 for an old offset that was a tombstone.
    GeoOffset newFromOld(GeoOffset oldOffset) const;

    /**
     * @brief The old page whose storage can be shared unchanged as new page `newPage`, or -1 when the page has to be
     * gathered. For the last, partial new page the shared old page also carries elements past the new end; those
     * were tombstones, so they are never read.
     */
    GeoOffset verbatimSourcePage(GeoOffset newPage) const;

    // Dense old -> new table, -1 for tombstones; for rewriting stored references such as vertex -> point offsets.
    std::vector<GeoOffset> newFromOldTable() const;

    // f(newOffset, oldOffset, count) for each run clipped to [newBegin, newEnd).
    template <typename F>
    void forEachRunInNewRange(GeoOffset newBegin,
                              GeoOffset newEnd,
                              F&& f) const
    {
        newBegin = std::max<GeoOffset>(newBegin, 0);
        newEnd = std::min(newEnd, _newOffsetSize);
        if (newBegin >= newEnd) {
            return;
        }
        for (std::size_t r = findRunByNew(newBegin); r < _runs.size(); ++r) {
            const Run& run = _runs[r];
            if (run.newOffset >= newEnd) {
                break;
            }
            const GeoOffset lo = std::max(newBegin, run.newOffset);
            const GeoOffset hi = std::min(newEnd, run.newOffset + run.count);
            f(lo, run.oldOffset + (lo - run.newOffset), hi - lo);
        }
    }

    // Moves a per-old-offset array into new-offset order; newValues must hold newOffsetSize() elements.
    template <typename T>
    void gather(std::span<const T> oldValues,
                std::span<T> newValues) const
    {
        for (const Run& run : _runs) {
            std::copy_n(oldValues.begin() + run.oldOffset, run.count, newValues.begin() + run.newOffset);
        }
    }

private:
    std::size_t findRunByNew(GeoOffset newOffset) const;

    GeoOffset _oldOffsetSize = 0;
    GeoOffset _newOffsetSize = 0;
    std::vector<Run> _runs;
};

/**
 * @brief Maps dense element numbers of one element class to storage offsets, with holes. Deleting tombstones an
 * offset; appending always takes fresh offsets at the end; only defragment() closes holes.
 *
 * Copy-on-write at two levels: copies share the whole map, and a write detaches the map's page table but only the
 * liveness page it touches. A map that has never had a tombstone stores no pages and maps number == offset.
 * Const access is thread-safe, so dense consumers can split work by page across threads with
 * indexAtPageStart() and forEachLiveRunInPage().
 */
class GeoIndexMap {
public:
    static constexpr int kPageShift = 10;
    static constexpr GeoOffset kPageSize = GeoOffset(1) << kPageShift;
    static constexpr GeoOffset kPageMask = kPageSize - 1;
    static constexpr int kWordsPerPage = int(kPageSize / 64);
    static constexpr GeoOffset kInvalidOffset = -1;
    static constexpr GeoIndex kInvalidIndex = -1;

    // Bit set = live. Bits for offsets at or past offsetSize() are always clear.
    struct LivePage {
        std::array<U64, kWordsPerPage> words {};
        GeoOffset liveCount = 0;
    };

    GeoIndexMap() = default;
    explicit GeoIndexMap(GeoOffset liveCount);

    GeoIndex size() const;

    GeoOffset offsetSize() const;

    GeoOffset tombstoneCount() const { return offsetSize() - size(); }

    bool empty() const { return size() == 0; }

    bool isTrivial() const;

    GeoOffset pageCount() const { return (offsetSize() + kPageMask) >> kPageShift; }

    bool isLive(GeoOffset offset) const;

    GeoOffset offsetFromIndex(GeoIndex index) const;

    GeoIndex indexFromOffset(GeoOffset offset) const;

    // Number of live elements in pages before `page`, which is the index of the page's first live element.
    GeoIndex indexAtPageStart(GeoOffset page) const;

    GeoOffset liveCountInPage(GeoOffset page) const;

    // nullptr when every existing offset in the page is live.
    const LivePage* livePage(GeoOffset page) const;

    // f(firstIndex, firstOffset, count) for each maximal run of live offsets in the page.
    template <typename F>
    void forEachLiveRunInPage(GeoOffset page,
                              F&& f) const
    {
        const GeoOffset begin = page << kPageShift;
        const GeoOffset end = std::min(begin + kPageSize, offsetSize());
        if (begin >= end) {
            return;
        }
        GeoIndex index = indexAtPageStart(page);
        const LivePage* live = livePage(page);
        if (!live) {
            f(index, begin, end - begin);
            return;
        }
        GeoOffset runStart = kInvalidOffset;
        for (int w = 0; w < kWordsPerPage; ++w) {
            const U64 word = live->words[w];
            const GeoOffset wordBase = begin + GeoOffset(w) * 64;
            int bit = 0;
            while (bit < 64) {
                const U64 rest = word >> bit;
                if (runStart == kInvalidOffset) {
                    if (rest == 0) {
                        break;
                    }
                    bit += std::countr_zero(rest);
                    runStart = wordBase + bit;
                } else {
                    // The shift fills the top with zeros, so this reaches 64 only when the run continues into the
                    // next word.
                    const int ones = std::countr_zero(~rest);
                    bit += ones;
                    if (bit >= 64) {
                        break;
                    }
                    const GeoOffset count = wordBase + bit - runStart;
                    f(index, runStart, count);
                    index += count;
                    runStart = kInvalidOffset;
                }
            }
        }
        if (runStart != kInvalidOffset) {
            f(index, runStart, end - runStart);
        }
    }

    // Runs in index order, merged across page boundaries.
    template <typename F>
    void forEachLiveRun(F&& f) const
    {
        if (isTrivial()) {
            if (offsetSize() > 0) {
                f(GeoIndex(0), GeoOffset(0), offsetSize());
            }
            return;
        }
        GeoIndex pendingIndex = 0;
        GeoOffset pendingOffset = 0;
        GeoOffset pendingCount = 0;
        const GeoOffset pages = pageCount();
        for (GeoOffset p = 0; p < pages; ++p) {
            forEachLiveRunInPage(p, [&](GeoIndex index, GeoOffset offset, GeoOffset count) {
                if (pendingCount > 0 && offset == pendingOffset + pendingCount) {
                    pendingCount += count;
                    return;
                }
                if (pendingCount > 0) {
                    f(pendingIndex, pendingOffset, pendingCount);
                }
                pendingIndex = index;
                pendingOffset = offset;
                pendingCount = count;
            });
        }
        if (pendingCount > 0) {
            f(pendingIndex, pendingOffset, pendingCount);
        }
    }

    // f(index, offset) for every live element in index order.
    template <typename F>
    void forEachLive(F&& f) const
    {
        forEachLiveRun([&](GeoIndex index, GeoOffset offset, GeoOffset count) {
            for (GeoOffset i = 0; i < count; ++i) {
                f(index + i, offset + i);
            }
        });
    }

    // Returns the first new offset; the new elements take the indices after every existing one.
    GeoOffset append(GeoOffset count);

    // Returns false when the offset was already a tombstone or out of range.
    bool tombstone(GeoOffset offset);

    // Returns how many offsets were live and are now tombstones; duplicates and dead offsets are ignored.
    GeoOffset tombstone(std::span<const GeoOffset> offsets);

    GeoOffset tombstoneRange(GeoOffset begin,
                             GeoOffset end);

    void clear();

    // With no tombstones the returned map shares this one's storage and the remap is the identity.
    GeoDefragmentResult defragment() const;

    bool sharesDataWith(const GeoIndexMap& other) const { return _data && _data == other._data; }

private:
    struct Data;

    Data& writable();
    LivePage& writablePage(Data& data,
                           GeoOffset page);
    GeoOffset tombstoneInPage(Data& data,
                              GeoOffset page,
                              GeoOffset localBegin,
                              GeoOffset localEnd);

    std::shared_ptr<Data> _data;
};

struct GeoDefragmentResult {
    GeoIndexMap map;
    GeoIndexRemap remap;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoIndexMap_h
