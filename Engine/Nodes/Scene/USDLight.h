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

#ifndef Engine_Nodes_Scene_USDLight_h
#define Engine_Nodes_Scene_USDLight_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDLIGHT "USDLight"

#define kUSDLightParamType "lightType"
#define kUSDLightParamColor "color"
#define kUSDLightParamIntensity "intensity"
#define kUSDLightParamExposure "exposure"
#define kUSDLightParamNormalize "normalize"
#define kUSDLightParamAngle "angle"
#define kUSDLightParamRadius "radius"
#define kUSDLightParamWidth "width"
#define kUSDLightParamHeight "height"
#define kUSDLightParamTextureFile "textureFile"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Authors a UsdLux light prim -- distant, sphere, rect or dome -- on top of its optional
 * scene input or as a scene source. Colour, intensity, exposure and normalize are the light's
 * UsdLux inputs, written unchanged: the colour is in the project working space, with no
 * transform. A dome's texture path is authored whether or not it resolves (it may on the farm or
 * through a pipeline resolver); one that does not resolve here sets a node warning naming it.
 **/
class USDLight
    : public NativeEffectBase {
public:
    enum LightTypeEnum {
        eLightTypeDistant = 0,
        eLightTypeSphere,
        eLightTypeRect,
        eLightTypeDome
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDLIGHT,
        .label = "USDLight",
        .description = "Create a UsdLux light (distant, sphere, rect or dome) on top of the input scene, or as a scene of its own. "
                       "Colour, intensity and exposure are written to the light's inputs unchanged; the colour is in the project "
                       "working space. A dome takes a texture file, authored even when it does not resolve, with a warning. "
                       "Animated knobs are written as time samples.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDLight.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDLight(node);
    }

    explicit USDLight(NodePtr node);

    virtual ~USDLight();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshKnobVisibility();

    LightTypeEnum getLightType() const;

    // The dome texture the knob names, empty for none.
    std::string getTexturePath() const;

    bool authorLayer(const SceneLayerEdit& edit,
                     std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    KnobChoiceWPtr _lightType;
    KnobColorWPtr _color;
    KnobDoubleWPtr _intensity;
    KnobDoubleWPtr _exposure;
    KnobBoolWPtr _normalize;
    KnobDoubleWPtr _angle;
    KnobDoubleWPtr _radius;
    KnobDoubleWPtr _width;
    KnobDoubleWPtr _height;
    KnobFileWPtr _textureFile;
    SceneTransformKnobs _transform;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDLight_h
