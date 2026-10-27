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

#include "Transform.h"

#include <cfloat>
#include <list>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
// The openfx-misc transform interact's scale range.
const double kScaleMax = 10000.;
} // anonymous namespace

void
TransformOp::createKnobs(KnobHolder* holder,
                         const KnobPagePtr& page)
{
    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Translate"), 2);
    translateKnob->setName(kTransformNodeParamTranslate);
    translateKnob->setHintToolTip(EffectInstance::tr("Translation along the x and y axes in pixels. Can also be adjusted by clicking and dragging the center handle in the Viewer."));
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
    translate = translateKnob;

    KnobDoublePtr rotateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Rotate"));
    rotateKnob->setName(kTransformNodeParamRotate);
    rotateKnob->setHintToolTip(EffectInstance::tr("Rotation angle in degrees around the Center. Can also be adjusted by clicking and dragging the rotation bar in the Viewer."));
    rotateKnob->setMinimum(-DBL_MAX);
    rotateKnob->setMaximum(DBL_MAX);
    rotateKnob->setDisplayMinimum(-180.);
    rotateKnob->setDisplayMaximum(180.);
    rotateKnob->setIncrement(0.1);
    rotateKnob->setDefaultValue(0.);
    page->addKnob(rotateKnob);
    rotate = rotateKnob;

    KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Scale"), 2);
    scaleKnob->setName(kTransformNodeParamScale);
    scaleKnob->setHintToolTip(EffectInstance::tr("Scale factor along the x and y axes. Can also be adjusted by clicking and dragging the outer circle or the diameter handles in the Viewer."));
    scaleKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        scaleKnob->setMinimum(-kScaleMax, d);
        scaleKnob->setMaximum(kScaleMax, d);
        scaleKnob->setDisplayMinimum(0.1, d);
        scaleKnob->setDisplayMaximum(10., d);
        scaleKnob->setIncrement(0.01, d);
        scaleKnob->setDefaultValue(1., d);
    }
    scaleKnob->setAddNewLine(false);
    scaleKnob->setSpacingBetweenItems(1);
    page->addKnob(scaleKnob);
    scale = scaleKnob;

    // Uniform scaling is easy through the host's knob and overlay, so the OpenFX plug-in hides
    // this until a project gives it a value or an animation; updateUniformVisibility() does too.
    KnobBoolPtr uniformKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Uniform"));
    uniformKnob->setName(kTransformNodeParamUniform);
    uniformKnob->setHintToolTip(EffectInstance::tr("Use the X scale for both directions"));
    uniformKnob->setDefaultValue(false);
    uniformKnob->setSecretByDefault(true);
    uniformKnob->setDefaultAllDimensionsEnabled(false);
    page->addKnob(uniformKnob);
    uniform = uniformKnob;

    KnobDoublePtr skewXKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Skew X"));
    skewXKnob->setName(kTransformNodeParamSkewX);
    skewXKnob->setHintToolTip(EffectInstance::tr("Skew along the x axis. Can also be adjusted by clicking and dragging the skew bar in the Viewer."));
    skewXKnob->setMinimum(-DBL_MAX);
    skewXKnob->setMaximum(DBL_MAX);
    skewXKnob->setDisplayMinimum(-1.);
    skewXKnob->setDisplayMaximum(1.);
    skewXKnob->setIncrement(0.01);
    skewXKnob->setDefaultValue(0.);
    page->addKnob(skewXKnob);
    skewX = skewXKnob;

    KnobDoublePtr skewYKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Skew Y"));
    skewYKnob->setName(kTransformNodeParamSkewY);
    skewYKnob->setHintToolTip(EffectInstance::tr("Skew along the y axis."));
    skewYKnob->setMinimum(-DBL_MAX);
    skewYKnob->setMaximum(DBL_MAX);
    skewYKnob->setDisplayMinimum(-1.);
    skewYKnob->setDisplayMaximum(1.);
    skewYKnob->setIncrement(0.01);
    skewYKnob->setDefaultValue(0.);
    page->addKnob(skewYKnob);
    skewY = skewYKnob;

    KnobChoicePtr skewOrderKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Skew Order"));
    skewOrderKnob->setName(kTransformNodeParamSkewOrder);
    skewOrderKnob->setHintToolTip(EffectInstance::tr("The order in which skew transforms are applied: X then Y, or Y then X."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XY", "XY", ""));
        options.push_back(ChoiceOption("YX", "YX", ""));
        skewOrderKnob->populateChoices(options);
    }
    skewOrderKnob->setDefaultValue(0);
    page->addKnob(skewOrderKnob);
    skewOrder = skewOrderKnob;

    KnobDoublePtr amountKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Amount"));
    amountKnob->setName(kTransformNodeParamAmount);
    amountKnob->setHintToolTip(EffectInstance::tr("Amount of transform to apply. 0 means the transform is identity, 1 means to apply the full transform."));
    amountKnob->setMinimum(-DBL_MAX);
    amountKnob->setMaximum(DBL_MAX);
    amountKnob->setDisplayMinimum(0.);
    amountKnob->setDisplayMaximum(1.);
    amountKnob->setIncrement(0.01);
    amountKnob->setDefaultValue(1.);
    page->addKnob(amountKnob);
    amount = amountKnob;

    KnobDoublePtr centerKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Center"), 2);
    centerKnob->setName(kTransformNodeParamCenter);
    centerKnob->setHintToolTip(EffectInstance::tr("Center of rotation and scale."));
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
    centerKnob->setAddNewLine(false);
    centerKnob->setSpacingBetweenItems(1);
    page->addKnob(centerKnob);
    center = centerKnob;

    KnobButtonPtr resetCenterButton = AppManager::createKnob<KnobButton>(holder, EffectInstance::tr("Reset Center"));
    resetCenterButton->setName(kTransformNodeParamResetCenter);
    resetCenterButton->setHintToolTip(EffectInstance::tr("Reset the position of the center to the center of the input region of definition"));
    page->addKnob(resetCenterButton);
    resetCenter = resetCenterButton;

    KnobBoolPtr centerChangedKnob = AppManager::createKnob<KnobBool>(holder, std::string(kTransformNodeParamCenterChanged));
    centerChangedKnob->setName(kTransformNodeParamCenterChanged);
    centerChangedKnob->setDefaultValue(false);
    centerChangedKnob->setSecretByDefault(true);
    centerChangedKnob->setDefaultAllDimensionsEnabled(false);
    centerChangedKnob->setAnimationEnabled(false);
    centerChangedKnob->setEvaluateOnChange(false);
    page->addKnob(centerChangedKnob);
    centerChanged = centerChangedKnob;

    KnobBoolPtr interactOpenKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Show Interact"));
    interactOpenKnob->setName(kTransformNodeParamInteractOpen);
    interactOpenKnob->setHintToolTip(EffectInstance::tr("If checked, the transform interact is displayed over the image."));
    interactOpenKnob->setDefaultValue(true);
    interactOpenKnob->setSecretByDefault(true);
    interactOpenKnob->setDefaultAllDimensionsEnabled(false);
    interactOpenKnob->setAnimationEnabled(false);
    page->addKnob(interactOpenKnob);
    interactOpen = interactOpenKnob;

    KnobBoolPtr interactiveKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Interactive Update"));
    interactiveKnob->setName(kTransformNodeParamInteractive);
    interactiveKnob->setHintToolTip(EffectInstance::tr("If checked, update the parameter values during interaction with the image viewer, else update the values when pen is released."));
    interactiveKnob->setDefaultValue(true);
    interactiveKnob->setEvaluateOnChange(false);
    page->addKnob(interactiveKnob);
    interactive = interactiveKnob;

    // Kept for knob parity with the OpenFX plug-in; the host overlay sizes itself.
    KnobBoolPtr hiDPIKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("HiDPI"));
    hiDPIKnob->setName(kTransformNodeParamHiDPI);
    hiDPIKnob->setHintToolTip(EffectInstance::tr("Should be checked when the display area is High-DPI (a.k.a Retina). Draws OpenGL overlays twice larger."));
    hiDPIKnob->setDefaultValue(false);
    hiDPIKnob->setAnimationEnabled(false);
    hiDPIKnob->setEvaluateOnChange(false);
    page->addKnob(hiDPIKnob);
    hiDPI = hiDPIKnob;

    KnobBoolPtr invertKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Invert"));
    invertKnob->setName(kTransformNodeParamInvert);
    invertKnob->setHintToolTip(EffectInstance::tr("Invert the transform."));
    invertKnob->setDefaultValue(false);
    page->addKnob(invertKnob);
    invert = invertKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr(kResamplerParamFilterTypeLabel));
    filterKnob->setName(kResamplerParamFilterType);
    filterKnob->setHintToolTip(EffectInstance::tr(kResamplerParamFilterTypeHint));
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
    filter = filterKnob;

    KnobBoolPtr clampKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr(kResamplerParamFilterClampLabel));
    clampKnob->setName(kResamplerParamFilterClamp);
    clampKnob->setHintToolTip(EffectInstance::tr(kResamplerParamFilterClampHint));
    clampKnob->setDefaultValue(false);
    clampKnob->setAddNewLine(false);
    clampKnob->setSpacingBetweenItems(1);
    page->addKnob(clampKnob);
    clamp = clampKnob;

    KnobBoolPtr blackOutsideKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutsideKnob->setName(kResamplerParamFilterBlackOutside);
    blackOutsideKnob->setHintToolTip(EffectInstance::tr(kResamplerParamFilterBlackOutsideHint));
    blackOutsideKnob->setDefaultValue(true);
    page->addKnob(blackOutsideKnob);
    blackOutside = blackOutsideKnob;

    KnobDoublePtr motionBlurKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Motion Blur"));
    motionBlurKnob->setName(kTransformNodeParamMotionBlur);
    motionBlurKnob->setHintToolTip(EffectInstance::tr("Quality of motion blur rendering. 0 disables motion blur, 1 is a good value. Increasing this slows down rendering."));
    motionBlurKnob->setMinimum(0.);
    motionBlurKnob->setMaximum(100.);
    motionBlurKnob->setDisplayMinimum(0.);
    motionBlurKnob->setDisplayMaximum(4.);
    motionBlurKnob->setIncrement(0.01);
    motionBlurKnob->setDefaultValue(0.);
    page->addKnob(motionBlurKnob);
    motionBlur = motionBlurKnob;

    KnobBoolPtr directionalBlurKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Directional Blur Mode"));
    directionalBlurKnob->setName(kTransformNodeParamDirectionalBlur);
    directionalBlurKnob->setHintToolTip(EffectInstance::tr("Motion blur is computed from the original image to the transformed image, each parameter being interpolated linearly. The motionBlur parameter must be set to a nonzero value, and the blackOutside parameter may have an important effect on the result."));
    directionalBlurKnob->setDefaultValue(false);
    page->addKnob(directionalBlurKnob);
    directionalBlur = directionalBlurKnob;

    KnobDoublePtr shutterKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Shutter"));
    shutterKnob->setName(kTransformNodeParamShutter);
    shutterKnob->setHintToolTip(EffectInstance::tr("Controls how long (in frames) the shutter should remain open."));
    shutterKnob->setMinimum(0.);
    shutterKnob->setMaximum(2.);
    shutterKnob->setDisplayMinimum(0.);
    shutterKnob->setDisplayMaximum(2.);
    shutterKnob->setIncrement(0.01);
    shutterKnob->setDefaultValue(0.5);
    page->addKnob(shutterKnob);
    shutter = shutterKnob;

    KnobChoicePtr shutterOffsetKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr(kResamplerParamShutterOffsetLabel));
    shutterOffsetKnob->setName(kResamplerParamShutterOffset);
    shutterOffsetKnob->setHintToolTip(EffectInstance::tr(kResamplerParamShutterOffsetHint));
    {
        const std::vector<Resampler::ChoiceOption>& offsets = Resampler::shutterOffsetOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            options.push_back(ChoiceOption(offsets[i].id, offsets[i].label, offsets[i].hint));
        }
        shutterOffsetKnob->populateChoices(options);
    }
    shutterOffsetKnob->setDefaultValue((int)Resampler::eShutterOffsetStart);
    page->addKnob(shutterOffsetKnob);
    shutterOffset = shutterOffsetKnob;

    KnobDoublePtr shutterCustomOffsetKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(kResamplerParamShutterCustomOffsetLabel));
    shutterCustomOffsetKnob->setName(kResamplerParamShutterCustomOffset);
    shutterCustomOffsetKnob->setHintToolTip(EffectInstance::tr(kResamplerParamShutterCustomOffsetHint));
    shutterCustomOffsetKnob->setMinimum(-1.);
    shutterCustomOffsetKnob->setMaximum(1.);
    shutterCustomOffsetKnob->setDisplayMinimum(-1.);
    shutterCustomOffsetKnob->setDisplayMaximum(1.);
    shutterCustomOffsetKnob->setIncrement(0.1);
    shutterCustomOffsetKnob->setDefaultValue(0.);
    page->addKnob(shutterCustomOffsetKnob);
    shutterCustomOffset = shutterCustomOffsetKnob;
} // TransformOp::createKnobs

TransformOp::Params
TransformOp::bake(const KnobReader& reader) const
{
    Params params;
    TransformMath::TransformParams& p = params.transform;

    p.translateX = reader.value(translate, 0);
    p.translateY = reader.value(translate, 1);
    p.rotate = reader.value(rotate);
    p.scaleX = reader.value(scale, 0);
    p.scaleY = reader.value(scale, 1);
    p.scaleUniform = reader.value(uniform);
    p.skewX = reader.value(skewX);
    p.skewY = reader.value(skewY);
    p.skewOrderYX = reader.value(skewOrder) != 0;
    p.centerX = reader.value(center, 0);
    p.centerY = reader.value(center, 1);
    p.amount = reader.value(amount);

    params.invert = reader.value(invert);
    params.filter = (Resampler::FilterEnum)reader.value(filter);
    params.clamp = reader.value(clamp);
    params.blackOutside = reader.value(blackOutside);

    Resampler::BlurSettings& blur = params.blur;
    blur.motionBlur = reader.value(motionBlur);
    blur.directionalBlur = reader.value(directionalBlur);
    blur.shutter = reader.value(shutter);
    blur.shutterOffset = (Resampler::ShutterOffsetEnum)reader.value(shutterOffset);
    blur.shutterCustomOffset = reader.value(shutterCustomOffset);

    return params;
} // TransformOp::bake

bool
TransformOp::isIdentity(const Params& params,
                       const PassContext& /*context*/) const
{
    if ((params.blur.shutter != 0.) && (params.blur.motionBlur != 0.)) {
        return false;
    }

    return TransformMath::isIdentity(params.transform);
}

ResampleSettings
TransformOp::settings(const Params& params,
                      const PassContext& /*context*/) const
{
    ResampleSettings settings;

    settings.filter = params.filter;
    settings.clamp = params.clamp;
    settings.blackOutside = params.blackOutside;
    settings.invert = params.invert;
    settings.blur = params.blur;
    settings.transformIsIdentity = TransformMath::isIdentity(params.transform);

    return settings;
}

bool
TransformOp::transform(const Params& params,
                       const PassContext& /*context*/,
                       double amount,
                       bool invert,
                       Mat3* matrix) const
{
    *matrix = TransformMath::inverseTransformCanonical(params.transform, amount, invert);

    return true;
}

bool
TransformOp::rendersWithMotionBlur(const Params& params)
{
    const Resampler::BlurSettings& blur = params.blur;

    return (blur.motionBlur != 0.) && ((blur.shutter != 0.) || blur.directionalBlur);
}

std::optional<GpuWeight>
TransformOp::gpuWeight(const Params& params,
                       const PassContext& /*context*/) const
{
    if (rendersWithMotionBlur(params)) {
        return GpuWeight::heavy();
    }

    return std::nullopt;
}

void
resetTransformCenter(KnobHolder* holder,
                     const TransformOp& op,
                     const RectD& sourceRoD,
                     double time)
{
    KnobDoublePtr center = op.center.lock();
    KnobDoublePtr translate = op.translate.lock();

    if (!center) {
        return;
    }
    const TransformMath::TransformParams p = bakeParams(op, time, ViewIdx(0)).transform;
    double sx, sy;
    TransformMath::effectiveScale(p.scaleX, p.scaleY, p.scaleUniform, &sx, &sy);
    const double rot = TransformMath::toRadians(p.rotate);
    const Mat3 rInv = TransformMath::rotation(-rot) * TransformMath::skewXY(p.skewX, p.skewY, p.skewOrderYX) * TransformMath::scale(sx, sy);

    const double newCenterX = (sourceRoD.x1 + sourceRoD.x2) / 2;
    const double newCenterY = (sourceRoD.y1 + sourceRoD.y2) / 2;
    if ((newCenterX == p.centerX) && (newCenterY == p.centerY)) {
        return;
    }
    double newTranslateX = 0.;
    double newTranslateY = 0.;
    if (translate) {
        const double dxrot = newCenterX - p.centerX;
        const double dyrot = newCenterY - p.centerY;
        TransformMath::Point3 dRot = rInv * TransformMath::Point3(dxrot, dyrot, 1.);
        if (dRot.z != 0) {
            dRot.x /= dRot.z;
            dRot.y /= dRot.z;
        }
        newTranslateX = p.translateX + dRot.x - dxrot;
        newTranslateY = p.translateY + dRot.y - dyrot;
    }

    holder->beginChanges();
    center->setValues(newCenterX, newCenterY, ViewSpec::all(), eValueChangedReasonPluginEdited);
    if (translate) {
        translate->setValues(newTranslateX, newTranslateY, ViewSpec::all(), eValueChangedReasonPluginEdited);
    }
    holder->endChanges();
} // resetTransformCenter

template <bool MASKED>
TransformImageNode<MASKED>::TransformImageNode(NodePtr node)
    : Base(node)
{
}

template <bool MASKED>
TransformImageNode<MASKED>::~TransformImageNode()
{
}

template <bool MASKED>
void
TransformImageNode<MASKED>::addAcceptedComponents(int inputNb,
                                                  std::list<ImageLayerDesc>* comps)
{
    Base::addAcceptedComponents(inputNb, comps);
    if (!this->isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

template <bool MASKED>
void
TransformImageNode<MASKED>::initializeKnobs()
{
    const KnobPagePtr page = this->createControlsPage();

    this->op().createKnobs(static_cast<KnobHolder*>(this), page);
    this->addStageKnobs(page);

    KnobBoolPtr srcClipChanged = this->template createKnob<KnobBool>(std::string(kTransformNodeParamSrcClipChanged));
    srcClipChanged->setName(kTransformNodeParamSrcClipChanged);
    srcClipChanged->setDefaultValue(false);
    srcClipChanged->setSecretByDefault(true);
    srcClipChanged->setDefaultAllDimensionsEnabled(false);
    srcClipChanged->setAnimationEnabled(false);
    srcClipChanged->setEvaluateOnChange(false);
    page->addKnob(srcClipChanged);
    _srcClipChanged = srcClipChanged;

    updateShutterEnabled();
    updateClampEnabled(this->getApp()->getTimeLine()->currentFrame(), ViewIdx(0));

    NodePtr node = this->getNode();
    if (node) {
        const TransformOp& op = this->op();
        node->addTransformInteract(op.translate.lock(), op.scale.lock(), op.uniform.lock(), op.rotate.lock(), op.skewX.lock(), op.skewY.lock(), op.skewOrder.lock(), op.center.lock(), op.invert.lock(), op.interactive.lock());
    }
} // TransformImageNode::initializeKnobs

template <bool MASKED>
void
TransformImageNode<MASKED>::updateShutterEnabled()
{
    const TransformOp& op = this->op();
    KnobBoolPtr directionalBlur = op.directionalBlur.lock();

    if (!directionalBlur) {
        return;
    }
    const bool enabled = !directionalBlur->getValue();
    KnobDoublePtr shutter = op.shutter.lock();
    KnobChoicePtr shutterOffset = op.shutterOffset.lock();
    KnobDoublePtr shutterCustomOffset = op.shutterCustomOffset.lock();
    if (shutter) {
        shutter->setAllDimensionsEnabled(enabled);
    }
    if (shutterOffset) {
        shutterOffset->setAllDimensionsEnabled(enabled);
    }
    if (shutterCustomOffset) {
        shutterCustomOffset->setAllDimensionsEnabled(enabled);
    }
}

template <bool MASKED>
void
TransformImageNode<MASKED>::updateClampEnabled(double time,
                                               ViewSpec view)
{
    KnobChoicePtr filter = this->op().filter.lock();
    KnobBoolPtr clamp = this->op().clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

template <bool MASKED>
void
TransformImageNode<MASKED>::updateUniformVisibility(bool allowHiding)
{
    KnobBoolPtr uniform = this->op().uniform.lock();

    if (!uniform) {
        return;
    }
    const bool used = uniform->getValue() || (uniform->getKeyFramesCount(ViewSpec(0), 0) > 0);
    if (!used && !allowHiding) {
        return;
    }
    uniform->setSecret(!used);
    uniform->setAllDimensionsEnabled(used);
}

template <bool MASKED>
void
TransformImageNode<MASKED>::onKnobsLoaded()
{
    updateShutterEnabled();
    updateClampEnabled(this->getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
    updateUniformVisibility(true);
}

template <bool MASKED>
void
TransformImageNode<MASKED>::onInputChanged(int inputNo)
{
    KnobBoolPtr centerChanged = this->op().centerChanged.lock();
    const int source = this->getSourceInput();

    if ((inputNo != source) || !this->getInput(source) || !centerChanged || centerChanged->getValue()) {
        return;
    }
    resetCenter(this->getApp()->getTimeLine()->currentFrame());
}

template <bool MASKED>
bool
TransformImageNode<MASKED>::knobChanged(KnobI* k,
                                        ValueChangedReasonEnum reason,
                                        ViewSpec view,
                                        double time,
                                        bool /*originatedFromMainThread*/)
{
    const TransformOp& op = this->op();
    KnobButtonPtr resetCenterButton = op.resetCenter.lock();
    KnobBoolPtr centerChanged = op.centerChanged.lock();
    if (resetCenterButton && (k == resetCenterButton.get())) {
        resetCenter(time);
        if (centerChanged && centerChanged->getValue()) {
            centerChanged->setValue(false);
        }

        return true;
    }
    KnobDoublePtr center = op.center.lock();
    if (center && (k == center.get())) {
        const bool edited = (reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonPluginEdited) || (reason == eValueChangedReasonNatronGuiEdited) || (reason == eValueChangedReasonNatronInternalEdited);
        if (edited && centerChanged && !centerChanged->getValue()) {
            centerChanged->setValue(true);
        }

        return true;
    }
    KnobChoicePtr filterKnob = op.filter.lock();
    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }
    KnobBoolPtr directionalBlur = op.directionalBlur.lock();
    if (directionalBlur && (k == directionalBlur.get())) {
        updateShutterEnabled();

        return true;
    }
    KnobBoolPtr uniform = op.uniform.lock();
    if (uniform && (k == uniform.get())) {
        if (reason != eValueChangedReasonTimeChanged) {
            updateUniformVisibility(false);
        }

        return true;
    }

    return false;
} // TransformImageNode::knobChanged

template <bool MASKED>
void
TransformImageNode<MASKED>::getTransformParams(double time,
                                               ViewIdx view,
                                               TransformMath::TransformParams* params) const
{
    *params = bakeParams(this->op(), time, view).transform;
}

template <bool MASKED>
bool
TransformImageNode<MASKED>::rendersWithMotionBlur(double time,
                                                  ViewIdx view) const
{
    return TransformOp::rendersWithMotionBlur(bakeParams(this->op(), time, view));
}

template <bool MASKED>
bool
TransformImageNode<MASKED>::getSourceRoD(double time,
                                         const RenderScale& scale,
                                         ViewIdx view,
                                         RectD* rod) const
{
    EffectInstancePtr input = this->getInput(this->getSourceInput());

    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) != eStatusFailed;
}

template <bool MASKED>
RectD
TransformImageNode<MASKED>::getProjectRect() const
{
    Format format;

    this->getApp()->getProject()->getProjectDefaultFormat(&format);
    RectI pixel;
    pixel.x1 = format.x1;
    pixel.x2 = format.x2;
    pixel.y1 = format.y1;
    pixel.y2 = format.y2;

    return pixel.toCanonical_noClipping(0, format.getPixelAspectRatio());
}

template <bool MASKED>
void
TransformImageNode<MASKED>::resetCenter(double time)
{
    if (!this->op().center.lock() || !this->getInput(this->getSourceInput())) {
        return;
    }
    RectD rod;
    if (!getSourceRoD(time, RenderScale::identity, ViewIdx(0), &rod)) {
        return;
    }
    if (rod.isInfinite()) {
        return;
    }
    if (rod.isNull()) {
        rod = getProjectRect();
    }

    resetTransformCenter(this, this->op(), rod, time);
} // TransformImageNode::resetCenter

template class TransformImageNode<false>;
template class TransformImageNode<true>;

NATRON_NAMESPACE_EXIT
