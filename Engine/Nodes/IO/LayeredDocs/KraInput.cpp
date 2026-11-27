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
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>

#include "Engine/Nodes/IO/LayeredDocs/Lzf.h"
#include "Engine/Nodes/IO/LayeredDocs/ZipArchive.h"

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    const char* const kMimeType = "application/x-krita";

    // Tile coordinates and layer offsets beyond this are taken for a corrupt file; it keeps every
    // extent derived from them well inside an int.
    const long long kMaxCoordinate = 1LL << 28;
    const int kMaxTileSide = 4096;
    const std::size_t kMaxGroupDepth = 256;

    const unsigned char kRawTile = 0;
    const unsigned char kLzfTile = 1;

    class KraError
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

    long long
    floorDiv(long long a,
             long long b)
    {
        const long long q = a / b;

        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
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

    // Krita's 8- and 16-bit RGB colour spaces keep their channels in B, G, R, A order; its float
    // ones in R, G, B, A.
    struct PixelFormat {
        OIIO::TypeDesc type = OIIO::TypeDesc::UINT8;
        int channels = 0;
        bool gray = false;
        bool bgr = false;

        std::size_t pixelSize() const
        {
            return (std::size_t)channels * type.size();
        }
    };

    bool
    pixelFormatOf(const std::string& colorSpace,
                  PixelFormat* format)
    {
        struct Known {
            const char* id;
            OIIO::TypeDesc::BASETYPE type;
            bool gray;
            bool bgr;
        };
        static const Known known[] = {
            { "RGBA", OIIO::TypeDesc::UINT8, false, true },
            { "RGBA16", OIIO::TypeDesc::UINT16, false, true },
            { "RGBAF16", OIIO::TypeDesc::HALF, false, false },
            { "RGBAF32", OIIO::TypeDesc::FLOAT, false, false },
            { "GRAYA", OIIO::TypeDesc::UINT8, true, false },
            { "GRAYAU16", OIIO::TypeDesc::UINT16, true, false },
            { "GRAYA16", OIIO::TypeDesc::UINT16, true, false },
            { "GRAYAF16", OIIO::TypeDesc::HALF, true, false },
            { "GRAYAF32", OIIO::TypeDesc::FLOAT, true, false },
        };

        for (const Known& k : known) {
            if (colorSpace == k.id) {
                format->type = OIIO::TypeDesc(k.type);
                format->channels = k.gray ? 2 : 4;
                format->gray = k.gray;
                format->bgr = k.bgr;

                return true;
            }
        }

        return false;
    }

    std::string
    colourModelOf(const std::string& colorSpace)
    {
        std::string upper = colorSpace;
        for (char& c : upper) {
            c = (char)std::toupper((unsigned char)c);
        }
        const std::pair<const char*, const char*> models[] = { { "CMYK", "CMYK" }, { "LAB", "Lab" }, { "XYZ", "XYZ" }, { "YCBCR", "YCbCr" }, { "ALPHA", "Alpha" } };
        for (const std::pair<const char*, const char*>& model : models) {
            if (upper.compare(0, std::strlen(model.first), model.first) == 0) {
                return model.second;
            }
        }

        return colorSpace;
    }

    PixelFormat
    requireFormat(const std::string& colorSpace,
                  const std::string& subject)
    {
        PixelFormat format;

        if (!pixelFormatOf(colorSpace, &format)) {
            throw KraError(subject + " uses the " + colourModelOf(colorSpace) + " colour model (" + (colorSpace.empty() ? std::string("none given") : colorSpace) + "); only RGB and Grayscale documents are read");
        }

        return format;
    }

    // `count` pixels stored as `format` as R, G, B, A floats, with alpha unassociated as Krita
    // keeps it.
    void
    toStraightRgba(const PixelFormat& format,
                   const unsigned char* bytes,
                   std::size_t count,
                   float* rgba)
    {
        std::vector<float> v(count * (std::size_t)format.channels);

        if (!convertValues(format.type, bytes, OIIO::TypeDesc::FLOAT, v.data(), v.size())) {
            throw KraError("Could not convert the stored pixels");
        }
        for (std::size_t p = 0; p < count; ++p) {
            float* out = rgba + p * 4;
            if (format.gray) {
                out[0] = out[1] = out[2] = v[p * 2];
                out[3] = v[p * 2 + 1];
            } else {
                const float* in = &v[p * 4];
                out[0] = format.bgr ? in[2] : in[0];
                out[1] = in[1];
                out[2] = format.bgr ? in[0] : in[2];
                out[3] = in[3];
            }
        }
    }

    struct Tile {
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    /**
     * A Krita paint device: its tiles, keyed by (row, column) of the tile grid, as offsets into
     * its member, and the pixel it holds wherever no tile is stored. Device pixel (0, 0) is at
     * canvas pixel (x, y).
     **/
    struct Device {
        std::shared_ptr<const Archive> archive;
        std::string member;
        long long x = 0;
        long long y = 0;
        int tileWidth = 0;
        int tileHeight = 0;
        std::size_t pixelSize = 0;
        std::map<std::pair<long long, long long>, Tile> tiles;
        std::vector<unsigned char> defaultPixel;
    };

    long long
    numberOf(std::string_view text,
             const std::string& what)
    {
        long long value = 0;
        const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);

        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
            throw KraError(what + " is not a number: \"" + std::string(text) + "\"");
        }

        return value;
    }

    std::shared_ptr<const Device>
    parseDevice(const std::shared_ptr<const Archive>& archive,
                const std::string& member,
                std::size_t pixelSize,
                long long x,
                long long y)
    {
        if (x < -kMaxCoordinate || x > kMaxCoordinate || y < -kMaxCoordinate || y > kMaxCoordinate) {
            throw KraError(member + " is offset too far from the canvas");
        }
        const std::shared_ptr<Device> device = std::make_shared<Device>();
        device->archive = archive;
        device->member = member;
        device->x = x;
        device->y = y;

        const std::span<const std::byte> data = archive->zip->data(member);
        const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
        std::size_t pos = 0;
        const auto nextLine = [&]() {
            const std::size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) {
                throw KraError(member + " is truncated");
            }
            const std::string_view line = text.substr(pos, end - pos);
            pos = end + 1;

            return line;
        };
        const auto headerValue = [&](const char* key) {
            const std::string_view line = nextLine();
            const std::size_t keyLength = std::strlen(key);
            if (line.size() <= keyLength + 1 || line.compare(0, keyLength, key) != 0 || line[keyLength] != ' ') {
                throw KraError(member + " has no " + key + " line where one belongs");
            }

            return numberOf(line.substr(keyLength + 1), member + ": " + key);
        };

        const long long version = headerValue("VERSION");
        if (version != 2) {
            throw KraError(member + " uses version " + std::to_string(version) + " of the tile format; only version 2 is read");
        }
        const long long tileWidth = headerValue("TILEWIDTH");
        const long long tileHeight = headerValue("TILEHEIGHT");
        const long long storedPixelSize = headerValue("PIXELSIZE");
        const long long count = headerValue("DATA");
        if (tileWidth <= 0 || tileHeight <= 0 || tileWidth > kMaxTileSide || tileHeight > kMaxTileSide) {
            throw KraError(member + " has tiles of " + std::to_string(tileWidth) + "x" + std::to_string(tileHeight) + " pixels");
        }
        if (storedPixelSize != (long long)pixelSize) {
            throw KraError(member + " has pixels of " + std::to_string(storedPixelSize) + " bytes where its colour space has " + std::to_string(pixelSize));
        }
        if (count < 0) {
            throw KraError(member + " has a negative tile count");
        }
        device->tileWidth = (int)tileWidth;
        device->tileHeight = (int)tileHeight;
        device->pixelSize = pixelSize;

        for (long long i = 0; i < count; ++i) {
            const std::string_view line = nextLine();
            std::string_view fields[4];
            std::size_t start = 0;
            for (int f = 0; f < 4; ++f) {
                const std::size_t comma = f < 3 ? line.find(',', start) : line.size();
                if (comma == std::string_view::npos) {
                    throw KraError(member + " has a malformed tile header: \"" + std::string(line) + "\"");
                }
                fields[f] = line.substr(start, comma - start);
                start = comma + 1;
            }
            const long long left = numberOf(fields[0], member + ": a tile's column");
            const long long top = numberOf(fields[1], member + ": a tile's row");
            const long long size = numberOf(fields[3], member + ": a tile's size");
            if (fields[2] != "LZF") {
                throw KraError(member + " has a tile compressed with " + std::string(fields[2]) + "; only LZF is read");
            }
            if (left < -kMaxCoordinate || left > kMaxCoordinate || top < -kMaxCoordinate || top > kMaxCoordinate || left % tileWidth != 0 || top % tileHeight != 0) {
                throw KraError(member + " has a tile at " + std::to_string(left) + "," + std::to_string(top) + ", off its tile grid");
            }
            if (size < 1 || (unsigned long long)size > text.size() - pos) {
                throw KraError(member + " is truncated");
            }
            device->tiles[std::make_pair(top / tileHeight, left / tileWidth)] = Tile { pos, (std::size_t)size };
            pos += (std::size_t)size;
        }

        const std::string defaultMember = member + ".defaultpixel";
        device->defaultPixel.assign(pixelSize, 0);
        if (archive->zip->contains(defaultMember)) {
            const std::span<const std::byte> pixel = archive->zip->data(defaultMember);
            if (pixel.size() != pixelSize) {
                throw KraError(defaultMember + " holds " + std::to_string(pixel.size()) + " bytes instead of one pixel of " + std::to_string(pixelSize));
            }
            std::memcpy(device->defaultPixel.data(), pixel.data(), pixelSize);
        }

        return device;
    }

    void
    decodeTile(const Device& device,
               std::span<const std::byte> data,
               const Tile& tile,
               std::vector<unsigned char>* linear,
               std::vector<unsigned char>* pixels)
    {
        const std::size_t count = (std::size_t)device.tileWidth * (std::size_t)device.tileHeight;
        const std::size_t bytes = count * device.pixelSize;
        const unsigned char* stored = reinterpret_cast<const unsigned char*>(data.data()) + tile.offset;

        linear->resize(bytes);
        pixels->resize(bytes);
        if (stored[0] == kLzfTile) {
            std::size_t written = 0;
            if (!lzfDecompress(stored + 1, tile.size - 1, linear->data(), bytes, &written) || written != bytes) {
                throw KraError(device.member + " has a corrupt LZF tile");
            }
            // Compressed tiles hold each byte of the pixel as its own plane: all first bytes,
            // then all second bytes, and so on.
            for (std::size_t b = 0; b < device.pixelSize; ++b) {
                const unsigned char* plane = linear->data() + b * count;
                for (std::size_t p = 0; p < count; ++p) {
                    (*pixels)[p * device.pixelSize + b] = plane[p];
                }
            }
        } else if (stored[0] == kRawTile) {
            if (tile.size - 1 < bytes) {
                throw KraError(device.member + " has a truncated uncompressed tile");
            }
            std::memcpy(pixels->data(), stored + 1, bytes);
        } else {
            throw KraError(device.member + " has a tile of unknown kind " + std::to_string((int)stored[0]));
        }
        countDecodedRows((std::uint64_t)device.tileHeight);
    }

    // The pixels of canvas columns [x1, x2) and rows [y1, y2) of `device`, decoding only the
    // tiles they touch.
    std::vector<unsigned char>
    readDevice(const Device& device,
               long long x1,
               long long y1,
               long long x2,
               long long y2)
    {
        const std::size_t width = (std::size_t)(x2 - x1);
        const std::size_t ps = device.pixelSize;
        std::vector<unsigned char> out(width * (std::size_t)(y2 - y1) * ps);

        for (std::size_t p = 0; p < out.size(); p += ps) {
            std::memcpy(out.data() + p, device.defaultPixel.data(), ps);
        }
        if (device.tiles.empty() || x2 <= x1 || y2 <= y1) {
            return out;
        }

        const std::span<const std::byte> data = device.archive->zip->data(device.member);
        const long long tw = device.tileWidth;
        const long long th = device.tileHeight;
        const long long col1 = floorDiv(x1 - device.x, tw);
        const long long col2 = floorDiv(x2 - 1 - device.x, tw);
        const long long row1 = floorDiv(y1 - device.y, th);
        const long long row2 = floorDiv(y2 - 1 - device.y, th);
        std::vector<unsigned char> linear;
        std::vector<unsigned char> pixels;
        for (long long row = row1; row <= row2; ++row) {
            for (std::map<std::pair<long long, long long>, Tile>::const_iterator it = device.tiles.lower_bound(std::make_pair(row, col1));
                 it != device.tiles.end() && it->first.first == row && it->first.second <= col2;
                 ++it) {
                decodeTile(device, data, it->second, &linear, &pixels);
                const long long left = device.x + it->first.second * tw;
                const long long top = device.y + row * th;
                const long long ix1 = std::max(x1, left);
                const long long ix2 = std::min(x2, left + tw);
                const long long iy1 = std::max(y1, top);
                const long long iy2 = std::min(y2, top + th);
                const std::size_t rowBytes = (std::size_t)(ix2 - ix1) * ps;
                for (long long y = iy1; y < iy2; ++y) {
                    std::memcpy(out.data() + ((std::size_t)(y - y1) * width + (std::size_t)(ix1 - x1)) * ps,
                                pixels.data() + ((std::size_t)(y - top) * (std::size_t)tw + (std::size_t)(ix1 - left)) * ps,
                                rowBytes);
                }
            }
        }

        return out;
    }

    struct LayerPixels {
        std::string path;
        std::string name;
        PixelFormat format;
        std::shared_ptr<const Device> device;
        std::vector<std::shared_ptr<const Device>> masks;
        long long planeX = 0;
        long long planeY = 0;
    };

    bool
    loadLayerRegion(const LayerPixels& layer,
                    const Region& region,
                    unsigned char* dst,
                    std::string* error)
    {
        try {
            const long long x1 = layer.planeX + region.x1;
            const long long y1 = layer.planeY + region.y1;
            const long long x2 = layer.planeX + region.x2;
            const long long y2 = layer.planeY + region.y2;
            const std::size_t count = (std::size_t)region.width() * (std::size_t)region.height();

            const std::vector<unsigned char> stored = readDevice(*layer.device, x1, y1, x2, y2);
            std::vector<float> rgba(count * 4);
            toStraightRgba(layer.format, stored.data(), count, rgba.data());
            for (const std::shared_ptr<const Device>& mask : layer.masks) {
                const std::vector<unsigned char> selection = readDevice(*mask, x1, y1, x2, y2);
                for (std::size_t p = 0; p < count; ++p) {
                    rgba[p * 4 + 3] *= (float)selection[p] / 255.f;
                }
            }
            for (std::size_t p = 0; p < count; ++p) {
                float* v = &rgba[p * 4];
                v[0] *= v[3];
                v[1] *= v[3];
                v[2] *= v[3];
            }
            if (!convertValues(OIIO::TypeDesc::FLOAT, rgba.data(), layer.format.type, dst, rgba.size())) {
                throw KraError("Could not convert the pixels of layer " + layer.name);
            }
        } catch (const KraError& e) {
            *error = layer.path + ": " + e.what();

            return false;
        } catch (const Serialization::ZipError& e) {
            *error = layer.path + ": " + e.what();

            return false;
        } catch (const std::bad_alloc&) {
            *error = layer.path + ": out of memory decoding layer " + layer.name;

            return false;
        }

        return true;
    }

    std::string
    attributeOf(const QXmlStreamReader& xml,
                const char* name)
    {
        return xml.attributes().value(QString::fromUtf8(name)).toString().toStdString();
    }

    long long
    integerAttributeOf(const QXmlStreamReader& xml,
                       const char* name)
    {
        bool ok = false;
        const double value = xml.attributes().value(QString::fromUtf8(name)).toString().toDouble(&ok);

        if (ok && !std::isfinite(value)) {
            throw KraError(std::string("maindoc.xml has a ") + name + " that is not a finite number");
        }

        return ok ? (long long)std::clamp(value, -1e12, 1e12) : 0;
    }

    double
    opacityOf(const QXmlStreamReader& xml)
    {
        bool ok = false;
        const double value = xml.attributes().value(QString::fromUtf8("opacity")).toString().toDouble(&ok);

        if (ok && !std::isfinite(value)) {
            throw KraError("maindoc.xml has an opacity that is not a finite number");
        }

        return ok ? std::clamp(value / 255., 0., 1.) : 1.;
    }

    bool
    visibleOf(const QXmlStreamReader& xml)
    {
        return attributeOf(xml, "visible") != "0";
    }

    std::string
    layerKindOf(const std::string& nodeType)
    {
        const std::pair<const char*, const char*> kinds[] = {
            { "adjustmentlayer", "filter layer" },
            { "generatorlayer", "fill layer" },
            { "clonelayer", "clone layer" },
            { "shapelayer", "vector layer" },
            { "filelayer", "file layer" },
            { "referenceimages", "reference images layer" },
            { "filtermask", "filter mask" },
            { "transformmask", "transform mask" },
            { "selectionmask", "selection mask" },
            { "colorizemask", "colorize mask" },
            { "transparencymask", "transparency mask" },
        };

        for (const std::pair<const char*, const char*>& kind : kinds) {
            if (nodeType == kind.first) {
                return kind.second;
            }
        }

        return nodeType.empty() ? std::string("layer of no kind") : nodeType;
    }

    struct MaskRef {
        std::string name;
        std::string filename;
        long long x = 0;
        long long y = 0;
    };

    class Parser {
    public:
        Parser(const std::shared_ptr<const Archive>& archive,
               const std::string& path,
               LayeredDocument* document)
            : _archive(archive)
            , _path(path)
            , _document(document)
        {
        }

        void parse(QXmlStreamReader& xml)
        {
            if (!xml.readNextStartElement() || xml.name() != QLatin1String("DOC")) {
                throw KraError("maindoc.xml has no DOC element");
            }
            bool sawImage = false;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("IMAGE") && !sawImage) {
                    sawImage = true;
                    parseImage(xml);
                } else {
                    xml.skipCurrentElement();
                }
            }
            if (xml.hasError()) {
                throw KraError("maindoc.xml is malformed: " + xml.errorString().toStdString());
            }
            if (!sawImage) {
                throw KraError("maindoc.xml has no IMAGE element");
            }
            _document->layers.assign(std::make_move_iterator(_topFirst.rbegin()), std::make_move_iterator(_topFirst.rend()));
        }

    private:
        struct Group {
            std::string name;
            double opacity;
            bool visible;
        };

        void parseImage(QXmlStreamReader& xml)
        {
            _document->width = (int)std::clamp<long long>(integerAttributeOf(xml, "width"), 0, kMaxCoordinate);
            _document->height = (int)std::clamp<long long>(integerAttributeOf(xml, "height"), 0, kMaxCoordinate);
            _imageName = attributeOf(xml, "name");
            _imageColorSpace = attributeOf(xml, "colorspacename");
            requireFormat(_imageColorSpace, _path);
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("layers")) {
                    parseLayers(xml);
                } else {
                    xml.skipCurrentElement();
                }
            }
        }

        void parseLayers(QXmlStreamReader& xml)
        {
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("layer")) {
                    parseLayer(xml);
                } else {
                    xml.skipCurrentElement();
                }
            }
        }

        std::vector<std::string> groupNames() const
        {
            std::vector<std::string> names;

            for (const Group& group : _groups) {
                names.push_back(group.name);
            }

            return names;
        }

        void skip(const std::vector<std::string>& groups,
                  const std::string& name,
                  const std::string& reason)
        {
            _document->skipped.push_back(joinedPath(groups, name) + " (" + reason + ")");
        }

        void parseMasks(QXmlStreamReader& xml,
                        const std::vector<std::string>& ownerPath,
                        bool ownerIsGroup,
                        std::vector<MaskRef>* masks)
        {
            while (xml.readNextStartElement()) {
                if (xml.name() != QLatin1String("mask")) {
                    xml.skipCurrentElement();
                    continue;
                }
                const std::string nodeType = attributeOf(xml, "nodetype");
                const std::string name = attributeOf(xml, "name");
                if (nodeType != "transparencymask") {
                    skip(ownerPath, name, layerKindOf(nodeType));
                } else if (ownerIsGroup) {
                    skip(ownerPath, name, "transparency mask on a group");
                } else if (!visibleOf(xml)) {
                    skip(ownerPath, name, "hidden transparency mask");
                } else {
                    MaskRef mask;
                    mask.name = name;
                    mask.filename = attributeOf(xml, "filename");
                    mask.x = integerAttributeOf(xml, "x");
                    mask.y = integerAttributeOf(xml, "y");
                    masks->push_back(mask);
                }
                xml.skipCurrentElement();
            }
        }

        // The member holding `filename` + `suffix` of this image, or empty. Krita files it under
        // the image's name; a file whose name and folder disagree is still found by its suffix.
        std::string memberOf(const std::string& filename,
                             const std::string& suffix) const
        {
            if (filename.empty()) {
                return std::string();
            }
            const std::string expected = _imageName + "/layers/" + filename + suffix;
            if (_archive->zip->contains(expected)) {
                return expected;
            }
            const std::string tail = "/layers/" + filename + suffix;
            for (const Serialization::ZipMember& member : _archive->zip->members()) {
                if (member.name.size() > tail.size() && member.name.compare(member.name.size() - tail.size(), tail.size(), tail) == 0) {
                    return member.name;
                }
            }

            return std::string();
        }

        void parseLayer(QXmlStreamReader& xml)
        {
            const std::string name = attributeOf(xml, "name");
            const std::string nodeType = attributeOf(xml, "nodetype");
            const std::vector<std::string> groups = groupNames();
            std::vector<std::string> path = groups;
            path.push_back(name);

            if (nodeType == "grouplayer") {
                if (_groups.size() >= kMaxGroupDepth) {
                    throw KraError("maindoc.xml nests groups more than " + std::to_string(kMaxGroupDepth) + " deep");
                }
                _groups.push_back(Group { name, opacityOf(xml), visibleOf(xml) });
                std::vector<MaskRef> ignored;
                while (xml.readNextStartElement()) {
                    if (xml.name() == QLatin1String("layers")) {
                        parseLayers(xml);
                    } else if (xml.name() == QLatin1String("masks")) {
                        parseMasks(xml, path, true, &ignored);
                    } else {
                        xml.skipCurrentElement();
                    }
                }
                _groups.pop_back();

                return;
            }
            if (nodeType != "paintlayer") {
                skip(groups, name, layerKindOf(nodeType));
                xml.skipCurrentElement();

                return;
            }

            Layer layer;
            layer.name = name;
            layer.groups = groups;
            for (const Group& group : _groups) {
                layer.groupOpacity *= group.opacity;
                layer.groupVisible = layer.groupVisible && group.visible;
            }
            layer.blendMode = attributeOf(xml, "compositeop");
            if (layer.blendMode.empty()) {
                layer.blendMode = "normal";
            }
            layer.opacity = opacityOf(xml);
            layer.visible = visibleOf(xml);
            // Krita's inherit alpha, its counterpart of clipping, is a disabled alpha channel,
            // which is the last of every colour space it reads here.
            const std::string channelFlags = attributeOf(xml, "channelflags");
            layer.clipping = !channelFlags.empty() && channelFlags.back() == '0';
            const std::string filename = attributeOf(xml, "filename");
            const long long x = integerAttributeOf(xml, "x");
            const long long y = integerAttributeOf(xml, "y");
            const bool animated = !attributeOf(xml, "keyframes").empty();
            std::string colorSpace = attributeOf(xml, "colorspacename");
            if (colorSpace.empty()) {
                colorSpace = _imageColorSpace;
            }

            std::vector<MaskRef> masks;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("masks")) {
                    parseMasks(xml, path, false, &masks);
                } else {
                    xml.skipCurrentElement();
                }
            }
            if (animated) {
                skip(groups, name, "animated layer");

                return;
            }

            const PixelFormat format = requireFormat(colorSpace, "Layer " + joinedPath(groups, name));
            const std::string member = memberOf(filename, "");
            if (member.empty()) {
                throw KraError("there is no pixel data for layer " + joinedPath(groups, name));
            }
            const std::shared_ptr<LayerPixels> pixels = std::make_shared<LayerPixels>();
            pixels->path = _path;
            pixels->name = name;
            pixels->format = format;
            pixels->device = parseDevice(_archive, member, format.pixelSize(), x, y);
            for (const MaskRef& mask : masks) {
                const std::string maskMember = memberOf(mask.filename, ".pixelselection");
                if (maskMember.empty()) {
                    skip(path, mask.name, "transparency mask without pixels");
                    continue;
                }
                pixels->masks.push_back(parseDevice(_archive, maskMember, 1, mask.x, mask.y));
            }

            placePlane(pixels, &layer);
            _topFirst.push_back(std::move(layer));
        }

        // The plane covers the stored tiles, and the canvas too when the pixel outside them is
        // not transparent, rounded out to whole tiles so that reads fall on Krita's tile grid.
        void placePlane(const std::shared_ptr<LayerPixels>& pixels,
                        Layer* layer) const
        {
            const Device& device = *pixels->device;
            const long long tw = device.tileWidth;
            const long long th = device.tileHeight;
            bool any = false;
            long long x1 = 0;
            long long y1 = 0;
            long long x2 = 0;
            long long y2 = 0;
            const auto include = [&](long long ax1, long long ay1, long long ax2, long long ay2) {
                x1 = any ? std::min(x1, ax1) : ax1;
                y1 = any ? std::min(y1, ay1) : ay1;
                x2 = any ? std::max(x2, ax2) : ax2;
                y2 = any ? std::max(y2, ay2) : ay2;
                any = true;
            };

            for (const std::pair<const std::pair<long long, long long>, Tile>& tile : device.tiles) {
                const long long left = device.x + tile.first.second * tw;
                const long long top = device.y + tile.first.first * th;
                include(left, top, left + tw, top + th);
            }
            float defaultRgba[4];
            toStraightRgba(pixels->format, device.defaultPixel.data(), 1, defaultRgba);
            if (defaultRgba[3] > 0.f) {
                include(device.x + floorDiv(-device.x, tw) * tw,
                        device.y + floorDiv(-device.y, th) * th,
                        device.x + (floorDiv(_document->width - 1 - device.x, tw) + 1) * tw,
                        device.y + (floorDiv(_document->height - 1 - device.y, th) + 1) * th);
            }
            if (!any) {
                return;
            }

            pixels->planeX = x1;
            pixels->planeY = y1;
            layer->plane.x = (int)x1;
            layer->plane.y = (int)y1;
            layer->plane.width = (int)(x2 - x1);
            layer->plane.height = (int)(y2 - y1);
            layer->plane.channels = { "R", "G", "B", "A" };
            layer->plane.type = pixels->format.type;
            layer->plane.tileWidth = device.tileWidth;
            layer->plane.tileHeight = device.tileHeight;
            layer->plane.loadRegion = [pixels](const Region& region, unsigned char* dst, std::string* loadError) {
                return loadLayerRegion(*pixels, region, dst, loadError);
            };
        }

        std::shared_ptr<const Archive> _archive;
        std::string _path;
        LayeredDocument* _document;
        std::string _imageName;
        std::string _imageColorSpace;
        std::vector<Group> _groups;
        std::vector<Layer> _topFirst;
    };
} // anonymous namespace

std::shared_ptr<LayeredDocument>
readKra(const std::string& path,
        std::string* error)
{
    const std::shared_ptr<Archive> archive = std::make_shared<Archive>();
    std::string readError;
    std::string zipError;

    if (!readArchive(path, archive.get(), &readError, &zipError)) {
        return failed(error, readError.empty() ? path + " is not a Krita file: " + zipError : readError);
    }

    const std::shared_ptr<LayeredDocument> document = std::make_shared<LayeredDocument>();
    const std::size_t archiveBytes = archive->bytes.size();
    document->readerBytes = [archiveBytes]() { return archiveBytes; };
    try {
        if (archive->zip->contains("mimetype")) {
            const std::span<const std::byte> mime = archive->zip->data("mimetype");
            std::string type(reinterpret_cast<const char*>(mime.data()), mime.size());
            type.erase(type.find_last_not_of(" \t\r\n") + 1);
            if (type != kMimeType) {
                return failed(error, path + " is not a Krita file: its mime type is " + type);
            }
        }
        if (!archive->zip->contains("maindoc.xml")) {
            return failed(error, path + " is not a Krita file: it has no maindoc.xml");
        }
        const std::span<const std::byte> mainDoc = archive->zip->data("maindoc.xml");
        QXmlStreamReader xml(QByteArray::fromRawData(reinterpret_cast<const char*>(mainDoc.data()), (qsizetype)mainDoc.size()));
        Parser parser(archive, path, document.get());
        parser.parse(xml);

        if (archive->zip->contains("mergedimage.png")) {
            PngInfo info;
            std::string pngError;
            if (!pngInfoOf(*archive, "mergedimage.png", &info, &pngError)) {
                return failed(error, path + ": " + pngError);
            }
            document->composite.width = info.width;
            document->composite.height = info.height;
            document->composite.channels = info.channels;
            document->composite.type = info.type;
            document->composite.load = [archive, info](std::vector<unsigned char>* pixels, std::string* loadError) {
                if (!decodePng(*archive, "mergedimage.png", info, false, pixels, loadError)) {
                    return false;
                }
                countDecodedRows((std::uint64_t)info.height);

                return true;
            };
        }
    } catch (const KraError& e) {
        const std::string what = e.what();

        return failed(error, what.compare(0, path.size(), path) == 0 ? what : path + ": " + what);
    } catch (const Serialization::ZipError& e) {
        return failed(error, path + ": " + e.what());
    } catch (const std::bad_alloc&) {
        return failed(error, path + ": out of memory reading the layer tiles");
    }

    std::string finaliseError;
    if (!document->finalise(&finaliseError)) {
        return failed(error, path + ": " + finaliseError);
    }

    return document;
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
