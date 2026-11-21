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
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2d.h>
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
#include <pxr/usd/usd/stage.h>
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
#include <cstdlib>
#include <functional>
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
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"
#include "Engine/Nodes/Scene/Utility/WavefrontCpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Gpu/GpuTestDevice.h"
#endif

#ifdef NATRON_HAVE_GPU
#include "Engine/AppManager.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#include "Gpu/GpuTestSelector.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

// One world unit is one pixel: pixel p spans x from p - 8 to p - 7.
const int kSize = 16;
const double kHalfSize = 8.;
const double kCameraZ = 30.;
const std::uint64_t kEvaluatedBit = std::uint64_t(1) << 63;

GfFrustum
orthographicCamera()
{
    GfFrustum camera;

    camera.SetOrthographic(-kHalfSize, kHalfSize, -kHalfSize, kHalfSize, 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., kCameraZ));

    return camera;
}

GfMatrix4d
translation(double x)
{
    return GfMatrix4d(1.).SetTranslate(GfVec3d(x, 0., 0.));
}

// A stripe one pixel wide, covering [from, from + 1) along x and the frame along y.
UtilityScene::GeometryId
addStripe(UtilityScene& scene,
          double from)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/stripe"), 1);
    const float a = static_cast<float>(from);
    const float b = static_cast<float>(from + 1.);

    scene.setTopology(id, VtVec3fArray { GfVec3f(a, -50.f, 0.f), GfVec3f(b, -50.f, 0.f), GfVec3f(b, 50.f, 0.f), GfVec3f(a, 50.f, 0.f) }, VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });

    return id;
}

// Moves the stripe from x0 at time t0 to x1 at time t1, by its transform.
void
moveStripe(UtilityScene& scene,
           UtilityScene::GeometryId id,
           double t0,
           double x0,
           double t1,
           double x1)
{
    std::shared_ptr<UtilityScene::TransformMotion> motion = std::make_shared<UtilityScene::TransformMotion>();

    motion->times = { t0, t1 };
    motion->transforms = { { translation(x0) }, { translation(x1) } };
    scene.setTransformMotion(id, motion);
}

WavefrontFrame
makeFrame(int samplesPerSide)
{
    const GfFrustum camera = orthographicCamera();
    WavefrontFrame frame;

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();
    frame.window.width = kSize;
    frame.window.height = kSize;
    frame.sampling.samplesPerSide = samplesPerSide;
    frame.sampling.filter = eKernelPixelFilterBox;
    frame.sampling.filterWidth = 1.f;
    frame.sampling.seed = 7;
    frame.maxRayDepth = 0;

    return frame;
}

WavefrontMotion
shutter(double open,
        double close,
        int segments)
{
    WavefrontMotion motion;

    motion.open = open;
    motion.close = close;
    motion.segments = segments;

    return motion;
}

// How much of the pixel [p - 8, p - 7) a one-pixel stripe at [x, x + 1) covers, averaged over x
// moving uniformly from x0 to x1 while the shutter is open: what a box filter of the pixel's width sees.
double
smearedCoverage(int pixel,
                double x0,
                double x1)
{
    const double left = pixel - kHalfSize;
    const int steps = 20000;
    double sum = 0.;

    for (int i = 0; i < steps; ++i) {
        const double x = x0 + (x1 - x0) * (i + 0.5) / steps;

        sum += std::max(0., std::min(left + 1., x + 1.) - std::max(left, x));
    }

    return sum / steps;
}

// The render pass's film, colour only.
class ColorFilm : public WavefrontFilm {
public:
    explicit ColorFilm(bool colorOnDevice)
        : _color(new UtilityRenderBuffer(SdfPath("/color")))
        , _colorOnDevice(colorOnDevice)
    {
        EXPECT_TRUE(_color->Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false));
        _color->clear(VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f)));
    }

    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;
                const float weight = tile.weight ? tile.weight[index] : 1.f;

                if (tile.color) {
                    _color->addSample(static_cast<unsigned int>(tile.rect.x + column), static_cast<unsigned int>(tile.rect.y + row), hit ? tile.color + index * 4 : nullptr, 4, weight, true);
                }
            }
        }
    }

    virtual UtilityRenderBuffer* deviceColor() const OVERRIDE
    {
        return _colorOnDevice ? _color.get() : nullptr;
    }

    virtual void passDrawn() OVERRIDE
    {
        _color->publishPass();
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

private:
    std::unique_ptr<UtilityRenderBuffer> _color;
    bool _colorOnDevice;
};

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

class NatronUtilityMotion : public ::testing::TestWithParam<Backend> {
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
        // A device film without a runtime hold counts as released, so the colour could not stay on the GPU.
        _gpu.reset(new WavefrontGpu(*_registry, *_pool, std::make_shared<int>(0)));
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
            EXPECT_TRUE(WavefrontGpu::waitForWarmUps(*_device, std::chrono::milliseconds(60000)));
            _device->waitIdle();
        }
        _pool.reset();
        _registry.reset();
        _device.reset();
        _check.expectClean("the test");
#endif
    }

    // Draws the whole frame as the render pass does.
    std::vector<GfVec4f> draw(const UtilityScene& scene,
                              const WavefrontFrame& frame,
                              const WavefrontMotion& motion)
    {
        ColorFilm film(GetParam() == Backend::GpuFilm);
        const std::atomic<bool> stop(false);
        WavefrontExecutor* executor = executorFor();

        if (!executor) {
            ADD_FAILURE() << "no executor";

            return std::vector<GfVec4f>();
        }
        EXPECT_TRUE(wavefrontDrawSamples(*executor, scene.snapshot(), frame, motion, film, stop, 2, std::function<void(std::size_t)>()));

        return film.colors();
    }

    bool available() const
    {
        return _available;
    }

private:
    WavefrontExecutor* executorFor()
    {
        if (!_available) {
            return nullptr;
        }
        if (GetParam() == Backend::Cpu) {
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

void
expectSameColors(const std::vector<GfVec4f>& expected,
                 const std::vector<GfVec4f>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(expected[i][c], actual[i][c]) << "pixel " << i % kSize << ", " << i / kSize;
        }
    }
}

// Renders a stage with the delegate on the CPU at a frame, through the engine the node drives it
// with. Settings that shape what the prims sync, the shutter among them, go in before the first render.
class MotionRender {
public:
    MotionRender(const std::string& usda,
                 const GfVec2d& shutterInterval,
                 int pixelSamples,
                 int segments,
                 int subdivLevel = 0,
                 const char* device = kUtilityDeviceCpu)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(usda));
        _stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color }));
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(device)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingPixelSamples), VtValue(pixelSamples));
        _engine->SetRendererSetting(TfToken(kUtilitySettingSubdivLevel), VtValue(subdivLevel));
        _engine->SetRendererSetting(TfToken(kUtilitySettingMaxRayDepth), VtValue(0));
        _engine->SetRendererSetting(TfToken(kUtilitySettingPixelFilter), VtValue(TfToken(kUtilityFilterBox)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingFilterWidth), VtValue(1.f));
        _engine->SetRendererSetting(TfToken(kUtilitySettingShutterSegments), VtValue(segments));
        _engine->SetRendererSetting(TfToken(kRenderDelegateUtilityShutterSetting), VtValue(shutterInterval));
    }

    void render(double frame)
    {
        // The first GPU frame compiles the kernels, which takes far longer than a frame.
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(180);

        renderOnce(frame);
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderOnce(frame);
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    float alphaAt(int x) const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no colour buffer";

            return -1.f;
        }
        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());
        const float alpha = pixels[(kSize / 2) * kSize + x][3];

        buffer->Unmap();

        return alpha;
    }

    VtDictionary stats() const
    {
        return _engine->GetRenderStats();
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

    std::string device() const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), kUtilityStatDevice, VtDefault = std::string());
    }

    // How many pixels of the middle row are partly covered.
    int partialInMiddleRow() const
    {
        int partial = 0;

        for (int x = 0; x < kSize; ++x) {
            const float alpha = alphaAt(x);

            partial += (alpha > 0.05f && alpha < 0.95f) ? 1 : 0;
        }

        return partial;
    }

private:
    void renderOnce(double frame)
    {
        const GfFrustum camera = orthographicCamera();

        _engine->SetRenderBufferSize(GfVec2i(kSize, kSize));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kSize, kSize)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.frame = UsdTimeCode(frame);
        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(_stage->GetPseudoRoot(), params);
    }

    SdfLayerRefPtr _layer;
    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

const char* const kStripeTopology = "    int[] faceVertexCounts = [4]\n"
                                    "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                    "    color3f[] primvars:displayColor = [(1, 1, 1)] (interpolation = \"constant\")\n";
const char* const kStripePoints = "[(0, -50, 0), (1, -50, 0), (1, 50, 0), (0, 50, 0)]";

std::string
stage(const std::string& stripeBody)
{
    return std::string("#usda 1.0\n(\n    timeCodesPerSecond = 24\n)\n")
        + "def Mesh \"Stripe\" {\n" + kStripeTopology + stripeBody + "}\n";
}

// Pixel 8 holds the stripe at rest; pixel 10 holds it 2 units along x.
const int kRestPixel = 8;
const int kMovedPixel = 10;

} // anon namespace

TEST(NatronUtilityMotionScene, AShutterHoldsATranslationsEndPoints)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addStripe(scene, 0.);

    moveStripe(scene, id, -0.25, -1., 0.25, 3.);

    const UtilityScene::Snapshot snapshot = scene.snapshot();

    ASSERT_TRUE(UtilityScene::moves(snapshot));
    ASSERT_EQ(1u, snapshot.instances.size());

    const UtilityScene::Snapshot one = UtilityScene::atShutter(snapshot, 0., 0.25, 1);
    const UtilityScene::Snapshot two = UtilityScene::atShutter(snapshot, -0.5, 0.5, 2);

    EXPECT_EQ(2, one.timeSteps);
    ASSERT_EQ(2u, one.instances[0].transformSteps.size());
    EXPECT_NEAR(1., one.instances[0].transformSteps[0].translation[0], 1e-9);
    EXPECT_NEAR(3., one.instances[0].transformSteps[1].translation[0], 1e-9);
    EXPECT_NEAR(2., UtilityScene::transformAt(one.instances[0], 0.5).ExtractTranslation()[0], 1e-9);
    EXPECT_EQ(3, two.timeSteps);
    ASSERT_EQ(3u, two.instances[0].transformSteps.size());
    EXPECT_NEAR(-1., two.instances[0].transformSteps[0].translation[0], 1e-9) << "before its samples a transform holds the first";
    EXPECT_NEAR(1., two.instances[0].transformSteps[1].translation[0], 1e-9);
    EXPECT_NEAR(3., two.instances[0].transformSteps[2].translation[0], 1e-9) << "beyond its samples a transform holds the last";
    EXPECT_NE(0u, one.version & kEvaluatedBit);
    EXPECT_EQ(one.version, UtilityScene::atShutter(snapshot, 0., 0.25, 1).version) << "the same shutter builds nothing again";
    EXPECT_NE(one.version, two.version) << "each shutter is a scene of its own to the tracers";
    EXPECT_EQ(1, UtilityScene::atShutter(snapshot, 0.1, 0.1, 4).timeSteps) << "a closed shutter";
}

// A spin's end-points blend along the shortest arc, so a quarter turn's middle keeps the object's size.
TEST(NatronUtilityMotionScene, ASpinKeepsItsSize)
{
    UtilityScene::Instance instance;

    instance.transformSteps = { UtilityScene::decompose(GfMatrix4d(1.)),
                                UtilityScene::decompose(GfMatrix4d().SetRotate(GfRotation(GfVec3d(0., 0., 1.), 90.))) };

    for (double time : { 0.25, 0.5, 0.75 }) {
        const GfVec3d x = UtilityScene::transformAt(instance, time).TransformDir(GfVec3d(1., 0., 0.));

        EXPECT_NEAR(1., x.GetLength(), 1e-9) << time;
        EXPECT_NEAR(90. * time, GfRadiansToDegrees(std::atan2(x[1], x[0])), 1e-6) << time;
    }
}

TEST(NatronUtilityMotionScene, TransformsInterpolateByDecomposition)
{
    const GfMatrix4d a = GfMatrix4d(1.).SetScale(GfVec3d(1., 1., 1.));
    const GfMatrix4d b = GfMatrix4d().SetScale(GfVec3d(3., 3., 3.)) * GfMatrix4d().SetRotate(GfRotation(GfVec3d(0., 0., 1.), 90.)) * GfMatrix4d().SetTranslate(GfVec3d(4., 0., 0.));
    const GfMatrix4d middle = UtilityScene::interpolate(a, b, 0.5);
    const GfMatrix4d expected = GfMatrix4d().SetScale(GfVec3d(2., 2., 2.)) * GfMatrix4d().SetRotate(GfRotation(GfVec3d(0., 0., 1.), 45.)) * GfMatrix4d().SetTranslate(GfVec3d(2., 0., 0.));

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            EXPECT_NEAR(expected[row][column], middle[row][column], 1e-9) << row << ", " << column;
        }
    }
    EXPECT_EQ(a, UtilityScene::interpolate(a, b, 0.));
    EXPECT_EQ(b, UtilityScene::interpolate(a, b, 1.));
}

TEST(NatronUtilityMotionScene, PointsInterpolateLinearly)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addStripe(scene, 0.);
    std::shared_ptr<UtilityScene::PointMotion> motion = std::make_shared<UtilityScene::PointMotion>();

    motion->times = { 0., 1. };
    motion->points = { VtVec3fArray { GfVec3f(0.f, -50.f, 0.f), GfVec3f(1.f, -50.f, 0.f), GfVec3f(1.f, 50.f, 0.f), GfVec3f(0.f, 50.f, 0.f) },
                       VtVec3fArray { GfVec3f(4.f, -50.f, 0.f), GfVec3f(5.f, -50.f, 0.f), GfVec3f(5.f, 50.f, 0.f), GfVec3f(4.f, 50.f, 0.f) } };
    scene.setPointMotion(id, motion);

    const UtilityScene::Snapshot snapshot = scene.snapshot();
    const UtilityScene::Snapshot sampled = UtilityScene::atShutter(snapshot, 0.25, 0.75, 1);
    const UtilityScene::Geometry& geometry = sampled.geometries.at(id);

    ASSERT_EQ(2u, geometry.pointSteps.size());
    ASSERT_EQ(4u, geometry.pointSteps[0].size());
    EXPECT_FLOAT_EQ(1.f, geometry.pointSteps[0][0][0]);
    EXPECT_FLOAT_EQ(2.f, geometry.pointSteps[0][1][0]);
    EXPECT_FLOAT_EQ(3.f, geometry.pointSteps[1][0][0]);
    EXPECT_EQ(snapshot.geometries.at(id).topologyVersion, geometry.topologyVersion) << "moved points keep their topology";
    EXPECT_NE(snapshot.geometries.at(id).pointsVersion, geometry.pointsVersion);
}

TEST(NatronUtilityMotionScene, TheCameraHasStepsOnlyWhenItMoves)
{
    const WavefrontFrame frame = makeFrame(1);
    WavefrontMotion motion = shutter(-0.5, 0.5, 2);

    EXPECT_TRUE(wavefrontCameraSteps(frame, motion).empty()) << "a still camera";
    for (double x : { -1., 0., 1. }) {
        GfFrustum camera = orthographicCamera();

        camera.SetPosition(GfVec3d(x, 0., kCameraZ));
        motion.cameraTimes.push_back(x / 2.);
        motion.cameraViews.push_back(camera.ComputeViewMatrix());
        motion.cameraProjections.push_back(camera.ComputeProjectionMatrix());
    }
    EXPECT_EQ(std::size_t(4 * (1 + 3 * kKernelCameraRowsPerStep)), wavefrontCameraSteps(frame, motion).size());

    WavefrontMotion closed = motion;

    closed.open = closed.close = 0.;
    EXPECT_TRUE(wavefrontCameraSteps(frame, closed).empty()) << "a closed shutter";
}

TEST_P(NatronUtilityMotion, AStripeMovingAPixelAFrameSmearsAsTheBoxFilterSays)
{
    if (!available()) {
        return;
    }
    UtilityScene scene;

    moveStripe(scene, addStripe(scene, 0.), -0.5, -0.5, 0.5, 0.5);

    const std::vector<GfVec4f> colors = draw(scene, makeFrame(8), shutter(-0.5, 0.5, 1));

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

    double sum = 0.;

    for (int x = 0; x < kSize; ++x) {
        const double alpha = colors[(kSize / 2) * kSize + x][3];

        sum += alpha;
        // 64 samples, each at a time of its own: within a sample's share in space and in time.
        EXPECT_NEAR(smearedCoverage(x, -0.5, 0.5), alpha, 0.1) << "pixel " << x;
    }
    EXPECT_NEAR(0.125, smearedCoverage(7, -0.5, 0.5), 1e-6);
    EXPECT_NEAR(0.75, smearedCoverage(8, -0.5, 0.5), 1e-6);
    EXPECT_NEAR(1., sum, 0.05) << "the smear spreads the stripe without adding to it";
    EXPECT_GT(colors[(kSize / 2) * kSize + 7][3], 0.f);
    EXPECT_GT(colors[(kSize / 2) * kSize + 9][3], 0.f);
}

// One segment already interpolates, so more segments of a straight move only split it where it is straight.
TEST_P(NatronUtilityMotion, MoreSegmentsOfAStraightMoveDrawTheSameSmear)
{
    if (!available()) {
        return;
    }
    UtilityScene scene;

    moveStripe(scene, addStripe(scene, 0.), -0.5, -0.5, 0.5, 0.5);

    const std::vector<GfVec4f> one = draw(scene, makeFrame(8), shutter(-0.5, 0.5, 1));
    const std::vector<GfVec4f> eight = draw(scene, makeFrame(8), shutter(-0.5, 0.5, 8));

    ASSERT_EQ(one.size(), eight.size());
    for (int x = 0; x < kSize; ++x) {
        // A sample right on the stripe's edge may land either side of it.
        EXPECT_NEAR(one[(kSize / 2) * kSize + x][3], eight[(kSize / 2) * kSize + x][3], 2.5 / 64.) << "pixel " << x;
    }
}

TEST_P(NatronUtilityMotion, AClosedShutterOrAStillSceneDrawsTheUnblurredFrame)
{
    if (!available()) {
        return;
    }
    UtilityScene scene;

    moveStripe(scene, addStripe(scene, 0.), -0.5, -0.5, 0.5, 0.5);

    UtilityScene still;

    addStripe(still, 0.);

    const std::vector<GfVec4f> unblurred = draw(still, makeFrame(4), shutter(0., 0., 1));

    {
        SCOPED_TRACE("a closed shutter");
        expectSameColors(unblurred, draw(scene, makeFrame(4), shutter(0., 0., 8)));
    }
    {
        SCOPED_TRACE("nothing moving");
        expectSameColors(unblurred, draw(still, makeFrame(4), shutter(-0.5, 0.5, 8)));
    }
}

// The stripe's points move rather than its transform: the same smear, through the deforming path.
TEST_P(NatronUtilityMotion, ADeformingStripeSmearsAsTheBoxFilterSays)
{
    if (!available()) {
        return;
    }
    UtilityScene scene;
    const UtilityScene::GeometryId id = addStripe(scene, 0.);
    std::shared_ptr<UtilityScene::PointMotion> motion = std::make_shared<UtilityScene::PointMotion>();

    motion->times = { -0.5, 0.5 };
    motion->points = { VtVec3fArray { GfVec3f(-0.5f, -50.f, 0.f), GfVec3f(0.5f, -50.f, 0.f), GfVec3f(0.5f, 50.f, 0.f), GfVec3f(-0.5f, 50.f, 0.f) },
                       VtVec3fArray { GfVec3f(0.5f, -50.f, 0.f), GfVec3f(1.5f, -50.f, 0.f), GfVec3f(1.5f, 50.f, 0.f), GfVec3f(0.5f, 50.f, 0.f) } };
    scene.setPointMotion(id, motion);

    const std::vector<GfVec4f> colors = draw(scene, makeFrame(8), shutter(-0.5, 0.5, 1));

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());
    for (int x = 0; x < kSize; ++x) {
        EXPECT_NEAR(smearedCoverage(x, -0.5, 0.5), colors[(kSize / 2) * kSize + x][3], 0.1) << "pixel " << x;
    }
}

// Set NATRON_UTILITY_MOTION_BENCH to run: a deforming mesh of a million triangles over 8 segments.
TEST_P(NatronUtilityMotion, BenchADeformingMillionTriangleMesh)
{
    if (!available()) {
        return;
    }
    if (!std::getenv("NATRON_UTILITY_MOTION_BENCH")) {
        std::printf("[  SKIPPED ] set NATRON_UTILITY_MOTION_BENCH to run the bench\n");

        return;
    }
    const int columns = 1000;
    const int rows = 500;
    VtVec3fArray from((columns + 1) * (rows + 1));
    VtVec3fArray to(from.size());
    VtVec3iArray triangles(static_cast<std::size_t>(columns * rows * 2));

    for (int r = 0; r <= rows; ++r) {
        for (int c = 0; c <= columns; ++c) {
            const float x = -kHalfSize + 2.f * kHalfSize * c / columns;
            const float y = -kHalfSize + 2.f * kHalfSize * r / rows;
            const std::size_t i = static_cast<std::size_t>(r * (columns + 1) + c);

            from[i] = GfVec3f(x, y, 0.1f * std::sin(x));
            to[i] = GfVec3f(x + 0.5f, y, 0.1f * std::cos(y));
        }
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const int a = r * (columns + 1) + c;
            const std::size_t t = static_cast<std::size_t>((r * columns + c) * 2);

            triangles[t] = GfVec3i(a, a + 1, a + columns + 2);
            triangles[t + 1] = GfVec3i(a, a + columns + 2, a + columns + 1);
        }
    }

    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/grid"), 1);
    std::shared_ptr<UtilityScene::PointMotion> motion = std::make_shared<UtilityScene::PointMotion>();

    scene.setTopology(id, from, triangles);
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    motion->times = { -0.5, 0.5 };
    motion->points = { from, to };
    scene.setPointMotion(id, motion);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    draw(scene, makeFrame(4), shutter(-0.5, 0.5, 8));

    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("[   BENCH  ] %d triangles, 8 segments, 16 samples a pixel: %.3f s\n", columns * rows * 2, total);
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityMotion, ::testing::Values(Backend::Cpu, Backend::GpuTiles, Backend::GpuFilm));

// A shutter from the frame to the next over which the stripe moves 4 units smears it over the pixels from
// its rest position on, each sample at a time of its own.
void
expectSmear(const MotionRender& render)
{
    double sum = 0.;

    for (int x = 0; x < kSize; ++x) {
        const float alpha = render.alphaAt(x);

        sum += alpha;
        EXPECT_NEAR(smearedCoverage(x, 0., 4.), alpha, 0.1) << "pixel " << x;
    }
    EXPECT_NEAR(1., sum, 0.05) << "the smear spreads the stripe without adding to it";
}

TEST(NatronUtilityMotionDelegate, ATranslatingStripeSmears)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 1.), 8, 2);

    render.render(1.);
    expectSmear(render);
}

TEST(NatronUtilityMotionDelegate, AVelocitiesOnlyMeshMovesByItsVelocity)
{
    // 96 units a second at 24 frames a second: 4 units a frame.
    MotionRender render(stage(std::string("    point3f[] points.timeSamples = { 1: ") + kStripePoints + " }\n"
                                                                                                        "    vector3f[] velocities.timeSamples = { 1: [(96, 0, 0), (96, 0, 0), (96, 0, 0), (96, 0, 0)] }\n"),
                        GfVec2d(0., 1.), 8, 2);

    render.render(1.);
    expectSmear(render);
}

TEST(NatronUtilityMotionDelegate, OneSegmentBlurs)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 1.), 8, 1);

    render.render(1.);
    expectSmear(render);
}

// The stripe gains a point it does not use at mid-shutter, where it is 2 units on.
TEST(NatronUtilityMotionDelegate, AMeshWhosePointCountChangesIsStillAtMidShutterAndNamed)
{
    MotionRender render(stage(std::string("    point3f[] points.timeSamples = { 1: ") + kStripePoints + ", 1.5: [(2, -50, 0), (3, -50, 0), (3, 50, 0), (2, 50, 0), (9, 9, 0)] }\n"),
                        GfVec2d(0., 1.), 2, 2);

    render.render(1.);
    EXPECT_FLOAT_EQ(0.f, render.alphaAt(kRestPixel)) << "a mesh whose topology changes is still at mid-shutter, not the frame";
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kMovedPixel));

    const std::string warning = VtDictionaryGet<std::string>(render.stats(), kUtilityStatMotionWarning, VtDefault = std::string());

    EXPECT_NE(std::string::npos, warning.find("/Stripe")) << warning;
}

// The points stay; at mid-shutter the face is drawn through the four of them 2 units on.
TEST(NatronUtilityMotionDelegate, AMeshWhoseIndicesChangeIsStillAtMidShutterAndNamed)
{
    MotionRender render("#usda 1.0\n(\n    timeCodesPerSecond = 24\n)\n"
                        "def Mesh \"Stripe\" {\n"
                        "    int[] faceVertexCounts = [4]\n"
                        "    int[] faceVertexIndices.timeSamples = { 1: [0, 1, 2, 3], 1.5: [4, 5, 6, 7] }\n"
                        "    point3f[] points = [(0, -50, 0), (1, -50, 0), (1, 50, 0), (0, 50, 0), (2, -50, 0), (3, -50, 0), (3, 50, 0), (2, 50, 0)]\n"
                        "    color3f[] primvars:displayColor = [(1, 1, 1)] (interpolation = \"constant\")\n"
                        "}\n",
                        GfVec2d(0., 1.), 2, 2);

    render.render(1.);
    EXPECT_FLOAT_EQ(0.f, render.alphaAt(kRestPixel)) << "a mesh whose indices change is still at mid-shutter, not the frame";
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kMovedPixel));

    const std::string warning = VtDictionaryGet<std::string>(render.stats(), kUtilityStatMotionWarning, VtDefault = std::string());

    EXPECT_NE(std::string::npos, warning.find("/Stripe")) << warning;
}

TEST(NatronUtilityMotionDelegate, TheSegmentsSettingSplitsAStraightMoveWithoutChangingIt)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 1.), 8, 4);

    render.render(1.);
    expectSmear(render);
    EXPECT_TRUE(VtDictionaryGet<std::string>(render.stats(), kUtilityStatMotionWarning, VtDefault = std::string()).empty());
}

// The stripe goes out 4 units and back within the shutter. One segment spans only its ends, both at rest;
// eight follow it out.
TEST(NatronUtilityMotionDelegate, MoreSegmentsFollowAMoveThatTurnsBack)
{
    const std::string usda = stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                            "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 1.25: (2, 0, 0), 1.5: (4, 0, 0), 1.75: (2, 0, 0), 2: (0, 0, 0) }\n"
                                                                                            "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n");
    MotionRender one(usda, GfVec2d(0., 1.), 8, 1);
    MotionRender eight(usda, GfVec2d(0., 1.), 8, 8);

    one.render(1.);
    eight.render(1.);
    EXPECT_NEAR(1.f, one.alphaAt(kRestPixel), 1e-5f) << "one segment from the stripe at rest back to rest holds it there";
    EXPECT_LT(eight.alphaAt(kRestPixel), 0.5f);
    EXPECT_GT(eight.alphaAt(kRestPixel + 2) + eight.alphaAt(kRestPixel + 3), 0.2f) << "eight segments reach out along the move";
}

TEST(NatronUtilityMotionDelegate, AClosedShutterRendersTheFrame)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 0.), 2, 4);

    render.render(1.);
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kRestPixel));
}

namespace {
// A Catmull-Clark cube cage over eight points in cubePoints' order.
std::string
cubeMesh(const std::string& name,
         const std::string& pointsLine)
{
    return "def Mesh \"" + name + "\" {\n"
                                  "    uniform token subdivisionScheme = \"catmullClark\"\n"
                                  "    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n"
                                  "    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]\n"
                                  "    color3f[] primvars:displayColor = [(1, 1, 1)] (interpolation = \"constant\")\n"
        + pointsLine + "}\n";
}

// The corners of a cube of side 2 * half about (x, 0, 0).
std::string
cubePoints(double x,
           double half)
{
    const auto point = [](double px, double py, double pz) {
        return "(" + std::to_string(px) + ", " + std::to_string(py) + ", " + std::to_string(pz) + ")";
    };
    const double l = x - half;
    const double r = x + half;
    const double h = half;

    return "[" + point(l, -h, h) + ", " + point(r, -h, h) + ", " + point(l, h, h) + ", " + point(r, h, h) + ", "
        + point(l, h, -h) + ", " + point(r, h, -h) + ", " + point(l, -h, -h) + ", " + point(r, -h, -h) + "]";
}

std::string
cubeStage(const std::string& body)
{
    return std::string("#usda 1.0\n(\n    timeCodesPerSecond = 24\n)\n") + body;
}

#ifdef NATRON_HAVE_GPU
// Puts a GPU context, on the test's device, where the delegate looks for one, and the old one back after.
class MotionGpuRuntime {
public:
    MotionGpuRuntime()
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

    ~MotionGpuRuntime()
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

// The delegate on the CPU, or on the GPU with its colour the one AOV, kept in a film on the device.
class NatronUtilityMotionSubdiv : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() OVERRIDE
    {
        if (GetParam() == Backend::Cpu) {
            _ready = true;

            return;
        }
#ifdef NATRON_HAVE_GPU
        _gpu.reset(new MotionGpuRuntime);
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

    const char* device() const
    {
        return GetParam() == Backend::Cpu ? kUtilityDeviceCpu : kUtilityDeviceGpu;
    }

    void expectDevice(const MotionRender& render) const
    {
        if (GetParam() == Backend::Cpu) {
            EXPECT_EQ(0u, render.device().find("CPU")) << render.device();
        } else {
            EXPECT_EQ(0u, render.device().find("GPU")) << render.device();
        }
    }

private:
    bool _ready = false;
#ifdef NATRON_HAVE_GPU
    std::unique_ptr<MotionGpuRuntime> _gpu;
#endif
};
} // anon namespace

// A cube refined to level 1, its points moving 4 units within the shutter, smears its edges over the
// pixels it sweeps; with the shutter closed only the pixels its silhouette cuts are partly covered.
TEST_P(NatronUtilityMotionSubdiv, ADeformingCubeAtLevelOneBlurs)
{
    if (!ready()) {
        return;
    }
    const std::string usda = cubeStage(cubeMesh("Cube", "    point3f[] points.timeSamples = { 1: " + cubePoints(0., 2.) + ", 2: " + cubePoints(4., 2.) + " }\n"));
    MotionRender still(usda, GfVec2d(0., 0.), 4, 8, 1, device());
    MotionRender moving(usda, GfVec2d(0., 1.), 4, 8, 1, device());

    still.render(1.);
    moving.render(1.);
    expectDevice(still);
    expectDevice(moving);
    EXPECT_EQ(48u, still.stat(kUtilityStatTriangleCount)) << "the cube did not refine to level 1";
    EXPECT_EQ(48u, moving.stat(kUtilityStatTriangleCount)) << "the cube did not refine to level 1";
    EXPECT_LE(still.partialInMiddleRow(), 2);
    EXPECT_GE(moving.partialInMiddleRow(), 4);
    // Its middle at mid-shutter, 2 units on, is covered throughout the shutter's second half at least.
    EXPECT_GT(moving.alphaAt(kSize / 2 + 2), 0.45f);
    EXPECT_TRUE(VtDictionaryGet<std::string>(moving.stats(), kUtilityStatMotionWarning, VtDefault = std::string()).empty());
}

// Two instances of a level-1 cube under a point instancer moving 4 units within the shutter each smear
// over the pixels they sweep, the prototype refined once for both.
TEST_P(NatronUtilityMotionSubdiv, AnInstancedSubdividedCubeBlursWithItsInstancersTransform)
{
    if (!ready()) {
        return;
    }
    const std::string usda = cubeStage("def PointInstancer \"Scatter\" {\n"
                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                       "    point3f[] positions = [(-6, 0, 0), (0, 0, 0)]\n"
                                       "    int[] protoIndices = [0, 0]\n"
                                       "    rel prototypes = </Scatter/Protos/Cube>\n"
                                       "    def Scope \"Protos\" {\n"
                                       + cubeMesh("Cube", "    point3f[] points = " + cubePoints(0., 1.) + "\n")
                                       + "    }\n"
                                         "}\n");
    MotionRender still(usda, GfVec2d(0., 0.), 4, 8, 1, device());
    MotionRender moving(usda, GfVec2d(0., 1.), 4, 8, 1, device());

    still.render(1.);
    moving.render(1.);
    expectDevice(still);
    expectDevice(moving);
    EXPECT_EQ(1u, moving.stat(kUtilityStatGeometryCount));
    EXPECT_EQ(2u, moving.stat(kUtilityStatInstanceCount));
    EXPECT_EQ(48u, moving.stat(kUtilityStatTriangleCount)) << "the prototype did not refine to level 1";
    EXPECT_LE(still.partialInMiddleRow(), 4);
    EXPECT_GE(moving.partialInMiddleRow(), 8);
    EXPECT_TRUE(VtDictionaryGet<std::string>(moving.stats(), kUtilityStatMotionWarning, VtDefault = std::string()).empty());
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityMotionSubdiv, ::testing::Values(Backend::Cpu, Backend::GpuFilm));
