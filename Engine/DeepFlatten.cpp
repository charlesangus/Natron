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
#include <memory>

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
    if (channelOrder.empty() || (alphaChannelIndex < 0) || (alphaChannelIndex >= (int)channelOrder.size())) {
        return eStatusFailed;
    }

    std::vector<FlattenTarget> targets(1);
    targets[0].channelNames = channelOrder;
    targets[0].dst = dst;

    return flattenLayersToImages(src, roi, channelOrder[alphaChannelIndex], targets, scratch, work);
}

StatusEnum
DeepFlatten::flattenLayersToImages(const DeepImage& src,
                                   const RectI& roi,
                                   const std::string& alphaChannelName,
                                   const std::vector<FlattenTarget>& targets,
                                   DeepPixelScratch* scratch,
                                   DeepTidyWorkspace* work)
{
    if (!scratch || !work) {
        return eStatusFailed;
    }

    const std::size_t numTargets = targets.size();
    for (std::size_t t = 0; t < numTargets; ++t) {
        const FlattenTarget& target = targets[t];
        if (!target.dst || target.channelNames.empty()) {
            return eStatusFailed;
        }
        if ((target.dst->getBitDepth() != eImageBitDepthFloat) || ((std::size_t)target.dst->getComponentsCount() != target.channelNames.size())) {
            return eStatusFailed;
        }
    }

    // Union slot 0 is always alpha, backed by zeros when src has none, so the tidy and flatten
    // calls always have a valid alpha index.
    std::vector<std::string> unionNames(1, alphaChannelName);
    std::vector<const float*> unionBases(1, NULL);
    {
        const DeepChannelBuffer* alphaBuffer = src.getChannel(alphaChannelName);
        if (alphaBuffer && alphaBuffer->data()) {
            unionBases[0] = alphaBuffer->data();
        }
    }

    // Per target component: its slot in the union, or -1 when src lacks the channel.
    std::vector<std::vector<int>> componentSlots(numTargets);
    for (std::size_t t = 0; t < numTargets; ++t) {
        const std::vector<std::string>& names = targets[t].channelNames;
        componentSlots[t].resize(names.size(), -1);
        for (std::size_t c = 0; c < names.size(); ++c) {
            if (names[c] == alphaChannelName) {
                componentSlots[t][c] = 0;
                continue;
            }
            const DeepChannelBuffer* buffer = src.getChannel(names[c]);
            if (!buffer || !buffer->data()) {
                continue;
            }
            int slot = -1;
            for (std::size_t u = 1; u < unionNames.size(); ++u) {
                if (unionNames[u] == names[c]) {
                    slot = (int)u;
                    break;
                }
            }
            if (slot < 0) {
                slot = (int)unionNames.size();
                unionNames.push_back(names[c]);
                unionBases.push_back(buffer->data());
            }
            componentSlots[t][c] = slot;
        }
    }

    const int numUnion = (int)unionNames.size();

    std::vector<RectI> regions(numTargets);
    RectI totalRegion;
    bool haveRegion = false;
    for (std::size_t t = 0; t < numTargets; ++t) {
        regions[t] = roi.intersect(targets[t].dst->getBounds());
        if (regions[t].isNull()) {
            continue;
        }
        if (!haveRegion) {
            totalRegion = regions[t];
            haveRegion = true;
        } else {
            totalRegion.merge(regions[t]);
        }
    }
    if (!haveRegion) {
        return eStatusOK;
    }

    const DeepChannelBuffer* zBuffer = src.getChannel(kDeepChannelZ);
    const DeepChannelBuffer* zBackBuffer = src.getChannel(kDeepChannelZBack);
    const float* zBase = (zBuffer && zBuffer->data()) ? zBuffer->data() : NULL;
    const float* zBackBase = (zBackBuffer && zBackBuffer->data()) ? zBackBuffer->data() : NULL;

    const RectI& srcBounds = src.getBounds();
    const SampleTable& table = src.getSampleTable();
    // With no alpha nothing occludes, so overlap is harmless, and tidying would only lose the
    // values of volume samples it splits (a zero alpha splits into zero-valued pieces).
    const bool mustTidy = !src.isTidy() && unionBases[0];
    std::vector<const float*> channelPtrs(numUnion, NULL);
    std::vector<float> flat(numUnion, 0.f);
    std::vector<float> zeroAlpha;

    std::vector<std::shared_ptr<Image::WriteAccess>> accesses(numTargets);
    for (std::size_t t = 0; t < numTargets; ++t) {
        accesses[t].reset(new Image::WriteAccess(targets[t].dst.get()));
    }
    std::vector<float*> rowPixels(numTargets, NULL);

    for (int y = totalRegion.y1; y < totalRegion.y2; ++y) {
        for (std::size_t t = 0; t < numTargets; ++t) {
            if ((y >= regions[t].y1) && (y < regions[t].y2) && !regions[t].isNull()) {
                rowPixels[t] = (float*)accesses[t]->pixelAt(regions[t].x1, y);
                if (!rowPixels[t]) {
                    return eStatusFailed;
                }
            } else {
                rowPixels[t] = NULL;
            }
        }

        for (int x = totalRegion.x1; x < totalRegion.x2; ++x) {
            bool haveSamples = false;
            U32 sampleCount = 0;
            U64 offset = 0;
            if (zBase && srcBounds.contains(x, y)) {
                const std::size_t pixelIndex = pixelIndexInBounds(srcBounds, x, y);
                sampleCount = table.getCount(pixelIndex);
                if (sampleCount > 0) {
                    offset = table.getOffset(pixelIndex);
                    haveSamples = true;
                }
            }

            if (haveSamples) {
                if (zeroAlpha.size() < (std::size_t)sampleCount) {
                    zeroAlpha.assign((std::size_t)sampleCount, 0.f);
                }
                for (int u = 0; u < numUnion; ++u) {
                    channelPtrs[u] = unionBases[u] ? (unionBases[u] + offset) : zeroAlpha.data();
                }

                DeepPixelView pixel;
                pixel.z = zBase + offset;
                pixel.zback = zBackBase ? (zBackBase + offset) : NULL;
                pixel.channels = channelPtrs.data();
                pixel.numChannels = numUnion;
                pixel.alphaChannelIndex = 0;
                pixel.numSamples = (int)sampleCount;

                if (mustTidy) {
                    DeepPixelOps::tidySamples(pixel, scratch, work);
                    DeepPixelOps::flattenFrontToBack(scratch->view(), flat.data());
                } else {
                    DeepPixelOps::flattenFrontToBack(pixel, flat.data());
                }
            }

            for (std::size_t t = 0; t < numTargets; ++t) {
                if (!rowPixels[t] || !regions[t].contains(x, y)) {
                    continue;
                }
                const std::vector<int>& slots = componentSlots[t];
                const int n = (int)slots.size();
                float* dstPixel = rowPixels[t] + (std::size_t)(x - regions[t].x1) * (std::size_t)n;
                for (int c = 0; c < n; ++c) {
                    dstPixel[c] = (haveSamples && (slots[c] >= 0)) ? flat[slots[c]] : 0.f;
                }
            }
        }
    }

    return eStatusOK;
} // DeepFlatten::flattenLayersToImages

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
