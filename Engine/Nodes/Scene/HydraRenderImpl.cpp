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
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#ifdef emit
#define NATRON_HYDRARENDER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
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
#include <pxr/usd/usd/primRange.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_HYDRARENDER_RESTORE_EMIT
#undef NATRON_HYDRARENDER_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraGLContext.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Storm renders a frame in passes until it converges; a rasterizer needs one.
const int kMaxRenderPasses = 8;

// The GPU memory the engines may hold together by default.
const std::size_t kDefaultMemoryBudget = (std::size_t)2 << 30;

std::mutex gFactoryMutex;
HydraGLContextFactory gFactory;

std::atomic<std::size_t> gMemoryBudget(kDefaultMemoryBudget);
std::atomic<std::uint64_t> gEngineBuilds(0);

struct EngineEntry {
    // The stage the engine shows, for the entry's whole life; each render syncs it to the
    // request's scene. Declared first so that the engine, which reads it, goes first.
    std::unique_ptr<SceneStage> stage;
    std::unique_ptr<UsdImagingGLEngine> engine;
    // Whether the engine lights the stage with a headlight, as it does when the stage has no light.
    bool headlight = false;
    // When the entry last rendered, by the thread's render serial, for the LRU.
    std::uint64_t lastUse = 0;
    // The GPU memory the engine held after its last render.
    std::size_t bytes = 0;

    // What the engine last rendered, which its AOVs hold until it renders again.
    bool rendered = false;
    std::uint64_t serial = 0;
    U64 writeCount = 0;
    UsdTimeCode time;
    GfMatrix4d view;
    GfMatrix4d projection;
    int width = 0;
    int height = 0;

    // That render's primId plane and the paths of its hashes, decoded on the CPU when first asked for.
    bool idsDecoded = false;
    std::vector<float> primId;
    std::map<std::uint32_t, std::string> primPaths;
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

// Whether stage has a light of its own; without one, renders get a light at the camera.
bool
hasSceneLight(const UsdStageRefPtr& stage)
{
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.HasAPI<UsdLuxLightAPI>()) {
            return true;
        }
    }

    return false;
}

// The light at the camera usdview and the viewer's scene view give a stage without lights.
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
 * The thread every render runs on, and what only it touches: the GL context, the engines and
 * the framebuffer Storm renders under. Never destroyed: at exit its thread is still waiting for
 * work, and the context and engines go with the process.
 **/
class RenderThread {
public:
    static RenderThread& instance()
    {
        static RenderThread* thread = new RenderThread;

        return *thread;
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
                    std::string* error)
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

    void releaseEngine(const void* owner)
    {
        std::map<const void*, EngineEntry>::iterator found = _engines.find(owner);
        if (found == _engines.end()) {
            return;
        }
        if (!_context || !_context->makeCurrent()) {
            // Its GL objects cannot be freed without the context; leak them with the engine.
            (void)found->second.engine.release();
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
    // request; null with *error set on failure.
    EngineEntry* renderStorm(const HydraRender::Request& request,
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

    // The GL name of Storm's resolved texture of AOV name, or 0 with *error set.
    GLuint aovTexture(EngineEntry& entry,
                      const TfToken& name,
                      std::string* error);

    RenderThread() = default;

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
    MappedAov(UsdImagingGLEngine* engine,
              const TfToken& name,
              int width,
              int height,
              std::string* error)
    {
        _buffer = engine->GetAovRenderBuffer(name);
        if (!_buffer) {
            *error = "Storm rendered no " + name.GetString() + " AOV.";

            return;
        }
        _buffer->Resolve();
        if (((int)_buffer->GetWidth() != width) || ((int)_buffer->GetHeight() != height)) {
            *error = "Storm's " + name.GetString() + " AOV is not the size asked for.";
            _buffer = nullptr;

            return;
        }
        _data = static_cast<const unsigned char*>(_buffer->Map());
        if (!_data) {
            *error = "Storm's " + name.GetString() + " AOV could not be read back.";
            _buffer = nullptr;

            return;
        }
        _format = _buffer->GetFormat();
        _stride = HdDataSizeOfFormat(_format);
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

EngineEntry*
RenderThread::renderStorm(const HydraRender::Request& request,
                          std::string* error)
{
    if (!ensureContext(error)) {
        return nullptr;
    }
    const int width = request.width;
    const int height = request.height;
    const TfTokenVector aovs = { HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId };

    EngineEntry& entry = _engines[request.owner];
    _engineCount = _engines.size();
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
    const UsdStageRefPtr& stage = entry.stage->getStage();
    bool headlight = entry.headlight;
    if (edited || !entry.engine) {
        headlight = !hasSceneLight(stage);
    }
    // A headlight once set cannot be taken back, so the engine is made again when the stage gains
    // or loses its lights.
    if (!entry.engine || (headlight != entry.headlight)) {
        entry.engine.reset();
        entry.rendered = false;
        UsdImagingGLEngine::Parameters parameters;
        parameters.driver.name = HgiTokens->renderDriver;
        parameters.driver.driver = VtValue(_hgi.get());
        entry.engine.reset(new UsdImagingGLEngine(parameters));
        ++gEngineBuilds;
        entry.engine->SetEnablePresentation(false);
        if (!entry.engine->SetRendererAovs(aovs)) {
            *error = "Storm on " + contextDescription() + " offers no color, depth and primId AOVs.";
            _engines.erase(request.owner);
            _engineCount = _engines.size();

            return nullptr;
        }
        entry.headlight = headlight;
        if (headlight) {
            setHeadlight(entry.engine.get());
        }
    }
    UsdImagingGLEngine* engine = entry.engine.get();
    const GfMatrix4d view = request.frustum.ComputeViewMatrix();
    const GfMatrix4d projection = request.frustum.ComputeProjectionMatrix();
    const U64 writeCount = SceneWriteBarrier::getWriteCount();
    entry.lastUse = ++_useSerial;
    if (entry.rendered && !edited && (entry.writeCount == writeCount) && (entry.time == request.time) && (entry.view == view) && (entry.projection == projection) && (entry.width == width) && (entry.height == height)) {
        return &entry;
    }
    entry.rendered = false;
    entry.idsDecoded = false;
    entry.primId.clear();
    entry.primPaths.clear();

    bindFramebuffer(width, height);
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
    for (int pass = 0; pass < kMaxRenderPasses; ++pass) {
        engine->Render(stage->GetPseudoRoot(), params);
        if (engine->IsConverged()) {
            break;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    entry.rendered = true;
    entry.serial = ++_renderSerial;
    entry.writeCount = writeCount;
    entry.time = request.time;
    entry.view = view;
    entry.projection = projection;
    entry.width = width;
    entry.height = height;

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
} // RenderThread::renderStorm

bool
RenderThread::cpuFrame(EngineEntry& entry,
                       HydraRender::Result* result,
                       std::string* error)
{
    UsdImagingGLEngine* engine = entry.engine.get();
    const int width = entry.width;
    const int height = entry.height;
    const GfMatrix4d& projection = entry.projection;
    const std::size_t pixels = (std::size_t)width * (std::size_t)height;

    {
        MappedAov color(engine, HdAovTokens->color, width, height, error);
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
        // Storm's depth is the window depth in [0, 1]: back to the camera space, the inverse
        // projection gives the point's distance along the view axis.
        MappedAov depth(engine, HdAovTokens->depth, width, height, error);
        if (!depth.valid()) {
            return false;
        }
        const HdFormat componentFormat = HdGetComponentFormat(depth.format());
        const GfMatrix4d inverse = projection.GetInverse();
        result->depth.assign(pixels, std::numeric_limits<float>::infinity());
        for (int y = 0; y < height; ++y) {
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

    if (!decodeIds(entry, error)) {
        return false;
    }
    result->primId = entry.primId;
    result->primPaths = entry.primPaths;

    return true;
} // RenderThread::cpuFrame

bool
RenderThread::decodeIds(EngineEntry& entry,
                        std::string* error)
{
    if (entry.idsDecoded) {
        return true;
    }
    UsdImagingGLEngine* engine = entry.engine.get();
    const int width = entry.width;
    const int height = entry.height;
    const std::size_t pixels = (std::size_t)width * (std::size_t)height;
    {
        MappedAov primIds(engine, HdAovTokens->primId, width, height, error);
        if (!primIds.valid()) {
            return false;
        }
        MappedAov instanceIds(engine, HdAovTokens->instanceId, width, height, error);
        if (!instanceIds.valid()) {
            return false;
        }
        entry.primId.assign(pixels, 0.f);
        entry.primPaths.clear();
        std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t> decoded;
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::int32_t prim = readInt(primIds.pixel(i));
            if (prim < 0) {
                continue;
            }
            const std::pair<std::int32_t, std::int32_t> key(prim, readInt(instanceIds.pixel(i)));
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
    std::string packError;
    if (!packAvailable(&packError)) {
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
    const std::size_t count = window.floatCount();
    pixels->assign(count, 0.f);
    if (count == 0) {
        return true;
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

bool
render(const Request& request,
       Result* result,
       std::string* error)
{
    if (request.scene.isEmpty() || (request.width <= 0) || (request.height <= 0)) {
        *error = "Nothing to render.";

        return false;
    }
    bool ok = false;
    RenderThread& thread = RenderThread::instance();
    const bool ran = thread.runAndWait([&]() {
        try {
            EngineEntry* entry = thread.renderStorm(request, error);
            ok = entry && thread.frame(*entry, result, error);
        } catch (const std::exception& e) {
            *error = std::string("Storm failed: ") + e.what();
            ok = false;
        }
    },
                                       error);

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
    bool ok = false;
    RenderThread& thread = RenderThread::instance();
    const bool ran = thread.runAndWait([&]() {
        try {
            EngineEntry* entry = thread.renderStorm(request, error);
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
    RenderThread& thread = RenderThread::instance();

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
    RenderThread& thread = RenderThread::instance();

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
    bool ok = false;
    RenderThread& thread = RenderThread::instance();
    const bool ran = thread.runAndWait([&]() {
        try {
            EngineEntry* entry = thread.renderStorm(request, error);
            if (!entry) {
                return;
            }
            ok = thread.readWindow(*entry, window, pixels, error);
            if (ok && primPaths && (window.plane == ePlanePrimId)) {
                ok = thread.decodeIds(*entry, error);
                *primPaths = entry->primPaths;
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
packsOnGpu()
{
    bool ok = false;
    RenderThread& thread = RenderThread::instance();

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
    RenderThread& thread = RenderThread::instance();

    (void)thread.post([&thread, owner]() { thread.releaseEngine(owner); }, false);
}

void
releaseAll()
{
    RenderThread& thread = RenderThread::instance();

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
    RenderThread& thread = RenderThread::instance();

    // Runs nothing if the thread never started, in which case there is no engine.
    std::promise<void> drained;
    std::future<void> done = drained.get_future();
    if (thread.post([&drained]() { drained.set_value(); }, false)) {
        done.wait();
    }

    return thread.engineCount();
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
    RenderThread& thread = RenderThread::instance();

    const bool ran = thread.runAndWait([&]() {
        if (thread.ensureContext(error)) {
            description = thread.contextDescription();
        }
    },
                                       error);

    return ran ? description : std::string();
}
} // namespace HydraRender

namespace SceneCaches {
void
releaseRenderEngines()
{
    HydraRender::releaseAll();
}
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT
