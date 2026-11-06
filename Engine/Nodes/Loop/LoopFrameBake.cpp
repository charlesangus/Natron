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

#include "LoopFrameBake.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"

NATRON_NAMESPACE_ENTER

namespace LoopFrameBake {
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
    regionText(const RectD& rod)
    {
        char text[128];
        std::snprintf(text, sizeof(text), "%.17g %.17g %.17g %.17g", rod.x1, rod.y1, rod.x2, rod.y2);

        return std::string(text);
    }

    bool
    parseRegion(const std::string& text,
                RectD* rod)
    {
        double v[4];
        if (std::sscanf(text.c_str(), "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]) != 4) {
            return false;
        }
        *rod = RectD(v[0], v[1], v[2], v[3]);

        return true;
    }

    bool
    parseHash(const std::string& text,
              U64* hash)
    {
        if (text.empty()) {
            return false;
        }
        char* end = 0;
        const unsigned long long value = std::strtoull(text.c_str(), &end, 16);
        if (!end || (*end != '\0')) {
            return false;
        }
        *hash = (U64)value;

        return true;
    }
} // anonymous namespace

std::string
hashText(U64 hash)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);

    return std::string(text);
}

bool
writeFrame(const std::string& path,
           const Image& image,
           const RectI& format,
           U64 hash,
           const std::function<bool()>& cancelled,
           std::string* error)
{
    if (image.getBitDepth() != eImageBitDepthFloat) {
        return fail(error, "Only float frames are baked");
    }
    const RectI bounds = image.getBounds();
    if (bounds.isNull()) {
        return fail(error, "The frame is empty");
    }
    const int nComps = (int)image.getComponentsCount();
    const std::vector<std::string>& names = image.getComponents().getChannels();
    if ((nComps <= 0) || ((int)names.size() != nComps)) {
        return fail(error, "The frame's channels have no names");
    }
    // As the native Write lays it out, so OiioReadSupport::dataWindowOf() gives bounds back.
    const RectI display = format.isNull() ? bounds : format;

    OIIO::ImageSpec spec(bounds.width(), bounds.height(), nComps, OiioWriteSupport::dataTypeOiio(OiioWriteSupport::DataType::Float));
    spec.channelnames = names;
    spec.alpha_channel = -1;
    spec.z_channel = -1;
    spec.x = bounds.x1;
    spec.y = display.y2 - bounds.y2;
    spec.full_x = display.x1;
    spec.full_y = display.y1;
    spec.full_width = display.width();
    spec.full_height = display.height();
    spec.attribute("oiio:UnassociatedAlpha", 1);
    spec.attribute("compression", "zip");
    spec.attribute("PixelAspectRatio", (float)image.getPixelAspectRatio());
    spec.attribute(kLoopFrameBakeHashAttribute, hashText(hash));
    spec.attribute(kLoopFrameBakeRegionAttribute, regionText(image.getRoD()));

    std::vector<OiioWriteSupport::WritePart> parts(1);
    parts[0].spec = spec;
    Image::ReadAccess access = image.getReadRights();
    const std::size_t rowFloats = (std::size_t)bounds.width() * nComps;
    const OiioWriteSupport::RowSource rows = [&](int /*part*/, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) -> bool {
        for (int r = rowBegin; r < rowEnd; ++r) {
            const int y = bounds.y2 - 1 - r;
            const float* src = (const float*)access.pixelAt(bounds.x1, y);
            if (!src) {
                return false;
            }
            float* dst = (float*)((char*)band + (std::ptrdiff_t)(r - rowBegin) * rowStride);
            std::copy(src, src + rowFloats, dst);
        }

        return true;
    };
    const OiioWriteSupport::CancelCheck cancel = [&cancelled]() -> bool {
        return cancelled && cancelled();
    };
    if (!OiioWriteSupport::writeFile(path, parts, rows, cancel, error)) {
        return false;
    }
    OiioReadSupport::evictHeaders(std::vector<std::string>(1, path));

    return true;
} // writeFrame

bool
readFrameInfo(const std::string& path,
              FrameInfo* info,
              std::string* error)
{
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, error);
    if (!header || header->subimages.empty()) {
        return header ? fail(error, "No image in " + path) : false;
    }
    const OIIO::ImageSpec& spec = header->subimages.front();
    *info = FrameInfo();
    info->hasHash = parseHash(spec.get_string_attribute(kLoopFrameBakeHashAttribute), &info->hash);
    info->bounds = OiioReadSupport::dataWindowOf(spec);
    info->pixelAspect = (double)spec.get_float_attribute("PixelAspectRatio", 1.f);
    if (!(info->pixelAspect > 0.)) {
        info->pixelAspect = 1.;
    }
    info->channels = spec.nchannels;
    if (!parseRegion(spec.get_string_attribute(kLoopFrameBakeRegionAttribute), &info->regionOfDefinition)) {
        info->regionOfDefinition = info->bounds.toCanonical_noClipping(0, info->pixelAspect);
    }

    return true;
}

ImagePtr
readFrame(const std::string& path,
          const ImageLayerDesc& layer,
          FrameInfo* info,
          std::string* error)
{
    if (!readFrameInfo(path, info, error)) {
        return ImagePtr();
    }
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, error);
    if (!header || header->subimages.empty()) {
        return ImagePtr();
    }
    const std::vector<std::string>& fileNames = header->subimages.front().channelnames;
    const std::vector<std::string>& names = layer.getChannels();
    const int nComps = layer.getNumComponents();
    std::vector<int> index(nComps, -1);
    for (int c = 0; c < nComps; ++c) {
        std::vector<std::string>::const_iterator found = ((std::size_t)c < names.size()) ? std::find(fileNames.begin(), fileNames.end(), names[c]) : fileNames.end();
        if (found == fileNames.end()) {
            fail(error, path + " lacks a channel of the loop's layer");

            return ImagePtr();
        }
        index[c] = (int)(found - fileNames.begin());
    }
    const RectI bounds = info->bounds;
    if (bounds.isNull()) {
        fail(error, path + " is empty");

        return ImagePtr();
    }
    const int fileComps = info->channels;
    const std::size_t rowFloats = (std::size_t)bounds.width() * fileComps;
    std::vector<float> pixels(rowFloats * bounds.height());
    if (!OiioReadSupport::decode(path, 0, 0, fileComps, bounds, pixels.data(), rowFloats, error)) {
        return ImagePtr();
    }
    ImagePtr image = std::make_shared<Image>(layer, info->regionOfDefinition, bounds, 0, info->pixelAspect, eImageBitDepthFloat, eImageFieldingOrderNone, false);
    Image::WriteAccess access(image.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        float* dst = (float*)access.pixelAt(bounds.x1, y);
        if (!dst) {
            return ImagePtr();
        }
        const float* src = &pixels[(std::size_t)(y - bounds.y1) * rowFloats];
        for (int x = 0; x < bounds.width(); ++x) {
            for (int c = 0; c < nComps; ++c) {
                dst[(std::size_t)x * nComps + c] = src[(std::size_t)x * fileComps + index[c]];
            }
        }
    }

    return image;
} // readFrame
} // namespace LoopFrameBake

NATRON_NAMESPACE_EXIT
