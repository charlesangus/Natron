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
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"
#include "Engine/Nodes/Scene/Utility/WavefrontCpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Gpu/GpuTestDevice.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

// One world unit is one pixel: pixel p spans x from p - 8 to p - 7.
const int kSize = 16;
const double kHalfSize = 8.;
const double kCameraZ = 30.;

GfFrustum
orthographicCamera()
{
    GfFrustum camera;

    camera.SetOrthographic(-kHalfSize, kHalfSize, -kHalfSize, kHalfSize, 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., kCameraZ));

    return camera;
}

double
pixelCentre(int x)
{
    return x + 0.5 - kHalfSize;
}

// The filters as their definitions give them, written apart from the kernel's, over t in [0, 1].
double
filterAt(KernelPixelFilter filter,
         double t)
{
    const double x = 2. * t - 1.;

    switch (filter) {
    case eKernelPixelFilterTriangle:
        return std::max(0., 1. - std::abs(x));
    case eKernelPixelFilterGaussian:
        return std::exp(-4.5 * x * x);
    case eKernelPixelFilterBlackmanHarris:
        return std::max(0., 0.35875 - 0.48829 * std::cos(2. * M_PI * t) + 0.14128 * std::cos(4. * M_PI * t) - 0.01168 * std::cos(6. * M_PI * t));
    default:
        return 1.;
    }
}

double
filterIntegral(KernelPixelFilter filter,
               double from,
               double to)
{
    from = std::max(from, 0.);
    to = std::min(to, 1.);
    if (to <= from) {
        return 0.;
    }

    const int steps = 4000;
    double sum = 0.;

    for (int i = 0; i < steps; ++i) {
        sum += filterAt(filter, from + (to - from) * (i + 0.5) / steps);
    }

    return sum * (to - from) / steps;
}

// What a pixel centred at x sees of a stripe covering [from, to) along x and all of y, through a
// separable filter of the given width: the y factors cancel.
double
stripeResponse(KernelPixelFilter filter,
               double width,
               double centre,
               double from,
               double to)
{
    return filterIntegral(filter, (from - centre) / width + 0.5, (to - centre) / width + 0.5) / filterIntegral(filter, 0., 1.);
}

void
addStripe(UtilityScene& scene,
          double from,
          double to)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/stripe"), 1);
    const float a = static_cast<float>(from);
    const float b = static_cast<float>(to);

    scene.setTopology(id, VtVec3fArray { GfVec3f(a, -50.f, 0.f), GfVec3f(b, -50.f, 0.f), GfVec3f(b, 50.f, 0.f), GfVec3f(a, 50.f, 0.f) }, VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
}

WavefrontFrame
makeFrame(int samplesPerSide,
          KernelPixelFilter filter,
          float filterWidth)
{
    const GfFrustum camera = orthographicCamera();
    WavefrontFrame frame;

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();
    frame.window.width = kSize;
    frame.window.height = kSize;
    frame.sampling.samplesPerSide = samplesPerSide;
    frame.sampling.filter = filter;
    frame.sampling.filterWidth = filterWidth;
    frame.sampling.seed = 7;

    return frame;
}

// The render pass's film: colour filtered, prim ids from each pixel's heaviest sample.
class SampleFilm : public WavefrontFilm {
public:
    explicit SampleFilm(bool colorOnDevice)
        : _color(new UtilityRenderBuffer(SdfPath("/color")))
        , _primId(new UtilityRenderBuffer(SdfPath("/primId")))
        , _colorOnDevice(colorOnDevice)
    {
        EXPECT_TRUE(_color->Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false));
        EXPECT_TRUE(_primId->Allocate(GfVec3i(kSize, kSize, 1), HdFormatInt32, false));
        _color->clear(VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f)));
        _primId->clear(VtValue(-1));
    }

    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;
                const float weight = tile.weight ? tile.weight[index] : 1.f;
                const unsigned int x = static_cast<unsigned int>(tile.rect.x + column);
                const unsigned int y = static_cast<unsigned int>(tile.rect.y + row);

                if (tile.color) {
                    _color->addSample(x, y, hit ? tile.color + index * 4 : nullptr, 4, weight, true);
                }
                _primId->addSample(x, y, hit ? tile.primId + index : nullptr, weight);
            }
        }
    }

    virtual UtilityRenderBuffer* deviceColor() const OVERRIDE
    {
        return _colorOnDevice ? _color.get() : nullptr;
    }

    std::vector<GfVec4f> colors() const
    {
        const GfVec4f* pixels = static_cast<const GfVec4f*>(_color->Map());
        std::vector<GfVec4f> out;

        if (pixels) {
            out.assign(pixels, pixels + kSize * kSize);
        }
        _color->Unmap();
        EXPECT_EQ(static_cast<std::size_t>(kSize * kSize), out.size());

        return out;
    }

    int primIdAt(int x,
                 int y) const
    {
        const std::int32_t* pixels = static_cast<const std::int32_t*>(_primId->Map());
        const int value = pixels ? pixels[y * kSize + x] : -2;

        _primId->Unmap();

        return value;
    }

private:
    std::unique_ptr<UtilityRenderBuffer> _color;
    std::unique_ptr<UtilityRenderBuffer> _primId;
    bool _colorOnDevice;
};

enum class Backend {
    Cpu,
    // The GPU handing its colour back in tiles, as for a film with several colour AOVs.
    GpuTiles,
    // The GPU keeping the colour on the device, as for the viewer.
    GpuFilm,
};

void
PrintTo(Backend backend,
        std::ostream* os)
{
    *os << (backend == Backend::Cpu ? "Cpu" : (backend == Backend::GpuTiles ? "GpuTiles" : "GpuFilm"));
}

class NatronUtilitySampling : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() OVERRIDE
    {
        _cpu.reset(new WavefrontCpu());
        if (GetParam() == Backend::Cpu) {
            _available = true;

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
        _gpu.reset(new WavefrontGpu(*_registry, *_pool));
        _available = true;
#else
        std::printf("[  SKIPPED ] Natron was built without the Vulkan backend, so there is no GPU executor\n");
#endif
    }

    void TearDown() OVERRIDE
    {
#ifdef NATRON_HAVE_GPU
        _gpu.reset();
        if (_device) {
            _device->waitIdle();
        }
        _pool.reset();
        _registry.reset();
        _device.reset();
        _check.expectClean("the test");
#endif
    }

    // Draws samples first to last - 1 of the frame into the film, one per pixel per call, as the
    // render pass does. The film holds only what it was given since it was made.
    bool draw(const UtilityScene& scene,
              WavefrontFrame frame,
              SampleFilm& film,
              int first,
              int last,
              bool onCpu = false)
    {
        WavefrontExecutor* executor = executorFor(onCpu);
        const UtilityScene::Snapshot snapshot = scene.snapshot();
        const std::atomic<bool> stop(false);

        if (!executor) {
            return false;
        }
        for (int sample = first; sample < last; ++sample) {
            frame.sampling.sampleIndex = sample;
            if (!executor->render(snapshot, frame, film, stop, 2)) {
                ADD_FAILURE() << "sample " << sample << " did not render";

                return false;
            }
        }

        return true;
    }

    bool drawAll(const UtilityScene& scene,
                 const WavefrontFrame& frame,
                 SampleFilm& film,
                 bool onCpu = false)
    {
        return draw(scene, frame, film, 0, frame.sampling.samplesPerPixel(), onCpu);
    }

    bool colorOnDevice() const
    {
        return GetParam() == Backend::GpuFilm;
    }

    bool available() const
    {
        return _available;
    }

private:
    WavefrontExecutor* executorFor(bool onCpu)
    {
        if (!_available) {
            return nullptr;
        }
        if (onCpu || GetParam() == Backend::Cpu) {
            return _cpu.get();
        }
#ifdef NATRON_HAVE_GPU
        return _gpu.get();
#else
        return nullptr;
#endif
    }

    bool _available = false;
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

TEST_P(NatronUtilitySampling, AHalfPlaneEdgeCoversEachPixelToWithinOneSample)
{
    if (!available()) {
        return;
    }

    const double edge = 0.3;
    UtilityScene scene;

    addStripe(scene, edge, 50.);
    for (int side : { 2, 3, 4, 7 }) {
        SampleFilm film(colorOnDevice());

        ASSERT_TRUE(drawAll(scene, makeFrame(side, eKernelPixelFilterBox, 1.f), film));

        const std::vector<GfVec4f> colors = film.colors();
        const double bound = 1. / (side * side) + 1e-5;

        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const double coverage = std::clamp(pixelCentre(x) + 0.5 - edge, 0., 1.);

                EXPECT_NEAR(coverage, colors[y * kSize + x][3], bound) << side << "x" << side << " samples, pixel " << x << ", " << y;
            }
        }
    }
}

TEST_P(NatronUtilitySampling, EachFilterReconstructsAOnePixelFeatureToItsResponse)
{
    if (!available()) {
        return;
    }

    UtilityScene scene;

    addStripe(scene, 0., 1.);
    for (KernelPixelFilter filter : { eKernelPixelFilterBox, eKernelPixelFilterTriangle, eKernelPixelFilterGaussian, eKernelPixelFilterBlackmanHarris }) {
        for (float width : { 1.f, 1.5f, 2.f }) {
            SampleFilm film(colorOnDevice());

            ASSERT_TRUE(drawAll(scene, makeFrame(8, filter, width), film));

            const std::vector<GfVec4f> colors = film.colors();

            for (int y = 0; y < kSize; y += 5) {
                for (int x = 5; x < 11; ++x) {
                    const double expected = stripeResponse(filter, width, pixelCentre(x), 0., 1.);

                    EXPECT_NEAR(expected, colors[y * kSize + x][3], 0.03) << "filter " << filter << ", width " << width << ", pixel " << x << ", " << y;
                    EXPECT_NEAR(colors[y * kSize + x][3], colors[y * kSize + x][0], 1e-5) << "a white feature's colour is its coverage";
                }
            }
        }
    }
}

TEST_P(NatronUtilitySampling, TheIdsComeFromTheHeaviestSampleNotAnAverage)
{
    if (!available()) {
        return;
    }

    UtilityScene scene;

    addStripe(scene, 0., 1.);

    SampleFilm film(colorOnDevice());

    ASSERT_TRUE(drawAll(scene, makeFrame(4, eKernelPixelFilterBlackmanHarris, 2.f), film));
    for (int y = 0; y < kSize; y += 5) {
        EXPECT_EQ(1, film.primIdAt(8, y));
        EXPECT_EQ(-1, film.primIdAt(4, y));
        EXPECT_EQ(-1, film.primIdAt(12, y));
    }
}

TEST_P(NatronUtilitySampling, OneSampleAPixelIsThePixelCentreUnfiltered)
{
    if (!available()) {
        return;
    }

    UtilityScene scene;

    addStripe(scene, 0.4, 50.);

    SampleFilm film(colorOnDevice());

    ASSERT_TRUE(drawAll(scene, makeFrame(1, eKernelPixelFilterBlackmanHarris, 3.f), film));

    const std::vector<GfVec4f> colors = film.colors();

    for (int x = 0; x < kSize; ++x) {
        EXPECT_EQ(pixelCentre(x) > 0.4 ? 1.f : 0.f, colors[3 * kSize + x][3]) << "pixel " << x;
    }
}

TEST_P(NatronUtilitySampling, KSamplesAreTheMeanOfKSingleSampleRenders)
{
    if (!available()) {
        return;
    }

    UtilityScene scene;

    addStripe(scene, -0.35, 0.8);

    const WavefrontFrame frame = makeFrame(3, eKernelPixelFilterBox, 1.5f);
    const int k = 4;
    SampleFilm progressive(colorOnDevice());

    ASSERT_TRUE(draw(scene, frame, progressive, 0, k));

    std::vector<GfVec4f> mean(kSize * kSize, GfVec4f(0.f));

    for (int sample = 0; sample < k; ++sample) {
        SampleFilm single(colorOnDevice());

        ASSERT_TRUE(draw(scene, frame, single, sample, sample + 1));

        const std::vector<GfVec4f> colors = single.colors();

        for (std::size_t i = 0; i < mean.size(); ++i) {
            mean[i] += colors[i] / static_cast<float>(k);
        }
    }

    const std::vector<GfVec4f> colors = progressive.colors();
    bool fractional = false;

    for (std::size_t i = 0; i < mean.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(mean[i][c], colors[i][c], 1e-5) << "pixel " << i % kSize << ", " << i / kSize;
        }
        fractional = fractional || ((colors[i][3] > 0.01f) && (colors[i][3] < 0.99f));
    }
    EXPECT_TRUE(fractional) << "the stripe's edges should fall part way through pixels";
}

TEST_P(NatronUtilitySampling, TheGpuMatchesTheCpu)
{
    if (!available() || GetParam() == Backend::Cpu) {
        return;
    }

    UtilityScene scene;

    addStripe(scene, -0.35, 0.8);
    for (KernelPixelFilter filter : { eKernelPixelFilterBox, eKernelPixelFilterBlackmanHarris }) {
        const WavefrontFrame frame = makeFrame(4, filter, 1.5f);
        SampleFilm cpu(false);
        SampleFilm gpu(colorOnDevice());

        ASSERT_TRUE(drawAll(scene, frame, cpu, true));
        ASSERT_TRUE(drawAll(scene, frame, gpu));

        const std::vector<GfVec4f> expected = cpu.colors();
        const std::vector<GfVec4f> actual = gpu.colors();

        for (std::size_t i = 0; i < expected.size(); ++i) {
            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(expected[i][c], actual[i][c], 1e-4) << "filter " << filter << ", pixel " << i % kSize << ", " << i / kSize;
            }
        }
    }
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilitySampling, ::testing::Values(Backend::Cpu, Backend::GpuTiles, Backend::GpuFilm));

namespace {

std::string
stripeStage(double from,
            double to)
{
    const std::string a = std::to_string(from);
    const std::string b = std::to_string(to);

    return "#usda 1.0\n"
           "def Mesh \"Stripe\" {\n"
           "    int[] faceVertexCounts = [4]\n"
           "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
           "    point3f[] points = [("
        + a + ", -50, 0), (" + b + ", -50, 0), (" + b + ", 50, 0), (" + a + ", 50, 0)]\n"
                                                                            "    color3f[] primvars:displayColor = [(1, 1, 1)] (interpolation = \"constant\")\n"
                                                                            "}\n";
}

// Renders through the engine the node drives the delegate with, on the CPU.
class DelegateRender {
public:
    explicit DelegateRender(const std::string& usda)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(usda));
        stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color }));
        setting(kUtilitySettingDevice, VtValue(TfToken(kUtilityDeviceCpu)));
    }

    void setting(const char* key,
                 const VtValue& value)
    {
        _engine->SetRendererSetting(TfToken(key), value);
    }

    void renderOnce()
    {
        const GfFrustum camera = orthographicCamera();

        _engine->SetRenderBufferSize(GfVec2i(kSize, kSize));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kSize, kSize)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(stage->GetPseudoRoot(), params);
    }

    // Renders until the frame converges, checking along the way that it converges only once it
    // drew every sample it wants, and that the count of them only grows.
    void renderToConvergence()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::size_t last = 0;

        renderOnce();
        while (std::chrono::steady_clock::now() < end) {
            const bool converged = _engine->IsConverged();
            const std::size_t samples = stat(kUtilityStatSamples);
            const std::size_t wanted = stat(kUtilityStatSamplesWanted);

            EXPECT_LE(samples, wanted);
            EXPECT_GE(samples, last);
            last = samples;
            if (converged) {
                EXPECT_EQ(wanted, samples) << "the frame converged before it drew all its samples";

                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderOnce();
        }
        ADD_FAILURE() << "the frame did not converge";
    }

    bool isConverged() const
    {
        return _engine->IsConverged();
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

    std::vector<GfVec4f> colors() const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);
        std::vector<GfVec4f> out;

        if (!buffer) {
            ADD_FAILURE() << "no colour buffer";

            return out;
        }

        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());

        out.assign(pixels, pixels + kSize * kSize);
        buffer->Unmap();

        return out;
    }

    void setStripe(double from,
                   double to)
    {
        const UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath("/Stripe")));
        const float a = static_cast<float>(from);
        const float b = static_cast<float>(to);

        ASSERT_TRUE(mesh.GetPointsAttr().Set(VtVec3fArray { GfVec3f(a, -50.f, 0.f), GfVec3f(b, -50.f, 0.f), GfVec3f(b, 50.f, 0.f), GfVec3f(a, 50.f, 0.f) }));
    }

    UsdStageRefPtr stage;

private:
    SdfLayerRefPtr _layer;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

void
expectSameColors(const std::vector<GfVec4f>& expected,
                 const std::vector<GfVec4f>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(expected[i][c], actual[i][c], 1e-6) << "pixel " << i % kSize << ", " << i / kSize;
        }
    }
}

} // anon namespace

TEST(NatronUtilitySamplingDelegate, ConvergesAtTheTargetSampleCount)
{
    DelegateRender render(stripeStage(-0.35, 0.8));

    render.setting(kUtilitySettingPixelSamples, VtValue(int(6)));
    render.renderToConvergence();
    EXPECT_EQ(36u, render.stat(kUtilityStatSamplesWanted));
    EXPECT_EQ(36u, render.stat(kUtilityStatSamples));
}

TEST(NatronUtilitySamplingDelegate, DefaultsToThreeByThreeBlackmanHarrisSamples)
{
    DelegateRender render(stripeStage(-0.35, 0.8));

    render.renderToConvergence();
    EXPECT_EQ(9u, render.stat(kUtilityStatSamplesWanted));

    const std::vector<GfVec4f> colors = render.colors();

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());
    EXPECT_NEAR(stripeResponse(eKernelPixelFilterBlackmanHarris, kUtilityDefaultFilterWidth, pixelCentre(7), -0.35, 0.8), colors[4 * kSize + 7][3], 0.1);
    EXPECT_GT(colors[4 * kSize + 7][3], 0.05f);
    EXPECT_LT(colors[4 * kSize + 7][3], 0.95f);
}

TEST(NatronUtilitySamplingDelegate, AnEditPartWayRestartsTheSamples)
{
    DelegateRender render(stripeStage(-0.35, 0.8));

    render.setting(kUtilitySettingPixelSamples, VtValue(int(16)));
    render.renderOnce();
    render.setStripe(-2.2, 1.6);
    render.renderToConvergence();

    DelegateRender fresh(stripeStage(-2.2, 1.6));

    fresh.setting(kUtilitySettingPixelSamples, VtValue(int(16)));
    fresh.renderToConvergence();
    expectSameColors(fresh.colors(), render.colors());
}

TEST(NatronUtilitySamplingDelegate, ASettingChangeRestartsTheSamples)
{
    DelegateRender render(stripeStage(-0.35, 0.8));

    render.setting(kUtilitySettingPixelSamples, VtValue(int(4)));
    render.setting(kUtilitySettingPixelFilter, VtValue(TfToken(kUtilityFilterBox)));
    render.renderToConvergence();

    const std::vector<GfVec4f> box = render.colors();

    render.setting(kUtilitySettingPixelFilter, VtValue(TfToken(kUtilityFilterGaussian)));
    render.setting(kUtilitySettingFilterWidth, VtValue(2.f));
    render.renderToConvergence();

    DelegateRender fresh(stripeStage(-0.35, 0.8));

    fresh.setting(kUtilitySettingPixelSamples, VtValue(int(4)));
    fresh.setting(kUtilitySettingPixelFilter, VtValue(TfToken(kUtilityFilterGaussian)));
    fresh.setting(kUtilitySettingFilterWidth, VtValue(2.f));
    fresh.renderToConvergence();
    expectSameColors(fresh.colors(), render.colors());

    const std::vector<GfVec4f> gaussian = render.colors();
    bool differs = false;

    for (std::size_t i = 0; i < box.size(); ++i) {
        differs = differs || (std::abs(box[i][3] - gaussian[i][3]) > 1e-3f);
    }
    EXPECT_TRUE(differs) << "the Gaussian frame should differ from the box one";
}
