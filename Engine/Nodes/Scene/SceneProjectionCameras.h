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

struct Unresolved {
    PXR_NS::SdfPath shader;
    std::string camera;
};

/**
 * @brief Authors into layer, as overs at the shaders' paths, the projection of every shader on
 * stage that names its camera by path. Shaders whose camera stage lacks are listed in unresolved
 * and keep projecting nothing. Reads all of stage before writing, so layer may be one of its own.
 **/
void resolve(const PXR_NS::UsdStageRefPtr& stage,
             const PXR_NS::SdfLayerRefPtr& layer,
             std::vector<Unresolved>* unresolved);
} // namespace SceneProjectionCameras

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneProjectionCameras_h
