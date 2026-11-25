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

#include "LayeredDocument.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstring>
#include <set>

#include <OpenImageIO/ustring.h>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
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
    lowerCase(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        return s;
    }

    std::string
    joined(const std::vector<std::string>& parts,
           const char* separator)
    {
        std::string result;

        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                result += separator;
            }
            result += parts[i];
        }

        return result;
    }

    std::string
    sanitisedName(const std::string& name)
    {
        std::string result = name;

        for (char& c : result) {
            const unsigned char u = (unsigned char)c;
            if (u >= 0x80 || (!std::isalnum(u) && c != '_')) {
                c = '_';
            }
        }

        return result;
    }

    // Names LayerRegistry refuses or keeps for its own layers, and those OiioReadSupport takes for
    // the colour plane, in lower case.
    bool
    isReservedId(const std::string& id)
    {
        static const char* const reserved[] = {
            "rgba", "rgb", "alpha", "xy", "color", "none", "all", "a",
            "forward", "backward", "disparityl", "disparityr", "depth", "primid", "normal"
        };
        const std::string lower = lowerCase(id);

        return std::any_of(std::begin(reserved), std::end(reserved), [&lower](const char* name) { return lower == name; });
    }

    int
    typeRank(OIIO::TypeDesc type)
    {
        switch (type.basetype) {
        case OIIO::TypeDesc::UINT8:
            return 0;
        case OIIO::TypeDesc::UINT16:
            return 1;
        case OIIO::TypeDesc::HALF:
            return 2;
        default:
            return 3;
        }
    }

    bool
    intersect(int x,
              int y,
              int width,
              int height,
              int canvasWidth,
              int canvasHeight,
              int* wx,
              int* wy,
              int* ww,
              int* wh)
    {
        const long long x1 = std::max<long long>(x, 0);
        const long long y1 = std::max<long long>(y, 0);
        const long long x2 = std::min<long long>((long long)x + width, canvasWidth);
        const long long y2 = std::min<long long>((long long)y + height, canvasHeight);

        if (x2 <= x1 || y2 <= y1) {
            return false;
        }
        *wx = (int)x1;
        *wy = (int)y1;
        *ww = (int)(x2 - x1);
        *wh = (int)(y2 - y1);

        return true;
    }

    std::string
    pathOf(const Layer& layer)
    {
        std::vector<std::string> path = layer.groups;

        path.push_back(layer.name);

        return joined(path, "/");
    }

    std::atomic<std::uint64_t> g_decodedRows { 0 };

    void
    stringArrayAttribute(OIIO::ImageSpec* spec,
                         const std::string& name,
                         const std::vector<std::string>& values)
    {
        if (values.empty()) {
            return;
        }
        std::vector<OIIO::ustring> strings;
        for (const std::string& value : values) {
            strings.push_back(OIIO::ustring(value));
        }
        spec->attribute(name, OIIO::TypeDesc(OIIO::TypeDesc::STRING, (int)strings.size()), strings.data());
    }
} // anonymous namespace

void
countDecodedRows(std::uint64_t rows)
{
    g_decodedRows.fetch_add(rows, std::memory_order_relaxed);
}

std::uint64_t
decodedRows()
{
    return g_decodedRows.load(std::memory_order_relaxed);
}

std::vector<std::string>
sanitisedLayerIds(const std::vector<std::vector<std::string>>& paths)
{
    std::vector<std::string> ids;
    std::set<std::string> taken;

    for (const std::vector<std::string>& path : paths) {
        std::vector<std::string> parts;
        for (const std::string& name : path) {
            const std::string part = sanitisedName(name);
            if (!part.empty()) {
                parts.push_back(part);
            }
        }
        std::string base = joined(parts, "_");
        const std::size_t first = base.find_first_not_of('_');
        base = first == std::string::npos ? std::string() : base.substr(first, base.find_last_not_of('_') - first + 1);
        if (base.empty()) {
            base = "layer";
        }

        std::string id = base;
        for (int suffix = 2; taken.count(id) > 0 || isReservedId(id); ++suffix) {
            id = base + "_" + std::to_string(suffix);
        }
        taken.insert(id);
        ids.push_back(id);
    }

    return ids;
}

void
toRgba(const float* src,
       std::size_t count,
       const std::vector<std::string>& names,
       float* dst)
{
    int index[4] = { -1, -1, -1, -1 };
    int gray = -1;
    const std::size_t nChannels = names.size();

    for (std::size_t c = 0; c < nChannels; ++c) {
        const std::string name = lowerCase(names[c]);
        if (name.size() != 1) {
            continue;
        }
        const std::size_t slot = std::string("rgba").find(name[0]);
        if (slot != std::string::npos && index[slot] < 0) {
            index[slot] = (int)c;
        } else if ((name[0] == 'y' || name[0] == 'i') && gray < 0) {
            gray = (int)c;
        }
    }
    for (int k = 0; k < 3; ++k) {
        if (index[k] < 0) {
            index[k] = gray;
        }
    }
    for (std::size_t p = 0; p < count; ++p) {
        const float* in = src + p * nChannels;
        float* out = dst + p * 4;
        for (int k = 0; k < 4; ++k) {
            out[k] = index[k] >= 0 ? in[index[k]] : (k == 3 ? 1.f : 0.f);
        }
    }
}

bool
convertValues(OIIO::TypeDesc from,
              const void* src,
              OIIO::TypeDesc to,
              void* dst,
              std::size_t count)
{
    const std::size_t chunk = (std::size_t)INT_MAX / 8;
    const unsigned char* in = static_cast<const unsigned char*>(src);
    unsigned char* out = static_cast<unsigned char*>(dst);

    for (std::size_t done = 0; done < count; done += chunk) {
        const std::size_t n = std::min(chunk, count - done);
        if (!OIIO::convert_pixel_values(from, in + done * from.size(), to, out + done * to.size(), (int)n)) {
            return false;
        }
    }

    return true;
}

OIIO::TypeDesc
deeperType(OIIO::TypeDesc a,
           OIIO::TypeDesc b)
{
    static const OIIO::TypeDesc byRank[] = { OIIO::TypeDesc::UINT8, OIIO::TypeDesc::UINT16, OIIO::TypeDesc::HALF, OIIO::TypeDesc::FLOAT };

    return byRank[std::max(typeRank(a), typeRank(b))];
}

bool
LayeredDocument::finalise(std::string* error)
{
    if (width <= 0 || height <= 0) {
        return fail(error, "The document has an empty canvas");
    }

    std::vector<Layer> kept;
    for (Layer& layer : layers) {
        const bool inside = intersect(layer.plane.x, layer.plane.y, layer.plane.width, layer.plane.height, width, height, &layer.windowX, &layer.windowY, &layer.windowWidth, &layer.windowHeight);
        if (!layer.plane.decodable() || layer.plane.width <= 0 || layer.plane.height <= 0) {
            skipped.push_back(pathOf(layer) + " (empty)");
        } else if (!inside) {
            skipped.push_back(pathOf(layer) + " (outside the canvas)");
        } else {
            kept.push_back(std::move(layer));
        }
    }
    layers = std::move(kept);

    std::vector<std::vector<std::string>> paths;
    for (const Layer& layer : layers) {
        std::vector<std::string> path = layer.groups;
        path.push_back(layer.name);
        paths.push_back(path);
    }
    const std::vector<std::string> ids = sanitisedLayerIds(paths);
    for (std::size_t i = 0; i < layers.size(); ++i) {
        layers[i].id = ids[i];
    }

    _flattened = !composite.decodable() || !intersect(composite.x, composite.y, composite.width, composite.height, width, height, &_compositeX, &_compositeY, &_compositeWidth, &_compositeHeight);
    if (_flattened) {
        OIIO::TypeDesc type = OIIO::TypeDesc::UINT8;
        for (const Layer& layer : layers) {
            type = deeperType(type, layer.plane.type);
        }
        composite = Plane();
        composite.width = width;
        composite.height = height;
        composite.channels = { "R", "G", "B", "A" };
        composite.type = type;
        _compositeX = 0;
        _compositeY = 0;
        _compositeWidth = width;
        _compositeHeight = height;
    }

    _caches.clear();
    for (int s = 0; s < subimageCount(); ++s) {
        _caches.push_back(std::make_unique<Cache>());
    }

    return true;
}

const Plane&
LayeredDocument::planeOf(int subimage) const
{
    return subimage == 0 ? composite : layers[(std::size_t)subimage - 1].plane;
}

void
LayeredDocument::windowOf(int subimage,
                          int* x,
                          int* y,
                          int* w,
                          int* h) const
{
    if (subimage == 0) {
        *x = _compositeX;
        *y = _compositeY;
        *w = _compositeWidth;
        *h = _compositeHeight;
    } else {
        const Layer& layer = layers[(std::size_t)subimage - 1];
        *x = layer.windowX;
        *y = layer.windowY;
        *w = layer.windowWidth;
        *h = layer.windowHeight;
    }
}

OIIO::ImageSpec
LayeredDocument::specOf(int subimage) const
{
    const Plane& plane = planeOf(subimage);
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    windowOf(subimage, &x, &y, &w, &h);
    OIIO::ImageSpec spec(w, h, (int)plane.channels.size(), plane.type);
    spec.x = x;
    spec.y = y;
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = width;
    spec.full_height = height;
    spec.channelnames = plane.channels;
    if (plane.tiled()) {
        spec.tile_width = plane.tileWidth;
        spec.tile_height = plane.tileHeight;
        spec.tile_depth = 1;
    }
    spec.alpha_channel = -1;
    for (std::size_t c = 0; c < plane.channels.size(); ++c) {
        if (plane.channels[c] == "A") {
            spec.alpha_channel = (int)c;
        }
    }
    if (subimage > 0) {
        spec.attribute("oiio:subimagename", layers[(std::size_t)subimage - 1].id);
    } else {
        describe(&spec);
    }
    spec.attribute("oiio:subimages", subimageCount());

    return spec;
}

bool
LayeredDocument::pixelsOf(int subimage,
                          const std::vector<unsigned char>** pixels,
                          std::string* error) const
{
    Cache& cache = *_caches[(std::size_t)subimage];
    std::lock_guard<std::mutex> lock(cache.mutex);

    if (!cache.loaded) {
        const Plane& plane = planeOf(subimage);
        std::vector<unsigned char> loaded;
        if (!plane.load(&loaded, error)) {
            return false;
        }
        const std::size_t expected = (std::size_t)plane.width * (std::size_t)plane.height * plane.channels.size() * plane.type.size();
        if (loaded.size() != expected) {
            return fail(error, "A layer decoded to " + std::to_string(loaded.size()) + " bytes instead of " + std::to_string(expected));
        }
        cache.pixels = std::move(loaded);
        cache.loaded = true;
    }
    *pixels = &cache.pixels;

    return true;
}

bool
LayeredDocument::readRegion(int subimage,
                            int x1,
                            int y1,
                            int x2,
                            int y2,
                            void* dst,
                            std::string* error) const
{
    if (subimage < 0 || subimage >= subimageCount() || _caches.size() != (std::size_t)subimageCount()) {
        return fail(error, "No subimage " + std::to_string(subimage));
    }
    int wx = 0;
    int wy = 0;
    int ww = 0;
    int wh = 0;
    windowOf(subimage, &wx, &wy, &ww, &wh);
    if (x1 < wx || x2 > wx + ww || x2 < x1 || y1 < wy || y2 > wy + wh || y2 < y1) {
        return fail(error, "Region out of range");
    }
    if (x1 == x2 || y1 == y2) {
        return true;
    }
    if (subimage == 0 && _flattened) {
        return flatten(x1, y1, x2, y2, dst, error);
    }

    const Plane& plane = planeOf(subimage);
    if (plane.loadRegion) {
        Region region;
        region.x1 = (int)((long long)x1 - plane.x);
        region.y1 = (int)((long long)y1 - plane.y);
        region.x2 = (int)((long long)x2 - plane.x);
        region.y2 = (int)((long long)y2 - plane.y);

        return plane.loadRegion(region, static_cast<unsigned char*>(dst), error);
    }

    const std::vector<unsigned char>* pixels = nullptr;
    if (!pixelsOf(subimage, &pixels, error)) {
        return false;
    }
    const std::size_t pixelBytes = plane.channels.size() * plane.type.size();
    const std::size_t sourceRowBytes = (std::size_t)plane.width * pixelBytes;
    const std::size_t rowBytes = (std::size_t)(x2 - x1) * pixelBytes;
    const std::size_t column = (std::size_t)((long long)x1 - plane.x) * pixelBytes;
    unsigned char* out = static_cast<unsigned char*>(dst);
    for (int y = y1; y < y2; ++y) {
        std::memcpy(out + (std::size_t)(y - y1) * rowBytes, pixels->data() + (std::size_t)((long long)y - plane.y) * sourceRowBytes + column, rowBytes);
    }

    return true;
}

bool
LayeredDocument::flatten(int x1,
                         int y1,
                         int x2,
                         int y2,
                         void* dst,
                         std::string* error) const
{
    const std::size_t width = (std::size_t)(x2 - x1);
    std::vector<float> out(width * (std::size_t)(y2 - y1) * 4, 0.f);

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = layers[i];
        const double opacity = std::clamp(layer.opacity * layer.groupOpacity, 0., 1.);
        if (!layer.visible || !layer.groupVisible || opacity <= 0.) {
            continue;
        }
        const int ix1 = std::max(x1, layer.windowX);
        const int iy1 = std::max(y1, layer.windowY);
        const int ix2 = std::min(x2, layer.windowX + layer.windowWidth);
        const int iy2 = std::min(y2, layer.windowY + layer.windowHeight);
        if (ix2 <= ix1 || iy2 <= iy1) {
            continue;
        }
        const Plane& plane = layer.plane;
        const std::size_t partWidth = (std::size_t)(ix2 - ix1);
        const std::size_t count = partWidth * (std::size_t)(iy2 - iy1);
        const std::size_t nChannels = plane.channels.size();
        std::vector<unsigned char> stored(count * nChannels * plane.type.size());
        if (!readRegion((int)i + 1, ix1, iy1, ix2, iy2, stored.data(), error)) {
            return false;
        }
        std::vector<float> values(count * nChannels);
        if (!convertValues(plane.type, stored.data(), OIIO::TypeDesc::FLOAT, values.data(), values.size())) {
            return fail(error, "Could not convert the pixels of layer " + layer.name);
        }
        std::vector<float> rgba(count * 4);
        toRgba(values.data(), count, plane.channels, rgba.data());

        const float o = (float)opacity;
        for (int y = iy1; y < iy2; ++y) {
            for (int x = ix1; x < ix2; ++x) {
                const float* s = &rgba[((std::size_t)(y - iy1) * partWidth + (std::size_t)(x - ix1)) * 4];
                float* d = &out[((std::size_t)(y - y1) * width + (std::size_t)(x - x1)) * 4];
                const float keep = 1.f - s[3] * o;
                for (int k = 0; k < 4; ++k) {
                    d[k] = s[k] * o + d[k] * keep;
                }
            }
        }
    }

    if (!convertValues(OIIO::TypeDesc::FLOAT, out.data(), composite.type, dst, out.size())) {
        return fail(error, "Could not convert the flattened composite");
    }

    return true;
}

void
LayeredDocument::describe(OIIO::ImageSpec* spec) const
{
    const std::string prefix = kLayeredMetadataPrefix;
    std::vector<std::string> ids;

    for (const Layer& layer : layers) {
        ids.push_back(layer.id);
        const std::string key = prefix + layer.id + "/";
        spec->attribute(key + "name", layer.name);
        spec->attribute(key + "blendMode", layer.blendMode);
        spec->attribute(key + "opacity", (float)layer.opacity);
        spec->attribute(key + "visible", layer.visible ? 1 : 0);
        spec->attribute(key + "clipping", layer.clipping ? 1 : 0);
        spec->attribute(key + "group", joined(layer.groups, "/"));
    }
    stringArrayAttribute(spec, prefix + "layers", ids);
    stringArrayAttribute(spec, prefix + "skipped", skipped);
    spec->attribute(prefix + "composite", _flattened ? "flattened" : "stored");
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
