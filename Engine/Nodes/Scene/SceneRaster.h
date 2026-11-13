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
#include <functional>
#include <memory>
#include <optional>
#include <set>
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
// A camera given by its pose and lens rather than by a prim, as the Viewer's own camera is.
struct ExplicitCamera {
    // Camera to world, row-major as a GfMatrix4d (translation in the last row); the camera looks
    // down its -Z with +Y up, as a UsdGeomCamera does. Any scale in it is ignored.
    double cameraToWorld[16] = { 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1. };
    // Perspective: the height the view spans at distance 1 in front of the camera (2 * tan(fovY / 2)).
    // Orthographic: the height, in world units, the view spans.
    double viewHeight = 1.;
    // The view's width over its height; the buffers' size is taken as it is, with no fit applied.
    double aspect = 1.;
    bool orthographic = false;
    double nearClip = 0.1;
    double farClip = 10000.;
};

struct CameraArgs {
    PXR_NS::SdfPath cameraPath;
    // When set, the camera is this, and cameraPath, pixelAspect and fit are ignored. When not set,
    // the camera is the prim at cameraPath.
    std::optional<ExplicitCamera> explicitCamera;
    // The texture coordinate primvar, with or without its "primvars:" namespace, whose per-pixel
    // value CameraBuffers::st and udimTile hold; empty to skip them.
    PXR_NS::TfToken stName;
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
    // When set and true, drawing stops and renderCamera returns false with an empty error.
    std::function<bool()> cancelled;
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
    // Only when CameraArgs::stName was set: the interpolated st of the nearest surface and its UDIM
    // tile, 1001 + floor(u) + 10 * floor(v). A pixel with nothing drawn, or whose mesh has no usable
    // st, holds (0, 0) and tile 0, as does one whose st lies outside the tile grid.
    std::vector<PXR_NS::GfVec2f> st;
    std::vector<std::int32_t> udimTile;
    // Every mesh drawn, in traversal order.
    std::vector<PXR_NS::SdfPath> meshPaths;
    // The frustum the buffers were drawn through, conformed to the buffers' aspect.
    PXR_NS::GfFrustum frustum;
};

/**
 * @brief The frustum renderCamera draws args through: the explicit camera as it is, or the camera
 * at args.cameraPath conformed per args.fit to width * pixelAspect / height. False, with
 * *errorMessage set, on the same camera and size errors as renderCamera.
 **/
bool cameraFrustum(const PXR_NS::UsdStageRefPtr& stage,
                   const CameraArgs& args,
                   PXR_NS::GfFrustum* frustum,
                   std::string* errorMessage) WARN_UNUSED_RETURN;

/**
 * @brief Draws the meshes of args.prims as the camera at args.cameraPath sees them: a z-buffer of
 * depth, mesh and world position per pixel. Triangles are clipped to the camera's near and far
 * planes. False, with *errorMessage set, when there is no camera at the path, the explicit camera
 * is degenerate, or the size is empty.
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
    // A second texture coordinate primvar, as a material's primvar reader names it, whose value at
    // each texel BakeBuffers::readerSt holds; empty to skip it.
    PXR_NS::TfToken readerStName;
    // When set and true, drawing stops and bakeUV returns false with an empty error.
    std::function<bool()> cancelled;
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
    // Only when BakeArgs::readerStName was set: that primvar's value where a covered texel lies;
    // (0, 0) where not covered or on a face the primvar has no value for.
    std::vector<PXR_NS::GfVec2f> readerSt;
};

/**
 * @brief Rasterizes the mesh at args.primPath in its texture space: for each texel of the UDIM
 * tile, the world position and normal of the point the texel maps to. The primvar may be
 * vertex, varying, faceVarying or uniform, indexed or not. Where UV faces overlap, the later face
 * wins. False, with *errorMessage naming the prim, when the prim is not a mesh, the primvar, or
 * args.readerStName when set, is missing or not 2D, its interpolation is constant, or the tile or
 * size is invalid.
 **/
bool bakeUV(const PXR_NS::UsdStageRefPtr& stage,
            const BakeArgs& args,
            BakeBuffers* buffers,
            std::string* errorMessage) WARN_UNUSED_RETURN;

/**
 * @brief A mesh laid out in its texture space as bakeUV() lays it out, read from the stage once,
 * so that the tile can be baked a band of rows at a time: a band's buffers are all that is held,
 * however large the tile.
 **/
class UVLayout {
public:
    UVLayout();
    ~UVLayout();

    UVLayout(const UVLayout&) = delete;
    UVLayout& operator=(const UVLayout&) = delete;

    // Reads the mesh as bakeUV() does, with its errors. The stage is only read here.
    bool prepare(const PXR_NS::UsdStageRefPtr& stage,
                 const BakeArgs& args,
                 std::string* errorMessage) WARN_UNUSED_RETURN;

    /**
     * @brief bakeUV()'s buffers over rows [firstRow, firstRow + rowCount) of the tile alone:
     * buffers->height is rowCount, and element x + y * width is texel (x, firstRow + y) of the
     * tile. False with *errorMessage empty when args.cancelled returned true.
     **/
    bool bakeRows(int firstRow,
                  int rowCount,
                  BakeBuffers* buffers,
                  std::string* errorMessage) const WARN_UNUSED_RETURN;

private:
    struct Data;
    std::unique_ptr<Data> _data;
};

/**
 * @brief Inserts into *tiles every UDIM tile a face of the mesh at primPath covers some area of,
 * by the texture coordinate primvar stName (with or without its "primvars:" namespace). Faces
 * outside the tile grid, u below 0 or from 10 on, or v below 0, are left out. False, with
 * *errorMessage naming the prim, on bakeUV's errors about the mesh and the primvar.
 **/
bool udimTiles(const PXR_NS::UsdStageRefPtr& stage,
               const PXR_NS::SdfPath& primPath,
               const PXR_NS::TfToken& stName,
               PXR_NS::UsdTimeCode time,
               std::set<int>* tiles,
               std::string* errorMessage) WARN_UNUSED_RETURN;
} // namespace SceneRaster

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneRaster_h
