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

#include "ICornerPin.h"

#include <string>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {
constexpr NodeSpec kICornerPinNodeSpec = {
    .id = PLUGINID_NATRON_ICORNERPIN,
    .label = "ICornerPin",
    .description = "Warps the image so that the \"from\" points land on the \"to\" points by an amount that varies per pixel with the Map input.\n"
                   "\n"
                   "Each output pixel takes the pin at its Map value: the \"to\" points blended from the \"from\" points by it, "
                   "as Amount blends them, so the pin is not applied where the Map is black and applied whole where it is white. "
                   "The extra matrix blends from the identity by the Map value in the same way. The source is resampled once per pixel, so a soft Map bends the image "
                   "rather than mixing two copies of it; a hard Map edge tears the image along it. Map Channel picks the Map's "
                   "channel in any of its layers, Invert Map inverts it, Map Gamma raises it to 1/gamma and Map Response picks a "
                   "Linear or Smooth (smoothstep) curve. The Map is read at the output pixel; outside the Map image it reads black. "
                   "With no Map connected the whole pin applies everywhere, as in CornerPin.\n"
                   "\n"
                   "Motion blur averages the warp over the shutter. This node does not concatenate transforms.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_ICORNERPIN,
    .icon = NATRON_IMAGES_PATH "NativeNodes/ICornerPin.png",
    .inputs = kIWarpInputs,
};
} // anonymous namespace

ICornerPin::ICornerPin(NodePtr node)
    : IWarp(node)
{
}

ICornerPin::~ICornerPin()
{
}

const NodeSpec*
ICornerPin::nodeSpec() const
{
    return staticNodeSpec<kICornerPinNodeSpec>();
}

const ImagePipelineSpec&
ICornerPin::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kIWarpPipeline, kICornerPinNodeSpec>();
}

std::string
ICornerPin::mapNotConnectedMessage() const
{
    return tr("The Map input is not connected: the whole pin applies everywhere.").toStdString();
}

void
ICornerPin::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    _op.createKnobs(static_cast<KnobHolder*>(this), page);
    createMapKnobs(page);
    addStageKnobs(page);

    KnobBoolPtr srcClipChanged = createKnob<KnobBool>(std::string(kCornerPinNodeParamSrcClipChanged));
    srcClipChanged->setName(kCornerPinNodeParamSrcClipChanged);
    srcClipChanged->setDefaultValue(false);
    srcClipChanged->setSecretByDefault(true);
    srcClipChanged->setDefaultAllDimensionsEnabled(false);
    srcClipChanged->setAnimationEnabled(false);
    srcClipChanged->setEvaluateOnChange(false);
    page->addKnob(srcClipChanged);
    _srcClipChanged = srcClipChanged;

    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));

    NodePtr node = getNode();
    if (node) {
        node->addCornerPinInteract(_op.from[0].lock(), _op.from[1].lock(), _op.from[2].lock(), _op.from[3].lock(),
                                   _op.to[0].lock(), _op.to[1].lock(), _op.to[2].lock(), _op.to[3].lock(),
                                   _op.enable[0].lock(), _op.enable[1].lock(), _op.enable[2].lock(), _op.enable[3].lock(),
                                   _op.overlayPoints.lock(), _op.invert.lock(), KnobBoolPtr());
    }
    declareMapLayerKnob();
} // ICornerPin::initializeKnobs

void
ICornerPin::updateClampEnabled(double time,
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
ICornerPin::onKnobsLoaded()
{
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

void
ICornerPin::onInputChanged(int inputNo)
{
    IWarp::onInputChanged(inputNo);

    KnobBoolPtr srcClipChanged = _srcClipChanged.lock();
    if ((inputNo != kIWarpInputSource) || !getInput(kIWarpInputSource) || !srcClipChanged || srcClipChanged->getValue()) {
        return;
    }
    setFromToInput(getApp()->getTimeLine()->currentFrame(), true);
    if (!srcClipChanged->getValue()) {
        srcClipChanged->setValue(true);
    }
}

void
ICornerPin::setFromToInput(double time,
                           bool alsoTo)
{
    EffectInstancePtr input = getInput(kIWarpInputSource);

    if (!input) {
        return;
    }
    RectD rod;
    bool isProjectFormat = false;
    if ((input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) || rod.isInfinite() || rod.isNull()) {
        return;
    }
    setCornerPinPointsFromRoD(this, _op, rod, alsoTo);
}

void
ICornerPin::markPointsEdited(ValueChangedReasonEnum reason)
{
    KnobBoolPtr srcClipChanged = _srcClipChanged.lock();

    if (isCornerPinPointsEdit(reason) && srcClipChanged && !srcClipChanged->getValue()) {
        srcClipChanged->setValue(true);
    }
}

bool
ICornerPin::knobChanged(KnobI* k,
                        ValueChangedReasonEnum reason,
                        ViewSpec view,
                        double time,
                        bool /*originatedFromMainThread*/)
{
    if (k == _op.setToInput.lock().get()) {
        setFromToInput(time, false);
        markPointsEdited(reason);

        return true;
    }
    if ((k == _op.copyFrom.lock().get()) || (k == _op.copyFromSingle.lock().get()) || (k == _op.copyTo.lock().get()) || (k == _op.copyToSingle.lock().get())) {
        const bool fromToTo = (k == _op.copyFrom.lock().get()) || (k == _op.copyFromSingle.lock().get());
        const bool withAnimation = (k == _op.copyFrom.lock().get()) || (k == _op.copyTo.lock().get());
        pushUndoCommand(makeCornerPinCopyPointsCommand(_op, _srcClipChanged, fromToTo, withAnimation, time, isCornerPinPointsEdit(reason)));

        return true;
    }
    for (int i = 0; i < 4; ++i) {
        if ((k == _op.to[i].lock().get()) || (k == _op.from[i].lock().get()) || (k == _op.enable[i].lock().get())) {
            markPointsEdited(reason);

            return true;
        }
    }
    if (k == _op.extraMatrix.lock().get()) {
        markPointsEdited(reason);

        return true;
    }
    KnobChoicePtr filterKnob = _op.filter.lock();
    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }

    return false;
} // ICornerPin::knobChanged

void
ICornerPin::readWarpSettings(double time,
                             ViewIdx view,
                             WarpSettings* settings) const
{
    const CornerPinNodeParams params = bakeParams(_op, time, view);

    settings->filter = params.filter;
    settings->clamp = params.clamp;
    settings->blackOutside = params.blackOutside;
    settings->invert = params.invert;
    settings->blur = params.blur;
    settings->blur.directionalBlur = false;
}

TransformInterp::MatrixFn
ICornerPin::canonicalMatrixFn(double time,
                              ViewIdx view,
                              bool invert) const
{
    const CornerPinPoints pin = bakeParams(_op, time, view).pin;

    // The extra matrix blends from the identity by m as the points do, so that m = 0 is the
    // identity exactly.
    return [pin, invert](double m, TransformMath::Mat3* matrix) {
        if (m == 0.) {
            *matrix = TransformMath::Mat3::identity();

            return true;
        }
        CornerPinPoints at = pin;
        const TransformMath::Mat3 identity = TransformMath::Mat3::identity();
        for (int k = 0; k < 9; ++k) {
            at.extraMatrix.m[k] = identity.m[k] + m * (pin.extraMatrix.m[k] - identity.m[k]);
        }

        return CornerPinOp::inverseMatrix(at, m, invert, matrix);
    };
}

bool
ICornerPin::isTransformIdentity(double time,
                                ViewIdx view) const
{
    return CornerPinOp::isIdentityPin(bakeParams(_op, time, view).pin);
}

NATRON_NAMESPACE_EXIT
