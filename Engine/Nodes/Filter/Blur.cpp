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

#include "Blur.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/SpatialFilterSupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuMorphology.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

using namespace SpatialFilter;

namespace {
bool
isGaussianFamily(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterQuasiGaussian) || (filter == BlurKernels::eFilterGaussian);
}

int
boxIterations(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterBox) ? 1 : ((filter == BlurKernels::eFilterTriangle) ? 2 : 3);
}

/// The FIR Gaussian's support for a size (diameter, in pixels at the render scale).
int
firRadius(double size)
{
    return BlurKernels::LineFilter::firGaussianRadius(size / 2.4);
}

#ifdef NATRON_HAVE_GPU
/// The device form of a LineFilter other than the FIR Gaussian.
gpu::GpuLineFilter
toGpuLineFilter(const BlurKernels::LineFilter& filter)
{
    gpu::GpuLineFilter f;

    switch (filter.kind()) {
    case BlurKernels::LineFilter::eKindDeriche:
        f.kind = gpu::GpuLineFilterKind::deriche;
        filter.dericheCoefficients(f.deriche.data());
        break;
    case BlurKernels::LineFilter::eKindVanVliet:
        f.kind = gpu::GpuLineFilterKind::vanVliet;
        filter.vanVlietCoefficients(f.vanVliet.data(), f.triggs.data());
        break;
    case BlurKernels::LineFilter::eKindBox:
        f.kind = gpu::GpuLineFilterKind::box;
        f.boxSize = filter.boxSize();
        f.iterations = filter.boxIterations();
        break;
    case BlurKernels::LineFilter::eKindIdentity:
    case BlurKernels::LineFilter::eKindFIRGaussian:
        break;
    }

    return f;
}
#endif
} // anonymous namespace

Blur::Blur(NodePtr node)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.hostUnPremult = true;
        traits.processesAllLayers = true;

        return traits;
    }())
    , _size()
    , _filter()
    , _cropToFormat()
{
}

Blur::~Blur()
{
}

void
Blur::addAcceptedComponents(int inputNb,
                            std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
Blur::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_BLUR;
    desc.label = "Blur";
    desc.description = tr("Blur the input stream.\n"
                          "The blur filter can be a Gaussian (FIR), a quasi-Gaussian or a Gaussian (both IIR), a box, a triangle or a quadratic filter.\n"
                          "\n"
                          "The default filter, Gaussian (FIR), is a Gaussian [1] implemented as a FIR (finite impulse response) filter, as in most compositing software: the Gaussian impulse response, of standard deviation sigma = size/2.4, is cropped at 3 sigma (rounded up to a whole pixel) and normalised. Its result is exactly zero further than that from any non-zero pixel.\n"
                          "\n"
                          "The Gaussian filter is implemented as an IIR (infinite impulse response) filter [2][3]. Consequently, when blurring a white dot on black background, it produces very small values very far away from the dot. The quasi-Gaussian filter is also IIR.\n"
                          "\n"
                          "A very common process in compositing to expand colors on the edge of a matte is to use the premult-blur-unpremult combination [4][5]. The very small values produced by the IIR filters produce undesirable artifacts after unpremult. For this process, the FIR filters (Gaussian (FIR), quadratic, or the faster triangle or box) should be preferred over the IIR ones.\n"
                          "\n"
                          "References:\n"
                          "[1] https://en.wikipedia.org/wiki/Gaussian_filter\n"
                          "[2] I.T. Young, L.J. van Vliet, M. van Ginkel, Recursive Gabor filtering. IEEE Trans. Sig. Proc., vol. 50, pp. 2799-2805, 2002. (this is an improvement over Young-Van Vliet, Sig. Proc. 44, 1995)\n"
                          "[3] B. Triggs and M. Sdika. Boundary conditions for Young-van Vliet recursive filtering. IEEE Trans. Signal Processing, vol. 54, pp. 2365-2367, 2006.\n"
                          "[4] Nuke Expand Edges or how to get rid of outlines. http://franzbrandstaetter.com/?p=452\n"
                          "[5] Colour Smear for Nuke. http://richardfrazer.com/tools-tutorials/colour-smear-for-nuke/\n"
                          "\n"
                          "The IIR, box, triangle and quadratic filters are ports of the 'vanvliet', 'deriche' and 'boxfilter' functions of the CImg library (http://cimg.eu), "
                          "distributed under the CeCILL-C licence.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_FILTER;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_BLUR;
    desc.minorVersion = PLUGIN_MINOR_NATRON_BLUR;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Blur::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr size = createKnob<KnobDouble>(tr("Size"), 2);
    size->setName(kBlurParamSize);
    size->setHintToolTip(tr("Size (diameter) of the filter kernel, in pixel units (>=0). The standard deviation of the corresponding Gaussian is size/2.4. No blur is applied if size < 0.24 (the Gaussians and quasi-Gaussian) or <= 1 (box, triangle and quadratic)."));
    size->setSpatial(true);
    size->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        size->setMinimum(0., d);
        size->setMaximum(1000., d);
        size->setDisplayMinimum(0., d);
        size->setDisplayMaximum(100., d);
        size->setIncrement(0.1, d);
        size->setDecimals(1, d);
        size->setDefaultValue(0., d);
    }
    size->setAddNewLine(false);
    size->setSpacingBetweenItems(1);
    page->addKnob(size);
    _size = size;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr("Filter"));
    filter->setName(kBlurParamFilter);
    filter->setHintToolTip(tr("Bluring filter. The default, Gaussian (FIR), has a finite support of 3 sigma, so it leaves pixels far from any detail exactly unchanged, which also suits the premult-blur-unpremult process; it is fast for small and moderate sizes. The IIR filters cost the same at any size: the quasi-Gaussian is the faster, the Gaussian is more isotropic (its impulse response has rotational symmetry), but both produce very small non-zero values far from any detail."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBlurParamFilterQuasiGaussian, tr("Quasi-Gaussian").toStdString(), tr("Quasi-Gaussian filter (0-order recursive Deriche filter, faster) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterGaussian, tr("Gaussian").toStdString(), tr("Gaussian filter (Van Vliet recursive Gaussian filter, more isotropic, slower) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterBox, tr("Box").toStdString(), tr("Box filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterTriangle, tr("Triangle").toStdString(), tr("Triangle/tent filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterQuadratic, tr("Quadratic").toStdString(), tr("Quadratic filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterFIRGaussian, tr("Gaussian (FIR)").toStdString(), tr("Gaussian filter cropped at 3 sigma and normalised - FIR (finite support / impulsional response).").toStdString()));
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)BlurKernels::eFilterFIRGaussian);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr cropToFormat = createKnob<KnobBool>(tr("Crop To Format"));
    cropToFormat->setName(kBlurParamCropToFormat);
    cropToFormat->setHintToolTip(tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormat->setDefaultValue(true);
    page->addKnob(cropToFormat);
    _cropToFormat = cropToFormat;

    addMaskMixKnobs(page);
} // Blur::initializeKnobs

void
Blur::getParams(double time,
                ViewIdx view,
                BlurParams* params) const
{
    KnobDoublePtr size = _size.lock();
    KnobChoicePtr filter = _filter.lock();
    KnobBoolPtr cropToFormat = _cropToFormat.lock();

    *params = BlurParams();
    if (size) {
        params->sizeX = size->getValueAtTime(time, 0, view);
        params->sizeY = size->getValueAtTime(time, 1, view);
    }
    const double par = getInput(0) ? getAspectRatio(0) : 0.;
    if (par != 0.) {
        params->sizeX /= par;
    }
    if (filter) {
        const int f = filter->getValueAtTime(time, 0, view);
        if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterFIRGaussian)) {
            params->filter = (BlurKernels::Filter)f;
        }
    }
    params->cropToFormat = cropToFormat ? cropToFormat->getValueAtTime(time, 0, view) : true;
} // Blur::getParams

RectI
Blur::getSourceRoI(const RectI& rect,
                   const RenderScale& scale,
                   const BlurParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;

    if (isGaussianFamily(params.filter)) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);
        if ((sigmax < 0.1) && (sigmay < 0.1)) {
            return rect;
        }
        const int deltaX = std::max(3, (int)std::ceil(sx * 1.5));
        const int deltaY = std::max(3, (int)std::ceil(sy * 1.5));

        return RectI(rect.x1 - deltaX, rect.y1 - deltaY, rect.x2 + deltaX, rect.y2 + deltaY);
    }
    if (params.filter == BlurKernels::eFilterFIRGaussian) {
        const int padX = firRadius(sx);
        const int padY = firRadius(sy);

        return RectI(rect.x1 - padX, rect.y1 - padY, rect.x2 + padX, rect.y2 + padY);
    }
    const int iter = boxIterations(params.filter);
    const int deltaX = iter * static_cast<int>(std::floor((sx - 1) / 2) + 1);
    const int deltaY = iter * static_cast<int>(std::floor((sy - 1) / 2) + 1);

    return RectI(rect.x1 - deltaX, rect.y1 - deltaY, rect.x2 + deltaX, rect.y2 + deltaY);
}

bool
Blur::paramsAreIdentity(const RenderScale& scale,
                        const BlurParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;

    if (isGaussianFamily(params.filter)) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);

        return (sigmax < 0.1) && (sigmay < 0.1);
    }
    if (params.filter == BlurKernels::eFilterFIRGaussian) {
        return !firRadius(sx) && !firRadius(sy);
    }

    return (sx <= 1) && (sy <= 1);
}

bool
Blur::isIdentityOp(double time,
                   const RenderScale& scale,
                   const RectI& /*roi*/,
                   ViewIdx view)
{
    BlurParams params;

    getParams(time, view, &params);

    return paramsAreIdentity(scale, params);
}

StatusEnum
Blur::getRegionOfDefinition(U64 hash,
                            double time,
                            const RenderScale& scale,
                            ViewIdx view,
                            RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }

    BlurParams params;
    getParams(time, view, &params);
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;
    const RectI srcRoDPixel = toPixelEnclosing(srcRoD, scale, getAspectRatio(0));

    // CImgBlurPlugin::getRegionOfDefinition(): an identity blur keeps the default region.
    RectI rodPixel = srcRoDPixel;
    bool changed = false;
    if (!isEmptyRect(srcRoDPixel)) {
        if (isGaussianFamily(params.filter)) {
            const float sigmax = (float)(sx / 2.4);
            const float sigmay = (float)(sy / 2.4);
            if ((sigmax < 0.1) && (sigmay < 0.1)) {
                return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
            }
            const int deltaX = std::max(3, (int)std::ceil(sx * 1.5));
            const int deltaY = std::max(3, (int)std::ceil(sy * 1.5));
            rodPixel.x1 -= deltaX;
            rodPixel.x2 += deltaX;
            rodPixel.y1 -= deltaY;
            rodPixel.y2 += deltaY;
        } else if (params.filter == BlurKernels::eFilterFIRGaussian) {
            const int padX = firRadius(sx);
            const int padY = firRadius(sy);
            if (!padX && !padY) {
                return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
            }
            rodPixel.x1 -= padX;
            rodPixel.x2 += padX;
            rodPixel.y1 -= padY;
            rodPixel.y2 += padY;
        } else {
            if ((sx <= 1) && (sy <= 1)) {
                return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
            }
            const int iter = boxIterations(params.filter);
            const int deltaX = iter * (int)std::ceil((sx - 1) / 2);
            const int deltaY = iter * (int)std::ceil((sy - 1) / 2);
            rodPixel.x1 -= deltaX;
            rodPixel.x2 += deltaX;
            rodPixel.y1 -= deltaY;
            rodPixel.y2 += deltaY;
        }
        changed = true;
    }
    if (params.cropToFormat) {
        // Only the borders the source had inside its format are clamped back to it.
        RectI format;
        EffectInstancePtr formatSource = input->getNearestNonIdentity(time);
        if (formatSource) {
            format = formatSource->getOutputFormat();
        } else {
            Format projectFormat;
            getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
            format = projectFormat;
        }
        RectI enclosing;
        enclosing.x1 = (int)std::floor(format.x1 * rs.x);
        enclosing.x2 = (int)std::ceil(format.x2 * rs.x);
        enclosing.y1 = (int)std::floor(format.y1 * rs.y);
        enclosing.y2 = (int)std::ceil(format.y2 * rs.y);
        RectI inner;
        inner.x1 = (int)std::ceil(format.x1 * rs.x);
        inner.x2 = (int)std::floor(format.x2 * rs.x);
        inner.y1 = (int)std::ceil(format.y1 * rs.y);
        inner.y2 = (int)std::floor(format.y2 * rs.y);
        if (!isEmptyRect(inner)) {
            if ((rodPixel.x1 < inner.x1) && (srcRoDPixel.x1 >= enclosing.x1)) {
                rodPixel.x1 = inner.x1;
                changed = true;
            }
            if ((rodPixel.x2 > inner.x2) && (srcRoDPixel.x2 <= enclosing.x2)) {
                rodPixel.x2 = inner.x2;
                changed = true;
            }
            if ((rodPixel.y1 < inner.y1) && (srcRoDPixel.y1 >= enclosing.y1)) {
                rodPixel.y1 = inner.y1;
                changed = true;
            }
            if ((rodPixel.y2 > inner.y2) && (srcRoDPixel.y2 <= enclosing.y2)) {
                rodPixel.y2 = inner.y2;
                changed = true;
            }
        }
    }
    if (!changed) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    *rod = toCanonical(rodPixel, scale, getAspectRatio(-1));

    return eStatusOK;
} // Blur::getRegionOfDefinition

void
Blur::getRegionsOfInterest(double time,
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

    BlurParams params;
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
} // Blur::getRegionsOfInterest

StatusEnum
Blur::render(const RenderActionArgs& args)
{
    BlurParams params;
    getParams(args.time, args.view, &params);

    const OfxPointD rs = args.mappedScale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;

    // CImgBlurPlugin::blur(): sigma = size / 2.4 for the IIR filters, the box width is the size.
    BlurKernels::LineFilter filterX;
    BlurKernels::LineFilter filterY;
    bool blurred = true;
    if (isGaussianFamily(params.filter)) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);
        if ((sigmax < 0.1) && (sigmay < 0.1)) {
            blurred = false;
        } else {
            filterX = BlurKernels::LineFilter::forFilter(params.filter, sigmax, 0, false);
            filterY = BlurKernels::LineFilter::forFilter(params.filter, sigmay, 0, false);
        }
    } else if (params.filter == BlurKernels::eFilterFIRGaussian) {
        filterX = BlurKernels::LineFilter::firGaussian(sx / 2.4, 0, false);
        filterY = BlurKernels::LineFilter::firGaussian(sy / 2.4, 0, false);
        blurred = !filterX.isIdentity() || !filterY.isIdentity();
    } else {
        filterX = BlurKernels::LineFilter::forFilter(params.filter, static_cast<float>(sx), 0, false);
        filterY = BlurKernels::LineFilter::forFilter(params.filter, static_cast<float>(sy), 0, false);
    }

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

    // The whole window is filtered whatever the mask, so that a pixel's value does not depend on
    // where the mask is zero; the mask only blends in writeWindow().
    RectI processWindow = roi;
    if (mixValue == 0.) {
        processWindow.x2 = processWindow.x1;
        processWindow.y2 = processWindow.y1;
    }
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

    // Left uninitialised: fillBuffer() writes every element, and zeroing here would fault the pages in
    // on this thread alone. Kept across jobs so that a later plane reuses the pages the first one faulted in.
    std::unique_ptr<float[]> bufferStorage;
    std::size_t bufferCapacity = 0;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const PlaneJob& job = jobs[j];
        const ProcessedPlanes planes(job);
        const int nPlanes = planes.count();
        const std::size_t bufferSize = processing ? planeSize * nPlanes : 0;
        if (bufferSize > bufferCapacity) {
            bufferStorage.reset(new float[bufferSize]);
            bufferCapacity = bufferSize;
        }
        float* const buffer = bufferSize ? bufferStorage.get() : NULL;

        if (buffer) {
            // 1. The source over the buffer, black outside its image, divided by the "(Un)premult by"
            // channel where that is usable.
            fillBuffer(job, planes, bufferRect, Boundary::Zero, nThreads, cancel, buffer);
            if (cancel.check()) {
                return eStatusOK;
            }

            if (blurred) {
                // A FIR Gaussian sample costs its 2r + 1 taps, so the passes are split by taps
                // rather than by pixels.
                const bool fir = (params.filter == BlurKernels::eFilterFIRGaussian);
                const std::size_t tapsX = fir ? (std::size_t)(2 * firRadius(sx) + 1) : 1;
                const std::size_t tapsY = fir ? (std::size_t)(2 * firRadius(sy) + 1) : 1;

                // 2. The horizontal pass over whole rows of every plane.
                if (!filterX.isIdentity()) {
                    const std::function<void(int, int)> rows = [&](int first, int end) {
                        BlurKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                                return;
                            }
                            const int p = line / bufferHeight;
                            const int row = line % bufferHeight;
                            filterX.apply(buffer + (std::size_t)p * planeSize + (std::size_t)row * bufferWidth, bufferWidth, 1, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth * tapsX, nThreads, cancel, rows);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                }

                // 3. The vertical pass over whole columns of every plane. The FIR Gaussian takes
                // them a block of adjacent columns at a time, so it reads the plane row by row.
                if (!filterY.isIdentity() && fir) {
                    const int block = BlurKernels::LineFilter::kColumnBlock;
                    const int blocksPerPlane = (bufferWidth + block - 1) / block;
                    const std::function<void(int, int)> columnBlocks = [&](int first, int end) {
                        BlurKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if (cancel.check()) {
                                return;
                            }
                            const int p = line / blocksPerPlane;
                            const int column = (line % blocksPerPlane) * block;
                            filterY.applyColumns(buffer + (std::size_t)p * planeSize + column, bufferHeight, bufferWidth, std::min(block, bufferWidth - column), scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * blocksPerPlane, (std::size_t)bufferHeight * block * tapsY, nThreads, cancel, columnBlocks);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                } else if (!filterY.isIdentity()) {
                    const std::function<void(int, int)> columns = [&](int first, int end) {
                        BlurKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                                return;
                            }
                            const int p = line / bufferWidth;
                            const int column = line % bufferWidth;
                            filterY.apply(buffer + (std::size_t)p * planeSize + column, bufferHeight, bufferWidth, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferWidth, (std::size_t)bufferHeight, nThreads, cancel, columns);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                }
            }
        }

        // 4. The window: multiply back, mask and mix against the undivided source; unprocessed
        // channels and the pixels outside the processed window pass through.
        WindowLayout layout;
        layout.roi = roi;
        layout.processWindow = processWindow;
        layout.processing = processing;
        layout.bufferRect = bufferRect;
        layout.buffer = buffer;
        layout.mix = mix;
        writeWindow(job, planes, maskSource, layout, nThreads, cancel);
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // Blur::render

bool
Blur::canRenderOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > (int)gpu::kGpuBlurMaxChannels)) {
        return false;
    }
    if ((query.unPremultChannel >= 0) && !query.unPremultInPlane) {
        return false;
    }
    BlurParams params;
    getParams(query.time, query.view, &params);
    if (params.filter != BlurKernels::eFilterFIRGaussian) {
        return true;
    }
    const OfxPointD rs = query.mappedScale.toOfxPointD();

    return (firRadius(rs.x * params.sizeX) <= (int)gpu::kGpuBlurMaxRadius) && (firRadius(rs.y * params.sizeY) <= (int)gpu::kGpuBlurMaxRadius);
#else
    Q_UNUSED(query);

    return false;
#endif
}

RectI
Blur::getGpuInputRoI(int inputNb,
                     const RectI& outputRect,
                     const RenderScale& scale) const
{
    if (inputNb != 0) {
        return NativeImageEffect::getGpuInputRoI(inputNb, outputRect, scale);
    }
    BlurParams params;
    getParams(getCurrentTime(), getCurrentView(), &params);

    return getSourceRoI(outputRect, scale, params);
}

StatusEnum
Blur::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    if (!canRenderOnGpu(args.query) || args.inputs.empty()) {
        return eStatusFailed;
    }
    const GpuImageView& src = args.inputs[0];
    const GpuImageView& dst = args.output;
    const int nComps = args.query.nComps;
    const RectI& rect = args.outputRect;
    EffectInstancePtr input = getInput(0);
    if (!input || !isUsableGpuView(src, nComps) || !isUsableGpuView(dst, nComps) || isEmptyRect(rect) || !dst.bounds.contains(rect)) {
        return eStatusFailed;
    }

    const double time = args.query.time;
    const ViewIdx view = args.query.view;
    const RenderScale& scale = args.query.mappedScale;
    BlurParams params;
    getParams(time, view, &params);
    const OfxPointD rs = scale.toOfxPointD();

    // render() filters the window plus its halo clipped to this effect's region of definition, and
    // fillBuffer() resolves the source against the input image.
    RectD ownRoD;
    bool isProjectFormat = false;
    if (getRegionOfDefinition_public(getRenderHash(), time, scale, view, &ownRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    const RectI rodPixel = toPixelEnclosing(ownRoD, scale, getAspectRatio(-1));
    const RectI sourceRoI = getSourceRoI(rect, scale, params);
    RectI domain;
    intersectRects(sourceRoI, rodPixel, &domain);

    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    RectI valid;
    intersectRects(src.bounds, toPixelEnclosing(srcRoD, scale, getAspectRatio(0)), &valid);

    const bool fir = (params.filter == BlurKernels::eFilterFIRGaussian);
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;
    BlurKernels::LineFilter filterX;
    BlurKernels::LineFilter filterY;
    if (!fir && isGaussianFamily(params.filter)) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);
        if ((sigmax >= 0.1) || (sigmay >= 0.1)) {
            filterX = BlurKernels::LineFilter::forFilter(params.filter, sigmax, 0, false);
            filterY = BlurKernels::LineFilter::forFilter(params.filter, sigmay, 0, false);
        }
    } else if (!fir) {
        filterX = BlurKernels::LineFilter::forFilter(params.filter, static_cast<float>(sx), 0, false);
        filterY = BlurKernels::LineFilter::forFilter(params.filter, static_cast<float>(sy), 0, false);
    }

    int divisor = -1;
    if (!getGpuUnPremultComponent(args.query, &divisor)) {
        return eStatusFailed;
    }
    bool allChannels = true;
    for (int c = 0; c < 4; ++c) {
        allChannels = allChannels && args.query.processChannels[c];
    }
    const bool finish = args.query.hasMask || (getMixValue(time, view) != 1.) || !allChannels || (divisor >= 0);

    // The source view may extend past the input's region of definition; render() reads the
    // divisor there as 1, as the wrap does outside its source view.
    GpuRenderArgs wrapArgs(args);
    wrapArgs.inputs[0] = cropGpuView(src, valid);

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBlurImage source = toGpuBlurImage(src);
    if ((divisor >= 0) && !isEmptyRect(valid)) {
        GpuImageView divided;
        divided.bounds = valid;
        divided.nComps = nComps;
        divided.rowStrideFloats = valid.width() * nComps;
        if (!ctx.scratch((VkDeviceSize)valid.area() * nComps * sizeof(float), divided.buffer) || (recordGpuDivide(wrapArgs, divided) != eStatusOK)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
        source = toGpuBlurImage(divided);
    }

    GpuImageView filtered = dst;
    if (finish) {
        filtered = GpuImageView();
        filtered.bounds = rect;
        filtered.nComps = nComps;
        filtered.rowStrideFloats = rect.width() * nComps;
        if (!ctx.scratch((VkDeviceSize)rect.area() * nComps * sizeof(float), filtered.buffer)) {
            return eStatusFailed;
        }
    }

    if (fir) {
        const std::vector<float> weightsX = BlurKernels::LineFilter::firGaussianWeights(sx / 2.4);
        const std::vector<float> weightsY = BlurKernels::LineFilter::firGaussianWeights(sy / 2.4);
        gpu::GpuSeparableBlur blur;
        blur.src = source;
        blur.srcValid = toGpuBlurRect(valid);
        blur.dst = toGpuBlurImage(filtered);
        blur.output = toGpuBlurRect(rect);
        blur.domain = toGpuBlurRect(domain);
        blur.channels = (uint32_t)nComps;
        blur.weightsX = weightsX;
        blur.weightsY = weightsY;
        if (!gpu::recordSeparableBlur(ctx, blur)) {
            return eStatusFailed;
        }
    } else {
        // A recursive filter depends on whole lines, so the filtered rect must be render()'s: the
        // window grown by the halo and clipped to the region of definition.
        gpu::GpuBuffer* window = nullptr;
        if (!gpu::recordFilterWindow(ctx, toGpuBlurRect(rect), toGpuBlurRect(rodPixel), rect.x1 - sourceRoI.x1, rect.y1 - sourceRoI.y1, window)) {
            return eStatusFailed;
        }
        gpu::GpuLineBlur blur;
        blur.src = source;
        blur.srcValid = toGpuBlurRect(valid);
        blur.dst = toGpuBlurImage(filtered);
        blur.output = toGpuBlurRect(rect);
        blur.domain = toGpuBlurRect(domain);
        blur.window = window;
        blur.channels = (uint32_t)nComps;
        blur.filterX = toGpuLineFilter(filterX);
        blur.filterY = toGpuLineFilter(filterY);
        if (!gpu::recordLineBlur(ctx, blur)) {
            return eStatusFailed;
        }
    }
    if (!finish) {
        return eStatusOK;
    }
    ctx.computeBarrier();

    return recordGpuFinish(wrapArgs, filtered);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // Blur::renderGpu

NATRON_NAMESPACE_EXIT
