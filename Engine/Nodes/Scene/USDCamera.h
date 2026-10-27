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

#ifndef Engine_Nodes_Scene_USDCamera_h
#define Engine_Nodes_Scene_USDCamera_h

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

#define PLUGINID_NATRON_USDCAMERA "USDCamera"

#define kUSDCameraParamProjection "projection"
#define kUSDCameraParamFocalLength "focalLength"
#define kUSDCameraParamHorizontalAperture "horizontalAperture"
#define kUSDCameraParamVerticalAperture "verticalAperture"
#define kUSDCameraParamHorizontalApertureOffset "horizontalApertureOffset"
#define kUSDCameraParamVerticalApertureOffset "verticalApertureOffset"
#define kUSDCameraParamNear "near"
#define kUSDCameraParamFar "far"
#define kUSDCameraParamFStop "fStop"
#define kUSDCameraParamFocusDistance "focusDistance"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Authors a UsdGeomCamera prim, as a scene source.
 *
 * Lens and aperture knobs are in millimetres and are written in USD's camera units, tenths of a
 * scene unit, so they read as millimetres in a centimetre scene. Clipping and focus distance are
 * in scene units. The aperture is not tied to the project format: whatever renders through the
 * camera conforms it to its own format.
 **/
class USDCamera
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDCAMERA,
        .label = "USDCamera",
        .description = "Create a scene holding one camera prim (UsdGeomCamera). "
                       "Focal length and aperture are in millimetres, written in USD's camera units (tenths of a scene unit); "
                       "clipping and focus distance are in scene units. Animated knobs are written as time samples.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDCamera.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDCamera(node);
    }

    explicit USDCamera(NodePtr node);

    virtual ~USDCamera();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool authorLayer(const SceneLayerEdit& edit,
                     std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    SceneTransformKnobs _transform;
    KnobChoiceWPtr _projection;
    KnobDoubleWPtr _focalLength;
    KnobDoubleWPtr _horizontalAperture;
    KnobDoubleWPtr _verticalAperture;
    KnobDoubleWPtr _horizontalApertureOffset;
    KnobDoubleWPtr _verticalApertureOffset;
    KnobDoubleWPtr _near;
    KnobDoubleWPtr _far;
    KnobDoubleWPtr _fStop;
    KnobDoubleWPtr _focusDistance;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDCamera_h
