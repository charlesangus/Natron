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
#include <condition_variable>
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
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_HYDRARENDER_RESTORE_EMIT
#undef NATRON_HYDRARENDER_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraGLContext.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Storm renders a frame in passes until it converges; a rasterizer needs one.
const int kMaxRenderPasses = 8;

std::mutex gFactoryMutex;
HydraGLContextFactory gFactory;

struct EngineEntry {
    std::unique_ptr<UsdImagingGLEngine> engine;
    UsdStageRefPtr stage;
};

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

    // Runs job on the thread and waits for it.
    template <typename Fn>
    void runAndWait(Fn job)
    {
        std::promise<void> done;
        std::future<void> finished = done.get_future();
        post([&job, &done]() {
            job();
            done.set_value();
        },
             true);
        finished.wait();
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
        if (_context && _context->makeCurrent()) {
            _engines.erase(found);
        } else {
            // Its GL objects cannot be freed without the context; leak them with the engine.
            (void)found->second.engine.release();
            _engines.erase(found);
        }
    }

    bool render(const HydraRender::Request& request,
                HydraRender::Result* result,
                std::string* error);

private:
    RenderThread() = default;

    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(_mutex);
                _wake.wait(l, [this]() { return !_jobs.empty(); });
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }
            job();
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

    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<std::function<void()>> _jobs;
    bool _started = false;

    std::unique_ptr<HydraGLContext> _context;
    bool _contextTried = false;
    std::string _contextError;
    std::map<const void*, EngineEntry> _engines;
    GLuint _framebuffer = 0;
    GLuint _renderbuffers[2] = { 0, 0 };
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

bool
RenderThread::render(const HydraRender::Request& request,
                     HydraRender::Result* result,
                     std::string* error)
{
    if (!ensureContext(error)) {
        return false;
    }
    const int width = request.width;
    const int height = request.height;
    const TfTokenVector aovs = { HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId };

    EngineEntry& entry = _engines[request.owner];
    if (!entry.engine || (entry.stage != request.stage)) {
        entry.engine.reset();
        entry.stage = request.stage;
        entry.engine.reset(new UsdImagingGLEngine(UsdImagingGLEngine::Parameters()));
        entry.engine->SetEnablePresentation(false);
        if (!entry.engine->SetRendererAovs(aovs)) {
            *error = "Storm on " + contextDescription() + " offers no color, depth and primId AOVs.";
            _engines.erase(request.owner);

            return false;
        }
        if (!hasSceneLight(request.stage)) {
            setHeadlight(entry.engine.get());
        }
    }
    UsdImagingGLEngine* engine = entry.engine.get();

    bindFramebuffer(width, height);
    engine->SetRenderBufferSize(GfVec2i(width, height));
    engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
    const GfMatrix4d projection = request.frustum.ComputeProjectionMatrix();
    engine->SetCameraState(request.frustum.ComputeViewMatrix(), projection);

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
        engine->Render(request.stage->GetPseudoRoot(), params);
        if (engine->IsConverged()) {
            break;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

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

    {
        MappedAov primIds(engine, HdAovTokens->primId, width, height, error);
        if (!primIds.valid()) {
            return false;
        }
        MappedAov instanceIds(engine, HdAovTokens->instanceId, width, height, error);
        if (!instanceIds.valid()) {
            return false;
        }
        result->primId.assign(pixels, 0.f);
        result->primPaths.clear();
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
                    result->primPaths[hash] = path.GetString();
                }
                decoded[key] = hash;
            }
            if (hash) {
                result->primId[i] = HydraRender::hashToFloat(hash);
            }
        }
    }

    return true;
} // RenderThread::render

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
    if (!request.stage || (request.width <= 0) || (request.height <= 0)) {
        *error = "Nothing to render.";

        return false;
    }
    bool ok = false;
    RenderThread& thread = RenderThread::instance();
    thread.runAndWait([&]() {
        try {
            ok = thread.render(request, result, error);
        } catch (const std::exception& e) {
            *error = std::string("Storm failed: ") + e.what();
            ok = false;
        }
    });

    return ok;
}

void
release(const void* owner)
{
    RenderThread& thread = RenderThread::instance();

    (void)thread.post([&thread, owner]() { thread.releaseEngine(owner); }, false);
}

std::string
contextDescription(std::string* error)
{
    std::string description;
    RenderThread& thread = RenderThread::instance();

    thread.runAndWait([&]() {
        if (thread.ensureContext(error)) {
            description = thread.contextDescription();
        }
    });

    return description;
}
} // namespace HydraRender

NATRON_NAMESPACE_EXIT
