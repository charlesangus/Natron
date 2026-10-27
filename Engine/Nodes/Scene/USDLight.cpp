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

#include "USDLight.h"

#include <functional>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
KnobDoublePtr
createScalarKnob(USDLight* holder,
                 const KnobPagePtr& page,
                 const QString& label,
                 const char* name,
                 const QString& toolTip,
                 double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

std::function<float(double)>
floatAt(const KnobDoublePtr& knob)
{
    return [knob](double frame) {
        return (float)knob->getValueAtTime(frame, 0);
    };
}
} // anonymous namespace

USDLight::USDLight(NodePtr node)
    : NativeEffectBase(node)
{
}

USDLight::~USDLight()
{
}

const NodeSpec*
USDLight::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDLight::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Light"));
}

void
USDLight::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Light"), tr("The path of the light prim the node authors."));

    KnobChoicePtr lightType = createKnob<KnobChoice>(tr("Type"));
    lightType->setName(kUSDLightParamType);
    lightType->setHintToolTip(tr("The UsdLux light the node authors."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("distant", tr("Distant").toStdString(), tr("A light from infinitely far away along the prim's -Z axis (DistantLight).").toStdString()));
        options.push_back(ChoiceOption("sphere", tr("Sphere").toStdString(), tr("A spherical area light (SphereLight).").toStdString()));
        options.push_back(ChoiceOption("rect", tr("Rect").toStdString(), tr("A rectangular area light facing the prim's -Z axis (RectLight).").toStdString()));
        options.push_back(ChoiceOption("dome", tr("Dome").toStdString(), tr("Light from a sphere at infinity, optionally textured with an environment map (DomeLight).").toStdString()));
        lightType->populateChoices(options);
    }
    lightType->setDefaultValue((int)eLightTypeDistant);
    lightType->setAnimationEnabled(false);
    page->addKnob(lightType);
    _lightType = lightType;

    KnobColorPtr color = createKnob<KnobColor>(tr("Color"), 3);
    color->setName(kUSDLightParamColor);
    color->setHintToolTip(tr("The light's colour (inputs:color), in the project working space, written unchanged."));
    for (int d = 0; d < 3; ++d) {
        color->setDefaultValue(1., d);
    }
    page->addKnob(color);
    _color = color;

    _intensity = createScalarKnob(this, page, tr("Intensity"), kUSDLightParamIntensity,
                                  tr("Scales the light's brightness linearly (inputs:intensity)."), 1.);
    _exposure = createScalarKnob(this, page, tr("Exposure"), kUSDLightParamExposure,
                                 tr("Scales the light's brightness by 2 to the power of the exposure (inputs:exposure)."), 0.);

    KnobBoolPtr normalize = createKnob<KnobBool>(tr("Normalize"));
    normalize->setName(kUSDLightParamNormalize);
    normalize->setHintToolTip(tr("Divides the light's power by its area (for a distant light, its angular size), so resizing it keeps its "
                                 "brightness and Intensity alone sets it (inputs:normalize). Off, the power grows with the area: a "
                                 "distant light of the default 0.53 degrees then gives about 7e-5 of its Intensity."));
    normalize->setDefaultValue(true);
    page->addKnob(normalize);
    _normalize = normalize;

    _angle = createScalarKnob(this, page, tr("Angle"), kUSDLightParamAngle,
                              tr("The angular diameter of a distant light, in degrees (inputs:angle)."), 0.53);
    _radius = createScalarKnob(this, page, tr("Radius"), kUSDLightParamRadius,
                               tr("The radius of a sphere light, in scene units (inputs:radius)."), 0.5);
    _width = createScalarKnob(this, page, tr("Width"), kUSDLightParamWidth,
                              tr("The width of a rect light along its X axis, in scene units (inputs:width)."), 1.);
    _height = createScalarKnob(this, page, tr("Height"), kUSDLightParamHeight,
                               tr("The height of a rect light along its Y axis, in scene units (inputs:height)."), 1.);

    KnobFilePtr textureFile = createKnob<KnobFile>(tr("Texture File"));
    textureFile->setName(kUSDLightParamTextureFile);
    textureFile->setHintToolTip(tr("The environment map of a dome light (inputs:texture:file). Authored even when it does not "
                                   "resolve here, since it may resolve on the farm or through the pipeline's resolver; "
                                   "the node then warns."));
    textureFile->setAnimationEnabled(false);
    page->addKnob(textureFile);
    _textureFile = textureFile;

    _transform.create(this, page);

    refreshKnobVisibility();
} // USDLight::initializeKnobs

USDLight::LightTypeEnum
USDLight::getLightType() const
{
    KnobChoicePtr lightType = _lightType.lock();
    const int value = lightType ? lightType->getValue() : (int)eLightTypeDistant;

    if ((value < (int)eLightTypeDistant) || (value > (int)eLightTypeDome)) {
        return eLightTypeDistant;
    }

    return (LightTypeEnum)value;
}

void
USDLight::refreshKnobVisibility()
{
    const LightTypeEnum type = getLightType();

    setSceneKnobSecret(_angle.lock(), type != eLightTypeDistant);
    setSceneKnobSecret(_radius.lock(), type != eLightTypeSphere);
    setSceneKnobSecret(_width.lock(), type != eLightTypeRect);
    setSceneKnobSecret(_height.lock(), type != eLightTypeRect);
    setSceneKnobSecret(_textureFile.lock(), type != eLightTypeDome);
}

bool
USDLight::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    KnobChoicePtr lightType = _lightType.lock();

    if (!lightType || (k != lightType.get())) {
        return false;
    }
    refreshKnobVisibility();

    return true;
}

void
USDLight::onKnobsLoaded()
{
    refreshKnobVisibility();
}

std::string
USDLight::getTexturePath() const
{
    return SceneNodeAuthoring::getAnchoredFilePath(_textureFile.lock());
}

bool
USDLight::authorLayer(const SceneLayerEdit& edit,
                      std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
    }
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }

    KnobColorPtr color = _color.lock();
    KnobDoublePtr intensity = _intensity.lock();
    KnobDoublePtr exposure = _exposure.lock();
    KnobBoolPtr normalize = _normalize.lock();
    KnobDoublePtr angle = _angle.lock();
    KnobDoublePtr radius = _radius.lock();
    KnobDoublePtr width = _width.lock();
    KnobDoublePtr height = _height.lock();
    if (!color || !intensity || !exposure || !normalize || !angle || !radius || !width || !height) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }

    using SceneNodeAuthoring::authorValue;
    using SceneNodeAuthoring::isAnimated;
    const LightTypeEnum type = getLightType();
    PXR_NS::UsdPrim prim;
    bool authored = true;
    switch (type) {
    case eLightTypeDistant: {
        PXR_NS::UsdLuxDistantLight light = PXR_NS::UsdLuxDistantLight::Define(stage, path);
        prim = light.GetPrim();
        authored = light && authorValue(light.CreateAngleAttr(), edit, isAnimated(angle), floatAt(angle));
        break;
    }
    case eLightTypeSphere: {
        PXR_NS::UsdLuxSphereLight light = PXR_NS::UsdLuxSphereLight::Define(stage, path);
        prim = light.GetPrim();
        authored = light && authorValue(light.CreateRadiusAttr(), edit, isAnimated(radius), floatAt(radius));
        break;
    }
    case eLightTypeRect: {
        PXR_NS::UsdLuxRectLight light = PXR_NS::UsdLuxRectLight::Define(stage, path);
        prim = light.GetPrim();
        authored = light
            && authorValue(light.CreateWidthAttr(), edit, isAnimated(width), floatAt(width))
            && authorValue(light.CreateHeightAttr(), edit, isAnimated(height), floatAt(height));
        break;
    }
    case eLightTypeDome: {
        PXR_NS::UsdLuxDomeLight light = PXR_NS::UsdLuxDomeLight::Define(stage, path);
        prim = light.GetPrim();
        const std::string texturePath = getTexturePath();
        authored = bool(light);
        if (authored && !texturePath.empty()) {
            authored = light.CreateTextureFileAttr().Set(PXR_NS::SdfAssetPath(texturePath));
        }
        break;
    }
    }
    if (!prim) {
        *errorMessage = tr("USD could not define a light at %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return false;
    }

    const PXR_NS::UsdLuxLightAPI light(prim);
    authored = authored
        && authorValue(light.CreateColorAttr(), edit, isAnimated(color), [&color](double frame) {
                   return PXR_NS::GfVec3f((float)color->getValueAtTime(frame, 0), (float)color->getValueAtTime(frame, 1), (float)color->getValueAtTime(frame, 2));
               })
        && authorValue(light.CreateIntensityAttr(), edit, isAnimated(intensity), floatAt(intensity)) && authorValue(light.CreateExposureAttr(), edit, isAnimated(exposure), floatAt(exposure)) && authorValue(light.CreateNormalizeAttr(), edit, isAnimated(normalize), [&normalize](double frame) {
                   return normalize->getValueAtTime(frame, 0);
               });
    if (!authored) {
        *errorMessage = tr("USD could not author the light at %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return false;
    }

    return SceneNodeAuthoring::authorTransform(_transform, PXR_NS::UsdGeomXformable(prim), edit, errorMessage);
} // USDLight::authorLayer

StatusEnum
USDLight::renderScene(const SceneRenderActionArgs& /*args*/,
                      ScenePayload* outputScene,
                      std::string* errorMessage)
{
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    if (!emitSceneLayer(ScenePayload(), reads, [this](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // Checked on every evaluation, not only when the layer is authored: the file can appear or
    // vanish while the layer stays the same.
    const std::string texturePath = (getLightType() == eLightTypeDome) ? getTexturePath() : std::string();
    if (!texturePath.empty() && PXR_NS::ArGetResolver().Resolve(texturePath).IsEmpty()) {
        setPersistentMessage(eMessageTypeWarning, tr("The dome texture %1 does not resolve; it is authored anyway, and the light renders untextured "
                                                     "wherever it does not resolve.")
                                                      .arg(QString::fromStdString(texturePath))
                                                      .toStdString());
    } else {
        clearPersistentMessage(false);
    }

    return eStatusOK;
} // USDLight::renderScene

NATRON_NAMESPACE_EXIT
