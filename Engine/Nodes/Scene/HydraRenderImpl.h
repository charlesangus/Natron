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

#ifndef Engine_Nodes_Scene_HydraRenderImpl_h
#define Engine_Nodes_Scene_HydraRenderImpl_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief USDRender's Storm render: a composed stage seen through a camera, read back from GL into
 * CPU buffers per AOV.
 *
 * Every render runs on one dedicated thread that owns the one GL context (HydraGLContext) all
 * of them use, and callers wait for theirs. Storm's GL objects (HgiGL, its buffers, textures and
 * programs) belong to the context that created them and an engine must render where it was
 * made, a GL context is current on at most one thread at a time, and Storm makes no promise that
 * two engines can render at once in one process; serializing on one context is what it supports.
 * The GPU, not the calling threads, bounds the throughput anyway.
 **/
namespace HydraRender {
// How the camera's aperture is conformed to the output format (CameraUtilConformWindow).
enum FitModeEnum {
    // The aperture fills the format: it is cropped to the format's aspect (CameraUtilCrop).
    eFitModeFill = 0,
    // The aperture's width is kept and its height follows the format (CameraUtilMatchHorizontally).
    eFitModeHorizontal,
    // The aperture's height is kept and its width follows the format (CameraUtilMatchVertically).
    eFitModeVertical
};

/**
 * @brief The frustum of the UsdGeomCamera at cameraPath on stage at time, its aperture conformed
 * to aspect (the format's width over height, pixel aspect included) per fit. False, with *error
 * naming the path, if no camera is there.
 **/
bool cameraFrustum(const PXR_NS::UsdStageRefPtr& stage,
                   const PXR_NS::SdfPath& cameraPath,
                   PXR_NS::UsdTimeCode time,
                   double aspect,
                   FitModeEnum fit,
                   PXR_NS::GfFrustum* frustum,
                   std::string* error) WARN_UNUSED_RETURN;

// Every UsdGeomCamera prim on stage, in traversal order.
std::vector<PXR_NS::SdfPath> cameraPaths(const PXR_NS::UsdStageRefPtr& stage) WARN_UNUSED_RETURN;

/**
 * @brief The primId value of a prim path, as Cryptomatte makes its IDs: MurmurHash3_x86_32 of the
 * path's text with seed 0, whose bits, when they would make an infinity, NaN or denormal float,
 * have the lowest exponent bit flipped. Never 0, the value of pixels that hit nothing.
 **/
std::uint32_t primPathHash(const std::string& path) WARN_UNUSED_RETURN;

// The hash's bits as a float, the value primId.id holds.
float hashToFloat(std::uint32_t hash) WARN_UNUSED_RETURN;

struct Request {
    // Whose engine renders: an engine shows one stage, and stays alive between renders of the
    // same owner so that Storm keeps its buffers, shaders and textures.
    const void* owner = nullptr;
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::UsdTimeCode time;
    PXR_NS::GfFrustum frustum;
    int width = 0;
    int height = 0;
};

// The AOVs, width x height pixels each, the bottom row first as Natron's images.
struct Result {
    std::vector<float> rgba; // 4 per pixel, as Storm shades it, with no colour transform
    std::vector<float> depth; // the distance along the camera's view axis, +inf where nothing was hit
    std::vector<float> primId; // hashToFloat(primPathHash(path)), 0 where nothing was hit
    std::map<std::uint32_t, std::string> primPaths; // the paths of the hashes primId holds
};

/**
 * @brief Renders request on the render thread, waiting for it. Callable from any thread but that
 * one. False with *error set if the context could not be made (the same message every time) or
 * the render failed.
 **/
bool render(const Request& request,
            Result* result,
            std::string* error) WARN_UNUSED_RETURN;

// Drops owner's engine, on the render thread, without waiting.
void release(const void* owner);

// The description of the render thread's context, making it if need be; empty if it cannot be.
std::string contextDescription(std::string* error);
} // namespace HydraRender

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_HydraRenderImpl_h
