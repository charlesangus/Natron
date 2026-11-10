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

#ifndef Engine_Geo_GeoGroup_h
#define Engine_Geo_GeoGroup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoIndexMap.h"
#include "Engine/Geo/GeoPage.h"

NATRON_NAMESPACE_ENTER

// Group pages, attribute pages and index-map liveness pages must cover the same offsets, so a
// group page can be masked with a liveness page word for word.
static_assert(kGeoPageSize == static_cast<std::size_t>(GeoIndexMap::kPageSize));

/**
 * @brief Membership of point, vertex or primitive offsets, as a paged copy-on-write bitset.
 *
 * Membership is by storage offset, so deleting elements leaves the group untouched; bits of
 * tombstoned offsets are ignored by the live-aware queries and dropped by remapped(). Copying
 * is O(1); a write detaches the page table and only the page it touches. Absent pages are empty.
 * Const members may run concurrently; mutating members need exclusive access to this object.
 **/
class GeoElementGroup {
public:
    static constexpr int kWordsPerPage = GeoIndexMap::kWordsPerPage;

    struct Page {
        std::array<U64, kWordsPerPage> words {};
    };

    GeoElementGroup() = default;

    bool contains(GeoOffset offset) const;

    void add(GeoOffset offset);
    void remove(GeoOffset offset);
    void set(GeoOffset offset, bool member);
    void addRange(GeoOffset begin, GeoOffset end);
    void add(std::span<const GeoOffset> offsets);
    void clear();

    /** @brief True when no offset, live or not, is a member. */
    bool empty() const;

    GeoIndex countLive(const GeoIndexMap& map) const;

    /** @brief Number of page slots, including empty ones; pages past it are empty. */
    GeoOffset pageCount() const
    {
        return _pages ? GeoOffset(_pages->size()) : 0;
    }

    /** @brief nullptr for an empty page. */
    const Page* page(GeoOffset pageIndex) const;

    /** @brief f(index, offset) for every live member, in index order. */
    template <typename F>
    void forEachLiveMember(const GeoIndexMap& map, F&& f) const
    {
        const GeoOffset pages = std::min(pageCount(), map.pageCount());
        for (GeoOffset p = 0; p < pages; ++p) {
            const Page* members = page(p);
            if (!members) {
                continue;
            }
            const GeoIndexMap::LivePage* live = map.livePage(p);
            const GeoOffset pageBegin = p << GeoIndexMap::kPageShift;
            const GeoOffset existing = std::min(GeoIndexMap::kPageSize, map.offsetSize() - pageBegin);
            GeoIndex index = map.indexAtPageStart(p);
            for (int w = 0; w < kWordsPerPage && GeoOffset(w) * 64 < existing; ++w) {
                const U64 liveWord = live ? live->words[w] : existingMask(existing, w);
                U64 hits = members->words[w] & liveWord;
                while (hits) {
                    const int bit = std::countr_zero(hits);
                    const U64 below = bit == 0 ? 0 : (liveWord & (~U64(0) >> (64 - bit)));
                    f(index + std::popcount(below), pageBegin + GeoOffset(w) * 64 + bit);
                    hits &= hits - 1;
                }
                index += std::popcount(liveWord);
            }
        }
    }

    /** @brief The group after a defragment: bit i of the result is bit oldFromNew(i) here. */
    GeoElementGroup remapped(const GeoIndexRemap& remap) const;

    /** @brief Equal for equal members whatever the page table length. Never 0. */
    U64 contentHash() const;

    std::size_t getSizeInBytes() const;

private:
    typedef std::vector<std::shared_ptr<Page>> PageTable;

    static U64 existingMask(GeoOffset existing, int word)
    {
        const GeoOffset bits = existing - GeoOffset(word) * 64;

        return bits >= 64 ? ~U64(0) : ((U64(1) << bits) - 1);
    }

    std::shared_ptr<Page>& writableSlot(GeoOffset pageIndex);
    Page& writablePage(GeoOffset pageIndex);

    std::shared_ptr<PageTable> _pages;
};

/** @brief An edge between two points, stored with p0 <= p1 since group edges are undirected. */
struct GeoEdge {
    GeoOffset p0 = GeoIndexMap::kInvalidOffset;
    GeoOffset p1 = GeoIndexMap::kInvalidOffset;

    GeoEdge() = default;
    GeoEdge(GeoOffset a, GeoOffset b)
        : p0(std::min(a, b))
        , p1(std::max(a, b))
    {
    }

    auto operator<=>(const GeoEdge&) const = default;
};

/**
 * @brief A set of undirected edges, each a pair of point offsets, kept sorted. Copy-on-write as
 * a whole: copying is O(1) and the first write copies the edge list. Edges whose points were
 * deleted stay until the points are defragmented away.
 **/
class GeoEdgeGroup {
public:
    GeoEdgeGroup() = default;

    bool contains(GeoOffset a, GeoOffset b) const;

    /** @brief False when the edge was already a member. */
    bool add(GeoOffset a, GeoOffset b);
    void add(std::span<const GeoEdge> edges);
    bool remove(GeoOffset a, GeoOffset b);
    void clear();

    std::size_t size() const
    {
        return _edges ? _edges->size() : 0;
    }

    bool empty() const
    {
        return size() == 0;
    }

    /** @brief Sorted, without duplicates. */
    std::span<const GeoEdge> edges() const;

    /** @brief Edges whose two points are live. */
    std::size_t countLive(const GeoIndexMap& points) const;

    /** @brief Rewrites both ends through @p newFromOld; edges with an end mapping to -1 are dropped. */
    GeoEdgeGroup remappedPoints(std::span<const GeoOffset> newFromOld) const;

    /** @brief Never 0. */
    U64 contentHash() const;

    std::size_t getSizeInBytes() const;

private:
    std::vector<GeoEdge>& writableEdges();

    std::shared_ptr<std::vector<GeoEdge>> _edges;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoGroup_h
