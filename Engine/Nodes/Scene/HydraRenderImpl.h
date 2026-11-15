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
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/vt/value.h>

PXR_NAMESPACE_OPEN_SCOPE
class UsdImagingGLEngine;
class UsdImagingGLRenderParams;
PXR_NAMESPACE_CLOSE_SCOPE

#ifdef NATRON_HAVE_GPU
namespace gpu {
class GpuRecordContext;
}
#endif

NATRON_NAMESPACE_ENTER

#ifdef NATRON_HAVE_GPU
struct GpuImageView;
class RectI;
class UtilityDeviceResource;
#endif

/**
 * @brief USDRender's Storm render: a composed stage seen through a camera, read back from GL into
 * CPU buffers per AOV.
 *
 * Every render runs on one dedicated thread that owns the one GL context (HydraGLContext) all
 * of them use, and callers wait for theirs. Storm's GL objects (HgiGL, its buffers, textures and
 * programs) belong to the context that created them and an engine must render where it was
 * made, a GL context is current on at most one thread at a time, and Storm makes no promise that
 * two engines can render at once in one process; serializing on one context is what it supports.
 * The GPU, not the calling threads, bounds the throughput anyway. The thread's Storm work runs
 * in StormGLSections, as the scene view's on the main thread does, since every HgiGL of the
 * process shares one garbage collector.
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
    eFitModeVertical,
    // The aperture grows to the format's aspect, so all of it is seen (CameraUtilFit).
    eFitModeExpand
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
    // time, camera, size, headlight and purposes, and no barrier write since, reuses that render, so the
    // planes of a frame asked for one by one cost one Storm render.
    const void* owner = nullptr;
    ScenePayload scene;
    PXR_NS::UsdTimeCode time;
    PXR_NS::GfFrustum frustum;
    int width = 0;
    int height = 0;
    // Whether a light at the camera lights the scene too. Without it, only the scene's own lights
    // do, and a scene with none renders its lit surfaces black.
    bool headlight = false;
    // Which prims of a purpose other than default are drawn. Default-purpose prims always are.
    bool showRender = true;
    bool showProxy = false;
    bool showGuides = false;
    // The render delegate's plug-in id. One that is not installed or cannot render here fails the
    // render with an error naming it, before any engine is made: there is no fallback.
    std::string renderer = kRenderDelegateStormId;
    // Polled on the calling thread while a render that is not Storm's runs; true stops the delegate
    // and cancels the render. A delegate's stop waits for what cannot be interrupted to finish.
    std::function<bool()> aborted;
    // The threads a CPU delegate's engine may render on, from the scheduler's budget for the
    // calling task; any count when 0. Cycles and MoonRay take it when an engine is made. Every
    // Cycles session of the process shares one count, the one asked for last (getCyclesThreads()):
    // a change of this asks for it, and the Cycles engines made under another, the viewers'
    // included, are made again.
    int threads = 0;
    // The delegate's settings (UsdImagingGLEngine::SetRendererSetting) that differ from its
    // defaults. The owner's engine is made again when they change: Cycles restarts a converged
    // render only for some of its settings.
    std::map<std::string, PXR_NS::VtValue> settings;
    // The camera's shutter:open and shutter:close, in frames relative to time.
    double shutterOpen = 0.;
    double shutterClose = 0.;
    // The camera's frustum at times within shutterInterval(*this), relative to time and increasing.
    // Empty for a camera that does not blur: its frustum is frustum throughout.
    std::vector<std::pair<double, PXR_NS::GfFrustum>> cameraMotion;
};

/**
 * @brief The shutter request's delegate samples motion over, in frames relative to request.time: the
 * camera's unless the delegate's settings override it. Closed, both equal, for a delegate other than
 * Natron Utility, the one that blurs, and for a shutter of no length.
 **/
PXR_NS::GfVec2d shutterInterval(const Request& request) WARN_UNUSED_RETURN;

// Whether request's delegate blurs the camera's motion within shutterInterval(request).
bool cameraMotionBlur(const Request& request) WARN_UNUSED_RETURN;

// The AOVs, width x height pixels each, the bottom row first as Natron's images.
struct Result {
    std::vector<float> rgba; // 4 per pixel, as Storm shades it, with no colour transform
    std::vector<float> depth; // the distance along the camera's view axis, +inf where nothing was hit
    std::vector<float> primId; // hashToFloat(primPathHash(path)), 0 where nothing was hit
    std::map<std::uint32_t, std::string> primPaths; // the paths of the hashes primId holds
    // 3 per pixel: the shading normal in camera space, 0 where nothing was hit. Empty when the
    // delegate renders no normal AOV, as Storm.
    std::vector<float> normal;
    // What the delegate cannot render of the scene, one line per kind of prim, material or light,
    // for the node's warning; empty if it renders everything or is not checked, as Storm.
    std::string warning;
    // The errors the delegate logged that name no prim of the scene, one per line; empty if none.
    std::string errors;
    // The device the delegate says it rendered on; empty for a delegate that does not say.
    std::string device;
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

#ifdef NATRON_HAVE_GPU
/**
 * @brief Renders request as render() does, then records into context the copy of window, a colour
 * window, into output's outputRect, on the device and from the frame the delegate left there:
 * Natron Utility's, when it rendered on the GPU. *info gets the render's warning, errors and device.
 * False with *error set when the frame is not on context's device, so the caller renders on the CPU,
 * where the frame is read back.
 **/
bool recordDeviceWindow(const Request& request,
                        const PlaneWindow& window,
                        gpu::GpuRecordContext& context,
                        const GpuImageView& output,
                        const RectI& outputRect,
                        Result* info,
                        std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief Whether a render of owner's found the delegate had rendered on the CPU instead (recordDeviceWindow()
 * got no frame on the device, or Natron Utility reported falling back from the GPU), under the same device generation, pool budget and Device setting (kUtilitySettingDevice, "auto" when
 * unset), and no render of owner's has been on the GPU since. A caller asking whether to take the GPU
 * path then knows it would fall back. Never waits. MT-safe.
 **/
bool deviceFrameFellBack(const void* owner,
                         std::uint64_t generation,
                         std::uint64_t budget,
                         const std::string& device);
#endif

/**
 * @brief Render settings every engine of renderer gets on top of its request's; for tests, to fix
 * a path tracer's sample count and seed whatever the node's knobs say.
 **/
void setRendererSettings(const std::string& renderer,
                         const std::map<std::string, PXR_NS::VtValue>& settings);

/**
 * @brief Has engine, a renderer engine, draw the purposes asked for in params. Natron Utility is made
 * to sync every purpose and told which to draw, so a purpose toggle builds no geometry there.
 **/
void setPurposes(PXR_NS::UsdImagingGLEngine* engine,
                 const std::string& renderer,
                 bool showRender,
                 bool showProxy,
                 bool showGuides,
                 PXR_NS::UsdImagingGLRenderParams* params);

/**
 * @brief The setting key of owner's engine as its delegate reports it
 * (UsdImagingGLEngine::GetRendererSetting), waiting for the engine's thread. False if owner has
 * no engine.
 **/
bool engineSetting(const void* owner,
                   const std::string& key,
                   PXR_NS::VtValue* value) WARN_UNUSED_RETURN;

// Drops owner's stage and engine, whichever delegate it is of, without waiting.
void release(const void* owner);

// Drops every owner's stage and Storm engine, and has every Natron Utility engine give back what it
// keeps on the GPU, on the render threads, without waiting.
void releaseAll();

// releaseAll() and ProgressiveRender::releaseAllGpu(), then, on the thread the Natron Utility engines
// render on, once they gave their GPU memory back, empties the GPU pool of its free buffers. Does not wait.
void releaseAllGpuMemory();

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

// How many frames engines of every delegate have rendered in this process, not counting a request
// served by the owner's last render.
std::uint64_t getEngineRenderCount();

/**
 * @brief The thread count every Cycles session is made under, 0 for any: Cycles' task scheduler is
 * process-wide, and the first live session fixes it for all. It is the count asked for last, by a
 * USDRender render whose Request::threads changed or by a ProgressiveRender whose setThreads()
 * budget changed; on a change every Cycles engine made under another is dropped at its holder's
 * next render or pass and made again, and none is made while one of another count lives.
 **/
int getCyclesThreads();

// The description of the render thread's context, making it if need be; empty if it cannot be.
std::string contextDescription(std::string* error);

/**
 * @brief A render of a delegate other than Storm refined in the background, as a viewport shows a
 * path tracer: each edit of the stage, view or sample cap restarts it, and it refines until the
 * delegate converges or reaches the cap, or until stop().
 *
 * A worker thread owns the engine (made without the GPU, as the CPU delegates' render thread
 * makes its own) and polls it: a path tracer renders on threads of its own between Render() calls,
 * each of which syncs the stage's changes into it. After each poll whose sample count moved it
 * publishes the colour, over a background and encoded to sRGB, and the depth, as the window depth
 * of the frame's projection, and calls the listener on the worker. Natron Utility, which has no
 * samples to refine, publishes only its finished frames.
 *
 * Edits coalesce: a restart waits until no edit has come for a short quiet time, or until the first
 * waiting one is a bounded time old, so that a burst of edits restarts once and a camera dragged
 * without pause still restarts several times a second. A pass that was under way when an edit came
 * is never published, and neither are the delegate's pixels from before a restart. A delegate that
 * can stop is stopped as soon as an edit or stop() comes, ahead of the quiet time and holding no lock.
 *
 * The worker, and the threads the delegate starts from it, run at a lowered priority, which is what
 * keeps the interface responsive: a Cycles engine renders on the process' one Cycles thread count
 * (getCyclesThreads()), which the setThreads() budget asks for but another render may have
 * asked for since.
 *
 * A delegate that cannot start, that throws, whose AOVs cannot be read or whose render session broke
 * fails: Status::error says why, the engine goes, and the listener is called. It stays failed until
 * the next invalidate(), setSettings() or setMaxSamples() that changes something, which try it
 * again with a new engine, or until another renderer is set; setView() does not.
 *
 * The stage is read, and the engine renders, under the lock given at construction, the one that
 * guards the stage's edits; nothing else of the class takes that lock while holding its own.
 **/
class ProgressiveRender {
public:
    struct View {
        PXR_NS::GfFrustum frustum;
        int width = 0;
        int height = 0;
        PXR_NS::UsdTimeCode time;
        bool showRender = false;
        bool showProxy = true;
        bool showGuides = false;

        bool operator==(const View& other) const;
        bool operator!=(const View& other) const
        {
            return !(*this == other);
        }
    };

    struct Frame {
        int width = 0;
        int height = 0;
        // 4 bytes per pixel, the bottom row first: the colour over the background, sRGB encoded.
        std::vector<unsigned char> rgba;
        // 1 per pixel: the window depth in the view's projection, 1 where nothing was hit.
        std::vector<float> depth;
        // The delegate's sample count when the frame was read, -1 if it reports none.
        int samples = -1;
        int maxSamples = 0;
        // The frame is the restart's last: the refinement is done.
        bool converged = false;
        // Which restart the frame belongs to (getRestartCount() then), and its rank among the
        // frames published.
        std::uint64_t restart = 0;
        std::uint64_t serial = 0;
        // The edits made before its pass, for pick().
        std::uint64_t generation = 0;
#ifdef NATRON_HAVE_GPU
        // Set instead of rgba when setDeviceColor() asked for the colour of a GPU frame to stay
        // on the device: its linear premultiplied RGBA floats, the bottom row first.
        std::shared_ptr<const UtilityDeviceResource> deviceColor;
#endif
    };

    struct Status {
        // A delegate other than Storm is set and has a stage to render.
        bool active = false;
        // stop() was asked for, and the delegate is paused where it is since.
        bool stopped = false;
        bool paused = false;
        // The refinement of the last restart is done, and its last frame published: the delegate
        // converged at the cap, or held still converged short of it.
        bool converged = false;
        // The engine's sample count at the last poll, -1 if unknown.
        int samples = -1;
        int maxSamples = 0;
        std::uint64_t restarts = 0;
        std::uint64_t frames = 0;
        // Why the delegate cannot render, empty if it can.
        std::string error;
    };

    /**
     * @brief stage, called on the worker with *stageLock held, gives the stage to render, null for
     * none. listener is called on the worker after each frame is published and when the delegate
     * fails. background is the
     * linear grey the colour is composited over.
     **/
    ProgressiveRender(std::mutex* stageLock,
                      const std::function<PXR_NS::UsdStageRefPtr()>& stage,
                      const std::function<void()>& listener,
                      float background);

    // Stops the delegate's threads and drops the engine, on the worker, then joins it.
    ~ProgressiveRender();

    /**
     * @brief Renders with the delegate id from now on; the engine of another is dropped first.
     * Storm's id, or an empty one, makes it idle and frees the engine and its threads.
     **/
    void setRenderer(const std::string& id);

    /**
     * @brief The threads a Cycles engine of this should render on, any count for 0: called on the
     * worker before its passes, which ask getCyclesThreads() for it when it changes.
     **/
    void setThreads(const std::function<int()>& threads);

    // Restarts when view differs from the last one set.
    void setView(const View& view);

    // Restarts: the stage was edited in a way its change notices do not show.
    void invalidate();

    /**
     * @brief The renderer settings of the stage's active render settings prim for the renderer
     * (RenderSettingsPrim::settings()), set on its engine before it renders. A change makes
     * the engine again, keeping the frame shown until the next one; the same settings change
     * nothing. The sample cap of setMaxSamples() applies over the prim's samples.
     **/
    void setSettings(const std::map<std::string, PXR_NS::VtValue>& settings);

    // The sample cap; <= 0 for the delegate's default, the prim's samples if it sets them. A change restarts.
    void setMaxSamples(int samples);

    // The cap in force: the one set, or the delegate's default once its engine is made, else 0.
    int getMaxSamples() const;

    // Pauses the delegate where it is, keeping its samples; resume() continues from them.
    void stop();
    void resume();

    Status getStatus() const;

    // The last frame published, null before the first.
    std::shared_ptr<const Frame> getFrame() const;

    // How many renders have been started, the first included.
    std::uint64_t getRestartCount() const;

#ifdef NATRON_HAVE_GPU
    /**
     * @brief Whether frames the Natron Utility renderer leaves on the GPU are published with
     * deviceColor and no rgba, for a viewer that imports them; off at first. Frames published
     * before a change keep the form they had.
     **/
    void setDeviceColor(bool onDevice);

    /**
     * @brief The rgba frame would have had without deviceColor, read back from the device. False
     * with *error set when frame has no deviceColor or the device cannot read it back.
     **/
    bool hostColor(const Frame& frame,
                   std::vector<unsigned char>* rgba,
                   std::string* error) const;

    /**
     * @brief Has a Natron Utility engine give back, on the worker, the GPU memory it keeps between
     * frames, then empties the GPU pool of its free buffers. A frame published with deviceColor is
     * published again with its colour read back into rgba, and its film is let go for every holder.
     * Does not wait.
     **/
    void releaseGpu();

    // Has a Natron Utility engine let go of its GPU, on the worker, if that was lost. Does not wait.
    void releaseLostGpu();

    // releaseGpu() on every ProgressiveRender alive. MT-safe.
    static void releaseAllGpu();

    // releaseLostGpu() on every ProgressiveRender alive; a GPU loss releaser calls it. MT-safe.
    static void releaseAllLostGpu();
#endif

    /**
     * @brief The path of the prim drawn at the pixel (x, y) of frame, one getFrame() gave, counted
     * from its bottom-left pixel, or empty. Empty too once the render was edited since frame's pass:
     * the engine's IDs are then of another render than the pixels frame shows. Takes the stage lock.
     **/
    std::string pick(const Frame& frame,
                     int x,
                     int y);

    // How many workers are alive in the process, for leak checks.
    static int getLiveCount();

    struct Implementation;

private:
    std::unique_ptr<Implementation> _imp;
};
} // namespace HydraRender

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_HydraRenderImpl_h
