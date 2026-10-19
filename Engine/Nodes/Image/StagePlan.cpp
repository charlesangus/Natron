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

#include "StagePlan.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows runStagesCpu() runs between two abort checks.
const int kAbortCheckRows = 16;

// The index in an nComps-channel pixel of the channel on colour bit `bit`, or -1.
int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

// The bits processed in a dstNComps-channel image holding `plane`. A one-channel plane rendered
// into a wider image sits where EffectInstance's copy back reads it: alpha for a colour plane,
// channel 0 otherwise.
std::bitset<4>
processedBitsForImage(const ImageLayerDesc& plane,
                      int dstNComps,
                      const std::bitset<4>& planeBits)
{
    if ((plane.getNumComponents() != 1) || (dstNComps == 1)) {
        return planeBits;
    }
    std::bitset<4> bits;
    bits[plane.isColorLayer() ? 3 : 0] = planeBits[3];

    return bits;
}

// Below this many pixels a band is not worth handing to another thread.
const std::size_t kMinBandPixels = 16384;
// Bands per available thread, so a helper that starts late, on a pool busy with other tasks,
// finds work left rather than leaving the band it would have had to the others.
const int kBandsPerThread = 4;

// Image::getBounds() takes the image's lock, which a band thread must never ask for: behind a
// writer waiting on an image this render holds for reading, it blocks forever. The bounds are
// read once on the calling thread instead.
struct PlaneAccess {
    std::shared_ptr<Image::ReadAccess> src;
    std::shared_ptr<Image::ReadAccess> divisor;
    std::shared_ptr<Image::WriteAccess> dst;
    RectI srcBounds;
    RectI divisorBounds;
};

struct RowBand {
    std::size_t job;
    int y1;
    int y2;

    RowBand(std::size_t jobIndex,
            int firstRow,
            int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

struct PlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    ImagePtr divisor;
    int divisorChannel;
    int skipChannel;

    PlaneJob()
        : dst()
        , channels()
        , src()
        , divisor()
        , divisorChannel(-1)
        , skipChannel(-1)
    {
    }
};

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

// Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel), writing zero
// wherever the image has no pixel. An image in another layout is mapped channel by colour bit,
// a channel it lacks reading as zero.
void
readSourceRow(const Image* image,
              const Image::ReadAccess* access,
              const RectI& bounds,
              int x0,
              int y,
              int width,
              int nComps,
              float* row)
{
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int srcNComps = (int)image->getComponentsCount();
    const float* srcPix = (const float*)access->pixelAt(xStart, y);
    float* dstPix = row + (std::size_t)(xStart - x0) * nComps;
    if (srcNComps == nComps) {
        std::copy(srcPix, srcPix + (std::size_t)(xEnd - xStart) * nComps, dstPix);

        return;
    }
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    for (int x = xStart; x < xEnd; ++x, srcPix += srcNComps, dstPix += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (srcIndex[c] >= 0) {
                dstPix[c] = srcPix[srcIndex[c]];
            }
        }
    }
}

// One value per pixel of `channel` of row y, `fill` wherever the image has no pixel.
void
readChannelRow(const Image* image,
               const Image::ReadAccess* access,
               const RectI& bounds,
               int channel,
               int x0,
               int y,
               int width,
               float fill,
               float* row)
{
    std::fill(row, row + width, fill);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int nComps = (int)image->getComponentsCount();
    const float* pix = (const float*)access->pixelAt(xStart, y);
    for (int x = xStart; x < xEnd; ++x, pix += nComps) {
        row[x - x0] = pix[channel];
    }
}
} // anonymous namespace

int
stageMaskInput(const EffectInstance& effect)
{
    const int nInputs = effect.getNInputs();

    for (int i = 0; i < nInputs; ++i) {
        if (effect.isInputMask(i)) {
            return i;
        }
    }

    return -1;
}

bool
isStageMaskApplied(const EffectInstance& effect)
{
    const int maskInput = stageMaskInput(effect);

    return (maskInput >= 0) && effect.getInput(maskInput) && effect.isMaskEnabled(maskInput);
}

bool
stageMaskMissesWindow(const EffectInstance& effect,
                      int maskInput,
                      double time,
                      const RenderScale& scale,
                      const RectI& roi,
                      ViewIdx view)
{
    EffectInstancePtr mask = effect.getInput(maskInput);

    if (!mask) {
        return false;
    }
    const RenderScale maskScale = mask->supportsRenderScale() ? scale : RenderScale::identity;
    RectD maskRoD;
    bool isProjectFormat = false;
    if (mask->getRegionOfDefinition_public(mask->getRenderHash(), time, maskScale, view, &maskRoD, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    const RectI maskPixels = maskRoD.toPixelEnclosing(scale.toMipmapLevel(), mask->getAspectRatio(-1));

    return !roi.intersects(maskPixels);
}

StagePlan
resolveStagePlan(EffectInstance& effect,
                 const StageSet& stages,
                 const StageKnobs& knobs,
                 int sourceInput,
                 double time,
                 ViewIdx view,
                 const std::bitset<4>& processChannels)
{
    StagePlan plan;

    plan.time = time;
    plan.view = view;
    plan.processChannels = processChannels;
    plan.stages = stages;
    plan.sourceInput = sourceInput;

    if (stages.has(Stage::MaskMix)) {
        const KnobReader reader(time, view);
        plan.mix = (float)reader.valueOr(knobs.mix, 1.);
        plan.maskInvert = reader.valueOr(knobs.maskInvert, false);
        plan.applyMask = isStageMaskApplied(effect);
        if (plan.applyMask) {
            plan.maskInput = stageMaskInput(effect);
            ImageLayerDesc maskLayer;
            int maskChannel = -1;
            if (effect.resolveInputPlaneForRender(plan.maskInput, time, view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
                plan.maskLayer = maskLayer;
                plan.maskChannel = maskChannel;
            }
        }
    }

    NodePtr node = effect.getNode();
    KnobChannelSelectPtr unPremultBy = (stages.has(Stage::Premult) && node) ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    if (unPremultBy && !unPremultBy->isNone() && (sourceInput >= 0) && effect.getInput(sourceInput)) {
        std::list<ImageLayerDesc> availableLayers;
        effect.getAvailableLayers(time, view, sourceInput, &availableLayers);
        plan.divisorChannel = node->getUnPremultChannel(availableLayers, &plan.divisorLayer);
        if (plan.divisorLayer.getNumComponents() == 0) {
            plan.divisorChannel = -1;
        }
    }

    return plan;
} // resolveStagePlan

void
makeStageRowBands(const RectI& roi,
                  int nThreads,
                  std::vector<RectI>* bands)
{
    bands->clear();
    const int height = roi.height();
    if ((height <= 0) || (roi.width() <= 0)) {
        return;
    }
    const std::size_t pixels = (std::size_t)roi.width() * height;
    std::size_t nBands = 1;
    if (nThreads > 1) {
        nBands = std::min((std::size_t)nThreads * kBandsPerThread, pixels / kMinBandPixels);
        nBands = std::max((std::size_t)1, std::min(nBands, (std::size_t)height));
    }
    const int rowsPerBand = (int)((height + nBands - 1) / nBands);
    for (int y = roi.y1; y < roi.y2; y += rowsPerBand) {
        bands->push_back(RectI(roi.x1, y, roi.x2, std::min(y + rowsPerBand, roi.y2)));
    }
}

StatusEnum
runStagesCpu(EffectInstance& effect,
             const StagePlan& plan,
             const EffectInstance::RenderActionArgs& args,
             const StageRowFn& row)
{
    const float mix = plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    const int sourceInput = plan.sourceInput;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = effect.getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && ((maskChannel >= (int)mask->getComponentsCount()))) {
            mask.reset();
        }
    }

    const ImageLayerDesc& divisorLayer = plan.divisorLayer;
    const int divisorChannel = plan.divisorChannel;

    std::vector<PlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            jobs.push_back(job);
            continue;
        }
        const int dstNComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, dstNComps, args.processChannels);

        ImageLayerDesc sourceLayer;
        if ((sourceInput >= 0) && effect.getInput(sourceInput) && effect.resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
            if (job.divisor && (divisorChannel < (int)job.divisor->getComponentsCount())) {
                job.divisorChannel = divisorChannel;
                job.skipChannel = Node::getUnPremultSkipChannel(it->first.isColorLayer() ? job.dst->getComponents() : it->first, divisorLayer, divisorChannel);
            } else {
                job.divisor.reset();
            }
        }
        if (!isFloatImage(job.dst) || !isFloatImage(job.src) || !isFloatImage(job.divisor)) {
            return eStatusFailed;
        }
        jobs.push_back(job);
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    std::shared_ptr<Image::ReadAccess> maskAccess;
    RectI maskBounds;
    if (mask) {
        maskBounds = mask->getBounds();
        maskAccess = std::make_shared<Image::ReadAccess>(mask.get());
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses.
    std::vector<PlaneAccess> accesses(jobs.size());
    std::vector<RowBand> bands;
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        if (!jobs[j].dst) {
            continue;
        }
        if (jobs[j].src) {
            accesses[j].srcBounds = jobs[j].src->getBounds();
            accesses[j].src = std::make_shared<Image::ReadAccess>(jobs[j].src.get());
        }
        if (jobs[j].divisor) {
            accesses[j].divisorBounds = jobs[j].divisor->getBounds();
            accesses[j].divisor = std::make_shared<Image::ReadAccess>(jobs[j].divisor.get());
        }
        accesses[j].dst = std::make_shared<Image::WriteAccess>(jobs[j].dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(RowBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(&effect);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const RowBand& band = bands[bandIndex];
        const PlaneJob& job = jobs[band.job];
        const PlaneAccess& access = accesses[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> sourceRow(rowSize);
        std::vector<float> dividedRow(rowSize);
        std::vector<float> kernelRow(rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> divisorRow(job.divisor ? width : 0);

        RowIO io;
        io.src[0] = &dividedRow[0];
        io.nSrc = 1;
        io.mask = doMask ? &maskRow[0] : 0;
        io.divisor = job.divisor ? &divisorRow[0] : 0;
        io.divisorSkipChannel = job.divisor ? job.skipChannel : -1;
        io.dst = &kernelRow[0];
        io.x0 = roi.x1;
        io.width = width;
        io.nComps = nComps;
        io.channels = job.channels;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            readSourceRow(job.src.get(), access.src.get(), access.srcBounds, roi.x1, y, width, nComps, &sourceRow[0]);
            std::copy(sourceRow.begin(), sourceRow.end(), dividedRow.begin());
            if (job.divisor) {
                readChannelRow(job.divisor.get(), access.divisor.get(), access.divisorBounds, job.divisorChannel, roi.x1, y, width, 1.f, &divisorRow[0]);
                for (int i = 0; i < width; ++i) {
                    const float d = divisorRow[i];
                    if (!Image::unPremultDivisorIsUsable(d)) {
                        continue;
                    }
                    float* pix = &dividedRow[(std::size_t)i * nComps];
                    for (int c = 0; (c < nComps) && (c < 4); ++c) {
                        if (c != job.skipChannel) {
                            pix[c] = Image::unPremultiplyValue(pix[c], d);
                        }
                    }
                }
            }
            if (doMask) {
                readChannelRow(mask.get(), maskAccess.get(), maskBounds, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }

            io.y = y;
            row(io);

            float* dstPix = (float*)access.dst->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &sourceRow[(std::size_t)i * nComps];
                const float* outPix = &kernelRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !job.channels[pixelKernelChannelBit(nComps, c)]) {
                        if (passThrough) {
                            dstPix[c] = srcPix[c];
                        }
                        continue;
                    }
                    float v = outPix[c];
                    if (job.divisor && (c != job.skipChannel)) {
                        v = Image::premultiplyValue(v, divisorRow[i]);
                    }
                    if (alpha == 0.f) {
                        v = srcPix[c];
                    } else if (alpha != 1.f) {
                        v = v * alpha + (1.f - alpha) * srcPix[c];
                    }
                    dstPix[c] = v;
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // runStagesCpu

NATRON_NAMESPACE_EXIT
