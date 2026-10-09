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

#include "NativeGenerator.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QThread>

#include <ofxMetadata.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows render() runs between two abort checks.
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

// The bits written in a dstNComps-channel image holding `plane`. A one-channel plane rendered
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

bool
writesEveryChannel(int nComps,
                   const std::bitset<4>& channels)
{
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        if (!channels[pixelKernelChannelBit(nComps, c)]) {
            return false;
        }
    }

    return true;
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

struct GeneratorPlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    bool passThrough;

    GeneratorPlaneJob()
        : dst()
        , channels()
        , src()
        , passThrough(false)
    {
    }
};

struct GeneratorBand {
    std::size_t job;
    int y1;
    int y2;

    GeneratorBand(std::size_t jobIndex,
                  int firstRow,
                  int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

bool
isAnimatedKnob(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    for (int d = 0; d < knob->getDimension(); ++d) {
        if (knob->isAnimated(d)) {
            return true;
        }
    }

    return false;
}

ExtentPolicy
generatorExtentPolicy()
{
    ExtentPolicy policy;

    policy.defaultExtent = ExtentKnobs::eExtentDefault;
    policy.reformatToggle = true;
    policy.overlayFollowsExtent = true;
    policy.recenterOnSource = false;

    return policy;
}

NativeImageTraits
generatorTraits()
{
    NativeImageTraits traits;

    traits.generator = true;

    return traits;
}
} // anonymous namespace

NativeGenerator::NativeGenerator(NodePtr node)
    : NativeImageEffect(node, generatorTraits())
    , _extentKnobs(this, generatorExtentPolicy())
{
}

NativeGenerator::~NativeGenerator()
{
}

void
NativeGenerator::describeSourceInput(NativePluginDescription* desc)
{
    desc->inputs.push_back(NativeInputDescription(kNativeGeneratorSourceInputLabel, true, eDataKindImage));
}

void
NativeGenerator::initializeGeneratorKnobs(const KnobPagePtr& page)
{
    _extentKnobs.createKnobs(page);
    _extentKnobs.finishKnobs();
}

NativeGenerator::ExtentEnum
NativeGenerator::getExtent() const
{
    return _extentKnobs.getExtent();
}

RectD
NativeGenerator::getProjectExtentRect() const
{
    return _extentKnobs.getProjectExtentRect();
}

bool
NativeGenerator::knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double time,
                             bool /*originatedFromMainThread*/)
{
    return _extentKnobs.onKnobChanged(k, reason, time);
}

void
NativeGenerator::onKnobsLoaded()
{
    _extentKnobs.updateVisibility();
}

bool
NativeGenerator::getExtentRegionOfDefinition(double time,
                                             ViewIdx view,
                                             RectD* rod) const
{
    return _extentKnobs.getExtentRegionOfDefinition(time, view, rod);
}

StatusEnum
NativeGenerator::getRegionOfDefinition(U64 /*hash*/,
                                       double time,
                                       const RenderScale& scale,
                                       ViewIdx view,
                                       RectD* rod)
{
    if (getExtentRegionOfDefinition(time, view, rod)) {
        return eStatusOK;
    }

    // The host's default for an OpenFX general-context effect: the Source clip when it is
    // connected, else the project extent from the origin.
    EffectInstancePtr source = getInput(0);
    if (source) {
        const RenderScale sourceScale = source->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;

        return source->getRegionOfDefinition_public(source->getRenderHash(), time, sourceScale, view, rod, &isProjectFormat);
    }
    const RectD project = getProjectExtentRect();
    rod->x1 = 0.;
    rod->y1 = 0.;
    rod->x2 = project.x2;
    rod->y2 = project.y2;

    return eStatusOK;
}

void
NativeGenerator::getFrameRange(double* first,
                               double* last)
{
    KnobIntPtr frameRange = _extentKnobs.getFrameRangeKnob();

    *first = frameRange ? frameRange->getValue(0) : 1.;
    *last = frameRange ? frameRange->getValue(1) : 1.;
}

ImageMetadata
NativeGenerator::getOutputMetadata(double /*time*/,
                                   ViewIdx /*view*/)
{
    ImageMetadata metadata;

    metadata.setDouble(kOfxMetadataKeyFrameRate, getApp()->getProjectFrameRate());
    metadata.setDouble(kOfxMetadataKeyPixelAspect, getAspectRatio(-1));

    return metadata;
}

StatusEnum
NativeGenerator::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setIsContinuous(true);
    metadata.setNComps(-1, 4);
    metadata.setComponentsType(-1, kNatronColorLayerID);

    double par = 0.;
    switch (getExtent()) {
    case ExtentKnobs::eExtentFormat: {
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        par = formatPar ? formatPar->getValue() : 1.;
        break;
    }
    case ExtentKnobs::eExtentProject:
    case ExtentKnobs::eExtentDefault: {
        _extentKnobs.getProjectExtentRect(&par);
        break;
    }
    case ExtentKnobs::eExtentSize: {
        KnobBoolPtr reformat = _extentKnobs.getReformatKnob();
        if (reformat && reformat->getValue() && !isAnimatedKnob(_extentKnobs.getBottomLeftKnob()) && !isAnimatedKnob(_extentKnobs.getSizeKnob())) {
            par = 1.;
        }
        break;
    }
    }

    if (par != 0.) {
        metadata.setPixelAspectRatio(-1, par);
        RectD rod;
        if (getExtentRegionOfDefinition(0., ViewIdx(0), &rod)) {
            RectI format(0, 0, 0, 0);
            if ((rod.x2 > rod.x1) && (rod.y2 > rod.y1)) {
                format.x1 = (int)std::floor(rod.x1 / par + 0.5);
                format.y1 = (int)std::floor(rod.y1 + 0.5);
                format.x2 = (int)std::ceil(rod.x2 / par - 0.5);
                format.y2 = (int)std::ceil(rod.y2 - 0.5);
            }
            metadata.setOutputFormat(format);
        }
    }

    return eStatusOK;
} // NativeGenerator::getPreferredMetadata

StatusEnum
NativeGenerator::render(const RenderActionArgs& args)
{
    KernelContext context;
    context.time = args.time;
    context.view = args.view;
    context.mappedScale = args.mappedScale;
    context.processChannels = args.processChannels;
    const PixelKernelPtr kernel = makeKernel(context);
    if (!kernel) {
        return eStatusFailed;
    }

    // Every source image is fetched before any is locked: fetching renders upstream, which may
    // write into a cached image this render would otherwise already hold a read lock on.
    std::vector<GeneratorPlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        GeneratorPlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            jobs.push_back(job);
            continue;
        }
        if (job.dst->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const int dstNComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, dstNComps, args.processChannels);
        job.passThrough = !writesEveryChannel(dstNComps, job.channels);

        ImageLayerDesc sourceLayer;
        if (job.passThrough && getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
            if (job.src && (job.src->getBitDepth() != eImageBitDepthFloat)) {
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
    // compute pixel addresses through these accesses. Their bounds are read here too, because
    // Image::getBounds() takes the lock, which a band thread must not ask for: behind a writer
    // waiting on an image this render holds for reading, it blocks forever.
    std::vector<std::shared_ptr<Image::ReadAccess>> srcAccesses(jobs.size());
    std::vector<RectI> srcBounds(jobs.size());
    std::vector<std::shared_ptr<Image::WriteAccess>> dstAccesses(jobs.size());
    std::vector<GeneratorBand> bands;
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeRowBands(roi, nThreads, &bandRects);
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        if (!jobs[j].dst) {
            continue;
        }
        if (jobs[j].src) {
            srcBounds[j] = jobs[j].src->getBounds();
            srcAccesses[j] = std::make_shared<Image::ReadAccess>(jobs[j].src.get());
        }
        dstAccesses[j] = std::make_shared<Image::WriteAccess>(jobs[j].dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(GeneratorBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const GeneratorBand& band = bands[bandIndex];
        const GeneratorPlaneJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();

        RowIO io;
        io.nSrc = 0;
        io.x0 = roi.x1;
        io.width = width;
        io.nComps = nComps;
        io.channels = job.channels;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)dstAccesses[band.job]->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            if (job.passThrough) {
                readSourceRow(job.src.get(), srcAccesses[band.job].get(), srcBounds[band.job], roi.x1, y, width, nComps, dstPix);
            }
            io.y = y;
            io.dst = dstPix;
            kernel->processRow(io);
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // NativeGenerator::render

NATRON_NAMESPACE_EXIT
