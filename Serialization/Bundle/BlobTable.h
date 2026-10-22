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

#ifndef Serialization_Bundle_BlobTable_h
#define Serialization_Bundle_BlobTable_h

#include "Global/Macros.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A flat table of equally long, typed, named columns (structure of arrays), stored
 * little-endian with each column on a 64-byte boundary so a mapped blob can be read in place.
 *
 * Layout: a 64-byte header (magic "NTBL", u16 version, u16 column count, u64 rows, u64 offset of
 * the column directory, zero padding), the column directory, then the columns. Each directory
 * entry is u64 data offset, u16 name length, u8 dtype, 5 zero bytes and the name, padded to 8.
 *
 * Readers match columns by name: a column the reader expects but the blob lacks reads as a
 * default value, and a column the reader does not know is ignored.
 **/

static_assert(std::endian::native == std::endian::little, "blob tables are read in place, which assumes a little-endian host");

class BlobTableError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class BlobDType : std::uint8_t {
    eF64 = 1,
    eF32 = 2,
    eI32 = 3,
    eU8 = 4,
};

inline constexpr std::uint16_t kBlobTableVersion = 1;
inline constexpr std::size_t kBlobTableHeaderSize = 64;
inline constexpr std::size_t kBlobTableColumnAlignment = 64;

std::size_t blobDTypeSize(BlobDType type);
std::string_view blobDTypeName(BlobDType type);
std::optional<BlobDType> blobDTypeFromName(std::string_view name);

template <typename T>
struct BlobDTypeOf;
template <>
struct BlobDTypeOf<double> {
    static constexpr BlobDType value = BlobDType::eF64;
};
template <>
struct BlobDTypeOf<float> {
    static constexpr BlobDType value = BlobDType::eF32;
};
template <>
struct BlobDTypeOf<std::int32_t> {
    static constexpr BlobDType value = BlobDType::eI32;
};
template <>
struct BlobDTypeOf<std::uint8_t> {
    static constexpr BlobDType value = BlobDType::eU8;
};

template <typename T>
concept BlobElement = requires { BlobDTypeOf<T>::value; };

class BlobTableWriter {
public:
    explicit BlobTableWriter(std::uint64_t rows);

    /**
     * @brief Adds a column of exactly rows() values. The values are not copied: they must stay
     * alive and unchanged until serialize() returns.
     **/
    template <BlobElement T>
    void addColumn(std::string_view name,
                   std::span<const T> values)
    {
        addRawColumn(name, BlobDTypeOf<T>::value, std::as_bytes(values), values.size());
    }

    template <BlobElement T>
    void addColumn(std::string_view name,
                   const std::vector<T>& values)
    {
        addColumn(name, std::span<const T>(values));
    }

    std::uint64_t rows() const
    {
        return _rows;
    }

    std::vector<std::byte> serialize() const;

private:
    struct Column {
        std::string name;
        BlobDType type;
        std::span<const std::byte> bytes;
    };

    void addRawColumn(std::string_view name, BlobDType type, std::span<const std::byte> bytes, std::size_t count);

    std::uint64_t _rows;
    std::vector<Column> _columns;
};

/**
 * @brief One column read from a BlobTableView. Refers to the blob's bytes in place when they are
 * suitably aligned, and holds its own copy otherwise, or a run of defaults when the blob lacks
 * the column.
 **/
template <BlobElement T>
class BlobColumn {
public:
    BlobColumn() = default;

    std::span<const T> values() const
    {
        return _owned ? std::span<const T>(_storage) : _view;
    }

    std::size_t size() const
    {
        return values().size();
    }

    const T& operator[](std::size_t i) const
    {
        return values()[i];
    }

    bool present() const
    {
        return _present;
    }

    bool zeroCopy() const
    {
        return !_owned;
    }

private:
    friend class BlobTableView;

    std::span<const T> _view;
    std::vector<T> _storage;
    bool _owned = false;
    bool _present = false;
};

class BlobTableView {
public:
    /**
     * @brief Validates the whole table up front; a malformed blob throws BlobTableError here.
     * The bytes are not copied and must outlive the view and every zero-copy column read from it.
     **/
    explicit BlobTableView(std::span<const std::byte> blob);

    std::uint64_t rows() const
    {
        return _rows;
    }

    std::vector<std::string> columnNames() const;
    std::optional<BlobDType> columnType(std::string_view name) const;

    bool hasColumn(std::string_view name) const
    {
        return columnType(name).has_value();
    }

    /**
     * @brief Reads the named column, or rows() copies of defaultValue when the blob has no such
     * column. A column stored with a different dtype throws BlobTableError rather than being
     * converted, since that means the writer and reader disagree on the schema.
     **/
    template <BlobElement T>
    BlobColumn<T> column(std::string_view name,
                         T defaultValue = T {}) const
    {
        BlobColumn<T> result;
        const Column* entry = findColumn(name);
        if (!entry) {
            result._storage.assign(_rows, defaultValue);
            result._owned = true;

            return result;
        }
        checkType(*entry, BlobDTypeOf<T>::value);
        result._present = true;
        const std::byte* data = _blob.data() + entry->offset;
        if (reinterpret_cast<std::uintptr_t>(data) % alignof(T) == 0) {
            result._view = std::span<const T>(reinterpret_cast<const T*>(data), _rows);
        } else {
            result._storage.resize(_rows);
            if (_rows > 0) {
                std::memcpy(result._storage.data(), data, _rows * sizeof(T));
            }
            result._owned = true;
        }

        return result;
    }

private:
    struct Column {
        std::string name;
        BlobDType type;
        std::uint64_t offset;
    };

    const Column* findColumn(std::string_view name) const;
    void checkType(const Column& column, BlobDType requested) const;

    std::span<const std::byte> _blob;
    std::uint64_t _rows = 0;
    std::vector<Column> _columns;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Bundle_BlobTable_h
