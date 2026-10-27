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

#include "ITransform.h"

#include <string>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {
constexpr NodeSpec kITransformNodeSpec = {
    .id = PLUGINID_NATRON_ITRANSFORM,
    .label = "ITransform",
    .description = "Translate / Rotate / Scale a 2D image by an amount that varies per pixel with the Map input.\n"
                   "\n"
                   "Each output pixel takes the transform at its Map value: none where the Map is black, the whole "
                   "transform where it is white, and in between the transform Amount would give, translate, rotate and "
                   "skew scaled linearly and scale geometrically. The source is resampled once per pixel, so a soft "
                   "Map bends the image rather than mixing two copies of it; a hard Map edge tears the image along it. "
                   "Map Channel picks the Map's channel in any of its layers, Invert Map inverts it, Map Gamma raises "
                   "it to 1/gamma and Map Response picks a Linear or Smooth (smoothstep) curve. The Map is read at the "
                   "output pixel; outside the Map image it reads black. With no Map connected the whole transform "
                   "applies everywhere, as in Transform.\n"
                   "\n"
                   "Motion blur averages the warp over the shutter, or with Directional Blur over the transform from "
                   "none to the pixel's. This node does not concatenate transforms.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_ITRANSFORM,
    .icon = NATRON_IMAGES_PATH "NativeNodes/ITransform.png",
    .inputs = kIWarpInputs,
};
} // anonymous namespace

ITransform::ITransform(NodePtr node)
    : IWarp(node)
{
}

ITransform::~ITransform()
{
}

const NodeSpec*
ITransform::nodeSpec() const
{
    return staticNodeSpec<kITransformNodeSpec>();
}

const ImagePipelineSpec&
ITransform::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kIWarpPipeline, kITransformNodeSpec>();
}

std::string
ITransform::mapNotConnectedMessage() const
{
    return tr("The Map input is not connected: the whole transform applies everywhere.").toStdString();
}

void
ITransform::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    _op.createKnobs(static_cast<KnobHolder*>(this), page);
    createMapKnobs(page);
    addStageKnobs(page);

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
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));

    NodePtr node = getNode();
    if (node) {
        node->addTransformInteract(_op.translate.lock(), _op.scale.lock(), _op.uniform.lock(), _op.rotate.lock(), _op.skewX.lock(), _op.skewY.lock(), _op.skewOrder.lock(), _op.center.lock(), _op.invert.lock(), _op.interactive.lock());
    }
    declareMapLayerKnob();
} // ITransform::initializeKnobs

void
ITransform::updateShutterEnabled()
{
    KnobBoolPtr directionalBlur = _op.directionalBlur.lock();

    if (!directionalBlur) {
        return;
    }
    const bool enabled = !directionalBlur->getValue();
    KnobDoublePtr shutter = _op.shutter.lock();
    KnobChoicePtr shutterOffset = _op.shutterOffset.lock();
    KnobDoublePtr shutterCustomOffset = _op.shutterCustomOffset.lock();
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
ITransform::updateClampEnabled(double time,
                               ViewSpec view)
{
    KnobChoicePtr filter = _op.filter.lock();
    KnobBoolPtr clamp = _op.clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
ITransform::updateUniformVisibility(bool allowHiding)
{
    KnobBoolPtr uniform = _op.uniform.lock();

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
ITransform::onKnobsLoaded()
{
    updateShutterEnabled();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
    updateUniformVisibility(true);
}

void
ITransform::onInputChanged(int inputNo)
{
    IWarp::onInputChanged(inputNo);

    KnobBoolPtr centerChanged = _op.centerChanged.lock();
    if ((inputNo != kIWarpInputSource) || !getInput(kIWarpInputSource) || !centerChanged || centerChanged->getValue()) {
        return;
    }
    resetCenter(getApp()->getTimeLine()->currentFrame());
}

bool
ITransform::knobChanged(KnobI* k,
                        ValueChangedReasonEnum reason,
                        ViewSpec view,
                        double time,
                        bool /*originatedFromMainThread*/)
{
    KnobButtonPtr resetCenterButton = _op.resetCenter.lock();
    KnobBoolPtr centerChanged = _op.centerChanged.lock();
    if (resetCenterButton && (k == resetCenterButton.get())) {
        resetCenter(time);
        if (centerChanged && centerChanged->getValue()) {
            centerChanged->setValue(false);
        }

        return true;
    }
    KnobDoublePtr center = _op.center.lock();
    if (center && (k == center.get())) {
        const bool edited = (reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonPluginEdited) || (reason == eValueChangedReasonNatronGuiEdited) || (reason == eValueChangedReasonNatronInternalEdited);
        if (edited && centerChanged && !centerChanged->getValue()) {
            centerChanged->setValue(true);
        }

        return true;
    }
    KnobChoicePtr filterKnob = _op.filter.lock();
    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }
    KnobBoolPtr directionalBlur = _op.directionalBlur.lock();
    if (directionalBlur && (k == directionalBlur.get())) {
        updateShutterEnabled();

        return true;
    }
    KnobBoolPtr uniform = _op.uniform.lock();
    if (uniform && (k == uniform.get())) {
        if (reason != eValueChangedReasonTimeChanged) {
            updateUniformVisibility(false);
        }

        return true;
    }

    return false;
} // ITransform::knobChanged

void
ITransform::resetCenter(double time)
{
    EffectInstancePtr input = getInput(kIWarpInputSource);

    if (!_op.center.lock() || !input) {
        return;
    }
    RectD rod;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
        return;
    }
    if (rod.isInfinite()) {
        return;
    }
    if (rod.isNull()) {
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectI pixel(format.x1, format.y1, format.x2, format.y2);
        rod = pixel.toCanonical_noClipping(0, format.getPixelAspectRatio());
    }

    resetTransformCenter(this, _op, rod, time);
}

void
ITransform::readWarpSettings(double time,
                             ViewIdx view,
                             WarpSettings* settings) const
{
    const TransformNodeParams params = bakeParams(_op, time, view);

    settings->filter = params.filter;
    settings->clamp = params.clamp;
    settings->blackOutside = params.blackOutside;
    settings->invert = params.invert;
    settings->blur = params.blur;
    // As Transform renders it: a directional blur has no shutter.
    if (settings->blur.directionalBlur) {
        settings->blur.shutter = 0.;
    }
}

TransformInterp::MatrixFn
ITransform::canonicalMatrixFn(double time,
                              ViewIdx view,
                              bool invert) const
{
    const TransformMath::TransformParams p = bakeParams(_op, time, view).transform;

    return [p, invert](double m, TransformMath::Mat3* matrix) {
        *matrix = TransformInterp::transformAt(p, m, invert);

        return true;
    };
}

bool
ITransform::isTransformIdentity(double time,
                                ViewIdx view) const
{
    return TransformMath::isIdentity(bakeParams(_op, time, view).transform);
}

int
ITransform::curveSegments(double time,
                          ViewIdx view) const
{
    return TransformInterp::segmentsForTransform(bakeParams(_op, time, view).transform);
}

NATRON_NAMESPACE_EXIT
