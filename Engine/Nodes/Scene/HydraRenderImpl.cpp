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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "HydraRenderImpl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <QDebug>

#ifdef emit
#define NATRON_HYDRARENDER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/imaging/hgiGL/texture.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_HYDRARENDER_RESTORE_EMIT
#undef NATRON_HYDRARENDER_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/DelegateSupport.h"
#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraGLContext.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/MoonRaySettings.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/StormGLSection.h"
#include "Engine/Nodes/Scene/StormPtexBudget.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Storm renders a frame in passes until it converges; a rasterizer needs one.
const int kMaxRenderPasses = 8;

// Hydra's worker threads rethrow into the caller of Render(), and nothing above it handles an
// exception. False with what() in *what when one came out.
bool
renderGuarded(UsdImagingGLEngine* engine,
              const UsdPrim& root,
              const UsdImagingGLRenderParams& params,
              std::string* what)
{
    try {
        RenderDelegates::runRenderFaultHook();
        engine->Render(root, params);
    } catch (const std::exception& e) {
        *what = e.what();
        if (what->empty()) {
            *what = "unknown exception";
        }

        return false;
    } catch (...) {
        *what = "unknown exception";

        return false;
    }

    return true;
}

// How long a render of another delegate waits between its passes, polling for convergence.
const std::chrono::milliseconds kConvergencePoll(10);

// How long a render with no abort callback may take to converge.
const std::chrono::seconds kUnabortableRenderLimit(600);

// How long a delegate may show no sign of a restart in its progress before its engine is made
// again: until it shows one its buffers may hold the render of before, which is not read.
const std::chrono::milliseconds kRestartUnseen(2000);

// The samples per pixel the delegate reports rendered, -1 if it reports none: Cycles' annotation
// carries "Sample N/M", "Sample N" when uncapped.
int
samplesOf(const VtDictionary& stats,
          int cap)
{
    const VtDictionary::const_iterator annotation = stats.find("renderProgressAnnotation");
    if ((annotation != stats.end()) && annotation->second.IsHolding<std::string>()) {
        const std::string& text = annotation->second.UncheckedGet<std::string>();
        const std::string::size_type at = text.find("Sample ");
        if (at != std::string::npos) {
            int samples = -1;
            if (std::sscanf(text.c_str() + at + 7, "%d", &samples) == 1) {
                return samples;
            }
        }
    }
    const VtDictionary::const_iterator fraction = stats.find("fractionDone");
    if ((cap > 0) && (fraction != stats.end()) && fraction->second.IsHolding<double>()) {
        return (int)std::floor(fraction->second.UncheckedGet<double>() * cap + 1e-6);
    }

    return -1;
}

// The seconds since the delegate's render began, -1 if it reports none.
double
clockOf(const VtDictionary& stats)
{
    const VtDictionary::const_iterator clock = stats.find("totalClockTime");
    if ((clock != stats.end()) && clock->second.IsHolding<double>()) {
        return clock->second.UncheckedGet<double>();
    }

    return -1.;
}

// Whether a layer the stage composes authors time samples, splines or value clips. Read from the
// layers, without composing: a stage with none shows the delegate nothing new at another time.
bool
mayVaryInTime(const UsdStageRefPtr& stage)
{
    for (const SdfLayerHandle& layer : stage->GetUsedLayers()) {
        if (!layer) {
            continue;
        }
        if (!layer->ListAllTimeSamples().empty()) {
            return true;
        }
        bool varies = false;
        layer->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& path) {
            varies = varies || layer->HasField(path, SdfFieldKeys->Spline) || layer->HasField(path, UsdTokens->clips) || layer->HasField(path, UsdTokens->clipSets);
        });
        if (varies) {
            return true;
        }
    }

    return false;
}

/**
 * Whether a delegate whose render restarted has shown since, in its progress, that it began
 * rendering again. Until it does its buffers may hold the pixels of before, and its word that it
 * converged means nothing: Cycles resets its session on its own thread, after the pass that edits
 * it, and marks its buffers converged when a reset cuts a render short.
 **/
class RestartWatch {
public:
    // Seen from the start when nothing could be mistaken for the new render's: an engine that has
    // not rendered has no pixels from before, and a delegate that is not watched is trusted.
    void restart(bool seen)
    {
        _seen = seen;
        _samplesAtRestart = -1;
        _clockAtRestart = -1.;
        _clockFallen = false;
        _samplesAtClockFall = -1;
    }

    // The delegate's progress just before the first pass after the restart.
    void before(const VtDictionary& stats,
                int cap)
    {
        _samplesAtRestart = samplesOf(stats, cap);
        _clockAtRestart = clockOf(stats);
    }

    // Reads the delegate's progress after a pass; returns seen().
    bool after(int samples,
               double clock)
    {
        if (_seen) {
            return true;
        }
        const bool samplesFell = (samples >= 0) && (_samplesAtRestart >= 0) && (samples < _samplesAtRestart);
        if (!_clockFallen && (clock >= 0.) && (_clockAtRestart >= 0.) && (clock < _clockAtRestart)) {
            // Cycles starts its clock over when its session resets, before it renders a sample of
            // the new render: until its sample count moves after that, the count it reports and
            // the pixels it holds are still the render's of before.
            _clockFallen = true;
            _samplesAtClockFall = samples;
        }
        const bool clockFell = _clockFallen && ((samples < 0) || (samples != _samplesAtClockFall));
        const bool blind = (samples < 0) && (clock < 0.);
        _seen = samplesFell || clockFell || blind;

        return _seen;
    }

    bool seen() const
    {
        return _seen;
    }

    // Whether the delegate's clock fell since the restart: its session took the restart, and may
    // spend long syncing a heavy stage before its first sample; making it again would only begin
    // that sync over.
    bool restarting() const
    {
        return _clockFallen;
    }

    // Whether a delegate's progress is known to show its restarts: Cycles' samples and clock start
    // over. MoonRay's progress carries no sample count, and a watch waiting on a clock that does
    // not fall would make its engine again for nothing.
    static bool watches(const std::string& renderer)
    {
        return CyclesAovs::isCycles(renderer);
    }

private:
    bool _seen = true;
    int _samplesAtRestart = -1;
    double _clockAtRestart = -1.;
    bool _clockFallen = false;
    int _samplesAtClockFall = -1;
};

// The GPU memory the engines may hold together by default.
const std::size_t kDefaultMemoryBudget = (std::size_t)2 << 30;

std::mutex gFactoryMutex;
HydraGLContextFactory gFactory;

std::atomic<std::size_t> gMemoryBudget(kDefaultMemoryBudget);
std::atomic<std::uint64_t> gEngineBuilds(0);

typedef std::map<std::string, VtValue> RendererSettings;
std::mutex gSettingsMutex;
std::map<std::string, RendererSettings> gSettings;

RendererSettings
rendererSettings(const std::string& renderer)
{
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    std::map<std::string, RendererSettings>::const_iterator found = gSettings.find(renderer);

    return (found == gSettings.end()) ? RendererSettings() : found->second;
}

/**
 * The one thread count every Cycles session of the process is made under. Cycles' TaskScheduler is
 * process-wide and counted: the first session to live fixes the threads of every session made while
 * any lives, so USDRender's engines and the viewers' progressive ones cannot each have a count.
 *
 * The count is the one asked for last: USDRender's thread asks for the scheduler's budget, a viewer
 * for its CPU budget less one, each only when what it wants changes, so that an engine dropped for
 * another's count comes back under that count instead of asking for its own again, and two holders
 * never take turns rebuilding each other. A viewer keeps the interface responsive through its
 * worker's low priority (kRefineNice), which the delegate's threads inherit, not through a count.
 *
 * A change of the count tells every holder, which drops its Cycles engines at its next boundary:
 * USDRender's thread after the render under way, a viewer before its next pass. A session opens
 * only while none lives under another count; until then its holder retries at its boundaries.
 **/
class CyclesThreads {
public:
    static CyclesThreads& instance()
    {
        static CyclesThreads* threads = new CyclesThreads;

        return *threads;
    }

    // onChange runs on the thread that changed the count, and must not call back into this.
    int addHolder(std::function<void()> onChange)
    {
        std::lock_guard<std::mutex> notify(_notifyMutex);
        std::lock_guard<std::mutex> l(_mutex);
        _holders[++_lastHolder] = std::move(onChange);

        return _lastHolder;
    }

    // No onChange of id runs once this returns.
    void removeHolder(int id)
    {
        std::lock_guard<std::mutex> notify(_notifyMutex);
        std::lock_guard<std::mutex> l(_mutex);
        _holders.erase(id);
    }

    void ask(int threads)
    {
        std::lock_guard<std::mutex> notify(_notifyMutex);
        std::map<int, std::function<void()>> holders;
        {
            std::lock_guard<std::mutex> l(_mutex);
            if (threads == _count) {
                return;
            }
            _count = threads;
            holders = _holders;
        }
        for (std::map<int, std::function<void()>>::const_iterator it = holders.begin(); it != holders.end(); ++it) {
            it->second();
        }
    }

    // 0 for any.
    int count() const
    {
        std::lock_guard<std::mutex> l(_mutex);

        return _count;
    }

    // Counts a session about to be made under count(), which *threads gets; false, counting
    // nothing, while one lives under another count.
    bool open(int* threads)
    {
        std::lock_guard<std::mutex> l(_mutex);
        if ((_live > 0) && (_liveThreads != _count)) {
            return false;
        }
        if (_live == 0) {
            _liveThreads = _count;
        }
        ++_live;
        *threads = _count;

        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> l(_mutex);
        --_live;
    }

private:
    // Held while holders are told, so that removeHolder() waits for a call under way.
    std::mutex _notifyMutex;
    mutable std::mutex _mutex;
    int _count = 0;
    int _live = 0;
    int _liveThreads = 0;
    int _lastHolder = 0;
    std::map<int, std::function<void()>> _holders;
};

// Closes the engine's Cycles session (CyclesThreads::open()) after the engine, which ends it, goes.
struct EngineDeleter {
    bool cyclesSession = false;

    void operator()(UsdImagingGLEngine* engine) const
    {
        delete engine;
        if (cyclesSession) {
            CyclesThreads::instance().close();
        }
    }
};

typedef std::unique_ptr<UsdImagingGLEngine, EngineDeleter> EnginePtr;

/**
 * A new engine of parameters. A Cycles one (cycles), whose session the caller has opened, is made
 * so that its CPU device renders on threads threads, and closes the session when it goes.
 **/
EnginePtr
newEngine(const UsdImagingGLEngine::Parameters& parameters,
          bool cycles,
          int threads)
{
    EnginePtr engine(nullptr, EngineDeleter { cycles });
    const std::function<void()> make = [&]() { engine.reset(new UsdImagingGLEngine(parameters)); };

    if (!cycles) {
        make();

        return engine;
    }
    try {
        CyclesAovs::withThreadBudget(threads, make);
    } catch (...) {
        if (!engine) {
            CyclesThreads::instance().close();
        }
        throw;
    }

    return engine;
}

struct EngineEntry {
    // The render delegate the engine was made with, and the name its errors give it.
    std::string renderer;
    std::string rendererName;
    // The stage the engine shows, for the entry's whole life; each render syncs it to the
    // request's scene. Declared first so that the engine, which reads it, goes first.
    std::unique_ptr<SceneStage> stage;
    EnginePtr engine;
    // Whether the engine lights the stage with a light at the camera.
    bool headlight = false;
    // The render settings the engine was made with: the request's, setRendererSettings()'s on top.
    std::map<std::string, VtValue> settings;
    // The thread budget a MoonRay engine was made with: its Arras session takes its cores when it starts.
    int threads = 0;
    // The AOV depth is read from, and whether the delegate renders the normal AOV, which only some
    // do, and whether it writes that in camera space rather than Hydra's world space.
    TfToken depthAov;
    bool normal = false;
    bool normalInCameraSpace = false;
    // What DelegateSupport finds the delegate cannot render of the stage, and whether that was
    // found since the stage or the delegate last changed.
    std::string supportWarning;
    bool supportChecked = false;
    // Whether the stage authors values that change in time, found at the stage edit and write
    // counts beside it; unknown until a render at another time asks.
    bool timeVarying = false;
    bool timeVaryingKnown = false;
    U64 timeVaryingEdits = 0;
    U64 timeVaryingWrites = 0;
    // When the entry last rendered, by the thread's render serial, for the LRU.
    std::uint64_t lastUse = 0;
    // The GPU memory the engine held after its last render.
    std::size_t bytes = 0;

    // What the engine last rendered, which its AOVs hold until it renders again.
    bool rendered = false;
    std::uint64_t serial = 0;
    U64 writeCount = 0;
    // What the delegate could not render of that render's stage, by DelegateSupport and by its
    // log, and the errors it logged naming no prim of it; empty for a delegate that reports neither.
    std::string warning;
    std::string errors;
    UsdTimeCode time;
    GfMatrix4d view;
    GfMatrix4d projection;
    int width = 0;
    int height = 0;

    // That render's primId plane and the paths of its hashes, decoded on the CPU when first asked for.
    bool idsDecoded = false;
    std::vector<float> primId;
    std::map<std::uint32_t, std::string> primPaths;

    // A CPU engine's render converted by cpuFrame(), for the windows of the render with serial
    // cpuSerial, so tiles and planes do not each convert the whole frame.
    std::uint64_t cpuSerial = 0;
    HydraRender::Result cpuResult;
};

// Packs a window of a plane of Storm's AOVs into a buffer, the one conversion every way USDRender
// hands its pixels over goes through, so they all give the same bits. Depth goes from the window
// depth back to camera space in double precision, as the inverse projection's third and fourth
// columns give it; primId is a copy of the plane decoded on the CPU.
const char* const kPackShader = R"GLSL(#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) writeonly buffer OutPixels { float outPixels[]; };
layout(std430, binding = 1) readonly buffer Ids { uint ids[]; };
layout(binding = 0) uniform sampler2D colorAov;
layout(binding = 1) uniform sampler2D depthAov;
uniform ivec4 windowRect;
uniform ivec2 renderSize;
uniform int plane;
uniform int nComps;
uniform dvec4 inverseZ;
uniform dvec4 inverseW;

void main()
{
    const ivec2 at = ivec2(gl_GlobalInvocationID.xy);
    const int width = windowRect.z - windowRect.x;
    if ((at.x >= width) || (at.y >= windowRect.w - windowRect.y)) {
        return;
    }
    const ivec2 pixel = windowRect.xy + at;
    const bool inside = all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, renderSize));
    const uint base = uint(at.y * width + at.x) * uint(nComps);
    if (plane == 0) {
        const vec4 rgba = inside ? texelFetch(colorAov, pixel, 0) : vec4(0.0);
        for (int c = 0; c < nComps; ++c) {
            const int channel = (nComps == 1) ? 3 : c;
            outPixels[base + uint(c)] = (channel < 4) ? rgba[channel] : 0.0;
        }
    } else if (plane == 1) {
        float value = uintBitsToFloat(0x7f800000u);
        if (inside) {
            const double depth = double(texelFetch(depthAov, pixel, 0).r);
            if (depth < 1.0LF) {
                precise dvec4 ndc = dvec4((double(pixel.x) + 0.5LF) / double(renderSize.x) * 2.0LF - 1.0LF,
                                          (double(pixel.y) + 0.5LF) / double(renderSize.y) * 2.0LF - 1.0LF,
                                          depth * 2.0LF - 1.0LF,
                                          1.0LF);
                precise double z = ndc.x * inverseZ.x + ndc.y * inverseZ.y + ndc.z * inverseZ.z + inverseZ.w;
                precise double w = ndc.x * inverseW.x + ndc.y * inverseW.y + ndc.z * inverseW.z + inverseW.w;
                value = float(-z / w);
            }
        }
        for (int c = 0; c < nComps; ++c) {
            outPixels[base + uint(c)] = value;
        }
    } else {
        const float value = inside ? uintBitsToFloat(ids[uint(pixel.y * renderSize.x + pixel.x)]) : 0.0;
        for (int c = 0; c < nComps; ++c) {
            outPixels[base + uint(c)] = value;
        }
    }
}
)GLSL";

// Drains GL's errors; the first, or GL_NO_ERROR.
GLenum
takeGlError()
{
    GLenum first = GL_NO_ERROR;

    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
        if (first == GL_NO_ERROR) {
            first = e;
        }
    }

    return first;
}

// The light at the camera usdview gives a stage, which the request may ask for.
void
setHeadlight(UsdImagingGLEngine* engine)
{
    GlfSimpleLight light;

    light.SetIsCameraSpaceLight(true);
    light.SetPosition(GfVec4f(0.f, 0.f, 0.f, 1.f));
    light.SetAmbient(GfVec4f(0.f, 0.f, 0.f, 1.f));
    light.SetDiffuse(GfVec4f(1.f, 1.f, 1.f, 1.f));
    light.SetSpecular(GfVec4f(0.2f, 0.2f, 0.2f, 1.f));
    GlfSimpleMaterial material;
    material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.f));
    material.SetSpecular(GfVec4f(0.1f, 0.1f, 0.1f, 1.f));
    material.SetShininess(32.);
    engine->SetLightingState(GlfSimpleLightVector(1, light), material, GfVec4f(0.01f, 0.01f, 0.01f, 1.f));
}

/**
 * A thread renders run on, and what only it touches: Storm's has the GL context, its engines and
 * the framebuffer Storm renders under; the other delegates' has their engines and no GL. Never
 * destroyed: at exit the thread is still waiting for work, and the context and engines go with
 * the process.
 **/
class RenderThread {
public:
    static RenderThread& storm()
    {
        static RenderThread* thread = new RenderThread(true);

        return *thread;
    }

    static RenderThread& cpu()
    {
        static RenderThread* thread = new RenderThread(false);

        return *thread;
    }

    bool usesGl() const
    {
        return _gl;
    }

    // Queues job; starts the thread if it has not started and start is set. False if the
    // thread was not running and start is not set: the job is dropped.
    bool post(std::function<void()> job,
              bool start)
    {
        std::call_once(_registered, [this]() {
            // Never unregistered: the thread lives as long as the process.
            (void)SceneWriteBarrier::registerParticipant([this]() { pause(); }, [this]() { resume(); });
        });
        {
            std::lock_guard<std::mutex> l(_mutex);
            if (!_started) {
                if (!start) {
                    return false;
                }
                _started = true;
                std::thread(&RenderThread::run, this).detach();
            }
            _jobs.push_back(std::move(job));
        }
        _wake.notify_one();

        return true;
    }

    /**
     * @brief Runs job on the thread and waits for it. False, with *error set if error is not
     * null, if job threw: the exception stops at the thread, which goes on to its next job.
     **/
    template <typename Fn>
    bool runAndWait(Fn job,
                    std::string* error,
                    const std::function<void()>& whileWaiting = std::function<void()>())
    {
        std::promise<std::string> done;
        std::future<std::string> finished = done.get_future();
        post([&job, &done]() {
            std::string failure;
            try {
                job();
            } catch (const std::exception& e) {
                failure = std::string("USDRender's render thread failed: ") + e.what();
            } catch (...) {
                failure = "USDRender's render thread failed.";
            }
            done.set_value(failure);
        },
             true);
        if (whileWaiting) {
            while (finished.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
                whileWaiting();
            }
        }
        const std::string failure = finished.get();
        if (failure.empty()) {
            return true;
        }
        if (error) {
            *error = failure;
        }

        return false;
    }

    // The members below are the thread's alone.

    bool ensureContext(std::string* error)
    {
        if (!_contextTried) {
            _contextTried = true;
            HydraGLContextFactory factory;
            {
                std::lock_guard<std::mutex> l(gFactoryMutex);
                factory = gFactory;
            }
            std::string message;
            if (factory) {
                _context = factory(&message);
            } else {
                _context = HeadlessGLContext::create(&message);
            }
            if (!_context) {
                _contextError = message.empty() ? std::string("USDRender could not make an OpenGL 4.5 compatibility context.") : message;
            } else if (!_context->makeCurrent()) {
                _contextError = "USDRender could not make its OpenGL context current (" + _context->description() + ").";
                _context.reset();
            } else if (!GarchGLApiLoad()) {
                _contextError = "USDRender could not load the OpenGL entry points of its context (" + _context->description() + ").";
                _context.reset();
            } else {
                // Before HgiGL and Storm compile anything on this thread: the factory's own guard has ended.
                const GLubyte* renderer = glGetString(GL_RENDERER);
                noteSoftwareGLRenderer(renderer ? reinterpret_cast<const char*>(renderer) : "");
                // An HgiGL that goes collects every thread's garbage.
                StormGLSection section;
                _hgi = Hgi::CreateNamedHgi(HgiTokens->OpenGL);
                if (!_hgi || !_hgi->IsBackendSupported()) {
                    _contextError = "Storm cannot draw through OpenGL on " + _context->description() + ".";
                    _hgi.reset();
                    _context.reset();
                }
            }
        }
        if (!_context) {
            *error = _contextError;

            return false;
        }
        if (!_context->makeCurrent()) {
            *error = "USDRender could not make its OpenGL context current (" + _context->description() + ").";

            return false;
        }

        return true;
    }

    std::string contextDescription() const
    {
        return _context ? _context->description() : std::string();
    }

    // The setting key of owner's engine; false if owner has none.
    bool engineSetting(const void* owner,
                       const TfToken& key,
                       VtValue* value) const
    {
        std::map<const void*, EngineEntry>::const_iterator found = _engines.find(owner);
        if ((found == _engines.end()) || !found->second.engine) {
            return false;
        }
        *value = found->second.engine->GetRendererSetting(key);

        return true;
    }

    void releaseEngine(const void* owner)
    {
        std::map<const void*, EngineEntry>::iterator found = _engines.find(owner);
        if (found == _engines.end()) {
            return;
        }
        std::optional<StormGLSection> section;
        if (_gl) {
            section.emplace();
        }
        if (_gl && (!_context || !_context->makeCurrent())) {
            // Its GL objects cannot be freed without the context; leak them with the engine.
            (void)found->second.engine.release();
        } else if (_gl) {
            section->collectWith(_hgi.get());
        }
        _engines.erase(found);
        _engineCount = _engines.size();
    }

    void releaseAllEngines()
    {
        while (!_engines.empty()) {
            releaseEngine(_engines.begin()->first);
        }
    }

    /**
     * Drops every Cycles engine unless they were all made under threads. A Cycles session sizes
     * its CPU device once, when it is made, from TaskScheduler::max_concurrency(), which is the
     * TBB arena concurrency the first live session was made in for as long as any session lives;
     * nothing changes it afterwards. So a session follows a new budget only if it is made when no
     * other lives. The engines keep their stages, and the next render of each makes it again.
     **/
    void dropCyclesEnginesUnlessMadeFor(int threads)
    {
        if (threads == _cyclesThreads) {
            return;
        }
        for (std::map<const void*, EngineEntry>::iterator it = _engines.begin(); it != _engines.end(); ++it) {
            if (it->second.engine && CyclesAovs::isCycles(it->second.renderer)) {
                it->second.engine.reset();
                it->second.rendered = false;
            }
        }
        _cyclesThreads = threads;
    }

    // Asks CyclesThreads for threads if this thread last asked for another count, and drops its
    // Cycles engines made under another count than the one in force.
    void followCyclesThreads(int threads)
    {
        if (!_cyclesHolder) {
            _cyclesHolder = CyclesThreads::instance().addHolder([this]() {
                (void)post([this]() { dropCyclesEnginesUnlessMadeFor(CyclesThreads::instance().count()); }, false);
            });
        }
        if (threads != _cyclesAsked) {
            _cyclesAsked = threads;
            CyclesThreads::instance().ask(threads);
        }
        dropCyclesEnginesUnlessMadeFor(CyclesThreads::instance().count());
    }

    /**
     * Opens a Cycles session for an engine about to be made, under the count in force, into
     * *threads. While another holder's sessions live under another count it waits for that holder
     * to drop them at its next boundary, letting barrier writes through, until cancelled.
     **/
    bool openCyclesSession(const HydraRender::Request& request,
                           const std::atomic<bool>* cancel,
                           int* threads,
                           std::string* error)
    {
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        for (;;) {
            dropCyclesEnginesUnlessMadeFor(CyclesThreads::instance().count());
            if (CyclesThreads::instance().open(threads)) {
                _cyclesThreads = *threads;

                return true;
            }
            if (cancel && cancel->load()) {
                *error = "The Cycles render was cancelled.";

                return false;
            }
            if (!request.aborted && (std::chrono::steady_clock::now() - started > kUnabortableRenderLimit)) {
                *error = "Cycles' threads were not released by the other renders within " + std::to_string(kUnabortableRenderLimit.count()) + " seconds.";

                return false;
            }
            yieldToBarrier();
            std::this_thread::sleep_for(kConvergencePoll);
        }
    }

    // Drops the least recently used engines other than keep's while they hold more than the budget.
    void enforceBudget(const void* keep)
    {
        const std::size_t budget = gMemoryBudget.load();
        for (;;) {
            std::size_t total = 0;
            std::map<const void*, EngineEntry>::iterator oldest = _engines.end();
            for (std::map<const void*, EngineEntry>::iterator it = _engines.begin(); it != _engines.end(); ++it) {
                total += it->second.bytes;
                if ((it->first != keep) && ((oldest == _engines.end()) || (it->second.lastUse < oldest->second.lastUse))) {
                    oldest = it;
                }
            }
            if ((total <= budget) || (oldest == _engines.end())) {
                return;
            }
            releaseEngine(oldest->first);
        }
    }

    std::size_t engineCount() const
    {
        return _engineCount.load();
    }

    // Renders request with its owner's engine, or reuses the engine's last render of the same
    // request; null with *error set on failure. cancel, when set, stops a render of a delegate
    // other than Storm between its passes.
    EngineEntry* renderEngine(const HydraRender::Request& request,
                              const std::atomic<bool>* cancel,
                              std::string* error);

    bool makeEngine(EngineEntry& entry,
                    const HydraRender::Request& request,
                    const std::string& renderer,
                    const RendererSettings& settings,
                    const std::atomic<bool>* cancel,
                    std::string* error);

    // The whole frame's planes of entry's render, as pack() gives them, or converted on the CPU
    // when the context cannot pack.
    bool frame(EngineEntry& entry,
               HydraRender::Result* result,
               std::string* error);

    bool decodeIds(EngineEntry& entry,
                   std::string* error);

    bool packAvailable(std::string* error);

    bool pack(EngineEntry& entry,
              const HydraRender::PlaneWindow& window,
              GLuint buffer,
              std::string* error);

    // window of entry's render read back into *pixels.
    bool readWindow(EngineEntry& entry,
                    const HydraRender::PlaneWindow& window,
                    std::vector<float>* pixels,
                    std::string* error);

    void* getProcAddress(const char* name) const
    {
        return _context ? _context->getProcAddress(name) : nullptr;
    }

    // Makes the context current if it was made; false if it was not or cannot be.
    bool makeCurrentIfMade()
    {
        return _context && _context->makeCurrent();
    }

private:
    bool cpuFrame(EngineEntry& entry,
                  HydraRender::Result* result,
                  std::string* error);

    // window of entry's render cut from cpuFrame()'s planes, as pack() would give it.
    bool cpuWindow(EngineEntry& entry,
                   const HydraRender::PlaneWindow& window,
                   std::vector<float>* pixels,
                   std::string* error);

    // The GL name of Storm's resolved texture of AOV name, or 0 with *error set.
    GLuint aovTexture(EngineEntry& entry,
                      const TfToken& name,
                      std::string* error);

    explicit RenderThread(bool gl)
        : _gl(gl)
    {
    }

    // Storm's thread enters the process's HgiGL section, collecting with its Hgi. The other
    // delegates' thread draws nothing through HgiGL, and must not hold the section through a
    // render that runs until it converges.
    void enterGLSection(std::optional<StormGLSection>* section)
    {
        if (_gl) {
            section->emplace();
            (*section)->collectWith(_hgi.get());
        }
    }

    // Lets a barrier write that is waiting for the running job through, then carries on: called by
    // a long render between its passes, when it is not reading the scene's layers.
    void yieldToBarrier()
    {
        std::unique_lock<std::mutex> l(_mutex);
        if (!_paused) {
            return;
        }
        _busy = false;
        _idle.notify_all();
        _wake.wait(l, [this]() { return !_paused; });
        _busy = true;
    }

    // The barrier's quiesce: waits for the running job and holds the queue until resume().
    void pause()
    {
        std::unique_lock<std::mutex> l(_mutex);
        _paused = true;
        _idle.wait(l, [this]() { return !_busy; });
    }

    void resume()
    {
        {
            std::lock_guard<std::mutex> l(_mutex);
            _paused = false;
        }
        _wake.notify_one();
    }

    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(_mutex);
                _wake.wait(l, [this]() { return !_paused && !_jobs.empty(); });
                job = std::move(_jobs.front());
                _jobs.pop_front();
                _busy = true;
            }
            // A job that does not wait has no one to report to; its exception must not end the
            // thread.
            try {
                job();
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> l(_mutex);
                _busy = false;
            }
            _idle.notify_all();
        }
    }

    // Binds a framebuffer of width x height: Storm draws into its own AOV textures, but HgiGL's
    // draws fail as an invalid framebuffer operation when the bound one is incomplete, as the
    // default framebuffer of a context without a surface is.
    void bindFramebuffer(int width,
                         int height)
    {
        if (!_framebuffer) {
            glGenFramebuffers(1, &_framebuffer);
            glGenRenderbuffers(2, _renderbuffers);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        if ((width != _framebufferWidth) || (height != _framebufferHeight)) {
            glBindRenderbuffer(GL_RENDERBUFFER, _renderbuffers[0]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _renderbuffers[0]);
            glBindRenderbuffer(GL_RENDERBUFFER, _renderbuffers[1]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _renderbuffers[1]);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            _framebufferWidth = width;
            _framebufferHeight = height;
        }
        glViewport(0, 0, width, height);
    }

    // Storm's thread, with the GL context; the other delegates' has none.
    const bool _gl;
    std::once_flag _registered;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::condition_variable _idle;
    std::deque<std::function<void()>> _jobs;
    bool _started = false;
    // A barrier write is in progress: no job starts.
    bool _paused = false;
    // A job is running.
    bool _busy = false;

    std::unique_ptr<HydraGLContext> _context;
    bool _contextTried = false;
    std::string _contextError;
    // The one Hgi every engine draws through, made with the context; declared before the engines,
    // which must go first.
    HgiUniquePtr _hgi;
    std::map<const void*, EngineEntry> _engines;
    std::atomic<std::size_t> _engineCount { 0 };
    // The thread budget every live Cycles engine was made under; see dropCyclesEnginesUnlessMadeFor().
    int _cyclesThreads = 0;
    // The count this thread last asked CyclesThreads for, -1 before it asked, and its holder id.
    int _cyclesAsked = -1;
    int _cyclesHolder = 0;
    GLuint _framebuffer = 0;
    GLuint _renderbuffers[2] = { 0, 0 };
    std::uint64_t _renderSerial = 0;
    std::uint64_t _useSerial = 0;
    // The pack program: 0 untried, 1 built, -1 failed with _packError.
    int _packState = 0;
    std::string _packError;
    GLuint _packProgram = 0;
    GLuint _packSampler = 0;
    GLint _packUniforms[6] = { -1, -1, -1, -1, -1, -1 };
    // The primId plane of the render of serial _idsSerial, for pack().
    GLuint _idsBuffer = 0;
    std::uint64_t _idsSerial = 0;
    GLuint _readBuffer = 0;
    int _framebufferWidth = 0;
    int _framebufferHeight = 0;
};

// The AOV buffer of name, resolved and mapped, or null with *error set. Unmaps on destruction.
class MappedAov {
public:
    MappedAov(const EngineEntry& entry,
              const TfToken& name,
              std::string* error)
    {
        map(entry.engine.get(), entry.rendererName, entry.width, entry.height, name, error);
    }

    MappedAov(UsdImagingGLEngine* engine,
              const std::string& rendererName,
              int width,
              int height,
              const TfToken& name,
              std::string* error)
    {
        map(engine, rendererName, width, height, name, error);
    }

    ~MappedAov()
    {
        if (_buffer) {
            _buffer->Unmap();
        }
    }

    bool valid() const
    {
        return _data != nullptr;
    }

    HdFormat format() const
    {
        return _format;
    }

    const unsigned char* pixel(std::size_t index) const
    {
        return _data + index * _stride;
    }

private:
    void map(UsdImagingGLEngine* engine,
             const std::string& renderer,
             int width,
             int height,
             const TfToken& name,
             std::string* error)
    {
        _buffer = engine->GetAovRenderBuffer(name);
        if (!_buffer) {
            *error = renderer + " rendered no " + name.GetString() + " AOV.";

            return;
        }
        _buffer->Resolve();
        if (((int)_buffer->GetWidth() != width) || ((int)_buffer->GetHeight() != height)) {
            *error = renderer + "'s " + name.GetString() + " AOV is not the size asked for.";
            _buffer = nullptr;

            return;
        }
        _data = static_cast<const unsigned char*>(_buffer->Map());
        if (!_data) {
            *error = renderer + "'s " + name.GetString() + " AOV could not be read back.";
            _buffer = nullptr;

            return;
        }
        _format = _buffer->GetFormat();
        _stride = HdDataSizeOfFormat(_format);
    }

    HdRenderBuffer* _buffer = nullptr;
    const unsigned char* _data = nullptr;
    HdFormat _format = HdFormatInvalid;
    std::size_t _stride = 0;
};

float
componentToFloat(HdFormat componentFormat,
                 const unsigned char* at)
{
    switch (componentFormat) {
    case HdFormatUNorm8:
        return *at / 255.f;
    case HdFormatSNorm8:
        return std::max(-1.f, *reinterpret_cast<const signed char*>(at) / 127.f);
    case HdFormatFloat16: {
        GfHalf value;
        std::memcpy(&value, at, sizeof(value));

        return float(value);
    }
    case HdFormatFloat32: {
        float value;
        std::memcpy(&value, at, sizeof(value));

        return value;
    }
    case HdFormatInt32: {
        std::int32_t value;
        std::memcpy(&value, at, sizeof(value));

        return (float)value;
    }
    default:
        return 0.f;
    }
}

std::int32_t
readInt(const unsigned char* at)
{
    std::int32_t value;
    std::memcpy(&value, at, sizeof(value));

    return value;
}

/**
 * Makes entry's engine again for request, with renderer under settings, its stage kept. False
 * with *error set if it could not be made; entry may then have been erased.
 **/
bool
RenderThread::makeEngine(EngineEntry& entry,
                         const HydraRender::Request& request,
                         const std::string& renderer,
                         const RendererSettings& settings,
                         const std::atomic<bool>* cancel,
                         std::string* error)
{
    const bool cycles = CyclesAovs::isCycles(renderer);
    entry.engine.reset();
    entry.rendered = false;
    // Named even for Storm: with no id the engine takes USD's default delegate, the first by
    // plugInfo priority that loads, which need not be Storm.
    UsdImagingGLEngine::Parameters parameters;
    parameters.rendererPluginId = TfToken(renderer);
    if (_gl) {
        parameters.driver.name = HgiTokens->renderDriver;
        parameters.driver.driver = VtValue(_hgi.get());
    } else {
        // No Hgi is made without the GPU: the delegate's AOVs are host buffers, and nothing
        // touches GL.
        parameters.gpuEnabled = false;
    }
    CyclesAovs::registerSceneIndices();
    MoonRayAovs::registerSceneIndices();
    int threads = 0;
    if (cycles && !openCyclesSession(request, cancel, &threads, error)) {
        return false;
    }
    std::string startError;
    {
        TfErrorMark mark;
        entry.engine = newEngine(parameters, cycles, threads);
        ++gEngineBuilds;
        // The engine reports a delegate it could not start as coding errors and renders nothing.
        if (entry.engine->GetCurrentRendererId() != TfToken(renderer)) {
            for (TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
                startError += (startError.empty() ? ": " : "; ") + it->GetCommentary();
            }
            mark.Clear();
            startError = "The renderer " + entry.rendererName + " could not be started" + startError + ".";
        }
    }
    if (!startError.empty()) {
        *error = startError;
        _engines.erase(request.owner);
        _engineCount = _engines.size();

        return false;
    }
    if (_gl) {
        applyStormPtexBudget(_hgi.get());
    }
    entry.engine->SetEnablePresentation(false);
    for (RendererSettings::const_iterator it = settings.begin(); it != settings.end(); ++it) {
        entry.engine->SetRendererSetting(TfToken(it->first), it->second);
    }
    // Last: a prim's settings must not override these.
    if (CyclesAovs::isCycles(renderer)) {
        CyclesAovs::configureEngine(entry.engine.get());
    } else if (MoonRayAovs::isMoonRay(renderer)) {
        MoonRayAovs::configureEngine(entry.engine.get(), request.threads);
    }
    entry.threads = request.threads;
    entry.settings = settings;
    TfTokenVector aovs = { HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId };
    entry.depthAov = HdAovTokens->depth;
    entry.normal = false;
    entry.normalInCameraSpace = false;
    if (MoonRayAovs::isMoonRay(renderer)) {
        aovs = MoonRayAovs::aovs();
        entry.depthAov = MoonRayAovs::depthAov();
        entry.normal = true;
        entry.normalInCameraSpace = true;
    } else if (!_gl) {
        const TfTokenVector offered = entry.engine->GetRendererAovs();
        entry.normal = std::find(offered.begin(), offered.end(), HdAovTokens->normal) != offered.end();
        if (entry.normal) {
            aovs.push_back(HdAovTokens->normal);
        }
    }
    if (!entry.engine->SetRendererAovs(aovs)) {
        std::string names;
        for (std::size_t i = 0; i < aovs.size(); ++i) {
            if (i > 0) {
                names += (i + 1 == aovs.size()) ? " and " : ", ";
            }
            names += aovs[i].GetString();
        }
        *error = _gl ? "Storm on " + contextDescription() + " offers no " + names + " AOVs."
                     : entry.rendererName + " offers no " + names + " AOVs.";
        _engines.erase(request.owner);
        _engineCount = _engines.size();

        return false;
    }
    entry.headlight = request.headlight;
    if (entry.headlight) {
        setHeadlight(entry.engine.get());
    }

    return true;
}

EngineEntry*
RenderThread::renderEngine(const HydraRender::Request& request,
                           const std::atomic<bool>* cancel,
                           std::string* error)
{
    if (_gl && !ensureContext(error)) {
        return nullptr;
    }
    const int width = request.width;
    const int height = request.height;
    const std::string renderer = request.renderer.empty() ? std::string(kRenderDelegateStormId) : request.renderer;

    EngineEntry& entry = _engines[request.owner];
    _engineCount = _engines.size();
    const bool rendererChanged = (entry.renderer != renderer);
    if (rendererChanged) {
        RenderDelegates::Delegate delegate;
        entry.renderer = renderer;
        entry.rendererName = RenderDelegates::find(renderer, &delegate) ? delegate.displayName : renderer;
    }
    if (!entry.stage) {
        entry.stage.reset(new SceneStage);
        if (!entry.stage->isValid()) {
            _engines.erase(request.owner);
            _engineCount = _engines.size();
            *error = "USD could not open a stage to render.";

            return nullptr;
        }
    }
    const bool edited = entry.stage->sync(request.scene);
    if (edited || rendererChanged) {
        entry.supportWarning.clear();
        entry.supportChecked = false;
    }
    std::optional<StormGLSection> section;
    enterGLSection(&section);
    // A Storm engine goes inside the section: its HgiGL garbage is shared with the main thread's.
    if (rendererChanged && entry.engine) {
        entry.engine.reset();
        entry.rendered = false;
    }
    const UsdStageRefPtr& stage = entry.stage->getStage();
    RendererSettings settings = request.settings;
    for (const std::pair<const std::string, VtValue>& setting : rendererSettings(renderer)) {
        settings[setting.first] = setting.second;
    }
    const bool cycles = CyclesAovs::isCycles(renderer);
    if (cycles) {
        followCyclesThreads(request.threads);
    }
    // A headlight once set cannot be taken back, so the engine is made again when the request
    // drops it; and some settings, such as Cycles' integrator's, restart a render only when they
    // change, so an engine is made again under new ones too, as a MoonRay engine is under a new
    // thread budget.
    const bool budgetChanged = MoonRayAovs::isMoonRay(renderer) && (request.threads != entry.threads);
    bool freshEngine = false;
    if (!entry.engine || (request.headlight != entry.headlight) || (settings != entry.settings) || budgetChanged) {
        if (!makeEngine(entry, request, renderer, settings, cancel, error)) {
            return nullptr;
        }
        freshEngine = true;
    }
    UsdImagingGLEngine* engine = entry.engine.get();
    const GfMatrix4d view = request.frustum.ComputeViewMatrix();
    const GfMatrix4d projection = request.frustum.ComputeProjectionMatrix();
    const U64 writeCount = SceneWriteBarrier::getWriteCount();
    entry.lastUse = ++_useSerial;
    if (entry.rendered && !edited && (entry.writeCount == writeCount) && (entry.time == request.time) && (entry.view == view) && (entry.projection == projection) && (entry.width == width) && (entry.height == height)) {
        return &entry;
    }
    // Cycles shows a restart in its progress only once a change reaches it: a request changing
    // nothing it sees would wait out kRestartUnseen for one, then make the engine again.
    bool watchRestart = false;
    if (!freshEngine && RestartWatch::watches(renderer)) {
        watchRestart = !entry.rendered || edited || (entry.writeCount != writeCount) || (entry.view != view) || (entry.projection != projection) || (entry.width != width) || (entry.height != height);
        if (!watchRestart && (entry.time != request.time)) {
            const U64 edits = entry.stage->getEditCount();
            if (!entry.timeVaryingKnown || (entry.timeVaryingEdits != edits) || (entry.timeVaryingWrites != writeCount)) {
                entry.timeVarying = mayVaryInTime(stage);
                entry.timeVaryingKnown = true;
                entry.timeVaryingEdits = edits;
                entry.timeVaryingWrites = writeCount;
            }
            watchRestart = entry.timeVarying;
        }
    }
    entry.rendered = false;
    entry.idsDecoded = false;
    entry.primId.clear();
    entry.primPaths.clear();

    if (_gl) {
        bindFramebuffer(width, height);
    }
    engine->SetRenderBufferSize(GfVec2i(width, height));
    engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
    engine->SetCameraState(view, projection);

    UsdImagingGLRenderParams params;
    params.frame = request.time;
    params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    params.enableLighting = true;
    params.enableSceneLights = true;
    params.enableSceneMaterials = true;
    params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
    // The colour AOV is read as Storm shades it, in the scene's linear space.
    params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
    params.highlight = false;
    std::string renderFailure;
    if (_gl) {
        for (int pass = 0; pass < kMaxRenderPasses; ++pass) {
            if (!renderGuarded(engine, stage->GetPseudoRoot(), params, &renderFailure)) {
                break;
            }
            if (engine->IsConverged()) {
                break;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        // A path tracer renders on threads of its own and converges when it is done; each Render()
        // syncs the stage's changes into it. MoonRay's log is taken off the console for the
        // render, after the engine, whose making loads the libraries that print it.
        std::unique_ptr<MoonRayAovs::LogCapture> log;
        if (MoonRayAovs::isMoonRay(renderer)) {
            log.reset(new MoonRayAovs::LogCapture);
        }
        // Without an abort callback nothing could stop a delegate that never converges.
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point restarted = started;
        RestartWatch restartWatch;
        restartWatch.restart(!watchRestart);
        if (watchRestart) {
            restartWatch.before(engine->GetRenderStats(), 0);
        }
        for (;;) {
            if (!renderGuarded(engine, stage->GetPseudoRoot(), params, &renderFailure)) {
                break;
            }
            // The progress is read before the convergence: once it shows the new render, the
            // delegate's word that it converged is about that render.
            const VtDictionary stats = engine->GetRenderStats();
            const bool restartSeen = restartWatch.after(samplesOf(stats, 0), clockOf(stats));
            if (restartSeen && engine->IsConverged()) {
                break;
            }
            // A MoonRay session that could not start or broke never converges.
            const std::string failure = log ? MoonRayAovs::sessionFailure(stats, log->notices()) : std::string();
            if (!failure.empty()) {
                *error = failure;
                entry.engine.reset();
                entry.rendered = false;

                return nullptr;
            }
            if (!request.aborted && (std::chrono::steady_clock::now() - started > kUnabortableRenderLimit)) {
                *error = "The " + entry.rendererName + " render did not converge within " + std::to_string(kUnabortableRenderLimit.count()) + " seconds.";
                entry.engine.reset();
                entry.rendered = false;

                return nullptr;
            }
            if (cancel && cancel->load()) {
                *error = "The " + entry.rendererName + " render was cancelled.";
                // Its threads would go on rendering the cancelled frame. Cycles has no stop: its
                // render pass cancels the session and its session joins its thread as the engine
                // goes. Nor has MoonRay: its Arras session ends its render process as the engine
                // goes. The next render makes the engine again.
                if (engine->IsStopRendererSupported()) {
                    (void)engine->StopRenderer();
                }
                entry.engine.reset();
                entry.rendered = false;

                return nullptr;
            }
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (!restartSeen && !restartWatch.restarting() && (now - restarted >= kRestartUnseen)) {
                // A new engine has no pixels from before to mistake for this render's.
                if (!makeEngine(entry, request, renderer, settings, cancel, error)) {
                    return nullptr;
                }
                engine = entry.engine.get();
                engine->SetRenderBufferSize(GfVec2i(width, height));
                engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
                engine->SetCameraState(view, projection);
                if (log) {
                    // What it kept of the session dropped must not fail the new one.
                    log.reset();
                    log.reset(new MoonRayAovs::LogCapture);
                }
                restartWatch.restart(true);
                restarted = now;
                continue;
            }
            yieldToBarrier();
            std::this_thread::sleep_for(kConvergencePoll);
        }
        // Only Cycles is probed: making a delegate to ask it its types may start a render.
        if (!entry.supportChecked) {
            entry.supportWarning = CyclesAovs::isCycles(renderer) ? DelegateSupport::warningText(stage, CyclesAovs::support(), entry.rendererName) : std::string();
            entry.supportChecked = true;
        }
        entry.warning = entry.supportWarning;
        entry.errors.clear();
        if (log) {
            const std::vector<MoonRayAovs::Notice> notices = log->notices();
            const std::string logged = MoonRayAovs::warningText(stage, MoonRayAovs::unsupportedLights(stage, MoonRayAovs::supportedPrimTypes()), notices);
            if (!logged.empty()) {
                entry.warning += (entry.warning.empty() ? std::string() : std::string("\n")) + logged;
            }
            entry.errors = MoonRayAovs::errorText(stage, notices);
        }
    }

    if (!renderFailure.empty()) {
        *error = "The " + entry.rendererName + " render failed: " + renderFailure;
        // Hydra's state after an exception is unknown; the next render makes the engine again.
        entry.engine.reset();
        entry.rendered = false;

        return nullptr;
    }

    entry.rendered = true;
    entry.serial = ++_renderSerial;
    entry.writeCount = writeCount;
    entry.time = request.time;
    entry.view = view;
    entry.projection = projection;
    entry.width = width;
    entry.height = height;

    if (!_gl) {
        // Host memory, which the GPU budget does not count.
        entry.bytes = 0;

        return &entry;
    }
    // Storm's own count of its buffers and textures, or failing that the AOVs alone (four of four
    // bytes or less a pixel).
    std::size_t bytes = (std::size_t)width * (std::size_t)height * 16;
    const VtDictionary stats = engine->GetRenderStats();
    const VtDictionary::const_iterator used = stats.find(HdPerfTokens->gpuMemoryUsed.GetString());
    if ((used != stats.end()) && used->second.IsHolding<size_t>()) {
        bytes = std::max(bytes, used->second.UncheckedGet<size_t>());
    }
    entry.bytes = bytes;
    const void* owner = request.owner;
    enforceBudget(owner);

    return &_engines[owner];
} // RenderThread::renderEngine

bool
RenderThread::cpuFrame(EngineEntry& entry,
                       HydraRender::Result* result,
                       std::string* error)
{
    const int width = entry.width;
    const int height = entry.height;
    const GfMatrix4d& projection = entry.projection;
    const std::size_t pixels = (std::size_t)width * (std::size_t)height;

    {
        MappedAov color(entry, HdAovTokens->color, error);
        if (!color.valid()) {
            return false;
        }
        const HdFormat componentFormat = HdGetComponentFormat(color.format());
        const std::size_t components = HdGetComponentCount(color.format());
        const std::size_t componentSize = HdDataSizeOfFormat(componentFormat);
        result->rgba.assign(pixels * 4, 0.f);
        for (std::size_t i = 0; i < pixels; ++i) {
            const unsigned char* at = color.pixel(i);
            for (std::size_t c = 0; (c < components) && (c < 4); ++c) {
                result->rgba[i * 4 + c] = componentToFloat(componentFormat, at + c * componentSize);
            }
            if (components < 4) {
                result->rgba[i * 4 + 3] = 1.f;
            }
        }
    }

    {
        MappedAov depth(entry, entry.depthAov, error);
        if (!depth.valid()) {
            return false;
        }
        const HdFormat componentFormat = HdGetComponentFormat(depth.format());
        const GfMatrix4d inverse = projection.GetInverse();
        result->depth.assign(pixels, std::numeric_limits<float>::infinity());
        const bool cycles = CyclesAovs::isCycles(entry.renderer);
        const bool moonRay = MoonRayAovs::isMoonRay(entry.renderer);
        // Cycles' depth and MoonRay's cameraDepth are already the distance along the view axis.
        for (std::size_t i = 0; cycles && (i < pixels); ++i) {
            result->depth[i] = CyclesAovs::depthToDistance(componentToFloat(componentFormat, depth.pixel(i)));
        }
        for (std::size_t i = 0; moonRay && (i < pixels); ++i) {
            result->depth[i] = MoonRayAovs::cameraDepth(componentToFloat(componentFormat, depth.pixel(i)));
        }
        // Storm's depth, and that of the delegates keeping to Hydra's convention, is the window
        // depth in [0, 1]: back to the camera space, the inverse projection gives the point's
        // distance along the view axis.
        for (int y = 0; !cycles && !moonRay && (y < height); ++y) {
            const double ndcY = (y + 0.5) / height * 2. - 1.;
            for (int x = 0; x < width; ++x) {
                const std::size_t i = (std::size_t)y * (std::size_t)width + (std::size_t)x;
                const double window = componentToFloat(componentFormat, depth.pixel(i));
                if (window >= 1.) {
                    continue;
                }
                const double ndcX = (x + 0.5) / width * 2. - 1.;
                const double ndcZ = window * 2. - 1.;
                const double z = ndcX * inverse[0][2] + ndcY * inverse[1][2] + ndcZ * inverse[2][2] + inverse[3][2];
                const double w = ndcX * inverse[0][3] + ndcY * inverse[1][3] + ndcZ * inverse[2][3] + inverse[3][3];
                result->depth[i] = (float)(-z / w);
            }
        }
    }

    result->normal.clear();
    if (entry.normal) {
        // Hydra's normal AOV is in world space; the normal matrix of the view takes it to the
        // camera's. MoonRay's is in camera space already.
        MappedAov normal(entry, HdAovTokens->normal, error);
        if (!normal.valid()) {
            return false;
        }
        const HdFormat componentFormat = HdGetComponentFormat(normal.format());
        const std::size_t components = HdGetComponentCount(normal.format());
        const std::size_t componentSize = HdDataSizeOfFormat(componentFormat);
        const GfMatrix4d toCamera = entry.view.GetInverse().GetTranspose();
        result->normal.assign(pixels * 3, 0.f);
        for (std::size_t i = 0; (components >= 3) && (i < pixels); ++i) {
            const unsigned char* at = normal.pixel(i);
            GfVec3d n(componentToFloat(componentFormat, at), componentToFloat(componentFormat, at + componentSize), componentToFloat(componentFormat, at + 2 * componentSize));
            if (n.GetLengthSq() == 0.) {
                continue;
            }
            if (!entry.normalInCameraSpace) {
                n = toCamera.TransformDir(n);
            }
            n.Normalize();
            for (int c = 0; c < 3; ++c) {
                result->normal[i * 3 + (std::size_t)c] = (float)n[c];
            }
        }
    }

    result->warning = entry.warning;
    result->errors = entry.errors;

    if (!decodeIds(entry, error)) {
        return false;
    }
    result->primId = entry.primId;
    result->primPaths = entry.primPaths;

    return true;
} // RenderThread::cpuFrame

bool
RenderThread::cpuWindow(EngineEntry& entry,
                        const HydraRender::PlaneWindow& window,
                        std::vector<float>* pixels,
                        std::string* error)
{
    if (entry.cpuSerial != entry.serial) {
        HydraRender::Result converted;
        if (!cpuFrame(entry, &converted, error)) {
            return false;
        }
        entry.cpuResult = std::move(converted);
        entry.cpuSerial = entry.serial;
    }
    const HydraRender::Result& frame = entry.cpuResult;
    const int width = entry.width;
    const int height = entry.height;
    const int nComps = window.nComps;
    const int windowWidth = window.x2 - window.x1;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const bool inside = (x >= 0) && (y >= 0) && (x < width) && (y < height);
            const std::size_t i = inside ? (std::size_t)y * (std::size_t)width + (std::size_t)x : 0;
            float* out = pixels->data() + ((std::size_t)(y - window.y1) * (std::size_t)windowWidth + (std::size_t)(x - window.x1)) * (std::size_t)nComps;
            for (int c = 0; c < nComps; ++c) {
                switch (window.plane) {
                case HydraRender::ePlaneColor: {
                    const int channel = (nComps == 1) ? 3 : c;
                    out[c] = (inside && (channel < 4)) ? frame.rgba[i * 4 + (std::size_t)channel] : 0.f;
                    break;
                }
                case HydraRender::ePlaneDepth:
                    out[c] = inside ? frame.depth[i] : std::numeric_limits<float>::infinity();
                    break;
                case HydraRender::ePlanePrimId:
                    out[c] = inside ? frame.primId[i] : 0.f;
                    break;
                case HydraRender::ePlaneNormal:
                    out[c] = (inside && (c < 3) && !frame.normal.empty()) ? frame.normal[i * 3 + (std::size_t)c] : 0.f;
                    break;
                }
            }
        }
    }

    return true;
} // RenderThread::cpuWindow

bool
RenderThread::decodeIds(EngineEntry& entry,
                        std::string* error)
{
    std::optional<StormGLSection> section;
    enterGLSection(&section);
    if (entry.idsDecoded) {
        return true;
    }
    UsdImagingGLEngine* engine = entry.engine.get();
    const int width = entry.width;
    const int height = entry.height;
    const std::size_t pixels = (std::size_t)width * (std::size_t)height;
    {
        MappedAov primIds(entry, HdAovTokens->primId, error);
        if (!primIds.valid()) {
            return false;
        }
        MappedAov instanceIds(entry, HdAovTokens->instanceId, error);
        if (!instanceIds.valid()) {
            return false;
        }
        entry.primId.assign(pixels, 0.f);
        entry.primPaths.clear();
        const bool cycles = CyclesAovs::isCycles(entry.renderer);
        std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t> decoded;
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::int32_t prim = readInt(primIds.pixel(i));
            if (prim < 0) {
                continue;
            }
            const std::int32_t instance = readInt(instanceIds.pixel(i));
            const std::pair<std::int32_t, std::int32_t> key(prim, cycles ? CyclesAovs::instanceIndex(instance) : instance);
            std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t>::const_iterator found = decoded.find(key);
            std::uint32_t hash = 0;
            if (found != decoded.end()) {
                hash = found->second;
            } else {
                SdfPath path;
                if (engine->DecodeIntersection(key.first, key.second, &path) && !path.IsEmpty()) {
                    hash = HydraRender::primPathHash(path.GetString());
                    entry.primPaths[hash] = path.GetString();
                }
                decoded[key] = hash;
            }
            if (hash) {
                entry.primId[i] = HydraRender::hashToFloat(hash);
            }
        }
    }

    entry.idsDecoded = true;

    return true;
} // RenderThread::decodeIds

bool
RenderThread::frame(EngineEntry& entry,
                    HydraRender::Result* result,
                    std::string* error)
{
    std::optional<StormGLSection> section;
    enterGLSection(&section);
    std::string packError;
    if (!_gl || !packAvailable(&packError)) {
        return cpuFrame(entry, result, error);
    }
    HydraRender::PlaneWindow window;
    window.x2 = entry.width;
    window.y2 = entry.height;
    window.plane = HydraRender::ePlaneColor;
    window.nComps = 4;
    if (!readWindow(entry, window, &result->rgba, error)) {
        return false;
    }
    window.nComps = 1;
    window.plane = HydraRender::ePlaneDepth;
    if (!readWindow(entry, window, &result->depth, error)) {
        return false;
    }
    window.plane = HydraRender::ePlanePrimId;
    if (!readWindow(entry, window, &result->primId, error)) {
        return false;
    }
    result->primPaths = entry.primPaths;

    return true;
}

bool
RenderThread::packAvailable(std::string* error)
{
    if (_packState == 0) {
        _packState = -1;
        GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(shader, 1, &kPackShader, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        GLint linked = GL_FALSE;
        if (compiled == GL_TRUE) {
            _packProgram = glCreateProgram();
            glAttachShader(_packProgram, shader);
            glLinkProgram(_packProgram);
            glGetProgramiv(_packProgram, GL_LINK_STATUS, &linked);
        }
        if ((compiled != GL_TRUE) || (linked != GL_TRUE)) {
            char log[1024] = { 0 };
            if (compiled != GL_TRUE) {
                glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            } else {
                glGetProgramInfoLog(_packProgram, sizeof(log) - 1, nullptr, log);
            }
            _packError = "USDRender's pack shader does not build on " + contextDescription() + ": " + log;
            if (_packProgram) {
                glDeleteProgram(_packProgram);
                _packProgram = 0;
            }
        } else {
            const char* const names[6] = { "windowRect", "renderSize", "plane", "nComps", "inverseZ", "inverseW" };
            for (int i = 0; i < 6; ++i) {
                _packUniforms[i] = glGetUniformLocation(_packProgram, names[i]);
            }
            glCreateSamplers(1, &_packSampler);
            glSamplerParameteri(_packSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(_packSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glSamplerParameteri(_packSampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glGenBuffers(1, &_idsBuffer);
            glGenBuffers(1, &_readBuffer);
            _packState = (takeGlError() == GL_NO_ERROR) ? 1 : -1;
            if (_packState < 0) {
                _packError = "USDRender's pack shader could not be set up on " + contextDescription() + ".";
            }
        }
        glDeleteShader(shader);
    }
    if (_packState < 0) {
        *error = _packError;

        return false;
    }

    return true;
} // RenderThread::packAvailable

GLuint
RenderThread::aovTexture(EngineEntry& entry,
                         const TfToken& name,
                         std::string* error)
{
    HdRenderBuffer* buffer = entry.engine->GetAovRenderBuffer(name);
    if (!buffer) {
        *error = "Storm rendered no " + name.GetString() + " AOV.";

        return 0;
    }
    buffer->Resolve();
    if (((int)buffer->GetWidth() != entry.width) || ((int)buffer->GetHeight() != entry.height)) {
        *error = "Storm's " + name.GetString() + " AOV is not the size asked for.";

        return 0;
    }
    const VtValue resource = buffer->GetResource(false);
    HgiTextureHandle texture;
    if (resource.IsHolding<HgiTextureHandle>()) {
        texture = resource.UncheckedGet<HgiTextureHandle>();
    }
    const HgiGLTexture* glTexture = dynamic_cast<const HgiGLTexture*>(texture.Get());
    if (!glTexture || !glTexture->GetTextureId()) {
        *error = "Storm's " + name.GetString() + " AOV is not a GL texture.";

        return 0;
    }

    return glTexture->GetTextureId();
}

bool
RenderThread::pack(EngineEntry& entry,
                   const HydraRender::PlaneWindow& window,
                   GLuint buffer,
                   std::string* error)
{
    std::optional<StormGLSection> section;
    enterGLSection(&section);
    if (!packAvailable(error)) {
        return false;
    }
    if ((window.x2 <= window.x1) || (window.y2 <= window.y1) || (window.nComps <= 0)) {
        return true;
    }
    (void)takeGlError();
    GLuint color = 0;
    GLuint depth = 0;
    switch (window.plane) {
    case HydraRender::ePlaneColor:
        color = aovTexture(entry, HdAovTokens->color, error);
        if (!color) {
            return false;
        }
        break;
    case HydraRender::ePlaneDepth:
        depth = aovTexture(entry, HdAovTokens->depth, error);
        if (!depth) {
            return false;
        }
        // A depth-stencil texture is read as its depth.
        glTextureParameteri(depth, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
        (void)takeGlError();
        break;
    case HydraRender::ePlaneNormal:
        *error = "Storm renders no normal AOV.";

        return false;
    case HydraRender::ePlanePrimId:
        if (!decodeIds(entry, error)) {
            return false;
        }
        if (_idsSerial != entry.serial) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, _idsBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(entry.primId.size() * sizeof(float)), entry.primId.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            _idsSerial = entry.serial;
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, _idsBuffer);
        break;
    }
    const GfMatrix4d inverse = entry.projection.GetInverse();
    const double inverseZ[4] = { inverse[0][2], inverse[1][2], inverse[2][2], inverse[3][2] };
    const double inverseW[4] = { inverse[0][3], inverse[1][3], inverse[2][3], inverse[3][3] };

    glUseProgram(_packProgram);
    glUniform4i(_packUniforms[0], window.x1, window.y1, window.x2, window.y2);
    glUniform2i(_packUniforms[1], entry.width, entry.height);
    glUniform1i(_packUniforms[2], (int)window.plane);
    glUniform1i(_packUniforms[3], window.nComps);
    glUniform4dv(_packUniforms[4], 1, inverseZ);
    glUniform4dv(_packUniforms[5], 1, inverseW);
    glBindTextureUnit(0, color);
    glBindTextureUnit(1, depth);
    glBindSampler(0, _packSampler);
    glBindSampler(1, _packSampler);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    glDispatchCompute((GLuint)((window.x2 - window.x1 + 7) / 8), (GLuint)((window.y2 - window.y1 + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindTextureUnit(0, 0);
    glBindTextureUnit(1, 0);
    glUseProgram(0);
    const GLenum failed = takeGlError();
    if (failed != GL_NO_ERROR) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%04x", failed);
        *error = std::string("USDRender's pack shader raised GL error ") + code + ".";

        return false;
    }

    return true;
} // RenderThread::pack

bool
RenderThread::readWindow(EngineEntry& entry,
                         const HydraRender::PlaneWindow& window,
                         std::vector<float>* pixels,
                         std::string* error)
{
    std::optional<StormGLSection> section;
    enterGLSection(&section);
    const std::size_t count = window.floatCount();
    pixels->assign(count, 0.f);
    if (count == 0) {
        return true;
    }
    if (!_gl) {
        return cpuWindow(entry, window, pixels, error);
    }
    if (!packAvailable(error)) {
        return false;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _readBuffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(count * sizeof(float)), nullptr, GL_STREAM_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (!pack(entry, window, _readBuffer, error)) {
        return false;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _readBuffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(count * sizeof(float)), pixels->data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (takeGlError() != GL_NO_ERROR) {
        *error = "USDRender could not read its pixels back from GL.";

        return false;
    }

    return true;
}

// The render access a withRender() job gets.
class EntryAccess
    : public HydraRender::RenderAccess {
public:
    EntryAccess(RenderThread& thread,
                EngineEntry& entry)
        : _thread(thread)
        , _entry(entry)
    {
    }

    virtual bool pack(const HydraRender::PlaneWindow& window,
                      unsigned int glBuffer,
                      std::string* error) OVERRIDE FINAL
    {
        return _thread.pack(_entry, window, glBuffer, error);
    }

    virtual bool primPaths(std::map<std::uint32_t, std::string>* paths,
                           std::string* error) OVERRIDE FINAL
    {
        if (!_thread.decodeIds(_entry, error)) {
            return false;
        }
        *paths = _entry.primPaths;

        return true;
    }

    virtual void* getProcAddress(const char* name) const OVERRIDE FINAL
    {
        return _thread.getProcAddress(name);
    }

private:
    RenderThread& _thread;
    EngineEntry& _entry;
};

std::uint32_t
rotateLeft(std::uint32_t x,
           int r)
{
    return (x << r) | (x >> (32 - r));
}

// MurmurHash3_x86_32, Austin Appleby's public-domain reference, as Cryptomatte uses it.
std::uint32_t
murmurHash3(const std::string& text,
            std::uint32_t seed)
{
    const unsigned char* data = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t length = text.size();
    const std::size_t blocks = length / 4;
    const std::uint32_t c1 = 0xcc9e2d51;
    const std::uint32_t c2 = 0x1b873593;
    std::uint32_t h1 = seed;

    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint32_t k1 = (std::uint32_t)data[i * 4] | ((std::uint32_t)data[i * 4 + 1] << 8) | ((std::uint32_t)data[i * 4 + 2] << 16) | ((std::uint32_t)data[i * 4 + 3] << 24);
        k1 *= c1;
        k1 = rotateLeft(k1, 15);
        k1 *= c2;
        h1 ^= k1;
        h1 = rotateLeft(h1, 13);
        h1 = h1 * 5 + 0xe6546b64;
    }
    const unsigned char* tail = data + blocks * 4;
    std::uint32_t k1 = 0;
    switch (length & 3) {
    case 3:
        k1 ^= (std::uint32_t)tail[2] << 16;
        // fall through
    case 2:
        k1 ^= (std::uint32_t)tail[1] << 8;
        // fall through
    case 1:
        k1 ^= tail[0];
        k1 *= c1;
        k1 = rotateLeft(k1, 15);
        k1 *= c2;
        h1 ^= k1;
        break;
    default:
        break;
    }
    h1 ^= (std::uint32_t)length;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6b;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35;
    h1 ^= h1 >> 16;

    return h1;
} // murmurHash3
} // anon namespace

void
setHydraGLContextFactory(const HydraGLContextFactory& factory)
{
    std::lock_guard<std::mutex> l(gFactoryMutex);

    gFactory = factory;
}

namespace HydraRender {
bool
cameraFrustum(const UsdStageRefPtr& stage,
              const SdfPath& cameraPath,
              UsdTimeCode time,
              double aspect,
              FitModeEnum fit,
              GfFrustum* frustum,
              std::string* error)
{
    const UsdPrim prim = stage ? stage->GetPrimAtPath(cameraPath) : UsdPrim();
    const UsdGeomCamera camera(prim);

    if (!camera) {
        *error = "No camera at " + cameraPath.GetString() + " on the input scene.";

        return false;
    }
    GfCamera gfCamera = camera.GetCamera(time);
    CameraUtilConformWindowPolicy policy = CameraUtilCrop;
    switch (fit) {
    case eFitModeFill:
        policy = CameraUtilCrop;
        break;
    case eFitModeHorizontal:
        policy = CameraUtilMatchHorizontally;
        break;
    case eFitModeVertical:
        policy = CameraUtilMatchVertically;
        break;
    case eFitModeExpand:
        policy = CameraUtilFit;
        break;
    }
    CameraUtilConformWindow(&gfCamera, policy, aspect);
    *frustum = gfCamera.GetFrustum();

    return true;
}

std::vector<SdfPath>
cameraPaths(const UsdStageRefPtr& stage)
{
    std::vector<SdfPath> paths;

    if (!stage) {
        return paths;
    }
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            paths.push_back(prim.GetPath());
        }
    }

    return paths;
}

std::uint32_t
primPathHash(const std::string& path)
{
    std::uint32_t hash = murmurHash3(path, 0);
    // As Cryptomatte: no infinity, NaN or denormal once the bits are read as a float.
    const std::uint32_t exponent = (hash >> 23) & 255;

    if ((exponent == 0) || (exponent == 255)) {
        hash ^= 1u << 23;
    }

    return hash;
}

float
hashToFloat(std::uint32_t hash)
{
    float value;

    std::memcpy(&value, &hash, sizeof(value));

    return value;
}

namespace {
    /**
     * The thread of request's delegate, after dropping the owner's engine on the other one; null with
     * *error naming the delegate when it is not installed or cannot render here, before any engine or
     * GL work.
     **/
    RenderThread*
    routeRequest(const Request& request,
                 std::string* error)
    {
        const std::string renderer = request.renderer.empty() ? std::string(kRenderDelegateStormId) : request.renderer;
        const std::string unavailable = RenderDelegates::unavailableMessage(renderer);

        if (!unavailable.empty()) {
            *error = unavailable;

            return nullptr;
        }
        const bool storm = RenderDelegates::isStorm(renderer);
        RenderThread& other = storm ? RenderThread::cpu() : RenderThread::storm();
        const void* owner = request.owner;
        (void)other.post([&other, owner]() { other.releaseEngine(owner); }, false);

        return storm ? &RenderThread::storm() : &RenderThread::cpu();
    }

    // Waits for thread's queue to drain, then reads how many engines it holds.
    std::size_t
    drainedEngineCount(RenderThread& thread)
    {
        // Runs nothing if the thread never started, in which case there is no engine.
        std::promise<void> drained;
        std::future<void> done = drained.get_future();
        if (thread.post([&drained]() { drained.set_value(); }, false)) {
            done.wait();
        }

        return thread.engineCount();
    }
} // anon namespace

bool
render(const Request& request,
       Result* result,
       std::string* error)
{
    if (request.scene.isEmpty() || (request.width <= 0) || (request.height <= 0)) {
        *error = "Nothing to render.";

        return false;
    }
    RenderThread* routed = routeRequest(request, error);
    if (!routed) {
        return false;
    }
    RenderThread& thread = *routed;
    bool ok = false;
    std::atomic<bool> cancel(false);
    std::function<void()> whileWaiting;
    if (request.aborted && !thread.usesGl()) {
        whileWaiting = [&request, &cancel]() {
            if (request.aborted()) {
                cancel = true;
            }
        };
    }
    const bool ran = thread.runAndWait([&]() {
        try {
            EngineEntry* entry = thread.renderEngine(request, &cancel, error);
            ok = entry && thread.frame(*entry, result, error);
        } catch (const std::exception& e) {
            *error = std::string("Hydra failed: ") + e.what();
            ok = false;
        }
    },
                                       error, whileWaiting);

    return ran && ok;
}

bool
withRender(const Request& request,
           const std::function<bool(RenderAccess& access, std::string* error)>& job,
           std::string* error)
{
    if (request.scene.isEmpty() || (request.width <= 0) || (request.height <= 0)) {
        *error = "Nothing to render.";

        return false;
    }
    RenderThread* routed = routeRequest(request, error);
    if (!routed) {
        return false;
    }
    if (!routed->usesGl()) {
        *error = "Only Storm renders into GL; " + request.renderer + "'s pixels are read back.";

        return false;
    }
    bool ok = false;
    RenderThread& thread = *routed;
    const bool ran = thread.runAndWait([&]() {
        try {
            EngineEntry* entry = thread.renderEngine(request, nullptr, error);
            if (entry) {
                EntryAccess access(thread, *entry);
                ok = job(access, error);
            }
        } catch (const std::exception& e) {
            *error = std::string("Storm failed: ") + e.what();
            ok = false;
        }
    },
                                       error);

    return ran && ok;
}

bool
withContext(const std::function<void(const std::function<void*(const char*)>& getProcAddress)>& job,
            std::string* error)
{
    bool ok = false;
    RenderThread& thread = RenderThread::storm();

    const bool ran = thread.runAndWait([&]() {
        if (thread.ensureContext(error)) {
            ok = true;
            const std::function<void*(const char*)> getProcAddress = [&thread](const char* name) { return thread.getProcAddress(name); };
            job(getProcAddress);
        }
    },
                                       error);

    return ran && ok;
}

void
postToContext(std::function<void(const std::function<void*(const char*)>* getProcAddress)> job)
{
    RenderThread& thread = RenderThread::storm();

    (void)thread.post([&thread, job]() {
        const std::function<void*(const char*)> getProcAddress = [&thread](const char* name) { return thread.getProcAddress(name); };
        job(thread.makeCurrentIfMade() ? &getProcAddress : nullptr);
    },
                      false);
}

bool
renderWindow(const Request& request,
             const PlaneWindow& window,
             std::vector<float>* pixels,
             std::map<std::uint32_t, std::string>* primPaths,
             std::string* error)
{
    if (request.scene.isEmpty() || (request.width <= 0) || (request.height <= 0)) {
        *error = "Nothing to render.";

        return false;
    }
    RenderThread* routed = routeRequest(request, error);
    if (!routed) {
        return false;
    }
    RenderThread& thread = *routed;
    bool ok = false;
    std::atomic<bool> cancel(false);
    std::function<void()> whileWaiting;
    if (request.aborted && !thread.usesGl()) {
        whileWaiting = [&request, &cancel]() {
            if (request.aborted()) {
                cancel = true;
            }
        };
    }
    const bool ran = thread.runAndWait([&]() {
        try {
            EngineEntry* entry = thread.renderEngine(request, &cancel, error);
            if (!entry) {
                return;
            }
            ok = thread.readWindow(*entry, window, pixels, error);
            if (ok && primPaths && (window.plane == ePlanePrimId)) {
                ok = thread.decodeIds(*entry, error);
                *primPaths = entry->primPaths;
            }
        } catch (const std::exception& e) {
            *error = std::string("Hydra failed: ") + e.what();
            ok = false;
        }
    },
                                       error, whileWaiting);

    return ran && ok;
}

bool
packsOnGpu()
{
    bool ok = false;
    RenderThread& thread = RenderThread::storm();

    const bool ran = thread.runAndWait([&]() {
        std::string error;
        ok = thread.ensureContext(&error) && thread.packAvailable(&error);
    },
                                       nullptr);

    return ran && ok;
}

void
release(const void* owner)
{
    RenderThread& storm = RenderThread::storm();
    RenderThread& cpu = RenderThread::cpu();

    (void)storm.post([&storm, owner]() { storm.releaseEngine(owner); }, false);
    (void)cpu.post([&cpu, owner]() { cpu.releaseEngine(owner); }, false);
}

void
releaseAll()
{
    RenderThread& thread = RenderThread::storm();

    (void)thread.post([&thread]() { thread.releaseAllEngines(); }, false);
}

void
setRendererSettings(const std::string& renderer,
                    const std::map<std::string, VtValue>& settings)
{
    std::lock_guard<std::mutex> lock(gSettingsMutex);

    if (settings.empty()) {
        gSettings.erase(renderer);
    } else {
        gSettings[renderer] = settings;
    }
}

bool
engineSetting(const void* owner,
              const std::string& key,
              VtValue* value)
{
    for (RenderThread* thread : { &RenderThread::storm(), &RenderThread::cpu() }) {
        // Posted without starting a thread that never ran, which holds no engine.
        std::promise<bool> read;
        std::future<bool> done = read.get_future();
        if (thread->post([thread, owner, &key, value, &read]() { read.set_value(thread->engineSetting(owner, TfToken(key), value)); }, false) && done.get()) {
            return true;
        }
    }

    return false;
}

void
releaseAllCpuEngines()
{
    RenderThread& thread = RenderThread::cpu();

    (void)thread.post([&thread]() { thread.releaseAllEngines(); }, false);
}

void
setMemoryBudget(std::size_t bytes)
{
    gMemoryBudget = bytes;
}

std::size_t
getMemoryBudget()
{
    return gMemoryBudget.load();
}

std::size_t
getEngineCount()
{
    return drainedEngineCount(RenderThread::storm());
}

std::size_t
getCpuEngineCount()
{
    return drainedEngineCount(RenderThread::cpu());
}

int
getCyclesThreads()
{
    return CyclesThreads::instance().count();
}

std::uint64_t
getEngineBuildCount()
{
    return gEngineBuilds.load();
}

std::string
contextDescription(std::string* error)
{
    std::string description;
    RenderThread& thread = RenderThread::storm();

    const bool ran = thread.runAndWait([&]() {
        if (thread.ensureContext(error)) {
            description = thread.contextDescription();
        }
    },
                                       error);

    return ran ? description : std::string();
}

bool
ProgressiveRender::View::operator==(const View& other) const
{
    return (width == other.width) && (height == other.height) && (time == other.time) && (frustum.ComputeViewMatrix() == other.frustum.ComputeViewMatrix()) && (frustum.ComputeProjectionMatrix() == other.frustum.ComputeProjectionMatrix());
}

namespace {
    // How often the worker reads a refining delegate's progress, and publishes its pixels if they
    // gained samples.
    const std::chrono::milliseconds kRefinePoll(50);
    // A restart waits for this long without an edit, but no longer than kRestartMaxWait after the
    // first edit it waits on.
    const std::chrono::milliseconds kRestartQuiet(20);
    const std::chrono::milliseconds kRestartMaxWait(100);
    // How long a converged delegate may hold a sample count short of the cap: an adaptive sampler
    // stops early.
    const std::chrono::milliseconds kConvergedStall(1000);
    // The niceness of the worker, which the threads the delegate starts from it inherit.
    const int kRefineNice = 10;

    std::atomic<int> gProgressiveLive(0);

    int
    intOf(const VtValue& value,
          int fallback)
    {
        if (value.IsHolding<int>()) {
            return value.UncheckedGet<int>();
        }
        if (value.IsHolding<unsigned int>()) {
            return (int)value.UncheckedGet<unsigned int>();
        }
        if (value.IsHolding<long>()) {
            return (int)value.UncheckedGet<long>();
        }
        if (value.IsHolding<long long>()) {
            return (int)value.UncheckedGet<long long>();
        }
        if (value.IsHolding<double>()) {
            return (int)value.UncheckedGet<double>();
        }
        if (value.IsHolding<float>()) {
            return (int)value.UncheckedGet<float>();
        }

        return fallback;
    }

    // The delegate's setting of its samples per pixel: Hydra's generic one, or one of its own ending
    // in "samples".
    TfToken
    samplesSettingKey(const UsdImagingGLRendererSettingsList& settings)
    {
        for (const UsdImagingGLRendererSetting& setting : settings) {
            if (setting.key == HdRenderSettingsTokens->convergedSamplesPerPixel) {
                return setting.key;
            }
        }
        for (const UsdImagingGLRendererSetting& setting : settings) {
            const std::string& key = setting.key.GetString();
            const std::string suffix = ":samples";
            if ((setting.type == UsdImagingGLRendererSetting::TYPE_INT) && ((key == "samples") || ((key.size() > suffix.size()) && (key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)))) {
                return setting.key;
            }
        }

        return TfToken();
    }

    void
    lowerThreadPriority()
    {
        (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), kRefineNice);
    }

    // Linear [0, 1] to 8-bit sRGB, by 4096 steps.
    const std::vector<unsigned char>&
    srgbTable()
    {
        static const std::vector<unsigned char> table = []() {
            std::vector<unsigned char> t(4096);
            for (int i = 0; i < 4096; ++i) {
                const double v = i / 4095.;
                const double s = (v <= 0.0031308) ? (v * 12.92) : (1.055 * std::pow(v, 1. / 2.4) - 0.055);
                t[i] = (unsigned char)std::lround(std::min(1., std::max(0., s)) * 255.);
            }

            return t;
        }();

        return table;
    }
} // anon namespace

struct ProgressiveRender::Implementation {
    typedef std::chrono::steady_clock Clock;

    std::mutex* const stageLock;
    const std::function<UsdStageRefPtr()> stageOf;
    const std::function<void()> listener;
    const float background;

    // Guards the requests, the status and the frame; never held while taking stageLock.
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::string renderer;
    std::function<int()> threadsOf;
    View view;
    bool hasView = false;
    int maxSamples = 0;
    // The settings of the stage's render settings prim for the renderer (setSettings()).
    RendererSettings settings;
    bool stopped = false;
    bool quit = false;
    // Bumped by each edit; a pass begun under another is not published.
    std::uint64_t generation = 0;
    bool pending = false;
    Clock::time_point firstPending;
    Clock::time_point lastPending;
    Status status;
    std::shared_ptr<const Frame> frame;
    int defaultMaxSamples = 0;

    // The worker's; engineMutex guards the engine against pick().
    std::mutex engineMutex;
    EnginePtr engine;
    std::string engineRenderer;
    // The settings the engine was made under, and the AOV its depth is read from.
    RendererSettings engineSettings;
    TfToken depthAov;
    std::string rendererName;
    std::string failedRenderer;
    // Bumped by each edit of the scene or the settings, and the count a failure happened under: a
    // later edit may be what the delegate needed, while a resize or an orbit changes nothing it failed on.
    std::uint64_t sceneEdits = 0;
    std::uint64_t failedEdits = 0;
    // What the worker's current iteration is about, for a failure that escapes it.
    std::string workingRenderer;
    std::uint64_t workingEdits = 0;
    TfToken samplesKey;
    int appliedCap = 0;
    bool paused = false;
    View engineView;
    bool engineHasView = false;
    // The count the engine's Cycles session was opened under, and the one the worker last asked
    // CyclesThreads for, -1 before it asked.
    int engineThreads = 0;
    int askedThreads = -1;
    int cyclesHolder = 0;

    std::thread worker;

    Implementation(std::mutex* stageLock_,
                   const std::function<UsdStageRefPtr()>& stage_,
                   const std::function<void()>& listener_,
                   float background_)
        : stageLock(stageLock_)
        , stageOf(stage_)
        , listener(listener_)
        , background(background_)
    {
    }

    // Under mutex.
    void edited()
    {
        ++generation;
        const Clock::time_point now = Clock::now();
        if (!pending) {
            firstPending = now;
        }
        pending = true;
        lastPending = now;
        wake.notify_all();
    }

    // Must not throw: it ends the worker. The engine's stage scene index stays subscribed to the
    // stage's notices until it is gone, its destructor does not revoke them and delivery holds no
    // reference to it, so it may only die while no edit of the stage can be under way.
    void dropEngine()
    {
        std::lock_guard<std::mutex> stageGuard(*stageLock);
        std::lock_guard<std::mutex> l(engineMutex);
        if (engine && paused) {
            try {
                (void)engine->ResumeRenderer();
            } catch (...) {
                qWarning("A render delegate threw while resuming to be released.");
            }
        }
        engine.reset();
        engineRenderer.clear();
        samplesKey = TfToken();
        appliedCap = 0;
        paused = false;
        engineHasView = false;
    }

    // Makes the engine of id, on the worker; false with *error set if the delegate cannot start.
    bool makeEngine(const std::string& id,
                    bool cycles,
                    int threads,
                    const RendererSettings& primSettings,
                    int* defaultCap,
                    std::string* error)
    {
        RenderDelegates::Delegate delegate;
        const std::string name = RenderDelegates::find(id, &delegate) ? delegate.displayName : id;
        UsdImagingGLEngine::Parameters parameters;
        parameters.rendererPluginId = TfToken(id);
        parameters.gpuEnabled = false;
        CyclesAovs::registerSceneIndices();
        MoonRayAovs::registerSceneIndices();
        EnginePtr made;
        {
            TfErrorMark mark;
            made = newEngine(parameters, cycles, threads);
            ++gEngineBuilds;
            if (made->GetCurrentRendererId() != TfToken(id)) {
                std::string reasons;
                for (TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
                    reasons += (reasons.empty() ? ": " : "; ") + it->GetCommentary();
                }
                mark.Clear();
                *error = "The renderer " + name + " could not be started" + reasons + ".";
            }
        }
        if (!error->empty()) {
            return false;
        }
        made->SetEnablePresentation(false);
        const bool moonRay = MoonRayAovs::isMoonRay(id);
        // The prim's settings, then those set for tests, as a USDRender's engine takes them.
        RendererSettings settings = primSettings;
        for (const std::pair<const std::string, VtValue>& setting : rendererSettings(id)) {
            settings[setting.first] = setting.second;
        }
        for (RendererSettings::const_iterator it = settings.begin(); it != settings.end(); ++it) {
            made->SetRendererSetting(TfToken(it->first), it->second);
        }
        // Last: a prim's settings must not override these.
        if (cycles) {
            CyclesAovs::configureEngine(made.get());
        } else if (moonRay) {
            // Its AOVs and thread budget, as a USDRender's MoonRay engine has them.
            MoonRayAovs::configureEngine(made.get(), threads);
        }
        const TfTokenVector aovs = moonRay ? MoonRayAovs::aovs() : TfTokenVector { HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId };
        if (!made->SetRendererAovs(aovs)) {
            std::string names;
            for (std::size_t i = 0; i < aovs.size(); ++i) {
                if (i > 0) {
                    names += (i + 1 == aovs.size()) ? " and " : ", ";
                }
                names += aovs[i].GetString();
            }
            *error = name + " offers no " + names + " AOVs.";

            return false;
        }
        const UsdImagingGLRendererSettingsList list = made->GetRendererSettingsList();
        // MoonRay's samples per pixel is a scene variable its delegate does not declare.
        const TfToken key = moonRay ? TfToken(MoonRaySettings::samplesKey()) : samplesSettingKey(list);
        int cap = 0;
        if (!key.IsEmpty()) {
            cap = intOf(made->GetRendererSetting(key), 0);
            for (const UsdImagingGLRendererSetting& setting : list) {
                if ((cap <= 0) && (setting.key == key)) {
                    cap = intOf(setting.defValue, 0);
                }
            }
        }
        std::lock_guard<std::mutex> l(engineMutex);
        engine = std::move(made);
        engineRenderer = id;
        engineSettings = primSettings;
        depthAov = moonRay ? MoonRayAovs::depthAov() : HdAovTokens->depth;
        engineThreads = threads;
        rendererName = name;
        samplesKey = key;
        appliedCap = cap;
        paused = false;
        engineHasView = false;
        *defaultCap = cap;

        return true;
    } // makeEngine

    // The pixels of the engine's colour and depth AOVs, as a frame of view. Under engineMutex.
    bool readFrame(const View& v,
                   Frame* out,
                   std::string* error)
    {
        const int width = v.width;
        const int height = v.height;
        const std::size_t pixels = (std::size_t)width * (std::size_t)height;
        const std::vector<unsigned char>& table = srgbTable();
        out->width = width;
        out->height = height;
        {
            MappedAov color(engine.get(), rendererName, width, height, HdAovTokens->color, error);
            if (!color.valid()) {
                return false;
            }
            const HdFormat componentFormat = HdGetComponentFormat(color.format());
            const std::size_t components = HdGetComponentCount(color.format());
            const std::size_t componentSize = HdDataSizeOfFormat(componentFormat);
            out->rgba.resize(pixels * 4);
            for (std::size_t i = 0; i < pixels; ++i) {
                const unsigned char* at = color.pixel(i);
                float c[4] = { 0.f, 0.f, 0.f, 1.f };
                for (std::size_t k = 0; (k < components) && (k < 4); ++k) {
                    c[k] = componentToFloat(componentFormat, at + k * componentSize);
                }
                // Premultiplied: over the background, then encoded.
                const float under = background * (1.f - std::min(1.f, std::max(0.f, c[3])));
                for (int k = 0; k < 3; ++k) {
                    const float value = std::min(1.f, std::max(0.f, c[k] + under));
                    out->rgba[i * 4 + k] = table[(std::size_t)(value * 4095.f + 0.5f)];
                }
                out->rgba[i * 4 + 3] = 255;
            }
        }
        {
            MappedAov depth(engine.get(), rendererName, width, height, depthAov, error);
            if (!depth.valid()) {
                return false;
            }
            const HdFormat componentFormat = HdGetComponentFormat(depth.format());
            out->depth.assign(pixels, 1.f);
            const bool cycles = CyclesAovs::isCycles(engineRenderer);
            if (cycles || MoonRayAovs::isMoonRay(engineRenderer)) {
                // Cycles' depth and MoonRay's cameraDepth are the distance along the view axis:
                // through the projection to the window depth.
                const GfMatrix4d projection = v.frustum.ComputeProjectionMatrix();
                for (std::size_t i = 0; i < pixels; ++i) {
                    const float value = componentToFloat(componentFormat, depth.pixel(i));
                    const double distance = cycles ? CyclesAovs::depthToDistance(value) : MoonRayAovs::cameraDepth(value);
                    if (!std::isfinite(distance)) {
                        continue;
                    }
                    const double z = -distance;
                    const double clipZ = z * projection[2][2] + projection[3][2];
                    const double clipW = z * projection[2][3] + projection[3][3];
                    if (clipW == 0.) {
                        continue;
                    }
                    const double window = (clipZ / clipW + 1.) * 0.5;
                    out->depth[i] = (float)std::min(1., std::max(0., window));
                }
            } else {
                for (std::size_t i = 0; i < pixels; ++i) {
                    out->depth[i] = std::min(1.f, std::max(0.f, componentToFloat(componentFormat, depth.pixel(i))));
                }
            }
        }

        return true;
    } // readFrame

    // Asks CyclesThreads for what threads gives when it changed since the worker last asked. Without
    // mutex: the count's change notifies this.
    void askThreads(const std::function<int()>& threads)
    {
        const int wanted = threads ? std::max(0, threads()) : 0;
        if (wanted != askedThreads) {
            askedThreads = wanted;
            CyclesThreads::instance().ask(wanted);
        }
    }

    void notifyListener()
    {
        if (!listener) {
            return;
        }
        try {
            listener();
        } catch (...) {
            qWarning("A progressive render listener threw.");
        }
    }

    // Under mutex, and again on return. The delegate of id failed in an iteration begun at edits;
    // it stays failed, without an engine, until a later edit or until another is chosen.
    void fail(std::unique_lock<std::mutex>& lock,
              const std::string& id,
              const std::string& error,
              std::uint64_t edits)
    {
        // Chosen away from during the iteration: the failure is not the new renderer's.
        const bool current = id == renderer;
        if (current) {
            failedRenderer = id;
            failedEdits = edits;
            status.error = error;
            status.active = false;
        }
        lock.unlock();
        dropEngine();
        if (current) {
            notifyListener();
        }
        lock.lock();
    }

    // A delegate can throw from any call, not only Render(), and nothing above the worker's thread
    // would catch it: the process would terminate.
    void failWorker(std::unique_lock<std::mutex>& lock,
                    const char* what)
    {
        if (!lock.owns_lock()) {
            lock.lock();
        }
        RenderDelegates::Delegate delegate;
        const std::string name = RenderDelegates::find(workingRenderer, &delegate) ? delegate.displayName : workingRenderer;
        const std::string reason = (what && what[0]) ? what : "unknown exception";
        fail(lock, workingRenderer, "The " + name + " render failed: " + reason, workingEdits);
    }

    void refine(std::unique_lock<std::mutex>& lock);
    void run();
};

void
ProgressiveRender::Implementation::run()
{
    lowerThreadPriority();
    std::unique_lock<std::mutex> lock(mutex);

    while (!quit) {
        try {
            refine(lock);
        } catch (const std::exception& e) {
            failWorker(lock, e.what());
        } catch (...) {
            failWorker(lock, nullptr);
        }
    }
    lock.unlock();
    dropEngine();
}

// Returns with lock held. Its record of the render's progress dies with a failure, as the engine does.
void
ProgressiveRender::Implementation::refine(std::unique_lock<std::mutex>& lock)
{
    int lastSamples = -1;
    RestartWatch restartWatch;
    bool lastConverged = false;
    bool firstPass = false;
    bool engineFresh = false;
    Clock::time_point restartTime = Clock::now();
    Clock::time_point stallSince = restartTime;
    bool done = false;
    bool needRestart = true;
    // The engine is being made again for the same render: the frame shown stays until the new one.
    bool keepFrame = false;
    // Lives no longer than the engine: what it kept of an earlier session must not fail a later one.
    std::unique_ptr<MoonRayAovs::LogCapture> log;

    while (!quit) {
        workingRenderer = renderer;
        workingEdits = sceneEdits;
        const bool idle = renderer.empty() || (renderer == kRenderDelegateStormId) || !hasView || !stageOf;
        if (idle || (renderer != engineRenderer)) {
            if (engine) {
                lock.unlock();
                dropEngine();
                lock.lock();
                continue;
            }
            log.reset();
            status.active = false;
            status.converged = false;
            status.paused = false;
            status.samples = -1;
            if ((renderer == failedRenderer) && (sceneEdits != failedEdits)) {
                if (pending) {
                    const Clock::time_point due = std::min(lastPending + kRestartQuiet, firstPending + kRestartMaxWait);
                    if (Clock::now() < due) {
                        wake.wait_until(lock, due);
                        continue;
                    }
                }
                failedRenderer.clear();
            }
            if (renderer != failedRenderer) {
                status.error.clear();
            }
            if (idle || (renderer == failedRenderer)) {
                keepFrame = false;
                wake.wait(lock);
                continue;
            }
            const std::string id = renderer;
            const std::function<int()> threadsFn = threadsOf;
            const RendererSettings primSettings = settings;
            lock.unlock();
            const bool cycles = CyclesAovs::isCycles(id);
            int threads = 0;
            if (MoonRayAovs::isMoonRay(id)) {
                threads = threadsFn ? std::max(0, threadsFn()) : 0;
            }
            if (cycles) {
                askThreads(threadsFn);
                if (!CyclesThreads::instance().open(&threads)) {
                    // Their holders drop the sessions of another count at their next boundary.
                    lock.lock();
                    wake.wait_for(lock, kRefinePoll);
                    continue;
                }
            }
            std::string error;
            int cap = 0;
            const bool made = makeEngine(id, cycles, threads, primSettings, &cap, &error);
            lock.lock();
            if (!made) {
                fail(lock, id, error, workingEdits);
                continue;
            }
            if (MoonRayAovs::isMoonRay(id)) {
                // After the engine, whose making loads the libraries that print.
                log.reset(new MoonRayAovs::LogCapture);
            }
            failedRenderer.clear();
            status.error.clear();
            defaultMaxSamples = cap;
            if (!keepFrame) {
                frame.reset();
            }
            keepFrame = false;
            lastSamples = -1;
            lastConverged = false;
            engineFresh = true;
            ++status.engines;
            needRestart = true;
            continue;
        }
        if (CyclesAovs::isCycles(engineRenderer)) {
            const std::function<int()> threadsFn = threadsOf;
            lock.unlock();
            askThreads(threadsFn);
            const bool stale = engineThreads != CyclesThreads::instance().count();
            if (stale) {
                dropEngine();
            }
            lock.lock();
            if (stale) {
                keepFrame = true;
                continue;
            }
        }
        if (settings != engineSettings) {
            // Some settings, such as Cycles' integrator's, restart a render only when the engine
            // is made under them: it is made again, the frame shown kept until the new one.
            lock.unlock();
            dropEngine();
            lock.lock();
            keepFrame = true;
            continue;
        }
        status.active = true;

        if (stopped) {
            if (!paused) {
                lock.unlock();
                {
                    std::lock_guard<std::mutex> l(engineMutex);
                    if (engine->IsPauseRendererSupported()) {
                        (void)engine->PauseRenderer();
                    }
                    paused = true;
                }
                lock.lock();
                continue;
            }
            status.stopped = true;
            status.paused = true;
            wake.wait(lock);
            continue;
        }
        status.stopped = false;
        status.paused = false;
        if (paused) {
            lock.unlock();
            {
                std::lock_guard<std::mutex> l(engineMutex);
                if (engine->IsPauseRendererSupported()) {
                    (void)engine->ResumeRenderer();
                }
                paused = false;
            }
            lock.lock();
            stallSince = Clock::now();
            continue;
        }

        if (pending) {
            const Clock::time_point now = Clock::now();
            const Clock::time_point due = std::min(lastPending + kRestartQuiet, firstPending + kRestartMaxWait);
            if (now < due) {
                wake.wait_until(lock, due);
                continue;
            }
            pending = false;
            needRestart = true;
        }
        if (needRestart) {
            needRestart = false;
            done = false;
            ++status.restarts;
            status.converged = false;
            restartTime = Clock::now();
            stallSince = restartTime;
            firstPass = true;
            restartWatch.restart(engineFresh);
            engineFresh = false;
        }
        if (done) {
            wake.wait(lock);
            continue;
        }

        const View v = view;
        const int cap = (maxSamples > 0) ? maxSamples : defaultMaxSamples;
        const std::uint64_t passGeneration = generation;
        const std::uint64_t restartIndex = status.restarts;
        lock.unlock();

        const bool readsRestart = firstPass;
        bool rendered = false;
        bool converged = false;
        VtDictionary before;
        VtDictionary stats;
        std::string error;
        std::string renderFailure;
        std::string sessionFailure;
        std::shared_ptr<Frame> read;
        {
            std::unique_lock<std::mutex> stageGuard(*stageLock);
            const UsdStageRefPtr stage = stageOf();
            if (stage) {
                std::lock_guard<std::mutex> l(engineMutex);
                if (!samplesKey.IsEmpty() && (cap > 0) && (cap != appliedCap)) {
                    // A change of a setting restarts the delegate's render.
                    engine->SetRendererSetting(samplesKey, VtValue(cap));
                    appliedCap = cap;
                }
                if (!engineHasView || (engineView != v)) {
                    engine->SetRenderBufferSize(GfVec2i(v.width, v.height));
                    engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), v.width, v.height)));
                    engine->SetCameraState(v.frustum.ComputeViewMatrix(), v.frustum.ComputeProjectionMatrix());
                    engineView = v;
                    engineHasView = true;
                }
                UsdImagingGLRenderParams params;
                params.frame = v.time;
                params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
                params.enableLighting = true;
                params.enableSceneLights = true;
                params.enableSceneMaterials = true;
                params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
                params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
                params.highlight = false;
                if (readsRestart) {
                    before = engine->GetRenderStats();
                }
                if (renderGuarded(engine.get(), stage->GetPseudoRoot(), params, &renderFailure)) {
                    rendered = true;
                    converged = engine->IsConverged();
                    stats = engine->GetRenderStats();
                    if (log && !converged) {
                        // A MoonRay session that could not start or broke never converges.
                        sessionFailure = MoonRayAovs::sessionFailure(stats, log->notices());
                    }
                }
            }
        }
        const int samples = samplesOf(stats, cap);
        const double clock = clockOf(stats);
        lock.lock();
        if (quit) {
            break;
        }
        if (!renderFailure.empty()) {
            // The engine is left in an unknown state: a retry builds a fresh one. Ahead of the
            // generation, or an edit during the pass would hide the failure.
            fail(lock, workingRenderer, "The " + rendererName + " render failed: " + renderFailure, workingEdits);
            continue;
        }
        if (!sessionFailure.empty()) {
            fail(lock, workingRenderer, sessionFailure, workingEdits);
            continue;
        }
        if (generation != passGeneration) {
            // Edited during the pass: what it rendered is stale.
            continue;
        }
        if (!rendered) {
            // No stage yet: the sync that brings one edits it, and restarts.
            wake.wait(lock);
            continue;
        }
        if (readsRestart) {
            firstPass = false;
            restartWatch.before(before, cap);
        }
        const Clock::time_point now = Clock::now();
        const bool restartSeen = restartWatch.after(samples, clock);
        if (!restartSeen) {
            if (!restartWatch.restarting() && (now - restartTime >= kRestartUnseen)) {
                // A new engine has no pixels from before to mistake for the restart's.
                lock.unlock();
                dropEngine();
                lock.lock();
                keepFrame = true;
                continue;
            }
        }
        const bool changed = (samples != lastSamples) || (converged != lastConverged);
        if (samples != lastSamples) {
            stallSince = now;
        }
        lastSamples = samples;
        lastConverged = converged;
        status.samples = samples;
        status.maxSamples = cap;
        // Done at the cap; or, for a delegate counting no samples, or one whose adaptive sampling
        // stopped short of the cap, once it converged and held still. A delegate's word alone is
        // not enough: Cycles marks its buffers converged when a reset cuts a render short.
        const bool atCap = (cap > 0) && (samples >= cap);
        const bool finished = restartSeen && converged && (atCap || (samples < 0) || (now - stallSince >= kConvergedStall));
        bool publish = restartSeen && (samples != 0) && (changed || finished || (samples < 0) || !frame || (frame->restart != restartIndex));
        if (publish) {
            lock.unlock();
            read.reset(new Frame);
            {
                std::lock_guard<std::mutex> l(engineMutex);
                publish = engine && readFrame(v, read.get(), &error);
            }
            lock.lock();
            if (quit) {
                break;
            }
            if (!publish && !error.empty()) {
                fail(lock, workingRenderer, error, workingEdits);
                continue;
            }
            if (generation != passGeneration) {
                continue;
            }
        }
        if (publish) {
            read->samples = samples;
            read->generation = passGeneration;
            read->maxSamples = cap;
            read->converged = finished;
            read->restart = restartIndex;
            read->serial = ++status.frames;
            frame = read;
        }
        done = finished && publish;
        status.converged = done;
        if (publish && listener) {
            lock.unlock();
            listener();
            lock.lock();
        }
        if (!done) {
            wake.wait_for(lock, kRefinePoll);
        }
    }
} // ProgressiveRender::Implementation::refine

ProgressiveRender::ProgressiveRender(std::mutex* stageLock,
                                     const std::function<UsdStageRefPtr()>& stage,
                                     const std::function<void()>& listener,
                                     float background)
    : _imp(new Implementation(stageLock, stage, listener, background))
{
    ++gProgressiveLive;
    Implementation* imp = _imp.get();
    _imp->cyclesHolder = CyclesThreads::instance().addHolder([imp]() {
        std::lock_guard<std::mutex> l(imp->mutex);
        imp->wake.notify_all();
    });
    _imp->worker = std::thread([this]() { _imp->run(); });
}

ProgressiveRender::~ProgressiveRender()
{
    {
        std::lock_guard<std::mutex> l(_imp->mutex);
        _imp->quit = true;
        _imp->wake.notify_all();
    }
    if (_imp->worker.joinable()) {
        _imp->worker.join();
    }
    CyclesThreads::instance().removeHolder(_imp->cyclesHolder);
    --gProgressiveLive;
}

void
ProgressiveRender::setRenderer(const std::string& id)
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    if (id == _imp->renderer) {
        return;
    }
    _imp->renderer = id;
    _imp->defaultMaxSamples = 0;
    _imp->failedRenderer.clear();
    _imp->status = Status();
    _imp->frame.reset();
    _imp->edited();
}

void
ProgressiveRender::setThreads(const std::function<int()>& threads)
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    _imp->threadsOf = threads;
    _imp->wake.notify_all();
}

void
ProgressiveRender::setView(const View& view)
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    if (_imp->hasView && (_imp->view == view)) {
        return;
    }
    _imp->view = view;
    _imp->hasView = (view.width > 0) && (view.height > 0);
    _imp->edited();
}

void
ProgressiveRender::invalidate()
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    ++_imp->sceneEdits;
    _imp->edited();
}

void
ProgressiveRender::setSettings(const std::map<std::string, VtValue>& settings)
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    if (settings == _imp->settings) {
        return;
    }
    _imp->settings = settings;
    ++_imp->sceneEdits;
    _imp->edited();
}

void
ProgressiveRender::setMaxSamples(int samples)
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    samples = std::max(0, samples);
    if (samples == _imp->maxSamples) {
        return;
    }
    _imp->maxSamples = samples;
    ++_imp->sceneEdits;
    _imp->edited();
}

int
ProgressiveRender::getMaxSamples() const
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    return (_imp->maxSamples > 0) ? _imp->maxSamples : _imp->defaultMaxSamples;
}

void
ProgressiveRender::stop()
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    _imp->stopped = true;
    _imp->wake.notify_all();
}

void
ProgressiveRender::resume()
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    _imp->stopped = false;
    _imp->wake.notify_all();
}

ProgressiveRender::Status
ProgressiveRender::getStatus() const
{
    std::lock_guard<std::mutex> l(_imp->mutex);
    Status status = _imp->status;

    // An edit waits out the quiet period before the worker restarts; until then the last frame
    // is not what the render converges to.
    if (_imp->pending) {
        status.converged = false;
    }
    status.stopped = _imp->stopped;
    status.maxSamples = (_imp->maxSamples > 0) ? _imp->maxSamples : _imp->defaultMaxSamples;

    return status;
}

std::shared_ptr<const ProgressiveRender::Frame>
ProgressiveRender::getFrame() const
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    return _imp->frame;
}

std::uint64_t
ProgressiveRender::getRestartCount() const
{
    std::lock_guard<std::mutex> l(_imp->mutex);

    return _imp->status.restarts;
}

std::string
ProgressiveRender::pick(const Frame& frame,
                        int x,
                        int y)
{
    std::lock_guard<std::mutex> stageGuard(*_imp->stageLock);
    std::lock_guard<std::mutex> l(_imp->engineMutex);
    UsdImagingGLEngine* engine = _imp->engine.get();

    if (!engine || !_imp->engineHasView) {
        return std::string();
    }
    {
        // Every edit bumps the generation before the engine sees it, and the worker changes the
        // engine only under engineMutex: an unchanged generation means the AOVs are frame's render.
        std::lock_guard<std::mutex> g(_imp->mutex);
        if (frame.generation != _imp->generation) {
            return std::string();
        }
    }
    const int width = _imp->engineView.width;
    const int height = _imp->engineView.height;
    if ((width != frame.width) || (height != frame.height)) {
        return std::string();
    }
    if ((x < 0) || (y < 0) || (x >= width) || (y >= height)) {
        return std::string();
    }
    std::string error;
    MappedAov primIds(engine, _imp->rendererName, width, height, HdAovTokens->primId, &error);
    if (!primIds.valid()) {
        return std::string();
    }
    MappedAov instanceIds(engine, _imp->rendererName, width, height, HdAovTokens->instanceId, &error);
    if (!instanceIds.valid()) {
        return std::string();
    }
    const std::size_t i = (std::size_t)y * (std::size_t)width + (std::size_t)x;
    const std::int32_t prim = readInt(primIds.pixel(i));
    if (prim < 0) {
        return std::string();
    }
    const std::int32_t instance = readInt(instanceIds.pixel(i));
    SdfPath path;
    if (!engine->DecodeIntersection(prim, CyclesAovs::isCycles(_imp->engineRenderer) ? CyclesAovs::instanceIndex(instance) : instance, &path)) {
        return std::string();
    }

    return path.IsEmpty() ? std::string() : path.GetString();
}

int
ProgressiveRender::getLiveCount()
{
    return gProgressiveLive.load();
}
} // namespace HydraRender

namespace SceneCaches {
void
releaseRenderEngines()
{
    HydraRender::releaseAll();
}

void
releaseCpuRenderEngines()
{
    HydraRender::releaseAllCpuEngines();
}
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT
