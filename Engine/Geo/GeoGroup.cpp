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

#include "GeoGroup.h"

#include <algorithm>
#include <stdexcept>

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr GeoOffset kShift = GeoIndexMap::kPageShift;
constexpr GeoOffset kMask = GeoIndexMap::kPageMask;

// Shared by every full page addRange() writes; it is never sole-owned, so it is never written.
const std::shared_ptr<GeoElementGroup::Page>&
fullPage()
{
    static const std::shared_ptr<GeoElementGroup::Page> page = [] {
        std::shared_ptr<GeoElementGroup::Page> p = std::make_shared<GeoElementGroup::Page>();
        p->words.fill(~U64(0));
        return p;
    }();

    return page;
}

bool
pageIsEmpty(const GeoElementGroup::Page& page)
{
    for (U64 w : page.words) {
        if (w) {
            return false;
        }
    }

    return true;
}

U64
bitsBelow(GeoOffset bit)
{
    return bit >= 64 ? ~U64(0) : ((U64(1) << bit) - 1);
}

U64
finalizeNonZero(const ContentHasher& hasher)
{
    const U64 h = hasher.finalize();

    return h ? h : 1;
}

void
checkOffsetArgument(GeoOffset offset)
{
    if (offset < 0) {
        throw std::out_of_range("GeoElementGroup: negative offset");
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GeoElementGroup

const GeoElementGroup::Page*
GeoElementGroup::page(GeoOffset pageIndex) const
{
    if (pageIndex < 0 || pageIndex >= pageCount()) {
        return nullptr;
    }

    return (*_pages)[std::size_t(pageIndex)].get();
}

bool
GeoElementGroup::contains(GeoOffset offset) const
{
    if (offset < 0) {
        return false;
    }
    const Page* p = page(offset >> kShift);
    if (!p) {
        return false;
    }
    const GeoOffset local = offset & kMask;

    return (p->words[std::size_t(local >> 6)] >> (local & 63)) & 1;
}

std::shared_ptr<GeoElementGroup::Page>&
GeoElementGroup::writableSlot(GeoOffset pageIndex)
{
    if (!_pages) {
        _pages = std::make_shared<PageTable>();
    } else if (!geoIsSoleOwner(_pages)) {
        _pages = std::make_shared<PageTable>(*_pages);
    }
    if (pageIndex >= GeoOffset(_pages->size())) {
        _pages->resize(std::size_t(pageIndex + 1));
    }

    return (*_pages)[std::size_t(pageIndex)];
}

GeoElementGroup::Page&
GeoElementGroup::writablePage(GeoOffset pageIndex)
{
    std::shared_ptr<Page>& slot = writableSlot(pageIndex);
    if (!slot) {
        slot = std::make_shared<Page>();
    } else if (!geoIsSoleOwner(slot)) {
        slot = std::make_shared<Page>(*slot);
    }

    return *slot;
}

void
GeoElementGroup::add(GeoOffset offset)
{
    checkOffsetArgument(offset);
    if (contains(offset)) {
        return;
    }
    const GeoOffset local = offset & kMask;
    writablePage(offset >> kShift).words[std::size_t(local >> 6)] |= U64(1) << (local & 63);
}

void
GeoElementGroup::remove(GeoOffset offset)
{
    if (!contains(offset)) {
        return;
    }
    const GeoOffset pageIndex = offset >> kShift;
    const GeoOffset local = offset & kMask;
    Page& p = writablePage(pageIndex);
    p.words[std::size_t(local >> 6)] &= ~(U64(1) << (local & 63));
    if (pageIsEmpty(p)) {
        (*_pages)[std::size_t(pageIndex)].reset();
    }
}

void
GeoElementGroup::set(GeoOffset offset,
                     bool member)
{
    if (member) {
        add(offset);
    } else {
        remove(offset);
    }
}

void
GeoElementGroup::addRange(GeoOffset begin,
                          GeoOffset end)
{
    checkOffsetArgument(begin);
    if (begin >= end) {
        return;
    }
    for (GeoOffset p = begin >> kShift; (p << kShift) < end; ++p) {
        const GeoOffset pageBegin = p << kShift;
        const GeoOffset lo = std::max(begin, pageBegin) - pageBegin;
        const GeoOffset hi = std::min(end, pageBegin + GeoIndexMap::kPageSize) - pageBegin;
        if (lo == 0 && hi == GeoIndexMap::kPageSize) {
            writableSlot(p) = fullPage();
            continue;
        }
        Page& page = writablePage(p);
        for (GeoOffset w = lo >> 6; w * 64 < hi; ++w) {
            const GeoOffset wlo = std::max(lo - w * 64, GeoOffset(0));
            const GeoOffset whi = std::min(hi - w * 64, GeoOffset(64));
            page.words[std::size_t(w)] |= bitsBelow(whi) & ~bitsBelow(wlo);
        }
    }
}

void
GeoElementGroup::add(std::span<const GeoOffset> offsets)
{
    for (GeoOffset offset : offsets) {
        add(offset);
    }
}

void
GeoElementGroup::clear()
{
    _pages.reset();
}

bool
GeoElementGroup::empty() const
{
    const GeoOffset n = pageCount();
    for (GeoOffset p = 0; p < n; ++p) {
        const Page* members = page(p);
        if (members && !pageIsEmpty(*members)) {
            return false;
        }
    }

    return true;
}

GeoIndex
GeoElementGroup::countLive(const GeoIndexMap& map) const
{
    GeoIndex count = 0;
    const GeoOffset pages = std::min(pageCount(), map.pageCount());
    for (GeoOffset p = 0; p < pages; ++p) {
        const Page* members = page(p);
        if (!members) {
            continue;
        }
        const GeoIndexMap::LivePage* live = map.livePage(p);
        const GeoOffset existing = std::min(GeoIndexMap::kPageSize, map.offsetSize() - (p << kShift));
        for (int w = 0; w < kWordsPerPage && GeoOffset(w) * 64 < existing; ++w) {
            const U64 liveWord = live ? live->words[w] : existingMask(existing, w);
            count += std::popcount(members->words[w] & liveWord);
        }
    }

    return count;
}

GeoElementGroup
GeoElementGroup::remapped(const GeoIndexRemap& remap) const
{
    GeoElementGroup out;
    const GeoOffset newSize = remap.newOffsetSize();
    const GeoOffset pages = (newSize + kMask) >> kShift;
    if (pages == 0 || pageCount() == 0) {
        return out;
    }
    out._pages = std::make_shared<PageTable>(std::size_t(pages));
    geoParallelFor(std::size_t(pages), [&](std::size_t np) {
        const GeoOffset newBegin = GeoOffset(np) << kShift;
        const GeoOffset count = std::min(GeoIndexMap::kPageSize, newSize - newBegin);
        const GeoOffset verbatim = remap.verbatimSourcePage(GeoOffset(np));
        if (verbatim >= 0) {
            const Page* source = page(verbatim);
            if (!source) {
                return;
            }
            bool tailClear = true;
            for (GeoOffset w = count >> 6; w < kWordsPerPage && tailClear; ++w) {
                const U64 keep = bitsBelow(std::max(count - w * 64, GeoOffset(0)));
                tailClear = (source->words[std::size_t(w)] & ~keep) == 0;
            }
            if (tailClear) {
                (*out._pages)[np] = (*_pages)[std::size_t(verbatim)];
                return;
            }
        }
        Page built;
        bool any = false;
        remap.forEachRunInNewRange(newBegin, newBegin + count, [&](GeoOffset newOffset, GeoOffset oldOffset, GeoOffset n) {
            for (GeoOffset i = 0; i < n; ++i) {
                if (contains(oldOffset + i)) {
                    const GeoOffset local = newOffset + i - newBegin;
                    built.words[std::size_t(local >> 6)] |= U64(1) << (local & 63);
                    any = true;
                }
            }
        });
        if (any) {
            (*out._pages)[np] = std::make_shared<Page>(built);
        }
    });

    return out;
}

U64
GeoElementGroup::contentHash() const
{
    ContentHasher hasher;
    const GeoOffset n = pageCount();
    for (GeoOffset p = 0; p < n; ++p) {
        const Page* members = page(p);
        if (!members || pageIsEmpty(*members)) {
            continue;
        }
        hasher.appendI64(p);
        hasher.appendBytes(members->words.data(), sizeof(members->words));
    }

    return finalizeNonZero(hasher);
}

std::size_t
GeoElementGroup::getSizeInBytes() const
{
    std::size_t bytes = std::size_t(pageCount()) * sizeof(std::shared_ptr<Page>);
    const GeoOffset n = pageCount();
    for (GeoOffset p = 0; p < n; ++p) {
        if (page(p)) {
            bytes += sizeof(Page);
        }
    }

    return bytes;
}

// ---------------------------------------------------------------------------------------------
// GeoEdgeGroup

std::span<const GeoEdge>
GeoEdgeGroup::edges() const
{
    if (!_edges) {
        return {};
    }

    return std::span<const GeoEdge>(_edges->data(), _edges->size());
}

bool
GeoEdgeGroup::contains(GeoOffset a,
                       GeoOffset b) const
{
    const std::span<const GeoEdge> all = edges();

    return std::binary_search(all.begin(), all.end(), GeoEdge(a, b));
}

std::vector<GeoEdge>&
GeoEdgeGroup::writableEdges()
{
    if (!_edges) {
        _edges = std::make_shared<std::vector<GeoEdge>>();
    } else if (!geoIsSoleOwner(_edges)) {
        _edges = std::make_shared<std::vector<GeoEdge>>(*_edges);
    }

    return *_edges;
}

bool
GeoEdgeGroup::add(GeoOffset a,
                  GeoOffset b)
{
    if (a < 0 || b < 0) {
        throw std::out_of_range("GeoEdgeGroup: negative point offset");
    }
    if (contains(a, b)) {
        return false;
    }
    const GeoEdge edge(a, b);
    std::vector<GeoEdge>& all = writableEdges();
    all.insert(std::lower_bound(all.begin(), all.end(), edge), edge);

    return true;
}

void
GeoEdgeGroup::add(std::span<const GeoEdge> edges)
{
    if (edges.empty()) {
        return;
    }
    std::vector<GeoEdge>& all = writableEdges();
    const std::size_t oldSize = all.size();
    all.reserve(oldSize + edges.size());
    for (const GeoEdge& e : edges) {
        if (e.p0 < 0 || e.p1 < 0) {
            all.resize(oldSize);
            throw std::out_of_range("GeoEdgeGroup: negative point offset");
        }
        all.push_back(GeoEdge(e.p0, e.p1));
    }
    std::sort(all.begin() + std::ptrdiff_t(oldSize), all.end());
    std::inplace_merge(all.begin(), all.begin() + std::ptrdiff_t(oldSize), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
}

bool
GeoEdgeGroup::remove(GeoOffset a,
                     GeoOffset b)
{
    if (!contains(a, b)) {
        return false;
    }
    std::vector<GeoEdge>& all = writableEdges();
    all.erase(std::lower_bound(all.begin(), all.end(), GeoEdge(a, b)));

    return true;
}

void
GeoEdgeGroup::clear()
{
    _edges.reset();
}

std::size_t
GeoEdgeGroup::countLive(const GeoIndexMap& points) const
{
    std::size_t count = 0;
    for (const GeoEdge& e : edges()) {
        if (points.isLive(e.p0) && points.isLive(e.p1)) {
            ++count;
        }
    }

    return count;
}

GeoEdgeGroup
GeoEdgeGroup::remappedPoints(std::span<const GeoOffset> newFromOld) const
{
    GeoEdgeGroup out;
    if (empty()) {
        return out;
    }
    const auto map = [&](GeoOffset p) {
        return p >= 0 && std::size_t(p) < newFromOld.size() ? newFromOld[std::size_t(p)] : GeoIndexMap::kInvalidOffset;
    };
    std::vector<GeoEdge> remappedEdges;
    remappedEdges.reserve(size());
    for (const GeoEdge& e : edges()) {
        const GeoOffset a = map(e.p0);
        const GeoOffset b = map(e.p1);
        if (a >= 0 && b >= 0) {
            remappedEdges.push_back(GeoEdge(a, b));
        }
    }
    std::sort(remappedEdges.begin(), remappedEdges.end());
    remappedEdges.erase(std::unique(remappedEdges.begin(), remappedEdges.end()), remappedEdges.end());
    if (!remappedEdges.empty()) {
        out._edges = std::make_shared<std::vector<GeoEdge>>(std::move(remappedEdges));
    }

    return out;
}

U64
GeoEdgeGroup::contentHash() const
{
    static_assert(sizeof(GeoEdge) == 2 * sizeof(GeoOffset), "GeoEdge is hashed as raw bytes");
    ContentHasher hasher;
    const std::span<const GeoEdge> all = edges();
    hasher.appendU64(all.size());
    if (!all.empty()) {
        hasher.appendBytes(all.data(), all.size_bytes());
    }

    return finalizeNonZero(hasher);
}

std::size_t
GeoEdgeGroup::getSizeInBytes() const
{
    return size() * sizeof(GeoEdge);
}

NATRON_NAMESPACE_EXIT
