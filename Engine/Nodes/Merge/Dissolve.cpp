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

#include "Dissolve.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kAbortCheckRows = 16;

int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

// A one-channel plane rendered into a wider image sits where EffectInstance's copy back reads
// it: alpha for a colour plane, channel 0 otherwise.
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

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

// Fills row (nComps floats per pixel, `width` pixels from x0) from row y of image, zero where the
// image has no pixel, and returns the pixel range [*xStart, *xEnd) it does have. An image in
// another layout is mapped channel by colour bit, a missing channel reading as zero.
void
readRow(const Image* image,
        const Image::ReadAccess* access,
        const RectI& bounds,
        int x0,
        int y,
        int width,
        int nComps,
        float* row,
        int* xStart,
        int* xEnd)
{
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    *xStart = x0;
    *xEnd = x0;
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int start = std::max(x0, bounds.x1);
    const int end = std::min(x0 + width, bounds.x2);
    if (start >= end) {
        return;
    }
    *xStart = start;
    *xEnd = end;
    const int srcNComps = (int)image->getComponentsCount();
    const float* srcPix = (const float*)access->pixelAt(start, y);
    float* dstPix = row + (std::size_t)(start - x0) * nComps;
    if (srcNComps == nComps) {
        std::copy(srcPix, srcPix + (std::size_t)(end - start) * nComps, dstPix);

        return;
    }
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    for (int x = start; x < end; ++x, srcPix += srcNComps, dstPix += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (srcIndex[c] >= 0) {
                dstPix[c] = srcPix[srcIndex[c]];
            }
        }
    }
}

void
readChannelRow(const Image* image,
               const Image::ReadAccess* access,
               const RectI& bounds,
               int channel,
               int x0,
               int y,
               int width,
               float* row)
{
    std::fill(row, row + width, 0.f);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int start = std::max(x0, bounds.x1);
    const int end = std::min(x0 + width, bounds.x2);
    if (start >= end) {
        return;
    }
    const int nComps = (int)image->getComponentsCount();
    const float* pix = (const float*)access->pixelAt(start, y);
    for (int x = start; x < end; ++x, pix += nComps) {
        row[x - x0] = pix[channel];
    }
}

struct DissolveJob {
    ImagePtr dst;
    ImagePtr from;
    ImagePtr to;
    ImagePtr unprocessed;
    std::bitset<4> channels;
    std::shared_ptr<Image::ReadAccess> fromAccess;
    std::shared_ptr<Image::ReadAccess> toAccess;
    std::shared_ptr<Image::ReadAccess> unprocessedAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    // Image::getBounds() takes the image's lock, which a band thread must never ask for: behind a
    // writer waiting on an image this render holds for reading, it blocks forever. The bounds are
    // read once on the calling thread instead.
    RectI fromBounds;
    RectI toBounds;
    RectI unprocessedBounds;
};

struct DissolveBand {
    std::size_t job;
    int y1;
    int y2;

    DissolveBand(std::size_t jobIndex,
                 int firstRow,
                 int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};
} // anonymous namespace

Dissolve::Dissolve(NodePtr node)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.processesAllLayers = true;

        return traits;
    }())
    , _which()
    , _dissolveMaskInvert()
{
}

Dissolve::~Dissolve()
{
}

void
Dissolve::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
Dissolve::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DISSOLVE;
    desc.label = "Dissolve";
    desc.description = tr("Weighted average of two inputs.").toStdString();
    desc.grouping = PLUGIN_GROUP_MERGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_DISSOLVE;
    desc.minorVersion = 0;
    for (int k = 0; k < kDissolveSourceCount; ++k) {
        if (k == kDissolveMaskInput) {
            desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
        }
        desc.inputs.push_back(NativeInputDescription(std::to_string(k), true, eDataKindImage));
    }
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Dissolve::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr which = createKnob<KnobDouble>(tr("Which"));
    which->setName(kDissolveParamWhich);
    which->setHintToolTip(tr("Mix factor between the inputs."));
    which->setDefaultValue(0.);
    which->setMinimum(0.);
    which->setMaximum(kDissolveSourceCount - 1);
    which->setDisplayMinimum(0.);
    which->setDisplayMaximum(1.);
    page->addKnob(which);
    _which = which;

    KnobBoolPtr maskInvert = createKnob<KnobBool>(std::string("Invert Mask"));
    maskInvert->setName(kOfxMaskInvertParamName);
    maskInvert->setHintToolTip(tr("When checked, the effect is fully applied where the mask is 0."));
    maskInvert->setDefaultValue(false);
    page->addKnob(maskInvert);
    _dissolveMaskInvert = maskInvert;
}

void
Dissolve::onInputChanged(int /*inputNo*/)
{
    updateRange();
}

void
Dissolve::updateRange()
{
    KnobDoublePtr which = _which.lock();

    if (!which) {
        return;
    }
    int maxConnected = 1;
    for (int k = 2; k < kDissolveSourceCount; ++k) {
        if (isSourceConnected(k)) {
            maxConnected = k;
        }
    }
    which->setDisplayMinimum(0.);
    which->setDisplayMaximum(maxConnected);
}

double
Dissolve::getWhich(double time,
                   ViewIdx view) const
{
    KnobDoublePtr which = _which.lock();
    const double value = which ? which->getValueAtTime(time, 0, view) : 0.;

    return std::max(0., std::min(value, (double)(kDissolveSourceCount - 1)));
}

bool
Dissolve::isSourceConnected(int k) const
{
    return bool(getInput(sourceInput(k)));
}

bool
Dissolve::getDissolveMaskInvert(double time,
                                ViewIdx view) const
{
    KnobBoolPtr maskInvert = _dissolveMaskInvert.lock();

    return maskInvert ? maskInvert->getValueAtTime(time, 0, view) : false;
}

StatusEnum
Dissolve::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    const double which = getWhich(time, view);
    const int prev = (int)which;
    const int next = std::min((int)which + 1, kDissolveSourceCount - 1);

    const std::function<StatusEnum(int, RectD*)> sourceRoD = [&](int k, RectD* r) {
        EffectInstancePtr input = getInput(sourceInput(k));
        const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;

        return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, r, &isProjectFormat);
    };

    if ((which <= 0.) && isSourceConnected(0)) {
        return (sourceRoD(0, rod) == eStatusFailed) ? eStatusFailed : eStatusOK;
    }
    if (((double)prev == which) && isSourceConnected(prev) && !isMaskApplied()) {
        return (sourceRoD(prev, rod) == eStatusFailed) ? eStatusFailed : eStatusOK;
    }
    if (isSourceConnected(prev) && isSourceConnected(next)) {
        RectD fromRoD;
        RectD toRoD;
        if ((sourceRoD(prev, &fromRoD) == eStatusFailed) || (sourceRoD(next, &toRoD) == eStatusFailed)) {
            return eStatusFailed;
        }
        *rod = fromRoD;
        rod->merge(toRoD);

        return eStatusOK;
    }

    // OpenFX's default region in the general context when every clip is optional: the project
    // extent from the origin, not the union of the connected inputs.
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    const RectD canonical = format.toCanonical_noClipping(0, format.getPixelAspectRatio());
    *rod = RectD(0., 0., canonical.right(), canonical.top());

    return eStatusOK;
}

bool
Dissolve::isIdentity(double time,
                     const RenderScale& scale,
                     const RectI& roi,
                     ViewIdx view,
                     double* inputTime,
                     ViewIdx* inputView,
                     int* inputNb)
{
    *inputTime = time;
    *inputView = view;

    const double which = getWhich(time, view);
    const int prev = (int)which;

    if (which <= 0.) {
        *inputNb = sourceInput(0);

        return true;
    }

    const bool maskApplied = isMaskApplied();
    if (((double)prev == which) && !maskApplied) {
        *inputNb = sourceInput(prev);

        return true;
    }

    // Where the mask does not reach the output is the lower source unchanged; the OpenFX plug-in
    // answers its input 0 there whatever `which` is.
    if (maskApplied && !getDissolveMaskInvert(time, view)) {
        EffectInstancePtr mask = getInput(kDissolveMaskInput);
        const RenderScale maskScale = mask->supportsRenderScale() ? scale : RenderScale::identity;
        RectD maskRoD;
        bool isProjectFormat = false;
        if (mask->getRegionOfDefinition_public(mask->getRenderHash(), time, maskScale, view, &maskRoD, &isProjectFormat) != eStatusFailed) {
            const RectI maskPixels = maskRoD.toPixelEnclosing(scale.toMipmapLevel(), mask->getAspectRatio(-1));
            if (!roi.intersects(maskPixels)) {
                *inputNb = sourceInput(prev);

                return true;
            }
        }
    }

    return false;
}

void
Dissolve::getRegionsOfInterest(double time,
                               const RenderScale& /*scale*/,
                               const RectD& /*outputRoD*/,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    const double which = getWhich(time, view);
    const int prev = (int)std::floor(which);
    const int next = (int)std::ceil(which);

    if (isSourceConnected(prev)) {
        ret->insert(std::make_pair(getInput(sourceInput(prev)), renderWindow));
    }
    if (isSourceConnected(next)) {
        ret->insert(std::make_pair(getInput(sourceInput(next)), renderWindow));
    }
    if ((prev != next) && isMaskApplied()) {
        ret->insert(std::make_pair(getInput(kDissolveMaskInput), renderWindow));
    }
}

FramesNeededMap
Dissolve::getFramesNeeded(double time,
                          ViewIdx view)
{
    FramesNeededMap ret;
    RangeD range;

    range.min = range.max = time;
    std::vector<RangeD> ranges;
    ranges.push_back(range);
    FrameRangesMap viewRanges;
    viewRanges.insert(std::make_pair(view, ranges));

    const double which = getWhich(time, view);
    const int prev = (int)std::floor(which);
    const int next = (int)std::ceil(which);
    if (isSourceConnected(prev)) {
        ret.insert(std::make_pair(sourceInput(prev), viewRanges));
    }
    if (isSourceConnected(next)) {
        ret.insert(std::make_pair(sourceInput(next), viewRanges));
    }
    if ((prev != next) && isMaskApplied()) {
        ret.insert(std::make_pair((int)kDissolveMaskInput, viewRanges));
    }

    return ret;
}

StatusEnum
Dissolve::render(const RenderActionArgs& args)
{
    const double which = getWhich(args.time, args.view);
    const int prev = (int)std::floor(which);
    const int next = (int)std::ceil(which);
    const bool copyOnly = (prev == next);
    const float blend = (float)(which - prev);
    const float blendComp = 1.f - blend;
    const int fromInput = sourceInput(prev);
    const int toInput = sourceInput(next);
    const bool doMask = !copyOnly && isMaskApplied();
    const bool maskInvert = doMask && getDissolveMaskInvert(args.time, args.view);

    int unprocessedInput = getNode()->getPreferredInput();
    if ((unprocessedInput != fromInput) && (unprocessedInput != toInput)) {
        unprocessedInput = -1;
    }

    // Every image is fetched before any is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    const std::function<ImagePtr(int)> fetch = [&](int inputNb) {
        ImageLayerDesc layer;
        ImagePtr image;

        if (getInput(inputNb) && resolveInputPlaneForRender(inputNb, args.time, args.view, &layer, NULL)) {
            RectI roiPixel;
            image = getImage(inputNb, args.time, args.mappedScale, args.view, NULL, &layer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
        }

        return image;
    };

    ImagePtr mask;
    int maskChannel = -1;
    if (doMask) {
        ImageLayerDesc maskLayer;
        if (resolveInputPlaneForRender(kDissolveMaskInput, args.time, args.view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
            RectI maskRoI;
            mask = getImage(kDissolveMaskInput, args.time, args.mappedScale, args.view, NULL, &maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }

    std::vector<DissolveJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        DissolveJob job;
        job.dst = it->second;
        if (job.dst) {
            job.channels = processedBitsForImage(it->first, (int)job.dst->getComponentsCount(), args.processChannels);
            job.from = fetch(fromInput);
            job.to = copyOnly ? job.from : fetch(toInput);
            if (unprocessedInput == fromInput) {
                job.unprocessed = job.from;
            } else if (unprocessedInput == toInput) {
                job.unprocessed = job.to;
            }
            if (!isFloatImage(job.dst) || !isFloatImage(job.from) || !isFloatImage(job.to)) {
                return eStatusFailed;
            }
        }
        jobs.push_back(job);
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
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    NativeImageEffect::makeRowBands(roi, nThreads, &bandRects);
    std::vector<DissolveBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        DissolveJob& job = jobs[j];
        if (!job.dst) {
            continue;
        }
        if (job.from) {
            job.fromBounds = job.from->getBounds();
            job.fromAccess = std::make_shared<Image::ReadAccess>(job.from.get());
        }
        if (job.to) {
            job.toBounds = job.to->getBounds();
            job.toAccess = (job.to == job.from) ? job.fromAccess : std::make_shared<Image::ReadAccess>(job.to.get());
        }
        if (job.unprocessed) {
            job.unprocessedBounds = job.unprocessed->getBounds();
            job.unprocessedAccess = (job.unprocessed == job.from) ? job.fromAccess : job.toAccess;
        }
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(DissolveBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    QThread* const callingThread = QThread::currentThread();
    std::atomic<bool> wasAborted(false);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const DissolveBand& band = bands[bandIndex];
        const DissolveJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> fromRow(rowSize);
        std::vector<float> toRow(rowSize);
        std::vector<float> unprocessedRow(rowSize);
        std::vector<float> maskRow(doMask ? width : 0);

        for (int y = band.y1; y < band.y2; ++y) {
            if (((y - band.y1) % kAbortCheckRows) == 0) {
                if (wasAborted.load(std::memory_order_relaxed)) {
                    return;
                }
                // Only the calling thread carries the render's TLS, so only it may ask.
                if ((QThread::currentThread() == callingThread) && aborted()) {
                    wasAborted = true;

                    return;
                }
            }

            int fromStart = roi.x1, fromEnd = roi.x1, toStart = roi.x1, toEnd = roi.x1, unusedStart, unusedEnd;
            readRow(job.from.get(), job.fromAccess.get(), job.fromBounds, roi.x1, y, width, nComps, &fromRow[0], &fromStart, &fromEnd);
            if (copyOnly) {
                std::copy(fromRow.begin(), fromRow.end(), toRow.begin());
                toStart = fromStart;
                toEnd = fromEnd;
            } else {
                readRow(job.to.get(), job.toAccess.get(), job.toBounds, roi.x1, y, width, nComps, &toRow[0], &toStart, &toEnd);
            }
            if (job.unprocessed) {
                readRow(job.unprocessed.get(), job.unprocessedAccess.get(), job.unprocessedBounds, roi.x1, y, width, nComps, &unprocessedRow[0], &unusedStart, &unusedEnd);
            }
            if (doMask) {
                readChannelRow(mask.get(), maskAccess.get(), maskBounds, maskChannel, roi.x1, y, width, &maskRow[0]);
            }

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const int x = roi.x1 + i;
                const bool hasFrom = (x >= fromStart) && (x < fromEnd);
                const bool hasTo = (x >= toStart) && (x < toEnd);
                const float* fromPix = &fromRow[(std::size_t)i * nComps];
                const float* toPix = &toRow[(std::size_t)i * nComps];
                float alpha = 0.f;
                if (doMask) {
                    float maskScale = maskRow[i];
                    if (maskInvert) {
                        maskScale = 1.f - maskScale;
                    }
                    alpha = maskScale * blend;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !job.channels[pixelKernelChannelBit(nComps, c)]) {
                        dstPix[c] = unprocessedRow[(std::size_t)i * nComps + c];
                        continue;
                    }
                    float v;
                    if (copyOnly) {
                        v = fromPix[c];
                    } else if (doMask) {
                        if (!hasFrom && !hasTo) {
                            v = 0.f;
                        } else if (alpha == 0.f) {
                            v = fromPix[c];
                        } else if (alpha == 1.f) {
                            v = toPix[c];
                        } else if (hasFrom) {
                            v = toPix[c] * alpha + (1.f - alpha) * fromPix[c];
                        } else {
                            v = toPix[c] * alpha;
                        }
                    } else if (hasFrom && hasTo) {
                        v = (toPix[c] - fromPix[c]) * blend + fromPix[c];
                    } else if (hasFrom) {
                        v = fromPix[c] * blendComp;
                    } else if (hasTo) {
                        v = toPix[c] * blend;
                    } else {
                        v = 0.f;
                    }
                    dstPix[c] = v;
                }
            }
        }
    };
    parallelForOnGlobalPool((int)bands.size(), nThreads, renderBand);

    return eStatusOK;
} // Dissolve::render

NATRON_NAMESPACE_EXIT
