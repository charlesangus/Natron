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

#ifndef Engine_Geo_GeoAttribute_h
#define Engine_Geo_GeoAttribute_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoPage.h"

NATRON_NAMESPACE_ENTER

enum GeoStorageEnum {
    eGeoStorageInt32 = 0,
    eGeoStorageInt64,
    eGeoStorageFloat32,
    eGeoStorageFloat64,
    eGeoStorageString,
};

enum GeoTypeInfoEnum {
    eGeoTypeInfoNone = 0,
    eGeoTypeInfoVector,
    eGeoTypeInfoNormal,
    eGeoTypeInfoPoint,
    eGeoTypeInfoColour,
    eGeoTypeInfoMatrix,
    eGeoTypeInfoQuaternion,
};

/** @brief Bytes of one tuple component; strings are stored as int32 string-table indices. */
std::size_t geoStorageComponentBytes(GeoStorageEnum storage);

template <typename T>
struct GeoStorageOf;
template <>
struct GeoStorageOf<std::int32_t> {
    static constexpr GeoStorageEnum value = eGeoStorageInt32;
};
template <>
struct GeoStorageOf<std::int64_t> {
    static constexpr GeoStorageEnum value = eGeoStorageInt64;
};
template <>
struct GeoStorageOf<float> {
    static constexpr GeoStorageEnum value = eGeoStorageFloat32;
};
template <>
struct GeoStorageOf<double> {
    static constexpr GeoStorageEnum value = eGeoStorageFloat64;
};

struct GeoAttributeDesc {
    std::string name;
    GeoStorageEnum storage = eGeoStorageFloat32;
    int tupleSize = 1;
    GeoTypeInfoEnum typeInfo = eGeoTypeInfoNone;
    bool isArray = false;

    bool operator==(const GeoAttributeDesc&) const = default;
};

/**
 * @brief Interned strings of one string attribute. Index 0 is always the empty string, which is
 * the default of every string attribute. Strings are never removed.
 **/
class GeoStringTable {
public:
    GeoStringTable();

    /** @brief -1 when absent. */
    std::int32_t find(std::string_view s) const;
    std::int32_t intern(std::string_view s);

    const std::string& at(std::int32_t index) const
    {
        return _strings.at(static_cast<std::size_t>(index));
    }

    std::size_t size() const noexcept
    {
        return _strings.size();
    }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>()(s);
        }
    };

    std::vector<std::string> _strings;
    std::unordered_map<std::string, std::int32_t, Hash, std::equal_to<>> _index;
};

template <typename T>
struct GeoPageView {
    std::size_t pageIndex;
    std::size_t firstOffset;
    std::size_t count;
    int tupleSize;
    bool constant;
    const T* values;

    const T* tuple(std::size_t indexInPage) const noexcept
    {
        return constant ? values : values + indexInPage * static_cast<std::size_t>(tupleSize);
    }
};

template <typename T>
struct GeoMutablePageView {
    std::size_t pageIndex;
    std::size_t firstOffset;
    std::size_t count;
    int tupleSize;
    T* values;

    T* tuple(std::size_t indexInPage) const noexcept
    {
        return values + indexInPage * static_cast<std::size_t>(tupleSize);
    }
};

/**
 * @brief One attribute column of a geometry detail: a name, a storage type, a tuple size of
 * 1-16, a type-info tag and an array flag, over paged copy-on-write storage (GeoPagedColumn).
 * Elements are addressed by storage offset; dense element numbers are the index maps' business.
 *
 * Typed access takes T matching the storage exactly (int32_t, int64_t, float, double). A string
 * attribute is also accessible as int32_t string-table indices through every typed member.
 *
 * Array attributes keep one (start, count) int64 pair per element into a paged pool of tuples.
 * setArray() always appends to the pool, because gathered() may leave several elements sharing
 * one pool range; compactArrayPool() reclaims the dead tail.
 *
 * Copying is O(1) and thread-safe against concurrent const use. The concurrency rules of
 * GeoPagedColumn apply, and pageForWriting() is the entry point for parallel per-page writers.
 **/
class GeoAttribute {
public:
    static constexpr int kMaxTupleSize = 16;

    GeoAttribute() = default;
    /** @brief Throws std::invalid_argument for a tuple size outside 1-16. */
    explicit GeoAttribute(GeoAttributeDesc desc, std::size_t size = 0);

    const GeoAttributeDesc& desc() const noexcept
    {
        return _desc;
    }

    const std::string& name() const noexcept
    {
        return _desc.name;
    }

    GeoStorageEnum storage() const noexcept
    {
        return _desc.storage;
    }

    int tupleSize() const noexcept
    {
        return _desc.tupleSize;
    }

    GeoTypeInfoEnum typeInfo() const noexcept
    {
        return _desc.typeInfo;
    }

    bool isArray() const noexcept
    {
        return _desc.isArray;
    }

    void setName(std::string name)
    {
        _desc.name = std::move(name);
    }

    void setTypeInfo(GeoTypeInfoEnum typeInfo) noexcept
    {
        _desc.typeInfo = typeInfo;
    }

    std::size_t size() const noexcept
    {
        return _values.size();
    }

    void resize(std::size_t size)
    {
        _values.resize(size);
    }

    /**
     * @brief Sets the value new elements take. Only allowed while the attribute is empty, since
     * untouched pages alias the default; throws std::logic_error otherwise. Not for strings or
     * arrays, whose default is always empty.
     **/
    template <typename T>
    void setDefault(std::span<const T> tuple)
    {
        checkScalarAccess<T>(false);
        checkTupleSpan(tuple.size());
        if (size() != 0) {
            throw std::logic_error("GeoAttribute::setDefault on a non-empty attribute");
        }
        _values = GeoPagedColumn(_values.elementBytes(), tuple.data(), 0);
    }

    template <typename T>
    void getDefault(std::span<T> out) const
    {
        checkScalarAccess<T>(false);
        checkTupleSpan(out.size());
        std::memcpy(out.data(), _values.defaultValue(), _values.elementBytes());
    }

    template <typename T>
    T get(std::size_t offset, int component = 0) const
    {
        checkScalarAccess<T>(false);
        checkComponent(component);
        T v;
        std::memcpy(&v, _values.element(checkedOffset(offset)) + component * sizeof(T), sizeof(T));

        return v;
    }

    template <typename T>
    void getTuple(std::size_t offset, std::span<T> out) const
    {
        checkScalarAccess<T>(false);
        checkTupleSpan(out.size());
        std::memcpy(out.data(), _values.element(checkedOffset(offset)), _values.elementBytes());
    }

    template <typename T>
    void set(std::size_t offset, T value, int component = 0)
    {
        checkScalarAccess<T>(false);
        checkComponent(component);
        checkStringIndices(&value, 1);
        const unsigned char* current = _values.element(checkedOffset(offset)) + component * sizeof(T);
        if (std::memcmp(current, &value, sizeof(T)) == 0) {
            return;
        }
        std::memcpy(_values.elementForWriting(offset) + component * sizeof(T), &value, sizeof(T));
    }

    template <typename T>
    void setTuple(std::size_t offset, std::span<const T> tuple)
    {
        checkScalarAccess<T>(false);
        checkTupleSpan(tuple.size());
        checkStringIndices(tuple.data(), tuple.size());
        _values.setElement(checkedOffset(offset), tuple.data());
    }

    std::string_view getString(std::size_t offset, int component = 0) const;
    void setString(std::size_t offset, std::string_view value, int component = 0);

    const GeoStringTable& stringTable() const;
    /** @brief Interns @p s in this attribute's table, detaching the table if it is shared. */
    std::int32_t internString(std::string_view s);

    /** @brief Number of tuples in the element's array. */
    std::size_t arrayLength(std::size_t offset) const;

    /** @brief Appends arrayLength() * tupleSize() components to a cleared @p out. */
    template <typename T>
    void getArray(std::size_t offset, std::vector<T>& out) const
    {
        checkScalarAccess<T>(true);
        const ArrayRange r = arrayRange(offset);
        out.resize(r.count * static_cast<std::size_t>(_desc.tupleSize));
        if (r.count) {
            _pool.copyOut(r.start, r.start + r.count, out.data());
        }
    }

    /** @brief @p values holds whole tuples, flattened; throws std::invalid_argument otherwise. */
    template <typename T>
    void setArray(std::size_t offset, std::span<const T> values)
    {
        checkScalarAccess<T>(true);
        checkStringIndices(values.data(), values.size());
        setArrayBytes(offset, values.data(), values.size());
    }

    void getStringArray(std::size_t offset, std::vector<std::string>& out) const;
    void setStringArray(std::size_t offset, std::span<const std::string> values);

    /** @brief Rewrites the pool so it holds only the ranges elements reference, in offset order. */
    void compactArrayPool();

    /** @brief Writes (end - begin) * tupleSize() components into @p out. */
    template <typename T>
    void copyDense(std::size_t begin, std::size_t end, std::span<T> out) const
    {
        checkScalarAccess<T>(false);
        checkDenseSpan(end >= begin ? end - begin : 0, out.size());
        _values.copyOut(begin, end, out.data());
    }

    template <typename T>
    void copyDense(std::span<const std::size_t> offsets, std::span<T> out) const
    {
        checkScalarAccess<T>(false);
        checkDenseSpan(offsets.size(), out.size());
        _values.copyOut(offsets, out.data());
    }

    /** @brief Inverse of copyDense(); pages whose content would not change stay shared. */
    template <typename T>
    void scatterDense(std::size_t begin, std::size_t end, std::span<const T> in)
    {
        checkScalarAccess<T>(false);
        checkDenseSpan(end >= begin ? end - begin : 0, in.size());
        checkStringIndices(in.data(), in.size());
        _values.scatterIn(begin, end, in.data());
    }

    template <typename T>
    void scatterDense(std::span<const std::size_t> offsets, std::span<const T> in)
    {
        checkScalarAccess<T>(false);
        checkDenseSpan(offsets.size(), in.size());
        checkStringIndices(in.data(), in.size());
        _values.scatterIn(offsets, in.data());
    }

    /** @brief Whole pages inside the range become one shared constant page. */
    template <typename T>
    void fill(std::size_t begin, std::size_t end, std::span<const T> tuple)
    {
        checkScalarAccess<T>(false);
        checkTupleSpan(tuple.size());
        checkStringIndices(tuple.data(), tuple.size());
        _values.fill(begin, end, tuple.data());
    }

    /** @brief Calls @p f with a GeoPageView<T> for every page, in offset order. */
    template <typename T, typename F>
    void visitPages(F&& f) const
    {
        checkScalarAccess<T>(false);
        const std::size_t n = _values.pageCount();
        for (std::size_t i = 0; i < n; ++i) {
            const GeoPage& page = *_values.page(i);
            const GeoPageView<T> view { i, i << kGeoPageBits, pageElementCount(i), _desc.tupleSize, page.isConstant(),
                                        reinterpret_cast<const T*>(page.data()) };
            f(view);
        }
    }

    /** @brief Like visitPages(), but detaches and expands every page first. */
    template <typename T, typename F>
    void visitPagesForWriting(F&& f)
    {
        checkScalarAccess<T>(false);
        const std::size_t n = _values.pageCount();
        for (std::size_t i = 0; i < n; ++i) {
            const GeoMutablePageView<T> view { i, i << kGeoPageBits, pageElementCount(i), _desc.tupleSize,
                                               reinterpret_cast<T*>(_values.pageForWriting(i)) };
            f(view);
        }
    }

    std::size_t pageCount() const noexcept
    {
        return _values.pageCount();
    }

    /** @brief Number of valid elements in the page (kGeoPageSize except possibly the last). */
    std::size_t pageElementCount(std::size_t pageIndex) const noexcept
    {
        const std::size_t first = pageIndex << kGeoPageBits;

        return std::min(kGeoPageSize, _values.size() - first);
    }

    /** @brief Call once before parallel pageForWriting() calls on distinct pages. */
    void makePageTableUnique()
    {
        _values.makePageTableUnique();
    }

    /** @brief kGeoPageSize tuples; elements past size() must keep the default value. */
    template <typename T>
    T* pageForWriting(std::size_t pageIndex)
    {
        checkScalarAccess<T>(false);

        return reinterpret_cast<T*>(_values.pageForWriting(pageIndex));
    }

    void compactConstantPages()
    {
        _values.compactConstantPages();
    }

    /**
     * @brief An attribute whose element i is this attribute's element sourceOffsets[i]. Pages
     * that map onto themselves stay shared; the string table and array pool are shared.
     **/
    GeoAttribute gathered(std::span<const std::size_t> sourceOffsets) const;

    /** @brief Per-element values, or the (start, count) pairs of an array attribute. */
    const GeoPagedColumn& values() const noexcept
    {
        return _values;
    }

    const GeoPagedColumn& arrayPool() const noexcept
    {
        return _pool;
    }

    /** @brief Covers the description, the pages, the string table and the array pool. Never 0. */
    U64 contentHash() const;

    std::size_t getSizeInBytes() const;

private:
    struct ArrayRange {
        std::size_t start;
        std::size_t count;
    };

    template <typename T>
    void checkScalarAccess(bool array) const
    {
        static_assert(std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, float> || std::is_same_v<T, double>,
                      "GeoAttribute stores int32_t, int64_t, float or double");
        const bool storageMatches = _desc.storage == GeoStorageOf<T>::value || (_desc.storage == eGeoStorageString && std::is_same_v<T, std::int32_t>);
        if (!storageMatches) {
            throwTypeMismatch();
        }
        checkArrayFlag(array);
    }

    template <typename T>
    void checkStringIndices(const T* values, std::size_t count) const
    {
        if constexpr (std::is_same_v<T, std::int32_t>) {
            if (_desc.storage == eGeoStorageString) {
                checkStringIndexValues(values, count);
            }
        }
    }

    [[noreturn]] void throwTypeMismatch() const;
    void checkArrayFlag(bool array) const;
    void checkComponent(int component) const;
    void checkTupleSpan(std::size_t count) const;
    void checkDenseSpan(std::size_t elements, std::size_t count) const;
    void checkStringIndexValues(const std::int32_t* values, std::size_t count) const;
    std::size_t checkedOffset(std::size_t offset) const;
    ArrayRange arrayRange(std::size_t offset) const;
    void setArrayBytes(std::size_t offset, const void* values, std::size_t componentCount);
    GeoStringTable& stringTableForWriting();

    GeoAttributeDesc _desc;
    GeoPagedColumn _values;
    GeoPagedColumn _pool;
    std::shared_ptr<GeoStringTable> _strings;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoAttribute_h
