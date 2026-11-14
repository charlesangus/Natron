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

#include "UtilityTextures.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <pxr/base/gf/half.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const char kUdimToken[] = "<UDIM>";

float
srgbToLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float
componentAt(const unsigned char* pixel,
            HioType type,
            int component)
{
    switch (type) {
    case HioTypeUnsignedByte:
    case HioTypeUnsignedByteSRGB:
        return pixel[component] / 255.f;
    case HioTypeUnsignedShort: {
        std::uint16_t v;

        std::memcpy(&v, pixel + 2 * component, sizeof(v));

        return v / 65535.f;
    }
    case HioTypeHalfFloat: {
        GfHalf v;

        std::memcpy(&v, pixel + 2 * component, sizeof(v));

        return static_cast<float>(v);
    }
    case HioTypeFloat: {
        float v;

        std::memcpy(&v, pixel + 4 * component, sizeof(v));

        return v;
    }
    default:
        return 0.f;
    }
}

int
componentSize(HioType type)
{
    switch (type) {
    case HioTypeUnsignedByte:
    case HioTypeUnsignedByteSRGB:
        return 1;
    case HioTypeUnsignedShort:
    case HioTypeHalfFloat:
        return 2;
    case HioTypeFloat:
        return 4;
    default:
        return 0;
    }
}

// One and two channel images read as grey and grey with alpha, as Storm swizzles them.
GfVec4f
expand(const float* c,
       int components)
{
    switch (components) {
    case 1:
        return GfVec4f(c[0], c[0], c[0], 1.f);
    case 2:
        return GfVec4f(c[0], c[0], c[0], c[1]);
    case 3:
        return GfVec4f(c[0], c[1], c[2], 1.f);
    default:
        return GfVec4f(c[0], c[1], c[2], c[3]);
    }
}

void
buildMips(UtilityTextureImage* image)
{
    int width = image->width;
    int height = image->height;
    std::size_t offset = 0;

    image->levels = 1;
    while (width > 1 || height > 1) {
        const int nextWidth = std::max(1, width / 2);
        const int nextHeight = std::max(1, height / 2);
        const std::size_t nextOffset = offset + static_cast<std::size_t>(width) * height;

        image->texels.resize(nextOffset + static_cast<std::size_t>(nextWidth) * nextHeight);
        const GfVec4f* source = image->texels.data() + offset;
        GfVec4f* destination = image->texels.data() + nextOffset;

        for (int y = 0; y < nextHeight; ++y) {
            for (int x = 0; x < nextWidth; ++x) {
                const int x0 = std::min(2 * x, width - 1);
                const int x1 = std::min(2 * x + 1, width - 1);
                const int y0 = std::min(2 * y, height - 1);
                const int y1 = std::min(2 * y + 1, height - 1);

                destination[y * nextWidth + x] = 0.25f * (source[y0 * width + x0] + source[y0 * width + x1] + source[y1 * width + x0] + source[y1 * width + x1]);
            }
        }
        width = nextWidth;
        height = nextHeight;
        offset = nextOffset;
        ++image->levels;
    }
}
} // anon namespace

bool
UtilityTextureAsset::loaded() const
{
    return std::any_of(tiles.begin(), tiles.end(), [](const UtilityTextureImageConstPtr& tile) { return bool(tile); });
}

UtilityTexturePool::UtilityTexturePool()
{
}

UtilityTexturePool::~UtilityTexturePool()
{
}

UtilityTextureImageConstPtr
UtilityTexturePool::loadImage(const std::string& path,
                              UtilityTextureRef::ColorSpace colorSpace,
                              std::string* error)
{
    const HioImage::SourceColorSpace source = colorSpace == UtilityTextureRef::eColorSpaceRaw ? HioImage::Raw
        : colorSpace == UtilityTextureRef::eColorSpaceSRGB                                    ? HioImage::SRGB
                                                                                              : HioImage::Auto;
    const HioImageSharedPtr reader = HioImage::OpenForReading(path, 0, 0, source, true);

    if (!reader) {
        *error = "cannot be opened";

        return UtilityTextureImageConstPtr();
    }
    const int width = reader->GetWidth();
    const int height = reader->GetHeight();
    const HioFormat format = reader->GetFormat();
    const HioType type = HioGetHioType(format);
    const int components = HioGetComponentCount(format);
    const int size = componentSize(type);

    if (width <= 0 || height <= 0 || size == 0 || components < 1 || components > 4) {
        *error = "has a pixel format the renderer cannot read";

        return UtilityTextureImageConstPtr();
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    const std::size_t pixelBytes = static_cast<std::size_t>(size) * components;
    std::vector<unsigned char> bytes(pixelCount * pixelBytes);
    HioImage::StorageSpec spec;

    spec.width = width;
    spec.height = height;
    spec.depth = 1;
    spec.format = format;
    spec.flipped = true;
    spec.data = bytes.data();
    if (!reader->Read(spec)) {
        *error = "cannot be read";

        return UtilityTextureImageConstPtr();
    }

    const bool integer = type != HioTypeHalfFloat && type != HioTypeFloat;
    const bool decode = type == HioTypeUnsignedByteSRGB
        || (integer && colorSpace == UtilityTextureRef::eColorSpaceSRGB)
        || (integer && colorSpace == UtilityTextureRef::eColorSpaceAuto && reader->IsColorSpaceSRGB());
    std::shared_ptr<UtilityTextureImage> image = std::make_shared<UtilityTextureImage>();

    image->width = width;
    image->height = height;
    image->texels.resize(pixelCount);
    for (std::size_t p = 0; p < pixelCount; ++p) {
        float c[4] = { 0.f, 0.f, 0.f, 1.f };

        for (int k = 0; k < components; ++k) {
            c[k] = componentAt(bytes.data() + p * pixelBytes, type, k);
        }
        // Alpha, and the second channel of grey with alpha, stay linear.
        const int colorComponents = components == 2 ? 1 : std::min(components, 3);

        if (decode) {
            for (int k = 0; k < colorComponents; ++k) {
                c[k] = srgbToLinear(c[k]);
            }
        }
        image->texels[p] = expand(c, components);
    }
    buildMips(image.get());

    return image;
}

UtilityTextureAssetConstPtr
UtilityTexturePool::acquire(const std::string& asset,
                            UtilityTextureRef::ColorSpace colorSpace)
{
    const Key key(asset, static_cast<int>(colorSpace));

    {
        std::lock_guard<std::mutex> l(_mutex);
        const auto it = _entries.find(key);

        if (it != _entries.end()) {
            return it->second.asset;
        }
    }

    // Loads run unlocked, so a slow file holds up no other asset; two threads may load the same
    // one, and the first to finish is kept.
    Entry entry;
    std::shared_ptr<UtilityTextureAsset> loaded = std::make_shared<UtilityTextureAsset>();
    const std::size_t udim = asset.find(kUdimToken);

    if (asset.empty()) {
        entry.error = "is not set";
    } else if (udim == std::string::npos) {
        UtilityTextureImageConstPtr image = loadImage(asset, colorSpace, &entry.error);

        if (image) {
            loaded->tiles.push_back(image);
        }
    } else {
        loaded->udim = true;
        loaded->tiles.resize(UtilityTextureAsset::kUdimTileCount);
        for (int tile = 0; tile < UtilityTextureAsset::kUdimTileCount; ++tile) {
            std::string path = asset;

            path.replace(udim, sizeof(kUdimToken) - 1, TfStringPrintf("%d", 1001 + tile));
            if (TfPathExists(path)) {
                std::string ignored;

                loaded->tiles[tile] = loadImage(path, colorSpace, &ignored);
            }
        }
        if (!loaded->loaded()) {
            entry.error = "has no readable UDIM tiles";
        }
    }
    if (loaded->loaded()) {
        entry.asset = loaded;
        entry.error.clear();
    }

    std::lock_guard<std::mutex> l(_mutex);

    return _entries.emplace(key, entry).first->second.asset;
}

void
UtilityTexturePool::retainOnly(const std::set<Key>& keys)
{
    std::lock_guard<std::mutex> l(_mutex);

    for (auto it = _entries.begin(); it != _entries.end();) {
        if (keys.count(it->first)) {
            ++it;
        } else {
            it = _entries.erase(it);
        }
    }
}

std::vector<std::string>
UtilityTexturePool::failures() const
{
    std::lock_guard<std::mutex> l(_mutex);
    std::vector<std::string> lines;

    for (const auto& entry : _entries) {
        if (!entry.second.asset) {
            const std::string name = entry.first.first.empty() ? std::string("A texture") : "The texture " + entry.first.first;

            lines.push_back(name + " " + entry.second.error + "; its fallback is used.");
        }
    }

    return lines;
}

std::size_t
UtilityTexturePool::size() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _entries.size();
}

NATRON_NAMESPACE_EXIT
