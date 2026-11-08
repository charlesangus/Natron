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

#include "GodRays.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/TimeLine.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
#ifdef NATRON_HAVE_GPU
// dirblur.slang's parameter block layout.
constexpr std::size_t kGodRaysGpuHeaderWords = 6;
constexpr std::size_t kGodRaysGpuSampleWords = 16;
constexpr uint32_t kGodRaysGpuSourceValid = 1;
constexpr uint32_t kGodRaysGpuMax = 2;
constexpr uint32_t kGodRaysGpuClamp = 16;
constexpr uint32_t kGodRaysGpuBlackOutside = 32;
constexpr uint32_t kGodRaysGpuIdentityNearest = 64;
#endif

constexpr InputSpec kGodRaysInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kGodRaysNodeSpec = {
    .id = PLUGINID_NATRON_GODRAYS,
    .label = "GodRays",
    .description = "Streak light out of the image: rays from the bright parts of the image, through a transform about Center.\n"
                   "\n"
                   "The result is the average of Samples copies of the image, transformed by even steps from none of the transform (Translate, Rotate, Scale and Skew, about Center) to all of it, and resampled with Filter, as the Transform node resamples, or their maximum with Max. Scaling up about Center streaks the image away from it; translating smears it along a line.\n"
                   "\n"
                   "The copies are tinted from From Color, at the untransformed copy, to To Color, at the fully transformed one. Gamma bends the ramp between them: above 1 the tint reaches To Color sooner, below 1 later. With white tints the rays keep the image's energy; a darker To Color fades them out.\n"
                   "\n"
                   "Beyond the source's region of definition the image reads black, or with Black outside off the source's edge pixels, extended outwards.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_GODRAYS,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/GodRays.png",
    .inputs = kGodRaysInputs,
};

constexpr ImagePipelineSpec kGodRaysPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Custom,
};

RectD
nullRect()
{
    return RectD(0., 0., 0., 0.);
}

void
mergeInto(const RectD& r,
          RectD* into)
{
    if (r.isNull()) {
        return;
    }
    if (into->isNull()) {
        *into = r;
    } else {
        into->merge(r);
    }
}
} // namespace

GodRays::GodRays(NodePtr node)
    : StagedImageEffect(node)
{
}

GodRays::~GodRays()
{
}

const NodeSpec*
GodRays::nodeSpec() const
{
    return staticNodeSpec<kGodRaysNodeSpec>();
}

const ImagePipelineSpec&
GodRays::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kGodRaysPipeline, kGodRaysNodeSpec>();
}

std::string
GodRays::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kGodRaysInputSource:
        return tr("The image to streak.").toStdString();
    case kGodRaysInputMask:
        return tr("The mask the rays are mixed with the source through.").toStdString();
    }

    return std::string();
}

void
GodRays::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(this, tr("Translate"), 2);
    translateKnob->setName(kGodRaysParamTranslate);
    translateKnob->setHintToolTip(tr("The translation, in pixels, of the last copy; the copies between move by a share of it. Can also be adjusted by dragging the centre handle in the Viewer."));
    translateKnob->setSpatial(true);
    translateKnob->setDefaultValuesAreNormalized(true);
    translateKnob->disableSlider();
    for (int d = 0; d < 2; ++d) {
        translateKnob->setMinimum(-DBL_MAX, d);
        translateKnob->setMaximum(DBL_MAX, d);
        translateKnob->setDisplayMinimum(-10000., d);
        translateKnob->setDisplayMaximum(10000., d);
        translateKnob->setIncrement(10., d);
        translateKnob->setDefaultValue(0., d);
    }
    page->addKnob(translateKnob);
    _translate = translateKnob;

    KnobDoublePtr rotateKnob = AppManager::createKnob<KnobDouble>(this, tr("Rotate"));
    rotateKnob->setName(kGodRaysParamRotate);
    rotateKnob->setHintToolTip(tr("The rotation, in degrees around Center, of the last copy. Can also be adjusted by dragging the rotation bar in the Viewer."));
    rotateKnob->setMinimum(-DBL_MAX);
    rotateKnob->setMaximum(DBL_MAX);
    rotateKnob->setDisplayMinimum(-180.);
    rotateKnob->setDisplayMaximum(180.);
    rotateKnob->setIncrement(0.1);
    rotateKnob->setDefaultValue(0.);
    page->addKnob(rotateKnob);
    _rotate = rotateKnob;

    KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(this, tr("Scale"), 2);
    scaleKnob->setName(kGodRaysParamScale);
    scaleKnob->setHintToolTip(tr("The scale, about Center, of the last copy; the copies between are scaled geometrically. Above 1 the rays streak away from Center. Can also be adjusted by dragging the outer circle in the Viewer."));
    scaleKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        scaleKnob->setMinimum(-1.e4, d);
        scaleKnob->setMaximum(1.e4, d);
        scaleKnob->setDisplayMinimum(0.1, d);
        scaleKnob->setDisplayMaximum(10., d);
        scaleKnob->setIncrement(0.01, d);
        scaleKnob->setDefaultValue(1., d);
    }
    page->addKnob(scaleKnob);
    _scale = scaleKnob;

    // Only for the Viewer's transform handle, which needs it: Scale folds its dimensions instead.
    KnobBoolPtr uniformKnob = AppManager::createKnob<KnobBool>(this, tr("Uniform"));
    uniformKnob->setName(kGodRaysParamUniform);
    uniformKnob->setHintToolTip(tr("Use the X scale for both directions."));
    uniformKnob->setDefaultValue(false);
    uniformKnob->setSecretByDefault(true);
    uniformKnob->setAnimationEnabled(false);
    page->addKnob(uniformKnob);
    _uniform = uniformKnob;

    KnobDoublePtr skewXKnob = AppManager::createKnob<KnobDouble>(this, tr("Skew X"));
    skewXKnob->setName(kGodRaysParamSkewX);
    skewXKnob->setHintToolTip(tr("The skew along the x axis of the last copy. Can also be adjusted by dragging the skew bar in the Viewer."));
    skewXKnob->setMinimum(-DBL_MAX);
    skewXKnob->setMaximum(DBL_MAX);
    skewXKnob->setDisplayMinimum(-1.);
    skewXKnob->setDisplayMaximum(1.);
    skewXKnob->setIncrement(0.01);
    skewXKnob->setDefaultValue(0.);
    page->addKnob(skewXKnob);
    _skewX = skewXKnob;

    KnobDoublePtr skewYKnob = AppManager::createKnob<KnobDouble>(this, tr("Skew Y"));
    skewYKnob->setName(kGodRaysParamSkewY);
    skewYKnob->setHintToolTip(tr("The skew along the y axis of the last copy. Can also be adjusted by dragging the skew bar in the Viewer."));
    skewYKnob->setMinimum(-DBL_MAX);
    skewYKnob->setMaximum(DBL_MAX);
    skewYKnob->setDisplayMinimum(-1.);
    skewYKnob->setDisplayMaximum(1.);
    skewYKnob->setIncrement(0.01);
    skewYKnob->setDefaultValue(0.);
    page->addKnob(skewYKnob);
    _skewY = skewYKnob;

    // Hidden, there for the Viewer's transform handle; the render honours it too, so the rays
    // follow the skew the handle draws.
    KnobChoicePtr skewOrderKnob = AppManager::createKnob<KnobChoice>(this, tr("Skew Order"));
    skewOrderKnob->setName(kGodRaysParamSkewOrder);
    skewOrderKnob->setHintToolTip(tr("The order in which the skews apply: X then Y, or Y then X."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XY", "XY", ""));
        options.push_back(ChoiceOption("YX", "YX", ""));
        skewOrderKnob->populateChoices(options);
    }
    skewOrderKnob->setDefaultValue(0);
    skewOrderKnob->setSecretByDefault(true);
    skewOrderKnob->setAnimationEnabled(false);
    page->addKnob(skewOrderKnob);
    _skewOrder = skewOrderKnob;

    KnobDoublePtr centerKnob = AppManager::createKnob<KnobDouble>(this, tr("Center"), 2);
    centerKnob->setName(kGodRaysParamCenter);
    centerKnob->setHintToolTip(tr("The point the rays come from: the centre of Rotate, Scale and Skew. Can also be moved by dragging the transform handle in the Viewer."));
    centerKnob->setSpatial(true);
    centerKnob->setDefaultValuesAreNormalized(true);
    centerKnob->disableSlider();
    for (int d = 0; d < 2; ++d) {
        centerKnob->setMinimum(-DBL_MAX, d);
        centerKnob->setMaximum(DBL_MAX, d);
        centerKnob->setDisplayMinimum(-10000., d);
        centerKnob->setDisplayMaximum(10000., d);
        centerKnob->setIncrement(1., d);
        centerKnob->setDefaultValue(0.5, d);
    }
    page->addKnob(centerKnob);
    _center = centerKnob;

    KnobColorPtr fromColorKnob = AppManager::createKnob<KnobColor>(this, tr("From Color"), 4);
    fromColorKnob->setName(kGodRaysParamFromColor);
    fromColorKnob->setHintToolTip(tr("The tint of the untransformed copy, where the rays start."));
    for (int d = 0; d < 4; ++d) {
        fromColorKnob->setDefaultValue(1., d);
    }
    page->addKnob(fromColorKnob);
    _fromColor = fromColorKnob;

    KnobColorPtr toColorKnob = AppManager::createKnob<KnobColor>(this, tr("To Color"), 4);
    toColorKnob->setName(kGodRaysParamToColor);
    toColorKnob->setHintToolTip(tr("The tint of the fully transformed copy, where the rays end."));
    for (int d = 0; d < 4; ++d) {
        toColorKnob->setDefaultValue(1., d);
    }
    page->addKnob(toColorKnob);
    _toColor = toColorKnob;

    KnobDoublePtr gammaKnob = AppManager::createKnob<KnobDouble>(this, tr("Gamma"));
    gammaKnob->setName(kGodRaysParamGamma);
    gammaKnob->setHintToolTip(tr("How the tint goes from From Color to To Color along the rays: linearly at 1, reaching To Color sooner above 1 and later below 1. A copy a fraction t of the way along takes the tint at t^(1/Gamma)."));
    gammaKnob->setMinimum(0.01);
    gammaKnob->setMaximum(100.);
    gammaKnob->setDisplayMinimum(0.2);
    gammaKnob->setDisplayMaximum(5.);
    gammaKnob->setIncrement(0.01);
    gammaKnob->setDefaultValue(1.);
    page->addKnob(gammaKnob);
    _gamma = gammaKnob;

    KnobIntPtr samplesKnob = AppManager::createKnob<KnobInt>(this, tr("Samples"));
    samplesKnob->setName(kGodRaysParamSamples);
    samplesKnob->setHintToolTip(tr("How many copies of the image make the rays, from the untransformed one to the fully transformed one. More give smoother rays and cost proportionally more."));
    samplesKnob->setMinimum(1);
    samplesKnob->setMaximum(kMaxSamples);
    samplesKnob->setDisplayMinimum(1);
    samplesKnob->setDisplayMaximum(128);
    samplesKnob->setDefaultValue(32);
    page->addKnob(samplesKnob);
    _samples = samplesKnob;

    KnobBoolPtr maxKnob = AppManager::createKnob<KnobBool>(this, tr("Max"));
    maxKnob->setName(kGodRaysParamMax);
    maxKnob->setHintToolTip(tr("When checked each channel is the maximum of the tinted copies instead of their average, which keeps the rays as bright as the image. The maximum starts from 0."));
    maxKnob->setDefaultValue(false);
    page->addKnob(maxKnob);
    _max = maxKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(this, tr(kResamplerParamFilterTypeLabel));
    filterKnob->setName(kResamplerParamFilterType);
    filterKnob->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)Resampler::eFilterCubic);
    filterKnob->setAddNewLine(false);
    filterKnob->setSpacingBetweenItems(1);
    page->addKnob(filterKnob);
    _filter = filterKnob;

    KnobBoolPtr clampKnob = AppManager::createKnob<KnobBool>(this, tr(kResamplerParamFilterClampLabel));
    clampKnob->setName(kResamplerParamFilterClamp);
    clampKnob->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clampKnob->setDefaultValue(false);
    clampKnob->setAddNewLine(false);
    clampKnob->setSpacingBetweenItems(1);
    page->addKnob(clampKnob);
    _clamp = clampKnob;

    KnobBoolPtr blackOutsideKnob = AppManager::createKnob<KnobBool>(this, tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutsideKnob->setName(kResamplerParamFilterBlackOutside);
    blackOutsideKnob->setHintToolTip(tr("Fill the area outside the source image with black. Unchecked, the copies read the source's edge pixels beyond it, extended outwards."));
    blackOutsideKnob->setDefaultValue(true);
    page->addKnob(blackOutsideKnob);
    _blackOutside = blackOutsideKnob;

    _filterKnobs.create(this, page, filterKnob, clampKnob);

    addStageKnobs(page);
    _filterKnobs.updateVisibility(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));

    NodePtr node = getNode();
    if (node) {
        node->addTransformInteract(translateKnob, scaleKnob, uniformKnob, rotateKnob, skewXKnob, skewYKnob, skewOrderKnob, centerKnob, KnobBoolPtr());
    }
} // GodRays::initializeKnobs

GodRaysParams
GodRays::getParams(double time,
                   ViewIdx view) const
{
    const KnobReader reader(time, view);
    GodRaysParams params;

    TransformMath::TransformParams& tp = params.transform;
    tp.translateX = reader.value(_translate, 0);
    tp.translateY = reader.value(_translate, 1);
    tp.rotate = reader.value(_rotate);
    tp.scaleX = reader.value(_scale, 0);
    tp.scaleY = reader.value(_scale, 1);
    tp.scaleUniform = reader.value(_uniform);
    tp.skewX = reader.value(_skewX);
    tp.skewY = reader.value(_skewY);
    tp.skewOrderYX = (reader.value(_skewOrder) == 1);
    tp.centerX = reader.value(_center, 0);
    tp.centerY = reader.value(_center, 1);
    tp.amount = 1.;
    for (int c = 0; c < 4; ++c) {
        params.fromColor[c] = reader.value(_fromColor, c);
        params.toColor[c] = reader.value(_toColor, c);
    }
    const double gamma = reader.value(_gamma);
    params.gamma = (gamma > 0.) ? gamma : 1.;
    params.samples = std::min(kMaxSamples, std::max(1, (int)reader.value(_samples)));
    params.max = reader.value(_max);
    params.filter = _filterKnobs.read(reader);
    params.clamp = reader.value(_clamp);
    params.blackOutside = reader.value(_blackOutside);

    return params;
}

bool
GodRays::isIdentity(const GodRaysParams& params)
{
    if (params.max || !TransformMath::isIdentity(params.transform)) {
        return false;
    }
    for (int c = 0; c < 4; ++c) {
        if ((params.fromColor[c] != 1.) || (params.toColor[c] != 1.)) {
            return false;
        }
    }

    return true;
}

double
GodRays::tintAt(const GodRaysParams& params,
                int c,
                double a)
{
    const double t = (params.gamma == 1.) ? a : std::pow(std::max(0., a), 1. / params.gamma);

    return params.fromColor[c] + (params.toColor[c] - params.fromColor[c]) * t;
}

std::vector<double>
GodRays::samplePositions(const GodRaysParams& params)
{
    const int n = std::max(1, params.samples);
    std::vector<double> a(n, 0.);
    for (int i = 1; i < n; ++i) {
        a[i] = (double)i / (n - 1);
    }

    return a;
}

std::vector<DirBlurSample>
GodRays::canonicalSamples(const GodRaysParams& params)
{
    const std::vector<double> positions = samplePositions(params);
    const double weight = 1. / positions.size();
    const bool untransformed = TransformMath::isIdentity(params.transform);
    std::vector<DirBlurSample> samples(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const double a = positions[i];
        DirBlurSample& s = samples[i];
        // Destination to source: the output pixel reads the source where the copy came from.
        s.inverse = ((a == 0.) || untransformed) ? Mat3::identity() : TransformMath::inverseTransformCanonical(params.transform, a, false);
        s.weight = (float)weight;
        // Averaging, each copy counts its share; with Max, its whole tint.
        const double factor = params.max ? 1. : weight;
        for (int c = 0; c < 4; ++c) {
            s.factor[c] = (float)(factor * tintAt(params, c, a));
        }
    }

    return samples;
}

std::vector<DirBlurSample>
GodRays::pixelSamples(const GodRaysParams& params,
                      double time,
                      const RenderScale& scale,
                      double par)
{
    const std::vector<DirBlurSample> canonical = canonicalSamples(params);
    std::vector<DirBlurSample> pixel = DirBlur::pixelSamples(canonical, time, scale, par);
    // The change of coordinates can leave an ulp off the identity at a non-square PAR, which
    // would send the copy back through the filter.
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        if (canonical[i].inverse.isIdentity()) {
            pixel[i].inverse = Mat3::identity();
        }
    }

    return pixel;
}

bool
GodRays::knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     ViewSpec view,
                     double time,
                     bool originatedFromMainThread)
{
    KnobChoicePtr filterKnob = _filter.lock();
    if (filterKnob && (k == filterKnob.get())) {
        _filterKnobs.updateVisibility(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }

    return StagedImageEffect::knobChanged(k, reason, view, time, originatedFromMainThread);
}

void
GodRays::onKnobsLoaded()
{
    _filterKnobs.updateVisibility(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

double
GodRays::outputPar(double time,
                   ViewIdx view) const
{
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);

    return metadata ? metadata->getPixelAspectRatio(-1) : 1.;
}

RectD
GodRays::sourceRoD(double time,
                   ViewIdx view,
                   const RenderScale& scale) const
{
    EffectInstancePtr input = getInput(kGodRaysInputSource);
    if (!input) {
        return nullRect();
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD rod;
    bool isProjectFormat = false;
    if ((input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
        return nullRect();
    }

    return rod;
}

StatusEnum
GodRays::getRegionOfDefinition(U64 /*hash*/,
                               double time,
                               const RenderScale& scale,
                               ViewIdx view,
                               RectD* rod)
{
    const RectD srcRoD = sourceRoD(time, view, scale);
    if (srcRoD.isNull()) {
        rod->clear();

        return eStatusOK;
    }
    const GodRaysParams params = getParams(time, view);
    if (srcRoD.isInfinite() || TransformMath::isIdentity(params.transform)) {
        *rod = srcRoD;

        return eStatusOK;
    }

    RectD region = nullRect();
    for (const DirBlurSample& s : canonicalSamples(params)) {
        Mat3 forward;
        if (!s.inverse.inverse(&forward)) {
            continue;
        }
        RectD r;
        Resampler::transformRegionFromRect(srcRoD, forward, &r);
        mergeInto(r, &region);
    }
    if (!region.isNull()) {
        Resampler::expandRoD(outputPar(time, view), scale.toOfxPointD().x, scale.toOfxPointD().y, params.blackOutside, &region);
    }
    if (isMaskApplied() || (getMixValue(time, view) != 1.)) {
        mergeInto(srcRoD, &region);
    }
    *rod = region;

    return eStatusOK;
}

void
GodRays::getRegionsOfInterest(double time,
                              const RenderScale& scale,
                              const RectD& outputRoD,
                              const RectD& renderWindow,
                              ViewIdx view,
                              RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kGodRaysInputSource);
    if (!source) {
        return;
    }
    const GodRaysParams params = getParams(time, view);
    // The window itself too, which the mask and mix blend with.
    RectD roi = renderWindow;
    if (!TransformMath::isIdentity(params.transform)) {
        mergeInto(DirBlur::sourceRegion(canonicalSamples(params), renderWindow, scale, outputPar(time, view), params.filter, sourceRoD(time, view, scale), params.blackOutside), &roi);
    }
    (*ret)[source] = roi;
}

bool
GodRays::isOpIdentity(double time,
                      const RenderScale& /*scale*/,
                      const RectI& /*roi*/,
                      ViewIdx view)
{
    return isIdentity(getParams(time, view));
}

StatusEnum
GodRays::renderOp(const RenderActionArgs& args,
                  const StagePlan& plan)
{
    const GodRaysParams params = getParams(args.time, args.view);
    const std::vector<DirBlurSample> samples = pixelSamples(params, args.time, args.mappedScale, outputPar(args.time, args.view));
    const Resampler::FilterSpec filter = args.draftMode ? Resampler::FilterSpec(Resampler::eFilterImpulse) : params.filter;
    const bool clamp = params.clamp && Resampler::filterHonoursClamp(filter.filter);
    const bool takeMax = params.max;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (resolveInputPlaneForRender(kGodRaysInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            src = getImage(kGodRaysInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (src && !PassExec::isFloatImage(src)) {
            return eStatusFailed;
        }

        // The source in the output plane's layout, locked here; the band threads only read it.
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::vector<float> converted;
        Resampler::SourceImage source((const float*)0, RectI(), nComps);
        if (src) {
            srcBounds = src->getBounds();
            const int srcNComps = (int)src->getComponentsCount();
            srcAccess = std::make_shared<Image::ReadAccess>(src.get());
            if (srcBounds.isNull()) {
                src.reset();
            } else if (srcNComps == nComps) {
                source = Resampler::SourceImage((const float*)srcAccess->pixelAt(srcBounds.x1, srcBounds.y1), srcBounds, nComps);
            } else {
                const int width = srcBounds.width();
                converted.resize((std::size_t)width * srcBounds.height() * nComps);
                for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
                    PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, srcBounds.x1, y, width, nComps, &converted[(std::size_t)(y - srcBounds.y1) * width * nComps]);
                }
                source = Resampler::SourceImage(&converted[0], srcBounds, nComps);
            }
        }
        const bool sampled = (bool)src;

        const auto row = [&](const RowIO& io) {
            const std::size_t n = (std::size_t)io.width * io.nComps;
            std::vector<float> sum(n, 0.f);
            if (sampled) {
                std::vector<float> tmp(n);
                int bits[4];
                for (int c = 0; c < io.nComps; ++c) {
                    bits[c] = pixelKernelChannelBit(io.nComps, c);
                }
                // Each copy is one matrix resampled on its own, summed in copy order, so a pixel
                // does not depend on the window it is rendered in.
                for (const DirBlurSample& s : samples) {
                    Resampler::ResampleParams rp;
                    rp.invTransforms = &s.inverse;
                    rp.alphas = 0;
                    rp.count = 1;
                    rp.filter = s.inverse.isIdentity() ? Resampler::FilterSpec(Resampler::eFilterImpulse) : filter;
                    rp.clamp = clamp;
                    rp.blackOutside = params.blackOutside;
                    rp.motionBlur = 0.;
                    Resampler::resampleRow(rp, source, io.y, io.x0, io.x0 + io.width, &tmp[0]);
                    for (int i = 0; i < io.width; ++i) {
                        for (int c = 0; c < io.nComps; ++c) {
                            const std::size_t k = (std::size_t)i * io.nComps + c;
                            const float v = s.factor[bits[c]] * tmp[k];
                            sum[k] = takeMax ? std::max(sum[k], v) : (sum[k] + v);
                        }
                    }
                }
            }
            for (int c = 0; c < io.nComps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < io.width; ++i) {
                    io.dst[(std::size_t)i * io.nComps + c] = sum[(std::size_t)i * io.nComps + c];
                }
            }
        };

        EffectInstance::RenderActionArgs planeArgs = args;
        planeArgs.outputLayers.clear();
        planeArgs.outputLayers.push_back(*it);
        const StatusEnum status = runStagesCpu(*this, plan, planeArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }

    return eStatusOK;
} // GodRays::renderOp

bool
GodRays::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    return (query.bitDepth == eImageBitDepthFloat) && canRunStagesGpu(resolveGpuPlan(query), query);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
GodRays::getOpGpuInputLayer(int inputNb,
                            const ImageLayerDesc& plane,
                            double time,
                            ViewIdx view,
                            ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
GodRays::getOpGpuInputRoI(int inputNb,
                          double time,
                          ViewIdx view,
                          const RectI& outputRect,
                          const RenderScale& scale) const
{
    if (inputNb != kGodRaysInputSource) {
        return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
    }
    const GodRaysParams params = getParams(time, view);
    if (TransformMath::isIdentity(params.transform)) {
        return outputRect;
    }
    // The same canonical region the CPU asks for, in pixels.
    const double par = outputPar(time, view);
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    const RectD window(outputRect.x1 * par / sx, outputRect.y1 / sy, outputRect.x2 * par / sx, outputRect.y2 / sy);
    const RectD read = DirBlur::sourceRegion(canonicalSamples(params), window, scale, par, params.filter, sourceRoD(time, view, scale), params.blackOutside);
    RectI roi = outputRect;
    if (!read.isNull()) {
        roi.merge(read.toPixelEnclosing(scale, par));
    }

    return roi;
}

StatusEnum
GodRays::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOpOnGpu(query)) {
        return eStatusFailed;
    }
    const StagePlan plan = resolveGpuPlan(query);
    const GodRaysParams params = getParams(query.time, query.view);
    const std::vector<DirBlurSample> samples = pixelSamples(params, query.time, query.mappedScale, outputPar(query.time, query.view));
    const Resampler::FilterSpec filter = query.draftMode ? Resampler::FilterSpec(Resampler::eFilterImpulse) : params.filter;

    // The source cropped to its region of definition, as the CPU fetches it, is both sampled and
    // blended under the mask and mix.
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    const bool sourceSlot = (sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size());
    if (sourceSlot && !stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
        return eStatusFailed;
    }
    const bool hasSource = sourceSlot && cropped.inputs[sourceInput].buffer;

    // dirblur.slang's parameter block: filter, flags, count, lobes, B, C, then per
    // copy the matrix's two rows as (hi, lo) float pairs and the four factors.
    std::vector<uint32_t> words(kGodRaysGpuHeaderWords + samples.size() * kGodRaysGpuSampleWords, 0u);
    uint32_t flags = (hasSource ? kGodRaysGpuSourceValid : 0u) | (params.max ? kGodRaysGpuMax : 0u) | kGodRaysGpuIdentityNearest;
    if (params.clamp && Resampler::filterHonoursClamp(filter.filter)) {
        flags |= kGodRaysGpuClamp;
    }
    if (params.blackOutside) {
        flags |= kGodRaysGpuBlackOutside;
    }
    const float bicubicB = (float)filter.B;
    const float bicubicC = (float)filter.C;
    words[0] = (uint32_t)filter.filter;
    words[1] = flags;
    words[2] = (uint32_t)samples.size();
    words[3] = (uint32_t)filter.lobes;
    std::memcpy(&words[4], &bicubicB, sizeof(float));
    std::memcpy(&words[5], &bicubicC, sizeof(float));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        uint32_t* w = &words[kGodRaysGpuHeaderWords + i * kGodRaysGpuSampleWords];
        const Mat3& m = samples[i].inverse;
        const double entries[6] = { m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2) };
        for (int e = 0; e < 6; ++e) {
            const float hi = (float)entries[e];
            const float lo = (float)(entries[e] - (double)hi);
            std::memcpy(&w[2 * e], &hi, sizeof(float));
            std::memcpy(&w[2 * e + 1], &lo, sizeof(float));
        }
        std::memcpy(&w[12], samples[i].factor, 4 * sizeof(float));
    }

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::dirblur;
    op.params.resize(words.size() * sizeof(uint32_t));
    std::memcpy(op.params.data(), words.data(), op.params.size());

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>());
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
