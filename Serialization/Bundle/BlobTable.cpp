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

#include "Serialization/Bundle/BlobTable.h"

#include <limits>
#include <unordered_set>

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    constexpr char kMagic[4] = { 'N', 'T', 'B', 'L' };
    constexpr std::size_t kDirectoryEntryFixedSize = 16;

    std::size_t
    alignUp(std::size_t value,
            std::size_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    void
    putU16(std::byte* p,
           std::uint16_t v)
    {
        std::memcpy(p, &v, sizeof(v));
    }

    void
    putU64(std::byte* p,
           std::uint64_t v)
    {
        std::memcpy(p, &v, sizeof(v));
    }

    std::uint16_t
    getU16(const std::byte* p)
    {
        std::uint16_t v;
        std::memcpy(&v, p, sizeof(v));

        return v;
    }

    std::uint64_t
    getU64(const std::byte* p)
    {
        std::uint64_t v;
        std::memcpy(&v, p, sizeof(v));

        return v;
    }

    bool
    isValidDType(std::uint8_t value)
    {
        return (value >= static_cast<std::uint8_t>(BlobDType::eF64)) && (value <= static_cast<std::uint8_t>(BlobDType::eU8));
    }
} // anon namespace

std::size_t
blobDTypeSize(BlobDType type)
{
    switch (type) {
    case BlobDType::eF64:
        return 8;
    case BlobDType::eF32:
    case BlobDType::eI32:
        return 4;
    case BlobDType::eU8:
        return 1;
    }

    return 0;
}

std::string_view
blobDTypeName(BlobDType type)
{
    switch (type) {
    case BlobDType::eF64:
        return "f64";
    case BlobDType::eF32:
        return "f32";
    case BlobDType::eI32:
        return "i32";
    case BlobDType::eU8:
        return "u8";
    }

    return std::string_view();
}

std::optional<BlobDType>
blobDTypeFromName(std::string_view name)
{
    for (BlobDType type : { BlobDType::eF64, BlobDType::eF32, BlobDType::eI32, BlobDType::eU8 }) {
        if (blobDTypeName(type) == name) {
            return type;
        }
    }

    return std::nullopt;
}

BlobTableWriter::BlobTableWriter(std::uint64_t rows)
    : _rows(rows)
{
}

void
BlobTableWriter::addRawColumn(std::string_view name,
                              BlobDType type,
                              std::span<const std::byte> bytes,
                              std::size_t count)
{
    if (name.empty() || (name.size() > std::numeric_limits<std::uint16_t>::max())) {
        throw BlobTableError("blob table column name must be 1 to 65535 bytes long");
    }
    if (count != _rows) {
        throw BlobTableError("blob table column \"" + std::string(name) + "\" has " + std::to_string(count) + " values, the table has " + std::to_string(_rows) + " rows");
    }
    if (_columns.size() >= std::numeric_limits<std::uint16_t>::max()) {
        throw BlobTableError("blob table has too many columns");
    }
    for (const Column& column : _columns) {
        if (column.name == name) {
            throw BlobTableError("duplicate blob table column \"" + std::string(name) + "\"");
        }
    }
    _columns.push_back(Column { std::string(name), type, bytes });
}

std::vector<std::byte>
BlobTableWriter::serialize() const
{
    if (_columns.empty() && (_rows != 0)) {
        throw BlobTableError("a blob table with rows needs at least one column");
    }

    std::size_t directorySize = 0;
    for (const Column& column : _columns) {
        directorySize += alignUp(kDirectoryEntryFixedSize + column.name.size(), 8);
    }
    std::vector<std::size_t> offsets;
    offsets.reserve(_columns.size());
    std::size_t total = kBlobTableHeaderSize + directorySize;
    for (const Column& column : _columns) {
        total = alignUp(total, kBlobTableColumnAlignment);
        offsets.push_back(total);
        total += column.bytes.size();
    }

    std::vector<std::byte> out(total);
    std::memcpy(out.data(), kMagic, sizeof(kMagic));
    putU16(out.data() + 4, kBlobTableVersion);
    putU16(out.data() + 6, static_cast<std::uint16_t>(_columns.size()));
    putU64(out.data() + 8, _rows);
    putU64(out.data() + 16, kBlobTableHeaderSize);

    std::size_t entry = kBlobTableHeaderSize;
    for (std::size_t i = 0; i < _columns.size(); ++i) {
        const Column& column = _columns[i];
        putU64(out.data() + entry, offsets[i]);
        putU16(out.data() + entry + 8, static_cast<std::uint16_t>(column.name.size()));
        out[entry + 10] = static_cast<std::byte>(column.type);
        std::memcpy(out.data() + entry + kDirectoryEntryFixedSize, column.name.data(), column.name.size());
        entry += alignUp(kDirectoryEntryFixedSize + column.name.size(), 8);
        if (!column.bytes.empty()) {
            std::memcpy(out.data() + offsets[i], column.bytes.data(), column.bytes.size());
        }
    }

    return out;
}

BlobTableView::BlobTableView(std::span<const std::byte> blob)
    : _blob(blob)
{
    const std::size_t size = blob.size();
    const std::byte* bytes = blob.data();

    if (size < kBlobTableHeaderSize) {
        throw BlobTableError("blob table is smaller than its header");
    }
    if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
        throw BlobTableError("not a blob table: bad magic");
    }
    std::uint16_t version = getU16(bytes + 4);
    if (version != kBlobTableVersion) {
        throw BlobTableError("unsupported blob table version " + std::to_string(version));
    }
    std::uint16_t columnCount = getU16(bytes + 6);
    _rows = getU64(bytes + 8);
    std::uint64_t directoryOffset = getU64(bytes + 16);

    if ((columnCount == 0) && (_rows != 0)) {
        throw BlobTableError("blob table has rows but no columns");
    }
    if ((directoryOffset < kBlobTableHeaderSize) || (directoryOffset > size)) {
        throw BlobTableError("blob table column directory lies outside the blob");
    }

    std::unordered_set<std::string> names;
    _columns.reserve(columnCount);
    std::uint64_t entry = directoryOffset;
    for (std::uint16_t i = 0; i < columnCount; ++i) {
        if (size - entry < kDirectoryEntryFixedSize) {
            throw BlobTableError("blob table column directory is truncated");
        }
        std::uint64_t offset = getU64(bytes + entry);
        std::size_t nameSize = getU16(bytes + entry + 8);
        std::uint8_t rawType = std::to_integer<std::uint8_t>(bytes[entry + 10]);
        if ((nameSize == 0) || (size - entry - kDirectoryEntryFixedSize < nameSize)) {
            throw BlobTableError("blob table column name is empty or lies outside the blob");
        }
        std::string name(reinterpret_cast<const char*>(bytes + entry + kDirectoryEntryFixedSize), nameSize);
        if (!isValidDType(rawType)) {
            throw BlobTableError("blob table column \"" + name + "\" has unknown dtype " + std::to_string(rawType));
        }
        BlobDType type = static_cast<BlobDType>(rawType);
        std::size_t elementSize = blobDTypeSize(type);
        if ((offset > size) || (_rows > (size - offset) / elementSize)) {
            throw BlobTableError("blob table column \"" + name + "\" lies outside the blob");
        }
        if (!names.insert(name).second) {
            throw BlobTableError("duplicate blob table column \"" + name + "\"");
        }
        _columns.push_back(Column { std::move(name), type, offset });

        std::uint64_t entrySize = alignUp(kDirectoryEntryFixedSize + nameSize, 8);
        if (size - entry < entrySize) {
            entry = size;
        } else {
            entry += entrySize;
        }
    }
}

std::vector<std::string>
BlobTableView::columnNames() const
{
    std::vector<std::string> names;
    names.reserve(_columns.size());
    for (const Column& column : _columns) {
        names.push_back(column.name);
    }

    return names;
}

std::optional<BlobDType>
BlobTableView::columnType(std::string_view name) const
{
    const Column* column = findColumn(name);

    return column ? std::optional<BlobDType>(column->type) : std::nullopt;
}

const BlobTableView::Column*
BlobTableView::findColumn(std::string_view name) const
{
    for (const Column& column : _columns) {
        if (column.name == name) {
            return &column;
        }
    }

    return nullptr;
}

void
BlobTableView::checkType(const Column& column,
                         BlobDType requested) const
{
    if (column.type != requested) {
        throw BlobTableError("blob table column \"" + column.name + "\" is " + std::string(blobDTypeName(column.type)) + ", read as " + std::string(blobDTypeName(requested)));
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
