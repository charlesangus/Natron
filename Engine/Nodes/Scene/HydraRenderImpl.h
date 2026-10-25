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

#include <cstddef>
#include <cstdint>
#include <functional>
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
    // The stack hash of the scene the stage was composed from, or 0. A request of the owner's last
    // render with the same non-zero hash, time, camera and size reuses that render, so the planes of
    // a frame asked for one by one cost one Storm render.
    std::uint64_t sceneHash = 0;
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

enum PlaneEnum {
    ePlaneColor = 0,
    ePlaneDepth,
    ePlanePrimId
};

/**
 * @brief A window of one plane of a render, in the render's pixels ((0, 0) its bottom-left pixel),
 * which may reach past the render: nComps floats per pixel, row by row from the bottom row. A
 * colour window of one component holds alpha, of more the components of rgba in order (0 past the
 * fourth); a pixel past the render holds what one that hit nothing holds.
 **/
struct PlaneWindow {
    PlaneEnum plane = ePlaneColor;
    int nComps = 4;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;

    std::size_t floatCount() const
    {
        return (std::size_t)(x2 - x1) * (std::size_t)(y2 - y1) * (std::size_t)nComps;
    }
};

// What a withRender() job can do on the render thread, with the context current.
class RenderAccess {
public:
    virtual ~RenderAccess() { }

    /**
     * @brief Writes window into the GL buffer glBuffer from its first byte, on the GPU: the GL
     * commands are issued, not finished. The same pixels give the same bits whatever the window.
     * False with *error set when the context cannot pack, in which case it never can.
     **/
    virtual bool pack(const PlaneWindow& window,
                      unsigned int glBuffer,
                      std::string* error)
        = 0;

    // The paths of the hashes the render's primId plane holds.
    virtual bool primPaths(std::map<std::uint32_t, std::string>* paths,
                           std::string* error)
        = 0;

    // The address of a GL entry point of the context.
    virtual void* getProcAddress(const char* name) const = 0;
};

/**
 * @brief Renders request on the render thread, unless the owner's last render can be reused (see
 * Request::sceneHash), then runs job there and waits for it. Returns what job returns; false with
 * *error set if the render failed.
 **/
bool withRender(const Request& request,
                const std::function<bool(RenderAccess& access, std::string* error)>& job,
                std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief Runs job on the render thread with the context current and waits for it, passing the
 * context's GL entry point lookup. False with *error set if there is no context.
 **/
bool withContext(const std::function<void(const std::function<void*(const char*)>& getProcAddress)>& job,
                 std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief Queues job on the render thread without waiting. getProcAddress is null when the context
 * could not be made current. Dropped if the thread never started.
 **/
void postToContext(std::function<void(const std::function<void*(const char*)>* getProcAddress)> job);

/**
 * @brief The pixels of window, as RenderAccess::pack() writes them, read back into *pixels, and
 * the paths of the primId hashes into *primPaths when window is of that plane and primPaths is
 * not null.
 **/
bool renderWindow(const Request& request,
                  const PlaneWindow& window,
                  std::vector<float>* pixels,
                  std::map<std::uint32_t, std::string>* primPaths,
                  std::string* error) WARN_UNUSED_RETURN;

// Whether the render thread's context packs windows on the GPU, making it if need be.
bool packsOnGpu();

// Drops owner's engine, on the render thread, without waiting.
void release(const void* owner);

// The description of the render thread's context, making it if need be; empty if it cannot be.
std::string contextDescription(std::string* error);
} // namespace HydraRender

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_HydraRenderImpl_h
