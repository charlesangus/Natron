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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/ray.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/usd/sdf/path.h>

#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const double kDirectionTolerance = 1e-5;

WavefrontFrame
frameOf(const GfFrustum& frustum,
        int width,
        int height)
{
    WavefrontFrame frame;

    frame.worldToView = frustum.ComputeViewMatrix();
    frame.projection = frustum.ComputeProjectionMatrix();
    frame.window.width = width;
    frame.window.height = height;

    return frame;
}

GfFrustum
offCentreCamera()
{
    GfFrustum frustum;

    frustum.SetPerspective(45., 1.5, 0.5, 200.);
    frustum.SetPosition(GfVec3d(3., -2., 30.));
    frustum.SetRotation(GfRotation(GfVec3d(0., 1., 0.), 20.));

    return frustum;
}

struct RayAt {
    GfVec3d origin;
    GfVec3d direction;
    float tMin;
    float tMax;
};

RayAt
readRay(const std::vector<float>& rays,
        std::size_t count,
        std::size_t index)
{
    return RayAt {
        GfVec3d(rays[index], rays[count + index], rays[2 * count + index]),
        GfVec3d(rays[3 * count + index], rays[4 * count + index], rays[5 * count + index]),
        rays[6 * count + index],
        rays[7 * count + index],
    };
}

void
expectRayMatchesFrustum(const RayAt& ray,
                        const GfFrustum& frustum,
                        double ndcX,
                        double ndcY)
{
    const GfVec3d expected = frustum.ComputeRay(GfVec2d(ndcX, ndcY)).GetDirection().GetNormalized();

    EXPECT_NEAR(expected[0], ray.direction[0], kDirectionTolerance) << ndcX << " " << ndcY;
    EXPECT_NEAR(expected[1], ray.direction[1], kDirectionTolerance) << ndcX << " " << ndcY;
    EXPECT_NEAR(expected[2], ray.direction[2], kDirectionTolerance) << ndcX << " " << ndcY;
    EXPECT_NEAR(1., ray.direction.GetLength(), kDirectionTolerance);
    EXPECT_EQ(0.f, ray.tMin);
}

// One hit to shade, with every table a test does not care about left minimal.
struct ShadeOne {
    explicit ShadeOne(const UtilityScene& scene,
                      std::uint32_t primitive = 0,
                      float u = 0.f,
                      float v = 0.f,
                      float t = 0.f)
    {
        const UtilityScene::Snapshot snapshot = scene.snapshot();
        WavefrontShadeTables tables;
        GfFrustum frustum;

        frustum.SetPerspective(45., 1., 1., 100.);
        tables.update(snapshot);

        const std::vector<float> rays = { 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 10.f };
        const std::vector<float> hitSurface = { t, u, v };
        const std::vector<std::uint32_t> hitIds = { 0u, 0u, primitive };
        KernelShadeBuffers buffers = tables.buffers();

        buffers.rays = rays.data();
        buffers.hitSurface = hitSurface.data();
        buffers.hitIds = hitIds.data();
        buffers.colorOut = color;
        buffers.depthOut = &depth;
        buffers.primIdOut = &primId;
        buffers.instanceIdOut = &instanceId;
        runShadeSurfaceKernel(makeShadeParams(frameOf(frustum, 1, 1), 1), buffers);
    }

    float color[4] = { -1.f, -1.f, -1.f, -1.f };
    float depth = -1.f;
    std::int32_t primId = -1;
    std::int32_t instanceId = -1;
};

UtilityScene::GeometryId
addTriangle(UtilityScene& scene,
            int primId = 5)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/triangle"), primId);

    scene.setTopology(id,
                      VtVec3fArray { GfVec3f(0.f, 0.f, 0.f), GfVec3f(1.f, 0.f, 0.f), GfVec3f(0.f, 1.f, 0.f) },
                      VtVec3iArray { GfVec3i(2, 0, 1) });
    scene.setInstances(id, { GfMatrix4d(1.) });

    return id;
}

// Binds a material that emits the display colour, so a lightless shade shows it.
void
emitDisplayColor(UtilityScene& scene,
                 UtilityScene::GeometryId id)
{
    UtilityMaterialDesc material = UtilityMaterialDesc::fallback();

    material.emissiveFromDisplayColor = true;
    scene.setMaterial(SdfPath("/EmitDisplayColor"), std::make_shared<const UtilityMaterialDesc>(material));
    scene.setMaterialId(id, SdfPath("/EmitDisplayColor"));
}
} // anon namespace

TEST(NatronUtilityKernels, RayGenCornersFollowTheCameraFrustum)
{
    const int size = 4;
    const std::size_t count = size * size;
    const GfFrustum frustum = offCentreCamera();
    const WavefrontFrame frame = frameOf(frustum, size, size);
    std::vector<float> rays(count * 8, std::numeric_limits<float>::quiet_NaN());

    runRayGenKernel(makeRayGenParams(frame, frame.window), rays.data());

    const double edge = 0.75;

    expectRayMatchesFrustum(readRay(rays, count, 0), frustum, -edge, -edge);
    expectRayMatchesFrustum(readRay(rays, count, 3), frustum, edge, -edge);
    expectRayMatchesFrustum(readRay(rays, count, 12), frustum, -edge, edge);
    expectRayMatchesFrustum(readRay(rays, count, 15), frustum, edge, edge);
}

TEST(NatronUtilityKernels, RayGenSpansTheCameraRange)
{
    const int size = 4;
    const std::size_t count = size * size;
    const GfFrustum frustum = offCentreCamera();
    const WavefrontFrame frame = frameOf(frustum, size, size);
    std::vector<float> rays(count * 8);

    runRayGenKernel(makeRayGenParams(frame, frame.window), rays.data());

    const GfMatrix4d worldToView = frustum.ComputeViewMatrix();

    for (std::size_t i = 0; i < count; ++i) {
        const RayAt ray = readRay(rays, count, i);
        const GfVec3d start = worldToView.Transform(ray.origin);
        const GfVec3d end = worldToView.Transform(ray.origin + ray.direction * ray.tMax);

        EXPECT_NEAR(-frustum.GetNearFar().GetMin(), start[2], 1e-3) << i;
        EXPECT_NEAR(-frustum.GetNearFar().GetMax(), end[2], 1e-2) << i;
    }
}

TEST(NatronUtilityKernels, RayGenTileMatchesTheWholeWindow)
{
    const int width = 70;
    const int height = 5;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    const WavefrontFrame frame = frameOf(offCentreCamera(), width, height);
    std::vector<float> whole(count * 8);

    runRayGenKernel(makeRayGenParams(frame, frame.window), whole.data());

    WavefrontRect tile;

    tile.x = 66;
    tile.y = 3;
    tile.width = 4;
    tile.height = 2;

    const std::size_t tileCount = 8;
    std::vector<float> part(tileCount * 8);

    runRayGenKernel(makeRayGenParams(frame, tile), part.data());
    for (std::size_t i = 0; i < tileCount; ++i) {
        const std::size_t x = tile.x + i % 4;
        const std::size_t y = tile.y + i / 4;
        const RayAt fromWhole = readRay(whole, count, y * width + x);
        const RayAt fromPart = readRay(part, tileCount, i);

        EXPECT_EQ(fromWhole.direction, fromPart.direction) << i;
        EXPECT_EQ(fromWhole.origin, fromPart.origin) << i;
    }
}

TEST(NatronUtilityKernels, RayGenFollowsAnOrthographicCamera)
{
    GfFrustum frustum;

    frustum.SetOrthographic(-2., 2., -2., 2., 1., 50.);
    frustum.SetPosition(GfVec3d(0., 0., 10.));

    const int size = 4;
    const std::size_t count = size * size;
    const WavefrontFrame frame = frameOf(frustum, size, size);
    std::vector<float> rays(count * 8);

    runRayGenKernel(makeRayGenParams(frame, frame.window), rays.data());

    const RayAt corner = readRay(rays, count, 0);

    EXPECT_NEAR(-1.5, corner.origin[0], 1e-5);
    EXPECT_NEAR(-1.5, corner.origin[1], 1e-5);
    EXPECT_NEAR(9., corner.origin[2], 1e-5);
    EXPECT_NEAR(0., corner.direction[0], kDirectionTolerance);
    EXPECT_NEAR(0., corner.direction[1], kDirectionTolerance);
    EXPECT_NEAR(-1., corner.direction[2], kDirectionTolerance);
    EXPECT_NEAR(49.f, corner.tMax, 1e-4f);
}

TEST(NatronUtilityKernels, ShadeSurfacePremultipliesAnEmittedConstantColor)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 0.5f, 0.25f, 0.5f) });

    emitDisplayColor(scene, id);

    const ShadeOne shade(scene);

    EXPECT_FLOAT_EQ(0.5f, shade.color[0]);
    EXPECT_FLOAT_EQ(0.25f, shade.color[1]);
    EXPECT_FLOAT_EQ(0.125f, shade.color[2]);
    EXPECT_FLOAT_EQ(0.5f, shade.color[3]);
    EXPECT_EQ(5, shade.primId);
    EXPECT_EQ(0, shade.instanceId);
}

TEST(NatronUtilityKernels, ShadeSurfaceReadsCornerColorsByBarycentrics)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingCorner,
                    VtVec4fArray { GfVec4f(1.f, 0.f, 0.f, 1.f), GfVec4f(0.f, 1.f, 0.f, 1.f), GfVec4f(0.f, 0.f, 1.f, 1.f) });

    emitDisplayColor(scene, id);

    const ShadeOne shade(scene, 0, 0.25f, 0.5f);

    EXPECT_FLOAT_EQ(0.25f, shade.color[0]);
    EXPECT_FLOAT_EQ(0.25f, shade.color[1]);
    EXPECT_FLOAT_EQ(0.5f, shade.color[2]);
}

// The triangle lists its vertices as 2, 0, 1, so vertex colours must go through its indices.
TEST(NatronUtilityKernels, ShadeSurfaceReadsVertexColorsThroughTheTriangle)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingVertex,
                    VtVec4fArray { GfVec4f(1.f, 0.f, 0.f, 1.f), GfVec4f(0.f, 1.f, 0.f, 1.f), GfVec4f(0.f, 0.f, 1.f, 1.f) });

    emitDisplayColor(scene, id);

    const ShadeOne shade(scene, 0, 1.f, 0.f);

    EXPECT_FLOAT_EQ(1.f, shade.color[0]);
    EXPECT_FLOAT_EQ(0.f, shade.color[1]);
    EXPECT_FLOAT_EQ(0.f, shade.color[2]);
}

TEST(NatronUtilityKernels, ShadeSurfaceReadsTriangleColorsByPrimitive)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/two"), 1);

    scene.setTopology(id,
                      VtVec3fArray { GfVec3f(0.f, 0.f, 0.f), GfVec3f(1.f, 0.f, 0.f), GfVec3f(1.f, 1.f, 0.f), GfVec3f(0.f, 1.f, 0.f) },
                      VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    scene.setColors(id, UtilityScene::eColorBindingTriangle,
                    VtVec4fArray { GfVec4f(1.f, 0.f, 0.f, 1.f), GfVec4f(0.f, 1.f, 0.f, 1.f) });

    emitDisplayColor(scene, id);

    const ShadeOne shade(scene, 1);

    EXPECT_FLOAT_EQ(0.f, shade.color[0]);
    EXPECT_FLOAT_EQ(1.f, shade.color[1]);
}

// Colours that no longer cover their binding must not be read past their end.
TEST(NatronUtilityKernels, ShadeTablesFallBackToGreyWhenColorsDoNotFit)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingCorner, VtVec4fArray { GfVec4f(1.f, 0.f, 0.f, 1.f) });
    emitDisplayColor(scene, id);

    const ShadeOne shade(scene);

    EXPECT_FLOAT_EQ(0.18f, shade.color[0]);
    EXPECT_FLOAT_EQ(0.18f, shade.color[1]);
    EXPECT_FLOAT_EQ(0.18f, shade.color[2]);
    EXPECT_FLOAT_EQ(1.f, shade.color[3]);
}

// No light reaches a scene without lights, so a surface that emits nothing shows black at full coverage.
TEST(NatronUtilityKernels, ShadeSurfaceWithoutLightOrEmissionIsOpaqueBlack)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 0.5f, 0.25f, 1.f) });

    const ShadeOne shade(scene);

    EXPECT_EQ(0.f, shade.color[0]);
    EXPECT_EQ(0.f, shade.color[1]);
    EXPECT_EQ(0.f, shade.color[2]);
    EXPECT_EQ(1.f, shade.color[3]);
    EXPECT_EQ(5, shade.primId);
}

TEST(NatronUtilityKernels, ShadeSurfaceDrawsAnUnsupportedMaterialMagenta)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);
    UtilityMaterialDesc material;

    material.kind = UtilityMaterialDesc::eKindUnsupported;
    material.unsupportedReason = "ND_standard_surface_surfaceshader";
    scene.setMaterial(SdfPath("/X"), std::make_shared<const UtilityMaterialDesc>(material));
    scene.setMaterialId(id, SdfPath("/X"));

    const ShadeOne shade(scene);

    EXPECT_EQ(1.f, shade.color[0]);
    EXPECT_EQ(0.f, shade.color[1]);
    EXPECT_EQ(1.f, shade.color[2]);
    EXPECT_EQ(1.f, shade.color[3]);
}

// Near and far are 1 and 100, and the shaded ray runs 10 long from near to far.
TEST(NatronUtilityKernels, ShadeSurfaceDepthIsWindowDepth)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });

    EXPECT_NEAR(0.f, ShadeOne(scene, 0, 0.f, 0.f, 0.f).depth, 1e-5f);
    EXPECT_NEAR(1.f, ShadeOne(scene, 0, 0.f, 0.f, 10.f).depth, 1e-5f);

    const double near = 1.;
    const double far = 100.;
    const double viewZ = near + 0.5 * (far - near);
    const double expected = (far / (far - near)) * (1. - near / viewZ);

    EXPECT_NEAR(expected, ShadeOne(scene, 0, 0.f, 0.f, 5.f).depth, 1e-5);
}

TEST(NatronUtilityKernels, ShadeSurfaceLeavesMissesUntouched)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene);

    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });

    WavefrontShadeTables tables;
    GfFrustum frustum;

    tables.update(scene.snapshot());

    const std::vector<float> rays = { 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 10.f };
    const std::vector<float> hitSurface = { 0.f, 0.f, 0.f };
    const std::vector<std::uint32_t> hitIds = { Tracer::kNoHit, Tracer::kNoHit, Tracer::kNoHit };
    float color[4] = { 9.f, 9.f, 9.f, 9.f };
    float depth = 9.f;
    std::int32_t primId = 9;
    std::int32_t instanceId = 9;
    KernelShadeBuffers buffers = tables.buffers();

    buffers.rays = rays.data();
    buffers.hitSurface = hitSurface.data();
    buffers.hitIds = hitIds.data();
    buffers.colorOut = color;
    buffers.depthOut = &depth;
    buffers.primIdOut = &primId;
    buffers.instanceIdOut = &instanceId;
    runShadeSurfaceKernel(makeShadeParams(frameOf(frustum, 1, 1), 1), buffers);

    EXPECT_EQ(9.f, color[0]);
    EXPECT_EQ(9.f, depth);
    EXPECT_EQ(9, primId);
    EXPECT_EQ(9, instanceId);
}

TEST(NatronUtilityKernels, CameraUsability)
{
    EXPECT_TRUE(wavefrontCameraIsUsable(frameOf(offCentreCamera(), 4, 4)));

    WavefrontFrame singular = frameOf(offCentreCamera(), 4, 4);

    singular.projection.SetDiagonal(0.);
    EXPECT_FALSE(wavefrontCameraIsUsable(singular));
}
