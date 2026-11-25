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
#include <bit>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

#include <zlib.h>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    const int kTileSide = 64;

    // Sizes and offsets beyond this are taken for a corrupt file; it keeps every extent derived
    // from them well inside an int.
    const long long kMaxCoordinate = 1LL << 28;

    // Versions from 100 up were written by CinePaint, not GIMP.
    const int kFirstForeignVersion = 100;

    // Pointers widen to 8 bytes from this version.
    const int kFirstWideVersion = 11;

    enum Property {
        kPropEnd = 0,
        kPropColormap = 1,
        kPropFloatingSelection = 5,
        kPropOpacity = 6,
        kPropMode = 7,
        kPropVisible = 8,
        kPropApplyMask = 11,
        kPropOffsets = 15,
        kPropCompression = 17,
        kPropGroupItem = 29,
        kPropItemPath = 30,
        kPropFloatOpacity = 33
    };

    enum Compression {
        kCompressionNone = 0,
        kCompressionRle = 1,
        kCompressionZlib = 2
    };

    enum BaseType {
        kBaseRgb = 0,
        kBaseGray = 1,
        kBaseIndexed = 2
    };

    class XcfError
        : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
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
    joinedPath(const std::vector<std::string>& groups,
               const std::string& name)
    {
        std::string path;

        for (const std::string& group : groups) {
            path += group + "/";
        }

        return path + name;
    }

    std::string
    baseTypeName(std::uint32_t type)
    {
        switch (type) {
        case kBaseRgb:
            return "RGB";
        case kBaseGray:
            return "Grayscale";
        case kBaseIndexed:
            return "Indexed";
        default:
            return "unknown (" + std::to_string(type) + ")";
        }
    }

    std::string
    modeName(std::uint32_t mode)
    {
        static const char* const names[] = {
            "Normal (legacy)",
            "Dissolve (legacy)",
            "Behind (legacy)",
            "Multiply (legacy)",
            "Screen (legacy)",
            "Old broken Overlay",
            "Difference (legacy)",
            "Addition (legacy)",
            "Subtract (legacy)",
            "Darken only (legacy)",
            "Lighten only (legacy)",
            "Hue (HSV) (legacy)",
            "Saturation (HSV) (legacy)",
            "Color (HSL) (legacy)",
            "Value (HSV) (legacy)",
            "Divide (legacy)",
            "Dodge (legacy)",
            "Burn (legacy)",
            "Hard light (legacy)",
            "Soft light (legacy)",
            "Grain extract (legacy)",
            "Grain merge (legacy)",
            "Color erase (legacy)",
            "Overlay",
            "Hue (LCH)",
            "Chroma (LCH)",
            "Color (LCH)",
            "Lightness (LCH)",
            "Normal",
            "Behind",
            "Multiply",
            "Screen",
            "Difference",
            "Addition",
            "Subtract",
            "Darken only",
            "Lighten only",
            "Hue (HSV)",
            "Saturation (HSV)",
            "Color (HSL)",
            "Value (HSV)",
            "Divide",
            "Dodge",
            "Burn",
            "Hard light",
            "Soft light",
            "Grain extract",
            "Grain merge",
            "Vivid light",
            "Pin light",
            "Linear light",
            "Hard mix",
            "Exclusion",
            "Linear burn",
            "Luma darken only",
            "Luma lighten only",
            "Luminance",
            "Color erase",
            "Erase",
            "Merge",
            "Split",
            "Pass through",
        };

        if (mode < sizeof(names) / sizeof(names[0])) {
            return names[mode];
        }

        return "mode " + std::to_string(mode);
    }

    /**
     * How the image's precision stores a component, and the type its planes are handed out as:
     * 32-bit integers and doubles become floats, the deepest type the seam carries.
     **/
    struct Storage {
        OIIO::TypeDesc stored = OIIO::TypeDesc::UINT8;
        OIIO::TypeDesc plane = OIIO::TypeDesc::UINT8;
    };

    bool
    storageOf(int version,
              std::uint32_t precision,
              Storage* storage)
    {
        OIIO::TypeDesc::BASETYPE type = OIIO::TypeDesc::UNKNOWN;

        if (version < 4) {
            type = OIIO::TypeDesc::UINT8;
        } else if (version == 4) {
            const OIIO::TypeDesc::BASETYPE byCode[] = { OIIO::TypeDesc::UINT8, OIIO::TypeDesc::UINT16, OIIO::TypeDesc::UINT32, OIIO::TypeDesc::HALF, OIIO::TypeDesc::FLOAT };
            if (precision < 5) {
                type = byCode[precision];
            }
        } else if (version < 7) {
            if (precision % 50 == 0 && precision >= 100 && precision <= 550) {
                const OIIO::TypeDesc::BASETYPE byHundred[] = { OIIO::TypeDesc::UINT8, OIIO::TypeDesc::UINT16, OIIO::TypeDesc::UINT32, OIIO::TypeDesc::HALF, OIIO::TypeDesc::FLOAT };
                type = byHundred[precision / 100 - 1];
            }
        } else {
            // The tens give the transfer: 0 linear, 50 non-linear, 75 perceptual.
            const std::uint32_t transfer = precision % 100;
            if (transfer == 0 || transfer == 50 || transfer == 75) {
                switch (precision / 100) {
                case 1:
                    type = OIIO::TypeDesc::UINT8;
                    break;
                case 2:
                    type = OIIO::TypeDesc::UINT16;
                    break;
                case 3:
                    type = OIIO::TypeDesc::UINT32;
                    break;
                case 5:
                    type = OIIO::TypeDesc::HALF;
                    break;
                case 6:
                    type = OIIO::TypeDesc::FLOAT;
                    break;
                case 7:
                    type = OIIO::TypeDesc::DOUBLE;
                    break;
                default:
                    break;
                }
            }
        }
        if (type == OIIO::TypeDesc::UNKNOWN) {
            return false;
        }
        storage->stored = OIIO::TypeDesc(type);
        storage->plane = (type == OIIO::TypeDesc::UINT32 || type == OIIO::TypeDesc::DOUBLE) ? OIIO::TypeDesc(OIIO::TypeDesc::FLOAT) : OIIO::TypeDesc(type);

        return true;
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
                throw XcfError("Could not open the file");
            }
            _in.seekg(0, std::ios::end);
            const std::streamoff end = _in.tellg();
            if (end < 0) {
                throw XcfError("Could not open the file");
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
                throw XcfError("The file is truncated");
            }
            _in.clear();
            _in.seekg((std::streamoff)pos);
            _pos = pos;
        }

        void read(void* dst,
                  std::uint64_t n)
        {
            if (n > _size - _pos) {
                throw XcfError("The file is truncated");
            }
            if (n > 0 && !_in.read(static_cast<char*>(dst), (std::streamsize)n)) {
                throw XcfError("The file could not be read");
            }
            _pos += n;
        }

        std::vector<unsigned char> bytes(std::uint64_t n)
        {
            if (n > _size - _pos) {
                throw XcfError("The file is truncated");
            }
            std::vector<unsigned char> data((std::size_t)n);
            read(data.data(), n);

            return data;
        }

        std::uint32_t u32()
        {
            unsigned char c[4];
            read(c, 4);

            return be32(c);
        }

        std::int32_t i32()
        {
            return (std::int32_t)u32();
        }

        float f32()
        {
            return std::bit_cast<float>(u32());
        }

        // Pointers widen from 4 to 8 bytes in later versions.
        std::uint64_t pointer(bool wide)
        {
            if (!wide) {
                return u32();
            }
            const std::uint64_t high = u32();

            return (high << 32) | u32();
        }

        // A length that counts the terminating zero, then the UTF-8 bytes and that zero; the
        // empty string is a lone zero length.
        std::string string()
        {
            const std::uint32_t n = u32();
            if (n == 0) {
                return std::string();
            }
            const std::vector<unsigned char> data = bytes(n);
            std::string text(reinterpret_cast<const char*>(data.data()), data.size());
            const std::size_t end = text.find('\0');

            return end == std::string::npos ? text : text.substr(0, end);
        }

    private:
        std::ifstream _in;
        std::uint64_t _size = 0;
        std::uint64_t _pos = 0;
    };

    /**
     * Calls `visit(type, length)` for each property of the list at the reader's position, with
     * the reader at the start of its payload, and leaves the reader past the list. A visitor
     * need not read the whole payload.
     **/
    template <typename Visit>
    void
    forEachProperty(Reader& in,
                    Visit visit)
    {
        for (;;) {
            const std::uint32_t type = in.u32();
            const std::uint32_t length = in.u32();
            if (type == kPropEnd) {
                return;
            }
            const std::uint64_t start = in.tell();
            if (type == kPropColormap) {
                // Some GIMP releases stored a wrong length here; the colour count is right.
                const std::uint32_t colours = in.u32();
                in.seek(start + 4 + 3ULL * colours);
                continue;
            }
            if (length > in.size() - start) {
                throw XcfError("A property list is truncated");
            }
            visit(type, length);
            in.seek(start + length);
        }
    }

    struct Source {
        std::string path;
        int compression = kCompressionNone;
        Storage storage;
    };

    /**
     * The pixels of a layer or a channel: `width` x `height` pixels of `pixelSize` bytes, in
     * tiles of 64 x 64 (smaller on the right and bottom edges) stored row by row from the top.
     **/
    struct Drawable {
        int width = 0;
        int height = 0;
        std::size_t pixelSize = 0;
        std::vector<std::uint64_t> tiles;

        int columns() const
        {
            return (width + kTileSide - 1) / kTileSide;
        }
    };

    Drawable
    readHierarchy(Reader& in,
                  std::uint64_t pointer,
                  bool wide,
                  int width,
                  int height,
                  std::size_t pixelSize,
                  const std::string& subject)
    {
        if (pointer == 0 || pointer >= in.size()) {
            throw XcfError(subject + " has no pixel data");
        }
        in.seek(pointer);
        const std::uint32_t storedWidth = in.u32();
        const std::uint32_t storedHeight = in.u32();
        const std::uint32_t bpp = in.u32();
        if (storedWidth != (std::uint32_t)width || storedHeight != (std::uint32_t)height) {
            throw XcfError(subject + " stores pixels of " + std::to_string(storedWidth) + "x" + std::to_string(storedHeight) + " where it is " + std::to_string(width) + "x" + std::to_string(height));
        }
        if (bpp != pixelSize) {
            throw XcfError(subject + " stores pixels of " + std::to_string(bpp) + " bytes where its mode and precision have " + std::to_string(pixelSize));
        }
        const std::uint64_t level = in.pointer(wide);
        if (level == 0 || level >= in.size()) {
            throw XcfError(subject + " has no pixel level");
        }
        in.seek(level);
        if (in.u32() != (std::uint32_t)width || in.u32() != (std::uint32_t)height) {
            throw XcfError(subject + " has a pixel level of another size");
        }

        Drawable drawable;
        drawable.width = width;
        drawable.height = height;
        drawable.pixelSize = pixelSize;
        const std::uint64_t count = (std::uint64_t)drawable.columns() * (std::uint64_t)((height + kTileSide - 1) / kTileSide);
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t tile = in.pointer(wide);
            if (tile == 0 || tile >= in.size()) {
                throw XcfError(subject + " lacks tile " + std::to_string(i) + " of " + std::to_string(count));
            }
            drawable.tiles.push_back(tile);
        }

        return drawable;
    }

    void
    unpackRle(const unsigned char* src,
              std::size_t size,
              unsigned char* dst,
              std::size_t dstSize,
              const std::string& subject)
    {
        std::size_t i = 0;
        std::size_t o = 0;

        while (o < dstSize) {
            if (i >= size) {
                throw XcfError(subject + " has a truncated RLE tile");
            }
            const unsigned n = src[i++];
            std::size_t count = 0;
            bool repeat = false;
            if (n <= 126) {
                count = n + 1;
                repeat = true;
            } else if (n == 127 || n == 128) {
                if (size - i < 2) {
                    throw XcfError(subject + " has a truncated RLE tile");
                }
                count = (std::size_t)src[i] * 256 + src[i + 1];
                i += 2;
                repeat = n == 127;
            } else {
                count = 256 - n;
            }
            if (count > dstSize - o) {
                throw XcfError(subject + " has a malformed RLE tile");
            }
            if (repeat) {
                if (i >= size) {
                    throw XcfError(subject + " has a truncated RLE tile");
                }
                std::memset(dst + o, src[i++], count);
            } else {
                if (count > size - i) {
                    throw XcfError(subject + " has a truncated RLE tile");
                }
                std::memcpy(dst + o, src + i, count);
                i += count;
            }
            o += count;
        }
    }

    void
    inflateTile(const unsigned char* src,
                std::size_t size,
                unsigned char* dst,
                std::size_t dstSize,
                const std::string& subject)
    {
        z_stream z;

        std::memset(&z, 0, sizeof(z));
        if (inflateInit(&z) != Z_OK) {
            throw XcfError("Could not start decompressing a zlib tile");
        }
        z.next_in = const_cast<Bytef*>(src);
        z.avail_in = (uInt)std::min<std::size_t>(size, UINT_MAX);
        z.next_out = dst;
        z.avail_out = (uInt)dstSize;
        const int status = ::inflate(&z, Z_FINISH);
        const std::size_t written = dstSize - z.avail_out;
        inflateEnd(&z);
        if ((status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) || written != dstSize) {
            throw XcfError(subject + " has a corrupt zlib tile");
        }
    }

    // Tile `index` of `drawable`, `tw` x `th` pixels, in native byte order.
    void
    decodeTile(Reader& in,
               const Source& source,
               const Drawable& drawable,
               std::size_t index,
               int tw,
               int th,
               const std::string& subject,
               std::vector<unsigned char>* planes,
               std::vector<unsigned char>* pixels)
    {
        const std::size_t count = (std::size_t)tw * (std::size_t)th;
        const std::size_t ps = drawable.pixelSize;
        const std::size_t bytes = count * ps;
        const std::uint64_t start = drawable.tiles[index];

        // Tiles of a drawable follow each other, so the next one bounds this one; the last is
        // bounded by the most an encoder could reasonably need.
        std::uint64_t available = in.size() - start;
        if (index + 1 < drawable.tiles.size() && drawable.tiles[index + 1] > start) {
            available = std::min(available, drawable.tiles[index + 1] - start);
        } else {
            available = std::min<std::uint64_t>(available, (std::uint64_t)bytes * 2 + 1024);
        }
        if (source.compression == kCompressionNone) {
            available = std::min<std::uint64_t>(available, bytes);
        }
        in.seek(start);
        const std::vector<unsigned char> data = in.bytes(available);

        pixels->resize(bytes);
        switch (source.compression) {
        case kCompressionNone:
            if (data.size() < bytes) {
                throw XcfError(subject + " has a truncated tile");
            }
            std::memcpy(pixels->data(), data.data(), bytes);
            break;
        case kCompressionRle: {
            // One stream per byte of the pixel: all first bytes, then all second bytes...
            planes->resize(bytes);
            unpackRle(data.data(), data.size(), planes->data(), bytes, subject);
            for (std::size_t b = 0; b < ps; ++b) {
                const unsigned char* plane = planes->data() + b * count;
                for (std::size_t p = 0; p < count; ++p) {
                    (*pixels)[p * ps + b] = plane[p];
                }
            }
            break;
        }
        case kCompressionZlib:
            inflateTile(data.data(), data.size(), pixels->data(), bytes, subject);
            break;
        default:
            throw XcfError(subject + " uses an unknown tile compression");
        }

        const std::size_t componentSize = source.storage.stored.size();
        if (componentSize > 1 && std::endian::native == std::endian::little) {
            for (std::size_t c = 0; c < bytes; c += componentSize) {
                std::reverse(pixels->begin() + (std::ptrdiff_t)c, pixels->begin() + (std::ptrdiff_t)(c + componentSize));
            }
        }
        countDecodedRows((std::uint64_t)th);
    }

    // The pixels of columns [x1, x2) and rows [y1, y2) of `drawable`, decoding only the tiles
    // they touch.
    std::vector<unsigned char>
    readDrawable(Reader& in,
                 const Source& source,
                 const Drawable& drawable,
                 const Region& region,
                 const std::string& subject)
    {
        const std::size_t width = (std::size_t)region.width();
        const std::size_t ps = drawable.pixelSize;
        std::vector<unsigned char> out(width * (std::size_t)region.height() * ps);

        if (region.width() <= 0 || region.height() <= 0) {
            return out;
        }
        const int col1 = region.x1 / kTileSide;
        const int col2 = (region.x2 - 1) / kTileSide;
        const int row1 = region.y1 / kTileSide;
        const int row2 = (region.y2 - 1) / kTileSide;
        std::vector<unsigned char> planes;
        std::vector<unsigned char> pixels;
        for (int row = row1; row <= row2; ++row) {
            const int top = row * kTileSide;
            const int th = std::min(kTileSide, drawable.height - top);
            for (int col = col1; col <= col2; ++col) {
                const int left = col * kTileSide;
                const int tw = std::min(kTileSide, drawable.width - left);
                decodeTile(in, source, drawable, (std::size_t)row * (std::size_t)drawable.columns() + (std::size_t)col, tw, th, subject, &planes, &pixels);
                const int ix1 = std::max(region.x1, left);
                const int ix2 = std::min(region.x2, left + tw);
                const int iy1 = std::max(region.y1, top);
                const int iy2 = std::min(region.y2, top + th);
                const std::size_t rowBytes = (std::size_t)(ix2 - ix1) * ps;
                for (int y = iy1; y < iy2; ++y) {
                    std::memcpy(out.data() + ((std::size_t)(y - region.y1) * width + (std::size_t)(ix1 - region.x1)) * ps,
                                pixels.data() + ((std::size_t)(y - top) * (std::size_t)tw + (std::size_t)(ix1 - left)) * ps,
                                rowBytes);
                }
            }
        }

        return out;
    }

    struct LayerPixels {
        std::shared_ptr<const Source> source;
        std::string name;
        int channels = 0;
        bool gray = false;
        bool alpha = false;
        Drawable pixels;
        bool masked = false;
        Drawable mask;
    };

    bool
    loadLayerRegion(const LayerPixels& layer,
                    const Region& region,
                    unsigned char* dst,
                    std::string* error)
    {
        const Source& source = *layer.source;

        try {
            Reader in(source.path);
            const std::size_t count = (std::size_t)region.width() * (std::size_t)region.height();
            const std::vector<unsigned char> stored = readDrawable(in, source, layer.pixels, region, "Layer " + layer.name);
            std::vector<float> values(count * (std::size_t)layer.channels);
            if (!convertValues(source.storage.stored, stored.data(), OIIO::TypeDesc::FLOAT, values.data(), values.size())) {
                throw XcfError("Could not convert the pixels of layer " + layer.name);
            }
            std::vector<float> rgba(count * 4);
            for (std::size_t p = 0; p < count; ++p) {
                const float* v = &values[p * (std::size_t)layer.channels];
                float* out = &rgba[p * 4];
                if (layer.gray) {
                    out[0] = out[1] = out[2] = v[0];
                } else {
                    out[0] = v[0];
                    out[1] = v[1];
                    out[2] = v[2];
                }
                out[3] = layer.alpha ? v[layer.channels - 1] : 1.f;
            }
            if (layer.masked) {
                const std::vector<unsigned char> maskStored = readDrawable(in, source, layer.mask, region, "The mask of layer " + layer.name);
                std::vector<float> mask(count);
                if (!convertValues(source.storage.stored, maskStored.data(), OIIO::TypeDesc::FLOAT, mask.data(), count)) {
                    throw XcfError("Could not convert the mask of layer " + layer.name);
                }
                for (std::size_t p = 0; p < count; ++p) {
                    rgba[p * 4 + 3] *= mask[p];
                }
            }
            // GIMP keeps alpha unassociated.
            for (std::size_t p = 0; p < count; ++p) {
                float* v = &rgba[p * 4];
                v[0] *= v[3];
                v[1] *= v[3];
                v[2] *= v[3];
            }
            if (!convertValues(OIIO::TypeDesc::FLOAT, rgba.data(), source.storage.plane, dst, rgba.size())) {
                throw XcfError("Could not convert the pixels of layer " + layer.name);
            }
        } catch (const XcfError& e) {
            *error = source.path + ": " + e.what();

            return false;
        } catch (const std::bad_alloc&) {
            *error = source.path + ": out of memory decoding layer " + layer.name;

            return false;
        }

        return true;
    }

    struct Group {
        std::vector<std::string> names;
        double opacity = 1.;
        bool visible = true;
    };

    struct LayerHeader {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t type = 0;
        std::string name;
        double opacity = 1.;
        std::uint32_t mode = 0;
        bool visible = true;
        bool applyMask = true;
        long long x = 0;
        long long y = 0;
        bool group = false;
        bool floating = false;
        std::vector<std::uint32_t> itemPath;
        std::uint64_t hierarchy = 0;
        std::uint64_t mask = 0;
    };

    LayerHeader
    readLayerHeader(Reader& in,
                    std::uint64_t pointer,
                    bool wide)
    {
        LayerHeader h;

        if (pointer >= in.size()) {
            throw XcfError("A layer lies past the end of the file");
        }
        in.seek(pointer);
        h.width = in.u32();
        h.height = in.u32();
        h.type = in.u32();
        h.name = in.string();
        bool floatOpacity = false;
        forEachProperty(in, [&](std::uint32_t type, std::uint32_t length) {
            switch (type) {
            case kPropFloatingSelection:
                h.floating = true;
                break;
            case kPropOpacity:
                if (length >= 4 && !floatOpacity) {
                    h.opacity = std::clamp((double)in.u32() / 255., 0., 1.);
                }
                break;
            case kPropFloatOpacity:
                if (length >= 4) {
                    h.opacity = std::clamp((double)in.f32(), 0., 1.);
                    floatOpacity = true;
                }
                break;
            case kPropMode:
                if (length >= 4) {
                    h.mode = in.u32();
                }
                break;
            case kPropVisible:
                if (length >= 4) {
                    h.visible = in.u32() != 0;
                }
                break;
            case kPropApplyMask:
                if (length >= 4) {
                    h.applyMask = in.u32() != 0;
                }
                break;
            case kPropOffsets:
                if (length >= 8) {
                    h.x = in.i32();
                    h.y = in.i32();
                }
                break;
            case kPropGroupItem:
                h.group = true;
                break;
            case kPropItemPath:
                h.itemPath.clear();
                for (std::uint32_t i = 0; i + 4 <= length; i += 4) {
                    h.itemPath.push_back(in.u32());
                }
                break;
            default:
                break;
            }
        });
        h.hierarchy = in.pointer(wide);
        h.mask = in.pointer(wide);

        return h;
    }

    struct ChannelHeader {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::string name;
        std::uint64_t hierarchy = 0;
    };

    ChannelHeader
    readChannelHeader(Reader& in,
                      std::uint64_t pointer,
                      bool wide)
    {
        ChannelHeader h;

        if (pointer >= in.size()) {
            throw XcfError("A layer mask lies past the end of the file");
        }
        in.seek(pointer);
        h.width = in.u32();
        h.height = in.u32();
        h.name = in.string();
        forEachProperty(in, [](std::uint32_t, std::uint32_t) { });
        h.hierarchy = in.pointer(wide);

        return h;
    }

    std::shared_ptr<LayeredDocument>
    parse(const std::string& path)
    {
        Reader in(path);

        char magic[9];
        if (in.size() < 14) {
            throw XcfError(path + " is not a GIMP file");
        }
        in.read(magic, 9);
        if (std::memcmp(magic, "gimp xcf ", 9) != 0) {
            throw XcfError(path + " is not a GIMP file");
        }
        char tag[5] = {};
        in.read(tag, 5);
        int version = -1;
        if (std::memcmp(tag, "file", 4) == 0) {
            version = 0;
        } else if (tag[0] == 'v' && std::isdigit((unsigned char)tag[1]) && std::isdigit((unsigned char)tag[2]) && std::isdigit((unsigned char)tag[3])) {
            version = std::atoi(tag + 1);
        }
        if (version < 0 || tag[4] != '\0') {
            throw XcfError(path + " is not a GIMP file: its version tag is \"" + std::string(tag, 4) + "\"");
        }
        if (version >= kFirstForeignVersion) {
            throw XcfError(path + " uses version " + std::to_string(version) + " of the format, which is CinePaint's rather than GIMP's");
        }
        const bool wide = version >= kFirstWideVersion;

        const std::uint32_t width = in.u32();
        const std::uint32_t height = in.u32();
        const std::uint32_t baseType = in.u32();
        if (baseType != kBaseRgb && baseType != kBaseGray) {
            throw XcfError(path + " uses the " + baseTypeName(baseType) + " colour mode; only RGB and Grayscale documents are read");
        }
        const std::uint32_t precision = version >= 4 ? in.u32() : 150;
        const std::shared_ptr<Source> source = std::make_shared<Source>();
        source->path = path;
        if (!storageOf(version, precision, &source->storage)) {
            throw XcfError(path + " has an unknown precision (" + std::to_string(precision) + ")");
        }
        // Development versions stored deep pixels in the byte order of the machine that saved
        // them, which the file does not record.
        if (version >= 7 && version < 12 && source->storage.stored.size() > 1) {
            throw XcfError(path + " was saved at more than 8 bits by a development version of GIMP (version " + std::to_string(version) + "), whose byte order is unknown");
        }
        if (width == 0 || height == 0 || width > (std::uint32_t)kMaxCoordinate || height > (std::uint32_t)kMaxCoordinate) {
            throw XcfError(path + " has a canvas of " + std::to_string(width) + "x" + std::to_string(height) + " pixels");
        }

        forEachProperty(in, [&](std::uint32_t type, std::uint32_t length) {
            if (type == kPropCompression && length >= 1) {
                unsigned char compression = 0;
                in.read(&compression, 1);
                source->compression = compression;
            }
        });
        if (source->compression != kCompressionNone && source->compression != kCompressionRle && source->compression != kCompressionZlib) {
            throw XcfError(path + " uses an unknown tile compression (" + std::to_string(source->compression) + ")");
        }

        std::vector<std::uint64_t> pointers;
        for (;;) {
            const std::uint64_t pointer = in.pointer(wide);
            if (pointer == 0) {
                break;
            }
            pointers.push_back(pointer);
        }

        const std::shared_ptr<LayeredDocument> document = std::make_shared<LayeredDocument>();
        document->width = (int)width;
        document->height = (int)height;

        const std::size_t componentSize = source->storage.stored.size();
        std::map<std::vector<std::uint32_t>, Group> groups;
        std::uint32_t topLevel = 0;
        std::vector<Layer> topFirst;
        // Layers come topmost first, each group before the layers inside it; the item path of
        // a layer in a group is its index at each level, outermost first.
        for (const std::uint64_t pointer : pointers) {
            const LayerHeader h = readLayerHeader(in, pointer, wide);
            const std::vector<std::uint32_t> itemPath = h.itemPath.empty() ? std::vector<std::uint32_t>(1, topLevel++) : h.itemPath;

            Group parent;
            if (itemPath.size() > 1) {
                const std::map<std::vector<std::uint32_t>, Group>::const_iterator it = groups.find(std::vector<std::uint32_t>(itemPath.begin(), itemPath.end() - 1));
                if (it != groups.end()) {
                    parent = it->second;
                }
            }
            const std::string layerPath = joinedPath(parent.names, h.name);

            if (h.floating) {
                document->skipped.push_back(layerPath + " (floating selection)");
                continue;
            }
            if (h.group) {
                Group group = parent;
                group.names.push_back(h.name);
                group.opacity *= h.opacity;
                group.visible = group.visible && h.visible;
                groups[itemPath] = group;
                if (h.mask != 0) {
                    const ChannelHeader mask = readChannelHeader(in, h.mask, wide);
                    document->skipped.push_back(joinedPath(group.names, mask.name) + " (layer mask on a group)");
                }
                continue;
            }

            int channels = 0;
            switch (h.type) {
            case 0:
                channels = 3;
                break;
            case 1:
                channels = 4;
                break;
            case 2:
                channels = 1;
                break;
            case 3:
                channels = 2;
                break;
            case 4:
            case 5:
                throw XcfError("Layer " + layerPath + " uses the Indexed colour mode; only RGB and Grayscale documents are read");
            default:
                throw XcfError("Layer " + layerPath + " uses an unknown colour mode (" + std::to_string(h.type) + ")");
            }
            if (h.width > (std::uint32_t)kMaxCoordinate || h.height > (std::uint32_t)kMaxCoordinate || std::abs(h.x) > kMaxCoordinate || std::abs(h.y) > kMaxCoordinate) {
                throw XcfError("Layer " + layerPath + " is too large or too far from the canvas");
            }

            Layer layer;
            layer.name = h.name;
            layer.groups = parent.names;
            layer.groupOpacity = parent.opacity;
            layer.groupVisible = parent.visible;
            layer.blendMode = modeName(h.mode);
            layer.opacity = h.opacity;
            layer.visible = h.visible;

            if (h.width > 0 && h.height > 0) {
                const std::shared_ptr<LayerPixels> pixels = std::make_shared<LayerPixels>();
                pixels->source = source;
                pixels->name = layerPath;
                pixels->channels = channels;
                pixels->gray = channels <= 2;
                pixels->alpha = channels == 2 || channels == 4;
                pixels->pixels = readHierarchy(in, h.hierarchy, wide, (int)h.width, (int)h.height, (std::size_t)channels * componentSize, "Layer " + layerPath);
                if (h.mask != 0) {
                    const ChannelHeader mask = readChannelHeader(in, h.mask, wide);
                    if (!h.applyMask) {
                        document->skipped.push_back(joinedPath(parent.names, h.name) + "/" + mask.name + " (disabled layer mask)");
                    } else {
                        if (mask.width != h.width || mask.height != h.height) {
                            throw XcfError("The mask of layer " + layerPath + " is not the size of its layer");
                        }
                        pixels->mask = readHierarchy(in, mask.hierarchy, wide, (int)h.width, (int)h.height, componentSize, "The mask of layer " + layerPath);
                        pixels->masked = true;
                    }
                }

                layer.plane.x = (int)h.x;
                layer.plane.y = (int)h.y;
                layer.plane.width = (int)h.width;
                layer.plane.height = (int)h.height;
                layer.plane.channels = { "R", "G", "B", "A" };
                layer.plane.type = source->storage.plane;
                layer.plane.tileWidth = kTileSide;
                layer.plane.tileHeight = kTileSide;
                layer.plane.loadRegion = [pixels](const Region& region, unsigned char* dst, std::string* loadError) {
                    return loadLayerRegion(*pixels, region, dst, loadError);
                };
            }
            topFirst.push_back(std::move(layer));
        }
        document->layers.assign(std::make_move_iterator(topFirst.rbegin()), std::make_move_iterator(topFirst.rend()));

        return document;
    }
} // anonymous namespace

std::shared_ptr<LayeredDocument>
readXcf(const std::string& path,
        std::string* error)
{
    std::shared_ptr<LayeredDocument> document;

    try {
        document = parse(path);
    } catch (const XcfError& e) {
        const std::string what = e.what();

        return failed(error, what.compare(0, path.size(), path) == 0 ? what : path + ": " + what);
    } catch (const std::bad_alloc&) {
        return failed(error, path + ": out of memory reading the layer structure");
    }

    // There is no stored composite: finalise flattens the visible layers.
    std::string finaliseError;
    if (!document->finalise(&finaliseError)) {
        return failed(error, path + ": " + finaliseError);
    }

    return document;
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
