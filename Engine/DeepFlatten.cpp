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

#include "DeepFlatten.h"

#include <cstddef>
#include <map>

#include "Engine/DeepImage.h"
#include "Engine/Image.h"

NATRON_NAMESPACE_ENTER

namespace {
const std::string kDeepChannelZ("Z");
const std::string kDeepChannelZBack("ZBack");

std::size_t
pixelIndexInBounds(const RectI& bounds,
                   int x,
                   int y)
{
    return ((std::size_t)(y - bounds.y1) * (std::size_t)bounds.width()) + (std::size_t)(x - bounds.x1);
}

void
writeZeroes(float* pixel,
            int numChannels)
{
    for (int c = 0; c < numChannels; ++c) {
        pixel[c] = 0.f;
    }
}
} // anonymous namespace

StatusEnum
DeepFlatten::flattenToImage(const DeepImage& src,
                            const RectI& roi,
                            const std::vector<std::string>& channelOrder,
                            int alphaChannelIndex,
                            DeepPixelScratch* scratch,
                            DeepTidyWorkspace* work,
                            const ImagePtr& dst)
{
    if (!dst || !scratch || !work || channelOrder.empty()) {
        return eStatusFailed;
    }

    const int numChannels = (int)channelOrder.size();

    if ((alphaChannelIndex < 0) || (alphaChannelIndex >= numChannels)) {
        return eStatusFailed;
    }
    if ((dst->getBitDepth() != eImageBitDepthFloat) || ((int)dst->getComponentsCount() != numChannels)) {
        return eStatusFailed;
    }

    const RectI region = roi.intersect(dst->getBounds());

    if (region.isNull()) {
        return eStatusOK;
    }

    std::vector<const float*> channelBases(numChannels, NULL);
    for (int c = 0; c < numChannels; ++c) {
        const DeepChannelBuffer* buffer = src.getChannel(channelOrder[c]);
        if (!buffer || !buffer->data()) {
            return eStatusFailed;
        }
        channelBases[c] = buffer->data();
    }

    const DeepChannelBuffer* zBuffer = src.getChannel(kDeepChannelZ);
    const DeepChannelBuffer* zBackBuffer = src.getChannel(kDeepChannelZBack);
    const float* zBase = zBuffer ? zBuffer->data() : NULL;
    const float* zBackBase = zBackBuffer ? zBackBuffer->data() : NULL;

    const RectI& srcBounds = src.getBounds();
    const SampleTable& table = src.getSampleTable();
    const bool mustTidy = !src.isTidy();
    std::vector<const float*> channelPtrs(numChannels, NULL);

    Image::WriteAccess dstAccess(dst.get());

    for (int y = region.y1; y < region.y2; ++y) {
        float* dstPixel = (float*)dstAccess.pixelAt(region.x1, y);
        if (!dstPixel) {
            return eStatusFailed;
        }
        for (int x = region.x1; x < region.x2; ++x, dstPixel += numChannels) {
            if (!zBase || !srcBounds.contains(x, y)) {
                writeZeroes(dstPixel, numChannels);
                continue;
            }

            const std::size_t pixelIndex = pixelIndexInBounds(srcBounds, x, y);
            const U32 sampleCount = table.getCount(pixelIndex);
            if (sampleCount == 0) {
                writeZeroes(dstPixel, numChannels);
                continue;
            }

            const U64 offset = table.getOffset(pixelIndex);
            for (int c = 0; c < numChannels; ++c) {
                channelPtrs[c] = channelBases[c] + offset;
            }

            DeepPixelView pixel;
            pixel.z = zBase + offset;
            pixel.zback = zBackBase ? (zBackBase + offset) : NULL;
            pixel.channels = channelPtrs.data();
            pixel.numChannels = numChannels;
            pixel.alphaChannelIndex = alphaChannelIndex;
            pixel.numSamples = (int)sampleCount;

            if (mustTidy) {
                DeepPixelOps::tidySamples(pixel, scratch, work);
                DeepPixelOps::flattenFrontToBack(scratch->view(), dstPixel);
            } else {
                DeepPixelOps::flattenFrontToBack(pixel, dstPixel);
            }
        }
    }

    return eStatusOK;
} // DeepFlatten::flattenToImage

bool
DeepFlatten::getSamplesAtPixel(const DeepImage& src,
                               int x,
                               int y,
                               std::vector<std::string>* channelNames,
                               std::vector<DeepSample>* samples)
{
    if (!channelNames || !samples) {
        return false;
    }
    channelNames->clear();
    samples->clear();

    const RectI& bounds = src.getBounds();
    if (!bounds.contains(x, y)) {
        return false;
    }

    const DeepChannelBuffer* zBuffer = src.getChannel(kDeepChannelZ);
    if (!zBuffer || !zBuffer->data()) {
        return false;
    }
    const DeepChannelBuffer* zBackBuffer = src.getChannel(kDeepChannelZBack);

    std::vector<const float*> channelPtrs;
    int alphaChannelIndex = 0;
    const std::map<std::string, DeepChannelBuffer>& channels = src.getChannels();
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = channels.begin(); it != channels.end(); ++it) {
        if ((it->first == kDeepChannelZ) || (it->first == kDeepChannelZBack) || !it->second.data()) {
            continue;
        }
        if (it->first == "A") {
            alphaChannelIndex = (int)channelNames->size();
        }
        channelNames->push_back(it->first);
        channelPtrs.push_back(it->second.data());
    }

    const std::size_t pixelIndex = pixelIndexInBounds(bounds, x, y);
    const U32 sampleCount = src.getSampleTable().getCount(pixelIndex);
    if (sampleCount == 0) {
        return true;
    }

    const U64 offset = src.getSampleTable().getOffset(pixelIndex);
    for (std::size_t c = 0; c < channelPtrs.size(); ++c) {
        channelPtrs[c] += offset;
    }

    DeepPixelView pixel;
    pixel.z = zBuffer->data() + offset;
    pixel.zback = (zBackBuffer && zBackBuffer->data()) ? (zBackBuffer->data() + offset) : NULL;
    pixel.channels = channelPtrs.data();
    pixel.numChannels = (int)channelPtrs.size();
    pixel.alphaChannelIndex = alphaChannelIndex;
    pixel.numSamples = (int)sampleCount;

    samples->reserve(sampleCount);
    for (U32 s = 0; s < sampleCount; ++s) {
        samples->push_back(DeepPixelOps::getSample(pixel, (int)s));
    }

    return true;
} // DeepFlatten::getSamplesAtPixel

NATRON_NAMESPACE_EXIT
