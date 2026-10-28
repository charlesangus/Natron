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

#include "DirBlur.h"

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

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
#ifdef NATRON_HAVE_GPU
// dirblur.slang's parameter block layout.
constexpr std::size_t kDirBlurGpuHeaderWords = 12;
constexpr std::size_t kDirBlurGpuSampleWords = 20;
constexpr uint32_t kDirBlurGpuSourceValid = 1;
constexpr uint32_t kDirBlurGpuMax = 2;
constexpr uint32_t kDirBlurGpuHoldout = 4;
constexpr uint32_t kDirBlurGpuHoldoutValid = 8;
#endif

constexpr InputSpec kDirBlurInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "Holdout", .role = InputRole::Aux, .optional = true },
};

constexpr NodeSpec kDirBlurNodeSpec = {
    .id = PLUGINID_NATRON_DIRBLUR,
    .label = "DirBlur",
    .description = "Blur the image along a direction, around a centre or towards a centre.\n"
                   "\n"
                   "Directional smears each pixel Length pixels along Direction, or towards Target with Use Target, as a straight motion blur. Radial smears it around Center by an angle of Length degrees, as a spinning motion blur. Zoom smears it along the line through Center, so that the corners of the format move by Length pixels; a positive Length streaks towards Center, a negative one away from it.\n"
                   "\n"
                   "The transform (Translate, Rotate, Scale, Skew, about Center) is applied along the blur in every mode, as the God Rays node does: a copy at a fraction t of the blur is transformed by t times Transform Amount. With Length 0 it alone gives God Rays.\n"
                   "\n"
                   "The result is the weighted average of Samples copies of the image, each moved by an even step along the blur and filtered bilinearly, or their maximum with Max. Falloff fades the copies out towards the end of the blur, and From Color and To Color tint them, from the start of the blur to its end, which gives God-rays-like light streaks in Zoom mode. The weights are normalised, so with the default white tints the blur keeps the image's energy.\n"
                   "\n"
                   "Where the Holdout Channel of the Holdout input is 1 the source is left out of the average, and where it is between 0 and 1 it counts that much less.\n"
                   "\n"
                   "Beyond the source's region of definition the image reads black.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_DIRBLUR,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/DirBlur.png",
    .inputs = kDirBlurInputs,
};

constexpr ImagePipelineSpec kDirBlurPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Custom,
};

// The source pixels a sample of a matrix with linear part (a b; d e) reads around its centre, at
// most: the bilinear footprint and the supersampling of a minifying sample.
double
footprint(const Mat3& m,
          double par)
{
    const double spread = std::fabs(m(0, 0)) + std::fabs(m(0, 1)) + std::fabs(m(1, 0)) + std::fabs(m(1, 1));

    return 2. + std::ceil(spread * std::max(par, 1. / par));
}

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

DirBlur::DirBlur(NodePtr node)
    : StagedImageEffect(node)
{
}

DirBlur::~DirBlur()
{
}

const NodeSpec*
DirBlur::nodeSpec() const
{
    return staticNodeSpec<kDirBlurNodeSpec>();
}

const ImagePipelineSpec&
DirBlur::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kDirBlurPipeline, kDirBlurNodeSpec>();
}

std::string
DirBlur::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kDirBlurInputSource:
        return tr("The image to blur.").toStdString();
    case kDirBlurInputMask:
        return tr("The mask the blurred result is mixed with the source through.").toStdString();
    case kDirBlurInputHoldout:
        return tr("The image whose Holdout Channel keeps parts of the source out of the blur.").toStdString();
    }

    return std::string();
}

void
DirBlur::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChoicePtr modeKnob = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    modeKnob->setName(kDirBlurParamMode);
    modeKnob->setHintToolTip(tr("How the image is smeared."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDirBlurParamModeDirectional, tr("Directional").toStdString(), tr("Along Direction, by Length pixels: a straight motion blur. Center is not used.").toStdString()));
        options.push_back(ChoiceOption(kDirBlurParamModeRadial, tr("Radial").toStdString(), tr("Around Center, by an angle of Length degrees, counter-clockwise for a positive Length: a spinning motion blur.").toStdString()));
        options.push_back(ChoiceOption(kDirBlurParamModeZoom, tr("Zoom").toStdString(), tr("Along the line through Center, the corners of the format moving by Length pixels: towards Center for a positive Length, away from it for a negative one.").toStdString()));
        modeKnob->populateChoices(options);
    }
    modeKnob->setDefaultValue((int)eDirBlurModeZoom);
    page->addKnob(modeKnob);
    _mode = modeKnob;

    KnobDoublePtr lengthKnob = AppManager::createKnob<KnobDouble>(this, tr("Length"));
    lengthKnob->setName(kDirBlurParamLength);
    lengthKnob->setHintToolTip(tr("The extent of the blur: in pixels in Directional and Zoom modes, in degrees in Radial mode. With Centered it extends this far on each side. 0, with no transform and no Pixel Offset, leaves the input unchanged."));
    lengthKnob->setDisplayMinimum(-100.);
    lengthKnob->setDisplayMaximum(100.);
    lengthKnob->setDefaultValue(20.);
    page->addKnob(lengthKnob);
    _length = lengthKnob;

    KnobDoublePtr centerKnob = AppManager::createKnob<KnobDouble>(this, tr("Center"), 2);
    centerKnob->setName(kDirBlurParamCenter);
    centerKnob->setHintToolTip(tr("The centre of the Radial and Zoom blurs, of the transform, and the start of the Directional blur with Use Target. Can also be moved by dragging the transform handle in the Viewer."));
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

    KnobButtonPtr resetCenterKnob = AppManager::createKnob<KnobButton>(this, tr("Reset Center"));
    resetCenterKnob->setName(kDirBlurParamResetCenter);
    resetCenterKnob->setHintToolTip(tr("Move Center to the middle of the source's region of definition, changing Translate so that the transform at its full amount keeps its place."));
    page->addKnob(resetCenterKnob);
    _resetCenter = resetCenterKnob;

    KnobDoublePtr directionKnob = AppManager::createKnob<KnobDouble>(this, tr("Direction"));
    directionKnob->setName(kDirBlurParamDirection);
    directionKnob->setHintToolTip(tr("The direction of the Directional blur, in degrees counter-clockwise from the x axis, when Use Target is off."));
    directionKnob->setDisplayMinimum(-180.);
    directionKnob->setDisplayMaximum(180.);
    directionKnob->setDefaultValue(45.);
    page->addKnob(directionKnob);
    _direction = directionKnob;

    KnobBoolPtr useTargetKnob = AppManager::createKnob<KnobBool>(this, tr("Use Target"));
    useTargetKnob->setName(kDirBlurParamUseTarget);
    useTargetKnob->setHintToolTip(tr("When checked the Directional blur runs from Center towards Target instead of along Direction. Length still sets its extent."));
    useTargetKnob->setDefaultValue(false);
    page->addKnob(useTargetKnob);
    _useTarget = useTargetKnob;

    KnobDoublePtr targetKnob = AppManager::createKnob<KnobDouble>(this, tr("Target"), 2);
    targetKnob->setName(kDirBlurParamTarget);
    targetKnob->setHintToolTip(tr("With Use Target, the point the Directional blur runs towards from Center. Can also be moved by dragging its handle in the Viewer."));
    targetKnob->setSpatial(true);
    targetKnob->setDefaultValuesAreNormalized(true);
    targetKnob->disableSlider();
    for (int d = 0; d < 2; ++d) {
        targetKnob->setMinimum(-DBL_MAX, d);
        targetKnob->setMaximum(DBL_MAX, d);
        targetKnob->setDisplayMinimum(-10000., d);
        targetKnob->setDisplayMaximum(10000., d);
        targetKnob->setIncrement(1., d);
    }
    targetKnob->setDefaultValue(0.75, 0);
    targetKnob->setDefaultValue(0.5, 1);
    page->addKnob(targetKnob);
    targetKnob->setHasHostOverlayHandle(true);
    _target = targetKnob;

    KnobIntPtr samplesKnob = AppManager::createKnob<KnobInt>(this, tr("Samples"));
    samplesKnob->setName(kDirBlurParamSamples);
    samplesKnob->setHintToolTip(tr("How many copies of the image are averaged along the blur. More give a smoother blur and cost proportionally more; about one per pixel of blur avoids visible steps."));
    samplesKnob->setMinimum(1);
    samplesKnob->setMaximum(kMaxSamples);
    samplesKnob->setDisplayMinimum(1);
    samplesKnob->setDisplayMaximum(128);
    samplesKnob->setDefaultValue(32);
    page->addKnob(samplesKnob);
    _samples = samplesKnob;

    KnobBoolPtr centeredKnob = AppManager::createKnob<KnobBool>(this, tr("Centered"));
    centeredKnob->setName(kDirBlurParamCentered);
    centeredKnob->setHintToolTip(tr("When checked the blur extends Length both ways from each pixel instead of one way. The transform then runs the other way too, inverted."));
    centeredKnob->setDefaultValue(false);
    page->addKnob(centeredKnob);
    _centered = centeredKnob;

    KnobDoublePtr falloffKnob = AppManager::createKnob<KnobDouble>(this, tr("Falloff"));
    falloffKnob->setName(kDirBlurParamFalloff);
    falloffKnob->setHintToolTip(tr("How the copies fade towards the end of the blur: a copy at a fraction t of the blur weighs (1 - t)^Falloff. 0 weighs every copy the same."));
    falloffKnob->setMinimum(0.);
    falloffKnob->setDisplayMinimum(0.);
    falloffKnob->setDisplayMaximum(4.);
    falloffKnob->setDefaultValue(0.);
    page->addKnob(falloffKnob);
    _falloff = falloffKnob;

    KnobDoublePtr pixelOffsetKnob = AppManager::createKnob<KnobDouble>(this, tr("Pixel Offset"));
    pixelOffsetKnob->setName(kDirBlurParamPixelOffset);
    pixelOffsetKnob->setHintToolTip(tr("Zoom mode only: moves every copy this many pixels further along the blur at the corners of the format, offsetting the whole blur along its direction."));
    pixelOffsetKnob->setDisplayMinimum(-100.);
    pixelOffsetKnob->setDisplayMaximum(100.);
    pixelOffsetKnob->setDefaultValue(0.);
    page->addKnob(pixelOffsetKnob);
    _pixelOffset = pixelOffsetKnob;

    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(this, tr("Translate"), 2);
    translateKnob->setName(kDirBlurParamTranslate);
    translateKnob->setHintToolTip(tr("The translation, in pixels, of the copy at the end of the blur; the copies between move by a share of it. Can also be adjusted by dragging the centre handle in the Viewer."));
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
    rotateKnob->setName(kDirBlurParamRotate);
    rotateKnob->setHintToolTip(tr("The rotation, in degrees around Center, of the copy at the end of the blur. Can also be adjusted by dragging the rotation bar in the Viewer."));
    rotateKnob->setMinimum(-DBL_MAX);
    rotateKnob->setMaximum(DBL_MAX);
    rotateKnob->setDisplayMinimum(-180.);
    rotateKnob->setDisplayMaximum(180.);
    rotateKnob->setIncrement(0.1);
    rotateKnob->setDefaultValue(0.);
    page->addKnob(rotateKnob);
    _rotate = rotateKnob;

    KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(this, tr("Scale"), 2);
    scaleKnob->setName(kDirBlurParamScale);
    scaleKnob->setHintToolTip(tr("The scale, about Center, of the copy at the end of the blur; the copies between are scaled geometrically. Can also be adjusted by dragging the outer circle in the Viewer."));
    scaleKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        scaleKnob->setMinimum(-1.e4, d);
        scaleKnob->setMaximum(1.e4, d);
        scaleKnob->setDisplayMinimum(0.1, d);
        scaleKnob->setDisplayMaximum(10., d);
        scaleKnob->setIncrement(0.01, d);
        scaleKnob->setDefaultValue(1., d);
    }
    scaleKnob->setAddNewLine(false);
    scaleKnob->setSpacingBetweenItems(1);
    page->addKnob(scaleKnob);
    _scale = scaleKnob;

    KnobBoolPtr uniformKnob = AppManager::createKnob<KnobBool>(this, tr("Uniform"));
    uniformKnob->setName(kDirBlurParamUniform);
    uniformKnob->setHintToolTip(tr("Use the X scale for both directions."));
    uniformKnob->setDefaultValue(false);
    page->addKnob(uniformKnob);
    _uniform = uniformKnob;

    KnobDoublePtr skewXKnob = AppManager::createKnob<KnobDouble>(this, tr("Skew X"));
    skewXKnob->setName(kDirBlurParamSkewX);
    skewXKnob->setHintToolTip(tr("The skew along the x axis of the copy at the end of the blur. Can also be adjusted by dragging the skew bar in the Viewer."));
    skewXKnob->setMinimum(-DBL_MAX);
    skewXKnob->setMaximum(DBL_MAX);
    skewXKnob->setDisplayMinimum(-1.);
    skewXKnob->setDisplayMaximum(1.);
    skewXKnob->setIncrement(0.01);
    skewXKnob->setDefaultValue(0.);
    page->addKnob(skewXKnob);
    _skewX = skewXKnob;

    KnobDoublePtr skewYKnob = AppManager::createKnob<KnobDouble>(this, tr("Skew Y"));
    skewYKnob->setName(kDirBlurParamSkewY);
    skewYKnob->setHintToolTip(tr("The skew along the y axis of the copy at the end of the blur."));
    skewYKnob->setMinimum(-DBL_MAX);
    skewYKnob->setMaximum(DBL_MAX);
    skewYKnob->setDisplayMinimum(-1.);
    skewYKnob->setDisplayMaximum(1.);
    skewYKnob->setIncrement(0.01);
    skewYKnob->setDefaultValue(0.);
    page->addKnob(skewYKnob);
    _skewY = skewYKnob;

    KnobChoicePtr skewOrderKnob = AppManager::createKnob<KnobChoice>(this, tr("Skew Order"));
    skewOrderKnob->setName(kDirBlurParamSkewOrder);
    skewOrderKnob->setHintToolTip(tr("The order in which the skews apply: X then Y, or Y then X."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XY", "XY", ""));
        options.push_back(ChoiceOption("YX", "YX", ""));
        skewOrderKnob->populateChoices(options);
    }
    skewOrderKnob->setDefaultValue(0);
    page->addKnob(skewOrderKnob);
    _skewOrder = skewOrderKnob;

    KnobDoublePtr transformAmountKnob = AppManager::createKnob<KnobDouble>(this, tr("Transform Amount"));
    transformAmountKnob->setName(kDirBlurParamTransformAmount);
    transformAmountKnob->setHintToolTip(tr("How much of the transform the copy at the end of the blur takes: 0 leaves the copies untransformed, 1 applies the full transform."));
    transformAmountKnob->setMinimum(-DBL_MAX);
    transformAmountKnob->setMaximum(DBL_MAX);
    transformAmountKnob->setDisplayMinimum(0.);
    transformAmountKnob->setDisplayMaximum(1.);
    transformAmountKnob->setIncrement(0.01);
    transformAmountKnob->setDefaultValue(1.);
    page->addKnob(transformAmountKnob);
    _transformAmount = transformAmountKnob;

    KnobBoolPtr invertKnob = AppManager::createKnob<KnobBool>(this, tr("Invert"));
    invertKnob->setName(kDirBlurParamInvert);
    invertKnob->setHintToolTip(tr("Invert the transform."));
    invertKnob->setDefaultValue(false);
    page->addKnob(invertKnob);
    _invert = invertKnob;

    KnobBoolPtr interactiveKnob = AppManager::createKnob<KnobBool>(this, tr("Interactive"));
    interactiveKnob->setName(kDirBlurParamInteractive);
    interactiveKnob->setHintToolTip(tr("If checked, update the parameter values while dragging the transform handles in the Viewer, else when the pen is released."));
    interactiveKnob->setDefaultValue(true);
    interactiveKnob->setEvaluateOnChange(false);
    page->addKnob(interactiveKnob);
    _interactive = interactiveKnob;

    KnobColorPtr fromColorKnob = AppManager::createKnob<KnobColor>(this, tr("From Color"), 4);
    fromColorKnob->setName(kDirBlurParamFromColor);
    fromColorKnob->setHintToolTip(tr("The tint of the copies at the start of the blur. The tint goes to To Color at its end, as Ramp says."));
    for (int d = 0; d < 4; ++d) {
        fromColorKnob->setDefaultValue(1., d);
    }
    page->addKnob(fromColorKnob);
    _fromColor = fromColorKnob;

    KnobColorPtr toColorKnob = AppManager::createKnob<KnobColor>(this, tr("To Color"), 4);
    toColorKnob->setName(kDirBlurParamToColor);
    toColorKnob->setHintToolTip(tr("The tint of the copies at the end of the blur."));
    for (int d = 0; d < 4; ++d) {
        toColorKnob->setDefaultValue(1., d);
    }
    page->addKnob(toColorKnob);
    _toColor = toColorKnob;

    KnobChoicePtr rampKnob = AppManager::createKnob<KnobChoice>(this, tr("Ramp"));
    rampKnob->setName(kDirBlurParamRamp);
    rampKnob->setHintToolTip(tr("How the tint goes from From Color to To Color along the blur."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDirBlurParamRampLinear, tr("Linear").toStdString(), tr("In even steps.").toStdString()));
        options.push_back(ChoiceOption(kDirBlurParamRampExponential, tr("Exponential").toStdString(), tr("By an even factor per step, as the God Rays node ramps; each colour is held to at least 0.001.").toStdString()));
        rampKnob->populateChoices(options);
    }
    rampKnob->setDefaultValue((int)eDirBlurRampLinear);
    page->addKnob(rampKnob);
    _ramp = rampKnob;

    KnobBoolPtr maxKnob = AppManager::createKnob<KnobBool>(this, tr("Max"));
    maxKnob->setName(kDirBlurParamMax);
    maxKnob->setHintToolTip(tr("When checked each channel is the maximum of the tinted copies, each weighed by Falloff but not normalised, instead of their average. The maximum starts from 0."));
    maxKnob->setDefaultValue(false);
    page->addKnob(maxKnob);
    _max = maxKnob;

    KnobChannelSelectPtr holdoutChannelKnob = AppManager::createKnob<KnobChannelSelect>(this, tr("Holdout Channel"));
    holdoutChannelKnob->setName(kDirBlurParamHoldoutChannel);
    holdoutChannelKnob->setAnimationEnabled(false);
    holdoutChannelKnob->setDefaultValue(holdoutChannelKnob->encode(std::string(kNatronColorViewRGBA) + ".A"));
    holdoutChannelKnob->setHintToolTip(tr("The channel of the Holdout input, in any of its layers, read where each copy comes from: the source counts (1 - holdout) as much there, and not at all where it is 1. Unused without a Holdout input; None turns it off."));
    page->addKnob(holdoutChannelKnob);
    _holdoutChannel = holdoutChannelKnob;

    addStageKnobs(page);

    NodePtr node = getNode();
    if (node) {
        node->addTransformInteract(translateKnob, scaleKnob, uniformKnob, rotateKnob, skewXKnob, skewYKnob, skewOrderKnob, centerKnob, invertKnob, interactiveKnob);
        node->declareLayerKnob(holdoutChannelKnob, kDirBlurInputHoldout, LayerKnobSpec::eRoleInputBound);
    }
} // DirBlur::initializeKnobs

DirBlurParams
DirBlur::getParams(double time,
                   ViewIdx view) const
{
    const KnobReader reader(time, view);
    DirBlurParams params;

    const int mode = reader.value(_mode);
    params.mode = (mode == (int)eDirBlurModeDirectional) ? eDirBlurModeDirectional : (mode == (int)eDirBlurModeRadial) ? eDirBlurModeRadial
                                                                                                                       : eDirBlurModeZoom;
    params.length = reader.value(_length);
    params.centerX = reader.value(_center, 0);
    params.centerY = reader.value(_center, 1);
    params.direction = reader.value(_direction);
    params.useTarget = reader.value(_useTarget);
    params.targetX = reader.value(_target, 0);
    params.targetY = reader.value(_target, 1);
    params.samples = std::min(kMaxSamples, std::max(1, (int)reader.value(_samples)));
    params.centered = reader.value(_centered);
    params.falloff = std::max(0., (double)reader.value(_falloff));
    params.pixelOffset = reader.value(_pixelOffset);

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
    tp.centerX = params.centerX;
    tp.centerY = params.centerY;
    tp.amount = reader.value(_transformAmount);
    params.invert = reader.value(_invert);

    for (int c = 0; c < 4; ++c) {
        params.fromColor[c] = reader.value(_fromColor, c);
        params.toColor[c] = reader.value(_toColor, c);
    }
    params.ramp = (reader.value(_ramp) == (int)eDirBlurRampExponential) ? eDirBlurRampExponential : eDirBlurRampLinear;
    params.max = reader.value(_max);

    return params;
}

bool
DirBlur::hasMotion(const DirBlurParams& params)
{
    if (params.length != 0.) {
        return true;
    }
    if ((params.mode == eDirBlurModeZoom) && (params.pixelOffset != 0.)) {
        return true;
    }

    return !TransformMath::isIdentity(params.transform);
}

double
DirBlur::tintAt(const DirBlurParams& params,
                int c,
                double a)
{
    if (params.ramp == eDirBlurRampExponential) {
        const double from = std::max(kMinExponentialTint, params.fromColor[c]);
        const double to = std::max(kMinExponentialTint, params.toColor[c]);

        return from * std::pow(to / from, a);
    }

    return params.fromColor[c] + (params.toColor[c] - params.fromColor[c]) * a;
}

bool
DirBlur::resolveHoldoutChannel(double time,
                               ViewIdx view,
                               ImageLayerDesc* layer,
                               int* channel) const
{
    *channel = -1;
    KnobChannelSelectPtr knob = _holdoutChannel.lock();
    if (!knob || knob->isNone() || !getInput(kDirBlurInputHoldout)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<DirBlur*>(this)->getPresentLayers(time, view, kDirBlurInputHoldout, &present);
    if (!knob->resolve(present, layer, channel) || (*channel < 0)) {
        *channel = -1;

        return false;
    }
    const int nComps = layer->getNumComponents();
    if ((nComps < 1) || (nComps > 4) || (*channel >= nComps)) {
        *channel = -1;

        return false;
    }

    return true;
}

bool
DirBlur::getInputLayersRead(int inputNb,
                            double time,
                            ViewIdx view,
                            std::list<ImageLayerDesc>* layers)
{
    if (inputNb != kDirBlurInputHoldout) {
        return false;
    }
    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveHoldoutChannel(time, view, &layer, &channel)) {
        return false;
    }
    layers->push_back(layer);

    return true;
}

bool
DirBlur::knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     ViewSpec view,
                     double time,
                     bool originatedFromMainThread)
{
    KnobButtonPtr resetCenterKnob = _resetCenter.lock();
    if (resetCenterKnob && (k == resetCenterKnob.get())) {
        resetCenter(time);

        return true;
    }

    return StagedImageEffect::knobChanged(k, reason, view, time, originatedFromMainThread);
}

void
DirBlur::resetCenter(double time)
{
    KnobDoublePtr center = _center.lock();
    KnobDoublePtr translate = _translate.lock();
    if (!center || !translate) {
        return;
    }
    const RectD rod = sourceRoD(time, ViewIdx(0), RenderScale::identity);
    if (rod.isNull() || rod.isInfinite()) {
        return;
    }
    const TransformMath::TransformParams p = getParams(time, ViewIdx(0)).transform;
    const double newCenterX = (rod.x1 + rod.x2) / 2;
    const double newCenterY = (rod.y1 + rod.y2) / 2;
    if ((newCenterX == p.centerX) && (newCenterY == p.centerY)) {
        return;
    }
    double sx, sy;
    TransformMath::effectiveScale(p.scaleX, p.scaleY, p.scaleUniform, &sx, &sy);
    const Mat3 rInv = TransformMath::rotation(-TransformMath::toRadians(p.rotate)) * TransformMath::skewXY(p.skewX, p.skewY, p.skewOrderYX) * TransformMath::scale(sx, sy);
    const double dx = newCenterX - p.centerX;
    const double dy = newCenterY - p.centerY;
    TransformMath::Point3 d = rInv * TransformMath::Point3(dx, dy, 1.);
    if (d.z != 0) {
        d.x /= d.z;
        d.y /= d.z;
    }

    beginChanges();
    center->setValues(newCenterX, newCenterY, ViewSpec::all(), eValueChangedReasonPluginEdited);
    translate->setValues(p.translateX + d.x - dx, p.translateY + d.y - dy, ViewSpec::all(), eValueChangedReasonPluginEdited);
    endChanges();
}

double
DirBlur::outputPar(double time,
                   ViewIdx view) const
{
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);

    return metadata ? metadata->getPixelAspectRatio(-1) : 1.;
}

double
DirBlur::zoomReference(double time,
                       ViewIdx view) const
{
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    if (!metadata) {
        return 0.;
    }
    const RectI format = metadata->getOutputFormat();
    if (format.isNull()) {
        return 0.;
    }
    const double par = metadata->getPixelAspectRatio(-1);

    return 0.5 * std::hypot(format.width() * par, (double)format.height());
}

RectD
DirBlur::sourceRoD(double time,
                   ViewIdx view,
                   const RenderScale& scale) const
{
    EffectInstancePtr input = getInput(kDirBlurInputSource);
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

std::vector<double>
DirBlur::samplePositions(const DirBlurParams& params)
{
    const int n = std::max(1, params.samples);
    std::vector<double> t(n);
    for (int i = 0; i < n; ++i) {
        t[i] = params.centered ? (-1. + (2. * i + 1.) / n) : ((i + 0.5) / n);
    }

    return t;
}

std::vector<DirBlurSample>
DirBlur::canonicalSamples(const DirBlurParams& params,
                          double zoomReference)
{
    const std::vector<double> positions = samplePositions(params);
    std::vector<double> weights(positions.size());
    double sum = 0.;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        weights[i] = (params.falloff > 0.) ? std::pow(1. - std::fabs(positions[i]), params.falloff) : 1.;
        sum += weights[i];
    }

    const Mat3 toCenter = TransformMath::translation(params.centerX, params.centerY);
    const Mat3 fromCenter = TransformMath::translation(-params.centerX, -params.centerY);
    double direction = TransformMath::toRadians(params.direction);
    if (params.useTarget && (params.mode == eDirBlurModeDirectional)) {
        const double dx = params.targetX - params.centerX;
        const double dy = params.targetY - params.centerY;
        if ((dx != 0.) || (dy != 0.)) {
            direction = std::atan2(dy, dx);
        }
    }
    const bool transformed = !TransformMath::isIdentity(params.transform);
    std::vector<DirBlurSample> samples(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const double t = positions[i];
        DirBlurSample& s = samples[i];
        // Destination to source: the output pixel reads the source where the sample came from.
        switch (params.mode) {
        case eDirBlurModeDirectional: {
            const double offset = t * params.length;
            s.inverse = TransformMath::translation(-offset * std::cos(direction), -offset * std::sin(direction));
            break;
        }
        case eDirBlurModeRadial:
            s.inverse = toCenter * TransformMath::rotation(TransformMath::toRadians(t * params.length)) * fromCenter;
            break;
        case eDirBlurModeZoom: {
            const double k = (zoomReference > 0.) ? std::max(kMinZoomScale, 1. + (t * params.length + params.pixelOffset) / zoomReference) : 1.;
            s.inverse = toCenter * TransformMath::scale(k, k) * fromCenter;
            break;
        }
        }
        if (transformed) {
            // A copy before the start of a centred blur takes the inverse of the transform at |t|.
            const Mat3 along = (t >= 0.) ? TransformMath::inverseTransformCanonical(params.transform, t, params.invert)
                                         : TransformMath::inverseTransformCanonical(params.transform, -t, !params.invert);
            s.inverse = s.inverse * along;
        }
        const double a = std::fabs(t);
        const double weight = params.max ? weights[i] : (weights[i] / sum);
        s.weight = (float)(weights[i] / sum);
        for (int c = 0; c < 4; ++c) {
            s.factor[c] = (float)(weight * tintAt(params, c, a));
        }
    }

    return samples;
}

std::vector<DirBlurSample>
DirBlur::pixelSamples(const std::vector<DirBlurSample>& canonical,
                      double time,
                      const RenderScale& scale,
                      double par)
{
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    std::vector<DirBlurSample> pixel(canonical.size());
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        const Mat3& m = canonical[i].inverse;
        const Resampler::CanonicalTransformFn fn = [&m](double, double, bool, Mat3* out) {
            *out = m;

            return true;
        };
        Resampler::SamplingTransforms transforms;
        Resampler::buildSamplingTransforms(fn, time, false, Resampler::BlurSettings(), sx, sy, false, par, par, &transforms);
        pixel[i] = canonical[i];
        pixel[i].inverse = transforms.invTransforms.front();
    }

    return pixel;
}

RectD
DirBlur::sourceRegion(const std::vector<DirBlurSample>& canonical,
                      const RectD& window,
                      const RenderScale& scale,
                      double par,
                      const RectD& srcRoD)
{
    RectD region = nullRect();
    if (window.isNull()) {
        return region;
    }
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    for (const DirBlurSample& s : canonical) {
        RectD r;
        Resampler::transformRegionFromRect(window, s.inverse, &r);
        if (r.isNull()) {
            continue;
        }
        const double grow = footprint(s.inverse, par);
        r.x1 -= grow * par / sx;
        r.x2 += grow * par / sx;
        r.y1 -= grow / sy;
        r.y2 += grow / sy;
        mergeInto(r, &region);
    }
    if (!srcRoD.isNull() && !region.isNull()) {
        if (!region.intersects(srcRoD)) {
            return nullRect();
        }
        region = region.intersect(srcRoD);
    }

    return region;
}

StatusEnum
DirBlur::getRegionOfDefinition(U64 /*hash*/,
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
    const DirBlurParams params = getParams(time, view);
    if (srcRoD.isInfinite() || !hasMotion(params)) {
        *rod = srcRoD;

        return eStatusOK;
    }

    RectD region = nullRect();
    for (const DirBlurSample& s : canonicalSamples(params, zoomReference(time, view))) {
        Mat3 forward;
        if (!s.inverse.inverse(&forward)) {
            continue;
        }
        RectD r;
        Resampler::transformRegionFromRect(srcRoD, forward, &r);
        mergeInto(r, &region);
    }
    if (!region.isNull()) {
        Resampler::expandRoD(outputPar(time, view), scale.toOfxPointD().x, scale.toOfxPointD().y, true, &region);
    }
    if (isMaskApplied() || (getMixValue(time, view) != 1.)) {
        mergeInto(srcRoD, &region);
    }
    *rod = region;

    return eStatusOK;
}

void
DirBlur::getRegionsOfInterest(double time,
                              const RenderScale& scale,
                              const RectD& outputRoD,
                              const RectD& renderWindow,
                              ViewIdx view,
                              RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kDirBlurInputSource);
    EffectInstancePtr holdout = getInput(kDirBlurInputHoldout);
    if (!source && !holdout) {
        return;
    }
    const DirBlurParams params = getParams(time, view);
    const bool moves = hasMotion(params);
    const double par = outputPar(time, view);
    std::vector<DirBlurSample> canonical;
    if (moves) {
        canonical = canonicalSamples(params, zoomReference(time, view));
    }
    if (source) {
        // The window itself too, which the mask and mix blend with.
        RectD roi = renderWindow;
        if (moves) {
            mergeInto(sourceRegion(canonical, renderWindow, scale, par, sourceRoD(time, view, scale)), &roi);
        }
        (*ret)[source] = roi;
    }
    if (holdout) {
        // The holdout is read where the source is, whatever its own region of definition.
        RectD roi = renderWindow;
        if (moves) {
            mergeInto(sourceRegion(canonical, renderWindow, scale, par, nullRect()), &roi);
        }
        RoIMap::iterator found = ret->find(holdout);
        if (found != ret->end()) {
            found->second.merge(roi);
        } else {
            ret->insert(std::make_pair(holdout, roi));
        }
    }
}

bool
DirBlur::isOpIdentity(double time,
                      const RenderScale& /*scale*/,
                      const RectI& /*roi*/,
                      ViewIdx view)
{
    // As God Rays: no motion leaves the source unchanged, whatever the tints, Max or holdout.
    return !hasMotion(getParams(time, view));
}

StatusEnum
DirBlur::renderOp(const RenderActionArgs& args,
                  const StagePlan& plan)
{
    const DirBlurParams params = getParams(args.time, args.view);
    const std::vector<DirBlurSample> samples = pixelSamples(canonicalSamples(params, zoomReference(args.time, args.view)), args.time, args.mappedScale, outputPar(args.time, args.view));
    const Resampler::FilterEnum filter = args.draftMode ? Resampler::eFilterImpulse : Resampler::eFilterBilinear;

    // The Holdout Channel as one float per pixel, locked here; the band threads only read it.
    ImageLayerDesc holdoutLayer;
    int holdoutChannel = -1;
    const bool holdoutOn = resolveHoldoutChannel(args.time, args.view, &holdoutLayer, &holdoutChannel);
    std::vector<float> holdoutPixels;
    Resampler::SourceImage holdoutSource((const float*)0, RectI(), 1);
    if (holdoutOn) {
        RectI holdoutRoI;
        const ImagePtr image = getImage(kDirBlurInputHoldout, args.time, args.mappedScale, args.view, NULL, &holdoutLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &holdoutRoI);
        if (image && !PassExec::isFloatImage(image)) {
            return eStatusFailed;
        }
        const RectI bounds = image ? image->getBounds() : RectI();
        const int imageNComps = image ? (int)image->getComponentsCount() : 0;
        if (image && !bounds.isNull() && (holdoutChannel < imageNComps)) {
            const Image::ReadAccess access(image.get());
            const int width = bounds.width();
            holdoutPixels.resize((std::size_t)width * bounds.height());
            for (int y = bounds.y1; y < bounds.y2; ++y) {
                PassExec::readChannelRow(image.get(), &access, bounds, imageNComps, holdoutChannel, bounds.x1, y, width, 0.f, &holdoutPixels[(std::size_t)(y - bounds.y1) * width]);
            }
            holdoutSource = Resampler::SourceImage(&holdoutPixels[0], bounds, 1);
        }
    }
    const bool holdoutSampled = holdoutSource.isValid();
    const bool takeMax = params.max;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (resolveInputPlaneForRender(kDirBlurInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            src = getImage(kDirBlurInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
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
                std::vector<float> hold(holdoutOn ? io.width : 0, 0.f);
                std::vector<float> weightSum((holdoutOn && !takeMax) ? io.width : 0, 0.f);
                int bits[4];
                for (int c = 0; c < io.nComps; ++c) {
                    bits[c] = pixelKernelChannelBit(io.nComps, c);
                }
                // Each sample is one matrix resampled on its own, summed in sample order, so a
                // pixel does not depend on the window it is rendered in.
                for (const DirBlurSample& s : samples) {
                    Resampler::ResampleParams rp;
                    rp.invTransforms = &s.inverse;
                    rp.alphas = 0;
                    rp.count = 1;
                    rp.filter = filter;
                    rp.clamp = false;
                    rp.blackOutside = true;
                    rp.motionBlur = 0.;
                    Resampler::resampleRow(rp, source, io.y, io.x0, io.x0 + io.width, &tmp[0]);
                    if (holdoutSampled) {
                        Resampler::resampleRow(rp, holdoutSource, io.y, io.x0, io.x0 + io.width, &hold[0]);
                    }
                    for (int i = 0; i < io.width; ++i) {
                        const float keep = holdoutOn ? (1.f - std::min(1.f, std::max(0.f, hold[i]))) : 1.f;
                        if (takeMax) {
                            if (keep <= 0.f) {
                                continue;
                            }
                            for (int c = 0; c < io.nComps; ++c) {
                                const std::size_t k = (std::size_t)i * io.nComps + c;
                                sum[k] = std::max(sum[k], keep * s.factor[bits[c]] * tmp[k]);
                            }
                        } else {
                            for (int c = 0; c < io.nComps; ++c) {
                                const std::size_t k = (std::size_t)i * io.nComps + c;
                                sum[k] += keep * s.factor[bits[c]] * tmp[k];
                            }
                            if (holdoutOn) {
                                weightSum[i] += keep * s.weight;
                            }
                        }
                    }
                }
                // Held-out copies leave the average: the rest are renormalised to their weights.
                if (holdoutOn && !takeMax) {
                    for (int i = 0; i < io.width; ++i) {
                        const float w = weightSum[i];
                        for (int c = 0; c < io.nComps; ++c) {
                            float& v = sum[(std::size_t)i * io.nComps + c];
                            v = (w > 0.f) ? (v / w) : 0.f;
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
} // DirBlur::renderOp

bool
DirBlur::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    return (query.bitDepth == eImageBitDepthFloat) && canRunStagesGpu(resolveGpuPlan(query), query);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
DirBlur::getOpGpuInputLayer(int inputNb,
                            const ImageLayerDesc& plane,
                            double time,
                            ViewIdx view,
                            ImageLayerDesc* layer) const
{
    if (inputNb == kDirBlurInputHoldout) {
        int channel = -1;
        if (resolveHoldoutChannel(time, view, layer, &channel)) {
            return true;
        }
    }
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

bool
DirBlur::readsGpuInput(int inputNb,
                       const ImageLayerDesc& /*plane*/,
                       double time,
                       ViewIdx view) const
{
    if (inputNb != kDirBlurInputHoldout) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return resolveHoldoutChannel(time, view, &layer, &channel);
}

RectI
DirBlur::getOpGpuInputRoI(int inputNb,
                          double time,
                          ViewIdx view,
                          const RectI& outputRect,
                          const RenderScale& scale) const
{
    if ((inputNb != kDirBlurInputSource) && (inputNb != kDirBlurInputHoldout)) {
        return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
    }
    const DirBlurParams params = getParams(time, view);
    if (!hasMotion(params)) {
        return outputRect;
    }
    // The same canonical region the CPU asks for, in pixels.
    const double par = outputPar(time, view);
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    const RectD window(outputRect.x1 * par / sx, outputRect.y1 / sy, outputRect.x2 * par / sx, outputRect.y2 / sy);
    const RectD clip = (inputNb == kDirBlurInputSource) ? sourceRoD(time, view, scale) : nullRect();
    const RectD read = sourceRegion(canonicalSamples(params, zoomReference(time, view)), window, scale, par, clip);
    RectI roi = outputRect;
    if (!read.isNull()) {
        roi.merge(read.toPixelEnclosing(scale, par));
    }

    return roi;
}

StatusEnum
DirBlur::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOpOnGpu(query)) {
        return eStatusFailed;
    }
    const StagePlan plan = resolveGpuPlan(query);
    const DirBlurParams params = getParams(query.time, query.view);
    const std::vector<DirBlurSample> samples = pixelSamples(canonicalSamples(params, zoomReference(query.time, query.view)), query.time, query.mappedScale, outputPar(query.time, query.view));
    const Resampler::FilterEnum filter = query.draftMode ? Resampler::eFilterImpulse : Resampler::eFilterBilinear;

    // The source cropped to its region of definition, as the CPU fetches it, is both sampled and
    // blended under the mask and mix.
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    const bool sourceSlot = (sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size());
    if (sourceSlot && !stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
        return eStatusFailed;
    }
    const bool hasSource = sourceSlot && cropped.inputs[sourceInput].buffer;

    GpuImageView holdoutView;
    if (!stageGpuInputView(*this, args, kDirBlurInputHoldout, &holdoutView)) {
        return eStatusFailed;
    }
    ImageLayerDesc holdoutLayer;
    int holdoutChannel = -1;
    const bool holdoutOn = resolveHoldoutChannel(query.time, query.view, &holdoutLayer, &holdoutChannel);

    // dirblur.slang's parameter block: filter, flags, count, the Holdout view (offset, stride,
    // bounds, components and the component read), then per sample the matrix's two rows as
    // (hi, lo) float pairs, the four factors and the weight.
    std::vector<uint32_t> words(kDirBlurGpuHeaderWords + samples.size() * kDirBlurGpuSampleWords, 0u);
    uint32_t flags = (hasSource ? kDirBlurGpuSourceValid : 0u) | (params.max ? kDirBlurGpuMax : 0u);
    if (holdoutOn) {
        flags |= kDirBlurGpuHoldout;
        const int component = holdoutView.buffer ? stageGpuMaskViewComponent(holdoutLayer, holdoutChannel, holdoutView.nComps) : -1;
        if ((component >= 0) && (component < holdoutView.nComps) && !holdoutView.bounds.isNull()) {
            flags |= kDirBlurGpuHoldoutValid;
            words[3] = (uint32_t)(holdoutView.byteOffset / sizeof(float));
            words[4] = (uint32_t)holdoutView.rowStrideFloats;
            words[5] = (uint32_t)holdoutView.bounds.x1;
            words[6] = (uint32_t)holdoutView.bounds.y1;
            words[7] = (uint32_t)holdoutView.bounds.x2;
            words[8] = (uint32_t)holdoutView.bounds.y2;
            words[9] = (uint32_t)holdoutView.nComps;
            words[10] = (uint32_t)component;
        }
    }
    words[0] = (uint32_t)filter;
    words[1] = flags;
    words[2] = (uint32_t)samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        uint32_t* w = &words[kDirBlurGpuHeaderWords + i * kDirBlurGpuSampleWords];
        const Mat3& m = samples[i].inverse;
        const double entries[6] = { m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2) };
        for (int e = 0; e < 6; ++e) {
            const float hi = (float)entries[e];
            const float lo = (float)(entries[e] - (double)hi);
            std::memcpy(&w[2 * e], &hi, sizeof(float));
            std::memcpy(&w[2 * e + 1], &lo, sizeof(float));
        }
        std::memcpy(&w[12], samples[i].factor, 4 * sizeof(float));
        std::memcpy(&w[16], &samples[i].weight, sizeof(float));
    }

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::dirblur;
    op.params.resize(words.size() * sizeof(uint32_t));
    std::memcpy(op.params.data(), words.data(), op.params.size());

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>(&holdoutView, 1));
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
