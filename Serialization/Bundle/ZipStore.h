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

#ifndef Serialization_Bundle_ZipStore_h
#define Serialization_Bundle_ZipStore_h

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A stored-only (method 0) zip container whose member data starts on a 64-byte boundary,
 * so a reader can map the file and hand out member bytes in place, typed and aligned.
 *
 * Only the classic 32-bit zip format is written or read: zip64, compression and encryption are
 * refused.
 **/

class ZipError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t kZipMemberAlignment = 64;
inline constexpr std::string_view kNatronProjectMimeType = "application/vnd.natron.project+zip";

std::uint32_t zipCrc32(std::span<const std::byte> data, std::uint32_t crc = 0);

class ZipWriter {
public:
    /**
     * @brief Creates (truncating) the archive at path and writes the leading `mimetype` member,
     * stored with no extra field so the mime type sits at a fixed offset for file sniffers.
     **/
    explicit ZipWriter(const std::string& path, std::string_view mimeType = kNatronProjectMimeType);
    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    /**
     * @brief Appends a member whose data starts on a kZipMemberAlignment boundary. Members are
     * laid out in call order, so the same calls always produce the same bytes.
     **/
    void addMember(std::string_view name, std::span<const std::byte> data);

    /**
     * @brief Writes the central directory and closes the file. Without it the archive on disk is
     * incomplete and unreadable.
     **/
    void finish();

private:
    struct Entry {
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
        std::uint32_t localHeaderOffset;
    };

    void writeMember(std::string_view name, std::span<const std::byte> data, bool align);
    void writeBytes(const void* data, std::size_t size);
    void close();

    std::string _path;
    std::FILE* _file = nullptr;
    std::uint64_t _offset = 0;
    std::vector<Entry> _entries;
    std::unordered_map<std::string, std::size_t> _names;
};

struct ZipMember {
    std::string name;
    std::uint64_t dataOffset = 0;
    std::uint32_t size = 0;
    std::uint32_t crc32 = 0;
};

class ZipReader {
public:
    /**
     * @brief Maps the archive at path read-only and validates its whole structure up front; every
     * malformed or unsupported archive throws ZipError here rather than on member access.
     *
     * The mapping is shared with the file, so truncating the file while the reader lives raises
     * SIGBUS on access: callers must not let the archive be rewritten in place while it is open.
     **/
    explicit ZipReader(const std::string& path);

    /**
     * @brief Reads an archive already in memory. The bytes are not copied and must outlive the
     * reader.
     **/
    explicit ZipReader(std::span<const std::byte> bytes);

    ~ZipReader();

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;
    ZipReader(ZipReader&& other) noexcept;
    ZipReader& operator=(ZipReader&& other) noexcept;

    const std::vector<ZipMember>& members() const
    {
        return _members;
    }

    const ZipMember* find(std::string_view name) const;

    bool contains(std::string_view name) const
    {
        return find(name) != nullptr;
    }

    std::span<const std::byte> data(const ZipMember& member) const;

    /**
     * @brief Throws ZipError if there is no member with that name.
     **/
    std::span<const std::byte> data(std::string_view name) const;

    /**
     * @brief Checks the CRC-32 of every member, or of one, throwing ZipError on a mismatch.
     * Opening an archive does not check CRCs so that mapping a large one stays cheap.
     **/
    void verify() const;
    void verify(std::string_view name) const;

private:
    void parse();
    void unmap() noexcept;

    const std::byte* _bytes = nullptr;
    std::size_t _size = 0;
    void* _mapping = nullptr;
    std::string _origin;
    std::vector<ZipMember> _members;
    std::unordered_map<std::string, std::size_t> _index;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Bundle_ZipStore_h
