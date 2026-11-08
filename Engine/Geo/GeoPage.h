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

#ifndef Engine_Geo_GeoPage_h
#define Engine_Geo_GeoPage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

inline constexpr unsigned kGeoPageBits = 10;
inline constexpr std::size_t kGeoPageSize = std::size_t(1) << kGeoPageBits;
inline constexpr std::size_t kGeoPageMask = kGeoPageSize - 1;

/**
 * @brief True when @p p is the only owner of its object, so the caller may mutate it in place.
 * The caller must own the shared_ptr exclusively (no other thread reads or copies it meanwhile).
 **/
template <typename T>
bool
geoIsSoleOwner(const std::shared_ptr<T>& p) noexcept
{
    if (!p || p.use_count() != 1) {
        return false;
    }
    // use_count() is a relaxed load. The former co-owners finished reading before their release
    // decrement; this fence makes the caller's coming writes happen after those reads.
    std::atomic_thread_fence(std::memory_order_acquire);

    return true;
}

/**
 * @brief One page of kGeoPageSize fixed-size elements of a paged column. A constant page stores
 * a single element that stands for all kGeoPageSize of them. Pages are immutable once shared:
 * a column writes into a page only while it is the page's sole owner.
 **/
class GeoPage {
    struct PrivateTag { };

public:
    GeoPage(PrivateTag, std::size_t elementBytes, bool constant);
    ~GeoPage();

    GeoPage(const GeoPage&) = delete;
    GeoPage& operator=(const GeoPage&) = delete;

    static std::shared_ptr<GeoPage> makeConstant(std::size_t elementBytes, const void* value);
    /** @brief A dense page with every element set to @p fillValue. */
    static std::shared_ptr<GeoPage> makeDense(std::size_t elementBytes, const void* fillValue);
    /** @brief A dense page whose content is left for the caller to write in full. */
    static std::shared_ptr<GeoPage> makeDenseUninitialized(std::size_t elementBytes);

    std::shared_ptr<GeoPage> denseCopy() const;

    bool isConstant() const noexcept
    {
        return _constant;
    }

    std::size_t elementBytes() const noexcept
    {
        return _elementBytes;
    }

    std::size_t storedBytes() const noexcept
    {
        return _constant ? _elementBytes : _elementBytes * kGeoPageSize;
    }

    const unsigned char* data() const noexcept
    {
        return _data;
    }

    const unsigned char* element(std::size_t indexInPage) const noexcept
    {
        return _constant ? _data : _data + indexInPage * _elementBytes;
    }

    /** @brief Invalidates the cached hash; only for the sole owner of a dense page. */
    unsigned char* mutableData() noexcept;

    /** @brief True when every element equals the first (always true for a constant page). */
    bool isUniform() const noexcept;

    /**
     * @brief Hash of the kGeoPageSize logical elements, cached. A constant page hashes equal to
     * a dense page holding the same value everywhere. Never 0.
     **/
    U64 contentHash() const;

private:
    std::size_t _elementBytes;
    bool _constant;
    unsigned char* _data;
    mutable std::atomic<U64> _hash;
};

typedef std::shared_ptr<const GeoPage> GeoPageConstPtr;

/**
 * @brief A copy-on-write column of fixed-size elements stored as a table of shared pages.
 *
 * Copying a column is O(1): the page table itself is shared and only duplicated (as a vector of
 * page pointers) on the first write. Writing an element then duplicates only its page. Every page
 * slot that was never written points at one shared constant page of the default value.
 *
 * Elements in the last page beyond size() always hold the default value, so growing never has
 * to clear them and page hashes depend on content alone.
 *
 * Const members may run concurrently. A mutating member needs exclusive access to this column
 * object, except pageForWriting(), which may run concurrently for distinct page indices once
 * makePageTableUnique() has run.
 **/
class GeoPagedColumn {
public:
    GeoPagedColumn() = default;
    GeoPagedColumn(std::size_t elementBytes, const void* defaultValue, std::size_t size = 0);

    std::size_t size() const noexcept
    {
        return _size;
    }

    std::size_t elementBytes() const noexcept
    {
        return _elementBytes;
    }

    std::size_t pageCount() const noexcept
    {
        return _pages ? _pages->size() : 0;
    }

    const unsigned char* defaultValue() const noexcept
    {
        return _defaultPage ? _defaultPage->data() : nullptr;
    }

    const GeoPageConstPtr& defaultPage() const noexcept
    {
        return _defaultPage;
    }

    const GeoPageConstPtr& page(std::size_t pageIndex) const
    {
        return (*_pages)[pageIndex];
    }

    void resize(std::size_t size);

    const unsigned char* element(std::size_t offset) const
    {
        return (*_pages)[offset >> kGeoPageBits]->element(offset & kGeoPageMask);
    }

    /** @brief Leaves the page shared when @p value already equals the stored element. */
    void setElement(std::size_t offset, const void* value);

    unsigned char* elementForWriting(std::size_t offset);

    void makePageTableUnique();

    /** @brief Detaches and expands the page if needed; returns its kGeoPageSize elements. */
    unsigned char* pageForWriting(std::size_t pageIndex);

    void fill(std::size_t begin, std::size_t end, const void* value);

    void copyOut(std::size_t begin, std::size_t end, void* out) const;
    void copyOut(std::span<const std::size_t> offsets, void* out) const;

    /** @brief Pages whose content would not change are left shared. */
    void scatterIn(std::size_t begin, std::size_t end, const void* in);
    void scatterIn(std::span<const std::size_t> offsets, const void* in);

    /** @brief Turns every uniform dense page back into a constant page. */
    void compactConstantPages();

    /**
     * @brief A column whose element i is this column's element sourceOffsets[i]. A destination
     * page whose elements all map onto themselves keeps sharing the source page.
     **/
    GeoPagedColumn gathered(std::span<const std::size_t> sourceOffsets) const;

    U64 contentHash() const;

    /** @brief Counts every page slot as if it owned its page, like DeepImage. */
    std::size_t getSizeInBytes() const;

private:
    typedef std::vector<GeoPageConstPtr> PageTable;

    /** @brief With @p preserveContent false, a fresh page is left uninitialised for the caller. */
    unsigned char* writablePage(std::size_t pageIndex, bool preserveContent);
    GeoPageConstPtr constantPageFor(const void* value) const;
    bool elementIsDefault(const GeoPage& page, std::size_t indexInPage) const;
    void resetTailToDefault();
    void checkRange(std::size_t begin, std::size_t end) const;
    void checkOffset(std::size_t offset) const;

    std::size_t _elementBytes = 0;
    std::size_t _size = 0;
    GeoPageConstPtr _defaultPage;
    std::shared_ptr<PageTable> _pages;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoPage_h
