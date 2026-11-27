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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/usd/sdf/path.h>

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuTexture.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
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

    // Compares the raster ids at time with the rays', away from the edges between triangles, where a texel
    // centre on the edge may go either way. Fails unless at least minCompared texels hit something.
    void expectRaysAgree(float time,
                         std::size_t minCompared)
    {
        const std::vector<Ids> raster = rasterIds(time);
        const std::vector<Ids> rays = rayIds(time);

        ASSERT_EQ(rays.size(), raster.size());

        std::size_t compared = 0;
        std::size_t covered = 0;
        std::size_t mismatches = 0;
        const int neighbours[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };

        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const std::size_t i = std::size_t(y) * kSize + x;
                bool edge = false;

                for (const auto& step : neighbours) {
                    const int nx = int(x) + step[0];
                    const int ny = int(y) + step[1];

                    if (nx >= 0 && ny >= 0 && nx < int(kSize) && ny < int(kSize) && rays[std::size_t(ny) * kSize + nx] != rays[i]) {
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

    expectRaysAgree(0.5f, kSize * kSize / 8);
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

#endif // NATRON_HAVE_GPU
