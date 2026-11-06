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

#include "LiveTextureImage.h"

#include <cstdint>
#include <cstring>

#ifdef emit
#define NATRON_LIVETEXTUREIMAGE_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/tf/diagnostic.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#ifdef NATRON_LIVETEXTUREIMAGE_RESTORE_EMIT
#undef NATRON_LIVETEXTUREIMAGE_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Scene/LiveTextureStore.h"

NATRON_NAMESPACE_ENTER

LiveTextureImage::LiveTextureImage()
{
}

LiveTextureImage::~LiveTextureImage()
{
}

bool
LiveTextureImage::_OpenForReading(const std::string& filename,
                                  int subimage,
                                  int mip,
                                  SourceColorSpace /*sourceColorSpace*/,
                                  bool suppressErrors)
{
    _filename = filename;
    _bytes.reset();
    _pixels = nullptr;
    _width = _height = 0;
    if ((subimage != 0) || (mip != 0)) {
        return false;
    }

    PXR_NS::ArResolver& resolver = PXR_NS::ArGetResolver();
    const PXR_NS::ArResolvedPath resolved = resolver.Resolve(filename);
    const std::shared_ptr<PXR_NS::ArAsset> asset = resolved.IsEmpty() ? std::shared_ptr<PXR_NS::ArAsset>() : resolver.OpenAsset(resolved);
    const std::shared_ptr<const char> bytes = asset ? asset->GetBuffer() : std::shared_ptr<const char>();
    const float* pixels = nullptr;
    if (!bytes || !LiveTextureBuffer::parse(bytes.get(), asset->GetSize(), &_width, &_height, &pixels)) {
        if (!suppressErrors) {
            PXR_NAMESPACE_USING_DIRECTIVE
            TF_RUNTIME_ERROR("The live texture %s is not rendered", filename.c_str());
        }

        return false;
    }
    _bytes = bytes;
    _pixels = pixels;

    return true;
}

bool
LiveTextureImage::_OpenForWriting(const std::string& /*filename*/)
{
    return false;
}

bool
LiveTextureImage::Read(const StorageSpec& storage)
{
    return ReadCropped(0, 0, 0, 0, storage);
}

bool
LiveTextureImage::ReadCropped(const int cropTop,
                              const int cropBottom,
                              const int cropLeft,
                              const int cropRight,
                              const StorageSpec& storage)
{
    const int cropWidth = _width - cropLeft - cropRight;
    const int cropHeight = _height - cropTop - cropBottom;

    if (!_pixels || !storage.data || (storage.format != PXR_NS::HioFormatFloat32Vec4)
        || (cropTop < 0) || (cropBottom < 0) || (cropLeft < 0) || (cropRight < 0)
        || (cropWidth <= 0) || (cropHeight <= 0) || (storage.width <= 0) || (storage.height <= 0)) {
        return false;
    }

    float* out = static_cast<float*>(storage.data);
    const std::size_t outRowFloats = std::size_t(storage.width) * 4;
    for (int outY = 0; outY < storage.height; ++outY) {
        // Hio rows run top-down unless flipped; Natron's run bottom-up.
        const int fromTop = storage.flipped ? (storage.height - 1 - outY) : outY;
        const int cropY = (int)((std::int64_t(fromTop) * 2 + 1) * cropHeight / (std::int64_t(storage.height) * 2));
        const int sourceY = _height - 1 - (cropTop + cropY);
        const float* source = _pixels + std::size_t(sourceY) * std::size_t(_width) * 4;
        float* row = out + std::size_t(outY) * outRowFloats;
        if (storage.width == cropWidth) {
            std::memcpy(row, source + std::size_t(cropLeft) * 4, outRowFloats * sizeof(float));
            continue;
        }
        for (int outX = 0; outX < storage.width; ++outX) {
            const int cropX = (int)((std::int64_t(outX) * 2 + 1) * cropWidth / (std::int64_t(storage.width) * 2));
            std::memcpy(row + std::size_t(outX) * 4, source + std::size_t(cropLeft + cropX) * 4, 4 * sizeof(float));
        }
    }

    return true;
}

bool
LiveTextureImage::Write(const StorageSpec& /*storage*/,
                        const PXR_NS::VtDictionary& /*metadata*/)
{
    return false;
}

const std::string&
LiveTextureImage::GetFilename() const
{
    return _filename;
}

int
LiveTextureImage::GetWidth() const
{
    return _width;
}

int
LiveTextureImage::GetHeight() const
{
    return _height;
}

PXR_NS::HioFormat
LiveTextureImage::GetFormat() const
{
    return PXR_NS::HioFormatFloat32Vec4;
}

int
LiveTextureImage::GetBytesPerPixel() const
{
    return 4 * sizeof(float);
}

int
LiveTextureImage::GetNumMipLevels() const
{
    return 1;
}

bool
LiveTextureImage::IsColorSpaceSRGB() const
{
    return false;
}

bool
LiveTextureImage::GetMetadata(const PXR_NS::TfToken& /*key*/,
                              PXR_NS::VtValue* /*value*/) const
{
    return false;
}

bool
LiveTextureImage::GetSamplerMetadata(PXR_NS::HioAddressDimension /*dim*/,
                                     PXR_NS::HioAddressMode* /*param*/) const
{
    return false;
}

NATRON_NAMESPACE_EXIT
