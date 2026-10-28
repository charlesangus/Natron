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

#ifndef Engine_Nodes_Scene_USDProject3D_h
#define Engine_Nodes_Scene_USDProject3D_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDPROJECT3D "USDProject3D"

#define kUSDProject3DParamCamera "camera"
#define kUSDProject3DParamProjectOn "projectOn"
#define kUSDProject3DParamCrop "crop"
#define kUSDProject3DParamClipNearFar "clipNearFar"
#define kUSDProject3DParamOutside "outside"
#define kUSDProject3DParamFitMode "fitMode"
#define kUSDProject3DParamUseFrameHold "useFrameHold"
#define kUSDProject3DParamFrameHold "frameHold"

#define kUSDProject3DInputImage 0
#define kUSDProject3DInputCamera 1

// The glslfx the material's surface runs, installed beside the live-texture plugInfo.json.
#define kUSDProject3DShaderFile "natronProject3D.glslfx"
// The names of the prims the node authors under the material.
#define kUSDProject3DShaderName "Projection"
#define kUSDProject3DTextureName "PlateTexture"
#define kUSDProject3DReaderName "TexCoordReader"

NATRON_NAMESPACE_ENTER

class USDProject3D
    : public NativeEffectBase {
public:
    enum ProjectOnEnum {
        eProjectOnBoth = 0,
        eProjectOnFront,
        eProjectOnBack
    };

    enum OutsideEnum {
        eOutsideBlack = 0,
        eOutsideTransparent
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Image", .kind = eDataKindImage, .role = InputRole::Aux, .optional = true, .time = InputTime::Any },
        InputSpec { .label = "Camera", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDPROJECT3D,
        .label = "USDProject3D",
        .description = "Author a material that projects the Image input through a camera, as Nuke's Project3D does: each "
                       "point of the surface shows the image where it lands in the camera's frame, per pixel and "
                       "perspective-correct, unlit. Connect it to the Material input of a geometry node. The camera "
                       "comes from the Camera input and is stored in the material as values, so the projection stays "
                       "where the camera is while the geometry moves through it. The material is a Storm shader: it "
                       "shows in the Viewer and USDRender; USDWrite bakes it to texture coordinates and a "
                       "UsdPreviewSurface for other renderers.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDProject3D(node);
    }

    explicit USDProject3D(NodePtr node);

    virtual ~USDProject3D();

    /**
     * @brief Fills the camera menu with every camera of the Camera input's scene at the current
     * frame, keeping the chosen one even when the scene no longer has it. Main thread only; the
     * node calls it itself, queued, whenever an input or anything upstream changes.
     **/
    void refreshCameraMenu();

    /**
     * @brief The installed natronProject3D.glslfx the material's surface names, absolute; empty
     * when the installation lacks it.
     **/
    static std::string getShaderPath();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleCameraMenuRefresh();

    // The Image input's format as a width over height in square pixels, the project's without one.
    double getPlateAspect(double time) const;

    // Defined where USD is included.
    struct CameraSamples;
    struct Implementation;
    std::unique_ptr<Implementation> _imp;

    KnobStringWPtr _primPath;
    KnobChoiceWPtr _camera;
    KnobChoiceWPtr _projectOn;
    KnobBoolWPtr _crop;
    KnobBoolWPtr _clipNearFar;
    KnobChoiceWPtr _outside;
    KnobChoiceWPtr _fitMode;
    KnobBoolWPtr _useFrameHold;
    KnobDoubleWPtr _frameHold;
    std::atomic<bool> _cameraMenuRefreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDProject3D_h
