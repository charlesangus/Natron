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

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstddef>
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
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
// How many rows render() runs between two abort checks.
const int kAbortCheckRows = 16;

// The openfx-misc transform interact's scale range.
const double kScaleMax = 10000.;

int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

// One plane of the render. The resampler reads the source through `source`, which points either
// into the source image or, when the image's layout differs from the output's, into `converted`.
struct TransformPlaneJob {
    ImagePtr dst;
    ImagePtr src;
    Transform::Matrix3x3Ptr inputTransform;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    std::vector<float> converted;
    Resampler::SourceImage source;
    Resampler::SamplingTransforms transforms;
    Resampler::ResampleParams params;
};

struct TransformBand {
    std::size_t job;
    int y1;
    int y2;

    TransformBand(std::size_t jobIndex,
                  int firstRow,
                  int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

// Copies the source pixels into `out` in an nComps layout, mapping channels by colour bit and
// reading zero for a channel the source lacks.
void
convertSource(const Image::ReadAccess& access,
              const RectI& bounds,
              int srcNComps,
              int nComps,
              std::vector<float>* out)
{
    const std::size_t width = (std::size_t)bounds.width();
    const std::size_t height = (std::size_t)bounds.height();

    out->assign(width * height * (std::size_t)nComps, 0.f);
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        const float* srcPix = (const float*)access.pixelAt(bounds.x1, y);
        if (!srcPix) {
            continue;
        }
        float* dstPix = &(*out)[(std::size_t)(y - bounds.y1) * width * nComps];
        for (std::size_t x = 0; x < width; ++x, srcPix += srcNComps, dstPix += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (srcIndex[c] >= 0) {
                    dstPix[c] = srcPix[srcIndex[c]];
                }
            }
        }
    }
}

// One value per pixel of `channel` of row y, zero wherever the image has no pixel.
void
readMaskRow(const Image::ReadAccess* access,
            const RectI& bounds,
            int nComps,
            int channel,
            int x0,
            int y,
            int width,
            float* row)
{
    std::fill(row, row + width, 0.f);
    if (!access || (y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const float* pix = (const float*)access->pixelAt(xStart, y);
    if (!pix) {
        return;
    }
    for (int x = xStart; x < xEnd; ++x, pix += nComps) {
        row[x - x0] = pix[channel];
    }
}

NativeImageTraits
transformTraits()
{
    NativeImageTraits traits;

    traits.processesAllLayers = true;

    return traits;
}
} // anonymous namespace

TransformNode::TransformNode(NodePtr node,
                             bool masked)
    : NativeImageEffect(node, transformTraits())
    , _masked(masked)
{
}

TransformNode::~TransformNode()
{
}

void
TransformNode::addAcceptedComponents(int inputNb,
                                     std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
TransformNode::getNativePluginDescription() const
{
    NativePluginDescription desc;

    if (_masked) {
        desc.id = PLUGINID_NATRON_TRANSFORMMASKED;
        desc.label = "TransformMasked";
        desc.description = tr("Translate / Rotate / Scale a 2D image, with optional masking.\n"
                              "This plugin concatenates transforms upstream.")
                               .toStdString();
        desc.majorVersion = PLUGIN_MAJOR_NATRON_TRANSFORMMASKED;
    } else {
        desc.id = PLUGINID_NATRON_TRANSFORM;
        desc.label = "Transform";
        desc.description = tr("Translate / Rotate / Scale a 2D image.\n"
                              "This plugin concatenates transforms.\n"
                              "See also https://web.archive.org/web/20220627030948/http://www.opticalenquiry.com/nuke/index.php?title=Transform")
                               .toStdString();
        desc.majorVersion = PLUGIN_MAJOR_NATRON_TRANSFORM;
    }
    desc.grouping = PLUGIN_GROUP_TRANSFORM;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    if (_masked) {
        desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    }
    desc.outputKind = eDataKindImage;

    return desc;
}

void
TransformNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr translate = createKnob<KnobDouble>(tr("Translate"), 2);
    translate->setName(kTransformNodeParamTranslate);
    translate->setHintToolTip(tr("Translation along the x and y axes in pixels. Can also be adjusted by clicking and dragging the center handle in the Viewer."));
    translate->setSpatial(true);
    translate->setDefaultValuesAreNormalized(true);
    translate->disableSlider();
    for (int d = 0; d < 2; ++d) {
        translate->setMinimum(-DBL_MAX, d);
        translate->setMaximum(DBL_MAX, d);
        translate->setDisplayMinimum(-10000., d);
        translate->setDisplayMaximum(10000., d);
        translate->setIncrement(10., d);
        translate->setDefaultValue(0., d);
    }
    page->addKnob(translate);
    _translate = translate;

    KnobDoublePtr rotate = createKnob<KnobDouble>(tr("Rotate"));
    rotate->setName(kTransformNodeParamRotate);
    rotate->setHintToolTip(tr("Rotation angle in degrees around the Center. Can also be adjusted by clicking and dragging the rotation bar in the Viewer."));
    rotate->setMinimum(-DBL_MAX);
    rotate->setMaximum(DBL_MAX);
    rotate->setDisplayMinimum(-180.);
    rotate->setDisplayMaximum(180.);
    rotate->setIncrement(0.1);
    rotate->setDefaultValue(0.);
    page->addKnob(rotate);
    _rotate = rotate;

    KnobDoublePtr scale = createKnob<KnobDouble>(tr("Scale"), 2);
    scale->setName(kTransformNodeParamScale);
    scale->setHintToolTip(tr("Scale factor along the x and y axes. Can also be adjusted by clicking and dragging the outer circle or the diameter handles in the Viewer."));
    scale->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        scale->setMinimum(-kScaleMax, d);
        scale->setMaximum(kScaleMax, d);
        scale->setDisplayMinimum(0.1, d);
        scale->setDisplayMaximum(10., d);
        scale->setIncrement(0.01, d);
        scale->setDefaultValue(1., d);
    }
    scale->setAddNewLine(false);
    scale->setSpacingBetweenItems(1);
    page->addKnob(scale);
    _scale = scale;

    // Uniform scaling is easy through the host's knob and overlay, so the OpenFX plug-in hides
    // this until a project gives it a value or an animation; updateUniformVisibility() does too.
    KnobBoolPtr uniform = createKnob<KnobBool>(tr("Uniform"));
    uniform->setName(kTransformNodeParamUniform);
    uniform->setHintToolTip(tr("Use the X scale for both directions"));
    uniform->setDefaultValue(false);
    uniform->setSecretByDefault(true);
    uniform->setDefaultAllDimensionsEnabled(false);
    page->addKnob(uniform);
    _uniform = uniform;

    KnobDoublePtr skewX = createKnob<KnobDouble>(tr("Skew X"));
    skewX->setName(kTransformNodeParamSkewX);
    skewX->setHintToolTip(tr("Skew along the x axis. Can also be adjusted by clicking and dragging the skew bar in the Viewer."));
    skewX->setMinimum(-DBL_MAX);
    skewX->setMaximum(DBL_MAX);
    skewX->setDisplayMinimum(-1.);
    skewX->setDisplayMaximum(1.);
    skewX->setIncrement(0.01);
    skewX->setDefaultValue(0.);
    page->addKnob(skewX);
    _skewX = skewX;

    KnobDoublePtr skewY = createKnob<KnobDouble>(tr("Skew Y"));
    skewY->setName(kTransformNodeParamSkewY);
    skewY->setHintToolTip(tr("Skew along the y axis."));
    skewY->setMinimum(-DBL_MAX);
    skewY->setMaximum(DBL_MAX);
    skewY->setDisplayMinimum(-1.);
    skewY->setDisplayMaximum(1.);
    skewY->setIncrement(0.01);
    skewY->setDefaultValue(0.);
    page->addKnob(skewY);
    _skewY = skewY;

    KnobChoicePtr skewOrder = createKnob<KnobChoice>(tr("Skew Order"));
    skewOrder->setName(kTransformNodeParamSkewOrder);
    skewOrder->setHintToolTip(tr("The order in which skew transforms are applied: X then Y, or Y then X."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XY", "XY", ""));
        options.push_back(ChoiceOption("YX", "YX", ""));
        skewOrder->populateChoices(options);
    }
    skewOrder->setDefaultValue(0);
    page->addKnob(skewOrder);
    _skewOrder = skewOrder;

    KnobDoublePtr amount = createKnob<KnobDouble>(tr("Amount"));
    amount->setName(kTransformNodeParamAmount);
    amount->setHintToolTip(tr("Amount of transform to apply. 0 means the transform is identity, 1 means to apply the full transform."));
    amount->setMinimum(-DBL_MAX);
    amount->setMaximum(DBL_MAX);
    amount->setDisplayMinimum(0.);
    amount->setDisplayMaximum(1.);
    amount->setIncrement(0.01);
    amount->setDefaultValue(1.);
    page->addKnob(amount);
    _amount = amount;

    KnobDoublePtr center = createKnob<KnobDouble>(tr("Center"), 2);
    center->setName(kTransformNodeParamCenter);
    center->setHintToolTip(tr("Center of rotation and scale."));
    center->setSpatial(true);
    center->setDefaultValuesAreNormalized(true);
    center->disableSlider();
    for (int d = 0; d < 2; ++d) {
        center->setMinimum(-DBL_MAX, d);
        center->setMaximum(DBL_MAX, d);
        center->setDisplayMinimum(-10000., d);
        center->setDisplayMaximum(10000., d);
        center->setIncrement(1., d);
        center->setDefaultValue(0.5, d);
    }
    center->setAddNewLine(false);
    center->setSpacingBetweenItems(1);
    page->addKnob(center);
    _center = center;

    KnobButtonPtr resetCenterButton = createKnob<KnobButton>(tr("Reset Center"));
    resetCenterButton->setName(kTransformNodeParamResetCenter);
    resetCenterButton->setHintToolTip(tr("Reset the position of the center to the center of the input region of definition"));
    page->addKnob(resetCenterButton);
    _resetCenter = resetCenterButton;

    KnobBoolPtr centerChanged = createKnob<KnobBool>(std::string(kTransformNodeParamCenterChanged));
    centerChanged->setName(kTransformNodeParamCenterChanged);
    centerChanged->setDefaultValue(false);
    centerChanged->setSecretByDefault(true);
    centerChanged->setDefaultAllDimensionsEnabled(false);
    centerChanged->setAnimationEnabled(false);
    centerChanged->setEvaluateOnChange(false);
    page->addKnob(centerChanged);
    _centerChanged = centerChanged;

    KnobBoolPtr interactOpen = createKnob<KnobBool>(tr("Show Interact"));
    interactOpen->setName(kTransformNodeParamInteractOpen);
    interactOpen->setHintToolTip(tr("If checked, the transform interact is displayed over the image."));
    interactOpen->setDefaultValue(true);
    interactOpen->setSecretByDefault(true);
    interactOpen->setDefaultAllDimensionsEnabled(false);
    interactOpen->setAnimationEnabled(false);
    page->addKnob(interactOpen);
    _interactOpen = interactOpen;

    KnobBoolPtr interactive = createKnob<KnobBool>(tr("Interactive Update"));
    interactive->setName(kTransformNodeParamInteractive);
    interactive->setHintToolTip(tr("If checked, update the parameter values during interaction with the image viewer, else update the values when pen is released."));
    interactive->setDefaultValue(true);
    interactive->setEvaluateOnChange(false);
    page->addKnob(interactive);
    _interactive = interactive;

    // Kept for knob parity with the OpenFX plug-in; the host overlay sizes itself.
    KnobBoolPtr hiDPI = createKnob<KnobBool>(tr("HiDPI"));
    hiDPI->setName(kTransformNodeParamHiDPI);
    hiDPI->setHintToolTip(tr("Should be checked when the display area is High-DPI (a.k.a Retina). Draws OpenGL overlays twice larger."));
    hiDPI->setDefaultValue(false);
    hiDPI->setAnimationEnabled(false);
    hiDPI->setEvaluateOnChange(false);
    page->addKnob(hiDPI);
    _hiDPI = hiDPI;

    KnobBoolPtr invert = createKnob<KnobBool>(tr("Invert"));
    invert->setName(kTransformNodeParamInvert);
    invert->setHintToolTip(tr("Invert the transform."));
    invert->setDefaultValue(false);
    page->addKnob(invert);
    _invert = invert;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    _clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    KnobDoublePtr motionBlur = createKnob<KnobDouble>(tr("Motion Blur"));
    motionBlur->setName(kTransformNodeParamMotionBlur);
    motionBlur->setHintToolTip(tr("Quality of motion blur rendering. 0 disables motion blur, 1 is a good value. Increasing this slows down rendering."));
    motionBlur->setMinimum(0.);
    motionBlur->setMaximum(100.);
    motionBlur->setDisplayMinimum(0.);
    motionBlur->setDisplayMaximum(4.);
    motionBlur->setIncrement(0.01);
    motionBlur->setDefaultValue(0.);
    page->addKnob(motionBlur);
    _motionBlur = motionBlur;

    KnobBoolPtr directionalBlur = createKnob<KnobBool>(tr("Directional Blur Mode"));
    directionalBlur->setName(kTransformNodeParamDirectionalBlur);
    directionalBlur->setHintToolTip(tr("Motion blur is computed from the original image to the transformed image, each parameter being interpolated linearly. The motionBlur parameter must be set to a nonzero value, and the blackOutside parameter may have an important effect on the result."));
    directionalBlur->setDefaultValue(false);
    page->addKnob(directionalBlur);
    _directionalBlur = directionalBlur;

    KnobDoublePtr shutter = createKnob<KnobDouble>(tr("Shutter"));
    shutter->setName(kTransformNodeParamShutter);
    shutter->setHintToolTip(tr("Controls how long (in frames) the shutter should remain open."));
    shutter->setMinimum(0.);
    shutter->setMaximum(2.);
    shutter->setDisplayMinimum(0.);
    shutter->setDisplayMaximum(2.);
    shutter->setIncrement(0.01);
    shutter->setDefaultValue(0.5);
    page->addKnob(shutter);
    _shutter = shutter;

    KnobChoicePtr shutterOffset = createKnob<KnobChoice>(tr(kResamplerParamShutterOffsetLabel));
    shutterOffset->setName(kResamplerParamShutterOffset);
    shutterOffset->setHintToolTip(tr(kResamplerParamShutterOffsetHint));
    {
        const std::vector<Resampler::ChoiceOption>& offsets = Resampler::shutterOffsetOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            options.push_back(ChoiceOption(offsets[i].id, offsets[i].label, offsets[i].hint));
        }
        shutterOffset->populateChoices(options);
    }
    shutterOffset->setDefaultValue((int)Resampler::eShutterOffsetStart);
    page->addKnob(shutterOffset);
    _shutterOffset = shutterOffset;

    KnobDoublePtr shutterCustomOffset = createKnob<KnobDouble>(tr(kResamplerParamShutterCustomOffsetLabel));
    shutterCustomOffset->setName(kResamplerParamShutterCustomOffset);
    shutterCustomOffset->setHintToolTip(tr(kResamplerParamShutterCustomOffsetHint));
    shutterCustomOffset->setMinimum(-1.);
    shutterCustomOffset->setMaximum(1.);
    shutterCustomOffset->setDisplayMinimum(-1.);
    shutterCustomOffset->setDisplayMaximum(1.);
    shutterCustomOffset->setIncrement(0.1);
    shutterCustomOffset->setDefaultValue(0.);
    page->addKnob(shutterCustomOffset);
    _shutterCustomOffset = shutterCustomOffset;

    if (_masked) {
        addMaskMixKnobs(page);
    }

    KnobBoolPtr srcClipChanged = createKnob<KnobBool>(std::string(kTransformNodeParamSrcClipChanged));
    srcClipChanged->setName(kTransformNodeParamSrcClipChanged);
    srcClipChanged->setDefaultValue(false);
    srcClipChanged->setSecretByDefault(true);
    srcClipChanged->setDefaultAllDimensionsEnabled(false);
    srcClipChanged->setAnimationEnabled(false);
    srcClipChanged->setEvaluateOnChange(false);
    page->addKnob(srcClipChanged);
    _srcClipChanged = srcClipChanged;

    updateShutterEnabled();

    NodePtr node = getNode();
    if (node) {
        node->addTransformInteract(translate, scale, uniform, rotate, skewX, skewY, skewOrder, center, invert, interactive);
    }
} // TransformNode::initializeKnobs

void
TransformNode::updateShutterEnabled()
{
    KnobBoolPtr directionalBlur = _directionalBlur.lock();

    if (!directionalBlur) {
        return;
    }
    const bool enabled = !directionalBlur->getValue();
    KnobDoublePtr shutter = _shutter.lock();
    KnobChoicePtr shutterOffset = _shutterOffset.lock();
    KnobDoublePtr shutterCustomOffset = _shutterCustomOffset.lock();
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

void
TransformNode::updateUniformVisibility(bool allowHiding)
{
    KnobBoolPtr uniform = _uniform.lock();

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

void
TransformNode::onKnobsLoaded()
{
    updateShutterEnabled();
    updateUniformVisibility(true);
}

void
TransformNode::onInputChanged(int inputNo)
{
    KnobBoolPtr centerChanged = _centerChanged.lock();

    if ((inputNo != 0) || !getInput(0) || !centerChanged || centerChanged->getValue()) {
        return;
    }
    resetCenter(getApp()->getTimeLine()->currentFrame());
}

bool
TransformNode::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason,
                           ViewSpec /*view*/,
                           double time,
                           bool /*originatedFromMainThread*/)
{
    KnobButtonPtr resetCenterButton = _resetCenter.lock();
    KnobBoolPtr centerChanged = _centerChanged.lock();
    if (resetCenterButton && (k == resetCenterButton.get())) {
        resetCenter(time);
        if (centerChanged && centerChanged->getValue()) {
            centerChanged->setValue(false);
        }

        return true;
    }
    KnobDoublePtr center = _center.lock();
    if (center && (k == center.get())) {
        const bool edited = (reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonPluginEdited) || (reason == eValueChangedReasonNatronGuiEdited) || (reason == eValueChangedReasonNatronInternalEdited);
        if (edited && centerChanged && !centerChanged->getValue()) {
            centerChanged->setValue(true);
        }

        return true;
    }
    KnobBoolPtr directionalBlur = _directionalBlur.lock();
    if (directionalBlur && (k == directionalBlur.get())) {
        updateShutterEnabled();

        return true;
    }
    KnobBoolPtr uniform = _uniform.lock();
    if (uniform && (k == uniform.get())) {
        if (reason != eValueChangedReasonTimeChanged) {
            updateUniformVisibility(false);
        }

        return true;
    }

    return false;
} // TransformNode::knobChanged

void
TransformNode::getTransformParams(double time,
                                  ViewIdx view,
                                  TransformMath::TransformParams* p) const
{
    *p = TransformMath::TransformParams();

    KnobDoublePtr translate = _translate.lock();
    if (translate) {
        p->translateX = translate->getValueAtTime(time, 0, view);
        p->translateY = translate->getValueAtTime(time, 1, view);
    }
    KnobDoublePtr rotate = _rotate.lock();
    if (rotate) {
        p->rotate = rotate->getValueAtTime(time, 0, view);
    }
    KnobDoublePtr scale = _scale.lock();
    if (scale) {
        p->scaleX = scale->getValueAtTime(time, 0, view);
        p->scaleY = scale->getValueAtTime(time, 1, view);
    }
    KnobBoolPtr uniform = _uniform.lock();
    if (uniform) {
        p->scaleUniform = uniform->getValueAtTime(time, 0, view);
    }
    KnobDoublePtr skewX = _skewX.lock();
    if (skewX) {
        p->skewX = skewX->getValueAtTime(time, 0, view);
    }
    KnobDoublePtr skewY = _skewY.lock();
    if (skewY) {
        p->skewY = skewY->getValueAtTime(time, 0, view);
    }
    KnobChoicePtr skewOrder = _skewOrder.lock();
    if (skewOrder) {
        p->skewOrderYX = skewOrder->getValueAtTime(time, 0, view) != 0;
    }
    KnobDoublePtr center = _center.lock();
    if (center) {
        p->centerX = center->getValueAtTime(time, 0, view);
        p->centerY = center->getValueAtTime(time, 1, view);
    }
    KnobDoublePtr amount = _amount.lock();
    if (amount) {
        p->amount = amount->getValueAtTime(time, 0, view);
    }
} // TransformNode::getTransformParams

Resampler::CanonicalTransformFn
TransformNode::makeCanonicalTransformFn(ViewIdx view) const
{
    const TransformNode* self = this;

    return [self, view](double time, double amount, bool invert, Mat3* matrix) {
        TransformMath::TransformParams p;
        self->getTransformParams(time, view, &p);
        *matrix = TransformMath::inverseTransformCanonical(p, amount, invert);

        return true;
    };
}

Resampler::BlurSettings
TransformNode::getBlurSettings(double time,
                               ViewIdx view) const
{
    Resampler::BlurSettings blur;

    KnobDoublePtr motionBlur = _motionBlur.lock();
    if (motionBlur) {
        blur.motionBlur = motionBlur->getValueAtTime(time, 0, view);
    }
    KnobBoolPtr directionalBlur = _directionalBlur.lock();
    if (directionalBlur) {
        blur.directionalBlur = directionalBlur->getValueAtTime(time, 0, view);
    }
    KnobDoublePtr shutter = _shutter.lock();
    if (shutter) {
        blur.shutter = shutter->getValueAtTime(time, 0, view);
    }
    KnobChoicePtr shutterOffset = _shutterOffset.lock();
    if (shutterOffset) {
        blur.shutterOffset = (Resampler::ShutterOffsetEnum)shutterOffset->getValueAtTime(time, 0, view);
    }
    KnobDoublePtr shutterCustomOffset = _shutterCustomOffset.lock();
    if (shutterCustomOffset) {
        blur.shutterCustomOffset = shutterCustomOffset->getValueAtTime(time, 0, view);
    }

    return blur;
}

bool
TransformNode::isMasking() const
{
    return _masked && isMaskApplied();
}

Resampler::RegionParams
TransformNode::getRegionParams(double time,
                               ViewIdx view) const
{
    Resampler::RegionParams params;

    KnobBoolPtr invert = _invert.lock();
    params.invert = invert ? invert->getValueAtTime(time, 0, view) : false;
    params.blur = getBlurSettings(time, view);
    KnobChoicePtr filter = _filter.lock();
    params.filter = filter ? (Resampler::FilterEnum)filter->getValueAtTime(time, 0, view) : Resampler::eFilterCubic;
    KnobBoolPtr blackOutside = _blackOutside.lock();
    params.blackOutside = blackOutside ? blackOutside->getValueAtTime(time, 0, view) : true;
    TransformMath::TransformParams p;
    getTransformParams(time, view, &p);
    params.isIdentity = TransformMath::isIdentity(p);
    params.doMasking = isMasking();
    params.mix = params.doMasking ? getMixValue(time, view) : 1.;

    return params;
}

bool
TransformNode::getSourceRoD(double time,
                            const RenderScale& scale,
                            ViewIdx view,
                            RectD* rod) const
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) != eStatusFailed;
}

RectD
TransformNode::getProjectRect() const
{
    Format format;

    getApp()->getProject()->getProjectDefaultFormat(&format);
    RectI pixel;
    pixel.x1 = format.x1;
    pixel.x2 = format.x2;
    pixel.y1 = format.y1;
    pixel.y2 = format.y2;

    return pixel.toCanonical_noClipping(0, format.getPixelAspectRatio());
}

void
TransformNode::resetCenter(double time)
{
    KnobDoublePtr center = _center.lock();
    KnobDoublePtr translate = _translate.lock();

    if (!center || !getInput(0)) {
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

    TransformMath::TransformParams p;
    getTransformParams(time, ViewIdx(0), &p);
    double sx, sy;
    TransformMath::effectiveScale(p.scaleX, p.scaleY, p.scaleUniform, &sx, &sy);
    const double rot = TransformMath::toRadians(p.rotate);
    const Mat3 rInv = TransformMath::rotation(-rot) * TransformMath::skewXY(p.skewX, p.skewY, p.skewOrderYX) * TransformMath::scale(sx, sy);

    const double newCenterX = (rod.x1 + rod.x2) / 2;
    const double newCenterY = (rod.y1 + rod.y2) / 2;
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

    beginChanges();
    center->setValues(newCenterX, newCenterY, ViewSpec::all(), eValueChangedReasonPluginEdited);
    if (translate) {
        translate->setValues(newTranslateX, newTranslateY, ViewSpec::all(), eValueChangedReasonPluginEdited);
    }
    endChanges();
} // TransformNode::resetCenter

bool
TransformNode::getInputsHoldingTransform(std::list<int>* inputs) const
{
    if (!inputs) {
        return false;
    }
    inputs->push_back(0);

    return true;
}

StatusEnum
TransformNode::getTransform(double time,
                            const RenderScale& renderScale,
                            bool draftRender,
                            ViewIdx view,
                            EffectInstancePtr* inputToTransform,
                            Transform::Matrix3x3* transform)
{
    if (!draftRender) {
        const Resampler::BlurSettings blur = getBlurSettings(time, view);
        const double shutter = blur.directionalBlur ? 0. : blur.shutter;
        if (((shutter != 0.) && (blur.motionBlur != 0.)) || blur.directionalBlur) {
            return eStatusReplyDefault;
        }
    }

    KnobBoolPtr invertKnob = _invert.lock();
    const bool invert = invertKnob ? invertKnob->getValueAtTime(time, 0, view) : false;
    TransformMath::TransformParams p;
    getTransformParams(time, view, &p);
    const Mat3 invCanonical = TransformMath::inverseTransformCanonical(p, 1., invert);
    Mat3 canonical;
    if (!invCanonical.inverse(&canonical)) {
        return eStatusReplyDefault;
    }

    EffectInstancePtr input = getInput(0);
    if (!input) {
        return eStatusFailed;
    }
    const double srcPar = getAspectRatio(0);
    const double dstPar = getAspectRatio(-1);
    const OfxPointD s = renderScale.toOfxPointD();
    *transform = TransformMath::toEngineMatrix(TransformMath::forwardToPixel(canonical, srcPar, dstPar, s.x, s.y));
    *inputToTransform = input;

    return eStatusOK;
}

StatusEnum
TransformNode::getRegionOfDefinition(U64 /*hash*/,
                                     double time,
                                     const RenderScale& scale,
                                     ViewIdx view,
                                     RectD* rod)
{
    if (!getInput(0)) {
        return eStatusReplyDefault;
    }
    RectD srcRoD;
    if (!getSourceRoD(time, scale, view, &srcRoD)) {
        return eStatusFailed;
    }
    const OfxPointD s = scale.toOfxPointD();
    Resampler::getRegionOfDefinition(makeCanonicalTransformFn(view), srcRoD, time, getAspectRatio(-1), s.x, s.y, getRegionParams(time, view), rod);

    return eStatusOK;
}

void
TransformNode::getRegionsOfInterest(double time,
                                    const RenderScale& scale,
                                    const RectD& /*outputRoD*/,
                                    const RectD& renderWindow,
                                    ViewIdx view,
                                    RoIMap* ret)
{
    EffectInstancePtr source = getInput(0);
    const int maskInput = _masked ? getMaskInput() : -1;
    EffectInstancePtr mask = (maskInput >= 0) ? getInput(maskInput) : EffectInstancePtr();

    RectD srcRoI = renderWindow;
    if (source) {
        RectD srcRoD;
        if (!getSourceRoD(time, scale, view, &srcRoD)) {
            srcRoD = RectD();
        }
        const OfxPointD s = scale.toOfxPointD();
        Resampler::getRegionOfInterest(makeCanonicalTransformFn(view), renderWindow, srcRoD, getProjectRect(), time, getAspectRatio(0), s.x, s.y, getRegionParams(time, view), &srcRoI);
        ret->insert(std::make_pair(source, srcRoI));
    }
    // The mask is read under each output pixel, as the OpenFX host's default region of interest.
    if (mask) {
        RoIMap::iterator found = ret->find(mask);
        if (found == ret->end()) {
            ret->insert(std::make_pair(mask, renderWindow));
        } else {
            RectD& r = found->second;
            r.x1 = std::min(r.x1, renderWindow.x1);
            r.y1 = std::min(r.y1, renderWindow.y1);
            r.x2 = std::max(r.x2, renderWindow.x2);
            r.y2 = std::max(r.y2, renderWindow.y2);
        }
    }
}

bool
TransformNode::isIdentity(double time,
                          const RenderScale& scale,
                          const RectI& roi,
                          ViewIdx view,
                          double* inputTime,
                          ViewIdx* inputView,
                          int* inputNb)
{
    *inputTime = time;
    *inputView = view;
    *inputNb = 0;

    const Resampler::BlurSettings blur = getBlurSettings(time, view);
    if ((blur.shutter != 0.) && (blur.motionBlur != 0.)) {
        return false;
    }
    KnobBoolPtr clamp = _clamp.lock();
    if (clamp && clamp->getValueAtTime(time, 0, view)) {
        return false;
    }
    TransformMath::TransformParams p;
    getTransformParams(time, view, &p);
    if (TransformMath::isIdentity(p)) {
        return true;
    }
    if (!_masked) {
        return false;
    }
    if (getMixValue(time, view) == 0.) {
        return true;
    }
    if (isMasking() && !getMaskInvertValue(time, view)) {
        EffectInstancePtr mask = getInput(getMaskInput());
        const RenderScale maskScale = mask->supportsRenderScale() ? scale : RenderScale::identity;
        RectD maskRoD;
        bool isProjectFormat = false;
        if (mask->getRegionOfDefinition_public(mask->getRenderHash(), time, maskScale, view, &maskRoD, &isProjectFormat) != eStatusFailed) {
            // The window does not reach the mask, so the source shows through unchanged.
            const RectI maskPixels = maskRoD.toPixelEnclosing(scale, mask->getAspectRatio(-1));
            if (!roi.intersects(maskPixels)) {
                return true;
            }
        }
    }

    return false;
} // TransformNode::isIdentity

StatusEnum
TransformNode::render(const RenderActionArgs& args)
{
    const double time = args.time;
    const ViewIdx view = args.view;
    const OfxPointD s = args.mappedScale.toOfxPointD();

    KnobBoolPtr invertKnob = _invert.lock();
    KnobChoicePtr filterKnob = _filter.lock();
    KnobBoolPtr clampKnob = _clamp.lock();
    KnobBoolPtr blackOutsideKnob = _blackOutside.lock();
    const bool invert = invertKnob ? invertKnob->getValueAtTime(time, 0, view) : false;
    Resampler::FilterEnum filter = Resampler::eFilterImpulse;
    if (!args.draftMode) {
        filter = filterKnob ? (Resampler::FilterEnum)filterKnob->getValueAtTime(time, 0, view) : Resampler::eFilterCubic;
    }
    const bool clamp = clampKnob ? clampKnob->getValueAtTime(time, 0, view) : false;
    const bool blackOutside = blackOutsideKnob ? blackOutsideKnob->getValueAtTime(time, 0, view) : false;

    Resampler::BlurSettings blur = getBlurSettings(time, view);
    if (blur.directionalBlur) {
        blur.shutter = 0.;
    }
    const Resampler::CanonicalTransformFn fn = makeCanonicalTransformFn(view);
    const double dstPar = getAspectRatio(-1);

    const float mix = _masked ? (float)getMixValue(time, view) : 1.f;
    const bool maskInvert = _masked ? getMaskInvertValue(time, view) : false;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = isMasking();
    ImagePtr mask;
    int maskChannel = -1;
    if (doMask) {
        const int maskInput = getMaskInput();
        ImageLayerDesc maskLayer;
        if (resolveInputPlaneForRender(maskInput, time, view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
            RectI maskRoI;
            mask = getImage(maskInput, time, args.mappedScale, view, NULL, &maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }

    std::vector<std::shared_ptr<TransformPlaneJob>> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        std::shared_ptr<TransformPlaneJob> job = std::make_shared<TransformPlaneJob>();
        job->dst = it->second;
        if (job->dst) {
            ImageLayerDesc sourceLayer;
            if (getInput(0) && resolveInputPlaneForRender(0, time, view, &sourceLayer, NULL)) {
                RectI sourceRoI;
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                job->src = getImage(0, time, args.mappedScale, view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI, &job->inputTransform);
            }
            if (!isFloatImage(job->dst) || !isFloatImage(job->src)) {
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
    int maskNComps = 0;
    if (mask) {
        maskBounds = mask->getBounds();
        maskNComps = (int)mask->getComponentsCount();
        maskAccess = std::make_shared<Image::ReadAccess>(mask.get());
    }

    // Images are locked, their bounds read and the matrices built here, on the calling thread;
    // the band threads only read through these.
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeRowBands(roi, nThreads, &bandRects);
    std::vector<TransformBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        TransformPlaneJob& job = *jobs[j];
        if (!job.dst) {
            continue;
        }
        const int nComps = (int)job.dst->getComponentsCount();
        if (job.src) {
            const RectI srcBounds = job.src->getBounds();
            const int srcNComps = (int)job.src->getComponentsCount();
            const double srcPar = job.src->getPixelAspectRatio();
            job.srcAccess = std::make_shared<Image::ReadAccess>(job.src.get());
            if (srcNComps == nComps) {
                job.source = Resampler::SourceImage((const float*)job.srcAccess->pixelAt(srcBounds.x1, srcBounds.y1), srcBounds, nComps);
            } else {
                convertSource(*job.srcAccess, srcBounds, srcNComps, nComps, &job.converted);
                job.source = Resampler::SourceImage(job.converted.empty() ? (const float*)0 : &job.converted[0], srcBounds, nComps);
            }
            Resampler::buildSamplingTransforms(fn, time, invert, blur, s.x, s.y, false, srcPar, dstPar, &job.transforms);
            if (job.inputTransform) {
                Resampler::concatenateInputTransform(TransformMath::fromEngineMatrix(*job.inputTransform), &job.transforms);
            }
        } else {
            job.source = Resampler::SourceImage((const float*)0, RectI(), nComps);
            // Without a source the OpenFX plug-in samples through a matrix that maps everything
            // nowhere, which renders zeros.
            Mat3 none;
            none(2, 2) = 1.;
            job.transforms.invTransforms.push_back(none);
        }
        job.params = Resampler::makeResampleParams(job.transforms, filter, clamp, blackOutside);
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(TransformBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    QThread* const callingThread = QThread::currentThread();
    std::atomic<bool> wasAborted(false);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const TransformBand& band = bands[bandIndex];
        const TransformPlaneJob& job = *jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        std::vector<float> resampled((std::size_t)width * nComps);
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

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            Resampler::resampleRow(job.params, job.source, y, roi.x1, roi.x2, &resampled[0]);
            if (!_masked) {
                std::copy(resampled.begin(), resampled.end(), dstPix);
                continue;
            }
            if (doMask) {
                readMaskRow(maskAccess.get(), maskBounds, maskNComps, maskChannel, roi.x1, y, width, &maskRow[0]);
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* tmpPix = &resampled[(std::size_t)i * nComps];
                const float* srcPix = job.source.pixel(roi.x1 + i, y);
                float maskScale = 1.f;
                if (doMask) {
                    maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                }
                const float alpha = maskScale * mix;
                if (alpha == 0.f) {
                    for (int c = 0; c < nComps; ++c) {
                        dstPix[c] = srcPix ? srcPix[c] : 0.f;
                    }
                } else if (alpha == 1.f) {
                    for (int c = 0; c < nComps; ++c) {
                        dstPix[c] = tmpPix[c];
                    }
                } else if (srcPix) {
                    for (int c = 0; c < nComps; ++c) {
                        dstPix[c] = tmpPix[c] * alpha + (1.f - alpha) * srcPix[c];
                    }
                } else {
                    for (int c = 0; c < nComps; ++c) {
                        dstPix[c] = tmpPix[c] * alpha;
                    }
                }
            }
        }
    };
    parallelForOnGlobalPool((int)bands.size(), nThreads, renderBand);

    return eStatusOK;
} // TransformNode::render

NATRON_NAMESPACE_EXIT
