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

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"
#include <pxr/base/vt/value.h>

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
 *
 * Each owner (a USDRender node) keeps one SceneStage and one UsdImagingGLEngine there between
 * renders, all engines sharing the thread's one HgiGL. A render syncs the owner's stage to the
 * request's scene, so that a frame whose stack is unchanged only moves the time and an edit
 * upstream reaches Storm as the change notices of the layers it swapped, and the engine keeps its
 * buffers, shaders and textures. Stage and engine go when the owner is released, when every one is
 * (releaseAll), and, least recently used first, while the engines' GPU memory is over the budget.
 *
 * Those stages read the scene's layers outside any SceneWriteBarrier::ReadScope, so the thread is
 * a participant of the barrier: a write waits for the job it is running and holds back the next.
 *
 * Every other Hydra render delegate (RenderDelegates) renders on a second thread with no GL at
 * all: its engines are made by plug-in id with the GPU off, their AOVs are CPU buffers read back
 * as they are, and they count against no GPU budget. A slow CPU render so never holds up Storm,
 * and between its passes it lets barrier writes through. An owner's engine lives on the thread of
 * the delegate it last rendered with; a render with another delegate drops it there.
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
    // Whose stage and engine render it. A request of the owner's last render with the same stack,
    // time, camera and size, and no barrier write since, reuses that render, so the planes of a
    // frame asked for one by one cost one Storm render.
    const void* owner = nullptr;
    ScenePayload scene;
    PXR_NS::UsdTimeCode time;
    PXR_NS::GfFrustum frustum;
    int width = 0;
    int height = 0;
    // The render delegate's plug-in id. One that is not installed or cannot render here fails the
    // render with an error naming it, before any engine is made: there is no fallback.
    std::string renderer = kRenderDelegateStormId;
    // Polled on the calling thread while a render that is not Storm's converges; true cancels it.
    std::function<bool()> aborted;
    // The threads a CPU delegate's engine may render on, from the scheduler's budget for the
    // calling task; any count when 0. Cycles and MoonRay take it when the owner's engine is
    // made.
    int threads = 0;
};

// The AOVs, width x height pixels each, the bottom row first as Natron's images.
struct Result {
    std::vector<float> rgba; // 4 per pixel, as Storm shades it, with no colour transform
    std::vector<float> depth; // the distance along the camera's view axis, +inf where nothing was hit
    std::vector<float> primId; // hashToFloat(primPathHash(path)), 0 where nothing was hit
    std::map<std::uint32_t, std::string> primPaths; // the paths of the hashes primId holds
    // 3 per pixel: the shading normal in camera space, 0 where nothing was hit. Empty when the
    // delegate renders no normal AOV, as Storm.
    std::vector<float> normal;
    // What the delegate could not render of the scene, one line per kind of prim, for the node's
    // warning; empty if it rendered everything or does not say.
    std::string warning;
    // The errors the delegate logged that name no prim of the scene, one per line; empty if none.
    std::string errors;
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
    ePlanePrimId,
    ePlaneNormal
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
 * @brief Renders request on the render thread, unless the owner's last render can be reused (its
 * stage unedited, and the time, view and size unchanged), then runs job there and waits for it. Returns what job returns; false with
 * *error set if the render failed. Storm only: other delegates' pixels are read back.
 **/
bool withRender(const Request& request,
                const std::function<bool(RenderAccess& access, std::string* error)>& job,
                std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief Runs job on the render thread with the context current and waits for it, passing the
 * context's GL entry point lookup. False with *error set if there is no context or job threw.
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

/**
 * @brief Render settings (UsdImagingGLEngine::SetRendererSetting) every engine of renderer gets
 * before it first renders; an engine made under other settings is made again. Until the settings
 * knobs exist only tests use it, to fix a path tracer's sample count and seed.
 **/
void setRendererSettings(const std::string& renderer,
                         const std::map<std::string, PXR_NS::VtValue>& settings);

// Drops owner's stage and engine, whichever delegate it is of, without waiting.
void release(const void* owner);

// Drops every owner's stage and Storm engine, on the render thread, without waiting.
void releaseAll();

// Drops every owner's stage and engine of a delegate other than Storm, without waiting.
void releaseAllCpuEngines();

/**
 * @brief The GPU memory, in bytes, the engines may hold together before the least recently used
 * ones other than the one rendering are dropped. Checked after each render.
 **/
void setMemoryBudget(std::size_t bytes);
std::size_t getMemoryBudget();

// How many Storm engines are alive, waiting for the render thread's queue to drain.
std::size_t getEngineCount();

// How many engines of other delegates are alive, waiting for their thread's queue to drain.
std::size_t getCpuEngineCount();

// How many engines, of every delegate, have been made in this process.
std::uint64_t getEngineBuildCount();

// The description of the render thread's context, making it if need be; empty if it cannot be.
std::string contextDescription(std::string* error);
} // namespace HydraRender

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_HydraRenderImpl_h
