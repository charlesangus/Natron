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

    // Draws the whole frame as the render pass does, bucket by bucket.
    std::vector<GfVec4f> draw(const UtilityScene& scene,
                              const WavefrontFrame& frame,
                              const WavefrontMotion& motion,
                              std::vector<double>* bucketSeconds = nullptr)
    {
        ColorFilm film(GetParam() == Backend::GpuFilm);
        const std::atomic<bool> stop(false);
        WavefrontExecutor* executor = executorFor();

        if (!executor) {
            ADD_FAILURE() << "no executor";

            return std::vector<GfVec4f>();
        }
        EXPECT_TRUE(wavefrontDrawSamples(*executor, scene.snapshot(), frame, motion, film, stop, 2, std::function<void(std::size_t)>(), bucketSeconds));

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
                 int segments)
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
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(kUtilityDeviceCpu)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingPixelSamples), VtValue(pixelSamples));
        _engine->SetRendererSetting(TfToken(kUtilitySettingPixelFilter), VtValue(TfToken(kUtilityFilterBox)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingFilterWidth), VtValue(1.f));
        _engine->SetRendererSetting(TfToken(kUtilitySettingShutterSegments), VtValue(segments));
        _engine->SetRendererSetting(TfToken(kRenderDelegateUtilityShutterSetting), VtValue(shutterInterval));
    }

    void render(double frame)
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(60);

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

TEST(NatronUtilityMotionScene, ATranslationEvaluatesToItsInterpolatedPosition)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addStripe(scene, 0.);

    moveStripe(scene, id, -0.25, -1., 0.25, 3.);

    const UtilityScene::Snapshot snapshot = scene.snapshot();

    ASSERT_TRUE(UtilityScene::moves(snapshot));
    ASSERT_EQ(1u, snapshot.instances.size());

    const UtilityScene::Snapshot middle = UtilityScene::evaluateAt(snapshot, 0.);
    const UtilityScene::Snapshot quarter = UtilityScene::evaluateAt(snapshot, 0.125);
    const UtilityScene::Snapshot past = UtilityScene::evaluateAt(snapshot, 1.);

    EXPECT_NEAR(1., middle.instances[0].transform.ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(2., quarter.instances[0].transform.ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(3., past.instances[0].transform.ExtractTranslation()[0], 1e-9) << "beyond its samples a transform holds the last";
    EXPECT_FALSE(UtilityScene::moves(middle)) << "an evaluated scene is still";
    EXPECT_NE(0u, middle.version & kEvaluatedBit);
    EXPECT_NE(middle.version, quarter.version) << "each evaluation is a scene of its own to the tracers";
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
    const UtilityScene::Snapshot quarter = UtilityScene::evaluateAt(snapshot, 0.25);
    const UtilityScene::Geometry& geometry = quarter.geometries.at(id);

    ASSERT_EQ(4u, geometry.points.size());
    EXPECT_FLOAT_EQ(1.f, geometry.points[0][0]);
    EXPECT_FLOAT_EQ(2.f, geometry.points[1][0]);
    EXPECT_EQ(quarter.version, geometry.pointsVersion) << "moved points are a refit";
    EXPECT_EQ(snapshot.geometries.at(id).topologyVersion, geometry.topologyVersion) << "moved points keep their topology";
    EXPECT_FALSE(UtilityScene::moves(quarter));
}

TEST(NatronUtilityMotionScene, BucketsFollowTheShutterAndTheSamples)
{
    UtilityScene still;

    addStripe(still, 0.);

    UtilityScene moving;

    moveStripe(moving, addStripe(moving, 0.), -0.5, -0.5, 0.5, 0.5);
    EXPECT_EQ(1, wavefrontBucketCount(shutter(0., 0., 8), moving.snapshot(), 64)) << "a closed shutter";
    EXPECT_EQ(1, wavefrontBucketCount(shutter(-0.5, 0.5, 8), still.snapshot(), 64)) << "nothing moving";
    EXPECT_EQ(8, wavefrontBucketCount(shutter(-0.5, 0.5, 8), moving.snapshot(), 64));
    EXPECT_EQ(4, wavefrontBucketCount(shutter(-0.5, 0.5, 8), moving.snapshot(), 4)) << "no more buckets than samples";
    EXPECT_DOUBLE_EQ(-0.375, wavefrontBucketTime(shutter(-0.5, 0.5, 4), 0, 4));
    EXPECT_DOUBLE_EQ(0.375, wavefrontBucketTime(shutter(-0.5, 0.5, 4), 3, 4));

    for (int samples : { 1, 4, 9, 64, 256 }) {
        for (int buckets : { 1, 3, 8 }) {
            std::vector<int> order = wavefrontSampleOrder(samples, buckets);

            std::sort(order.begin(), order.end());
            ASSERT_EQ(static_cast<std::size_t>(samples), order.size());
            for (int i = 0; i < samples; ++i) {
                EXPECT_EQ(i, order[static_cast<std::size_t>(i)]) << samples << " samples in " << buckets << " buckets";
            }
        }
    }
}

TEST_P(NatronUtilityMotion, AStripeMovingAPixelAFrameSmearsAsTheBoxFilterSays)
{
    if (!available()) {
        return;
    }
    UtilityScene scene;

    moveStripe(scene, addStripe(scene, 0.), -0.5, -0.5, 0.5, 0.5);

    std::vector<double> bucketSeconds;
    const std::vector<GfVec4f> colors = draw(scene, makeFrame(8), shutter(-0.5, 0.5, 16), &bucketSeconds);

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());
    EXPECT_EQ(16u, bucketSeconds.size());

    double sum = 0.;

    for (int x = 0; x < kSize; ++x) {
        const double alpha = colors[(kSize / 2) * kSize + x][3];

        sum += alpha;
        // 64 samples in 16 buckets: within a sample's share in space, and a bucket's in time.
        EXPECT_NEAR(smearedCoverage(x, -0.5, 0.5), alpha, 0.1) << "pixel " << x;
    }
    EXPECT_NEAR(0.125, smearedCoverage(7, -0.5, 0.5), 1e-6);
    EXPECT_NEAR(0.75, smearedCoverage(8, -0.5, 0.5), 1e-6);
    EXPECT_NEAR(1., sum, 0.05) << "the smear spreads the stripe without adding to it";
    EXPECT_GT(colors[(kSize / 2) * kSize + 7][3], 0.f);
    EXPECT_GT(colors[(kSize / 2) * kSize + 9][3], 0.f);
}

TEST_P(NatronUtilityMotion, OneSegmentDrawsTheUnblurredFrame)
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
        SCOPED_TRACE("one segment");
        expectSameColors(unblurred, draw(scene, makeFrame(4), shutter(-0.5, 0.5, 1)));
    }
    {
        SCOPED_TRACE("a closed shutter");
        expectSameColors(unblurred, draw(scene, makeFrame(4), shutter(0., 0., 8)));
    }
    {
        SCOPED_TRACE("nothing moving");
        expectSameColors(unblurred, draw(still, makeFrame(4), shutter(-0.5, 0.5, 8)));
    }
}

// Set NATRON_UTILITY_MOTION_BENCH to run: a deforming mesh of a million triangles, 8 buckets.
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

    std::vector<double> bucketSeconds;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    draw(scene, makeFrame(4), shutter(-0.5, 0.5, 8), &bucketSeconds);

    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    ASSERT_EQ(8u, bucketSeconds.size());
    for (std::size_t b = 0; b < bucketSeconds.size(); ++b) {
        std::printf("[   BENCH  ] bucket %zu: %.3f s\n", b, bucketSeconds[b]);
    }
    std::printf("[   BENCH  ] %d triangles, 8 buckets, 16 samples a pixel: %.3f s\n", columns * rows * 2, total);
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityMotion, ::testing::Values(Backend::Cpu, Backend::GpuTiles, Backend::GpuFilm));

TEST(NatronUtilityMotionDelegate, ATranslatingStripeIsAtItsMidShutterPosition)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 1.), 1, 1);

    render.render(1.);
    EXPECT_FLOAT_EQ(0.f, render.alphaAt(kRestPixel)) << "the frame's position is not mid-shutter";
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kMovedPixel));
}

TEST(NatronUtilityMotionDelegate, AVelocitiesOnlyMeshMovesByItsVelocity)
{
    // 96 units a second over the half frame from the frame to mid-shutter, at 24 frames a second: 2 units.
    MotionRender render(stage(std::string("    point3f[] points.timeSamples = { 1: ") + kStripePoints + " }\n"
                                                                                                        "    vector3f[] velocities.timeSamples = { 1: [(96, 0, 0), (96, 0, 0), (96, 0, 0), (96, 0, 0)] }\n"),
                        GfVec2d(0., 1.), 1, 1);

    render.render(1.);
    EXPECT_FLOAT_EQ(0.f, render.alphaAt(kRestPixel));
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kMovedPixel));
}

TEST(NatronUtilityMotionDelegate, ATopologyChangingMeshIsStillAndNamed)
{
    MotionRender render(stage(std::string("    point3f[] points.timeSamples = { 1: ") + kStripePoints + ", 2: [(4, -50, 0), (5, -50, 0), (5, 50, 0), (4, 50, 0), (9, 9, 0)] }\n"),
                        GfVec2d(0., 1.), 1, 1);

    render.render(1.);
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kRestPixel)) << "a mesh whose topology changes renders at the frame";
    EXPECT_FLOAT_EQ(0.f, render.alphaAt(kMovedPixel));

    const std::string warning = VtDictionaryGet<std::string>(render.stats(), kUtilityStatMotionWarning, VtDefault = std::string());

    EXPECT_NE(std::string::npos, warning.find("/Stripe")) << warning;
}

TEST(NatronUtilityMotionDelegate, AMovingFrameTracesEachSegment)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 1.), 2, 4);

    render.render(1.);

    const VtDoubleArray buckets = VtDictionaryGet<VtDoubleArray>(render.stats(), kUtilityStatBucketSeconds, VtDefault = VtDoubleArray());

    EXPECT_EQ(4u, buckets.size());
    EXPECT_GT(render.alphaAt(kRestPixel + 1), 0.f) << "the stripe sweeps the pixels between";
    EXPECT_TRUE(VtDictionaryGet<std::string>(render.stats(), kUtilityStatMotionWarning, VtDefault = std::string()).empty());
}

TEST(NatronUtilityMotionDelegate, AClosedShutterRendersTheFrame)
{
    MotionRender render(stage(std::string("    point3f[] points = ") + kStripePoints + "\n"
                                                                                       "    double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (4, 0, 0) }\n"
                                                                                       "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"),
                        GfVec2d(0., 0.), 2, 4);

    render.render(1.);
    EXPECT_FLOAT_EQ(1.f, render.alphaAt(kRestPixel));
    EXPECT_EQ(1u, VtDictionaryGet<VtDoubleArray>(render.stats(), kUtilityStatBucketSeconds, VtDefault = VtDoubleArray()).size());
}
