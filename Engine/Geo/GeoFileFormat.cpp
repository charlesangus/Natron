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

#include "GeoFileFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_map>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zstd.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "Engine/Geo/GeoPacked.h"

NATRON_NAMESPACE_ENTER

namespace {

static_assert(std::endian::native == std::endian::little, "the geometry file format is little-endian");

constexpr unsigned char kMagic[8] = { 'N', 'G', 'E', 'O', '\r', '\n', 0x1a, '\n' };
constexpr std::size_t kHeaderBytes = 8 + 4 + 4 + 8 + 8;
constexpr std::uint32_t kTagDetail = 0x4c544544; // "DETL"
constexpr std::uint32_t kTagDetailEnd = 0x444e4544; // "DEND"
constexpr std::size_t kGroupPageBytes = sizeof(GeoElementGroup::Page::words);
constexpr std::size_t kEncodeBatchPages = 256;

static_assert(sizeof(GeoEdge) == 2 * sizeof(GeoOffset) && std::is_trivially_copyable_v<GeoEdge>);

enum PageKindEnum : std::uint8_t {
    ePageConstant = 0,
    ePageRaw = 1,
    ePageZstd = 2,
};

// Smallest page record: kind byte plus stored length. Constant and raw pages also carry an XXH64
// of their stored bytes; zstd pages rely on the frame's own checksum.
constexpr std::size_t kMinPageRecordBytes = 1 + 8;

// A compression level that stores every non-uniform page raw.
constexpr int kNoCompression = -1000000;

bool
pageHasChecksum(PageKindEnum kind)
{
    return kind != ePageZstd;
}

std::uint64_t
pageChecksum(const unsigned char* data,
             std::size_t bytes)
{
    return XXH64(data, bytes, 0);
}

std::size_t
pageCountFor(std::size_t size)
{
    return (size + kGeoPageMask) >> kGeoPageBits;
}

std::size_t
pageElementCount(std::size_t size,
                 std::size_t pageIndex)
{
    return std::min(kGeoPageSize, size - (pageIndex << kGeoPageBits));
}

template <typename F>
void
visitStorage(GeoStorageEnum storage,
             F&& f)
{
    switch (storage) {
    case eGeoStorageInt32:
    case eGeoStorageString:
        f(std::int32_t());
        return;
    case eGeoStorageInt64:
        f(std::int64_t());
        return;
    case eGeoStorageFloat32:
        f(float());
        return;
    case eGeoStorageFloat64:
        f(double());
        return;
    }
    throw std::invalid_argument("unknown attribute storage");
}

struct CCtxDeleter {
    void operator()(ZSTD_CCtx* c) const noexcept
    {
        ZSTD_freeCCtx(c);
    }
};

struct DCtxDeleter {
    void operator()(ZSTD_DCtx* c) const noexcept
    {
        ZSTD_freeDCtx(c);
    }
};

ZSTD_CCtx*
threadCCtx()
{
    thread_local std::unique_ptr<ZSTD_CCtx, CCtxDeleter> ctx(ZSTD_createCCtx());
    if (!ctx) {
        throw std::bad_alloc();
    }

    return ctx.get();
}

ZSTD_DCtx*
threadDCtx()
{
    thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> ctx(ZSTD_createDCtx());
    if (!ctx) {
        throw std::bad_alloc();
    }

    return ctx.get();
}

class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const void* data, std::size_t bytes) = 0;

    void u8(std::uint8_t v) { write(&v, sizeof(v)); }
    void u32(std::uint32_t v) { write(&v, sizeof(v)); }
    void u64(std::uint64_t v) { write(&v, sizeof(v)); }
    void i64(std::int64_t v) { write(&v, sizeof(v)); }

    void string(std::string_view s)
    {
        u64(s.size());
        write(s.data(), s.size());
    }
};

class VectorSink : public Sink {
public:
    explicit VectorSink(std::vector<unsigned char>& out)
        : _out(out)
    {
    }

    void write(const void* data, std::size_t bytes) override
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        _out.insert(_out.end(), p, p + bytes);
    }

private:
    std::vector<unsigned char>& _out;
};

std::string
osError(const std::string& path,
        const char* action,
        int err)
{
    return "Cannot " + std::string(action) + " geometry file '" + path + "': " + std::strerror(err);
}

class FileSink : public Sink {
public:
    FileSink(std::FILE* file,
             const std::string& path)
        : _file(file)
        , _path(path)
    {
    }

    void write(const void* data, std::size_t bytes) override
    {
        if (bytes && std::fwrite(data, 1, bytes, _file) != bytes) {
            throw GeoFileError(osError(_path, "write", errno ? errno : EIO));
        }
    }

private:
    std::FILE* _file;
    const std::string& _path;
};

struct PageRef {
    const unsigned char* data;
    bool constant;
};

typedef std::function<PageRef(std::size_t)> PageSource;

struct EncodedPage {
    PageKindEnum kind = ePageRaw;
    const unsigned char* data = nullptr;
    std::size_t bytes = 0;
    std::uint64_t checksum = 0;
    std::vector<unsigned char> compressed;
};

void
encodePage(EncodedPage& out,
           const PageRef& ref,
           std::size_t elementBytes,
           std::size_t count,
           int level)
{
    bool uniform = true;
    for (std::size_t i = 1; !ref.constant && i < count && uniform; ++i) {
        uniform = std::memcmp(ref.data, ref.data + i * elementBytes, elementBytes) == 0;
    }
    if (uniform) {
        out.kind = ePageConstant;
        out.data = ref.data;
        out.bytes = elementBytes;
        out.checksum = pageChecksum(out.data, out.bytes);
        return;
    }

    const std::size_t rawBytes = count * elementBytes;
    out.kind = ePageRaw;
    out.data = ref.data;
    out.bytes = rawBytes;
    if (level == kNoCompression) {
        out.checksum = pageChecksum(out.data, out.bytes);
        return;
    }

    ZSTD_CCtx* ctx = threadCCtx();
    ZSTD_CCtx_reset(ctx, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_checksumFlag, 1);
    out.compressed.resize(ZSTD_compressBound(rawBytes));
    const std::size_t n = ZSTD_compress2(ctx, out.compressed.data(), out.compressed.size(), ref.data, rawBytes);
    if (!ZSTD_isError(n) && n < rawBytes) {
        out.compressed.resize(n);
        out.kind = ePageZstd;
        out.data = out.compressed.data();
        out.bytes = n;
    } else {
        out.compressed.clear();
        out.checksum = pageChecksum(out.data, out.bytes);
    }
}

void
writeColumn(Sink& sink,
            std::size_t elementBytes,
            std::size_t size,
            const PageSource& page,
            int level)
{
    sink.u32(std::uint32_t(elementBytes));
    sink.u64(size);
    const std::size_t pages = pageCountFor(size);
    std::vector<EncodedPage> batch;
    for (std::size_t first = 0; first < pages; first += kEncodeBatchPages) {
        const std::size_t n = std::min(kEncodeBatchPages, pages - first);
        batch.assign(n, EncodedPage());
        geoParallelFor(n, [&](std::size_t i) {
            const std::size_t pi = first + i;
            encodePage(batch[i], page(pi), elementBytes, pageElementCount(size, pi), level); }, 2);
        for (const EncodedPage& e : batch) {
            sink.u8(e.kind);
            sink.u64(e.bytes);
            if (pageHasChecksum(e.kind)) {
                sink.u64(e.checksum);
            }
            sink.write(e.data, e.bytes);
        }
    }
}

void
writePagedColumn(Sink& sink,
                 const GeoPagedColumn& column,
                 int level)
{
    writeColumn(sink, column.elementBytes(), column.size(), [&](std::size_t pi) {
        const GeoPage& p = *column.page(pi);
        return PageRef { p.data(), p.isConstant() }; }, level);
}

void
writeBufferColumn(Sink& sink,
                  std::size_t elementBytes,
                  std::size_t size,
                  const unsigned char* data,
                  int level)
{
    writeColumn(sink, elementBytes, size, [&](std::size_t pi) { return PageRef { data + (pi << kGeoPageBits) * elementBytes, false }; }, level);
}

struct ArrayPairBytes {
    std::int64_t start;
    std::int64_t count;
};

ArrayPairBytes
arrayPair(const GeoAttribute& a,
          std::size_t offset)
{
    ArrayPairBytes pair;
    std::memcpy(&pair, a.values().element(offset), sizeof(pair));

    return pair;
}

bool
arrayPoolIsCompact(const GeoAttribute& a)
{
    std::int64_t live = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const ArrayPairBytes pair = arrayPair(a, i);
        if (pair.count && pair.start != live) {
            return false;
        }
        live += pair.count;
    }

    return std::size_t(live) == a.arrayPool().size();
}

bool
hasTombstones(const GeoDetail& d)
{
    return d.pointMap().tombstoneCount() || d.vertexMap().tombstoneCount() || d.primitiveMap().tombstoneCount();
}

bool
hasSparseArrays(const GeoDetail& d)
{
    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        for (std::size_t i = 0; i < d.attributeCount(GeoAttribClassEnum(cls)); ++i) {
            const GeoAttribute& a = d.attribute(GeoAttribClassEnum(cls), i);
            if (a.isArray() && !arrayPoolIsCompact(a)) {
                return true;
            }
        }
    }

    return false;
}

GeoDetail
normalizedImpl(const GeoDetail& d,
               bool& changed)
{
    std::vector<GeoDetailConstPtr> children;
    bool childChanged = false;
    for (const GeoDetailConstPtr& shared : d.sharedDetails()) {
        bool c = false;
        GeoDetail n = normalizedImpl(*shared, c);
        if (c) {
            childChanged = true;
            children.push_back(std::make_shared<const GeoDetail>(std::move(n)));
        } else {
            children.push_back(shared);
        }
    }

    const bool tombstones = hasTombstones(d);
    if (!tombstones && !childChanged && !hasSparseArrays(d)) {
        changed = false;
        return d;
    }
    changed = true;

    GeoDetailBuilder b(d);
    if (tombstones) {
        b.defragment();
    }
    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        std::vector<std::string> arrays;
        for (std::size_t i = 0; i < b.detail().attributeCount(GeoAttribClassEnum(cls)); ++i) {
            const GeoAttribute& a = b.detail().attribute(GeoAttribClassEnum(cls), i);
            if (a.isArray() && !arrayPoolIsCompact(a)) {
                arrays.push_back(a.name());
            }
        }
        for (const std::string& name : arrays) {
            b.attributeForWriting(GeoAttribClassEnum(cls), name)->compactArrayPool();
        }
    }
    if (childChanged) {
        // Fresh pointers are always appended after the old entries, which finish() then prunes,
        // so the table keeps its order.
        std::vector<std::int32_t> newIndex(children.size());
        for (std::size_t i = 0; i < children.size(); ++i) {
            newIndex[i] = b.addSharedDetail(std::make_shared<const GeoDetail>(*children[i]));
        }
        GeoAttribute* refs = b.attributeForWriting(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
        if (refs) {
            const GeoDetail& cur = b.detail();
            std::vector<GeoOffset> packed;
            cur.primitiveMap().forEachLive([&](GeoIndex, GeoOffset offset) {
                if (cur.primitiveType(offset) == eGeoPrimitivePackedGeometry) {
                    packed.push_back(offset);
                }
            });
            for (const GeoOffset offset : packed) {
                const std::int32_t ref = refs->get<std::int32_t>(std::size_t(offset)) - 1;
                if (ref >= 0 && std::size_t(ref) < children.size()) {
                    refs->set<std::int32_t>(std::size_t(offset), newIndex[std::size_t(ref)] + 1);
                }
            }
        }
    }

    return b.finish();
}

void
checkVertexOrder(const GeoDetail& d)
{
    const GeoTopology& topo = d.topology();
    GeoOffset next = 0;
    for (GeoOffset p = 0; p < GeoOffset(d.primitiveCount()); ++p) {
        const GeoVertexRange r = topo.primitiveVertices(p);
        if (r.count > 0 && r.start != next) {
            throw GeoFileError("Cannot write geometry: primitive vertices are not in primitive order");
        }
        next += r.count;
    }
    if (next != GeoOffset(d.vertexCount())) {
        throw GeoFileError("Cannot write geometry: vertices not owned by a primitive");
    }
}

void
writeAttribute(Sink& sink,
               const GeoAttribute& a,
               int level)
{
    sink.string(a.name());
    sink.u8(std::uint8_t(a.storage()));
    sink.u8(std::uint8_t(a.tupleSize()));
    sink.u8(std::uint8_t(a.typeInfo()));
    sink.u8(a.isArray() ? 1 : 0);

    if (!a.isArray()) {
        const std::size_t eb = a.values().elementBytes();
        sink.u32(std::uint32_t(eb));
        std::vector<unsigned char> zeros;
        const unsigned char* def = a.values().defaultValue();
        if (!def) {
            zeros.assign(eb, 0);
            def = zeros.data();
        }
        sink.write(def, eb);
    }
    if (a.storage() == eGeoStorageString) {
        const GeoStringTable& table = a.stringTable();
        sink.u64(table.size());
        for (std::size_t i = 1; i < table.size(); ++i) {
            sink.string(table.at(std::int32_t(i)));
        }
    }
    if (a.isArray()) {
        std::vector<std::int64_t> lengths(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            lengths[i] = arrayPair(a, i).count;
        }
        writeBufferColumn(sink, sizeof(std::int64_t), lengths.size(), reinterpret_cast<const unsigned char*>(lengths.data()), level);
        writePagedColumn(sink, a.arrayPool(), level);
    } else {
        writePagedColumn(sink, a.values(), level);
    }
}

void
writeDetail(Sink& sink,
            const GeoDetail& d,
            const std::vector<std::uint64_t>& sharedIds,
            int level)
{
    checkVertexOrder(d);
    sink.u32(kTagDetail);
    sink.u8(d.pointPrecision64() ? 1 : 0);
    sink.i64(d.pointCount());
    sink.i64(d.vertexCount());
    sink.i64(d.primitiveCount());
    sink.u64(sharedIds.size());
    for (const std::uint64_t id : sharedIds) {
        sink.u64(id);
    }

    const GeoTopology& topo = d.topology();
    writePagedColumn(sink, topo.primitiveTypeColumn().values(), level);
    writePagedColumn(sink, topo.primitiveVertexCountColumn().values(), level);
    writePagedColumn(sink, topo.vertexPointColumn().values(), level);

    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        const std::size_t n = d.attributeCount(GeoAttribClassEnum(cls));
        sink.u64(n);
        for (std::size_t i = 0; i < n; ++i) {
            writeAttribute(sink, d.attribute(GeoAttribClassEnum(cls), i), level);
        }
    }

    for (int cls = 0; cls < 3; ++cls) {
        const std::vector<std::string> names = d.groupNames(GeoAttribClassEnum(cls));
        sink.u64(names.size());
        for (const std::string& name : names) {
            const GeoElementGroup* g = d.findGroup(GeoAttribClassEnum(cls), name);
            const std::size_t pages = std::size_t(g->pageCount());
            std::vector<unsigned char> bits(pages * kGroupPageBytes, 0);
            for (std::size_t p = 0; p < pages; ++p) {
                if (const GeoElementGroup::Page* page = g->page(GeoOffset(p))) {
                    std::memcpy(bits.data() + p * kGroupPageBytes, page->words.data(), kGroupPageBytes);
                }
            }
            sink.string(name);
            writeBufferColumn(sink, kGroupPageBytes, pages, bits.data(), level);
        }
    }

    const std::vector<std::string> edgeNames = d.edgeGroupNames();
    sink.u64(edgeNames.size());
    for (const std::string& name : edgeNames) {
        const std::span<const GeoEdge> edges = d.findEdgeGroup(name)->edges();
        sink.string(name);
        writeBufferColumn(sink, sizeof(GeoEdge), edges.size(), reinterpret_cast<const unsigned char*>(edges.data()), level);
    }
    sink.u32(kTagDetailEnd);
}

std::vector<unsigned char>
uncompressedRecord(const GeoDetail& d)
{
    std::vector<unsigned char> bytes;
    VectorSink sink(bytes);
    writeDetail(sink, d, std::vector<std::uint64_t>(), kNoCompression);

    return bytes;
}

// Equal content hashes are only a hint: two details share a record only once their shared details
// match in order and their records match byte for byte.
bool
sameContent(const GeoDetail& a,
            const GeoDetail& b)
{
    if (&a == &b) {
        return true;
    }
    const auto& sa = a.sharedDetails();
    const auto& sb = b.sharedDetails();
    if (sa.size() != sb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < sa.size(); ++i) {
        if (!sameContent(*sa[i], *sb[i])) {
            return false;
        }
    }

    return uncompressedRecord(a) == uncompressedRecord(b);
}

struct RecordPlan {
    std::vector<const GeoDetail*> details;
    std::vector<std::vector<std::uint64_t>> sharedIds;
    std::unordered_multimap<U64, std::uint64_t> idsByHash;
};

std::uint64_t
planRecord(RecordPlan& plan,
           const GeoDetail& d,
           bool isRoot)
{
    if (!isRoot) {
        const auto range = plan.idsByHash.equal_range(d.contentHash());
        for (auto it = range.first; it != range.second; ++it) {
            if (sameContent(*plan.details[it->second], d)) {
                return it->second;
            }
        }
    }
    std::vector<std::uint64_t> ids;
    for (const GeoDetailConstPtr& shared : d.sharedDetails()) {
        ids.push_back(planRecord(plan, *shared, false));
    }
    const std::uint64_t id = plan.details.size();
    plan.details.push_back(&d);
    plan.sharedIds.push_back(std::move(ids));
    if (!isRoot) {
        plan.idsByHash.emplace(d.contentHash(), id);
    }

    return id;
}

void
encodeTo(Sink& sink,
         const GeoDetail& detail,
         const GeoFileWriteOptions& options)
{
    const GeoDetail root = geoFileNormalized(detail);
    RecordPlan plan;
    planRecord(plan, root, true);

    sink.write(kMagic, sizeof(kMagic));
    sink.u32(kGeoFileFormatVersion);
    sink.u32(0);
    sink.u64(root.contentHash());
    sink.u64(plan.details.size());
    for (std::size_t i = 0; i < plan.details.size(); ++i) {
        writeDetail(sink, *plan.details[i], plan.sharedIds[i], options.compressionLevel);
    }
}

class Cursor {
public:
    Cursor(std::span<const unsigned char> bytes,
           std::string_view source)
        : _p(bytes.data())
        , _end(bytes.data() + bytes.size())
        , _source(source)
    {
    }

    void setSection(std::string section) { _section = std::move(section); }
    const std::string& section() const { return _section; }
    std::size_t remaining() const { return std::size_t(_end - _p); }

    [[noreturn]] void fail(const std::string& what) const
    {
        throw GeoFileError(error(_section, what));
    }

    std::string error(const std::string& section,
                      const std::string& what) const
    {
        return "Geometry file '" + _source + "' is corrupt in " + section + ": " + what;
    }

    const unsigned char* bytes(std::size_t n)
    {
        if (remaining() < n) {
            fail("the file is truncated");
        }
        const unsigned char* p = _p;
        _p += n;

        return p;
    }

    template <typename T>
    T pod()
    {
        T v;
        std::memcpy(&v, bytes(sizeof(T)), sizeof(T));

        return v;
    }

    std::uint8_t u8() { return pod<std::uint8_t>(); }
    std::uint32_t u32() { return pod<std::uint32_t>(); }
    std::uint64_t u64() { return pod<std::uint64_t>(); }
    std::int64_t i64() { return pod<std::int64_t>(); }

    std::string string()
    {
        const std::uint64_t n = u64();
        if (n > remaining()) {
            fail("the file is truncated");
        }
        const unsigned char* p = bytes(std::size_t(n));

        return std::string(reinterpret_cast<const char*>(p), std::size_t(n));
    }

    // A count of records that each take at least @p minBytes.
    std::uint64_t count(std::size_t minBytes)
    {
        const std::uint64_t n = u64();
        if (minBytes && n > remaining() / minBytes) {
            fail("a count exceeds the file size");
        }

        return n;
    }

private:
    const unsigned char* _p;
    const unsigned char* _end;
    std::string _source;
    std::string _section;
};

struct ParsedPage {
    PageKindEnum kind;
    const unsigned char* data;
    std::size_t bytes;
    std::uint64_t checksum;
};

struct ParsedColumn {
    std::size_t elementBytes = 0;
    std::size_t size = 0;
    std::vector<ParsedPage> pages;
    std::string section;
};

constexpr std::size_t kAnySize = std::size_t(-1);

ParsedColumn
parseColumn(Cursor& c,
            std::size_t expectedElementBytes,
            std::size_t expectedSize)
{
    ParsedColumn col;
    col.section = c.section();
    col.elementBytes = c.u32();
    if (col.elementBytes != expectedElementBytes) {
        c.fail("element size " + std::to_string(col.elementBytes) + ", expected " + std::to_string(expectedElementBytes));
    }
    const std::uint64_t size = c.u64();
    if (expectedSize != kAnySize && size != expectedSize) {
        c.fail("element count " + std::to_string(size) + ", expected " + std::to_string(expectedSize));
    }
    if (size > std::uint64_t(c.remaining()) / kMinPageRecordBytes * kGeoPageSize) {
        c.fail("the file is truncated");
    }
    col.size = std::size_t(size);
    const std::size_t pages = pageCountFor(col.size);
    if (pages > c.remaining() / kMinPageRecordBytes) {
        c.fail("the file is truncated");
    }
    col.pages.reserve(pages);
    for (std::size_t pi = 0; pi < pages; ++pi) {
        const std::uint8_t kind = c.u8();
        const std::uint64_t stored = c.u64();
        const std::size_t rawBytes = pageElementCount(col.size, pi) * col.elementBytes;
        if (kind == ePageConstant) {
            if (stored != col.elementBytes) {
                c.fail("page " + std::to_string(pi) + " has a bad constant size");
            }
        } else if (kind == ePageRaw) {
            if (stored != rawBytes) {
                c.fail("page " + std::to_string(pi) + " has a bad size");
            }
        } else if (kind != ePageZstd || stored == 0) {
            c.fail("page " + std::to_string(pi) + " has an unknown encoding");
        }
        const std::uint64_t checksum = pageHasChecksum(PageKindEnum(kind)) ? c.u64() : 0;
        if (stored > c.remaining()) {
            c.fail("the file is truncated");
        }
        const unsigned char* data = c.bytes(std::size_t(stored));
        col.pages.push_back(ParsedPage { PageKindEnum(kind), data, std::size_t(stored), checksum });
    }

    return col;
}

void
verifyPageChecksum(const ParsedColumn& col,
                   std::size_t pi,
                   const Cursor& errors)
{
    const ParsedPage& page = col.pages[pi];
    if (pageHasChecksum(page.kind) && pageChecksum(page.data, page.bytes) != page.checksum) {
        throw GeoFileError(errors.error(col.section, "page " + std::to_string(pi) + " fails its checksum"));
    }
}

void
decodePage(const ParsedColumn& col,
           std::size_t pi,
           unsigned char* dst,
           const Cursor& errors)
{
    const ParsedPage& page = col.pages[pi];
    const std::size_t count = pageElementCount(col.size, pi);
    const std::size_t rawBytes = count * col.elementBytes;
    verifyPageChecksum(col, pi, errors);
    switch (page.kind) {
    case ePageConstant:
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(dst + i * col.elementBytes, page.data, col.elementBytes);
        }
        return;
    case ePageRaw:
        std::memcpy(dst, page.data, rawBytes);
        return;
    case ePageZstd: {
        const std::size_t n = ZSTD_decompressDCtx(threadDCtx(), dst, rawBytes, page.data, page.bytes);
        if (ZSTD_isError(n)) {
            throw GeoFileError(errors.error(col.section, "page " + std::to_string(pi) + ": " + ZSTD_getErrorName(n)));
        }
        if (n != rawBytes) {
            throw GeoFileError(errors.error(col.section, "page " + std::to_string(pi) + " decompresses to the wrong size"));
        }
        return;
    }
    }
}

std::vector<unsigned char>
decodeColumn(const ParsedColumn& col,
             const Cursor& errors)
{
    std::vector<unsigned char> out(col.size * col.elementBytes);
    geoParallelFor(col.pages.size(), [&](std::size_t pi) { decodePage(col, pi, out.data() + (pi << kGeoPageBits) * col.elementBytes, errors); }, 2);

    return out;
}

template <typename T>
std::vector<T>
decodeTyped(const ParsedColumn& col,
            const Cursor& errors)
{
    const std::vector<unsigned char> bytes = decodeColumn(col, errors);
    std::vector<T> out(col.size);
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }

    return out;
}

struct ParsedAttribute {
    GeoAttributeDesc desc;
    std::vector<unsigned char> defaultValue;
    std::vector<std::string> strings;
    ParsedColumn values;
    ParsedColumn lengths;
    ParsedColumn pool;
};

struct ParsedGroup {
    std::string name;
    ParsedColumn bits;
};

struct ParsedDetail {
    bool precision64 = false;
    GeoIndex points = 0;
    GeoIndex vertices = 0;
    GeoIndex primitives = 0;
    std::vector<std::uint64_t> sharedIds;
    ParsedColumn primitiveTypes;
    ParsedColumn vertexCounts;
    ParsedColumn vertexPoints;
    std::array<std::vector<ParsedAttribute>, kGeoAttribClassCount> attributes;
    std::array<std::vector<ParsedGroup>, 3> groups;
    std::vector<ParsedGroup> edgeGroups;
};

const char*
className(int cls)
{
    static const char* const names[kGeoAttribClassCount] = { "point", "vertex", "primitive", "detail" };

    return names[cls];
}

ParsedDetail
parseDetail(Cursor& c,
            std::uint64_t index)
{
    const std::string record = "detail record " + std::to_string(index);
    c.setSection(record + " header");
    if (c.u32() != kTagDetail) {
        c.fail("missing record tag");
    }
    ParsedDetail d;
    const std::uint8_t precision = c.u8();
    if (precision > 1) {
        c.fail("bad point precision flag");
    }
    d.precision64 = precision == 1;
    d.points = c.i64();
    d.vertices = c.i64();
    d.primitives = c.i64();
    if (d.points < 0 || d.vertices < 0 || d.primitives < 0) {
        c.fail("negative element count");
    }
    const std::uint64_t shared = c.count(sizeof(std::uint64_t));
    for (std::uint64_t i = 0; i < shared; ++i) {
        const std::uint64_t id = c.u64();
        if (id >= index) {
            c.fail("embedded detail " + std::to_string(id) + " is not stored before its user");
        }
        d.sharedIds.push_back(id);
    }

    c.setSection(record + " primitive types");
    d.primitiveTypes = parseColumn(c, sizeof(std::int32_t), std::size_t(d.primitives));
    c.setSection(record + " primitive vertex counts");
    d.vertexCounts = parseColumn(c, sizeof(std::int32_t), std::size_t(d.primitives));
    c.setSection(record + " vertex points");
    d.vertexPoints = parseColumn(c, sizeof(std::int64_t), std::size_t(d.vertices));

    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        c.setSection(record + " " + className(cls) + " attributes");
        const std::size_t classSize = cls == eGeoAttribClassPoint ? std::size_t(d.points)
            : cls == eGeoAttribClassVertex                        ? std::size_t(d.vertices)
            : cls == eGeoAttribClassPrimitive                     ? std::size_t(d.primitives)
                                                                  : 1;
        const std::uint64_t n = c.count(8 + 4);
        std::set<std::string, std::less<>> names;
        for (std::uint64_t i = 0; i < n; ++i) {
            ParsedAttribute a;
            a.desc.name = c.string();
            c.setSection(record + " " + className(cls) + " attribute '" + a.desc.name + "'");
            if (a.desc.name.empty() || !names.insert(a.desc.name).second) {
                c.fail("empty or repeated attribute name");
            }
            const std::uint8_t storage = c.u8();
            const std::uint8_t tuple = c.u8();
            const std::uint8_t typeInfo = c.u8();
            const std::uint8_t isArray = c.u8();
            if (storage > eGeoStorageString || tuple < 1 || tuple > GeoAttribute::kMaxTupleSize || typeInfo > eGeoTypeInfoQuaternion || isArray > 1) {
                c.fail("bad attribute description");
            }
            a.desc.storage = GeoStorageEnum(storage);
            a.desc.tupleSize = tuple;
            a.desc.typeInfo = GeoTypeInfoEnum(typeInfo);
            a.desc.isArray = isArray == 1;
            const std::size_t tupleBytes = geoStorageComponentBytes(a.desc.storage) * tuple;
            if (cls == eGeoAttribClassPoint && i == 0
                && !(a.desc.name == GeoDetail::kPositionName && a.desc == GeoDetail::positionDesc(d.precision64))) {
                c.fail("the first point attribute is not P with its required description");
            }
            if (!a.desc.isArray) {
                if (c.u32() != tupleBytes) {
                    c.fail("bad default value size");
                }
                const unsigned char* def = c.bytes(tupleBytes);
                a.defaultValue.assign(def, def + tupleBytes);
            }
            if (a.desc.storage == eGeoStorageString) {
                const std::uint64_t strings = c.count(8);
                if (strings == 0) {
                    c.fail("empty string table");
                }
                for (std::uint64_t s = 1; s < strings; ++s) {
                    a.strings.push_back(c.string());
                }
            }
            if (a.desc.isArray) {
                a.lengths = parseColumn(c, sizeof(std::int64_t), classSize);
                c.setSection(c.section() + " array pool");
                a.pool = parseColumn(c, tupleBytes, kAnySize);
            } else {
                a.values = parseColumn(c, tupleBytes, classSize);
            }
            d.attributes[cls].push_back(std::move(a));
        }
        if (cls == eGeoAttribClassPoint && d.attributes[cls].empty()) {
            c.fail("P is missing");
        }
    }

    for (int cls = 0; cls < 3; ++cls) {
        c.setSection(record + " " + className(cls) + " groups");
        const std::uint64_t n = c.count(8 + 4 + 8);
        for (std::uint64_t i = 0; i < n; ++i) {
            ParsedGroup g;
            g.name = c.string();
            c.setSection(record + " " + className(cls) + " group '" + g.name + "'");
            g.bits = parseColumn(c, kGroupPageBytes, kAnySize);
            d.groups[cls].push_back(std::move(g));
        }
    }

    c.setSection(record + " edge groups");
    const std::uint64_t edges = c.count(8 + 4 + 8);
    for (std::uint64_t i = 0; i < edges; ++i) {
        ParsedGroup g;
        g.name = c.string();
        c.setSection(record + " edge group '" + g.name + "'");
        g.bits = parseColumn(c, sizeof(GeoEdge), kAnySize);
        d.edgeGroups.push_back(std::move(g));
    }

    c.setSection(record + " end");
    if (c.u32() != kTagDetailEnd) {
        c.fail("missing end tag");
    }

    return d;
}

GeoFileInfo
parseHeader(Cursor& c,
            bool wholeFile)
{
    c.setSection("the header");
    if (c.remaining() >= sizeof(kMagic) && std::memcmp(c.bytes(0), kMagic, sizeof(kMagic)) != 0) {
        c.fail("not a native geometry file");
    }
    c.bytes(sizeof(kMagic));
    GeoFileInfo info;
    info.version = c.u32();
    if (info.version != kGeoFileFormatVersion) {
        c.fail("written by geometry file format version " + std::to_string(info.version) + ", but this build reads only version "
               + std::to_string(kGeoFileFormatVersion));
    }
    if (c.u32() != 0) {
        c.fail("unknown header flags");
    }
    info.contentHash = c.u64();
    info.detailCount = c.u64();
    if (info.detailCount == 0) {
        c.fail("no detail record");
    }
    // The smallest record is its tags, counts and three empty columns.
    constexpr std::size_t kMinRecordBytes = 4 + 1 + 3 * 8 + 8 + 3 * 12 + 4 * 8 + 3 * 8 + 8 + 4;
    if (wholeFile && info.detailCount > c.remaining() / kMinRecordBytes) {
        c.fail("the record count exceeds the file size");
    }

    return info;
}

[[noreturn]] void
rethrowIn(const Cursor& errors,
          const std::string& section)
{
    try {
        throw;
    } catch (const GeoFileError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        throw GeoFileError(errors.error(section, e.what()));
    }
}

void
checkStringIndices(const std::int32_t* values,
                   std::size_t count,
                   std::size_t tableSize,
                   const Cursor& errors,
                   const std::string& section)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] < 0 || std::size_t(values[i]) >= tableSize) {
            throw GeoFileError(errors.error(section, "string index out of range"));
        }
    }
}

template <typename T>
void
fillValues(GeoAttribute& a,
           const ParsedColumn& col,
           const Cursor& errors)
{
    const std::size_t tuple = std::size_t(a.tupleSize());
    const bool strings = a.storage() == eGeoStorageString;
    const std::size_t tableSize = strings ? a.stringTable().size() : 0;
    std::vector<std::size_t> dense;
    for (std::size_t pi = 0; pi < col.pages.size(); ++pi) {
        const ParsedPage& page = col.pages[pi];
        if (page.kind != ePageConstant) {
            dense.push_back(pi);
            continue;
        }
        verifyPageChecksum(col, pi, errors);
        std::array<T, GeoAttribute::kMaxTupleSize> value;
        std::memcpy(value.data(), page.data, col.elementBytes);
        if constexpr (std::is_same_v<T, std::int32_t>) {
            if (strings) {
                checkStringIndices(value.data(), tuple, tableSize, errors, col.section);
            }
        }
        const std::size_t begin = pi << kGeoPageBits;
        a.fill<T>(begin, begin + pageElementCount(col.size, pi), std::span<const T>(value.data(), tuple));
    }
    if (dense.empty()) {
        return;
    }
    a.makePageTableUnique();
    geoParallelFor(dense.size(), [&](std::size_t i) {
        const std::size_t pi = dense[i];
        T* dst = a.pageForWriting<T>(pi);
        decodePage(col, pi, reinterpret_cast<unsigned char*>(dst), errors);
        if constexpr (std::is_same_v<T, std::int32_t>) {
            if (strings) {
                checkStringIndices(dst, pageElementCount(col.size, pi) * tuple, tableSize, errors, col.section);
            }
        } }, 2);
}

GeoAttribute
buildAttribute(const ParsedAttribute& pa,
               std::size_t classSize,
               const Cursor& errors)
{
    const std::string& section = pa.desc.isArray ? pa.lengths.section : pa.values.section;
    try {
        GeoAttribute a(pa.desc, 0);
        visitStorage(pa.desc.storage, [&](auto zero) {
            using T = decltype(zero);
            if (pa.desc.isArray || pa.desc.storage == eGeoStorageString) {
                return;
            }
            std::array<T, GeoAttribute::kMaxTupleSize> def;
            std::memcpy(def.data(), pa.defaultValue.data(), pa.defaultValue.size());
            a.setDefault<T>(std::span<const T>(def.data(), std::size_t(pa.desc.tupleSize)));
        });
        for (std::size_t i = 0; i < pa.strings.size(); ++i) {
            if (a.internString(pa.strings[i]) != std::int32_t(i + 1)) {
                throw GeoFileError(errors.error(section, "repeated string in the string table"));
            }
        }
        a.resize(classSize);

        visitStorage(pa.desc.storage, [&](auto zero) {
            using T = decltype(zero);
            if (!pa.desc.isArray) {
                fillValues<T>(a, pa.values, errors);
                return;
            }
            const std::vector<std::int64_t> lengths = decodeTyped<std::int64_t>(pa.lengths, errors);
            const std::vector<unsigned char> pool = decodeColumn(pa.pool, errors);
            const std::size_t tuple = std::size_t(pa.desc.tupleSize);
            std::size_t start = 0;
            for (const std::int64_t len : lengths) {
                if (len < 0 || std::size_t(len) > pa.pool.size - start) {
                    throw GeoFileError(errors.error(section, "array lengths exceed the array pool"));
                }
                start += std::size_t(len);
            }
            if (start != pa.pool.size) {
                throw GeoFileError(errors.error(section, "array lengths do not cover the array pool"));
            }
            start = 0;
            for (std::size_t i = 0; i < lengths.size(); ++i) {
                const std::size_t len = std::size_t(lengths[i]);
                if (len) {
                    const T* values = reinterpret_cast<const T*>(pool.data() + start * pa.pool.elementBytes);
                    a.setArray<T>(i, std::span<const T>(values, len * tuple));
                }
                start += len;
            }
        });
        a.endWriting();

        return a;
    } catch (...) {
        rethrowIn(errors, section);
    }
}

void
addGroupBits(GeoElementGroup& g,
             const std::vector<unsigned char>& bits)
{
    constexpr std::size_t kWordBits = 64;
    const std::size_t words = bits.size() / sizeof(U64);
    GeoOffset runStart = -1;
    for (std::size_t w = 0; w < words; ++w) {
        U64 word;
        std::memcpy(&word, bits.data() + w * sizeof(U64), sizeof(U64));
        const GeoOffset base = GeoOffset(w * kWordBits);
        std::size_t bit = 0;
        while (bit < kWordBits) {
            const U64 rest = word >> bit;
            if (runStart < 0) {
                if (rest == 0) {
                    break;
                }
                bit += std::size_t(std::countr_zero(rest));
                runStart = base + GeoOffset(bit);
            } else {
                bit += std::size_t(std::countr_zero(~rest));
                if (bit >= kWordBits) {
                    break;
                }
                g.addRange(runStart, base + GeoOffset(bit));
                runStart = -1;
            }
        }
    }
    if (runStart >= 0) {
        g.addRange(runStart, GeoOffset(words * kWordBits));
    }
}

GeoDetail
buildDetail(const ParsedDetail& pd,
            const std::vector<GeoDetailConstPtr>& records,
            const Cursor& errors)
{
    std::string section = pd.primitiveTypes.section;
    try {
        GeoDetailBuilder b;
        b.setPointPrecision64(pd.precision64);

        const std::vector<std::int32_t> types = decodeTyped<std::int32_t>(pd.primitiveTypes, errors);
        const std::vector<std::int32_t> counts = decodeTyped<std::int32_t>(pd.vertexCounts, errors);
        const std::vector<GeoOffset> points = decodeTyped<GeoOffset>(pd.vertexPoints, errors);
        std::size_t vertices = 0;
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (!geoPrimitiveTypeIsValid(types[i])) {
                throw GeoFileError(errors.error(section, "unknown primitive type " + std::to_string(types[i])));
            }
            if (counts[i] < 0 || std::size_t(counts[i]) > points.size() - vertices) {
                throw GeoFileError(errors.error(pd.vertexCounts.section, "vertex counts exceed the vertex count"));
            }
            vertices += std::size_t(counts[i]);
        }
        if (vertices != points.size()) {
            throw GeoFileError(errors.error(pd.vertexCounts.section, "vertex counts do not add up to the vertex count"));
        }
        for (const GeoOffset p : points) {
            if (p < 0 || p >= pd.points) {
                throw GeoFileError(errors.error(pd.vertexPoints.section, "vertex point out of range"));
            }
        }

        if (pd.points > 0) {
            b.appendPoints(pd.points);
        }
        std::size_t vertex = 0;
        for (std::size_t first = 0; first < types.size();) {
            std::size_t last = first;
            std::size_t runVertices = 0;
            while (last < types.size() && types[last] == types[first]) {
                runVertices += std::size_t(counts[last]);
                ++last;
            }
            b.appendPrimitives(GeoPrimitiveTypeEnum(types[first]),
                               std::span<const std::int32_t>(counts.data() + first, last - first),
                               std::span<const GeoOffset>(points.data() + vertex, runVertices));
            vertex += runVertices;
            first = last;
        }

        std::set<const GeoDetail*> used;
        for (std::size_t i = 0; i < pd.sharedIds.size(); ++i) {
            GeoDetailConstPtr shared = records[std::size_t(pd.sharedIds[i])];
            if (!used.insert(shared.get()).second) {
                shared = std::make_shared<const GeoDetail>(*shared);
            }
            if (b.addSharedDetail(shared) != std::int32_t(i)) {
                throw GeoFileError(errors.error(section, "embedded detail table out of order"));
            }
        }

        for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
            const std::size_t classSize = b.detail().elementOffsetSize(GeoAttribClassEnum(cls));
            const std::vector<ParsedAttribute>& list = pd.attributes[std::size_t(cls)];
            for (std::size_t i = 0; i < list.size(); ++i) {
                section = list[i].desc.isArray ? list[i].lengths.section : list[i].values.section;
                GeoAttribute built = buildAttribute(list[i], classSize, errors);
                if (cls == eGeoAttribClassPoint && i == 0) {
                    b.positionForWriting() = std::move(built);
                } else {
                    b.addAttribute(GeoAttribClassEnum(cls), list[i].desc) = std::move(built);
                }
            }
        }

        for (int cls = 0; cls < 3; ++cls) {
            for (const ParsedGroup& pg : pd.groups[std::size_t(cls)]) {
                section = pg.bits.section;
                const std::vector<unsigned char> bits = decodeColumn(pg.bits, errors);
                addGroupBits(b.addGroup(GeoAttribClassEnum(cls), pg.name), bits);
            }
        }
        for (const ParsedGroup& pg : pd.edgeGroups) {
            section = pg.bits.section;
            const std::vector<GeoEdge> edges = decodeTyped<GeoEdge>(pg.bits, errors);
            b.addEdgeGroup(pg.name).add(std::span<const GeoEdge>(edges));
        }

        section = pd.primitiveTypes.section;
        return b.finish();
    } catch (...) {
        rethrowIn(errors, section);
    }
}

class MappedFile {
public:
    explicit MappedFile(const std::string& path)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw GeoFileError(osError(path, "open", errno));
        }
        struct stat st;
        if (::fstat(fd, &st) != 0) {
            const int err = errno;
            ::close(fd);
            throw GeoFileError(osError(path, "read", err));
        }
        _size = std::size_t(st.st_size);
        if (_size > 0) {
            void* p = ::mmap(nullptr, _size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                const int err = errno;
                ::close(fd);
                throw GeoFileError(osError(path, "map", err));
            }
            _data = static_cast<const unsigned char*>(p);
            ::madvise(p, _size, MADV_SEQUENTIAL);
        }
        ::close(fd);
    }

    ~MappedFile()
    {
        if (_data) {
            ::munmap(const_cast<unsigned char*>(_data), _size);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const unsigned char> bytes() const
    {
        return std::span<const unsigned char>(_data, _size);
    }

private:
    const unsigned char* _data = nullptr;
    std::size_t _size = 0;
};

} // anon namespace

GeoDetail
geoFileNormalized(const GeoDetail& detail)
{
    bool changed = false;

    return normalizedImpl(detail, changed);
}

std::vector<unsigned char>
geoFileEncode(const GeoDetail& detail,
              const GeoFileWriteOptions& options)
{
    std::vector<unsigned char> out;
    VectorSink sink(out);
    encodeTo(sink, detail, options);

    return out;
}

GeoDetail
geoFileDecode(std::span<const unsigned char> bytes,
              std::string_view sourceName,
              const GeoFileReadOptions& options)
{
    Cursor c(bytes, sourceName);
    const GeoFileInfo info = parseHeader(c, true);

    std::vector<ParsedDetail> parsed;
    parsed.reserve(std::size_t(info.detailCount));
    for (std::uint64_t i = 0; i < info.detailCount; ++i) {
        parsed.push_back(parseDetail(c, i));
    }
    c.setSection("the end of the file");
    if (c.remaining() != 0) {
        c.fail("unexpected data after the last record");
    }

    std::vector<GeoDetailConstPtr> records;
    records.reserve(parsed.size());
    for (const ParsedDetail& pd : parsed) {
        records.push_back(std::make_shared<const GeoDetail>(buildDetail(pd, records, c)));
    }
    GeoDetail root = *records.back();
    if (options.verifyContentHash && root.contentHash() != info.contentHash) {
        throw GeoFileError(c.error("the content", "the content hash does not match the header"));
    }

    return root;
}

void
geoFileWrite(const GeoDetail& detail,
             const std::string& path,
             const GeoFileWriteOptions& options)
{
    static std::atomic<unsigned> counter { 0 };
    const std::string temp = path + ".tmp" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1));
    std::FILE* file = std::fopen(temp.c_str(), "wbe");
    if (!file) {
        throw GeoFileError(osError(path, "create", errno));
    }
    try {
        FileSink sink(file, path);
        encodeTo(sink, detail, options);
        if (std::fflush(file) != 0) {
            throw GeoFileError(osError(path, "write", errno));
        }
        if (::fsync(::fileno(file)) != 0) {
            throw GeoFileError(osError(path, "write", errno));
        }
    } catch (...) {
        std::fclose(file);
        std::remove(temp.c_str());
        throw;
    }
    if (std::fclose(file) != 0) {
        const int err = errno;
        std::remove(temp.c_str());
        throw GeoFileError(osError(path, "write", err));
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        std::remove(temp.c_str());
        throw GeoFileError(osError(path, "replace", err));
    }
    // The rename is durable only once the directory entry is.
    const std::string dir = std::filesystem::path(path).parent_path().string();
    const int dirFd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0) {
        throw GeoFileError(osError(path, "sync the directory of", errno));
    }
    const int synced = ::fsync(dirFd);
    const int err = errno;
    ::close(dirFd);
    if (synced != 0) {
        throw GeoFileError(osError(path, "sync the directory of", err));
    }
}

GeoDetail
geoFileRead(const std::string& path,
            const GeoFileReadOptions& options)
{
    const MappedFile file(path);

    return geoFileDecode(file.bytes(), path, options);
}

GeoFileInfo
geoFileReadInfo(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rbe");
    if (!file) {
        throw GeoFileError(osError(path, "open", errno));
    }
    unsigned char header[kHeaderBytes];
    const std::size_t n = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    Cursor c(std::span<const unsigned char>(header, n), path);

    return parseHeader(c, false);
}

NATRON_NAMESPACE_EXIT
