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
#include <new>
#include <stdexcept>

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

    std::vector<unsigned char>
    readAt(const std::string& path,
           std::uint64_t offset,
           std::uint64_t length)
    {
        Reader in(path);

        if (offset > in.size()) {
            throw PsdError("The file is truncated");
        }
        in.seek(offset);

        return in.bytes(std::min(length, in.size() - offset));
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

    // `planes` planes of width x height samples from data compressed with `compression`, where
    // the data holds `storedPlanes` planes (only RLE needs to know, for its row-length table).
    std::vector<float>
    decodePlanes(int compression,
                 const unsigned char* data,
                 std::size_t size,
                 std::size_t planes,
                 std::size_t storedPlanes,
                 std::size_t width,
                 std::size_t height,
                 int depth,
                 bool psb)
    {
        const std::size_t sampleBytes = (std::size_t)depth / 8;
        const std::size_t rowBytes = width * sampleBytes;
        const std::size_t planeBytes = rowBytes * height;
        std::vector<unsigned char> raw(planeBytes * planes);

        switch (compression) {
        case 0:
            if (size < raw.size()) {
                throw PsdError("Raw channel data is truncated");
            }
            std::memcpy(raw.data(), data, raw.size());
            break;
        case 1: {
            const std::size_t countBytes = psb ? 4 : 2;
            const std::size_t rows = storedPlanes * height;
            if (size / countBytes < rows) {
                throw PsdError("The RLE row table is truncated");
            }
            std::size_t offset = rows * countBytes;
            for (std::size_t r = 0; r < planes * height; ++r) {
                const unsigned char* entry = data + r * countBytes;
                const std::size_t length = psb ? be32(entry) : be16(entry);
                if (offset > size || length > size - offset) {
                    throw PsdError("RLE data is truncated");
                }
                unpackBits(data + offset, length, raw.data() + r * rowBytes, rowBytes);
                offset += length;
            }
            break;
        }
        case 2:
        case 3:
            inflateInto(data, size, raw.data(), raw.size());
            if (compression == 3) {
                unpredict(raw.data(), width, planes * height, depth);
            }
            break;
        default:
            throw PsdError("Unknown channel compression " + std::to_string(compression));
        }

        const std::size_t count = width * height * planes;
        std::vector<float> values(count);
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char* p = raw.data() + i * sampleBytes;
            if (depth == 8) {
                values[i] = p[0] / 255.f;
            } else if (depth == 16) {
                values[i] = be16(p) / 65535.f;
            } else {
                const std::uint32_t bits = be32(p);
                std::memcpy(&values[i], &bits, sizeof(float));
            }
        }

        return values;
    }

    std::vector<float>
    decodeChannel(const Source& source,
                  const Channel& channel,
                  long long width,
                  long long height)
    {
        if (width <= 0 || height <= 0) {
            return std::vector<float>();
        }
        const std::vector<unsigned char> bytes = readAt(source.path, channel.offset, channel.length);
        if (bytes.size() < 2 || bytes.size() != channel.length) {
            throw PsdError("A layer channel is truncated");
        }

        return decodePlanes(be16(bytes.data()), bytes.data() + 2, bytes.size() - 2, 1, 1, (std::size_t)width, (std::size_t)height, source.depth, source.psb);
    }

    bool
    toStoredType(const std::vector<float>& values,
                 OIIO::TypeDesc type,
                 std::vector<unsigned char>* pixels)
    {
        pixels->resize(values.size() * type.size());

        return convertValues(OIIO::TypeDesc::FLOAT, values.data(), type, pixels->data(), values.size());
    }

    bool
    hasChannel(const Record& record,
               int id)
    {
        return std::any_of(record.channels.begin(), record.channels.end(), [id](const Channel& c) { return c.id == id; });
    }

    bool
    loadLayer(const Source& source,
              const Record& record,
              std::vector<unsigned char>* pixels,
              std::string* error)
    {
        try {
            const long long width = (long long)record.right - record.left;
            const long long height = (long long)record.bottom - record.top;
            const std::size_t count = (std::size_t)width * (std::size_t)height;
            std::vector<float> rgba(count * 4, 0.f);
            for (std::size_t p = 0; p < count; ++p) {
                rgba[p * 4 + 3] = 1.f;
            }

            const Mask* mask = nullptr;
            int maskId = 0;
            if (hasChannel(record, -3) && record.realMask.present && !record.realMask.disabled) {
                mask = &record.realMask;
                maskId = -3;
            } else if (hasChannel(record, -2) && record.userMask.present && !record.userMask.disabled) {
                mask = &record.userMask;
                maskId = -2;
            }
            std::vector<float> maskValues;
            for (const Channel& channel : record.channels) {
                if (channel.id == -1 || (channel.id >= 0 && channel.id < source.colourChannels)) {
                    const std::vector<float> values = decodeChannel(source, channel, width, height);
                    const int first = channel.id == -1 ? 3 : channel.id;
                    const int last = channel.id == -1 ? 3 : source.colourChannels == 1 ? 2
                                                                                       : channel.id;
                    for (std::size_t p = 0; p < count; ++p) {
                        for (int k = first; k <= last; ++k) {
                            rgba[p * 4 + (std::size_t)k] = values[p];
                        }
                    }
                } else if (mask && channel.id == maskId) {
                    maskValues = decodeChannel(source, channel, (long long)mask->right - mask->left, (long long)mask->bottom - mask->top);
                }
            }

            if (mask) {
                const long long maskWidth = (long long)mask->right - mask->left;
                const float outside = mask->defaultColour / 255.f;
                for (long long y = 0; y < height; ++y) {
                    const long long canvasY = record.top + y;
                    for (long long x = 0; x < width; ++x) {
                        const long long canvasX = record.left + x;
                        const bool inside = !maskValues.empty() && canvasX >= mask->left && canvasX < mask->right && canvasY >= mask->top && canvasY < mask->bottom;
                        const float m = inside ? maskValues[(std::size_t)((canvasY - mask->top) * maskWidth + (canvasX - mask->left))] : outside;
                        rgba[((std::size_t)(y * width + x)) * 4 + 3] *= m;
                    }
                }
            }

            for (std::size_t p = 0; p < count; ++p) {
                float* v = &rgba[p * 4];
                v[0] *= v[3];
                v[1] *= v[3];
                v[2] *= v[3];
            }
            if (!toStoredType(rgba, source.type, pixels)) {
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
    loadComposite(const Source& source,
                  std::uint64_t offset,
                  bool alpha,
                  std::size_t storedPlanes,
                  int width,
                  int height,
                  std::vector<unsigned char>* pixels,
                  std::string* error)
    {
        try {
            const std::vector<unsigned char> bytes = readAt(source.path, offset, UINT64_MAX);
            if (bytes.size() < 2) {
                throw PsdError("The merged image data is truncated");
            }
            const std::size_t planes = (std::size_t)source.colourChannels + (alpha ? 1 : 0);
            const std::vector<float> planar = decodePlanes(be16(bytes.data()), bytes.data() + 2, bytes.size() - 2, planes, storedPlanes, (std::size_t)width, (std::size_t)height, source.depth, source.psb);
            const std::size_t count = (std::size_t)width * (std::size_t)height;
            std::vector<float> values(count * planes);
            for (std::size_t p = 0; p < count; ++p) {
                for (std::size_t c = 0; c < planes; ++c) {
                    values[p * planes + c] = planar[c * count + p];
                }
                if (alpha) {
                    const float a = values[p * planes + planes - 1];
                    for (std::size_t c = 0; c + 1 < planes; ++c) {
                        float& v = values[p * planes + c];
                        v = std::max(0.f, v + a - 1.f);
                    }
                }
            }
            if (!toStoredType(values, source.type, pixels)) {
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
                const std::shared_ptr<const Record> shared = std::make_shared<Record>(record);
                layer.plane.load = [source, shared](std::vector<unsigned char>* pixels, std::string* loadError) {
                    return loadLayer(*source, *shared, pixels, loadError);
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
            const std::size_t storedPlanes = (std::size_t)channels;
            const int w = (int)width;
            const int h = (int)height;
            composite.load = [source, sectionEnd, alpha, storedPlanes, w, h](std::vector<unsigned char>* pixels, std::string* loadError) {
                return loadComposite(*source, sectionEnd, alpha, storedPlanes, w, h, pixels, loadError);
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
