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

#include "ErodeDilate.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/SpatialFilterSupport.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

using namespace SpatialFilter;

namespace ErodeDilateKernels {
namespace {
    inline float
    reduce(float a,
           float b,
           bool takeMax)
    {
        return takeMax ? ((b > a) ? b : a) : ((b < a) ? b : a);
    }
} // anonymous namespace

void
filterLine(float* data,
           int length,
           std::ptrdiff_t stride,
           int halfWidth,
           bool takeMax,
           LineScratch& scratch)
{
    if ((halfWidth <= 0) || (length <= 1)) {
        return;
    }
    if (length <= halfWidth + 2) {
        float extremum = data[0];
        for (int i = 1; i < length; ++i) {
            extremum = reduce(extremum, data[(std::ptrdiff_t)i * stride], takeMax);
        }
        for (int i = 0; i < length; ++i) {
            data[(std::ptrdiff_t)i * stride] = extremum;
        }

        return;
    }

    const int width = 2 * halfWidth + 1;
    const int n = length + 2 * halfWidth;
    const float neutral = takeMax ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    scratch.padded.resize(n);
    scratch.prefix.resize(n);
    scratch.suffix.resize(n);
    float* padded = &scratch.padded[0];
    float* prefix = &scratch.prefix[0];
    float* suffix = &scratch.suffix[0];

    std::fill(padded, padded + halfWidth, neutral);
    for (int i = 0; i < length; ++i) {
        padded[halfWidth + i] = data[(std::ptrdiff_t)i * stride];
    }
    std::fill(padded + halfWidth + length, padded + n, neutral);

    // Running extremum inside each block of `width` values, from the block start and from its end:
    // a window spans at most two blocks, so it is the combination of one suffix and one prefix.
    for (int j = 0; j < n; ++j) {
        prefix[j] = ((j % width) == 0) ? padded[j] : reduce(prefix[j - 1], padded[j], takeMax);
    }
    for (int j = n - 1; j >= 0; --j) {
        suffix[j] = ((j == n - 1) || (((j + 1) % width) == 0)) ? padded[j] : reduce(suffix[j + 1], padded[j], takeMax);
    }
    for (int i = 0; i < length; ++i) {
        data[(std::ptrdiff_t)i * stride] = reduce(suffix[i], prefix[i + width - 1], takeMax);
    }
}
} // namespace ErodeDilateKernels

ErodeDilate::ErodeDilate(NodePtr node)
    : ErodeDilate(node, false)
{
}

ErodeDilate::ErodeDilate(NodePtr node,
                         bool dilate)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.hostUnPremult = true;
        traits.processesAllLayers = true;

        return traits;
    }())
    , _dilate(dilate)
    , _size()
    , _expandRoD()
{
}

ErodeDilate::~ErodeDilate()
{
}

void
ErodeDilate::addAcceptedComponents(int inputNb,
                                   std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
ErodeDilate::getNativePluginDescription() const
{
    NativePluginDescription desc;

    if (_dilate) {
        desc.id = PLUGINID_NATRON_DILATE;
        desc.label = "Dilate";
        desc.description = tr("Dilate (or erode) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n"
                              "A negative size will perform an erosion instead of a dilation.\n"
                              "Different sizes can be given for the x and y axis.\n"
                              "The operation is the 'dilate' and 'erode' behaviour of the CImg library (http://cimg.eu), "
                              "distributed under the CeCILL-C licence.")
                               .toStdString();
        desc.majorVersion = PLUGIN_MAJOR_NATRON_DILATE;
    } else {
        desc.id = PLUGINID_NATRON_ERODE;
        desc.label = "Erode";
        desc.description = tr("Erode (or dilate) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n"
                              "A negative size will perform a dilation instead of an erosion.\n"
                              "Different sizes can be given for the x and y axis.\n"
                              "The operation is the 'erode' and 'dilate' behaviour of the CImg library (http://cimg.eu), "
                              "distributed under the CeCILL-C licence.")
                               .toStdString();
        desc.majorVersion = PLUGIN_MAJOR_NATRON_ERODE;
    }
    desc.grouping = PLUGIN_GROUP_FILTER;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
ErodeDilate::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobIntPtr size = createKnob<KnobInt>(tr("Size"), 2);
    size->setName(kErodeDilateParamSize);
    size->setHintToolTip(tr("Width/height of the rectangular structuring element is 2*size+1, in pixel units (>=0)."));
    for (int d = 0; d < 2; ++d) {
        size->setMinimum(-1000, d);
        size->setMaximum(1000, d);
        size->setDisplayMinimum(-100, d);
        size->setDisplayMaximum(100, d);
        size->setDefaultValue(1, d);
    }
    page->addKnob(size);
    _size = size;

    KnobBoolPtr expandRoD = createKnob<KnobBool>(tr("Expand RoD"));
    expandRoD->setName(kErodeDilateParamExpandRoD);
    if (_dilate) {
        expandRoD->setHintToolTip(tr("Expand the source region of definition by 2*size pixels if size is positive"));
    } else {
        expandRoD->setHintToolTip(tr("Expand the source region of definition by 2*size pixels if size is negative"));
    }
    expandRoD->setDefaultValue(true);
    page->addKnob(expandRoD);
    _expandRoD = expandRoD;

    addMaskMixKnobs(page);
}

void
ErodeDilate::getParams(double time,
                       ViewIdx view,
                       ErodeDilateParams* params) const
{
    KnobIntPtr size = _size.lock();
    KnobBoolPtr expandRoD = _expandRoD.lock();

    *params = ErodeDilateParams();
    if (size) {
        params->sizeX = size->getValueAtTime(time, 0, view);
        params->sizeY = size->getValueAtTime(time, 1, view);
    }
    params->expandRoD = expandRoD ? expandRoD->getValueAtTime(time, 0, view) : true;
}

RectI
ErodeDilate::getSourceRoI(const RectI& rect,
                          const RenderScale& scale,
                          const ErodeDilateParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();
    const int deltaX = (int)std::ceil(std::abs(params.sizeX) * rs.x);
    const int deltaY = (int)std::ceil(std::abs(params.sizeY) * rs.y);

    return RectI(rect.x1 - deltaX, rect.y1 - deltaY, rect.x2 + deltaX, rect.y2 + deltaY);
}

bool
ErodeDilate::paramsAreIdentity(const RenderScale& scale,
                               const ErodeDilateParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();

    return (std::floor(params.sizeX * rs.x) == 0) && (std::floor(params.sizeY * rs.y) == 0);
}

void
ErodeDilate::getExpansion(const RenderScale& scale,
                          const ErodeDilateParams& params,
                          bool dilate,
                          int* deltaX,
                          int* deltaY)
{
    const OfxPointD rs = scale.toOfxPointD();
    const bool growsX = dilate ? (params.sizeX > 0) : (params.sizeX < 0);
    const bool growsY = dilate ? (params.sizeY > 0) : (params.sizeY < 0);

    *deltaX = growsX ? (int)std::ceil(std::abs(params.sizeX) * rs.x) : 0;
    *deltaY = growsY ? (int)std::ceil(std::abs(params.sizeY) * rs.y) : 0;
}

bool
ErodeDilate::isIdentityOp(double time,
                          const RenderScale& scale,
                          const RectI& /*roi*/,
                          ViewIdx view)
{
    ErodeDilateParams params;

    getParams(time, view, &params);

    return paramsAreIdentity(scale, params);
}

StatusEnum
ErodeDilate::getRegionOfDefinition(U64 hash,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    ErodeDilateParams params;
    getParams(time, view, &params);
    if (!params.expandRoD) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }

    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    RectI rodPixel = toPixelEnclosing(srcRoD, scale, getAspectRatio(0));
    if (isEmptyRect(rodPixel)) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    int deltaX = 0;
    int deltaY = 0;
    getExpansion(scale, params, _dilate, &deltaX, &deltaY);
    rodPixel.x1 -= deltaX;
    rodPixel.x2 += deltaX;
    rodPixel.y1 -= deltaY;
    rodPixel.y2 += deltaY;
    *rod = toCanonical(rodPixel, scale, getAspectRatio(-1));

    return eStatusOK;
}

void
ErodeDilate::getRegionsOfInterest(double time,
                                  const RenderScale& scale,
                                  const RectD& outputRoD,
                                  const RectD& renderWindow,
                                  ViewIdx view,
                                  RoIMap* ret)
{
    EffectInstance::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(0);
    if (!input) {
        return;
    }
    const bool doMasking = isMaskApplied();
    double mix = 1.;
    if (doMasking) {
        mix = getMixValue(time, view);
        if (mix == 0.) {
            return;
        }
    }

    ErodeDilateParams params;
    getParams(time, view, &params);
    const double par = getAspectRatio(0);
    const RectI rectPixel = toPixelEnclosing(renderWindow, scale, par);
    RectD srcRoI = toCanonical(getSourceRoI(rectPixel, scale, params), scale, par);
    if (doMasking && (mix != 1.)) {
        srcRoI.merge(renderWindow);
    }
    // The mask may be the same effect as the source, in which case both needs share one entry.
    RoIMap::iterator found = ret->find(input);
    if (found != ret->end()) {
        if (isMaskApplied() && (getInput(getMaskInput()) == input)) {
            srcRoI.merge(found->second);
        }
        found->second = srcRoI;
    } else {
        ret->insert(std::make_pair(input, srcRoI));
    }
}

StatusEnum
ErodeDilate::render(const RenderActionArgs& args)
{
    ErodeDilateParams params;
    getParams(args.time, args.view, &params);

    const OfxPointD rs = args.mappedScale.toOfxPointD();
    // The operation of the positive axes first, then the opposite one on the negative axes.
    const int positiveHalfX = (int)std::floor(std::max(0, params.sizeX) * rs.x);
    const int positiveHalfY = (int)std::floor(std::max(0, params.sizeY) * rs.y);
    const int negativeHalfX = (int)std::floor(std::max(0, -params.sizeX) * rs.x);
    const int negativeHalfY = (int)std::floor(std::max(0, -params.sizeY) * rs.y);
    struct Phase {
        int halfX;
        int halfY;
        bool takeMax;
    };
    const Phase phases[2] = {
        { positiveHalfX, positiveHalfY, _dilate },
        { negativeHalfX, negativeHalfY, !_dilate }
    };

    const double mixValue = getMixValue(args.time, args.view);
    const float mix = (float)mixValue;
    const bool maskInvert = getMaskInvertValue(args.time, args.view);
    NodePtr node = getNode();

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = isMaskApplied();
    ImagePtr mask;
    int maskChannel = -1;
    if (doMask) {
        const int maskInput = getMaskInput();
        ImageLayerDesc maskLayer;
        if (resolveInputPlaneForRender(maskInput, args.time, args.view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
            RectI maskRoI;
            mask = getImage(maskInput, args.time, args.mappedScale, args.view, NULL, &maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && ((maskChannel >= (int)mask->getComponentsCount()))) {
            mask.reset();
        }
    }

    KnobChannelSelectPtr unPremultBy = node ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    ImageLayerDesc divisorLayer;
    int divisorChannel = -1;
    if (unPremultBy && !unPremultBy->isNone() && getInput(0)) {
        std::list<ImageLayerDesc> availableLayers;
        getAvailableLayers(args.time, args.view, 0, &availableLayers);
        divisorChannel = node->getUnPremultChannel(availableLayers, &divisorLayer);
        if (divisorLayer.getNumComponents() == 0) {
            divisorChannel = -1;
        }
    }

    std::vector<PlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            continue;
        }
        job.nComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, job.nComps, args.processChannels);

        ImageLayerDesc sourceLayer;
        if (getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = getImage(0, args.time, args.mappedScale, args.view, NULL, &divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
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
    if ((roi.width() <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // The filters run over the window plus the halo, clipped to this effect's own region of
    // definition, as CImgFilterPluginHelper::render() clips its source region of interest.
    RectD ownRoD;
    bool isProjectFormat = false;
    if (getRegionOfDefinition_public(getRenderHash(), args.time, args.mappedScale, args.view, &ownRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    const RectI dstRoDPixel = toPixelEnclosing(ownRoD, args.mappedScale, getAspectRatio(-1));

    // Images are locked here, on the calling thread, for the whole render; the pool threads only
    // compute pixel addresses through these accesses.
    MaskInput maskSource;
    maskSource.applied = doMask;
    maskSource.invert = maskInvert;
    maskSource.image = mask;
    maskSource.channel = maskChannel;
    maskSource.lock();
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        jobs[j].lock();
    }

    // The processed window shrinks past the rows and columns where the mask is zero, exactly as
    // CImgFilterPluginHelper::render() does: the shrunk window decides the filtered extent.
    RectI processWindow = roi;
    if (mixValue == 0.) {
        processWindow.x2 = processWindow.x1;
        processWindow.y2 = processWindow.y1;
    }
    processWindow = shrinkToMask(processWindow, maskSource);
    const bool processing = !isEmptyRect(processWindow);

    RectI bufferRect(0, 0, 0, 0);
    if (processing) {
        intersectRects(getSourceRoI(processWindow, args.mappedScale, params), dstRoDPixel, &bufferRect);
    }
    const int bufferWidth = std::max(0, bufferRect.width());
    const int bufferHeight = std::max(0, bufferRect.height());
    const std::size_t planeSize = (std::size_t)bufferWidth * bufferHeight;

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);

    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const PlaneJob& job = jobs[j];
        const ProcessedPlanes planes(job);
        const int nPlanes = planes.count();
        std::vector<float> buffer((processing && (nPlanes > 0)) ? planeSize * nPlanes : 0);

        if (!buffer.empty()) {
            // 1. The source over the buffer, zero outside its image, divided by the "(Un)premult by"
            // channel where that is usable.
            fillBuffer(job, planes, bufferRect, Boundary::Zero, nThreads, cancel, &buffer[0]);
            if (cancel.check()) {
                return eStatusOK;
            }

            // 2. Each phase runs its horizontal pass over whole rows and its vertical pass over
            // whole columns of every plane.
            for (int ph = 0; ph < 2; ++ph) {
                const Phase& phase = phases[ph];
                if (phase.halfX > 0) {
                    const std::function<void(int, int)> rows = [&](int first, int end) {
                        ErodeDilateKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                                return;
                            }
                            const int p = line / bufferHeight;
                            const int row = line % bufferHeight;
                            ErodeDilateKernels::filterLine(&buffer[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth], bufferWidth, 1, phase.halfX, phase.takeMax, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth, nThreads, cancel, rows);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                }
                if (phase.halfY > 0) {
                    const std::function<void(int, int)> columns = [&](int first, int end) {
                        ErodeDilateKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                                return;
                            }
                            const int p = line / bufferWidth;
                            const int column = line % bufferWidth;
                            ErodeDilateKernels::filterLine(&buffer[(std::size_t)p * planeSize + column], bufferHeight, (std::ptrdiff_t)bufferWidth, phase.halfY, phase.takeMax, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferWidth, (std::size_t)bufferHeight, nThreads, cancel, columns);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                }
            }
        }

        // 3. The window: multiply back, mask and mix against the undivided source; unprocessed
        // channels and the pixels outside the processed window pass through.
        WindowLayout layout;
        layout.roi = roi;
        layout.processWindow = processWindow;
        layout.processing = processing;
        layout.bufferRect = bufferRect;
        layout.buffer = buffer.empty() ? NULL : &buffer[0];
        layout.mix = mix;
        writeWindow(job, planes, maskSource, layout, nThreads, cancel, [](float v, int) -> float { return v; });
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // ErodeDilate::render

NATRON_NAMESPACE_EXIT
