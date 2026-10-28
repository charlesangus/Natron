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

#ifndef Engine_Nodes_Scene_USDProjectUV_h
#define Engine_Nodes_Scene_USDProjectUV_h

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

#define PLUGINID_NATRON_USDPROJECTUV "USDProjectUV"

#define kUSDProjectUVParamCamera "camera"
#define kUSDProjectUVParamPrimPattern "primPattern"
#define kUSDProjectUVParamProjection "projection"
#define kUSDProjectUVParamPlane "plane"
#define kUSDProjectUVParamProjectOn "projectOn"
#define kUSDProjectUVParamFrustumCulling "frustumCulling"
#define kUSDProjectUVParamInvertU "invertU"
#define kUSDProjectUVParamInvertV "invertV"
#define kUSDProjectUVParamUScale "uScale"
#define kUSDProjectUVParamVScale "vScale"
#define kUSDProjectUVParamPrimvarName "primvarName"
#define kUSDProjectUVParamFitMode "fitMode"
#define kUSDProjectUVParamUseFrameHold "useFrameHold"
#define kUSDProjectUVParamFrameHold "frameHold"

NATRON_NAMESPACE_ENTER

/**
 * @brief Writes texture coordinates on the meshes a prim pattern names in its scene by projecting
 * their points through a camera, as Nuke's UVProject does: in Perspective mode a point's UV is
 * where it lands in the camera's aperture, conformed to the Format by Fit Mode, (0, 0) at the
 * bottom left and (1, 1) at the top right. Planar, Spherical and Cylindrical map the points in
 * the camera's space. The result is a faceVarying primvar (st by default) indexed per face
 * vertex, time-sampled over the project range when a mesh or the camera moves.
 *
 * The camera comes from the Camera input, read where it is wired, or from the scene itself when
 * that input is not connected.
 **/
class USDProjectUV
    : public NativeEffectBase {
public:
    enum ProjectionEnum {
        eProjectionOff = 0,
        eProjectionPerspective,
        eProjectionPlanar,
        eProjectionSpherical,
        eProjectionCylindrical
    };

    enum PlaneEnum {
        ePlaneXY = 0,
        ePlaneYZ,
        ePlaneZX
    };

    enum ProjectOnEnum {
        eProjectOnBoth = 0,
        eProjectOnFront,
        eProjectOnBack
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
        InputSpec { .label = "Camera", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDPROJECTUV,
        .label = "USDProjectUV",
        .description = "Write texture coordinates on the meshes a pattern names (paths, with * and ? wildcards) in the "
                       "input scene by projecting their points through a camera, as Nuke's UVProject does. The camera "
                       "comes from the Camera input, or from the scene when that input is not connected. The UVs are a "
                       "faceVarying primvar (st by default) that any renderer reads; texture them with a USDMaterial. "
                       "They are per vertex, so a projection onto coarse geometry warps between vertices. A moving "
                       "mesh or camera writes time samples over the project range.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDProjectUV(node);
    }

    explicit USDProjectUV(NodePtr node);

    virtual ~USDProjectUV();

    /**
     * @brief Fills the camera menu with every camera of the camera scene (the Camera input's, else
     * the Scene input's) at the current frame, keeping the chosen one even when the scene no
     * longer has it. Main thread only; the node calls it itself, queued, whenever an input or
     * anything upstream changes.
     **/
    void refreshCameraMenu();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleCameraMenuRefresh();

    // Greys out the knobs the chosen projection does not read, as Nuke does.
    void refreshProjectionKnobs();

    // The format the UVs map the camera's aperture to, as a width over height in square pixels.
    double getFormatAspect() const;

    // Defined where USD is included.
    struct Settings;
    struct Result;
    struct Implementation;
    std::unique_ptr<Implementation> _imp;

    KnobChoiceWPtr _camera;
    KnobStringWPtr _primPattern;
    KnobChoiceWPtr _projection;
    KnobChoiceWPtr _plane;
    KnobChoiceWPtr _projectOn;
    KnobBoolWPtr _frustumCulling;
    KnobBoolWPtr _invertU;
    KnobBoolWPtr _invertV;
    KnobDoubleWPtr _uScale;
    KnobDoubleWPtr _vScale;
    KnobStringWPtr _primvarName;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobChoiceWPtr _fitMode;
    KnobBoolWPtr _useFrameHold;
    KnobDoubleWPtr _frameHold;
    std::atomic<bool> _cameraMenuRefreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDProjectUV_h
