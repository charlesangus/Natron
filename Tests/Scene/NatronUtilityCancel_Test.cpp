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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"
#include "Engine/Nodes/Scene/Utility/WavefrontCpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/AppManager.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Gpu/GpuTestDevice.h"
#include "Gpu/GpuTestSelector.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
typedef std::chrono::steady_clock Clock;
typedef HydraRender::ProgressiveRender ProgressiveRender;

const std::chrono::milliseconds kCancelBound(500);
const std::chrono::seconds kTimeout(300);
const int kHugeGrid = 1500;
const int kManyGridsAcross = 16;
const int kManyGridSize = 100;
const int kLatencyWindow = 4096;
const int kViewWindow = 1024;
const double kCameraZ = 30.;
const int kEndMs = 60000;

// A material emitting the display colour, which the scenes here show without lights.
UsdShadeMaterial
emitDisplayColor(const UsdStageRefPtr& stage)
{
    const SdfPath path("/EmitDisplayColor");
    UsdShadeMaterial material = UsdShadeMaterial::Get(stage, path);

    if (material) {
        return material;
    }
    material = UsdShadeMaterial::Define(stage, path);

    UsdShadeShader reader = UsdShadeShader::Define(stage, path.AppendChild(TfToken("Reader")));
    UsdShadeShader surface = UsdShadeShader::Define(stage, path.AppendChild(TfToken("Surface")));

    reader.CreateIdAttr(VtValue(TfToken("UsdPrimvarReader_float3")));
    reader.CreateInput(TfToken("varname"), SdfValueTypeNames->Token).Set(TfToken("displayColor"));
    surface.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    surface.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).ConnectToSource(reader.ConnectableAPI(), TfToken("result"));
    surface.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.f));
    material.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), TfToken("surface"));

    return material;
}

// An n by n grid of quads, width units wide and facing the camera, n * n * 2 triangles.
void
defineGrid(const UsdStageRefPtr& stage,
           const std::string& path,
           int n,
           const GfVec3f& color,
           const GfVec3f& centre = GfVec3f(0.f),
           float width = 20.f)
{
    VtVec3fArray points;
    VtIntArray counts(n * n, 4);
    VtIntArray indices;
    const float left = centre[0] - width / 2.f;
    const float bottom = centre[1] - width / 2.f;

    points.reserve((n + 1) * (n + 1));
    indices.reserve(n * n * 4);
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            points.push_back(GfVec3f(left + width * x / n, bottom + width * y / n, centre[2]));
        }
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int corner = y * (n + 1) + x;

            indices.push_back(corner);
            indices.push_back(corner + 1);
            indices.push_back(corner + n + 2);
            indices.push_back(corner + n + 1);
        }
    }

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(path));

    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    mesh.CreateDisplayColorAttr(VtValue(VtVec3fArray(1, color)));
    UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(emitDisplayColor(stage));
}

UsdStageRefPtr
hugeMeshStage(const GfVec3f& color)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();

    defineGrid(stage, "/Huge", kHugeGrid, color);

    return stage;
}

// The grids tile the view side by side rather than stack: stacked, every ray walks every grid's
// hierarchy, and the frame's tracing outgrows the test's time limit on slower runners. A grid is
// centred on the axis so the centre pixel never falls on a seam.
UsdStageRefPtr
manyMeshesStage(const GfVec3f& color)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const float width = 20.f / kManyGridsAcross;

    for (int row = 0; row < kManyGridsAcross; ++row) {
        for (int column = 0; column < kManyGridsAcross; ++column) {
            const GfVec3f centre(width * (column - kManyGridsAcross / 2 + 1), width * (row - kManyGridsAcross / 2 + 1), 0.f);

            defineGrid(stage, "/Grid" + std::to_string(row * kManyGridsAcross + column), kManyGridSize, color, centre, width);
        }
    }

    return stage;
}

// The scene the node layer layer authors, as a node upstream hands it on.
ScenePayload
payloadOf(const SdfLayerRefPtr& layer,
          U64 hash)
{
    return SceneOps::appendLayer(ScenePayload(), layer, hash);
}

SdfLayerRefPtr
hugeMeshLayer(const GfVec3f& color)
{
    SdfLayerRefPtr layer = SceneOps::createNodeLayer("NatronUtilityCancelHuge");

    defineGrid(UsdStage::Open(layer), "/Huge", kHugeGrid, color);

    return layer;
}

// base with the display colour of the prim at path overridden by color, as a node downstream would.
ScenePayload
recoloured(const ScenePayload& base,
           const std::string& path,
           const GfVec3f& color,
           U64 hash)
{
    SdfLayerRefPtr layer = SceneOps::createNodeLayer("NatronUtilityCancelRecolour");
    SdfPrimSpecHandle prim = SdfCreatePrimInLayer(layer, SdfPath(path));

    SdfAttributeSpec::New(prim, "primvars:displayColor", SdfValueTypeNames->Color3fArray)->SetDefaultValue(VtValue(VtVec3fArray(1, color)));

    return SceneOps::appendLayer(base, layer, hash);
}

GfFrustum
viewFrom(double x)
{
    GfFrustum camera;

    camera.SetPerspective(45., 1., 0.1, 1000.);
    camera.SetPosition(GfVec3d(x, 0., kCameraZ));

    return camera;
}

// The CPU, the GPU handing its colour over in tiles, and the GPU keeping it in a film on the device.
enum class Backend {
    Cpu,
    GpuTiles,
    GpuFilm,
};

void
PrintTo(Backend backend,
        std::ostream* os)
{
    *os << (backend == Backend::Cpu ? "Cpu" : (backend == Backend::GpuTiles ? "GpuTiles" : "GpuFilm"));
}

#ifdef NATRON_HAVE_GPU
// Puts a GPU context, on the test's device, where the delegate looks for one, and the old one back after.
class CancelGpuRuntime {
public:
    CancelGpuRuntime()
    {
        GpuContextOptions options;
        std::shared_ptr<std::string> logged = std::make_shared<std::string>();

        options.mode = GpuResolvedMode::Force;
        options.device = gputest::cpuFallbackSelector();
        options.infoLog = [logged](const std::string& line) {
            *logged = line;
        };

        std::unique_ptr<GpuContext> own(new GpuContext(options));

        if (!own->waitForInit(std::chrono::seconds(60)) || !own->isAvailable()) {
            unavailable = logged->empty() ? std::string("the GPU context did not come up") : *logged;

            return;
        }
        _saved = appPTR->swapGpuContextForTests(std::move(own));
        _swapped = true;
    }

    ~CancelGpuRuntime()
    {
        if (_swapped) {
            appPTR->swapGpuContextForTests(std::move(_saved));
        }
    }

    std::string missingSupport() const
    {
        return WavefrontGpu::missingSupport(*appPTR->getGpuContext()->runtime());
    }

    std::string unavailable;

private:
    std::unique_ptr<GpuContext> _saved;
    bool _swapped = false;
};
#endif // NATRON_HAVE_GPU

// The delegate through the same engine the node drives it with, at a size whose frame takes a while.
class HeavyRender {
public:
    explicit HeavyRender(const UsdStageRefPtr& stage,
                         Backend backend = Backend::Cpu)
        : _stage(stage)
    {
        RenderDelegates::list();

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        // The default grid of samples traces the 4096-pixel frame that many times over, which a debug
        // build cannot finish within the suite's limit; one pass keeps the frame heavy but finishable.
        _engine->SetRendererSetting(TfToken(kUtilitySettingPixelSamples), VtValue(1));
        // Nor does it trace reflections, whose extra bounces the limit was not set for.
        _engine->SetRendererSetting(TfToken(kUtilitySettingMaxRayDepth), VtValue(0));
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(backend == Backend::Cpu ? kUtilityDeviceCpu : kUtilityDeviceGpu)));
        if (backend == Backend::GpuFilm) {
            // The colour alone is the one buffer the GPU may keep on the device.
            EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color }));
        } else {
            EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId }));
        }
    }

    // The device the last frame rendered on, as the delegate reports it.
    std::string device() const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), kUtilityStatDevice, VtDefault = std::string());
    }

    // size is the frame's side in pixels; a frame that is only drawn to its end for its colour uses a
    // small one, as the scene's preparation, which the stops cut short, does not depend on it.
    void render(double cameraX = 0.,
                int size = kLatencyWindow)
    {
        const GfFrustum camera = viewFrom(cameraX);

        _size = size;
        _engine->SetRenderBufferSize(GfVec2i(size, size));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), size, size)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(_stage->GetPseudoRoot(), params);
    }

    bool renderToTheEnd(double cameraX = 0.,
                        int size = kLatencyWindow)
    {
        const Clock::time_point end = Clock::now() + kTimeout;

        for (;;) {
            render(cameraX, size);
            if (_engine->IsConverged()) {
                return true;
            }
            if (Clock::now() >= end) {
                ADD_FAILURE() << "the frame did not finish in " << kTimeout.count() << " s";

                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // How long a blocking stop takes.
    std::chrono::milliseconds timedStop()
    {
        const Clock::time_point start = Clock::now();

        EXPECT_TRUE(_engine->StopRenderer());

        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    }

    bool isConverged() const
    {
        return _engine->IsConverged();
    }

    void restart()
    {
        EXPECT_TRUE(_engine->RestartRenderer());
    }

    GfVec4f centreColor() const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no colour buffer";

            return GfVec4f(0.f);
        }

        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());
        const GfVec4f value = pixels[(_size / 2) * _size + _size / 2];

        buffer->Unmap();

        return value;
    }

private:
    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
    int _size = kLatencyWindow;
};

// Stops a frame at several points of its preparation and drawing: a stop takes less than the bound and
// leaves it unfinished, and the render after it finishes correctly.
void
checkCancelLatency(const UsdStageRefPtr& stage,
                   const char* name,
                   Backend backend)
{
    if (backend != Backend::Cpu) {
        // The first GPU frame compiles the kernels, which is no stop point of the frame's; a small scene
        // has them compiled before the frames whose stops are timed.
        UsdStageRefPtr small = UsdStage::CreateInMemory();

        defineGrid(small, "/Warm", 2, GfVec3f(1.f, 0.f, 0.f));

        HeavyRender warm(small, backend);

        ASSERT_TRUE(warm.renderToTheEnd(0., kViewWindow));
        ASSERT_EQ(0u, warm.device().find("GPU")) << warm.device();
    }

    HeavyRender render(stage, backend);
    int stopsOfARunningFrame = 0;

    for (const int delayMs : { 0, 20, 100, 300 }) {
        render.render();
        if (render.isConverged()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        const std::chrono::milliseconds took = render.timedStop();

        std::cout << "[   NOTE   ] " << name << ": a stop " << delayMs << " ms into the frame took " << took.count() << " ms" << std::endl;
        EXPECT_LT(took.count(), kCancelBound.count()) << delayMs << " ms into the frame";
        render.render();
        EXPECT_FALSE(render.isConverged()) << "a stopped frame finished, " << delayMs << " ms into the frame";
        render.restart();
        ++stopsOfARunningFrame;
    }
    EXPECT_GT(stopsOfARunningFrame, 0) << "the scene is too light to test this";

    ASSERT_TRUE(render.renderToTheEnd(0., kViewWindow));
    EXPECT_FLOAT_EQ(1.f, render.centreColor()[0]);
    if (backend != Backend::Cpu) {
        EXPECT_EQ(0u, render.device().find("GPU")) << render.device();
    }

    // Now that the scene is prepared, a new view spends its time on the tiles alone.
    render.render(0.5);
    if (!render.isConverged()) {
        const std::chrono::milliseconds took = render.timedStop();

        std::cout << "[   NOTE   ] " << name << ": a stop while drawing took " << took.count() << " ms" << std::endl;
        EXPECT_LT(took.count(), kCancelBound.count());
        render.restart();
    }
    ASSERT_TRUE(render.renderToTheEnd(0.5, kViewWindow));
    EXPECT_FLOAT_EQ(1.f, render.centreColor()[0]);
}

ProgressiveRender::View
viewOf(bool showRender)
{
    ProgressiveRender::View view;

    view.frustum = viewFrom(0.);
    view.width = kViewWindow;
    view.height = kViewWindow;
    view.time = UsdTimeCode::Default();
    view.showRender = showRender;

    return view;
}

// The progressive render asks for four AOVs, so on the GPU its colour comes in tiles.
class NatronUtilityCancelTest
    : public BaseTest,
      public ::testing::WithParamInterface<Backend> {
protected:
    // One sample a pixel, so that every frame published is a finished one.
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _stage.reset(new SceneStage);
        _passes = 0;
        if (GetParam() == Backend::Cpu) {
            _ready = true;
        } else {
#ifdef NATRON_HAVE_GPU
            _gpu.reset(new CancelGpuRuntime);
            GPUTEST_DEVICE_OR_SKIP(_gpu->unavailable.empty(), _gpu->unavailable);

            const std::string missing = _gpu->missingSupport();

            if (!missing.empty()) {
                GPUTEST_SKIP() << missing;
            }
            _ready = true;
#else
            std::printf("[  SKIPPED ] Natron was built without the Vulkan backend, so there is no GPU to render on\n");

            return;
#endif
        }
        HydraRender::setRendererSettings(kRenderDelegateUtilityId, { { kUtilitySettingPixelSamples, PXR_NS::VtValue(1) }, { kUtilitySettingMaxRayDepth, PXR_NS::VtValue(0) }, { kUtilitySettingDevice, PXR_NS::VtValue(TfToken(GetParam() == Backend::Cpu ? kUtilityDeviceCpu : kUtilityDeviceGpu)) } });
    }

    virtual void TearDown() OVERRIDE
    {
        ProgressiveRender::setPassHook(std::function<void()>());
        _render.reset();
        EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
        _stage.reset();
        HydraRender::setRendererSettings(kRenderDelegateUtilityId, std::map<std::string, PXR_NS::VtValue>());
#ifdef NATRON_HAVE_GPU
        _gpu.reset();
#endif
        BaseTest::TearDown();
    }

    bool ready() const
    {
        return _ready;
    }

    // Syncs payload into the stage under the stage lock, and restarts the render if that edited it,
    // as the viewer's sync does. The restart the edit brings, or 0 if it changed nothing.
    std::uint64_t sync(const ScenePayload& payload)
    {
        std::lock_guard<std::mutex> l(_stageLock);

        if (!_stage->sync(payload)) {
            return 0;
        }

        const std::uint64_t restart = _render ? _render->getRestartCount() + 1 : 0;

        if (_render) {
            _render->invalidate();
        }

        return restart;
    }

    void start(const ScenePayload& payload,
               bool showRender = false)
    {
        RenderDelegates::list();
        sync(payload);
        ASSERT_TRUE(_stage->isValid());

        SceneStage* stage = _stage.get();
        std::atomic<int>* passes = &_passes;
        const std::function<bool(SceneStageSnapshot*)> snapshot = [stage](SceneStageSnapshot* into) {
            *into = stage->getSnapshot();

            return true;
        };

        ProgressiveRender::setPassHook([passes]() { ++*passes; });
        _render.reset(new ProgressiveRender(&_stageLock, snapshot, std::function<void()>()));
        _render->setRenderer(kRenderDelegateUtilityId);
        _render->setThreads([]() { return 0; });
        _render->setView(viewOf(showRender));
    }

    // Keeps each frame published, until a finished frame of a render at least as late as minRestart is
    // the latest. True if that came.
    bool collectUntilFinished(std::uint64_t minRestart,
                              std::vector<std::shared_ptr<const ProgressiveRender::Frame>>* frames)
    {
        const Clock::time_point end = Clock::now() + kTimeout;

        while (Clock::now() < end) {
            const ProgressiveRender::Status status = _render->getStatus();
            const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();

            if (!status.error.empty()) {
                ADD_FAILURE() << status.error;

                return false;
            }
            if (frame && (frames->empty() || (frames->back()->serial != frame->serial))) {
                frames->push_back(frame);
            }
            if (status.converged && frame && frame->converged && (frame->restart == status.restarts) && (status.restarts >= minRestart)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "the render did not finish in " << kTimeout.count() << " s";

        return false;
    }

    // The pass hook runs just before the delegate renders, so a while after it the frame is under way.
    void waitForFrameUnderWay()
    {
        const Clock::time_point end = Clock::now() + kTimeout;

        while (_passes.load() == 0 && Clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_GT(_passes.load(), 0) << "the worker started no pass in " << kTimeout.count() << " s";
        ASSERT_TRUE(_render->getStatus().active);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    static GfVec4f centreOf(const ProgressiveRender::Frame& frame)
    {
        const std::size_t at = (static_cast<std::size_t>(frame.height / 2) * frame.width + frame.width / 2) * 4;

        return GfVec4f((float)frame.rgba[at], (float)frame.rgba[at + 1], (float)frame.rgba[at + 2], (float)frame.rgba[at + 3]);
    }

    std::mutex _stageLock;
    std::unique_ptr<SceneStage> _stage;
    std::atomic<int> _passes { 0 };
    std::unique_ptr<ProgressiveRender> _render;

private:
    bool _ready = false;
#ifdef NATRON_HAVE_GPU
    std::unique_ptr<CancelGpuRuntime> _gpu;
#endif
};

// Brings the delegate's GPU context up for the GPU backends; ready() says whether the test can run.
class NatronUtilityCancelLatency : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() OVERRIDE
    {
        if (GetParam() == Backend::Cpu) {
            _ready = true;

            return;
        }
#ifdef NATRON_HAVE_GPU
        _gpu.reset(new CancelGpuRuntime);
        GPUTEST_DEVICE_OR_SKIP(_gpu->unavailable.empty(), _gpu->unavailable);

        const std::string missing = _gpu->missingSupport();

        if (!missing.empty()) {
            GPUTEST_SKIP() << missing;
        }
        _ready = true;
#else
        std::printf("[  SKIPPED ] Natron was built without the Vulkan backend, so there is no GPU to render on\n");
#endif
    }

    bool ready() const
    {
        return _ready;
    }

private:
    bool _ready = false;
#ifdef NATRON_HAVE_GPU
    std::unique_ptr<CancelGpuRuntime> _gpu;
#endif
};
} // anon namespace

TEST_P(NatronUtilityCancelLatency, AStopCutsAHugeMeshsFrameShortWithinTheBound)
{
    if (!ready()) {
        return;
    }
    // Built once for the three backends; never destroyed, as a stage outliving the plugins' teardown
    // would release them in the wrong order.
    static const UsdStageRefPtr& stage = *new UsdStageRefPtr(hugeMeshStage(GfVec3f(1.f, 0.f, 0.f)));

    checkCancelLatency(stage, "one huge mesh", GetParam());
}

TEST_P(NatronUtilityCancelLatency, AStopCutsTheFrameOfManyMeshesShortWithinTheBound)
{
    if (!ready()) {
        return;
    }
    static const UsdStageRefPtr& stage = *new UsdStageRefPtr(manyMeshesStage(GfVec3f(1.f, 0.f, 0.f)));

    checkCancelLatency(stage, "many meshes", GetParam());
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityCancelLatency, ::testing::Values(Backend::Cpu, Backend::GpuTiles, Backend::GpuFilm));

namespace {
class CountingFilm : public WavefrontFilm {
public:
    CountingFilm(int size,
                 bool colorOnDevice)
        : _color(colorOnDevice ? new UtilityRenderBuffer(SdfPath("/color")) : nullptr)
    {
        if (_color) {
            EXPECT_TRUE(_color->Allocate(GfVec3i(size, size, 1), HdFormatFloat32Vec4, false));
            _color->clear(VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f)));
        }
    }

    virtual void writeTile(const WavefrontTile&) OVERRIDE
    {
        ++tiles;
    }

    virtual UtilityRenderBuffer* deviceColor() const OVERRIDE
    {
        return _color.get();
    }

    std::atomic<int> tiles { 0 };

private:
    std::unique_ptr<UtilityRenderBuffer> _color;
};

// One card lit by lightCount lights, which add up to a white light.
UtilityScene::Snapshot
litCard(UtilityScene& scene,
        int lightCount)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/Card"), 1);

    scene.setTopology(id,
                      VtVec3fArray { GfVec3f(-50.f, -50.f, 0.f), GfVec3f(50.f, -50.f, 0.f), GfVec3f(50.f, 50.f, 0.f), GfVec3f(-50.f, 50.f, 0.f) },
                      VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    for (int i = 0; i < lightCount; ++i) {
        std::shared_ptr<UtilityLightDesc> light = std::make_shared<UtilityLightDesc>();

        light->emission = GfVec3f(1.f / lightCount);
        scene.setLight(SdfPath("/Light" + std::to_string(i)), light);
    }

    return scene.snapshot();
}

WavefrontFrame
cardFrame(int size)
{
    GfFrustum camera;

    camera.SetOrthographic(-10., 10., -10., 10., 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., kCameraZ));

    WavefrontFrame frame;

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();
    frame.window.width = size;
    frame.window.height = size;
    frame.sampling.samplesPerSide = 1;
    frame.maxRayDepth = 0;

    return frame;
}

// An executor of the backend's own, made on a device of the test's for the GPU backends.
class NatronUtilityCancelTile : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() OVERRIDE
    {
        if (GetParam() == Backend::Cpu) {
            _cpu.reset(new WavefrontCpu());
            _executor = _cpu.get();

            return;
        }
#ifdef NATRON_HAVE_GPU
        const gpu::GpuStatus s = gputest::createDevice(_device);

        GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);

        const std::string missing = GpuTracer::missingRayQuerySupport(*_device);

        if (!missing.empty()) {
            GPUTEST_SKIP() << missing;
        }
        _registry.reset(new gpu::GpuKernelRegistry(*_device));
        _pool.reset(new gpu::GpuBufferPool(*_device));
        _gpu.reset(new WavefrontGpu(*_registry, *_pool, std::make_shared<int>(0)));
        _executor = _gpu.get();
#else
        std::printf("[  SKIPPED ] Natron was built without the Vulkan backend, so there is no GPU executor\n");
#endif
    }

    void TearDown() OVERRIDE
    {
        _executor = nullptr;
#ifdef NATRON_HAVE_GPU
        _gpu.reset();
        if (_device) {
            EXPECT_TRUE(WavefrontGpu::waitForWarmUps(*_device, std::chrono::milliseconds(60000)));
            _device->waitIdle();
        }
        _pool.reset();
        _registry.reset();
        _device.reset();
        _check.expectClean("the test");
#endif
    }

    WavefrontExecutor* _executor = nullptr;

private:
    std::unique_ptr<WavefrontCpu> _cpu;
#ifdef NATRON_HAVE_GPU
    gputest::ValidationCheck _check;
    std::unique_ptr<gpu::GpuDevice> _device;
    std::unique_ptr<gpu::GpuKernelRegistry> _registry;
    std::unique_ptr<gpu::GpuBufferPool> _pool;
    std::unique_ptr<WavefrontGpu> _gpu;
#endif
};
} // anon namespace

// One tile lit by so many lights that shading it alone takes far longer than the bound: a stop ends it
// between batches of lights, and the tile never reaches the film.
TEST_P(NatronUtilityCancelTile, AStopCutsATileShortBetweenItsLights)
{
    if (!_executor) {
        return;
    }
    const int lightCount = 8192;
    const bool onGpu = GetParam() != Backend::Cpu;
    // A device shades many pixels at once, so it gets more of them to keep it busy past the stop.
    const int size = onGpu ? 256 : 64;
    const std::atomic<bool> unstopped(false);

    if (onGpu) {
        // The first frame compiles the kernels, which is no stop point of the tile's.
        UtilityScene warmScene;
        CountingFilm warmFilm(8, GetParam() == Backend::GpuFilm);

        ASSERT_TRUE(_executor->render(litCard(warmScene, 1), cardFrame(8), warmFilm, unstopped, 1));
    }

    UtilityScene scene;
    const UtilityScene::Snapshot snapshot = litCard(scene, lightCount);
    CountingFilm film(size, GetParam() == Backend::GpuFilm);
    std::atomic<bool> stop(false);
    Clock::time_point stopped;
    std::thread stopper([&stop, &stopped]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stopped = Clock::now();
        stop = true;
    });
    const bool finished = _executor->render(snapshot, cardFrame(size), film, stop, 1);
    const Clock::time_point returned = Clock::now();

    stopper.join();
    if (finished && onGpu) {
        GPUTEST_SKIP() << "the device shaded " << lightCount << " lights over the frame before the stop came";
    }

    const std::chrono::milliseconds took = std::chrono::duration_cast<std::chrono::milliseconds>(returned - stopped);

    std::cout << "[   NOTE   ] a stop while shading " << lightCount << " lights took " << took.count() << " ms" << std::endl;
    EXPECT_FALSE(finished);
    EXPECT_LT(took.count(), kCancelBound.count());
    EXPECT_EQ(0, film.tiles.load());
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityCancelTile, ::testing::Values(Backend::Cpu, Backend::GpuTiles, Backend::GpuFilm));

TEST_P(NatronUtilityCancelTest, AnEditDuringAFrameRestartsItAndTheOldFrameNeverLands)
{
    if (!ready()) {
        return;
    }
    const ScenePayload red = payloadOf(hugeMeshLayer(GfVec3f(1.f, 0.f, 0.f)), 0x4e0c01ULL);

    start(red);
    waitForFrameUnderWay();
    ASSERT_FALSE(_render->getFrame()) << "the scene is too light to test this";

    const std::uint64_t editedRestart = sync(recoloured(red, "/Huge", GfVec3f(0.f, 1.f, 0.f), 0x4e0c02ULL));

    ASSERT_GT(editedRestart, 0u) << "the recolour did not edit the stage";

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;

    ASSERT_TRUE(collectUntilFinished(editedRestart, &frames));
    ASSERT_FALSE(frames.empty());
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : frames) {
        const GfVec4f centre = centreOf(*frame);

        EXPECT_GT(centre[1], centre[0]) << "frame " << frame->serial << " of render " << frame->restart << " shows the colour from before the edit";
        EXPECT_TRUE(frame->converged);
    }
}

TEST_P(NatronUtilityCancelTest, StopDuringAFramePublishesNothingAndResumeFinishesIt)
{
    if (!ready()) {
        return;
    }
    start(payloadOf(hugeMeshLayer(GfVec3f(1.f, 0.f, 0.f)), 0x4e0c03ULL));
    waitForFrameUnderWay();
    ASSERT_FALSE(_render->getFrame()) << "the scene is too light to test this";

    _render->stop();

    const Clock::time_point end = Clock::now() + kTimeout;

    while (!_render->getStatus().stopped && Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(_render->getStatus().stopped);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(_render->getFrame());
    EXPECT_FALSE(_render->getStatus().converged);

    _render->resume();

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;

    ASSERT_TRUE(collectUntilFinished(0, &frames));
    ASSERT_FALSE(frames.empty());
    EXPECT_TRUE(frames.back()->converged);
}

TEST_P(NatronUtilityCancelTest, TheViewsPurposesDecideWhatIsDrawn)
{
    if (!ready()) {
        return;
    }
    const SdfLayerRefPtr layer = SceneOps::createNodeLayer("NatronUtilityCancelPurposes");
    const UsdStageRefPtr authoring = UsdStage::Open(layer);

    defineGrid(authoring, "/Default", 1, GfVec3f(1.f, 0.f, 0.f));
    defineGrid(authoring, "/RenderOnly", 1, GfVec3f(0.f, 1.f, 0.f), GfVec3f(0.f, 0.f, 1.f));
    UsdGeomMesh(authoring->GetPrimAtPath(SdfPath("/RenderOnly"))).CreatePurposeAttr(VtValue(UsdGeomTokens->render));

    start(payloadOf(layer, 0x4e0c04ULL));

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;

    ASSERT_TRUE(collectUntilFinished(0, &frames));
    GfVec4f centre = centreOf(*frames.back());
    EXPECT_GT(centre[0], centre[1]) << "the render-purpose prim shows with the purpose off";

    std::uint64_t restart = _render->getRestartCount() + 1;

    _render->setView(viewOf(true));
    ASSERT_TRUE(collectUntilFinished(restart, &frames));
    centre = centreOf(*frames.back());
    EXPECT_GT(centre[1], centre[0]) << "the render-purpose prim is missing with the purpose on";

    restart = _render->getRestartCount() + 1;
    _render->setView(viewOf(false));
    ASSERT_TRUE(collectUntilFinished(restart, &frames));
    centre = centreOf(*frames.back());
    EXPECT_GT(centre[0], centre[1]) << "the render-purpose prim stays with the purpose off again";
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityCancelTest, ::testing::Values(Backend::Cpu, Backend::GpuTiles));
