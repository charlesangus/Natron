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

#ifndef Engine_Nodes_Scene_TextureProjection_h
#define Engine_Nodes_Scene_TextureProjection_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Projects an image painted through a camera into the texture space of one mesh: each
 * texel of a UDIM tile of the mesh's UV layout is placed on the mesh, seen through the camera, and
 * takes the colour of the image there, filtered bilinearly.
 *
 * Texels the camera does not see stay transparent black: those off its frame or behind it, those
 * on faces turned away from it (unless projectBackFaces), and, with occlusion, those hidden behind
 * any mesh of the stage. Texels off the UV layout are transparent black too, except within
 * edgeDilation texels of it, which take the mean colour of their filled neighbours, one texel per
 * pass, so that texture filtering does not bleed black across UV seams.
 *
 * Buffers are row-major and bottom-up, as SceneRaster's are.
 **/
namespace TextureProjection {
struct SourceImage {
    // width * height premultiplied RGBA pixels, bottom-up; the image spans the camera's whole view.
    const float* rgba = nullptr;
    int width = 0;
    int height = 0;
    // The width of a pixel over its height; with a camera given by path, the camera's aperture is
    // conformed per fit to width * pixelAspect / height, as USDRender frames its render.
    double pixelAspect = 1.;
};

struct Args {
    // When set, the camera the image was painted through is this, and cameraPath and fit are
    // ignored; it maps the image to its whole view, whatever their aspects. When not set, the
    // camera is the prim at cameraPath.
    std::optional<SceneRaster::ExplicitCamera> explicitCamera;
    PXR_NS::SdfPath cameraPath;
    HydraRender::FitModeEnum fit = HydraRender::eFitModeFill;
    PXR_NS::SdfPath meshPath;
    // The texture coordinate primvar, with or without its "primvars:" namespace.
    PXR_NS::TfToken stName = PXR_NS::TfToken("st");
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    // The UDIM tile the texture covers: tile 1001 + u + 10 * v spans [u, u + 1] x [v, v + 1].
    int udimTile = 1001;
    // The texture's size in texels.
    int width = 0;
    int height = 0;
    bool occlusion = true;
    // How far behind the nearest surface the camera sees a texel may lie and still be seen, as a
    // fraction of its distance from the camera.
    double depthBias = 0.01;
    bool projectBackFaces = false;
    // Passes of edge dilation, each growing the layout by one texel.
    int edgeDilation = 0;
    // When set and true, the projection stops and project returns false with an empty error.
    std::function<bool()> cancelled;
};

struct Result {
    int width = 0;
    int height = 0;
    // Premultiplied RGBA per texel.
    std::vector<float> rgba;
    // 1 where the texel lies on the UV layout and took a sample of the image, else 0; dilated
    // texels are not covered.
    std::vector<std::uint8_t> coverage;
};

/**
 * @brief Projects image through the camera of args onto args.meshPath of stage, into result.
 * The stage is only read on the calling thread. False, with *errorMessage set, when the image is
 * empty, there is no camera at the path or the explicit camera is degenerate, the size is empty,
 * or the mesh cannot be baked (SceneRaster::bakeUV's errors, which name the prim).
 **/
bool project(const PXR_NS::UsdStageRefPtr& stage,
             const SourceImage& image,
             const Args& args,
             Result* result,
             std::string* errorMessage) WARN_UNUSED_RETURN;

/**
 * @brief Lays paint, projected as project() projects it, over the args.width x args.height
 * premultiplied RGBA texels of *rgba, mesh after mesh in the order given (args.meshPath is
 * ignored): where a mesh's texel sees the image, it becomes rgba * (1 - m) + p, m and p being
 * mask and paint sampled there. mask is one float per image pixel. The camera's depth is drawn
 * once for every mesh, and each mesh is laid out a band of rows at a time, so the only buffer as
 * large as the tile is the caller's. No edge dilation. project()'s errors, plus a null rgba.
 **/
bool projectOver(const PXR_NS::UsdStageRefPtr& stage,
                 const SourceImage& paint,
                 const float* mask,
                 const Args& args,
                 const std::vector<PXR_NS::SdfPath>& meshes,
                 float* rgba,
                 std::string* errorMessage) WARN_UNUSED_RETURN;
} // namespace TextureProjection

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TextureProjection_h
