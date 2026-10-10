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

#include "OiioReadSupport.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <vector>

#include <ofxMetadata.h>

#include "Engine/LayerRegistry.h"

NATRON_NAMESPACE_ENTER

namespace OiioReadSupport {
namespace {
    struct CacheEntry {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
        std::shared_ptr<const Header> header;
    };

    // Bounds the memory held by a long session that walks many sequences.
    const std::size_t kMaxCachedHeaders = 4096;

    // Scanline reads are made in blocks of about this many bytes of float pixels.
    const std::size_t kScanlineBlockBytes = std::size_t(16) << 20;

    std::mutex g_cacheMutex;
    std::map<std::string, CacheEntry> g_cache;

    bool
    fail(std::string* error,
         const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    std::string
    openError(const std::string& path)
    {
        const std::string detail = OIIO::geterror();

        return detail.empty() ? "Could not open " + path : detail;
    }

    // A block of decoded rows of the file and where the part inside the window lands.
    struct Sink {
        float* dst;
        std::size_t rowStride;
        std::size_t channels;
        int64_t windowX1;
        int64_t windowX2;
        int64_t windowFileTop; // first file row of the window
        int64_t windowFileBottom; // one past the last file row of the window

        // `block` holds file pixels [bx1, bx2) x [by1, by2), rows ascending in file order.
        void store(const float* block,
                   int64_t bx1,
                   int64_t bx2,
                   int64_t by1,
                   int64_t by2) const
        {
            const int64_t x1 = std::max(bx1, windowX1);
            const int64_t x2 = std::min(bx2, windowX2);
            const int64_t y1 = std::max(by1, windowFileTop);
            const int64_t y2 = std::min(by2, windowFileBottom);
            if (x1 >= x2 || y1 >= y2) {
                return;
            }
            const std::size_t blockWidth = (std::size_t)(bx2 - bx1);
            const std::size_t rowBytes = (std::size_t)(x2 - x1) * channels * sizeof(float);
            for (int64_t fy = y1; fy < y2; ++fy) {
                // The window's last file row is its first (bottom) row.
                const std::size_t dstRow = (std::size_t)(windowFileBottom - 1 - fy);
                const float* src = block + ((std::size_t)(fy - by1) * blockWidth + (std::size_t)(x1 - bx1)) * channels;
                float* out = dst + dstRow * rowStride + (std::size_t)(x1 - windowX1) * channels;
                std::memcpy(out, src, rowBytes);
            }
        }
    };
} // anonymous namespace

std::shared_ptr<const Header>
readHeader(const std::string& path,
           std::string* error)
{
    std::error_code ec;
    const std::filesystem::path fsPath = std::filesystem::path(path);
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(fsPath, ec);
    const std::uintmax_t size = ec ? 0 : std::filesystem::file_size(fsPath, ec);
    if (ec) {
        fail(error, "Could not read " + path + ": " + ec.message());

        return std::shared_ptr<const Header>();
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        const std::map<std::string, CacheEntry>::const_iterator it = g_cache.find(path);
        if (it != g_cache.end() && it->second.mtime == mtime && it->second.size == size) {
            return it->second.header;
        }
    }

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    if (!input) {
        fail(error, openError(path));

        return std::shared_ptr<const Header>();
    }

    std::shared_ptr<Header> header = std::make_shared<Header>();
    header->subimages.push_back(input->spec());
    for (int s = 1; input->seek_subimage(s, 0); ++s) {
        header->subimages.push_back(input->spec());
    }
    input->close();

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (g_cache.size() >= kMaxCachedHeaders && g_cache.find(path) == g_cache.end()) {
            g_cache.clear();
        }
        CacheEntry& entry = g_cache[path];
        entry.mtime = mtime;
        entry.size = size;
        entry.header = header;
    }

    return header;
}

void
clearHeaderCache()
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.clear();
}

RectI
dataWindowOf(const OIIO::ImageSpec& spec)
{
    const int displayTop = spec.full_y + spec.full_height;

    return RectI(spec.x, displayTop - (spec.y + spec.height), spec.x + spec.width, displayTop - spec.y);
}

namespace {
    // OIIO names a part that has no name of its own "subimage" followed by digits.
    std::string
    partNameOf(const OIIO::ImageSpec& spec)
    {
        const OIIO::ParamValue* value = spec.find_attribute("oiio:subimagename", OIIO::TypeDesc::STRING);

        if (!value || !value->data()) {
            return std::string();
        }
        const char* const text = *(const char* const*)value->data();
        const std::string name = text ? std::string(text) : std::string();
        static const std::string synthesised("subimage");
        if (name.size() > synthesised.size() && name.compare(0, synthesised.size(), synthesised) == 0 && std::all_of(name.begin() + (std::ptrdiff_t)synthesised.size(), name.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return std::string();
        }

        return name;
    }

    // Whether `flatName` is the channel `channel` of the layer groupChannelNames named `layerID`.
    bool
    isChannelOf(const std::string& flatName,
                const std::string& layerID,
                const std::string& channel)
    {
        if (flatName.find('.') != std::string::npos) {
            return flatName.size() == layerID.size() + 1 + channel.size() && flatName.compare(0, layerID.size(), layerID) == 0 && flatName[layerID.size()] == '.' && flatName.compare(layerID.size() + 1, std::string::npos, channel) == 0;
        }

        return flatName == channel && (layerID == channel || (layerID == "depth" && channel == "Z"));
    }
} // anonymous namespace

void
fileLayers(const Header& header,
           std::vector<FileLayer>* layers)
{
    if (!layers) {
        return;
    }
    layers->clear();

    const bool multiPart = header.subimages.size() > 1;
    std::set<std::string> seen;
    for (std::size_t s = 0; s < header.subimages.size(); ++s) {
        const OIIO::ImageSpec& spec = header.subimages[s];
        const std::string part = multiPart ? partNameOf(spec) : std::string();

        std::vector<std::string> flat;
        std::vector<int> fileIndex;
        const int nNamed = std::min(spec.nchannels, (int)spec.channelnames.size());
        for (int i = 0; i < nNamed; ++i) {
            const std::string& name = spec.channelnames[(std::size_t)i];
            if (name.empty()) {
                continue;
            }
            flat.push_back((part.empty() || name.find('.') != std::string::npos) ? name : part + "." + name);
            fileIndex.push_back(i);
        }

        std::vector<ImageLayerDesc> grouped;
        LayerRegistry::groupChannelNames(flat, &grouped);

        std::vector<bool> used(flat.size(), false);
        for (std::size_t g = 0; g < grouped.size(); ++g) {
            const ImageLayerDesc& desc = grouped[g];
            if (desc.isColorLayer() || !LayerRegistry::validate(desc, true, NULL) || seen.count(desc.getLayerID()) > 0) {
                continue;
            }

            FileLayer layer;
            layer.desc = desc;
            layer.subimage = (int)s;
            const std::vector<std::string>& channels = desc.getChannels();
            for (std::size_t c = 0; c < channels.size(); ++c) {
                for (std::size_t f = 0; f < flat.size(); ++f) {
                    if (!used[f] && isChannelOf(flat[f], desc.getLayerID(), channels[c])) {
                        used[f] = true;
                        layer.channels.push_back(fileIndex[f]);
                        break;
                    }
                }
            }
            if (layer.channels.size() != channels.size()) {
                continue;
            }
            seen.insert(desc.getLayerID());
            layers->push_back(layer);
        }
    }
} // fileLayers

bool
decode(const std::string& path,
       int subimage,
       int chbegin,
       int chend,
       const RectI& window,
       float* dst,
       std::size_t rowStride,
       std::string* error)
{
    const std::shared_ptr<const Header> header = readHeader(path, error);
    if (!header) {
        return false;
    }
    if (subimage < 0 || (std::size_t)subimage >= header->subimages.size()) {
        return fail(error, "No subimage " + std::to_string(subimage) + " in " + path);
    }
    const OIIO::ImageSpec& spec = header->subimages[(std::size_t)subimage];
    if (spec.depth > 1) {
        return fail(error, "Volume images are not supported: " + path);
    }
    if (chbegin < 0 || chend <= chbegin || chend > spec.nchannels) {
        return fail(error, "Channel range out of bounds for " + path);
    }
    const RectI dataWindow = dataWindowOf(spec);
    if (window.x1 < dataWindow.x1 || window.x2 > dataWindow.x2 || window.y1 < dataWindow.y1 || window.y2 > dataWindow.y2 || window.x2 < window.x1 || window.y2 < window.y1) {
        return fail(error, "Requested window is outside the data window of " + path);
    }
    if (window.x2 == window.x1 || window.y2 == window.y1) {
        return true;
    }
    if (!dst) {
        return fail(error, "No destination buffer");
    }
    const std::size_t channels = (std::size_t)(chend - chbegin);
    if (rowStride < (std::size_t)window.width() * channels) {
        return fail(error, "Row stride is smaller than a window row");
    }

    const int64_t displayTop = (int64_t)spec.full_y + spec.full_height;
    Sink sink;
    sink.dst = dst;
    sink.rowStride = rowStride;
    sink.channels = channels;
    sink.windowX1 = window.x1;
    sink.windowX2 = window.x2;
    sink.windowFileTop = displayTop - window.y2;
    sink.windowFileBottom = displayTop - window.y1;

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    if (!input) {
        return fail(error, openError(path));
    }

    const int64_t dataX1 = spec.x;
    const int64_t dataX2 = (int64_t)spec.x + spec.width;
    const int64_t dataY2 = (int64_t)spec.y + spec.height;
    std::vector<float> block;

    if (spec.tile_width > 0 && spec.tile_height > 0) {
        const int64_t tw = spec.tile_width;
        const int64_t th = spec.tile_height;
        block.resize((std::size_t)tw * (std::size_t)th * channels);
        const int64_t firstTileY = spec.y + (sink.windowFileTop - spec.y) / th * th;
        const int64_t firstTileX = dataX1 + (sink.windowX1 - dataX1) / tw * tw;
        for (int64_t ty = firstTileY; ty < sink.windowFileBottom; ty += th) {
            const int64_t ty2 = std::min(ty + th, dataY2);
            for (int64_t tx = firstTileX; tx < sink.windowX2; tx += tw) {
                const int64_t tx2 = std::min(tx + tw, dataX2);
                if (!input->read_tiles(subimage, 0, (int)tx, (int)tx2, (int)ty, (int)ty2, 0, 1, chbegin, chend, OIIO::TypeDesc::FLOAT, block.data())) {
                    return fail(error, input->geterror());
                }
                sink.store(block.data(), tx, tx2, ty, ty2);
            }
        }
    } else {
        const std::size_t rowBytes = (std::size_t)spec.width * channels * sizeof(float);
        const int64_t blockRows = std::max<int64_t>(1, (int64_t)(kScanlineBlockBytes / std::max<std::size_t>(rowBytes, 1)));
        const int64_t rowsInWindow = sink.windowFileBottom - sink.windowFileTop;
        block.resize((std::size_t)std::min(blockRows, rowsInWindow) * (std::size_t)spec.width * channels);
        for (int64_t y = sink.windowFileTop; y < sink.windowFileBottom; y += blockRows) {
            const int64_t y2 = std::min(y + blockRows, sink.windowFileBottom);
            if (!input->read_scanlines(subimage, 0, (int)y, (int)y2, 0, chbegin, chend, OIIO::TypeDesc::FLOAT, block.data())) {
                return fail(error, input->geterror());
            }
            sink.store(block.data(), dataX1, dataX2, y, y2);
        }
    }
    input->close();

    return true;
}

namespace {
    struct KeyPrefix {
        const char* attribute;
        const char* key;
    };

    const KeyPrefix kKeyPrefixes[] = {
        { "Exif:", "exif/" },
        { "GPS:", "exif/GPS:" },
        { "oiio:", "oiio/" },
        { "dpx:", "dpx/" },
        { "cineon:", "cin/" },
        { "tiff:", "tiff/" },
        { "openexr:", "exr/" }
    };

    bool
    isOpenExrPath(const std::string& path)
    {
        const std::size_t dot = path.rfind('.');

        if (dot == std::string::npos) {
            return false;
        }
        std::string extension = path.substr(dot + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });

        return extension == "exr" || extension == "sxr" || extension == "mxr";
    }

    // Empty when the attribute has no place in the metadata.
    std::string
    keyOf(const std::string& name,
          bool openExr)
    {
        if (name == "smpte:TimeCode") {
            return kOfxMetadataKeyTimecode;
        }
        if (name == "smpte:KeyCode") {
            return kOfxMetadataKeyEdgecode;
        }
        for (const KeyPrefix& prefix : kKeyPrefixes) {
            const std::size_t length = std::strlen(prefix.attribute);
            if (name.compare(0, length, prefix.attribute) == 0) {
                return std::string(prefix.key) + name.substr(length);
            }
        }
        if (name.empty() || name.find(':') != std::string::npos) {
            return std::string();
        }

        return std::string(openExr ? kOfxMetadataKeyPrefixExr : kOfxMetadataKeyPrefixExif) + name;
    }

    template <typename T>
    void
    widen(const void* data,
          std::vector<double>* values)
    {
        const T* typed = static_cast<const T*>(data);

        for (std::size_t i = 0; i < values->size(); ++i) {
            (*values)[i] = (double)typed[i];
        }
    }

    bool
    valueOf(const OIIO::ParamValue& attribute,
            ImageMetadata::Value* value)
    {
        const OIIO::TypeDesc type = attribute.type();
        const std::size_t count = (std::size_t)type.basevalues() * (std::size_t)attribute.nvalues();

        if (count == 0 || !attribute.data()) {
            return false;
        }
        if (type.vecsemantics == OIIO::TypeDesc::TIMECODE || type.vecsemantics == OIIO::TypeDesc::KEYCODE) {
            *value = attribute.get_string();

            return true;
        }
        if (type.basetype == OIIO::TypeDesc::STRING) {
            const OIIO::ustring* strings = static_cast<const OIIO::ustring*>(attribute.data());
            std::string joined;
            for (std::size_t i = 0; i < count; ++i) {
                if (i > 0) {
                    joined += ", ";
                }
                joined += strings[i].string();
            }
            *value = joined;

            return true;
        }

        std::vector<double> numbers(count);
        bool integral = true;
        const void* data = attribute.data();
        switch (type.basetype) {
        case OIIO::TypeDesc::UINT8:
            widen<uint8_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::INT8:
            widen<int8_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::UINT16:
            widen<uint16_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::INT16:
            widen<int16_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::UINT32:
            widen<uint32_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::INT32:
            widen<int32_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::UINT64:
            widen<uint64_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::INT64:
            widen<int64_t>(data, &numbers);
            break;
        case OIIO::TypeDesc::FLOAT:
            widen<float>(data, &numbers);
            integral = false;
            break;
        case OIIO::TypeDesc::DOUBLE:
            widen<double>(data, &numbers);
            integral = false;
            break;
        default:
            return false;
        }

        if (type.vecsemantics == OIIO::TypeDesc::RATIONAL && count == 2) {
            if (numbers[1] == 0.) {
                return false;
            }
            *value = numbers[0] / numbers[1];

            return true;
        }
        if (integral) {
            for (double number : numbers) {
                if (number < (double)std::numeric_limits<int>::min() || number > (double)std::numeric_limits<int>::max()) {
                    integral = false;
                    break;
                }
            }
        }
        if (integral) {
            std::vector<int> ints(count);
            for (std::size_t i = 0; i < count; ++i) {
                ints[i] = (int)numbers[i];
            }
            if (count == 1) {
                *value = ints[0];
            } else {
                *value = ints;
            }
        } else if (count == 1) {
            *value = numbers[0];
        } else {
            *value = numbers;
        }

        return true;
    } // valueOf
} // namespace

ImageMetadata
attributeMetadata(const std::string& path,
                  const OIIO::ImageSpec& spec)
{
    ImageMetadata metadata;
    const bool openExr = isOpenExrPath(path);

    for (const OIIO::ParamValue& attribute : spec.extra_attribs) {
        const std::string key = keyOf(attribute.name().string(), openExr);
        ImageMetadata::Value value;
        if (!key.empty() && valueOf(attribute, &value)) {
            metadata.set(key, value);
        }
    }

    return metadata;
}
} // namespace OiioReadSupport

NATRON_NAMESPACE_EXIT
