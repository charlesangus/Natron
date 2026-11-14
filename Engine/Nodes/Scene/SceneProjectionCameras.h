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

#ifndef Engine_Nodes_Scene_SceneProjectionCameras_h
#define Engine_Nodes_Scene_SceneProjectionCameras_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief USDProject3D's camera, read where the material renders. A projector whose camera is not
 * on its Camera input names it by path on its shader (kUSDProject3DAttrCamera); the stage that
 * renders the material resolves that path once it has composed everything, as Hydra reads only
 * sampled values and never a relationship's target.
 **/
namespace SceneProjectionCameras {
// True when the prim's world transform may change over time: its own or one above it.
bool transformMightVary(const PXR_NS::UsdPrim& prim);

// True when the camera's projection may change over time: its attributes or a transform above it.
bool cameraMightVary(const PXR_NS::UsdGeomCamera& camera);

/**
 * @brief The matrix taking world points to the camera's frame, 0..1 across it with w kept, and
 * the point it projects from: its position with w 1, or the direction back to it with w 0 when
 * orthographic.
 **/
bool computeProjector(const PXR_NS::UsdStageRefPtr& stage,
                      const PXR_NS::SdfPath& cameraPath,
                      PXR_NS::UsdTimeCode time,
                      double aspect,
                      HydraRender::FitModeEnum fit,
                      PXR_NS::GfMatrix4d* worldToProjector,
                      PXR_NS::GfVec4f* origin,
                      std::string* errorMessage);

// The customData key, set true, on every shader of the Cycles and MoonRay networks.
#define kSceneProjectionNetworkMark "natronProjectionNetwork"

/**
 * @brief The shaders of the Cycles network USDProject3D authors beside its Storm surface, under
 * the same material, that take values from the projector, the plate's window or the prim.
 * cyclesProjectorDot(k) and cyclesProjectorOffset(k) give the projected point's component k
 * (x, y, z, w) as dot(P, vector2) + value2.
 **/
#define kSceneProjectionCyclesToward "CyclesTowardProjector"
#define kSceneProjectionCyclesPlateTransform "CyclesPlateTransform"
#define kSceneProjectionCyclesPlateTexture "CyclesPlateTexture"
#define kSceneProjectionCyclesReferencePoints "CyclesReferencePoints"
#define kSceneProjectionCyclesReferencePresent "CyclesReferencePresent"
std::string cyclesProjectorDot(int component);
std::string cyclesProjectorOffset(int component);
// The attribute node reading frameColumnPrimvar(..., column).
std::string cyclesFrameColumn(int column);

/**
 * @brief The same for the MoonRay network. MoonRay's maps pass colours, so a scalar is a colour
 * of three equal channels: moonRayProjectorDot(k) and moonRayProjectorOffset(k) give component k
 * as dot(P, op2) + op2, and the plate's window is a multiply by one op2 and an add of the other.
 **/
#define kSceneProjectionMoonRayToward "MoonRayTowardProjector"
#define kSceneProjectionMoonRayPlateScale "MoonRayPlateScale"
#define kSceneProjectionMoonRayPlateOffset "MoonRayPlateOffset"
#define kSceneProjectionMoonRayPlateTexture "MoonRayPlateTexture"
#define kSceneProjectionMoonRayReferencePoints "MoonRayReferencePoints"
std::string moonRayProjectorDot(int component);
std::string moonRayProjectorOffset(int component);
// The attribute map reading moonRayFramePrimvar(..., part).
std::string moonRayFrameReader(int part);

// One input of a shader under the projection material, by the shader's name there.
struct ShaderValue {
    PXR_NS::TfToken shader;
    PXR_NS::TfToken input;
    PXR_NS::SdfValueTypeName type;
    PXR_NS::VtValue value;
};

/**
 * @brief Every input the projector sets, on the Storm surface and on the Cycles and MoonRay
 * networks alike, so the three read one source. The list's order and length do not depend on the values.
 **/
std::vector<ShaderValue> projectorValues(const PXR_NS::GfMatrix4d& worldToProjector,
                                         const PXR_NS::GfVec4f& origin);

// As projectorValues(), for the plate's window: (scale u, scale v, offset u, offset v).
std::vector<ShaderValue> plateValues(const PXR_NS::GfVec4f& plateTransform);

/**
 * @brief Cycles reads no matrix primvar, so a prim's projection frame is also authored as three
 * constant float4 primvars, column j of the frame minus the identity: a prim without them reads
 * zeros, which is the identity, as the Storm surface's fallback.
 **/
std::string frameColumnPrimvar(const std::string& framePrimvar,
                               int column);
PXR_NS::VtVec4fArray frameColumn(const PXR_NS::GfMatrix4d& frame,
                                 int column);

/**
 * @brief hdMoonray passes neither matrix nor float4 primvars to its maps, so the frame is also
 * authored as four constant float3 primvars: parts 0 to 2 are the upper three entries of the
 * frame's columns minus the identity's, part 3 its translation. A prim without them reads
 * AttributeMap's zero default, the identity, as for Cycles.
 **/
std::string moonRayFramePrimvar(const std::string& framePrimvar,
                                int part);
PXR_NS::GfVec3f moonRayFramePart(const PXR_NS::GfMatrix4d& frame,
                                 int part);

/**
 * @brief Cycles reads a missing attribute as zero, which a reference point at the world origin
 * also reads as, so a prim carrying reference points also carries this constant float primvar,
 * set to one, for the Cycles network to tell the two apart.
 **/
std::string cyclesReferencePresentPrimvar(const std::string& referencePrimvar);

struct Unresolved {
    PXR_NS::SdfPath shader;
    std::string camera;
};

/**
 * @brief Authors into layer, as overs at the shaders' paths, the projection of every shader on
 * stage that names its camera by path. Shaders whose camera stage lacks are listed in unresolved
 * and keep projecting nothing. Reads all of stage before writing, so layer may be one of its own.
 * Returns a digest of everything authored, 0 when there is nothing; when it equals skipIfDigest
 * layer is left untouched.
 **/
U64 resolve(const PXR_NS::UsdStageRefPtr& stage,
            const PXR_NS::SdfLayerRefPtr& layer,
            std::vector<Unresolved>* unresolved,
            U64 skipIfDigest = 0);
} // namespace SceneProjectionCameras

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneProjectionCameras_h
