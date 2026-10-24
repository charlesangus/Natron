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

#include "Serialization/Bundle/ZipStore.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
    constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50;
    constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
    constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50;

    constexpr std::size_t kLocalHeaderSize = 30;
    constexpr std::size_t kCentralHeaderSize = 46;
    constexpr std::size_t kEndOfCentralDirectorySize = 22;
    constexpr std::size_t kZip64LocatorSize = 20;

    // The id usdz uses for its alignment padding; any reader that does not know it skips it.
    constexpr std::uint16_t kAlignmentExtraId = 0x1986;
    constexpr std::uint16_t kZip64ExtraId = 0x0001;

    constexpr std::uint16_t kVersionNeededStored = 10;
    constexpr std::uint16_t kVersionMadeByUnix = (3 << 8) | 20;
    constexpr std::uint16_t kFlagEncrypted = 1 << 0;
    constexpr std::uint16_t kFlagStrongEncryption = 1 << 6;
    constexpr std::uint16_t kFlagUtf8Name = 1 << 11;
    constexpr std::uint16_t kMethodStored = 0;
    constexpr std::uint16_t kMethodDeflated = 8;

    // 1980-01-01 00:00, the earliest DOS date, so output never depends on the clock.
    constexpr std::uint16_t kDosTime = 0;
    constexpr std::uint16_t kDosDate = (1 << 5) | 1;

    constexpr std::uint32_t kRegularFileMode = 0100644;

    constexpr std::uint16_t kMax16 = 0xFFFF;
    constexpr std::uint32_t kMax32 = 0xFFFFFFFF;

    std::string
    methodName(std::uint16_t method)
    {
        switch (method) {
        case 12:
            return "bzip2 (method 12)";
        case 14:
            return "lzma (method 14)";
        case 93:
            return "zstd (method 93)";
        default:
            return "method " + std::to_string(method);
        }
    }

    struct AlignedFree {
        void operator()(std::byte* p) const
        {
            std::free(p);
        }
    };

    using AlignedBuffer = std::unique_ptr<std::byte, AlignedFree>;

    class ByteSink {
    public:
        void u16(std::uint16_t v)
        {
            _bytes.push_back(static_cast<unsigned char>(v & 0xFF));
            _bytes.push_back(static_cast<unsigned char>(v >> 8));
        }

        void u32(std::uint32_t v)
        {
            u16(static_cast<std::uint16_t>(v & 0xFFFF));
            u16(static_cast<std::uint16_t>(v >> 16));
        }

        void bytes(std::string_view s)
        {
            _bytes.insert(_bytes.end(), s.begin(), s.end());
        }

        void zeros(std::size_t n)
        {
            _bytes.insert(_bytes.end(), n, 0);
        }

        const unsigned char* data() const
        {
            return _bytes.data();
        }

        std::size_t size() const
        {
            return _bytes.size();
        }

    private:
        std::vector<unsigned char> _bytes;
    };

    std::uint16_t
    readU16(const std::byte* p)
    {
        return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8));
    }

    std::uint32_t
    readU32(const std::byte* p)
    {
        return static_cast<std::uint32_t>(readU16(p)) | (static_cast<std::uint32_t>(readU16(p + 2)) << 16);
    }

    bool
    isAscii(std::string_view s)
    {
        for (char c : s) {
            if (static_cast<unsigned char>(c) >= 0x80) {
                return false;
            }
        }

        return true;
    }

    // Returns an empty string when the name is acceptable, else why it is not. Names that could
    // escape an extraction directory are refused even though this reader never extracts, because
    // other tools will.
    std::string
    memberNameProblem(std::string_view name,
                      bool allowDirectory)
    {
        if (name.empty()) {
            return "empty member name";
        }
        if (name.find('\0') != std::string_view::npos) {
            return "member name contains a NUL byte";
        }
        if (name.find('\\') != std::string_view::npos) {
            return "member name contains a backslash";
        }
        if (name.front() == '/') {
            return "absolute member name";
        }
        if ((name.size() >= 2) && (name[1] == ':')) {
            return "member name starts with a drive letter";
        }
        std::string_view path = name;
        if (allowDirectory && (path.back() == '/')) {
            path.remove_suffix(1);
        }
        std::size_t start = 0;
        for (;;) {
            std::size_t slash = path.find('/', start);
            std::string_view component = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (component.empty()) {
                return "member name has an empty path component";
            }
            if ((component == ".") || (component == "..")) {
                return "member name has a '.' or '..' path component";
            }
            if (slash == std::string_view::npos) {
                break;
            }
            start = slash + 1;
        }

        return std::string();
    }

} // anon namespace

std::uint32_t
zipCrc32(std::span<const std::byte> data,
         std::uint32_t crc)
{
    return static_cast<std::uint32_t>(crc32_z(crc, reinterpret_cast<const Bytef*>(data.data()), data.size()));
}

ZipOutput::~ZipOutput() = default;

struct ZipReader::InflateCache {
    std::mutex mutex;
    std::vector<AlignedBuffer> buffers;
};

std::string
zipMemberNameProblem(std::string_view name)
{
    return memberNameProblem(name, false);
}

ZipWriter::ZipWriter(const std::string& path,
                     std::string_view mimeType,
                     std::string_view root)
    : _path(path)
    , _root(root.empty() ? std::string() : std::string(root) + '/')
{
    _file = std::fopen(path.c_str(), "wb");
    if (!_file) {
        throw ZipError("cannot create " + path + ": " + std::strerror(errno));
    }
    try {
        writeMember(std::string_view("mimetype"), std::as_bytes(std::span<const char>(mimeType.data(), mimeType.size())), false);
    } catch (...) {
        close();
        throw;
    }
}

ZipWriter::ZipWriter(ZipOutput& output,
                     std::string_view mimeType,
                     std::string_view root)
    : _path("<output>")
    , _root(root.empty() ? std::string() : std::string(root) + '/')
    , _output(&output)
{
    try {
        writeMember(std::string_view("mimetype"), std::as_bytes(std::span<const char>(mimeType.data(), mimeType.size())), false);
    } catch (...) {
        close();
        throw;
    }
}

ZipWriter::~ZipWriter()
{
    close();
}

void
ZipWriter::close()
{
    if (_file) {
        std::fclose(_file);
        _file = nullptr;
    }
    _output = nullptr;
}

void
ZipWriter::writeBytes(const void* data,
                      std::size_t size)
{
    if (_output) {
        if (size > 0) {
            try {
                _output->write(data, size);
            } catch (...) {
                close();
                throw;
            }
        }
        _offset += size;

        return;
    }
    if ((size > 0) && (std::fwrite(data, 1, size, _file) != size)) {
        int error = errno;
        close();
        throw ZipError("cannot write " + _path + ": " + std::strerror(error));
    }
    _offset += size;
}

void
ZipWriter::addMember(std::string_view name,
                     std::span<const std::byte> data)
{
    writeMember(name, data, true);
}

void
ZipWriter::writeMember(std::string_view relativeName,
                       std::span<const std::byte> data,
                       bool align)
{
    if (!_file && !_output) {
        throw ZipError("zip archive " + _path + " is closed");
    }
    const std::string fullName = _root + std::string(relativeName);
    const std::string_view name = fullName;
    std::string problem = memberNameProblem(name, false);
    if (!problem.empty()) {
        throw ZipError(problem + ": \"" + std::string(name) + "\"");
    }
    if (name.size() > kMax16) {
        throw ZipError("member name longer than 65535 bytes");
    }
    std::string key(name);
    if (_names.count(key)) {
        throw ZipError("duplicate member name \"" + key + "\"");
    }
    if (_entries.size() >= kMax16) {
        throw ZipError("more than 65534 members needs zip64, which is not supported");
    }
    if (data.size() >= kMax32) {
        throw ZipError("member \"" + key + "\" is 4 GiB or larger, which needs zip64 and is not supported");
    }
    if (_offset >= kMax32) {
        throw ZipError("archive exceeds 4 GiB, which needs zip64 and is not supported");
    }

    std::size_t extraSize = 0;
    if (align) {
        std::uint64_t unpaddedDataStart = _offset + kLocalHeaderSize + name.size() + 4;
        extraSize = 4 + (kZipMemberAlignment - unpaddedDataStart % kZipMemberAlignment) % kZipMemberAlignment;
    }

    Entry entry;
    entry.name = key;
    entry.crc = zipCrc32(data);
    entry.size = static_cast<std::uint32_t>(data.size());
    entry.localHeaderOffset = static_cast<std::uint32_t>(_offset);

    ByteSink header;
    header.u32(kLocalHeaderSignature);
    header.u16(kVersionNeededStored);
    header.u16(isAscii(name) ? 0 : kFlagUtf8Name);
    header.u16(kMethodStored);
    header.u16(kDosTime);
    header.u16(kDosDate);
    header.u32(entry.crc);
    header.u32(entry.size);
    header.u32(entry.size);
    header.u16(static_cast<std::uint16_t>(name.size()));
    header.u16(static_cast<std::uint16_t>(extraSize));
    header.bytes(name);
    if (extraSize > 0) {
        header.u16(kAlignmentExtraId);
        header.u16(static_cast<std::uint16_t>(extraSize - 4));
        header.zeros(extraSize - 4);
    }
    writeBytes(header.data(), header.size());
    writeBytes(data.data(), data.size());

    _names.emplace(key, _entries.size());
    _entries.push_back(std::move(entry));
}

void
ZipWriter::finish()
{
    if (!_file && !_output) {
        throw ZipError("zip archive " + _path + " is closed");
    }
    std::uint64_t directoryOffset = _offset;
    for (const Entry& entry : _entries) {
        ByteSink header;
        header.u32(kCentralHeaderSignature);
        header.u16(kVersionMadeByUnix);
        header.u16(kVersionNeededStored);
        header.u16(isAscii(entry.name) ? 0 : kFlagUtf8Name);
        header.u16(kMethodStored);
        header.u16(kDosTime);
        header.u16(kDosDate);
        header.u32(entry.crc);
        header.u32(entry.size);
        header.u32(entry.size);
        header.u16(static_cast<std::uint16_t>(entry.name.size()));
        header.u16(0);
        header.u16(0);
        header.u16(0);
        header.u16(0);
        header.u32(kRegularFileMode << 16);
        header.u32(entry.localHeaderOffset);
        header.bytes(entry.name);
        writeBytes(header.data(), header.size());
    }
    std::uint64_t directorySize = _offset - directoryOffset;
    if ((directoryOffset >= kMax32) || (directorySize >= kMax32) || (_offset >= kMax32)) {
        close();
        throw ZipError("archive exceeds 4 GiB, which needs zip64 and is not supported");
    }

    ByteSink end;
    end.u32(kEndOfCentralDirectorySignature);
    end.u16(0);
    end.u16(0);
    end.u16(static_cast<std::uint16_t>(_entries.size()));
    end.u16(static_cast<std::uint16_t>(_entries.size()));
    end.u32(static_cast<std::uint32_t>(directorySize));
    end.u32(static_cast<std::uint32_t>(directoryOffset));
    end.u16(0);
    writeBytes(end.data(), end.size());

    if (_output) {
        _output = nullptr;

        return;
    }
    bool flushed = std::fflush(_file) == 0;
    int flushErrno = errno;
    bool closed = std::fclose(_file) == 0;
    _file = nullptr;
    if (!flushed || !closed) {
        throw ZipError("cannot write " + _path + ": " + std::strerror(flushed ? errno : flushErrno));
    }
}

ZipReader::ZipReader(const std::string& path)
    : _origin(path)
    , _inflated(std::make_unique<InflateCache>())
{
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw ZipError("cannot open " + path + ": " + std::strerror(errno));
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        int error = errno;
        ::close(fd);
        throw ZipError("cannot stat " + path + ": " + std::strerror(error));
    }
    if (st.st_size <= 0) {
        ::close(fd);
        throw ZipError(path + " is empty, not a zip archive");
    }
    _size = static_cast<std::size_t>(st.st_size);
    void* mapping = ::mmap(nullptr, _size, PROT_READ, MAP_PRIVATE, fd, 0);
    int mapErrno = errno;
    ::close(fd);
    if (mapping == MAP_FAILED) {
        throw ZipError("cannot map " + path + ": " + std::strerror(mapErrno));
    }
    _mapping = mapping;
    _bytes = static_cast<const std::byte*>(mapping);
    try {
        parse();
    } catch (...) {
        unmap();
        throw;
    }
}

ZipReader::ZipReader(std::span<const std::byte> bytes)
    : _bytes(bytes.data())
    , _size(bytes.size())
    , _origin("<memory>")
    , _inflated(std::make_unique<InflateCache>())
{
    parse();
}

ZipReader::~ZipReader()
{
    _inflated.reset();
    unmap();
}

ZipReader::ZipReader(ZipReader&& other) noexcept
    : _bytes(std::exchange(other._bytes, nullptr))
    , _size(std::exchange(other._size, 0))
    , _mapping(std::exchange(other._mapping, nullptr))
    , _origin(std::move(other._origin))
    , _members(std::move(other._members))
    , _index(std::move(other._index))
    , _inflated(std::move(other._inflated))
{
}

ZipReader&
ZipReader::operator=(ZipReader&& other) noexcept
{
    if (this != &other) {
        unmap();
        _bytes = std::exchange(other._bytes, nullptr);
        _size = std::exchange(other._size, 0);
        _mapping = std::exchange(other._mapping, nullptr);
        _origin = std::move(other._origin);
        _members = std::move(other._members);
        _index = std::move(other._index);
        _inflated = std::move(other._inflated);
    }

    return *this;
}

void
ZipReader::unmap() noexcept
{
    if (_mapping) {
        ::munmap(_mapping, _size);
        _mapping = nullptr;
    }
}

void
ZipReader::parse()
{
    auto fail = [this](const std::string& why) {
        throw ZipError(_origin + ": " + why);
    };

    if (_size < kEndOfCentralDirectorySize) {
        fail("too small to be a zip archive");
    }

    std::size_t endOffset = 0;
    bool foundEnd = false;
    std::size_t lowest = (_size - kEndOfCentralDirectorySize > kMax16) ? _size - kEndOfCentralDirectorySize - kMax16 : 0;
    for (std::size_t pos = _size - kEndOfCentralDirectorySize;; --pos) {
        if ((readU32(_bytes + pos) == kEndOfCentralDirectorySignature) && (pos + kEndOfCentralDirectorySize + readU16(_bytes + pos + 20) == _size)) {
            endOffset = pos;
            foundEnd = true;
            break;
        }
        if (pos == lowest) {
            break;
        }
    }
    if (!foundEnd) {
        fail("no end of central directory record, not a zip archive or truncated");
    }

    const std::byte* end = _bytes + endOffset;
    std::uint16_t diskNumber = readU16(end + 4);
    std::uint16_t directoryDisk = readU16(end + 6);
    std::uint16_t entriesOnDisk = readU16(end + 8);
    std::uint16_t entryCount = readU16(end + 10);
    std::uint32_t directorySize = readU32(end + 12);
    std::uint32_t directoryOffset = readU32(end + 16);

    if ((entriesOnDisk == kMax16) || (entryCount == kMax16) || (directorySize == kMax32) || (directoryOffset == kMax32) || ((endOffset >= kZip64LocatorSize) && (readU32(end - kZip64LocatorSize) == kZip64LocatorSignature))) {
        fail("zip64 archives are not supported");
    }
    if ((diskNumber != 0) || (directoryDisk != 0) || (entriesOnDisk != entryCount)) {
        fail("multi-disk archives are not supported");
    }
    if (static_cast<std::uint64_t>(directoryOffset) + directorySize > endOffset) {
        fail("central directory lies outside the archive");
    }

    const std::size_t directoryEnd = static_cast<std::size_t>(directoryOffset) + directorySize;
    std::size_t pos = directoryOffset;
    _members.reserve(entryCount);
    for (std::uint32_t i = 0; i < entryCount; ++i) {
        if (directoryEnd - pos < kCentralHeaderSize) {
            fail("central directory is truncated");
        }
        const std::byte* header = _bytes + pos;
        if (readU32(header) != kCentralHeaderSignature) {
            fail("bad central directory entry signature");
        }
        std::uint16_t flags = readU16(header + 8);
        std::uint16_t method = readU16(header + 10);
        std::uint32_t crc = readU32(header + 16);
        std::uint32_t compressedSize = readU32(header + 20);
        std::uint32_t size = readU32(header + 24);
        std::size_t nameSize = readU16(header + 28);
        std::size_t extraSize = readU16(header + 30);
        std::size_t commentSize = readU16(header + 32);
        std::uint16_t startDisk = readU16(header + 34);
        std::uint32_t localOffset = readU32(header + 42);

        if (directoryEnd - pos - kCentralHeaderSize < nameSize + extraSize + commentSize) {
            fail("central directory entry overruns the directory");
        }
        std::string name(reinterpret_cast<const char*>(header + kCentralHeaderSize), nameSize);
        const std::string label = "member \"" + name + "\": ";

        if ((compressedSize == kMax32) || (size == kMax32) || (localOffset == kMax32) || (startDisk == kMax16)) {
            fail(label + "zip64 members are not supported");
        }
        const std::byte* extra = header + kCentralHeaderSize + nameSize;
        for (std::size_t e = 0; e < extraSize;) {
            if (extraSize - e < 4) {
                fail(label + "malformed extra field");
            }
            std::uint16_t id = readU16(extra + e);
            std::size_t blockSize = readU16(extra + e + 2);
            if (extraSize - e - 4 < blockSize) {
                fail(label + "malformed extra field");
            }
            if (id == kZip64ExtraId) {
                fail(label + "zip64 members are not supported");
            }
            e += 4 + blockSize;
        }
        if (flags & (kFlagEncrypted | kFlagStrongEncryption)) {
            fail(label + "encrypted members are not supported");
        }
        if ((method != kMethodStored) && (method != kMethodDeflated)) {
            fail(label + "compression " + methodName(method) + " is not supported, only stored and deflate");
        }
        if ((method == kMethodStored) && (compressedSize != size)) {
            fail(label + "stored member with differing compressed and uncompressed sizes");
        }
        if (startDisk != 0) {
            fail(label + "multi-disk archives are not supported");
        }
        std::string problem = memberNameProblem(name, true);
        if (!problem.empty()) {
            fail(label + problem);
        }

        if ((localOffset > directoryOffset) || (directoryOffset - localOffset < kLocalHeaderSize)) {
            fail(label + "local header lies outside the archive data");
        }
        const std::byte* local = _bytes + localOffset;
        if (readU32(local) != kLocalHeaderSignature) {
            fail(label + "bad local header signature");
        }
        if (readU16(local + 6) & (kFlagEncrypted | kFlagStrongEncryption)) {
            fail(label + "encrypted members are not supported");
        }
        if (readU16(local + 8) != method) {
            fail(label + "local header and central directory compression methods differ");
        }
        std::size_t localNameSize = readU16(local + 26);
        std::size_t localExtraSize = readU16(local + 28);
        std::uint64_t dataOffset = static_cast<std::uint64_t>(localOffset) + kLocalHeaderSize + localNameSize + localExtraSize;
        if ((dataOffset > directoryOffset) || (directoryOffset - dataOffset < compressedSize)) {
            fail(label + "member data lies outside the archive data");
        }
        if ((localNameSize != nameSize) || (std::memcmp(local + kLocalHeaderSize, name.data(), nameSize) != 0)) {
            fail(label + "local and central directory names differ");
        }

        pos += kCentralHeaderSize + nameSize + extraSize + commentSize;

        if (name.back() == '/') {
            if (size != 0) {
                fail(label + "directory entry with data");
            }
            continue;
        }
        if (_index.count(name)) {
            fail(label + "duplicate member name");
        }
        _index.emplace(name, _members.size());
        ZipMember member;
        member.name = std::move(name);
        member.dataOffset = dataOffset;
        member.size = size;
        member.compressedSize = compressedSize;
        member.method = method;
        member.crc32 = crc;
        _members.push_back(std::move(member));
    }
}

const ZipMember*
ZipReader::find(std::string_view name) const
{
    auto it = _index.find(std::string(name));

    return it == _index.end() ? nullptr : &_members[it->second];
}

std::span<const std::byte>
ZipReader::data(const ZipMember& member) const
{
    if (member.method == kMethodStored) {
        return std::span<const std::byte>(_bytes + member.dataOffset, member.size);
    }
    if (member.size == 0) {
        return {};
    }
    auto it = _index.find(member.name);
    if (it == _index.end()) {
        throw ZipError(_origin + ": no member \"" + member.name + "\"");
    }
    const std::size_t slot = it->second;
    const std::string label = _origin + ": member \"" + member.name + "\": ";

    std::lock_guard<std::mutex> lock(_inflated->mutex);
    if (_inflated->buffers.empty()) {
        _inflated->buffers.resize(_members.size());
    }
    AlignedBuffer& buffer = _inflated->buffers[slot];
    if (!buffer) {
        // One byte beyond the declared size lets a stream that is too long be told apart from
        // one that fits exactly.
        const std::size_t capacity = static_cast<std::size_t>(member.size) + 1;
        const std::size_t rounded = (capacity + kZipMemberAlignment - 1) / kZipMemberAlignment * kZipMemberAlignment;
        AlignedBuffer out(static_cast<std::byte*>(std::aligned_alloc(kZipMemberAlignment, rounded)));
        if (!out) {
            throw ZipError(label + "cannot allocate " + std::to_string(member.size) + " bytes");
        }

        z_stream strm {};
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
            throw ZipError(label + "cannot initialise inflate");
        }
        strm.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(_bytes + member.dataOffset));
        strm.avail_in = member.compressedSize;
        strm.next_out = reinterpret_cast<Bytef*>(out.get());
        strm.avail_out = static_cast<uInt>(capacity);
        int ret = inflate(&strm, Z_FINISH);
        const uLong produced = strm.total_out;
        inflateEnd(&strm);

        if (produced > member.size) {
            throw ZipError(label + "deflate stream is longer than its declared size of " + std::to_string(member.size) + " bytes");
        }
        if (ret == Z_BUF_ERROR) {
            throw ZipError(label + "deflate stream is truncated");
        }
        if (ret != Z_STREAM_END) {
            throw ZipError(label + "corrupt deflate stream");
        }
        if (produced != member.size) {
            throw ZipError(label + "deflate stream is shorter than its declared size of " + std::to_string(member.size) + " bytes");
        }
        if (zipCrc32(std::span<const std::byte>(out.get(), member.size)) != member.crc32) {
            throw ZipError(label + "CRC-32 mismatch");
        }
        buffer = std::move(out);
    }

    return std::span<const std::byte>(buffer.get(), member.size);
}

std::span<const std::byte>
ZipReader::data(std::string_view name) const
{
    const ZipMember* member = find(name);
    if (!member) {
        throw ZipError(_origin + ": no member \"" + std::string(name) + "\"");
    }

    return data(*member);
}

void
ZipReader::verify() const
{
    for (const ZipMember& member : _members) {
        if (zipCrc32(data(member)) != member.crc32) {
            throw ZipError(_origin + ": member \"" + member.name + "\": CRC-32 mismatch");
        }
    }
}

void
ZipReader::verify(std::string_view name) const
{
    std::span<const std::byte> bytes = data(name);
    if (zipCrc32(bytes) != find(name)->crc32) {
        throw ZipError(_origin + ": member \"" + std::string(name) + "\": CRC-32 mismatch");
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
