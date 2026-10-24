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

#include "RawDisplay.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/IO/RawMetadata.h"

NATRON_NAMESPACE_ENTER

namespace RawDisplay {
ModeEnum
modeOf(const ImageMetadata& metadata,
       int nComps)
{
    const std::optional<std::string> pattern = metadata.getString(kRawMetadataCfaPattern);

    if (pattern) {
        return (*pattern == kRawCfaPatternNone) ? eModeLinear : eModeMosaic;
    }

    return (nComps <= 1) ? eModeMosaic : eModeLinear;
}

double
Normalisation::blackAt(int x,
                       int y,
                       int channel) const
{
    if (black.empty()) {
        return 0.;
    }
    std::size_t index = (std::size_t)channel;
    if (layout.hasCfa()) {
        index = (std::size_t)((((y - layout.originY()) & 1) * 2) + ((x - layout.originX()) & 1));
    }

    return black[index % black.size()];
}

std::optional<Normalisation>
normalisationOf(const ImageMetadata& metadata)
{
    const std::optional<RawStageEnum> stage = RawMetadata::getStage(metadata);
    const std::optional<double> white = RawMetadata::getWhiteLevel(metadata);

    if (!stage || (*stage != eRawStageRaw) || !white) {
        return std::nullopt;
    }
    Normalisation n;
    n.layout = RawLayout::fromMetadata(metadata).value_or(RawLayout());
    n.black = RawMetadata::getBlackLevels(metadata).value_or(std::vector<double>());
    n.white = *white;

    return n;
}

const char*
label(ModeEnum mode)
{
    return (mode == eModeMosaic) ? "RAW mosaic" : "RAW linear";
}

void
convert(const Image& src,
        Image* dst,
        const RectI& window,
        const Normalisation* normalisation)
{
    if (!dst || (src.getBitDepth() != eImageBitDepthFloat) || (dst->getBitDepth() != eImageBitDepthFloat)) {
        return;
    }
    const int srcComps = (int)src.getComponentsCount();
    const int dstComps = (int)dst->getComponentsCount();
    const RectI srcBounds = src.getBounds();
    const RectI area = window.intersect(dst->getBounds());
    // A one-channel destination is an alpha plane, which takes toRgba()'s alpha.
    const int firstChannel = (dstComps == 1) ? 3 : 0;
    Image::ReadAccess in(&src);
    Image::WriteAccess out(dst);

    for (int y = area.y1; y < area.y2; ++y) {
        float* dstPixel = (float*)out.pixelAt(area.x1, y);
        if (!dstPixel) {
            continue;
        }
        for (int x = area.x1; x < area.x2; ++x, dstPixel += dstComps) {
            float rgba[4] = { 0.f, 0.f, 0.f, 0.f };
            if ((x >= srcBounds.x1) && (x < srcBounds.x2) && (y >= srcBounds.y1) && (y < srcBounds.y2)) {
                const float* srcPixel = (const float*)in.pixelAt(x, y);
                if (srcPixel) {
                    float value[4] = { 0.f, 0.f, 0.f, 0.f };
                    const int n = std::min(srcComps, 4);
                    for (int c = 0; c < n; ++c) {
                        value[c] = srcPixel[c];
                        if (normalisation) {
                            const double black = normalisation->blackAt(x, y, c);
                            const double span = normalisation->white - black;
                            if (span != 0.) {
                                value[c] = (float)((srcPixel[c] - black) / span);
                            }
                        }
                    }
                    toRgba(value, n, rgba);
                }
            }
            for (int c = 0; c < dstComps; ++c) {
                dstPixel[c] = rgba[firstChannel + c];
            }
        }
    }
}

ImagePtr
toDisplayImage(const ImagePtr& src,
               const Normalisation* normalisation)
{
    if (!src) {
        return ImagePtr();
    }
    ImagePtr dst = std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), src->getRoD(), src->getBounds(), src->getMipmapLevel(), src->getPixelAspectRatio(), eImageBitDepthFloat, src->getFieldingOrder(), false);
    convert(*src, dst.get(), src->getBounds(), normalisation);

    return dst;
}
} // namespace RawDisplay

NATRON_NAMESPACE_EXIT
