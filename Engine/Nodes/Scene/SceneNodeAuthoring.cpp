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

#include "SceneNodeAuthoring.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

void
setSceneKnobSecret(const KnobIPtr& knob,
                   bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

void
SceneTransformKnobs::create(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Translate"), 3);
    translateKnob->setName(kSceneParamTranslate);
    translateKnob->setHintToolTip(EffectInstance::tr("The prim's position in its parent's space, in scene units."));
    for (int d = 0; d < 3; ++d) {
        translateKnob->setDefaultValue(0., d);
    }
    page->addKnob(translateKnob);
    translate = translateKnob;

    KnobDoublePtr rotateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Rotate"), 3);
    rotateKnob->setName(kSceneParamRotate);
    rotateKnob->setHintToolTip(EffectInstance::tr("Rotations about the X, Y and Z axes, in degrees, applied in the rotate order."));
    for (int d = 0; d < 3; ++d) {
        rotateKnob->setDefaultValue(0., d);
    }
    page->addKnob(rotateKnob);
    rotate = rotateKnob;

    KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Scale"), 3);
    scaleKnob->setName(kSceneParamScale);
    scaleKnob->setHintToolTip(EffectInstance::tr("Scale along the X, Y and Z axes, multiplied by the uniform scale."));
    for (int d = 0; d < 3; ++d) {
        scaleKnob->setDefaultValue(1., d);
    }
    page->addKnob(scaleKnob);
    scale = scaleKnob;

    KnobDoublePtr uniformScaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Uniform Scale"));
    uniformScaleKnob->setName(kSceneParamUniformScale);
    uniformScaleKnob->setHintToolTip(EffectInstance::tr("Scale along every axis, multiplied into the scale."));
    uniformScaleKnob->setDefaultValue(1.);
    page->addKnob(uniformScaleKnob);
    uniformScale = uniformScaleKnob;

    KnobChoicePtr rotateOrderKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Rotate Order"));
    rotateOrderKnob->setName(kSceneParamRotateOrder);
    rotateOrderKnob->setHintToolTip(EffectInstance::tr("The order the three rotations are applied in, as USD names its rotate ops: "
                                                       "XYZ turns about X first, then Y, then Z."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XYZ", "XYZ", ""));
        options.push_back(ChoiceOption("XZY", "XZY", ""));
        options.push_back(ChoiceOption("YXZ", "YXZ", ""));
        options.push_back(ChoiceOption("YZX", "YZX", ""));
        options.push_back(ChoiceOption("ZXY", "ZXY", ""));
        options.push_back(ChoiceOption("ZYX", "ZYX", ""));
        rotateOrderKnob->populateChoices(options);
    }
    rotateOrderKnob->setDefaultValue((int)eSceneRotateOrderXYZ);
    rotateOrderKnob->setAnimationEnabled(false);
    page->addKnob(rotateOrderKnob);
    rotateOrder = rotateOrderKnob;
} // SceneTransformKnobs::create

void
SceneTransformKnobs::setSecret(bool secret) const
{
    setSceneKnobSecret(translate.lock(), secret);
    setSceneKnobSecret(rotate.lock(), secret);
    setSceneKnobSecret(scale.lock(), secret);
    setSceneKnobSecret(uniformScale.lock(), secret);
    setSceneKnobSecret(rotateOrder.lock(), secret);
}

KnobStringPtr
createScenePrimPathKnob(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const std::string& defaultPath,
                        const QString& toolTip)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, EffectInstance::tr("Prim Path"));

    knob->setName(kSceneParamPrimPath);
    knob->setHintToolTip(toolTip);
    knob->setAnimationEnabled(false);
    knob->setDefaultValue(defaultPath);
    page->addKnob(knob);

    return knob;
}

namespace SceneNodeAuthoring {
double
getTimeCodesPerSecond(const SceneLayerEdit& edit)
{
    if (edit.upstream.isEmpty()) {
        return (edit.framesPerSecond > 0.) ? edit.framesPerSecond : edit.upstream.getMetadata().timeCodesPerSecond;
    }

    return edit.upstream.getMetadata().timeCodesPerSecond;
}

double
getMetersPerUnit(const SceneLayerEdit& edit)
{
    const double metersPerUnit = edit.upstream.getMetadata().metersPerUnit;

    return (metersPerUnit > 0.) ? metersPerUnit : SceneMetadata().metersPerUnit;
}

PXR_NS::UsdStageRefPtr
openLayerStage(const SceneLayerEdit& edit,
               std::string* errorMessage)
{
    if (!edit.layer) {
        *errorMessage = EffectInstance::tr("No layer to author.").toStdString();

        return PXR_NS::UsdStageRefPtr();
    }
    edit.layer->SetTimeCodesPerSecond(getTimeCodesPerSecond(edit));
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(edit.layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
    if (!stage) {
        *errorMessage = EffectInstance::tr("USD could not open a stage on the node's layer.").toStdString();
    }

    return stage;
}

bool
getPrimPath(const KnobStringPtr& knob,
            PXR_NS::SdfPath* path,
            std::string* errorMessage)
{
    const std::string value = knob ? knob->getValue() : std::string();

    if (PXR_NS::SdfPath::IsValidPathString(value)) {
        const PXR_NS::SdfPath candidate(value);
        if (candidate.IsAbsolutePath() && candidate.IsPrimPath()) {
            *path = candidate;

            return true;
        }
    }
    *errorMessage = EffectInstance::tr("\"%1\" is not an absolute prim path such as /World/Prim.").arg(QString::fromStdString(value)).toStdString();

    return false;
}

bool
isAnimated(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    for (int d = 0; d < knob->getDimension(); ++d) {
        if (knob->isAnimated(d) || !knob->getExpression(d).empty()) {
            return true;
        }
    }

    return false;
}

std::vector<double>
getSampleFrames(const SceneLayerEdit& edit)
{
    std::vector<double> frames;
    const double first = std::ceil(edit.firstFrame);
    const double last = std::max(first, std::floor(edit.lastFrame));

    frames.reserve((std::size_t)(last - first) + 1);
    for (double frame = first; frame <= last; frame += 1.) {
        frames.push_back(frame);
    }

    return frames;
}

PXR_NS::UsdTimeCode
frameToTimeCode(const SceneLayerEdit& edit,
                double frame)
{
    if (edit.framesPerSecond <= 0.) {
        return PXR_NS::UsdTimeCode(frame);
    }

    return PXR_NS::UsdTimeCode(frame * getTimeCodesPerSecond(edit) / edit.framesPerSecond);
}

bool
authorTransform(const SceneTransformKnobs& knobs,
                const PXR_NS::UsdGeomXformable& xformable,
                const SceneLayerEdit& edit,
                std::string* errorMessage)
{
    static const PXR_NS::UsdGeomXformOp::Type kRotateTypes[] = {
        PXR_NS::UsdGeomXformOp::TypeRotateXYZ,
        PXR_NS::UsdGeomXformOp::TypeRotateXZY,
        PXR_NS::UsdGeomXformOp::TypeRotateYXZ,
        PXR_NS::UsdGeomXformOp::TypeRotateYZX,
        PXR_NS::UsdGeomXformOp::TypeRotateZXY,
        PXR_NS::UsdGeomXformOp::TypeRotateZYX,
    };
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();

    if (!xformable || !translate || !rotate || !scale || !uniformScale || !rotateOrder) {
        *errorMessage = EffectInstance::tr("The prim cannot hold a transform.").toStdString();

        return false;
    }
    int order = rotateOrder->getValue();
    if ((order < 0) || (order >= (int)(sizeof(kRotateTypes) / sizeof(kRotateTypes[0])))) {
        order = (int)eSceneRotateOrderXYZ;
    }

    const PXR_NS::UsdGeomXformOp translateOp = xformable.AddTranslateOp(PXR_NS::UsdGeomXformOp::PrecisionDouble);
    const PXR_NS::UsdGeomXformOp rotateOp = xformable.AddXformOp(kRotateTypes[order], PXR_NS::UsdGeomXformOp::PrecisionFloat);
    const PXR_NS::UsdGeomXformOp scaleOp = xformable.AddScaleOp(PXR_NS::UsdGeomXformOp::PrecisionFloat);
    const bool authored = translateOp && rotateOp && scaleOp
        && authorValue(translateOp.GetAttr(), edit, isAnimated(translate), [&translate](double frame) {
                              return PXR_NS::GfVec3d(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1), translate->getValueAtTime(frame, 2));
                          })
        && authorValue(rotateOp.GetAttr(), edit, isAnimated(rotate), [&rotate](double frame) {
                              return PXR_NS::GfVec3f((float)rotate->getValueAtTime(frame, 0), (float)rotate->getValueAtTime(frame, 1), (float)rotate->getValueAtTime(frame, 2));
                          })
        && authorValue(scaleOp.GetAttr(), edit, isAnimated(scale) || isAnimated(uniformScale), [&scale, &uniformScale](double frame) {
                              const double uniform = uniformScale->getValueAtTime(frame, 0);

                              return PXR_NS::GfVec3f((float)(scale->getValueAtTime(frame, 0) * uniform), (float)(scale->getValueAtTime(frame, 1) * uniform), (float)(scale->getValueAtTime(frame, 2) * uniform));
                          });
    if (!authored) {
        *errorMessage = EffectInstance::tr("USD could not author the transform of %1.").arg(QString::fromStdString(xformable.GetPath().GetString())).toStdString();

        return false;
    }

    return true;
} // authorTransform
} // namespace SceneNodeAuthoring

NATRON_NAMESPACE_EXIT
