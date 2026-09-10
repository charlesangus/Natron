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

    const std::size_t rowWidth = (std::size_t)bounds.width();
    QThread* const callingThread = QThread::currentThread();
    std::atomic<bool> wasAborted(false);
    SampleTable& table = out->getSampleTableForWriting();

    // Pass 1: per-pixel sample counts. Distinct pixels are distinct elements of the count
    // vector, so threads never collide, and nothing is allocated.
    QtConcurrent::blockingMap(chunks, [&](RectI chunk) {
        QThread* const curThread = QThread::currentThread();
        const bool spawnedThread = (curThread != callingThread);

        if (spawnedThread) {
            appPTR->getAppTLS()->copyTLS(callingThread, curThread);
        }
        if (aborted()) {
            wasAborted = true;
        } else {
            for (int y = chunk.y1; y < chunk.y2; ++y) {
                const std::size_t rowIndex = (std::size_t)(y - bounds.y1) * rowWidth;
                for (int x = chunk.x1; x < chunk.x2; ++x) {
                    table.setCount(rowIndex + (std::size_t)(x - bounds.x1), countSamples(x, y));
                }
            }
        }
        if (spawnedThread) {
            appPTR->getAppTLS()->cleanupTLSForThread();
        }
    });

    if (wasAborted) {
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
    QtConcurrent::blockingMap(chunks, [&](RectI chunk) {
        QThread* const curThread = QThread::currentThread();
        const bool spawnedThread = (curThread != callingThread);

        if (spawnedThread) {
            appPTR->getAppTLS()->copyTLS(callingThread, curThread);
        }
        if (aborted()) {
            wasAborted = true;
        } else {
            std::vector<float*> pixelChannels(channelNames.size());
            for (int y = chunk.y1; y < chunk.y2; ++y) {
                const std::size_t rowIndex = (std::size_t)(y - bounds.y1) * rowWidth;
                for (int x = chunk.x1; x < chunk.x2; ++x) {
                    const std::size_t pixelIndex = rowIndex + (std::size_t)(x - bounds.x1);
                    const U32 count = table.getCount(pixelIndex);

                    if (count == 0) {
                        continue;
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
                }
            }
        }
        if (spawnedThread) {
            appPTR->getAppTLS()->cleanupTLSForThread();
        }
    });

    if (wasAborted) {
        return eStatusFailed;
    }

    out->setTidy(resultIsTidy);

    return eStatusOK;
} // NativeEffectBase::renderDeepTwoPass

NATRON_NAMESPACE_EXIT
