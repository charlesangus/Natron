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

#include "ChannelCopy.h"

#include "Engine/EffectInstance.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {

// Mirrors ResolvedLayer::channelBit: a single-channel colour plane is alpha, on bit 3.
int
colorBitOfIndex(int nComps,
                int index)
{
    return (nComps == 1) ? 3 : index;
}

int
colorChannelOnBit(const ImageLayerDesc& layout,
                  int nAvailable,
                  int bit)
{
    const int nComps = layout.getNumComponents();

    for (int i = 0; (i < nComps) && (i < nAvailable) && (i < 4); ++i) {
        if (colorBitOfIndex(nComps, i) == bit) {
            return i;
        }
    }

    return -1;
}

template <typename SRCPIX, typename DSTPIX>
void
copyRow(const Image::ReadAccess* src,
        Image::WriteAccess* dst,
        int y,
        int x1,
        int x2,
        int dstComps,
        const std::vector<int>& srcChannels)
{
    for (int x = x1; x < x2; ++x) {
        DSTPIX* out = (DSTPIX*)dst->pixelAt(x, y);
        if (!out) {
            continue;
        }
        const SRCPIX* in = src ? (const SRCPIX*)src->pixelAt(x, y) : NULL;
        for (int c = 0; c < dstComps; ++c) {
            const int s = (c < (int)srcChannels.size()) ? srcChannels[c] : -1;
            out[c] = (in && (s >= 0)) ? Image::convertPixelDepth<SRCPIX, DSTPIX>(in[s]) : DSTPIX(0);
        }
    }
}

typedef void (*CopyRowFunc)(const Image::ReadAccess*, Image::WriteAccess*, int, int, int, int, const std::vector<int>&);

template <typename DSTPIX>
CopyRowFunc
selectCopyRowForSource(ImageBitDepthEnum srcDepth)
{
    switch (srcDepth) {
    case eImageBitDepthByte:
        return &copyRow<unsigned char, DSTPIX>;
    case eImageBitDepthShort:
        return &copyRow<unsigned short, DSTPIX>;
    case eImageBitDepthFloat:
        return &copyRow<float, DSTPIX>;
    default:
        return NULL;
    }
}

CopyRowFunc
selectCopyRow(ImageBitDepthEnum srcDepth,
              ImageBitDepthEnum dstDepth)
{
    switch (dstDepth) {
    case eImageBitDepthByte:
        return selectCopyRowForSource<unsigned char>(srcDepth);
    case eImageBitDepthShort:
        return selectCopyRowForSource<unsigned short>(srcDepth);
    case eImageBitDepthFloat:
        return selectCopyRowForSource<float>(srcDepth);
    default:
        return NULL;
    }
}
} // namespace

const ImageLayerDesc*
ChannelCopy::findColorStorage(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer() && (it->getNumComponents() > 0)) {
            return &(*it);
        }
    }

    return NULL;
}

ImageLayerDesc
ChannelCopy::metadataColorStorage(const EffectInstance& effect,
                                  int inputNb)
{
    EffectInstancePtr input = effect.getInput(inputNb);

    if (!input) {
        return ImageLayerDesc();
    }

    ImageLayerDesc layer, pairedLayer;
    input->getMetadataComponents(-1, &layer, &pairedLayer);
    if (!layer.isColorLayer() || (layer.getNumComponents() == 0)) {
        return ImageLayerDesc();
    }

    return ImageLayerDesc::mapNCompsToColorLayer(layer.getNumComponents());
}

std::vector<int>
ChannelCopy::mapColorChannels(const ImageLayerDesc& plane,
                              const Image& source,
                              const std::bitset<4>& copiedBits)
{
    const int nPlaneComps = plane.getNumComponents();
    std::vector<int> srcChannels((std::size_t)nPlaneComps, -1);
    const ImageLayerDesc& sourceLayout = source.getComponents();
    const int nAvailable = (int)source.getComponentsCount();

    for (int c = 0; (c < nPlaneComps) && (c < 4); ++c) {
        const int bit = colorBitOfIndex(nPlaneComps, c);
        if (copiedBits[bit]) {
            srcChannels[c] = colorChannelOnBit(sourceLayout, nAvailable, bit);
        }
    }

    return srcChannels;
}

StatusEnum
ChannelCopy::copyChannels(const Image::ReadAccess* src,
                          ImageBitDepthEnum srcDepth,
                          Image* dst,
                          const RectI& roi,
                          const std::vector<int>& srcChannels,
                          const EffectInstance& effect)
{
    const CopyRowFunc copyRowFunc = selectCopyRow(srcDepth, dst->getBitDepth());

    if (!copyRowFunc) {
        return eStatusFailed;
    }

    // The image can be wider than its plane (a plane with no same-sized supported layout); its
    // extra channels are written as 0.
    const int dstComps = (int)dst->getComponentsCount();
    Image::WriteAccess dstAccess(dst);
    for (int y = roi.y1; y < roi.y2; ++y) {
        if (effect.aborted()) {
            return eStatusOK;
        }
        copyRowFunc(src, &dstAccess, y, roi.x1, roi.x2, dstComps, srcChannels);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
