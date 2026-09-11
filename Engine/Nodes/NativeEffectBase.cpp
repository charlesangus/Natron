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

#include "NativeEffectBase.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <QThread>
#include <QThreadPool>
#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5

#include "Engine/DeepImage.h"
#include "Engine/Node.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace {
float*
allocateDeepChannel(const DeepImagePtr& image,
                    const std::string& name,
                    std::size_t sampleCount)
{
    DeepChannelBuffer& buf = image->getChannelForWriting(name);

    if (buf.size() != sampleCount) {
        buf.allocate(sampleCount);
    }

    return buf.dataForWriting();
}

bool
isDepthChannelName(const std::string& name)
{
    return (name == "Z") || (name == "ZBack");
}

// Calls pixel(x, y, pixelIndex) for every pixel of chunk, pixelIndex being its index into the
// sample table of a DeepImage over bounds.
template <typename PixelFunc>
void
forEachPixelOfChunk(const RectI& bounds,
                    const RectI& chunk,
                    const PixelFunc& pixel)
{
    const std::size_t rowWidth = (std::size_t)bounds.width();

    for (int y = chunk.y1; y < chunk.y2; ++y) {
        const std::size_t rowIndex = (std::size_t)(y - bounds.y1) * rowWidth;
        for (int x = chunk.x1; x < chunk.x2; ++x) {
            pixel(x, y, rowIndex + (std::size_t)(x - bounds.x1));
        }
    }
}
} // anonymous namespace

NativeEffectBase::NativeEffectBase(NodePtr node)
    : EffectInstance(node)
{
}

NativeEffectBase::~NativeEffectBase()
{
}

std::string
NativeEffectBase::getInputLabel(int inputNb) const
{
    const NativePluginDescription desc = getNativePluginDescription();

    if ((inputNb < 0) || ((std::size_t)inputNb >= desc.inputs.size())) {
        return EffectInstance::getInputLabel(inputNb);
    }

    return desc.inputs[inputNb].label;
}

bool
NativeEffectBase::isInputOptional(int inputNb) const
{
    const NativePluginDescription desc = getNativePluginDescription();

    if ((inputNb < 0) || ((std::size_t)inputNb >= desc.inputs.size())) {
        return false;
    }

    return desc.inputs[inputNb].optional;
}

DataKindEnum
NativeEffectBase::getInputDataKind(int inputNb) const
{
    const NativePluginDescription desc = getNativePluginDescription();

    if ((inputNb < 0) || ((std::size_t)inputNb >= desc.inputs.size())) {
        return eDataKindImage;
    }

    return desc.inputs[inputNb].kind;
}

DataKindEnum
NativeEffectBase::resolveOutputDataKind(bool* isAmbiguous) const
{
    NodePtr node = getNode();

    if (!node) {
        if (isAmbiguous) {
            *isAmbiguous = false;
        }

        return eDataKindPolymorphic;
    }

    return node->resolveStructuralOutputDataKind(isAmbiguous);
}

void
NativeEffectBase::addAcceptedComponents(int /*inputNb*/,
                                        std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
NativeEffectBase::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthByte);
    depths->push_back(eImageBitDepthShort);
    depths->push_back(eImageBitDepthFloat);
}

void
NativeEffectBase::makeDeepScanlineChunks(const RectI& bounds,
                                         std::vector<RectI>* chunks)
{
    chunks->clear();

    if (bounds.isNull()) {
        return;
    }

    const int height = bounds.height();
    // More chunks than threads so that pixels whose sample counts differ wildly (a volumetric
    // patch in one corner) do not leave most threads idle waiting on one long chunk.
    const int maxChunks = std::max(1, QThreadPool::globalInstance()->maxThreadCount()) * 4;
    const int numChunks = std::min(height, maxChunks);
    const int rowsPerChunk = (height + numChunks - 1) / numChunks;

    for (int y = bounds.y1; y < bounds.y2; y += rowsPerChunk) {
        chunks->push_back(RectI(bounds.x1, y, bounds.x2, std::min(y + rowsPerChunk, bounds.y2)));
    }
}

bool
NativeEffectBase::forEachDeepChunk(const std::vector<RectI>& chunks,
                                   const DeepChunkFunc& body)
{
    QThread* const callingThread = QThread::currentThread();
    std::atomic<bool> wasAborted(false);

    QtConcurrent::blockingMap(chunks, [&](RectI chunk) {
        QThread* const curThread = QThread::currentThread();
        const bool spawnedThread = (curThread != callingThread);

        if (spawnedThread) {
            appPTR->getAppTLS()->copyTLS(callingThread, curThread);
        }
        if (aborted()) {
            wasAborted = true;
        } else {
            body(chunk);
        }
        if (spawnedThread) {
            appPTR->getAppTLS()->cleanupTLSForThread();
        }
    });

    return !wasAborted;
}

StatusEnum
NativeEffectBase::renderDeepTwoPass(const DeepRenderActionArgs& args,
                                    const std::vector<std::string>& channelNames,
                                    int alphaChannelIndex,
                                    const DeepSampleCountFunc& countSamples,
                                    const DeepFillSamplesFunc& fillSamples,
                                    bool resultIsTidy)
{
    const DeepImagePtr& out = args.outputDeepImage;

    if (!out || !countSamples || !fillSamples) {
        return eStatusFailed;
    }
    if (channelNames.empty() || (alphaChannelIndex < 0) || (alphaChannelIndex >= (int)channelNames.size())) {
        return eStatusFailed;
    }

    const RectI bounds = out->getBounds();
    if (bounds.isNull()) {
        return eStatusOK;
    }

    std::vector<RectI> chunks;
    makeDeepScanlineChunks(bounds, &chunks);

    SampleTable& table = out->getSampleTableForWriting();

    // Pass 1: per-pixel sample counts. Distinct pixels are distinct elements of the count
    // vector, so threads never collide, and nothing is allocated.
    const bool counted = forEachDeepChunk(chunks, [&](const RectI& chunk) {
        forEachPixelOfChunk(bounds, chunk, [&](int x, int y, std::size_t pixelIndex) {
            table.setCount(pixelIndex, countSamples(x, y));
        });
    });
    if (!counted) {
        return eStatusFailed;
    }

    // The single allocation the whole render gets: offsets first, then one contiguous buffer per
    // channel sized to the total the offsets add up to.
    table.recomputeOffsets();

    const std::size_t totalSampleCount = (std::size_t)table.getTotalSampleCount();
    float* const zData = allocateDeepChannel(out, "Z", totalSampleCount);
    float* const zBackData = allocateDeepChannel(out, "ZBack", totalSampleCount);
    std::vector<float*> channelData(channelNames.size());
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        channelData[c] = allocateDeepChannel(out, channelNames[c], totalSampleCount);
    }

    // Pass 2: fill. Every view points straight into the buffers allocated just above, at the
    // offset the prefix sum gave that pixel, so no sample is ever copied or reallocated.
    const bool filled = forEachDeepChunk(chunks, [&](const RectI& chunk) {
        std::vector<float*> pixelChannels(channelNames.size());

        forEachPixelOfChunk(bounds, chunk, [&](int x, int y, std::size_t pixelIndex) {
            const U32 count = table.getCount(pixelIndex);

            if (count == 0) {
                return;
            }

            const U64 offset = table.getOffset(pixelIndex);
            for (std::size_t c = 0; c < pixelChannels.size(); ++c) {
                pixelChannels[c] = channelData[c] + offset;
            }

            MutableDeepPixelView view;
            view.z = zData + offset;
            view.zback = zBackData + offset;
            view.channels = pixelChannels.data();
            view.numChannels = (int)pixelChannels.size();
            view.alphaChannelIndex = alphaChannelIndex;
            view.numSamples = (int)count;

            fillSamples(x, y, view);
        });
    });
    if (!filled) {
        return eStatusFailed;
    }

    out->setTidy(resultIsTidy);

    return eStatusOK;
} // NativeEffectBase::renderDeepTwoPass

StatusEnum
NativeEffectBase::renderDeepFromInput(const DeepRenderActionArgs& args,
                                      const DeepImagePtr& input,
                                      const std::vector<std::string>& channelsToWrite,
                                      int alphaChannelIndex,
                                      const DeepRewriteSamplesFunc& rewrite)
{
    const DeepImagePtr& out = args.outputDeepImage;

    if (!out || !input || !rewrite || channelsToWrite.empty()) {
        return eStatusFailed;
    }
    if ((alphaChannelIndex < -1) || (alphaChannelIndex >= (int)channelsToWrite.size())) {
        return eStatusFailed;
    }
    for (std::size_t c = 0; c < channelsToWrite.size(); ++c) {
        if (isDepthChannelName(channelsToWrite[c])) {
            return eStatusFailed;
        }
    }

    std::vector<std::string> inputChannelNames;
    std::vector<const float*> inputChannels;
    int inputAlphaChannelIndex = -1;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = input->getChannels().begin(); it != input->getChannels().end(); ++it) {
        if (isDepthChannelName(it->first)) {
            continue;
        }
        if (it->first == "A") {
            inputAlphaChannelIndex = (int)inputChannelNames.size();
        }
        inputChannelNames.push_back(it->first);
        inputChannels.push_back(it->second.data());
    }
    const DeepChannelBuffer* const inputZ = input->getChannel("Z");
    const DeepChannelBuffer* const inputZBack = input->getChannel("ZBack");
    const float* const inputZData = inputZ ? inputZ->data() : nullptr;
    const float* const inputZBackData = inputZBack ? inputZBack->data() : nullptr;
    const RectI& inputBounds = input->getBounds();
    const SampleTable& inputTable = input->getSampleTable();
    const std::size_t inputRowWidth = (std::size_t)inputBounds.width();
    const auto inputPixelIndex = [&inputBounds, inputRowWidth](int x, int y) -> std::size_t {
        return ((std::size_t)(y - inputBounds.y1) * inputRowWidth) + (std::size_t)(x - inputBounds.x1);
    };

    if (!out->aliasContentsOf(*input)) {
        std::vector<std::string> copyNames = inputChannelNames;
        for (std::size_t c = 0; c < channelsToWrite.size(); ++c) {
            if (std::find(copyNames.begin(), copyNames.end(), channelsToWrite[c]) == copyNames.end()) {
                copyNames.push_back(channelsToWrite[c]);
            }
        }
        // Only the views handed to the fill pass carry this, and the copy reads nothing through
        // it, so an input with no alpha at all still has a well-formed index to report.
        const int copyAlphaChannelIndex = std::max(0, inputAlphaChannelIndex);

        const StatusEnum copied = renderDeepTwoPass(args, copyNames, copyAlphaChannelIndex, [&](int x, int y) -> U32 {
            if (!inputZData || !inputBounds.contains(x, y)) {
                return 0;
            }

            return inputTable.getCount(inputPixelIndex(x, y)); }, [&](int x, int y, const MutableDeepPixelView& dst) {
            const U64 offset = inputTable.getOffset(inputPixelIndex(x, y));

            for (int s = 0; s < dst.numSamples; ++s) {
                dst.z[s] = inputZData[offset + s];
                dst.zback[s] = inputZBackData ? inputZBackData[offset + s] : dst.z[s];
            }
            for (std::size_t c = 0; c < inputChannels.size(); ++c) {
                for (int s = 0; s < dst.numSamples; ++s) {
                    dst.channels[c][s] = inputChannels[c][offset + s];
                }
            } }, input->isTidy());
        if (copied != eStatusOK) {
            return copied;
        }
    }

    std::vector<float*> writeChannels(channelsToWrite.size());
    for (std::size_t c = 0; c < channelsToWrite.size(); ++c) {
        writeChannels[c] = out->getChannelForWriting(channelsToWrite[c]).dataForWriting();
    }

    const RectI bounds = out->getBounds();
    if (bounds.isNull() || !inputZData) {
        return eStatusOK;
    }

    std::vector<RectI> chunks;
    makeDeepScanlineChunks(bounds, &chunks);

    const SampleTable& table = out->getSampleTable();

    const bool rewritten = forEachDeepChunk(chunks, [&](const RectI& chunk) {
        std::vector<const float*> inPixelChannels(inputChannels.size());
        std::vector<float*> outPixelChannels(writeChannels.size());

        forEachPixelOfChunk(bounds, chunk, [&](int x, int y, std::size_t pixelIndex) {
            const U32 count = table.getCount(pixelIndex);

            if (count == 0) {
                return;
            }

            // Either path gives a pixel samples only where the input has them, so (x, y) lies
            // within the input's bounds here; the two tables only differ on the copy path.
            const U64 offset = table.getOffset(pixelIndex);
            const U64 inputOffset = inputTable.getOffset(inputPixelIndex(x, y));
            for (std::size_t c = 0; c < inPixelChannels.size(); ++c) {
                inPixelChannels[c] = inputChannels[c] + inputOffset;
            }
            for (std::size_t c = 0; c < outPixelChannels.size(); ++c) {
                outPixelChannels[c] = writeChannels[c] + offset;
            }

            DeepPixelView in;
            in.z = inputZData + inputOffset;
            in.zback = inputZBackData ? (inputZBackData + inputOffset) : nullptr;
            in.channels = inPixelChannels.data();
            in.numChannels = (int)inPixelChannels.size();
            in.alphaChannelIndex = inputAlphaChannelIndex;
            in.numSamples = (int)count;

            MutableDeepPixelView outView;
            outView.channels = outPixelChannels.data();
            outView.numChannels = (int)outPixelChannels.size();
            outView.alphaChannelIndex = alphaChannelIndex;
            outView.numSamples = (int)count;

            rewrite(x, y, in, outView);
        });
    });

    return rewritten ? eStatusOK : eStatusFailed;
} // NativeEffectBase::renderDeepFromInput

NATRON_NAMESPACE_EXIT
