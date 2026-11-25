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

#include "LayeredImageInput.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include <zlib.h>

#include <QString>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    const int kMaxCanvasSide = 300000;
    const int kMaxLayerSide = 1 << 24;

    enum ColourMode {
        kModeBitmap = 0,
        kModeGrayscale = 1,
        kModeIndexed = 2,
        kModeRgb = 3,
        kModeCmyk = 4,
        kModeMultichannel = 7,
        kModeDuotone = 8,
        kModeLab = 9
    };

    enum SectionType {
        kSectionNone = 0,
        kSectionOpenFolder = 1,
        kSectionClosedFolder = 2,
        kSectionEnd = 3
    };

    class PsdError
        : public std::runtime_error {
    public:
        explicit PsdError(const std::string& what)
            : std::runtime_error(what)
        {
        }
    };

    std::shared_ptr<LayeredDocument>
    failed(std::string* error,
           const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return std::shared_ptr<LayeredDocument>();
    }

    std::string
    modeName(int mode)
    {
        switch (mode) {
        case kModeBitmap:
            return "Bitmap";
        case kModeGrayscale:
            return "Grayscale";
        case kModeIndexed:
            return "Indexed";
        case kModeRgb:
            return "RGB";
        case kModeCmyk:
            return "CMYK";
        case kModeMultichannel:
            return "Multichannel";
        case kModeDuotone:
            return "Duotone";
        case kModeLab:
            return "Lab";
        default:
            return "unknown (" + std::to_string(mode) + ")";
        }
    }

    std::uint16_t
    be16(const unsigned char* p)
    {
        return (std::uint16_t)((p[0] << 8) | p[1]);
    }

    std::uint32_t
    be32(const unsigned char* p)
    {
        return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) | ((std::uint32_t)p[2] << 8) | (std::uint32_t)p[3];
    }

    class Reader {
    public:
        explicit Reader(const std::string& path)
            : _in(path, std::ios::binary)
        {
            if (!_in) {
                throw PsdError("Could not read " + path);
            }
            _in.seekg(0, std::ios::end);
            const std::streamoff end = _in.tellg();
            if (end < 0) {
                throw PsdError("Could not read " + path);
            }
            _size = (std::uint64_t)end;
            _in.seekg(0);
        }

        std::uint64_t size() const
        {
            return _size;
        }

        std::uint64_t tell() const
        {
            return _pos;
        }

        void seek(std::uint64_t pos)
        {
            if (pos > _size) {
                throw PsdError("The file is truncated");
            }
            _in.clear();
            _in.seekg((std::streamoff)pos);
            _pos = pos;
        }

        void read(void* dst,
                  std::uint64_t n)
        {
            if (n > _size - _pos) {
                throw PsdError("The file is truncated");
            }
            if (n > 0 && !_in.read(static_cast<char*>(dst), (std::streamsize)n)) {
                throw PsdError("The file could not be read");
            }
            _pos += n;
        }

        std::vector<unsigned char> bytes(std::uint64_t n)
        {
            if (n > _size - _pos) {
                throw PsdError("The file is truncated");
            }
            std::vector<unsigned char> data((std::size_t)n);
            read(data.data(), n);

            return data;
        }

        std::string tag()
        {
            char c[4];
            read(c, 4);

            return std::string(c, 4);
        }

        unsigned u8()
        {
            unsigned char c;
            read(&c, 1);

            return c;
        }

        std::uint16_t u16()
        {
            unsigned char c[2];
            read(c, 2);

            return be16(c);
        }

        std::uint32_t u32()
        {
            unsigned char c[4];
            read(c, 4);

            return be32(c);
        }

        std::uint64_t u64()
        {
            const std::uint64_t high = u32();

            return (high << 32) | u32();
        }

        int i16()
        {
            return (int)(std::int16_t)u16();
        }

        int i32()
        {
            return (int)(std::int32_t)u32();
        }

        // PSB widens these lengths to 8 bytes.
        std::uint64_t length(bool wide)
        {
            return wide ? u64() : u32();
        }

        void skip(std::uint64_t n)
        {
            if (n > _size - _pos) {
                throw PsdError("The file is truncated");
            }
            seek(_pos + n);
        }

    private:
        std::ifstream _in;
        std::uint64_t _size = 0;
        std::uint64_t _pos = 0;
    };

    struct Mask {
        bool present = false;
        bool disabled = false;
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
        unsigned defaultColour = 0;
    };

    struct Channel {
        int id = 0;
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
    };

    struct Record {
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
        std::vector<Channel> channels;
        std::string blendMode;
        double opacity = 1.;
        bool clipping = false;
        bool hidden = false;
        std::string name;
        int section = kSectionNone;
        std::string kind;
        Mask userMask;
        Mask realMask;
    };

    struct LayerInfo {
        bool mergedAlpha = false;
        std::vector<Record> records;
    };

    struct Source {
        std::string path;
        bool psb = false;
        int depth = 8;
        int colourChannels = 3;
        OIIO::TypeDesc type = OIIO::TypeDesc::UINT8;
    };

    OIIO::TypeDesc
    typeOfDepth(int depth)
    {
        return depth == 8 ? OIIO::TypeDesc::UINT8 : depth == 16 ? OIIO::TypeDesc::UINT16
                                                                : OIIO::TypeDesc::FLOAT;
    }

    std::string
    trimmed(std::string s)
    {
        s.erase(s.find_last_not_of(' ') + 1);

        return s;
    }

    std::string
    latin1(const std::vector<unsigned char>& bytes)
    {
        return QString::fromLatin1(reinterpret_cast<const char*>(bytes.data()), (qsizetype)bytes.size()).toStdString();
    }

    std::string
    unicodeString(Reader& in,
                  std::uint64_t end)
    {
        const std::uint32_t count = in.u32();

        if (in.tell() > end || (std::uint64_t)count * 2 > end - in.tell()) {
            throw PsdError("A Unicode layer name is malformed");
        }
        std::u16string s;
        for (std::uint32_t i = 0; i < count; ++i) {
            s.push_back((char16_t)in.u16());
        }
        while (!s.empty() && s.back() == 0) {
            s.pop_back();
        }

        return QString::fromUtf16(s.data(), (qsizetype)s.size()).toStdString();
    }

    bool
    isOneOf(const std::string& key,
            std::initializer_list<const char*> keys)
    {
        return std::any_of(keys.begin(), keys.end(), [&key](const char* k) { return key == k; });
    }

    bool
    isWideKey(const std::string& key)
    {
        return isOneOf(key, { "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD" });
    }

    // Calls visit(key, dataOffset, length) for each tagged block in [tell(), end).
    template <typename Visit>
    void
    forEachBlock(Reader& in,
                 std::uint64_t end,
                 bool psb,
                 Visit visit)
    {
        int realign = 0;

        while (in.tell() < end && end - in.tell() >= 12) {
            const std::uint64_t start = in.tell();
            const std::string signature = in.tag();
            if (signature != "8BIM" && signature != "8B64") {
                // Writers disagree on whether blocks are padded to 2 or 4 bytes, and on whether
                // the length counts the padding, so the next signature is looked for.
                if (++realign > 3) {
                    return;
                }
                in.seek(start + 1);
                continue;
            }
            realign = 0;
            const std::string key = in.tag();
            const std::uint64_t length = in.length(psb && isWideKey(key));
            const std::uint64_t data = in.tell();
            if (data > end || length > end - data) {
                return;
            }
            visit(key, data, length);
            in.seek(data + length);
        }
    }

    void
    readRect(Reader& in,
             Mask* mask)
    {
        mask->top = in.i32();
        mask->left = in.i32();
        mask->bottom = in.i32();
        mask->right = in.i32();
    }

    void
    readMasks(Reader& in,
              std::uint64_t limit,
              Record* record)
    {
        const std::uint32_t length = in.u32();
        const std::uint64_t end = in.tell() + length;

        if (end > limit) {
            throw PsdError("A layer mask record is malformed");
        }
        if (length >= 18) {
            readRect(in, &record->userMask);
            record->userMask.defaultColour = in.u8();
            const unsigned flags = in.u8();
            record->userMask.disabled = (flags & 2) != 0;
            record->userMask.present = true;
            if (length >= 36) {
                if (flags & 16) {
                    const unsigned parameters = in.u8();
                    in.skip((parameters & 1 ? 1 : 0) + (parameters & 2 ? 8 : 0) + (parameters & 4 ? 1 : 0) + (parameters & 8 ? 8 : 0));
                }
                if (in.tell() <= end && end - in.tell() >= 18) {
                    const unsigned realFlags = in.u8();
                    record->realMask.defaultColour = in.u8();
                    readRect(in, &record->realMask);
                    record->realMask.disabled = (realFlags & 2) != 0;
                    record->realMask.present = true;
                }
            }
        }
        in.seek(end);
    }

    std::string
    kindOf(bool text,
           bool adjustment,
           bool fill,
           bool vector)
    {
        if (text) {
            return "text layer";
        }
        if (adjustment) {
            return "adjustment layer";
        }
        if (fill) {
            return vector ? "vector layer" : "fill layer";
        }

        return std::string();
    }

    Record
    readRecord(Reader& in,
               std::uint64_t end,
               bool psb)
    {
        Record r;

        r.top = in.i32();
        r.left = in.i32();
        r.bottom = in.i32();
        r.right = in.i32();
        const unsigned channelCount = in.u16();
        if (channelCount > 64) {
            throw PsdError("A layer record lists " + std::to_string(channelCount) + " channels");
        }
        for (unsigned c = 0; c < channelCount; ++c) {
            Channel channel;
            channel.id = in.i16();
            channel.length = in.length(psb);
            r.channels.push_back(channel);
        }
        if (in.tag() != "8BIM") {
            throw PsdError("A layer record is malformed");
        }
        r.blendMode = trimmed(in.tag());
        r.opacity = in.u8() / 255.;
        r.clipping = in.u8() != 0;
        r.hidden = (in.u8() & 2) != 0;
        in.u8();

        const std::uint32_t extraLength = in.u32();
        const std::uint64_t extraEnd = in.tell() + extraLength;
        if (extraEnd > end) {
            throw PsdError("A layer record is malformed");
        }
        readMasks(in, extraEnd, &r);
        in.skip(in.u32());
        const std::uint64_t nameStart = in.tell();
        const unsigned nameLength = in.u8();
        r.name = latin1(in.bytes(nameLength));
        in.seek(std::min<std::uint64_t>(nameStart + ((nameLength + 1 + 3) / 4) * 4, extraEnd));

        bool text = false;
        bool adjustment = false;
        bool fill = false;
        bool vector = false;
        forEachBlock(in, extraEnd, psb, [&](const std::string& key, std::uint64_t data, std::uint64_t length) {
            if (key == "luni") {
                r.name = unicodeString(in, data + length);
            } else if (key == "lsct" || key == "lsdk") {
                if (length >= 4) {
                    r.section = (int)in.u32();
                }
            } else if (isOneOf(key, { "TySh", "tySh" })) {
                text = true;
            } else if (isOneOf(key, { "SoCo", "GdFl", "PtFl" })) {
                fill = true;
            } else if (isOneOf(key, { "vmsk", "vsms" })) {
                vector = true;
            } else if (isOneOf(key, { "brit", "levl", "curv", "expA", "vibA", "hue ", "hue2", "blnc", "blwh", "phfl", "mixr", "clrL", "nvrt", "post", "thrs", "grdm", "selc" })) {
                adjustment = true;
            }
        });
        r.kind = kindOf(text, adjustment, fill, vector);
        in.seek(extraEnd);

        return r;
    }

    // The layer info structure: the layer count, the records, then every layer's channel data.
    LayerInfo
    readLayerInfo(Reader& in,
                  std::uint64_t end,
                  bool psb)
    {
        LayerInfo info;

        if (in.tell() > end || end - in.tell() < 2) {
            return info;
        }
        const int count = in.i16();
        info.mergedAlpha = count < 0;
        for (int i = 0; i < std::abs(count); ++i) {
            info.records.push_back(readRecord(in, end, psb));
        }
        std::uint64_t offset = in.tell();
        for (Record& record : info.records) {
            for (Channel& channel : record.channels) {
                if (offset > end || channel.length > end - offset) {
                    throw PsdError("The layer channel data is truncated");
                }
                channel.offset = offset;
                offset += channel.length;
            }
        }

        return info;
    }

    void
    readExactly(const std::string& path,
                std::uint64_t offset,
                unsigned char* dst,
                std::uint64_t length)
    {
        Reader in(path);

        if (offset > in.size()) {
            throw PsdError("The file is truncated");
        }
        in.seek(offset);
        in.read(dst, length);
    }

    std::vector<unsigned char>
    bytesAt(const std::string& path,
            std::uint64_t offset,
            std::uint64_t length)
    {
        Reader in(path);

        if (offset > in.size()) {
            throw PsdError("The file is truncated");
        }
        in.seek(offset);

        return in.bytes(length);
    }

    void
    unpackBits(const unsigned char* src,
               std::size_t size,
               unsigned char* dst,
               std::size_t dstSize)
    {
        std::size_t i = 0;
        std::size_t o = 0;

        while (o < dstSize) {
            if (i >= size) {
                throw PsdError("RLE data is truncated");
            }
            const int n = (int)(signed char)src[i++];
            if (n >= 0) {
                const std::size_t count = (std::size_t)n + 1;
                if (count > size - i || count > dstSize - o) {
                    throw PsdError("RLE data is malformed");
                }
                std::memcpy(dst + o, src + i, count);
                i += count;
                o += count;
            } else if (n != -128) {
                const std::size_t count = (std::size_t)(1 - n);
                if (i >= size || count > dstSize - o) {
                    throw PsdError("RLE data is malformed");
                }
                std::memset(dst + o, src[i++], count);
                o += count;
            }
        }
    }

    void
    inflateInto(const unsigned char* src,
                std::size_t size,
                unsigned char* dst,
                std::size_t dstSize)
    {
        z_stream z;

        std::memset(&z, 0, sizeof(z));
        if (inflateInit(&z) != Z_OK) {
            throw PsdError("Could not start decompressing ZIP data");
        }
        std::size_t inUsed = 0;
        std::size_t outUsed = 0;
        int status = Z_OK;
        while (outUsed < dstSize && status == Z_OK) {
            const std::size_t inChunk = std::min<std::size_t>(size - inUsed, UINT_MAX);
            const std::size_t outChunk = std::min<std::size_t>(dstSize - outUsed, UINT_MAX);
            z.next_in = const_cast<Bytef*>(src + inUsed);
            z.avail_in = (uInt)inChunk;
            z.next_out = dst + outUsed;
            z.avail_out = (uInt)outChunk;
            status = ::inflate(&z, Z_NO_FLUSH);
            inUsed += inChunk - z.avail_in;
            outUsed += outChunk - z.avail_out;
        }
        inflateEnd(&z);
        if (outUsed < dstSize) {
            throw PsdError("ZIP data is truncated or malformed");
        }
    }

    // Undoes the per-row delta of ZIP-with-prediction. 32-bit rows are delta-coded as bytes after
    // the samples were split into four planes of their big-endian bytes.
    void
    unpredict(unsigned char* data,
              std::size_t width,
              std::size_t rows,
              int depth)
    {
        std::vector<unsigned char> row;

        for (std::size_t y = 0; y < rows; ++y) {
            if (depth == 8) {
                unsigned char* p = data + y * width;
                for (std::size_t x = 1; x < width; ++x) {
                    p[x] = (unsigned char)(p[x] + p[x - 1]);
                }
            } else if (depth == 16) {
                unsigned char* p = data + y * width * 2;
                for (std::size_t x = 1; x < width; ++x) {
                    const std::uint16_t v = (std::uint16_t)(be16(p + x * 2) + be16(p + (x - 1) * 2));
                    p[x * 2] = (unsigned char)(v >> 8);
                    p[x * 2 + 1] = (unsigned char)(v & 0xff);
                }
            } else {
                unsigned char* p = data + y * width * 4;
                for (std::size_t x = 1; x < width * 4; ++x) {
                    p[x] = (unsigned char)(p[x] + p[x - 1]);
                }
                row.assign(p, p + width * 4);
                for (std::size_t x = 0; x < width; ++x) {
                    for (std::size_t k = 0; k < 4; ++k) {
                        p[x * 4 + k] = row[k * width + x];
                    }
                }
            }
        }
    }

    std::size_t
    sampleBytesOf(int depth)
    {
        return (std::size_t)depth / 8;
    }

    // Big-endian samples [x1, x2) of a stored row as floats, `stride` floats apart.
    void
    samplesToFloat(const unsigned char* row,
                   std::size_t x1,
                   std::size_t x2,
                   int depth,
                   float* out,
                   std::size_t stride)
    {
        for (std::size_t x = x1; x < x2; ++x, out += stride) {
            if (depth == 8) {
                *out = row[x] / 255.f;
            } else if (depth == 16) {
                *out = be16(row + x * 2) / 65535.f;
            } else {
                const std::uint32_t bits = be32(row + x * 4);
                std::memcpy(out, &bits, sizeof(float));
            }
        }
    }

    // Samples stored one plane after another, all compressed together: a layer channel (one
    // plane) or the merged image data (a plane per channel). Raw and RLE rows are read from the
    // file only when asked for, RLE through the row lengths that open the data; ZIP data has no
    // way to start mid-stream, so it is decompressed whole on the first read and kept.
    class StoredPlanes {
    public:
        // `planes` are those ever read; the data holds `storedPlanes`, which only RLE's row table
        // needs to know.
        StoredPlanes(const std::shared_ptr<const Source>& source,
                     std::uint64_t offset,
                     std::uint64_t length,
                     std::size_t width,
                     std::size_t height,
                     std::size_t planes,
                     std::size_t storedPlanes)
            : _source(source)
            , _offset(offset)
            , _length(length)
            , _width(width)
            , _height(height)
            , _planes(planes)
            , _storedPlanes(storedPlanes)
        {
        }

        std::size_t rowBytes() const
        {
            return _width * sampleBytesOf(_source->depth);
        }

        // Rows [y1, y2) of `plane`, rowBytes() each, as stored big-endian samples.
        void rows(std::size_t plane,
                  std::size_t y1,
                  std::size_t y2,
                  unsigned char* dst)
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (!_prepared) {
                    prepare();
                    _prepared = true;
                }
            }
            if (plane >= _planes || y2 > _height || y1 > y2) {
                throw PsdError("Channel rows out of range");
            }
            const std::size_t rb = rowBytes();
            const std::size_t first = plane * _height + y1;
            const std::size_t last = plane * _height + y2;
            switch (_compression) {
            case 0:
                readExactly(_source->path, _offset + 2 + (std::uint64_t)first * rb, dst, (std::uint64_t)(last - first) * rb);
                countDecodedRows(last - first);
                break;
            case 1: {
                const std::vector<unsigned char> packed = bytesAt(_source->path, _rowStarts[first], _rowStarts[last] - _rowStarts[first]);
                for (std::size_t r = first; r < last; ++r) {
                    unpackBits(packed.data() + (_rowStarts[r] - _rowStarts[first]), (std::size_t)(_rowStarts[r + 1] - _rowStarts[r]), dst + (r - first) * rb, rb);
                }
                countDecodedRows(last - first);
                break;
            }
            default:
                std::memcpy(dst, _inflated.data() + first * rb, (last - first) * rb);
                break;
            }
        }

    private:
        void prepare()
        {
            if (_length < 2) {
                throw PsdError("A channel is truncated");
            }
            unsigned char word[2];
            readExactly(_source->path, _offset, word, 2);
            _compression = be16(word);
            const std::uint64_t size = _length - 2;
            const std::size_t rb = rowBytes();
            const std::size_t rows = _planes * _height;
            switch (_compression) {
            case 0:
                if (size / std::max<std::size_t>(rb, 1) < rows) {
                    throw PsdError("Raw channel data is truncated");
                }
                break;
            case 1: {
                const std::size_t countBytes = _source->psb ? 4 : 2;
                if (size / countBytes < (std::uint64_t)_storedPlanes * _height) {
                    throw PsdError("The RLE row table is truncated");
                }
                const std::vector<unsigned char> table = bytesAt(_source->path, _offset + 2, (std::uint64_t)rows * countBytes);
                const std::uint64_t end = _offset + _length;
                _rowStarts.resize(rows + 1);
                _rowStarts[0] = _offset + 2 + (std::uint64_t)_storedPlanes * _height * countBytes;
                for (std::size_t r = 0; r < rows; ++r) {
                    const unsigned char* entry = table.data() + r * countBytes;
                    const std::uint64_t length = _source->psb ? be32(entry) : be16(entry);
                    if (_rowStarts[r] > end || length > end - _rowStarts[r]) {
                        throw PsdError("RLE data is truncated");
                    }
                    _rowStarts[r + 1] = _rowStarts[r] + length;
                }
                break;
            }
            case 2:
            case 3: {
                const std::vector<unsigned char> data = bytesAt(_source->path, _offset + 2, size);
                _inflated.resize(rows * rb);
                inflateInto(data.data(), data.size(), _inflated.data(), _inflated.size());
                if (_compression == 3) {
                    unpredict(_inflated.data(), _width, rows, _source->depth);
                }
                countDecodedRows(rows);
                break;
            }
            default:
                throw PsdError("Unknown channel compression " + std::to_string(_compression));
            }
        }

        std::shared_ptr<const Source> _source;
        std::uint64_t _offset;
        std::uint64_t _length;
        std::size_t _width;
        std::size_t _height;
        std::size_t _planes;
        std::size_t _storedPlanes;

        std::mutex _mutex;
        bool _prepared = false;
        int _compression = 0;
        std::vector<std::uint64_t> _rowStarts;
        std::vector<unsigned char> _inflated;
    };

    bool
    hasChannel(const Record& record,
               int id)
    {
        return std::any_of(record.channels.begin(), record.channels.end(), [id](const Channel& c) { return c.id == id; });
    }

    struct LayerPixels {
        std::shared_ptr<const Source> source;
        Record record;
        // Channel IDs: -1 is the transparency, 0 and up the colour channels.
        std::vector<std::pair<int, std::shared_ptr<StoredPlanes>>> channels;
        const Mask* mask = nullptr;
        // Null when the mask holds no pixels, so that all of the layer is outside it.
        std::shared_ptr<StoredPlanes> maskPlanes;
    };

    std::shared_ptr<const LayerPixels>
    layerPixelsOf(const std::shared_ptr<const Source>& source,
                  const Record& record)
    {
        const std::shared_ptr<LayerPixels> pixels = std::make_shared<LayerPixels>();

        pixels->source = source;
        pixels->record = record;
        const Record& r = pixels->record;
        const std::size_t width = (std::size_t)((long long)r.right - r.left);
        const std::size_t height = (std::size_t)((long long)r.bottom - r.top);
        int maskId = 0;
        if (hasChannel(r, -3) && r.realMask.present && !r.realMask.disabled) {
            pixels->mask = &r.realMask;
            maskId = -3;
        } else if (hasChannel(r, -2) && r.userMask.present && !r.userMask.disabled) {
            pixels->mask = &r.userMask;
            maskId = -2;
        }
        for (const Channel& channel : r.channels) {
            if (channel.id == -1 || (channel.id >= 0 && channel.id < source->colourChannels)) {
                pixels->channels.push_back(std::make_pair(channel.id, std::make_shared<StoredPlanes>(source, channel.offset, channel.length, width, height, 1, 1)));
            } else if (pixels->mask && channel.id == maskId) {
                const long long maskWidth = (long long)pixels->mask->right - pixels->mask->left;
                const long long maskHeight = (long long)pixels->mask->bottom - pixels->mask->top;
                if (maskWidth > 0 && maskHeight > 0 && maskWidth <= kMaxLayerSide && maskHeight <= kMaxLayerSide) {
                    pixels->maskPlanes = std::make_shared<StoredPlanes>(source, channel.offset, channel.length, (std::size_t)maskWidth, (std::size_t)maskHeight, 1, 1);
                }
            }
        }

        return pixels;
    }

    bool
    loadLayerRegion(const LayerPixels& layer,
                    const Region& region,
                    unsigned char* dst,
                    std::string* error)
    {
        const Source& source = *layer.source;
        const Record& record = layer.record;

        try {
            const std::size_t width = (std::size_t)region.width();
            const std::size_t height = (std::size_t)region.height();
            const std::size_t count = width * height;
            std::vector<float> rgba(count * 4, 0.f);
            for (std::size_t p = 0; p < count; ++p) {
                rgba[p * 4 + 3] = 1.f;
            }

            std::vector<unsigned char> rows;
            std::vector<float> values(width);
            for (const std::pair<int, std::shared_ptr<StoredPlanes>>& channel : layer.channels) {
                const std::size_t rb = channel.second->rowBytes();
                rows.resize(height * rb);
                channel.second->rows(0, (std::size_t)region.y1, (std::size_t)region.y2, rows.data());
                const int first = channel.first == -1 ? 3 : channel.first;
                const int last = channel.first == -1 ? 3 : source.colourChannels == 1 ? 2
                                                                                      : channel.first;
                for (std::size_t y = 0; y < height; ++y) {
                    samplesToFloat(rows.data() + y * rb, (std::size_t)region.x1, (std::size_t)region.x2, source.depth, values.data(), 1);
                    for (std::size_t x = 0; x < width; ++x) {
                        for (int k = first; k <= last; ++k) {
                            rgba[(y * width + x) * 4 + (std::size_t)k] = values[x];
                        }
                    }
                }
            }

            if (layer.mask) {
                const Mask& mask = *layer.mask;
                const float outside = mask.defaultColour / 255.f;
                const long long top = (long long)record.top + region.y1;
                const long long left = (long long)record.left + region.x1;
                const long long my1 = std::max<long long>(top, mask.top);
                const long long my2 = std::min<long long>(top + (long long)height, mask.bottom);
                const long long mx1 = std::max<long long>(left, mask.left);
                const long long mx2 = std::min<long long>(left + (long long)width, mask.right);
                const bool overlaps = layer.maskPlanes && my2 > my1 && mx2 > mx1;
                std::vector<unsigned char> maskRows;
                std::vector<float> maskValues;
                std::size_t mrb = 0;
                if (overlaps) {
                    mrb = layer.maskPlanes->rowBytes();
                    maskRows.resize((std::size_t)(my2 - my1) * mrb);
                    layer.maskPlanes->rows(0, (std::size_t)(my1 - mask.top), (std::size_t)(my2 - mask.top), maskRows.data());
                    maskValues.resize((std::size_t)(mx2 - mx1));
                }
                for (std::size_t y = 0; y < height; ++y) {
                    const long long canvasY = top + (long long)y;
                    const bool rowInside = overlaps && canvasY >= my1 && canvasY < my2;
                    if (rowInside) {
                        samplesToFloat(maskRows.data() + (std::size_t)(canvasY - my1) * mrb, (std::size_t)(mx1 - mask.left), (std::size_t)(mx2 - mask.left), source.depth, maskValues.data(), 1);
                    }
                    for (std::size_t x = 0; x < width; ++x) {
                        const long long canvasX = left + (long long)x;
                        const bool inside = rowInside && canvasX >= mx1 && canvasX < mx2;
                        rgba[(y * width + x) * 4 + 3] *= inside ? maskValues[(std::size_t)(canvasX - mx1)] : outside;
                    }
                }
            }

            for (std::size_t p = 0; p < count; ++p) {
                float* v = &rgba[p * 4];
                v[0] *= v[3];
                v[1] *= v[3];
                v[2] *= v[3];
            }
            if (!convertValues(OIIO::TypeDesc::FLOAT, rgba.data(), source.type, dst, rgba.size())) {
                throw PsdError("Could not convert the pixels of layer " + record.name);
            }
        } catch (const PsdError& e) {
            *error = source.path + ": " + e.what();

            return false;
        } catch (const std::bad_alloc&) {
            *error = source.path + ": out of memory decoding layer " + record.name;

            return false;
        }

        return true;
    }

    // Photoshop mattes a merged image that has transparency against white, so the white is taken
    // back out to leave colour associated with alpha.
    bool
    loadCompositeRegion(const Source& source,
                        StoredPlanes& stored,
                        bool alpha,
                        const Region& region,
                        unsigned char* dst,
                        std::string* error)
    {
        try {
            const std::size_t planes = (std::size_t)source.colourChannels + (alpha ? 1 : 0);
            const std::size_t width = (std::size_t)region.width();
            const std::size_t height = (std::size_t)region.height();
            const std::size_t count = width * height;
            const std::size_t rb = stored.rowBytes();
            std::vector<float> values(count * planes);
            std::vector<unsigned char> rows(height * rb);
            for (std::size_t c = 0; c < planes; ++c) {
                stored.rows(c, (std::size_t)region.y1, (std::size_t)region.y2, rows.data());
                for (std::size_t y = 0; y < height; ++y) {
                    samplesToFloat(rows.data() + y * rb, (std::size_t)region.x1, (std::size_t)region.x2, source.depth, values.data() + y * width * planes + c, planes);
                }
            }
            if (alpha) {
                for (std::size_t p = 0; p < count; ++p) {
                    const float a = values[p * planes + planes - 1];
                    for (std::size_t c = 0; c + 1 < planes; ++c) {
                        float& v = values[p * planes + c];
                        v = std::max(0.f, v + a - 1.f);
                    }
                }
            }
            if (!convertValues(OIIO::TypeDesc::FLOAT, values.data(), source.type, dst, values.size())) {
                throw PsdError("Could not convert the merged image");
            }
        } catch (const PsdError& e) {
            *error = source.path + ": " + e.what();

            return false;
        } catch (const std::bad_alloc&) {
            *error = source.path + ": out of memory decoding the merged image";

            return false;
        }

        return true;
    }

    std::string
    joinedPath(const std::vector<std::string>& groups,
               const std::string& name)
    {
        std::string path;

        for (const std::string& group : groups) {
            path += group + "/";
        }

        return path + name;
    }

    std::shared_ptr<LayeredDocument>
    parse(const std::string& path)
    {
        Reader in(path);

        if (in.size() < 26 || in.tag() != "8BPS") {
            throw PsdError(path + " is not a Photoshop file");
        }
        const int version = in.u16();
        if (version != 1 && version != 2) {
            throw PsdError(path + " is not a Photoshop file: its version is " + std::to_string(version));
        }
        const std::shared_ptr<Source> source = std::make_shared<Source>();
        source->path = path;
        source->psb = version == 2;
        in.skip(6);
        const int channels = in.u16();
        const std::uint32_t height = in.u32();
        const std::uint32_t width = in.u32();
        source->depth = in.u16();
        const int mode = in.u16();

        if (mode != kModeGrayscale && mode != kModeRgb) {
            throw PsdError(path + " uses the " + modeName(mode) + " colour mode; only RGB and Grayscale documents are read");
        }
        if (source->depth != 8 && source->depth != 16 && source->depth != 32) {
            throw PsdError(path + " has an unsupported depth of " + std::to_string(source->depth) + " bits");
        }
        source->colourChannels = mode == kModeRgb ? 3 : 1;
        source->type = typeOfDepth(source->depth);
        if (channels < source->colourChannels || channels > 56) {
            throw PsdError(path + " has " + std::to_string(channels) + " channels, which its colour mode cannot have");
        }
        if (width > (std::uint32_t)kMaxCanvasSide || height > (std::uint32_t)kMaxCanvasSide) {
            throw PsdError(path + " has a canvas larger than " + std::to_string(kMaxCanvasSide) + " pixels");
        }

        in.skip(in.u32());

        bool realMergedData = true;
        const std::uint32_t resourcesLength = in.u32();
        const std::uint64_t resourcesEnd = in.tell() + resourcesLength;
        if (resourcesEnd > in.size()) {
            throw PsdError("The image resources are truncated");
        }
        while (in.tell() < resourcesEnd && resourcesEnd - in.tell() >= 12) {
            in.tag();
            const unsigned id = in.u16();
            const unsigned nameLength = in.u8();
            in.skip(nameLength + ((nameLength + 1) % 2));
            const std::uint32_t size = in.u32();
            const std::uint64_t data = in.tell();
            if (data > resourcesEnd || size > resourcesEnd - data) {
                break;
            }
            // Version info: a document saved without maximised compatibility has no real merged
            // image, only a blank one.
            if (id == 1057 && size >= 5) {
                in.u32();
                realMergedData = in.u8() != 0;
            }
            in.seek(std::min<std::uint64_t>(data + size + (size & 1), resourcesEnd));
        }
        in.seek(resourcesEnd);

        const std::uint64_t sectionLength = in.length(source->psb);
        const std::uint64_t sectionEnd = in.tell() + sectionLength;
        if (sectionEnd > in.size()) {
            throw PsdError("The layer and mask information is truncated");
        }
        LayerInfo info;
        if (sectionLength > 0) {
            const std::uint64_t infoLength = in.length(source->psb);
            const std::uint64_t infoStart = in.tell();
            if (infoStart > sectionEnd || infoLength > sectionEnd - infoStart) {
                throw PsdError("The layer info is truncated");
            }
            if (infoLength > 0) {
                info = readLayerInfo(in, infoStart + infoLength, source->psb);
            }
            in.seek(infoStart + infoLength);
            if (sectionEnd - in.tell() >= 4) {
                const std::uint32_t globalMaskLength = in.u32();
                in.seek(std::min<std::uint64_t>(in.tell() + globalMaskLength, sectionEnd));
            }
            // 16- and 32-bit documents keep their layers here rather than in the layer info.
            forEachBlock(in, sectionEnd, source->psb, [&](const std::string& key, std::uint64_t data, std::uint64_t length) {
                if (isOneOf(key, { "Lr16", "Lr32", "Layr" }) && info.records.empty()) {
                    in.seek(data);
                    info = readLayerInfo(in, data + length, source->psb);
                }
            });
        }

        const std::shared_ptr<LayeredDocument> document = std::make_shared<LayeredDocument>();
        document->width = (int)width;
        document->height = (int)height;

        struct Group {
            std::string name;
            double opacity;
            bool visible;
        };
        std::vector<Group> groups;
        std::vector<Layer> topFirst;
        for (std::vector<Record>::const_reverse_iterator it = info.records.rbegin(); it != info.records.rend(); ++it) {
            const Record& record = *it;
            if (record.section == kSectionOpenFolder || record.section == kSectionClosedFolder) {
                groups.push_back(Group { record.name, record.opacity, !record.hidden });
                continue;
            }
            if (record.section == kSectionEnd) {
                if (!groups.empty()) {
                    groups.pop_back();
                }
                continue;
            }

            Layer layer;
            layer.name = record.name;
            for (const Group& group : groups) {
                layer.groups.push_back(group.name);
                layer.groupOpacity *= group.opacity;
                layer.groupVisible = layer.groupVisible && group.visible;
            }
            if (!record.kind.empty()) {
                document->skipped.push_back(joinedPath(layer.groups, layer.name) + " (" + record.kind + ")");
                continue;
            }
            layer.blendMode = record.blendMode;
            layer.opacity = record.opacity;
            layer.visible = !record.hidden;
            layer.clipping = record.clipping;

            const long long layerWidth = (long long)record.right - record.left;
            const long long layerHeight = (long long)record.bottom - record.top;
            bool hasPixels = hasChannel(record, -1);
            for (int c = 0; c < source->colourChannels; ++c) {
                hasPixels = hasPixels || hasChannel(record, c);
            }
            if (hasPixels && layerWidth > 0 && layerHeight > 0 && layerWidth <= kMaxLayerSide && layerHeight <= kMaxLayerSide) {
                layer.plane.x = record.left;
                layer.plane.y = record.top;
                layer.plane.width = (int)layerWidth;
                layer.plane.height = (int)layerHeight;
                layer.plane.channels = { "R", "G", "B", "A" };
                layer.plane.type = source->type;
                const std::shared_ptr<const LayerPixels> pixels = layerPixelsOf(source, record);
                layer.plane.loadRegion = [pixels](const Region& region, unsigned char* dst, std::string* loadError) {
                    return loadLayerRegion(*pixels, region, dst, loadError);
                };
            }
            topFirst.push_back(std::move(layer));
        }
        document->layers.assign(std::make_move_iterator(topFirst.rbegin()), std::make_move_iterator(topFirst.rend()));

        if (realMergedData && in.size() - sectionEnd >= 2) {
            const bool alpha = info.mergedAlpha && channels > source->colourChannels;
            Plane& composite = document->composite;
            composite.width = (int)width;
            composite.height = (int)height;
            if (source->colourChannels == 1) {
                composite.channels = { "Y" };
            } else {
                composite.channels = { "R", "G", "B" };
            }
            if (alpha) {
                composite.channels.push_back("A");
            }
            composite.type = source->type;
            const std::size_t planes = (std::size_t)source->colourChannels + (alpha ? 1 : 0);
            const std::shared_ptr<StoredPlanes> stored = std::make_shared<StoredPlanes>(source, sectionEnd, in.size() - sectionEnd, (std::size_t)width, (std::size_t)height, planes, (std::size_t)channels);
            composite.loadRegion = [source, stored, alpha](const Region& region, unsigned char* dst, std::string* loadError) {
                return loadCompositeRegion(*source, *stored, alpha, region, dst, loadError);
            };
        }

        return document;
    }
} // anonymous namespace

std::shared_ptr<LayeredDocument>
readPsd(const std::string& path,
        std::string* error)
{
    std::shared_ptr<LayeredDocument> document;

    try {
        document = parse(path);
    } catch (const PsdError& e) {
        const std::string what = e.what();

        return failed(error, what.compare(0, path.size(), path) == 0 ? what : path + ": " + what);
    } catch (const std::bad_alloc&) {
        return failed(error, path + ": out of memory reading the layer records");
    }

    std::string finaliseError;
    if (!document->finalise(&finaliseError)) {
        return failed(error, path + ": " + finaliseError);
    }

    return document;
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
