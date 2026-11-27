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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuTexture.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Gpu/GpuTestDevice.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

#ifdef NATRON_HAVE_GPU

namespace {
// The view spans [-kHalfWidth, kHalfWidth] in x and y over kSize by kSize texels, looking down -z.
const float kHalfWidth = 4.f;
const std::uint32_t kSize = 48;
// The orthographic view maps z from kDepthRange (depth 0) to -kDepthRange (depth 1).
const float kDepthRange = 10.f;

// An id pair: the instance and the primitive.
using Ids = std::array<std::uint32_t, 2>;

VtVec3fArray
quadPoints()
{
    return VtVec3fArray { GfVec3f(0.f, 0.f, 0.f), GfVec3f(1.f, 0.f, 0.f), GfVec3f(1.f, 1.f, 0.f), GfVec3f(0.f, 1.f, 0.f) };
}

VtVec3iArray
quadTriangles()
{
    return VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) };
}

// The cube from -1 to 1, two triangles per face.
void
addCube(UtilityScene& scene,
        UtilityScene::GeometryId id)
{
    VtVec3fArray points;

    for (int corner = 0; corner < 8; ++corner) {
        points.push_back(GfVec3f(corner & 1 ? 1.f : -1.f, corner & 2 ? 1.f : -1.f, corner & 4 ? 1.f : -1.f));
    }

    const int faces[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
    VtVec3iArray triangles;

    for (const auto& face : faces) {
        triangles.push_back(GfVec3i(face[0], face[1], face[2]));
        triangles.push_back(GfVec3i(face[0], face[2], face[3]));
    }
    scene.setTopology(id, points, triangles);
}

GfMatrix4d
placed(const GfVec3d& axis,
       double degrees,
       const GfVec3d& at)
{
    GfMatrix4d rotation(1.);
    GfMatrix4d translation(1.);

    rotation.SetRotate(GfRotation(axis, degrees));
    translation.SetTranslate(at);

    return rotation * translation;
}

GfMatrix4d
translation(double x,
            double y,
            double z)
{
    GfMatrix4d matrix(1.);

    matrix.SetTranslate(GfVec3d(x, y, z));

    return matrix;
}

GfMatrix4d
spin(double degrees)
{
    GfMatrix4d matrix(1.);

    matrix.SetRotate(GfRotation(GfVec3d(0., 0., 1.), degrees));

    return matrix;
}

void
moveInstance(UtilityScene& scene,
             UtilityScene::GeometryId id,
             const GfMatrix4d& open,
             const GfMatrix4d& close)
{
    std::shared_ptr<UtilityScene::TransformMotion> motion = std::make_shared<UtilityScene::TransformMotion>();

    motion->times = { 0., 1. };
    motion->transforms = { { open }, { close } };
    scene.setInstances(id, { open });
    scene.setTransformMotion(id, motion);
}

// The world position of a texel's centre.
float
worldX(std::uint32_t x)
{
    return ((x + 0.5f) / kSize * 2.f - 1.f) * kHalfWidth;
}

// Vulkan's y runs down the attachment, and the view puts +y at its top.
float
worldY(std::uint32_t y)
{
    return -((y + 0.5f) / kSize * 2.f - 1.f) * kHalfWidth;
}

GpuTracer::VisibilityPass
orthographic(float time)
{
    GpuTracer::VisibilityPass pass;
    const float rows[4][4] = {
        { 1.f / kHalfWidth, 0.f, 0.f, 0.f },
        { 0.f, -1.f / kHalfWidth, 0.f, 0.f },
        { 0.f, 0.f, -0.5f / kDepthRange, 0.f },
        { 0.f, 0.f, 0.5f, 1.f },
    };

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            pass.viewProjection[row][column] = rows[row][column];
        }
    }
    pass.time = time;

    return pass;
}

// The frames the executor draws: kFrameSize by kFrameSize pixels.
const int kFrameSize = 48;

// A camera at z = 12 looking down -z at the origin, its view 30 degrees wide.
WavefrontFrame
perspectiveFrame()
{
    GfFrustum camera;

    camera.SetPerspective(30., 1., 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., 12.));

    WavefrontFrame frame;

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();
    frame.window.width = kFrameSize;
    frame.window.height = kFrameSize;
    frame.sampling.samplesPerSide = 1;
    frame.sampling.filter = eKernelPixelFilterBox;
    frame.sampling.filterWidth = 1.f;
    frame.sampling.seed = 7;
    frame.maxRayDepth = 0;

    return frame;
}

// An orthographic camera over kFrameSize world units, one per pixel: pixel p spans x from p - kFrameSize / 2.
WavefrontFrame
pixelFrame()
{
    const double half = kFrameSize / 2.;
    GfFrustum camera;

    camera.SetOrthographic(-half, half, -half, half, 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., 30.));

    WavefrontFrame frame = perspectiveFrame();

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();

    return frame;
}

// With no lights in the scene, surfaces emit their displayColor, so that the colour is the interpolated primvar.
void
emitDisplayColor(UtilityScene& scene,
                 UtilityScene::GeometryId id)
{
    UtilityMaterialDesc emissive;

    emissive.diffuseColor = GfVec3f(0.f);
    emissive.emissiveFromDisplayColor = true;
    scene.setMaterial(SdfPath("/Emissive"), std::make_shared<const UtilityMaterialDesc>(emissive));
    scene.setMaterialId(id, SdfPath("/Emissive"));
}

// A card two units wide at z = 1, turned about y, its colour a gradient over its corners as a texture's lookup
// through the barycentrics would give, and a cube behind it.
void
cardAndCube(UtilityScene& scene)
{
    const UtilityScene::GeometryId card = scene.addGeometry(SdfPath("/card"), 1);
    const UtilityScene::GeometryId cube = scene.addGeometry(SdfPath("/cube"), 2);

    scene.setTopology(card, VtVec3fArray { GfVec3f(-1.f, -1.f, 0.f), GfVec3f(1.f, -1.f, 0.f), GfVec3f(1.f, 1.f, 0.f), GfVec3f(-1.f, 1.f, 0.f) }, quadTriangles());
    scene.setColors(card, UtilityScene::eColorBindingVertex, VtVec4fArray { GfVec4f(1.f, 0.f, 0.f, 1.f), GfVec4f(0.f, 1.f, 0.f, 1.f), GfVec4f(0.f, 0.f, 1.f, 1.f), GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(card, { placed(GfVec3d(0., 1., 0.), 30., GfVec3d(-0.6, 0.2, 1.)) });
    emitDisplayColor(scene, card);
    addCube(scene, cube);
    scene.setColors(cube, UtilityScene::eColorBindingTriangle, VtVec4fArray(12, GfVec4f(0.25f, 0.5f, 0.75f, 1.f)));
    scene.setInstances(cube, { placed(GfVec3d(1., 1., 0.), 35., GfVec3d(0.8, -0.4, -1.5)) });
    emitDisplayColor(scene, cube);
}

// A white stripe one pixel wide over [from, from + 1) along x and the whole frame along y, moving by
// distance along x from the shutter's opening to its closing.
void
movingStripe(UtilityScene& scene,
             double from,
             double distance)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/stripe"), 1);
    const float a = static_cast<float>(from);
    const float b = static_cast<float>(from + 1.);
    const float tall = static_cast<float>(kFrameSize);

    scene.setTopology(id, VtVec3fArray { GfVec3f(a, -tall, 0.f), GfVec3f(b, -tall, 0.f), GfVec3f(b, tall, 0.f), GfVec3f(a, tall, 0.f) }, quadTriangles());
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    emitDisplayColor(scene, id);
    if (distance != 0.) {
        moveInstance(scene, id, translation(0., 0., 0.), translation(distance, 0., 0.));
    }
}

WavefrontMotion
openShutter()
{
    WavefrontMotion motion;

    motion.open = 0.;
    motion.close = 1.;

    return motion;
}

// Every AOV of each pixel: the colour filtered over the samples, the rest the heaviest sample's.
class AovFilm : public WavefrontFilm {
public:
    AovFilm()
        : color(kFrameSize * kFrameSize, GfVec4f(0.f))
        , weight(kFrameSize * kFrameSize, 0.f)
        , heaviest(kFrameSize * kFrameSize, -1.f)
        , depth(kFrameSize * kFrameSize, 0.f)
        , primId(kFrameSize * kFrameSize, -1)
        , instanceId(kFrameSize * kFrameSize, -1)
        , geometry(kFrameSize * kFrameSize, Tracer::kNoHit)
    {
    }

    virtual void writeTile(const WavefrontTile& tile) override
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const std::size_t pixel = static_cast<std::size_t>(tile.rect.y + row) * kFrameSize + tile.rect.x + column;
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;
                const float w = tile.weight ? tile.weight[index] : 1.f;

                if (hit && tile.color) {
                    color[pixel] += w * GfVec4f(tile.color[4 * index], tile.color[4 * index + 1], tile.color[4 * index + 2], tile.color[4 * index + 3]);
                }
                weight[pixel] += w;
                if (w > heaviest[pixel]) {
                    heaviest[pixel] = w;
                    geometry[pixel] = tile.hitGeometry[index];
                    depth[pixel] = hit && tile.depth ? tile.depth[index] : 0.f;
                    primId[pixel] = hit && tile.primId ? tile.primId[index] : -1;
                    instanceId[pixel] = hit && tile.instanceId ? tile.instanceId[index] : -1;
                }
            }
        }
    }

    virtual void passDrawn() override
    {
        ++passes;
    }

    GfVec4f filtered(std::size_t pixel) const
    {
        return weight[pixel] > 0.f ? color[pixel] / weight[pixel] : GfVec4f(0.f);
    }

    std::vector<GfVec4f> color;
    std::vector<float> weight;
    std::vector<float> heaviest;
    std::vector<float> depth;
    std::vector<std::int32_t> primId;
    std::vector<std::int32_t> instanceId;
    std::vector<std::uint32_t> geometry;
    int passes = 0;
};

// Whether a pixel and its eight neighbours all see the same surface, so that a sub-texel difference in where
// raster and rays put an edge cannot change it.
bool
awayFromEdges(const AovFilm& film,
              int x,
              int y)
{
    if (x == 0 || y == 0 || x == kFrameSize - 1 || y == kFrameSize - 1) {
        return false;
    }

    const std::size_t pixel = static_cast<std::size_t>(y) * kFrameSize + x;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const std::size_t other = static_cast<std::size_t>(y + dy) * kFrameSize + x + dx;

            if (film.geometry[other] != film.geometry[pixel] || film.primId[other] != film.primId[pixel] || film.instanceId[other] != film.instanceId[pixel]) {
                return false;
            }
        }
    }

    return true;
}

class NatronUtilityRaster : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        const gpu::GpuStatus s = gputest::createDevice(_device);

        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);

        const std::string missing = GpuTracer::missingRayQuerySupport(*_device);

        if (!missing.empty()) {
            GPUTEST_SKIP_TEST() << missing;
        }
        if (!_device->info().rasterVisibility) {
            GPUTEST_SKIP_TEST() << "the device cannot draw a visibility buffer";
        }
        _registry.reset(new gpu::GpuKernelRegistry(*_device));
        _pool.reset(new gpu::GpuBufferPool(*_device));
        _tracer = std::make_shared<GpuTracer>(*_registry, *_pool);
    }

    void TearDown() override
    {
        if (_device) {
            EXPECT_TRUE(WavefrontGpu::waitForWarmUps(*_device, std::chrono::milliseconds(60000)));
        }
        if (_tracer) {
            const GpuTracer::Outcome outcome = _tracer->status();

            EXPECT_TRUE(outcome.status == GpuTracer::Status::Ok) << outcome.message;
        }
        _tracer.reset();
        if (_device) {
            _device->waitIdle();
        }
        _pool.reset();
        _registry.reset();
        _device.reset();
        _check.expectClean("the test");
    }

    // The ids of each texel, row by row, of one visibility pass over the tracer's scene.
    std::vector<Ids> rasterIds(float time)
    {
        std::vector<Ids> ids;
        std::unique_ptr<gpu::GpuRecordContext> ctx;
        std::unique_ptr<gpu::GpuTexture> colour;
        std::unique_ptr<gpu::GpuTexture> depth;
        gpu::GpuBuffer readback;
        gpu::GpuStatus s = gpu::GpuRecordContext::createRaster(*_registry, ctx, _pool.get());

        EXPECT_TRUE(s.ok()) << s.message;
        if (s.ok()) {
            s = gpu::GpuTexture::createAttachment(*_device, kSize, kSize, VK_FORMAT_R32G32_UINT, colour);
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = gpu::GpuTexture::createDepthAttachment(*_device, kSize, kSize, depth);
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = gpu::GpuBuffer::createStaging(*_device, colour->packedBytes(), gpu::kGpuBufferDefaultUsage, readback);
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = ctx->beginRasterUint(*colour, { Tracer::kNoHit, Tracer::kNoHit, 0u, 0u }, depth.get());
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = _tracer->recordVisibility(*ctx, orthographic(time));
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = ctx->endRaster();
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = ctx->copyToImage(readback);
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            s = ctx->submitAndWait();
            EXPECT_TRUE(s.ok()) << s.message;
        }
        if (s.ok()) {
            const std::uint32_t* texels = static_cast<const std::uint32_t*>(readback.mapped());

            for (std::size_t i = 0; i < std::size_t(kSize) * kSize; ++i) {
                ids.push_back({ texels[2 * i], texels[2 * i + 1] });
            }
        }

        return ids;
    }

    // The ids of a ray down -z through each texel's centre at time, row by row.
    std::vector<Ids> rayIds(float time)
    {
        const std::size_t count = std::size_t(kSize) * kSize;
        std::vector<float> originX, originY, originZ(count, 2.f * kDepthRange);
        std::vector<float> directionX(count, 0.f), directionY(count, 0.f), directionZ(count, -1.f);
        std::vector<float> tMin(count, 0.f), tMax(count, 4.f * kDepthRange), times(count, time);

        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                originX.push_back(worldX(x));
                originY.push_back(worldY(y));
            }
        }

        Tracer::RayBatch rays;

        rays.originX = originX.data();
        rays.originY = originY.data();
        rays.originZ = originZ.data();
        rays.directionX = directionX.data();
        rays.directionY = directionY.data();
        rays.directionZ = directionZ.data();
        rays.tMin = tMin.data();
        rays.tMax = tMax.data();
        rays.time = times.data();
        rays.count = count;

        std::vector<float> t(count), u(count), v(count);
        std::vector<std::uint32_t> geometry(count), instance(count), primitive(count);
        Tracer::HitBatch hits;

        hits.t = t.data();
        hits.u = u.data();
        hits.v = v.data();
        hits.geometry = geometry.data();
        hits.instance = instance.data();
        hits.primitive = primitive.data();
        _tracer->intersect(rays, hits);

        std::vector<Ids> ids(count);

        for (std::size_t i = 0; i < count; ++i) {
            ids[i] = { instance[i], primitive[i] };
        }

        return ids;
    }

    // Compares the raster ids at time with the rays', away from silhouettes, where the raster's top-left rule
    // and the ray's watertight test may give a texel centre on the edge to either side. With silhouettesOnly
    // the edges between one instance's triangles are compared too; without, they are also skipped, for shapes
    // whose shared edges pass exactly through texel centres. Fails unless at least minCompared texels hit
    // something.
    void expectRaysAgree(float time,
                         std::size_t minCompared,
                         bool silhouettesOnly = false)
    {
        const std::vector<Ids> raster = rasterIds(time);
        const std::vector<Ids> rays = rayIds(time);

        ASSERT_EQ(rays.size(), raster.size());

        std::size_t compared = 0;
        std::size_t covered = 0;
        std::size_t mismatches = 0;
        const int neighbours[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        const auto differs = [silhouettesOnly](const Ids& a, const Ids& b) { return silhouettesOnly ? a[0] != b[0] : a != b; };

        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const std::size_t i = std::size_t(y) * kSize + x;
                bool edge = false;

                for (const auto& step : neighbours) {
                    const int nx = int(x) + step[0];
                    const int ny = int(y) + step[1];

                    if (nx >= 0 && ny >= 0 && nx < int(kSize) && ny < int(kSize) && differs(rays[std::size_t(ny) * kSize + nx], rays[i])) {
                        edge = true;
                    }
                }
                if (edge) {
                    continue;
                }
                ++compared;
                if (rays[i][0] != Tracer::kNoHit) {
                    ++covered;
                }
                if (raster[i] != rays[i]) {
                    if (++mismatches <= 8) {
                        ADD_FAILURE() << "texel (" << x << ", " << y << "): raster " << raster[i][0] << "/" << raster[i][1] << ", ray " << rays[i][0] << "/" << rays[i][1];
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches) << "of " << compared << " texels away from edges";
        EXPECT_GE(covered, minCompared);
    }

    // Draws one sample of every pixel.
    bool renderFrame(WavefrontGpu& executor,
                     const UtilityScene& scene,
                     const WavefrontFrame& frame,
                     AovFilm& film)
    {
        const std::atomic<bool> stop(false);
        const bool ok = executor.render(scene.snapshot(), frame, film, stop, 2);

        EXPECT_TRUE(ok) << executor.status().message;

        return ok;
    }

    // Draws every pass of a frame in Raster mode, as the render pass does.
    bool drawPasses(WavefrontGpu& executor,
                    const UtilityScene& scene,
                    const WavefrontFrame& frame,
                    const WavefrontMotion& motion,
                    AovFilm& film)
    {
        const std::atomic<bool> stop(false);
        const bool ok = wavefrontDrawSamples(executor, scene.snapshot(), frame, motion, film, stop, 2, std::function<void(std::size_t)>());

        EXPECT_TRUE(ok) << executor.status().message;
        EXPECT_EQ(frame.rasterSamples, film.passes);

        return ok;
    }

    gputest::ValidationCheck _check;
    std::unique_ptr<gpu::GpuDevice> _device;
    std::unique_ptr<gpu::GpuKernelRegistry> _registry;
    std::unique_ptr<gpu::GpuBufferPool> _pool;
    std::shared_ptr<GpuTracer> _tracer;
};
} // anon namespace

// Three turned cubes that hide parts of one another: the nearest surface wins whatever the draw order, and
// the ids are those a ray through each texel centre reports.
GPU_TEST_F(NatronUtilityRaster, StaticCubesMatchTheRaysIds)
{
    UtilityScene scene;
    const UtilityScene::GeometryId cube = scene.addGeometry(SdfPath("/cube"), 1);
    const UtilityScene::GeometryId other = scene.addGeometry(SdfPath("/other"), 2);

    addCube(scene, cube);
    addCube(scene, other);
    scene.setInstances(cube, { placed(GfVec3d(1., 1., 0.), 35., GfVec3d(-1., -0.5, 0.)), placed(GfVec3d(0., 1., 1.), 60., GfVec3d(0.2, 1.2, -1.5)) });
    scene.setInstances(other, { placed(GfVec3d(1., 0., 1.), 20., GfVec3d(0.8, 0.3, 1.)) });
    ASSERT_TRUE(_tracer->update(scene.snapshot(), 2));

    std::shared_ptr<const GpuTracer::RasterScene> raster;
    const gpu::GpuStatus s = _tracer->rasterScene(raster);

    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_NE(nullptr, raster.get());
    ASSERT_EQ(2u, raster->geometries.size());
    EXPECT_EQ(3u, raster->geometries[0].instanceCount + raster->geometries[1].instanceCount);
    EXPECT_EQ(12u, raster->geometries[0].triangleCount);

    expectRaysAgree(0.5f, kSize * kSize / 8, true);
}

// At mid-shutter a quad that moves 4 to the right over it sits 2 along, which a ray at that time also sees.
GPU_TEST_F(NatronUtilityRaster, ATranslatingQuadSitsAtItsInterpolatedPosition)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/quad"), 1);

    scene.setTopology(id, quadPoints(), quadTriangles());
    moveInstance(scene, id, translation(-2., 0., 0.), translation(2., 0., 0.));
    ASSERT_TRUE(_tracer->update(UtilityScene::atShutter(scene.snapshot(), 0., 1., 1), 2));

    const std::vector<Ids> ids = rasterIds(0.5f);

    ASSERT_EQ(std::size_t(kSize) * kSize, ids.size());

    std::size_t inside = 0;
    std::size_t atOpen = 0;
    std::size_t elsewhere = 0;

    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool hit = ids[std::size_t(y) * kSize + x][0] == 0u;
            const float wx = worldX(x);
            const float wy = worldY(y);

            if (wx > 0.f && wx < 1.f && wy > 0.f && wy < 1.f) {
                inside += hit ? 1 : 0;
            } else if (hit) {
                ++(wx > -2.f && wx < -1.f && wy > 0.f && wy < 1.f ? atOpen : elsewhere);
            }
        }
    }
    // The quad covers 6 by 6 texel centres there.
    EXPECT_EQ(36u, inside);
    EXPECT_EQ(0u, atOpen) << "the quad has left where the shutter opened";
    EXPECT_EQ(0u, elsewhere);
    expectRaysAgree(0.5f, 36);
}

// Rotation is slerped: half-way through a quarter turn the quad has turned 45 degrees and kept its size,
// where blending the matrices would have shrunk it.
GPU_TEST_F(NatronUtilityRaster, ASpinningQuadMatchesTheRaysAtItsTime)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/quad"), 1);
    VtVec3fArray points = quadPoints();

    for (GfVec3f& point : points) {
        point = point * 3.f;
    }
    scene.setTopology(id, points, quadTriangles());
    moveInstance(scene, id, spin(0.), spin(90.));
    ASSERT_TRUE(_tracer->update(UtilityScene::atShutter(scene.snapshot(), 0., 1., 1), 2));
    for (const float time : { 0.f, 0.5f, 0.8f }) {
        SCOPED_TRACE(time);
        expectRaysAgree(time, 100);
    }
}

// One pass at the pixel centres and mid-shutter rebuilds the hits a ray query finds: away from the edges the
// colour, through the barycentrics, and the depth agree, and every id is the same.
GPU_TEST_F(NatronUtilityRaster, OnePassMatchesRayModeOnACardAndACube)
{
    UtilityScene scene;

    cardAndCube(scene);

    WavefrontGpu executor(*_registry, *_pool, std::make_shared<int>(0));
    WavefrontFrame frame = perspectiveFrame();
    AovFilm rays;
    AovFilm raster;

    ASSERT_TRUE(renderFrame(executor, scene, frame, rays));
    frame.raster = true;
    frame.rasterPass = WavefrontRasterPass();
    ASSERT_TRUE(renderFrame(executor, scene, frame, raster));

    int compared = 0;
    int idMismatches = 0;
    std::map<std::uint32_t, int> seen;

    for (int y = 0; y < kFrameSize; ++y) {
        for (int x = 0; x < kFrameSize; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * kFrameSize + x;

            if (!awayFromEdges(rays, x, y)) {
                continue;
            }
            ++compared;
            if (raster.geometry[pixel] != rays.geometry[pixel] || raster.primId[pixel] != rays.primId[pixel] || raster.instanceId[pixel] != rays.instanceId[pixel]) {
                if (++idMismatches <= 8) {
                    ADD_FAILURE() << "pixel (" << x << ", " << y << "): raster ids " << raster.geometry[pixel] << "/" << raster.primId[pixel] << "/" << raster.instanceId[pixel]
                                  << ", ray ids " << rays.geometry[pixel] << "/" << rays.primId[pixel] << "/" << rays.instanceId[pixel];
                }
                continue;
            }
            if (rays.geometry[pixel] == Tracer::kNoHit) {
                continue;
            }
            ++seen[rays.geometry[pixel]];

            const GfVec4f a = raster.filtered(pixel);
            const GfVec4f b = rays.filtered(pixel);

            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(b[c], a[c], 1e-4f) << "colour " << c << " of pixel (" << x << ", " << y << ")";
            }
            EXPECT_NEAR(rays.depth[pixel], raster.depth[pixel], 1e-4f * std::max(1.f, std::fabs(rays.depth[pixel]))) << "depth of pixel (" << x << ", " << y << ")";
        }
    }
    EXPECT_EQ(0, idMismatches) << "of " << compared << " pixels away from edges";
    ASSERT_EQ(2u, seen.size());
    for (const auto& [geometry, pixels] : seen) {
        EXPECT_GE(pixels, 20) << "pixels of geometry " << geometry;
    }
}

// A stripe that moves one pixel over the shutter smears into a box: with the pixel filter narrowed to its
// centre, each pixel is white for the part of the shutter the stripe covers its centre, which N passes at
// the centres of N equal spans of the shutter find to within one span.
GPU_TEST_F(NatronUtilityRaster, AStripeMovingOnePixelSmearsIntoABox)
{
    const int passes = 16;
    const double from = -0.25;
    UtilityScene scene;

    movingStripe(scene, from, 1.);

    WavefrontGpu executor(*_registry, *_pool, std::make_shared<int>(0));
    WavefrontFrame frame = pixelFrame();
    AovFilm film;

    frame.raster = true;
    frame.rasterSamples = passes;
    frame.sampling.filterWidth = 1e-3f;
    ASSERT_TRUE(drawPasses(executor, scene, frame, openShutter(), film));

    const int row = kFrameSize / 2;
    double total = 0.;

    for (int x = 0; x < kFrameSize; ++x) {
        const double centre = x - kFrameSize / 2. + 0.5;
        const double expected = std::max(0., std::min(1., centre - from) - std::max(0., centre - from - 1.));
        const float got = film.filtered(static_cast<std::size_t>(row) * kFrameSize + x)[0];

        EXPECT_NEAR(expected, got, 1. / passes + 1e-3) << "pixel " << x;
        total += got;
    }
    EXPECT_NEAR(1., total, 1. / passes);
}

// A closed shutter draws every pass at the frame's own time, which is the scene without its motion.
GPU_TEST_F(NatronUtilityRaster, AClosedShutterEqualsTheStaticRender)
{
    UtilityScene moving;
    UtilityScene still;

    movingStripe(moving, -0.25, 3.);
    movingStripe(still, -0.25, 0.);

    WavefrontGpu executor(*_registry, *_pool, std::make_shared<int>(0));
    WavefrontFrame frame = pixelFrame();
    AovFilm closed;
    AovFilm reference;

    frame.raster = true;
    frame.rasterSamples = 4;
    ASSERT_TRUE(drawPasses(executor, moving, frame, WavefrontMotion(), closed));
    ASSERT_TRUE(drawPasses(executor, still, frame, WavefrontMotion(), reference));
    for (std::size_t pixel = 0; pixel < closed.color.size(); ++pixel) {
        for (int c = 0; c < 4; ++c) {
            ASSERT_NEAR(reference.filtered(pixel)[c], closed.filtered(pixel)[c], 1e-6f) << "pixel " << pixel;
        }
    }
}

// Tiles of a few pixels draw the same frame as one tile does.
GPU_TEST_F(NatronUtilityRaster, SmallTilesEqualOneTile)
{
    UtilityScene scene;

    cardAndCube(scene);

    WavefrontGpu executor(*_registry, *_pool, std::make_shared<int>(0));
    WavefrontFrame frame = perspectiveFrame();
    AovFilm whole;
    AovFilm tiled;

    frame.raster = true;
    frame.rasterSamples = 4;
    ASSERT_TRUE(drawPasses(executor, scene, frame, openShutter(), whole));
    executor.limitRasterTile(7);
    ASSERT_TRUE(drawPasses(executor, scene, frame, openShutter(), tiled));
    for (std::size_t pixel = 0; pixel < whole.color.size(); ++pixel) {
        ASSERT_EQ(whole.geometry[pixel], tiled.geometry[pixel]) << "pixel " << pixel;
        ASSERT_EQ(whole.primId[pixel], tiled.primId[pixel]) << "pixel " << pixel;
        for (int c = 0; c < 4; ++c) {
            ASSERT_NEAR(whole.filtered(pixel)[c], tiled.filtered(pixel)[c], 1e-5f) << "pixel " << pixel;
        }
    }
}

#endif // NATRON_HAVE_GPU

#ifdef NATRON_HAVE_GPU
TEST(NatronUtilityRasterSupport, NamesWhatTheGpuLacks)
{
    gpu::GpuDeviceInfo info;

    info.rasterVisibility = true;
    info.rasterFloatBlend = true;
    EXPECT_EQ(std::string(), WavefrontGpu::missingRasterSupport(info));

    info.rasterFloatBlend = false;
    EXPECT_NE(std::string::npos, WavefrontGpu::missingRasterSupport(info).find("blend float attachments"));

    info.rasterVisibility = false;
    EXPECT_NE(std::string::npos, WavefrontGpu::missingRasterSupport(info).find("visibility buffers"));
}
#endif // NATRON_HAVE_GPU

namespace {

const int kModeSize = 16;

// Renders a lit card through the engine the node drives the delegate with, in one render mode.
class ModeRender {
public:
    ModeRender(const char* device,
               const char* mode,
               const char* projection)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString("#usda 1.0\n"
                                             "def Mesh \"Card\" {\n"
                                             "    int[] faceVertexCounts = [4]\n"
                                             "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                             "    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n"
                                             "    color3f[] primvars:displayColor = [(1, 0.5, 0.25)] (interpolation = \"constant\")\n"
                                             "}\n"
                                             "def DistantLight \"Light\" {\n"
                                             "    float inputs:intensity = 1\n"
                                             "}\n"));
        _stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color }));
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(device)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingRenderMode), VtValue(TfToken(mode)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingRasterSamples), VtValue(4));
        _engine->SetRendererSetting(TfToken(kUtilitySettingProjection), VtValue(TfToken(projection)));
        _engine->SetRendererSetting(TfToken(kUtilitySettingPixelSamples), VtValue(1));
        _engine->SetRendererSetting(TfToken(kUtilitySettingSubdivLevel), VtValue(0));
        _engine->SetRendererSetting(TfToken(kUtilitySettingMaxRayDepth), VtValue(0));
    }

    void render()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(180);

        renderOnce();
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderOnce();
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    std::vector<GfVec4f> pixels() const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no colour buffer";

            return {};
        }

        const GfVec4f* mapped = static_cast<const GfVec4f*>(buffer->Map());
        std::vector<GfVec4f> copy(mapped, mapped + kModeSize * kModeSize);

        buffer->Unmap();

        return copy;
    }

    std::string warning() const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), kUtilityStatWarning, VtDefault = std::string());
    }

private:
    void renderOnce()
    {
        GfFrustum camera;

        camera.SetPerspective(45., 1., 0.1, 100.);
        camera.SetPosition(GfVec3d(0., 0., 4.));
        _engine->SetRenderBufferSize(GfVec2i(kModeSize, kModeSize));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kModeSize, kModeSize)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

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

// A Raster frame that cannot rasterize is the Ray frame, warned about with the reason.
void
expectRayWithWarning(const char* device,
                     const char* projection,
                     const char* reason)
{
    ModeRender ray(device, kUtilityRenderModeRay, projection);
    ModeRender raster(device, kUtilityRenderModeRaster, projection);

    ray.render();
    raster.render();

    const std::vector<GfVec4f> rayPixels = ray.pixels();
    const std::vector<GfVec4f> rasterPixels = raster.pixels();
    float coverage = 0.f;

    ASSERT_EQ(rayPixels.size(), rasterPixels.size());
    for (std::size_t i = 0; i < rayPixels.size(); ++i) {
        coverage += rayPixels[i][3];
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(rayPixels[i][c], rasterPixels[i][c], 1e-6f) << "pixel " << i % kModeSize << ", " << i / kModeSize;
        }
    }
    EXPECT_GT(coverage, 0.f);
    EXPECT_EQ(std::string::npos, ray.warning().find("Raster")) << ray.warning();

    const std::string warning = raster.warning();

    EXPECT_NE(std::string::npos, warning.find("rendered in Ray mode")) << warning;
    EXPECT_NE(std::string::npos, warning.find(reason)) << warning;
}

} // namespace

TEST(NatronUtilityRasterFallback, OnTheCpuRendersAsRayAndWarns)
{
    expectRayWithWarning(kUtilityDeviceCpu, kUtilityProjectionCamera, "Device is CPU");
}

TEST(NatronUtilityRasterFallback, ASphericalProjectionRendersAsRayAndWarns)
{
    expectRayWithWarning(kUtilityDeviceCpu, kUtilityProjectionSpherical, "Spherical projection");
}

TEST(NatronUtilityRasterFallback, ACylindricalProjectionOnAutoRendersAsRayAndWarns)
{
    expectRayWithWarning(kUtilityDeviceAuto, kUtilityProjectionCylindrical, "Cylindrical projection");
}
