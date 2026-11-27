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

#include "GeoIndexMap.h"

#include <atomic>
#include <mutex>
#include <utility>

#include "Engine/Geo/GeoPage.h"

NATRON_NAMESPACE_ENTER

namespace {

const std::shared_ptr<GeoIndexMap::LivePage>&
allDeadPage()
{
    static const std::shared_ptr<GeoIndexMap::LivePage> page = std::make_shared<GeoIndexMap::LivePage>();
    return page;
}

U64
bitsBelow(int bit)
{
    return bit >= 64 ? ~U64(0) : ((U64(1) << bit) - 1);
}

// Mask of bits [lo, hi) within one 64-bit word, 0 <= lo <= hi <= 64.
U64
wordMask(int lo,
         int hi)
{
    return bitsBelow(hi) & ~bitsBelow(lo);
}

} // namespace

GeoIndexRemap::GeoIndexRemap(GeoOffset oldOffsetSize,
                             GeoOffset newOffsetSize,
                             std::vector<Run> runs)
    : _oldOffsetSize(oldOffsetSize)
    , _newOffsetSize(newOffsetSize)
    , _runs(std::move(runs))
{
}

bool
GeoIndexRemap::isIdentity() const
{
    if (_oldOffsetSize != _newOffsetSize) {
        return false;
    }
    return _runs.empty() || (_runs.size() == 1 && _runs[0].oldOffset == 0);
}

std::size_t
GeoIndexRemap::findRunByNew(GeoOffset newOffset) const
{
    auto it = std::upper_bound(_runs.begin(), _runs.end(), newOffset, [](GeoOffset value, const Run& run) {
        return value < run.newOffset;
    });
    return it == _runs.begin() ? 0 : std::size_t(it - _runs.begin()) - 1;
}

GeoOffset
GeoIndexRemap::oldFromNew(GeoOffset newOffset) const
{
    if (newOffset < 0 || newOffset >= _newOffsetSize) {
        return GeoIndexMap::kInvalidOffset;
    }
    const Run& run = _runs[findRunByNew(newOffset)];
    return run.oldOffset + (newOffset - run.newOffset);
}

GeoOffset
GeoIndexRemap::newFromOld(GeoOffset oldOffset) const
{
    if (oldOffset < 0 || oldOffset >= _oldOffsetSize) {
        return GeoIndexMap::kInvalidOffset;
    }
    auto it = std::upper_bound(_runs.begin(), _runs.end(), oldOffset, [](GeoOffset value, const Run& run) {
        return value < run.oldOffset;
    });
    if (it == _runs.begin()) {
        return GeoIndexMap::kInvalidOffset;
    }
    const Run& run = *(it - 1);
    if (oldOffset >= run.oldOffset + run.count) {
        return GeoIndexMap::kInvalidOffset;
    }
    return run.newOffset + (oldOffset - run.oldOffset);
}

GeoOffset
GeoIndexRemap::verbatimSourcePage(GeoOffset newPage) const
{
    const GeoOffset newBegin = newPage << GeoIndexMap::kPageShift;
    if (newPage < 0 || newBegin >= _newOffsetSize) {
        return GeoIndexMap::kInvalidOffset;
    }
    const GeoOffset newEnd = std::min(newBegin + GeoIndexMap::kPageSize, _newOffsetSize);
    const Run& run = _runs[findRunByNew(newBegin)];
    if (run.newOffset + run.count < newEnd) {
        return GeoIndexMap::kInvalidOffset;
    }
    const GeoOffset oldBegin = run.oldOffset + (newBegin - run.newOffset);
    if ((oldBegin & GeoIndexMap::kPageMask) != 0) {
        return GeoIndexMap::kInvalidOffset;
    }
    return oldBegin >> GeoIndexMap::kPageShift;
}

std::vector<GeoOffset>
GeoIndexRemap::newFromOldTable() const
{
    std::vector<GeoOffset> table(std::size_t(_oldOffsetSize), GeoIndexMap::kInvalidOffset);
    for (const Run& run : _runs) {
        for (GeoOffset i = 0; i < run.count; ++i) {
            table[std::size_t(run.oldOffset + i)] = run.newOffset + i;
        }
    }
    return table;
}

struct GeoIndexMap::Data {
    GeoOffset offsetSize = 0;
    GeoIndex liveCount = 0;

    // Empty while the map has no tombstone; otherwise one entry per page, null meaning all existing offsets are live.
    // Pages are shared between copies and written only when this map holds the sole reference.
    std::vector<std::shared_ptr<LivePage>> pages;

    // Lazily built so that a run of single-element deletes doesn't pay a full prefix sum each.
    mutable std::vector<GeoIndex> pageBase;
    mutable std::atomic<bool> pageBaseValid { false };
    mutable std::mutex pageBaseMutex;

    Data() = default;

    Data(const Data& other)
        : offsetSize(other.offsetSize)
        , liveCount(other.liveCount)
        , pages(other.pages)
    {
    }

    Data& operator=(const Data&) = delete;

    void invalidatePageBase() { pageBaseValid.store(false, std::memory_order_relaxed); }

    void ensurePageBase() const
    {
        if (pageBaseValid.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lock(pageBaseMutex);
        if (pageBaseValid.load(std::memory_order_relaxed)) {
            return;
        }
        pageBase.resize(pages.size());
        GeoIndex base = 0;
        for (std::size_t p = 0; p < pages.size(); ++p) {
            pageBase[p] = base;
            if (pages[p]) {
                base += pages[p]->liveCount;
            } else {
                const GeoOffset begin = GeoOffset(p) << kPageShift;
                base += std::min(kPageSize, offsetSize - begin);
            }
        }
        pageBaseValid.store(true, std::memory_order_release);
    }
};

GeoIndexMap::GeoIndexMap(GeoOffset liveCount)
{
    if (liveCount > 0) {
        _data = std::make_shared<Data>();
        _data->offsetSize = liveCount;
        _data->liveCount = liveCount;
    }
}

GeoIndex
GeoIndexMap::size() const
{
    return _data ? _data->liveCount : 0;
}

GeoOffset
GeoIndexMap::offsetSize() const
{
    return _data ? _data->offsetSize : 0;
}

bool
GeoIndexMap::isTrivial() const
{
    return !_data || _data->pages.empty();
}

bool
GeoIndexMap::isLive(GeoOffset offset) const
{
    if (offset < 0 || offset >= offsetSize()) {
        return false;
    }
    if (_data->pages.empty()) {
        return true;
    }
    const LivePage* page = _data->pages[std::size_t(offset >> kPageShift)].get();
    if (!page) {
        return true;
    }
    const GeoOffset local = offset & kPageMask;
    return (page->words[std::size_t(local >> 6)] >> (local & 63)) & 1;
}

GeoOffset
GeoIndexMap::offsetFromIndex(GeoIndex index) const
{
    if (index < 0 || index >= size()) {
        return kInvalidOffset;
    }
    if (_data->pages.empty()) {
        return index;
    }
    _data->ensurePageBase();
    const std::vector<GeoIndex>& base = _data->pageBase;
    // Pages with no live element share their successor's base, so the last page whose base is <= index is the one
    // holding it.
    const std::size_t p = std::size_t(std::upper_bound(base.begin(), base.end(), index) - base.begin()) - 1;
    GeoOffset local = index - base[p];
    const GeoOffset pageBegin = GeoOffset(p) << kPageShift;
    const LivePage* page = _data->pages[p].get();
    if (!page) {
        return pageBegin + local;
    }
    for (int w = 0; w < kWordsPerPage; ++w) {
        U64 word = page->words[w];
        const int count = std::popcount(word);
        if (local < count) {
            for (GeoOffset k = 0; k < local; ++k) {
                word &= word - 1;
            }
            return pageBegin + GeoOffset(w) * 64 + std::countr_zero(word);
        }
        local -= count;
    }
    return kInvalidOffset;
}

GeoIndex
GeoIndexMap::indexFromOffset(GeoOffset offset) const
{
    if (!isLive(offset)) {
        return kInvalidIndex;
    }
    if (_data->pages.empty()) {
        return offset;
    }
    _data->ensurePageBase();
    const std::size_t p = std::size_t(offset >> kPageShift);
    const GeoOffset local = offset & kPageMask;
    const GeoIndex base = _data->pageBase[p];
    const LivePage* page = _data->pages[p].get();
    if (!page) {
        return base + local;
    }
    const int word = int(local >> 6);
    GeoIndex below = 0;
    for (int w = 0; w < word; ++w) {
        below += std::popcount(page->words[w]);
    }
    below += std::popcount(page->words[word] & bitsBelow(int(local & 63)));
    return base + below;
}

GeoIndex
GeoIndexMap::indexAtPageStart(GeoOffset page) const
{
    if (page <= 0) {
        return 0;
    }
    if (page >= pageCount()) {
        return size();
    }
    if (_data->pages.empty()) {
        return page << kPageShift;
    }
    _data->ensurePageBase();
    return _data->pageBase[std::size_t(page)];
}

GeoOffset
GeoIndexMap::liveCountInPage(GeoOffset page) const
{
    if (page < 0 || page >= pageCount()) {
        return 0;
    }
    const LivePage* live = livePage(page);
    if (live) {
        return live->liveCount;
    }
    return std::min(kPageSize, offsetSize() - (page << kPageShift));
}

const GeoIndexMap::LivePage*
GeoIndexMap::livePage(GeoOffset page) const
{
    if (!_data || _data->pages.empty() || page < 0 || page >= GeoOffset(_data->pages.size())) {
        return nullptr;
    }
    return _data->pages[std::size_t(page)].get();
}

GeoIndexMap::Data&
GeoIndexMap::writable()
{
    if (!_data) {
        _data = std::make_shared<Data>();
    } else if (!geoIsSoleOwner(_data)) {
        _data = std::make_shared<Data>(*_data);
    }
    _data->invalidatePageBase();
    return *_data;
}

GeoIndexMap::LivePage&
GeoIndexMap::writablePage(Data& data,
                          GeoOffset page)
{
    if (data.pages.empty()) {
        data.pages.assign(std::size_t((data.offsetSize + kPageMask) >> kPageShift), nullptr);
    }
    std::shared_ptr<LivePage>& slot = data.pages[std::size_t(page)];
    if (!slot) {
        slot = std::make_shared<LivePage>();
        const GeoOffset count = std::min(kPageSize, data.offsetSize - (page << kPageShift));
        for (GeoOffset w = 0; w * 64 < count; ++w) {
            slot->words[std::size_t(w)] = bitsBelow(int(std::min<GeoOffset>(64, count - w * 64)));
        }
        slot->liveCount = count;
    } else if (!geoIsSoleOwner(slot)) {
        slot = std::make_shared<LivePage>(*slot);
    }
    return *slot;
}

GeoOffset
GeoIndexMap::append(GeoOffset count)
{
    const GeoOffset first = offsetSize();
    if (count <= 0) {
        return first;
    }
    Data& data = writable();
    const GeoOffset newSize = first + count;
    if (!data.pages.empty()) {
        const GeoOffset tailPage = first >> kPageShift;
        if ((first & kPageMask) != 0 && data.pages[std::size_t(tailPage)]) {
            LivePage& page = writablePage(data, tailPage);
            const GeoOffset lo = first & kPageMask;
            const GeoOffset hi = std::min(kPageSize, newSize - (tailPage << kPageShift));
            for (GeoOffset w = lo >> 6; w * 64 < hi; ++w) {
                const int wlo = int(std::max(lo - w * 64, GeoOffset(0)));
                const int whi = int(std::min(hi - w * 64, GeoOffset(64)));
                page.words[std::size_t(w)] |= wordMask(wlo, whi);
            }
            page.liveCount += hi - lo;
        }
        data.pages.resize(std::size_t((newSize + kPageMask) >> kPageShift), nullptr);
    }
    data.offsetSize = newSize;
    data.liveCount += count;
    return first;
}

GeoOffset
GeoIndexMap::tombstoneInPage(Data& data,
                             GeoOffset page,
                             GeoOffset localBegin,
                             GeoOffset localEnd)
{
    LivePage& live = writablePage(data, page);
    GeoOffset removed = 0;
    for (GeoOffset w = localBegin >> 6; w * 64 < localEnd; ++w) {
        const int wlo = int(std::max(localBegin - w * 64, GeoOffset(0)));
        const int whi = int(std::min(localEnd - w * 64, GeoOffset(64)));
        U64& word = live.words[std::size_t(w)];
        const U64 hit = word & wordMask(wlo, whi);
        removed += std::popcount(hit);
        word &= ~hit;
    }
    live.liveCount -= removed;
    data.liveCount -= removed;
    if (live.liveCount == 0) {
        data.pages[std::size_t(page)] = allDeadPage();
    }
    return removed;
}

bool
GeoIndexMap::tombstone(GeoOffset offset)
{
    if (!isLive(offset)) {
        return false;
    }
    const GeoOffset local = offset & kPageMask;
    return tombstoneInPage(writable(), offset >> kPageShift, local, local + 1) == 1;
}

GeoOffset
GeoIndexMap::tombstone(std::span<const GeoOffset> offsets)
{
    Data* data = nullptr;
    GeoOffset removed = 0;
    for (GeoOffset offset : offsets) {
        if (!isLive(offset)) {
            continue;
        }
        if (!data) {
            data = &writable();
        }
        const GeoOffset local = offset & kPageMask;
        removed += tombstoneInPage(*data, offset >> kPageShift, local, local + 1);
    }
    return removed;
}

GeoOffset
GeoIndexMap::tombstoneRange(GeoOffset begin,
                            GeoOffset end)
{
    begin = std::max<GeoOffset>(begin, 0);
    end = std::min(end, offsetSize());
    if (begin >= end) {
        return 0;
    }
    Data& data = writable();
    GeoOffset removed = 0;
    for (GeoOffset p = begin >> kPageShift; (p << kPageShift) < end; ++p) {
        const GeoOffset pageBegin = p << kPageShift;
        const GeoOffset pageEnd = std::min(pageBegin + kPageSize, data.offsetSize);
        const GeoOffset lo = std::max(begin, pageBegin);
        const GeoOffset hi = std::min(end, pageEnd);
        if (lo == pageBegin && hi == pageEnd) {
            const GeoOffset live = liveCountInPage(p);
            if (live == 0) {
                continue;
            }
            if (data.pages.empty()) {
                data.pages.assign(std::size_t((data.offsetSize + kPageMask) >> kPageShift), nullptr);
            }
            data.pages[std::size_t(p)] = allDeadPage();
            data.liveCount -= live;
            removed += live;
        } else {
            removed += tombstoneInPage(data, p, lo - pageBegin, hi - pageBegin);
        }
    }
    return removed;
}

void
GeoIndexMap::clear()
{
    _data.reset();
}

GeoDefragmentResult
GeoIndexMap::defragment() const
{
    GeoDefragmentResult result;
    const GeoOffset oldSize = offsetSize();
    const GeoIndex liveCount = size();
    if (isTrivial()) {
        result.map = *this;
        std::vector<GeoIndexRemap::Run> runs;
        if (oldSize > 0) {
            runs.push_back({ 0, 0, oldSize });
        }
        result.remap = GeoIndexRemap(oldSize, oldSize, std::move(runs));
        return result;
    }
    std::vector<GeoIndexRemap::Run> runs;
    forEachLiveRun([&](GeoIndex index, GeoOffset offset, GeoOffset count) {
        runs.push_back({ index, offset, count });
    });
    result.map = GeoIndexMap(liveCount);
    result.remap = GeoIndexRemap(oldSize, liveCount, std::move(runs));
    return result;
}

NATRON_NAMESPACE_EXIT
