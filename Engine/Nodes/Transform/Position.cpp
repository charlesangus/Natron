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

#include "Position.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/PoolParallelFor.h"
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
// image has no pixel. An image in another layout is mapped channel by colour bit, a missing
// channel reading as zero.
void
readRow(const Image* image,
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
    const int start = std::max(x0, bounds.x1);
    const int end = std::min(x0 + width, bounds.x2);
    if (start >= end) {
        return;
    }
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

struct PositionJob {
    ImagePtr dst;
    ImagePtr src;
    std::bitset<4> channels;
    bool anyUnprocessed;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    // Image::getBounds() takes the image's lock, which a band thread must never ask for: behind a
    // writer waiting on an image this render holds for reading, it blocks forever. The bounds are
    // read once on the calling thread instead.
    RectI srcBounds;

    PositionJob()
        : anyUnprocessed(false)
    {
    }
};

struct PositionBand {
    std::size_t job;
    int y1;
    int y2;

    PositionBand(std::size_t jobIndex,
                 int firstRow,
                 int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};
} // anonymous namespace

Position::Position(NodePtr node)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.processesAllLayers = true;

        return traits;
    }())
    , _translate()
{
}

Position::~Position()
{
}

void
Position::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

NativePluginDescription
Position::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_POSITION;
    desc.label = "Position";
    desc.description = tr("Translate an image by an integer number of pixels.\n"
                          "This plugin does not concatenate transforms.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_TRANSFORM;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_POSITION;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Position::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr translate = createKnob<KnobDouble>(tr("Translate"), 2);
    translate->setName(kPositionParamTranslate);
    translate->setHintToolTip(tr("New position of the bottom-left pixel. Rounded to the closest pixel."));
    translate->setSpatial(true);
    translate->disableSlider();
    translate->setDefaultValuesAreNormalized(true);
    for (int d = 0; d < 2; ++d) {
        translate->setMinimum(-DBL_MAX, d);
        translate->setMaximum(DBL_MAX, d);
        translate->setDisplayMinimum(-10000., d);
        translate->setDisplayMaximum(10000., d);
        translate->setDefaultValue(0., d);
    }
    page->addKnob(translate);
    translate->setHasHostOverlayHandle(true);
    _translate = translate;

    KnobBoolPtr interactive = createKnob<KnobBool>(tr("Interactive"));
    interactive->setName(kPositionParamInteractive);
    interactive->setHintToolTip(tr("When checked the image will be rendered whenever moving the overlay interact instead of when releasing the mouse button."));
    interactive->setDefaultValue(false);
    interactive->setAnimationEnabled(false);
    page->addKnob(interactive);
}

void
Position::getPixelShift(double time,
                        ViewIdx view,
                        const RenderScale& scale,
                        int* x,
                        int* y) const
{
    KnobDoublePtr translate = _translate.lock();
    *x = 0;
    *y = 0;
    if (!translate) {
        return;
    }
    const double par = getAspectRatio(-1);
    const double s = scale.toOfxPointD().x;
    const double tx = translate->getValueAtTime(time, 0, view);
    const double ty = translate->getValueAtTime(time, 1, view);

    // Rounding is done in pixels, then converted back to canonical coordinates.
    *x = (int)std::floor(tx * s / par + 0.5);
    *y = (int)std::floor(ty * s + 0.5);
}

StatusEnum
Position::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusReplyDefault;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    if (srcRoD.isNull()) {
        return eStatusReplyDefault;
    }
    int tx, ty;
    getPixelShift(time, view, scale, &tx, &ty);
    const double s = scale.toOfxPointD().x;
    const double dx = tx * getAspectRatio(-1) / s;
    const double dy = ty / s;

    rod->x1 = srcRoD.x1 + dx;
    rod->x2 = srcRoD.x2 + dx;
    rod->y1 = srcRoD.y1 + dy;
    rod->y2 = srcRoD.y2 + dy;

    return eStatusOK;
}

bool
Position::isIdentity(double time,
                     const RenderScale& scale,
                     const RectI& /*roi*/,
                     ViewIdx view,
                     double* inputTime,
                     ViewIdx* inputView,
                     int* inputNb)
{
    int tx, ty;

    getPixelShift(time, view, scale, &tx, &ty);
    if ((tx != 0) || (ty != 0)) {
        return false;
    }
    *inputTime = time;
    *inputView = view;
    *inputNb = 0;

    return true;
}

void
Position::getRegionsOfInterest(double time,
                               const RenderScale& scale,
                               const RectD& /*outputRoD*/,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    int tx, ty;
    getPixelShift(time, view, scale, &tx, &ty);
    if (((tx == 0) && (ty == 0)) || (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) || srcRoD.isNull()) {
        ret->insert(std::make_pair(input, renderWindow));

        return;
    }
    const double s = scale.toOfxPointD().x;
    const double dx = tx * getAspectRatio(-1) / s;
    const double dy = ty / s;
    const RectD srcRoI(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 - dx, renderWindow.y2 - dy);

    ret->insert(std::make_pair(input, srcRoI.intersect(srcRoD)));
}

StatusEnum
Position::render(const RenderActionArgs& args)
{
    int tx, ty;

    getPixelShift(args.time, args.view, args.mappedScale, &tx, &ty);

    // Every image is fetched before any is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    std::vector<PositionJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PositionJob job;
        job.dst = it->second;
        if (job.dst) {
            const int nComps = (int)job.dst->getComponentsCount();
            job.channels = processedBitsForImage(it->first, nComps, args.processChannels);
            for (int c = 0; c < nComps; ++c) {
                if ((c >= 4) || !job.channels[pixelKernelChannelBit(nComps, c)]) {
                    job.anyUnprocessed = true;
                }
            }
            ImageLayerDesc layer;
            if (getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &layer, NULL)) {
                RectI roiPixel;
                job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &layer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
            }
            if (!isFloatImage(job.dst) || !isFloatImage(job.src)) {
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

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses.
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    NativeImageEffect::makeRowBands(roi, nThreads, &bandRects);
    std::vector<PositionBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        PositionJob& job = jobs[j];
        if (!job.dst) {
            continue;
        }
        if (job.src) {
            job.srcBounds = job.src->getBounds();
            job.srcAccess = std::make_shared<Image::ReadAccess>(job.src.get());
        }
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(PositionBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const PositionBand& band = bands[bandIndex];
        const PositionJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> shiftedRow(rowSize);
        std::vector<float> unshiftedRow(job.anyUnprocessed ? rowSize : 0);

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            readRow(job.src.get(), job.srcAccess.get(), job.srcBounds, roi.x1 - tx, y - ty, width, nComps, &shiftedRow[0]);
            if (job.anyUnprocessed) {
                readRow(job.src.get(), job.srcAccess.get(), job.srcBounds, roi.x1, y, width, nComps, &unshiftedRow[0]);
            }

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const std::size_t base = (std::size_t)i * nComps;
                for (int c = 0; c < nComps; ++c) {
                    const bool processed = (c < 4) && job.channels[pixelKernelChannelBit(nComps, c)];
                    dstPix[c] = processed ? shiftedRow[base + c] : unshiftedRow[base + c];
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // Position::render

NATRON_NAMESPACE_EXIT
