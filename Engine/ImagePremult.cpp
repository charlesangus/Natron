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

#include "Image.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <stdexcept>

NATRON_NAMESPACE_ENTER

template <typename PIX, int maxValue, bool divide>
void
Image::premultByChannelForDepth(const RectI& roi,
                                const Image* divisorImg,
                                int divisorChannel,
                                std::bitset<4> processChannels,
                                int skipChannel)
{
    const int dstNComps = (int)getComponentsCount();
    // processChannels follows copyUnProcessedChannels(): a one-channel image's only channel is
    // bit 3, whatever its index in the pixel.
    const auto channelBit = [dstNComps](int c) { return dstNComps == 1 ? 3 : c; };
    const unsigned int dstRowElements = _bounds.width() * dstNComps;
    PIX* dst_pixels = (PIX*)pixelAt(roi.x1, roi.y1);

    if (!dst_pixels) {
        return;
    }

    for (int y = roi.y1; y < roi.y2; ++y,
             dst_pixels += (dstRowElements - (roi.x2 - roi.x1) * dstNComps)) {
        for (int x = roi.x1; x < roi.x2; ++x,
                 dst_pixels += dstNComps) {
            const PIX* divisor_pixels = (const PIX*)divisorImg->pixelAt(x, y);

            // Outside the divisor's bounds there is no value to divide or multiply by. Both
            // directions skip the same pixels, so the pair still round-trips there.
            if (!divisor_pixels) {
                continue;
            }
            const float d = (float)divisor_pixels[divisorChannel] / (float)maxValue;

            if (divide) {
                // Dividing by <= 0 is left as identity, as ofxsUnPremult() did in the plug-in.
                if (d <= (float)(FLT_EPSILON)) {
                    continue;
                }
            }
            for (int c = 0; c < dstNComps && c < 4; ++c) {
                if (!processChannels[channelBit(c)] || (c == skipChannel)) {
                    continue;
                }
                const float v = divide ? ((float)dst_pixels[c] / d) : ((float)dst_pixels[c] * std::max(0.f, d));
                dst_pixels[c] = clampIfInt<PIX>(v);
            }
        }
    }
} // Image::premultByChannelForDepth

void
Image::premultByChannel(const RectI& roi,
                        const Image* divisorImg,
                        int divisorChannel,
                        std::bitset<4> processChannels,
                        int skipChannel,
                        bool divide)
{
    if (!divisorImg) {
        return;
    }
    assert(divisorChannel >= 0 && divisorChannel < (int)divisorImg->getComponentsCount());
    if ((divisorChannel < 0) || (divisorChannel >= (int)divisorImg->getComponentsCount())) {
        return;
    }

    // Both storages must be RAM: the callers that pair the divide with the multiply
    // (OfxClipInstance::getInputImageInternal() and EffectInstance::tiledRenderingFunctor())
    // both skip OpenGL renders, so no half-applied pair can reach here.
    assert(getStorageMode() != eStorageModeGLTex && divisorImg->getStorageMode() != eStorageModeGLTex);
    if ((getStorageMode() == eStorageModeGLTex) || (divisorImg->getStorageMode() == eStorageModeGLTex)) {
        return;
    }

    const RectI intersection = roi.intersect(_bounds);
    if (intersection.isNull()) {
        return;
    }

    // The kernel reads the divisor through the same PIX as this image. A plane of the same
    // input at another bit depth is possible (the node's preferred depth is per-clip), so
    // rather than templating the pair of depths, rescale the divisor into ours first.
    ImagePtr convertedDivisor;
    if (divisorImg->getBitDepth() != getBitDepth()) {
        convertedDivisor = std::make_shared<Image>(divisorImg->getComponents(),
                                                   divisorImg->getRoD(),
                                                   divisorImg->getBounds(),
                                                   divisorImg->getMipmapLevel(),
                                                   divisorImg->getPixelAspectRatio(),
                                                   getBitDepth(),
                                                   divisorImg->getFieldingOrder(),
                                                   false);
        divisorImg->convertToFormat(divisorImg->getBounds(),
                                    eViewerColorSpaceLinear,
                                    eViewerColorSpaceLinear,
                                    -1,
                                    false,
                                    convertedDivisor.get());
        divisorImg = convertedDivisor.get();
    }

    switch (getBitDepth()) {
    case eImageBitDepthByte:
        if (divide) {
            premultByChannelForDepth<unsigned char, 255, true>(intersection, divisorImg, divisorChannel, processChannels, skipChannel);
        } else {
            premultByChannelForDepth<unsigned char, 255, false>(intersection, divisorImg, divisorChannel, processChannels, skipChannel);
        }
        break;
    case eImageBitDepthShort:
        if (divide) {
            premultByChannelForDepth<unsigned short, 65535, true>(intersection, divisorImg, divisorChannel, processChannels, skipChannel);
        } else {
            premultByChannelForDepth<unsigned short, 65535, false>(intersection, divisorImg, divisorChannel, processChannels, skipChannel);
        }
        break;
    case eImageBitDepthFloat:
        if (divide) {
            premultByChannelForDepth<float, 1, true>(intersection, divisorImg, divisorChannel, processChannels, skipChannel);
        } else {
            premultByChannelForDepth<float, 1, false>(intersection, divisorImg, divisorChannel, processChannels, skipChannel);
        }
        break;
    default:
        break;
    }
} // Image::premultByChannel

void
Image::unPremultiplyByChannel(const RectI& roi,
                              const Image* divisorImg,
                              int divisorChannel,
                              std::bitset<4> processChannels,
                              int skipChannel)
{
    premultByChannel(roi, divisorImg, divisorChannel, processChannels, skipChannel, true);
}

void
Image::premultiplyByChannel(const RectI& roi,
                            const Image* divisorImg,
                            int divisorChannel,
                            std::bitset<4> processChannels,
                            int skipChannel)
{
    premultByChannel(roi, divisorImg, divisorChannel, processChannels, skipChannel, false);
}

NATRON_NAMESPACE_EXIT
