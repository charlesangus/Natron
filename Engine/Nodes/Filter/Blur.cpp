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
#include <cfloat>
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
} // anonymous namespace

Blur::Blur(NodePtr node)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.hostUnPremult = true;
        traits.processesAllLayers = true;

        return traits;
    }())
    , _size()
    , _uniform()
    , _orderX()
    , _orderY()
    , _boundary()
    , _filter()
    , _expandRoD()
    , _cropToFormat()
    , _alphaThreshold()
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
    desc.description = tr("Blur input stream or compute derivatives.\n"
                          "The blur filter can be a quasi-Gaussian, a Gaussian, a box, a triangle or a quadratic filter.\n"
                          "\n"
                          "Note that the Gaussian filter [1] is implemented as an IIR (infinite impulse response) filter [2][3], whereas most compositing software implement the Gaussian as a FIR (finite impulse response) filter by cropping the Gaussian impulse response. Consequently, when blurring a white dot on black background, it produces very small values very far away from the dot. The quasi-Gaussian filter is also IIR.\n"
                          "\n"
                          "A very common process in compositing to expand colors on the edge of a matte is to use the premult-blur-unpremult combination [4][5]. The very small values produced by the IIR Gaussian filter produce undesirable artifacts after unpremult. For this process, the FIR quadratic filter (or the faster triangle or box filters) should be preferred over the IIR Gaussian filter.\n"
                          "\n"
                          "References:\n"
                          "[1] https://en.wikipedia.org/wiki/Gaussian_filter\n"
                          "[2] I.T. Young, L.J. van Vliet, M. van Ginkel, Recursive Gabor filtering. IEEE Trans. Sig. Proc., vol. 50, pp. 2799-2805, 2002. (this is an improvement over Young-Van Vliet, Sig. Proc. 44, 1995)\n"
                          "[3] B. Triggs and M. Sdika. Boundary conditions for Young-van Vliet recursive filtering. IEEE Trans. Signal Processing, vol. 54, pp. 2365-2367, 2006.\n"
                          "[4] Nuke Expand Edges or how to get rid of outlines. http://franzbrandstaetter.com/?p=452\n"
                          "[5] Colour Smear for Nuke. http://richardfrazer.com/tools-tutorials/colour-smear-for-nuke/\n"
                          "\n"
                          "The filters are ports of the 'vanvliet', 'deriche' and 'boxfilter' functions of the CImg library (http://cimg.eu), "
                          "distributed under the CeCILL-C licence.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_FILTER;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_BLUR;
    desc.minorVersion = 0;
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
    size->setHintToolTip(tr("Size (diameter) of the filter kernel, in pixel units (>=0). The standard deviation of the corresponding Gaussian is size/2.4. No blur is applied if size < 0.24 (Gaussian and quasi-Gaussian) or <= 1 (box, triangle and quadratic)."));
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

    KnobBoolPtr uniform = createKnob<KnobBool>(tr("Uniform"));
    uniform->setName(kBlurParamUniform);
    uniform->setHintToolTip(tr("Apply the same amount of blur on X and Y."));
    uniform->setDefaultValue(false);
    page->addKnob(uniform);
    _uniform = uniform;

    KnobIntPtr orderX = createKnob<KnobInt>(tr("X derivation order"));
    orderX->setName(kBlurParamOrderX);
    orderX->setHintToolTip(tr("Derivation order in the X direction. (orderX=0,orderY=0) does smoothing, (orderX=1,orderY=0) computes the X component of the image gradient."));
    orderX->setMinimum(0);
    orderX->setMaximum(2);
    orderX->setDisplayMinimum(0);
    orderX->setDisplayMaximum(2);
    orderX->setDefaultValue(0);
    page->addKnob(orderX);
    _orderX = orderX;

    KnobIntPtr orderY = createKnob<KnobInt>(tr("Y derivation order"));
    orderY->setName(kBlurParamOrderY);
    orderY->setHintToolTip(tr("Derivation order in the Y direction. (orderX=0,orderY=0) does smoothing, (orderX=0,orderY=1) computes the X component of the image gradient."));
    orderY->setMinimum(0);
    orderY->setMaximum(2);
    orderY->setDisplayMinimum(0);
    orderY->setDisplayMaximum(2);
    orderY->setDefaultValue(0);
    page->addKnob(orderY);
    _orderY = orderY;

    KnobChoicePtr boundary = createKnob<KnobChoice>(tr("Border Conditions"));
    boundary->setName(kBlurParamBoundary);
    boundary->setHintToolTip(tr("Specifies how pixel values are computed out of the image domain. This mostly affects values at the boundary of the image. If the image represents intensities, Nearest (Neumann) conditions should be used. If the image represents gradients or derivatives, Black (Dirichlet) boundary conditions should be used."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBlurParamBoundaryBlack, tr("Black").toStdString(), tr("Dirichlet boundary condition: pixel values out of the image domain are zero.").toStdString()));
        options.push_back(ChoiceOption(kBlurParamBoundaryNearest, tr("Nearest").toStdString(), tr("Neumann boundary condition: pixel values out of the image domain are those of the closest pixel location in the image domain.").toStdString()));
        boundary->populateChoices(options);
    }
    boundary->setDefaultValue(0);
    page->addKnob(boundary);
    _boundary = boundary;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr("Filter"));
    filter->setName(kBlurParamFilter);
    filter->setHintToolTip(tr("Bluring filter. The quasi-Gaussian filter should be appropriate in most cases. The Gaussian filter is more isotropic (its impulse response has rotational symmetry), but slower."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBlurParamFilterQuasiGaussian, tr("Quasi-Gaussian").toStdString(), tr("Quasi-Gaussian filter (0-order recursive Deriche filter, faster) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterGaussian, tr("Gaussian").toStdString(), tr("Gaussian filter (Van Vliet recursive Gaussian filter, more isotropic, slower) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterBox, tr("Box").toStdString(), tr("Box filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterTriangle, tr("Triangle").toStdString(), tr("Triangle/tent filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterQuadratic, tr("Quadratic").toStdString(), tr("Quadratic filter - FIR (finite support / impulsional response).").toStdString()));
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)BlurKernels::eFilterGaussian);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr expandRoD = createKnob<KnobBool>(tr("Expand RoD"));
    expandRoD->setName(kBlurParamExpandRoD);
    expandRoD->setHintToolTip(tr("Expand the source region of definition by 1.5*size (3.6*sigma)."));
    expandRoD->setDefaultValue(true);
    expandRoD->setAddNewLine(false);
    page->addKnob(expandRoD);
    _expandRoD = expandRoD;

    KnobBoolPtr cropToFormat = createKnob<KnobBool>(tr("Crop To Format"));
    cropToFormat->setName(kBlurParamCropToFormat);
    cropToFormat->setHintToolTip(tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormat->setDefaultValue(true);
    page->addKnob(cropToFormat);
    _cropToFormat = cropToFormat;

    KnobDoublePtr alphaThreshold = createKnob<KnobDouble>(tr("Alpha Threshold"));
    alphaThreshold->setName(kBlurParamAlphaThreshold);
    alphaThreshold->setHintToolTip(tr("If this value is non-zero, any alpha value below this is set to zero. This is only useful for IIR filters (Gaussian and Quasi-Gaussian), which may produce alpha values very close to zero due to arithmetic precision. Remind that, in theory, a black image with a single white pixel should produce non-zero values everywhere, but a few VFX tricks rely on the fact that alpha should be zero far from the alpha edges (e.g. the premult-blur-unpremult trick to fill holes)). A threshold value of 0.003 is reasonable, and values between 0.001 and 0.01 are usually enough to remove these artifacts."));
    alphaThreshold->setMinimum(0.);
    alphaThreshold->setMaximum(DBL_MAX);
    alphaThreshold->setDisplayMinimum(0.);
    alphaThreshold->setDisplayMaximum(1.);
    alphaThreshold->setDefaultValue(0.);
    page->addKnob(alphaThreshold);
    _alphaThreshold = alphaThreshold;

    addMaskMixKnobs(page);

    updateUniformVisibility();
} // Blur::initializeKnobs

void
Blur::updateUniformVisibility()
{
    KnobBoolPtr uniform = _uniform.lock();

    if (!uniform) {
        return;
    }
    const bool hide = !uniform->getValue() && !uniform->isAnimated(0);
    uniform->setSecret(hide);
    uniform->setAllDimensionsEnabled(!hide);
}

void
Blur::onKnobsLoaded()
{
    updateUniformVisibility();
}

void
Blur::getParams(double time,
                ViewIdx view,
                BlurParams* params) const
{
    KnobDoublePtr size = _size.lock();
    KnobBoolPtr uniform = _uniform.lock();
    KnobIntPtr orderX = _orderX.lock();
    KnobIntPtr orderY = _orderY.lock();
    KnobChoicePtr boundary = _boundary.lock();
    KnobChoicePtr filter = _filter.lock();
    KnobBoolPtr expandRoD = _expandRoD.lock();
    KnobBoolPtr cropToFormat = _cropToFormat.lock();
    KnobDoublePtr alphaThreshold = _alphaThreshold.lock();

    *params = BlurParams();
    if (size) {
        params->sizeX = size->getValueAtTime(time, 0, view);
        params->sizeY = size->getValueAtTime(time, 1, view);
    }
    if (uniform && uniform->getValueAtTime(time, 0, view)) {
        params->sizeY = params->sizeX;
    }
    const double par = getInput(0) ? getAspectRatio(0) : 0.;
    if (par != 0.) {
        params->sizeX /= par;
    }
    params->orderX = orderX ? std::max(0, orderX->getValueAtTime(time, 0, view)) : 0;
    params->orderY = orderY ? std::max(0, orderY->getValueAtTime(time, 0, view)) : 0;
    params->neumann = boundary ? (boundary->getValueAtTime(time, 0, view) == 1) : false;
    if (filter) {
        const int f = filter->getValueAtTime(time, 0, view);
        if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterQuadratic)) {
            params->filter = (BlurKernels::Filter)f;
        }
    }
    params->expandRoD = expandRoD ? expandRoD->getValueAtTime(time, 0, view) : true;
    params->cropToFormat = cropToFormat ? cropToFormat->getValueAtTime(time, 0, view) : true;
    params->alphaThreshold = alphaThreshold ? alphaThreshold->getValueAtTime(time, 0, view) : 0.;
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
        if ((sigmax < 0.1) && (sigmay < 0.1) && (params.orderX == 0) && (params.orderY == 0)) {
            return rect;
        }
        const int deltaX = std::max(3, (int)std::ceil(sx * 1.5));
        const int deltaY = std::max(3, (int)std::ceil(sy * 1.5));

        return RectI(rect.x1 - deltaX - params.orderX,
                     rect.y1 - deltaY - params.orderY,
                     rect.x2 + deltaX + params.orderX,
                     rect.y2 + deltaY + params.orderY);
    }
    const int iter = boxIterations(params.filter);
    const int deltaX = iter * static_cast<int>(std::floor((sx - 1) / 2) + 1);
    const int deltaY = iter * static_cast<int>(std::floor((sy - 1) / 2) + 1);
    const int derivX = (params.orderX > 0) ? 1 : 0;
    const int derivY = (params.orderY > 0) ? 1 : 0;

    return RectI(rect.x1 - deltaX - derivX,
                 rect.y1 - deltaY - derivY,
                 rect.x2 + deltaX + derivX,
                 rect.y2 + deltaY + derivY);
}

bool
Blur::paramsAreIdentity(const RenderScale& scale,
                        const BlurParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;

    if ((params.orderX != 0) || (params.orderY != 0)) {
        return false;
    }
    if (isGaussianFamily(params.filter)) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);

        return (sigmax < 0.1) && (sigmay < 0.1);
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
    if (params.expandRoD && !isEmptyRect(srcRoDPixel)) {
        if (isGaussianFamily(params.filter)) {
            const float sigmax = (float)(sx / 2.4);
            const float sigmay = (float)(sy / 2.4);
            if ((sigmax < 0.1) && (sigmay < 0.1) && (params.orderX == 0) && (params.orderY == 0)) {
                return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
            }
            const int deltaX = std::max(3, (int)std::ceil(sx * 1.5));
            const int deltaY = std::max(3, (int)std::ceil(sy * 1.5));
            rodPixel.x1 -= deltaX + params.orderX;
            rodPixel.x2 += deltaX + params.orderX;
            rodPixel.y1 -= deltaY + params.orderY;
            rodPixel.y2 += deltaY + params.orderY;
        } else {
            if ((sx <= 1) && (sy <= 1) && (params.orderX == 0) && (params.orderY == 0)) {
                return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
            }
            const int iter = boxIterations(params.filter);
            const int deltaX = iter * (int)std::ceil((sx - 1) / 2);
            const int deltaY = iter * (int)std::ceil((sy - 1) / 2);
            const int derivX = (params.orderX > 0) ? 1 : 0;
            const int derivY = (params.orderY > 0) ? 1 : 0;
            rodPixel.x1 -= deltaX + derivX;
            rodPixel.x2 += deltaX + derivX;
            rodPixel.y1 -= deltaY + derivY;
            rodPixel.y2 += deltaY + derivY;
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
        if ((sigmax < 0.1) && (sigmay < 0.1) && (params.orderX == 0) && (params.orderY == 0)) {
            blurred = false;
        } else {
            filterX = BlurKernels::LineFilter::forFilter(params.filter, sigmax, (unsigned int)params.orderX, params.neumann);
            filterY = BlurKernels::LineFilter::forFilter(params.filter, sigmay, (unsigned int)params.orderY, params.neumann);
        }
    } else {
        // CImg's box filter is the identity at size 0, but a derivative is still wanted there.
        const double boxX = (params.orderX && (sx <= 0.)) ? 1e-8 : sx;
        const double boxY = (params.orderY && (sy <= 0.)) ? 1e-8 : sy;
        filterX = BlurKernels::LineFilter::forFilter(params.filter, static_cast<float>(boxX), (unsigned int)params.orderX, params.neumann);
        filterY = BlurKernels::LineFilter::forFilter(params.filter, static_cast<float>(boxY), (unsigned int)params.orderY, params.neumann);
    }

    // d f(a x) / dx = a f'(a x): a derivative computed at a reduced scale is scaled back.
    double derivativeScale = 1.;
    if (blurred && (((params.orderX > 0) && (rs.x != 1)) || ((params.orderY > 0) && (rs.y != 1)))) {
        if (params.orderX > 0) {
            derivativeScale *= rs.x;
            if (params.orderX > 1) {
                derivativeScale *= rs.x;
            }
        }
        if (params.orderY > 0) {
            derivativeScale *= rs.y;
            if (params.orderY > 1) {
                derivativeScale *= rs.y;
            }
        }
    }
    const bool scaleDerivative = (derivativeScale != 1.);
    const bool thresholdAlpha = blurred && (params.alphaThreshold > 0.);

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
        std::vector<float> buffer(processing ? planeSize * nPlanes : 0);

        if (!buffer.empty()) {
            // 1. The source over the buffer, outside its image by the boundary condition, divided by the "(Un)premult by"
            // channel where that is usable.
            fillBuffer(job, planes, bufferRect, params.neumann ? Boundary::Nearest : Boundary::Zero, nThreads, cancel, &buffer[0]);
            if (cancel.check()) {
                return eStatusOK;
            }

            if (blurred) {
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
                            filterX.apply(&buffer[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth], bufferWidth, 1, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth, nThreads, cancel, rows);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                }

                // 3. The vertical pass over whole columns of every plane.
                if (!filterY.isIdentity()) {
                    const std::function<void(int, int)> columns = [&](int first, int end) {
                        BlurKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                                return;
                            }
                            const int p = line / bufferWidth;
                            const int column = line % bufferWidth;
                            filterY.apply(&buffer[(std::size_t)p * planeSize + column], bufferHeight, bufferWidth, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferWidth, (std::size_t)bufferHeight, nThreads, cancel, columns);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                }
            }
        }

        // 4. The window: derivative scale and alpha threshold, multiply back, mask and mix
        // against the undivided source; unprocessed channels and the pixels outside the
        // processed window pass through.
        WindowLayout layout;
        layout.roi = roi;
        layout.processWindow = processWindow;
        layout.processing = processing;
        layout.bufferRect = bufferRect;
        layout.buffer = buffer.empty() ? NULL : &buffer[0];
        layout.mix = mix;
        writeWindow(job, planes, maskSource, layout, nThreads, cancel, [&](float v, int p) -> float {
            if (scaleDerivative) {
                v = (float)(v * derivativeScale);
            }
            if (thresholdAlpha && (p == planes.alphaPlane) && (v < params.alphaThreshold)) {
                v = 0.f;
            }

            return v;
        });
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // Blur::render

NATRON_NAMESPACE_EXIT
