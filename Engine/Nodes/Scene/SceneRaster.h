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

#ifndef Engine_Nodes_Scene_SceneRaster_h
#define Engine_Nodes_Scene_SceneRaster_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A CPU triangle rasterizer over a composed stage, with no GL, so that it runs anywhere
 * the engine does. Meshes are triangulated by Hydra's HdMeshUtil (n-gons fan-triangulated, holes
 * skipped, leftHanded winding flipped); a subdivision surface rasterizes its control cage.
 *
 * The stage is only read on the calling thread, which may hold a SceneQueryPool lease on it: the
 * geometry is gathered first, and the rasterization that follows runs on the global pool over
 * scanline chunks, each chunk writing its own rows.
 *
 * Buffers are row-major and bottom-up, as Natron images are: element x + y * width covers the
 * pixel whose centre is (x + 0.5, y + 0.5), y = 0 at the bottom.
 **/
namespace SceneRaster {
struct CameraArgs {
    PXR_NS::SdfPath cameraPath;
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    int width = 0;
    int height = 0;
    // The width of a pixel over its height; the camera's aperture is conformed per fit to
    // width * pixelAspect / height, as USDRender does.
    double pixelAspect = 1.;
    HydraRender::FitModeEnum fit = HydraRender::eFitModeFill;
    // The prims whose meshes, at or under them, are drawn; empty draws every mesh of the stage.
    // Invisible meshes and those of guide purpose are left out.
    std::vector<PXR_NS::SdfPath> prims;
};

struct CameraBuffers {
    int width = 0;
    int height = 0;
    // The distance in front of the camera, along its view axis, of the nearest surface; +inf where
    // nothing was drawn.
    std::vector<float> depth;
    // The index in meshPaths of the mesh nearest the camera, -1 where nothing was drawn.
    std::vector<std::int32_t> primIndex;
    // The world-space position of the nearest surface; (0, 0, 0) where nothing was drawn.
    std::vector<PXR_NS::GfVec3f> worldPosition;
    // Every mesh drawn, in traversal order.
    std::vector<PXR_NS::SdfPath> meshPaths;
    // The frustum the buffers were drawn through, conformed to the buffers' aspect.
    PXR_NS::GfFrustum frustum;
};

/**
 * @brief Draws the meshes of args.prims as the camera at args.cameraPath sees them: a z-buffer of
 * depth, mesh and world position per pixel. Triangles are clipped to the camera's near and far
 * planes. False, with *errorMessage set, when there is no camera at the path or the size is empty.
 **/
bool renderCamera(const PXR_NS::UsdStageRefPtr& stage,
                  const CameraArgs& args,
                  CameraBuffers* buffers,
                  std::string* errorMessage) WARN_UNUSED_RETURN;

struct BakeArgs {
    PXR_NS::SdfPath primPath;
    // The texture coordinate primvar, with or without its "primvars:" namespace.
    PXR_NS::TfToken stName = PXR_NS::TfToken("st");
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    int width = 0;
    int height = 0;
    // The UDIM tile the texture covers: tile 1001 + u + 10 * v spans [u, u + 1] x [v, v + 1].
    int udimTile = 1001;
};

struct BakeBuffers {
    int width = 0;
    int height = 0;
    // 1 where the texel's centre lies on the mesh's UV layout, else 0.
    std::vector<std::uint8_t> coverage;
    // Where on the mesh, in world space, a covered texel lies; (0, 0, 0) where not covered.
    std::vector<PXR_NS::GfVec3f> worldPosition;
    // The unit world-space normal of the face a covered texel lies on, pointing out of the mesh's
    // front side (its orientation, through the world transform); (0, 0, 0) where not covered.
    std::vector<PXR_NS::GfVec3f> worldNormal;
};

/**
 * @brief Rasterizes the mesh at args.primPath in its texture space: for each texel of the UDIM
 * tile, the world position and normal of the point the texel maps to. The primvar may be
 * vertex, varying, faceVarying or uniform, indexed or not. Where UV faces overlap, the later face
 * wins. False, with *errorMessage naming the prim, when the prim is not a mesh, the primvar is
 * missing or not 2D, its interpolation is constant, or the tile or size is invalid.
 **/
bool bakeUV(const PXR_NS::UsdStageRefPtr& stage,
            const BakeArgs& args,
            BakeBuffers* buffers,
            std::string* errorMessage) WARN_UNUSED_RETURN;
} // namespace SceneRaster

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneRaster_h
