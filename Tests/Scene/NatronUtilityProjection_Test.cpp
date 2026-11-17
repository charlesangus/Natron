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
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
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

const float kCubeHalf = 5.f;

enum Face {
    eFaceNone = -1,
    eFaceFront = 1, // -z, where the camera looks
    eFaceBack,
    eFaceLeft,
    eFaceRight,
    eFaceTop,
    eFaceBottom,
};

void
addFace(UtilityScene& scene,
        const char* path,
        Face face,
        const GfVec3f& centre,
        const GfVec3f& alongA,
        const GfVec3f& alongB)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath(path), face);

    scene.setTopology(id,
                      VtVec3fArray { centre - alongA - alongB, centre + alongA - alongB, centre + alongA + alongB, centre - alongA + alongB },
                      VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
}

// A cube around the origin, each face a prim of its own whose prim id says which.
void
addCube(UtilityScene& scene)
{
    const float h = kCubeHalf;
    const GfVec3f x(h, 0.f, 0.f);
    const GfVec3f y(0.f, h, 0.f);
    const GfVec3f z(0.f, 0.f, h);

    addFace(scene, "/front", eFaceFront, -z, x, y);
    addFace(scene, "/back", eFaceBack, z, x, y);
    addFace(scene, "/left", eFaceLeft, -x, z, y);
    addFace(scene, "/right", eFaceRight, x, z, y);
    addFace(scene, "/top", eFaceTop, y, x, z);
    addFace(scene, "/bottom", eFaceBottom, -y, x, z);
}

// The face a ray from inside the cube along direction ends on, or none when it passes so close to
// an edge that a sample's rounding could pick either face.
Face
faceAlong(const GfVec3d& direction)
{
    const double ax = std::abs(direction[0]);
    const double ay = std::abs(direction[1]);
    const double az = std::abs(direction[2]);
    const double largest = std::max(ax, std::max(ay, az));
    const double second = std::max(std::min(ax, ay), std::min(std::max(ax, ay), az));

    if (largest - second < 0.03 * largest) {
        return eFaceNone;
    }
    if (largest == ax) {
        return direction[0] > 0. ? eFaceRight : eFaceLeft;
    }
    if (largest == ay) {
        return direction[1] > 0. ? eFaceTop : eFaceBottom;
    }

    return direction[2] > 0. ? eFaceBack : eFaceFront;
}

// The camera at the origin with no rotation looks down -z, its right +x and its up +y; u runs from
// behind it on its left, through its forward axis at 0.5, to behind it on its right.
GfVec3d
aroundAt(double u)
{
    const double longitude = 2. * M_PI * (u - 0.5);

    return GfVec3d(std::sin(longitude), 0., -std::cos(longitude));
}

GfVec3d
sphericalDirection(double u,
                   double v)
{
    const double latitude = M_PI * (v - 0.5);

    return aroundAt(u) * std::cos(latitude) + GfVec3d(0., std::sin(latitude), 0.);
}

WavefrontFrame
frameThrough(const GfFrustum& camera,
             int width,
             int height,
             WavefrontMapping mapping)
{
    WavefrontFrame frame;

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();
    frame.window.width = width;
    frame.window.height = height;
    frame.mapping = mapping;
    frame.sampling.samplesPerSide = 1;

    return frame;
}

GfFrustum
perspectiveCamera(double fieldOfViewHeight,
                  double aspect)
{
    GfFrustum camera;

    camera.SetPerspective(fieldOfViewHeight, aspect, 0.1, 100.);
    camera.SetPosition(GfVec3d(0.));

    return camera;
}

// Each pixel's prim id, depth and colour from its one sample, in rows from the bottom.
class ProjectionFilm : public WavefrontFilm {
public:
    ProjectionFilm(int width,
                   int height)
        : _width(width)
        , _primIds(static_cast<std::size_t>(width * height), -1)
        , _depths(static_cast<std::size_t>(width * height), -1.f)
        , _colors(static_cast<std::size_t>(width * height), GfVec4f(0.f))
    {
    }

    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const std::size_t pixel = static_cast<std::size_t>((tile.rect.y + row) * _width + tile.rect.x + column);
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;

                _primIds[pixel] = hit ? tile.primId[index] : -1;
                _depths[pixel] = (hit && tile.depth) ? tile.depth[index] : -1.f;
                if (hit && tile.color) {
                    _colors[pixel] = GfVec4f(tile.color + index * 4);
                }
            }
        }
    }

    int primIdAt(int x,
                 int y) const
    {
        return _primIds[static_cast<std::size_t>(y * _width + x)];
    }

    GfVec4f colorAt(int x,
                    int y) const
    {
        return _colors[static_cast<std::size_t>(y * _width + x)];
    }

private:
    int _width;
    std::vector<int> _primIds;
    std::vector<float> _depths;
    std::vector<GfVec4f> _colors;
};

enum class Backend {
    Cpu,
    Gpu,
};

void
PrintTo(Backend backend,
        std::ostream* os)
{
    *os << (backend == Backend::Cpu ? "Cpu" : "Gpu");
}

class NatronUtilityProjection : public ::testing::TestWithParam<Backend> {
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

    bool draw(const UtilityScene& scene,
              const WavefrontFrame& frame,
              ProjectionFilm& film)
    {
        const std::atomic<bool> stop(false);

        if (!_executor) {
            return false;
        }

        const UtilityScene::Snapshot snapshot = scene.snapshot();
        WavefrontFrame drawn = frame;

        if (drawn.mapping == WavefrontMapping::UvUnwrap) {
            std::string error;

            drawn.unwrap = wavefrontUnwrap(snapshot, drawn, stop, &error);
            if (!drawn.unwrap) {
                ADD_FAILURE() << "the prim did not unwrap: " << error;

                return false;
            }
        }
        if (!_executor->render(snapshot, drawn, film, stop, 2)) {
            ADD_FAILURE() << "the frame did not render";

            return false;
        }
        film.passDrawn();

        return true;
    }

    // How many of the frame's pixels see a face other than the one along their centre's direction.
    template <typename DirectionAt>
    int wrongFaces(const ProjectionFilm& film,
                   int width,
                   int height,
                   DirectionAt directionAt)
    {
        int wrong = 0;
        int checked = 0;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const Face expected = faceAlong(directionAt((x + 0.5) / width, (y + 0.5) / height));

                if (expected == eFaceNone) {
                    continue;
                }
                ++checked;
                if (film.primIdAt(x, y) != expected) {
                    if (wrong == 0) {
                        ADD_FAILURE() << "pixel " << x << ", " << y << " saw " << film.primIdAt(x, y) << " for " << expected;
                    }
                    ++wrong;
                }
            }
        }
        EXPECT_GT(checked, width * height / 2);

        return wrong;
    }

    bool available() const
    {
        return _executor != nullptr;
    }

private:
    WavefrontExecutor* _executor = nullptr;
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

// The lat-long covers the whole sphere: forward at its centre, behind at both side edges, the
// camera's left and right a quarter in, its up along the top row and its down along the bottom.
TEST_P(NatronUtilityProjection, ASphericalFrameShowsEachCubeFaceWhereTheLatLongPutsIt)
{
    if (!available()) {
        return;
    }
    const int width = 64;
    const int height = 32;
    UtilityScene scene;

    addCube(scene);

    ProjectionFilm film(width, height);

    ASSERT_TRUE(draw(scene, frameThrough(perspectiveCamera(60., 1.), width, height, WavefrontMapping::Spherical), film));
    EXPECT_EQ(eFaceFront, film.primIdAt(width / 2, height / 2));
    EXPECT_EQ(eFaceBack, film.primIdAt(0, height / 2));
    EXPECT_EQ(eFaceBack, film.primIdAt(width - 1, height / 2));
    EXPECT_EQ(eFaceLeft, film.primIdAt(width / 4, height / 2));
    EXPECT_EQ(eFaceRight, film.primIdAt(3 * width / 4, height / 2));
    EXPECT_EQ(eFaceTop, film.primIdAt(width / 2, height - 1));
    EXPECT_EQ(eFaceBottom, film.primIdAt(width / 2, 0));
    EXPECT_EQ(0, wrongFaces(film, width, height, sphericalDirection));
}

// A camera turned to look down +x centres the lat-long on +x, so the projection follows the camera.
TEST_P(NatronUtilityProjection, ASphericalFrameFollowsTheCamerasOrientation)
{
    if (!available()) {
        return;
    }
    const int width = 64;
    const int height = 32;
    UtilityScene scene;
    GfFrustum camera = perspectiveCamera(60., 1.);

    addCube(scene);
    camera.SetRotation(GfRotation(GfVec3d(0., 1., 0.), -90.));

    ProjectionFilm film(width, height);

    ASSERT_TRUE(draw(scene, frameThrough(camera, width, height, WavefrontMapping::Spherical), film));
    EXPECT_EQ(eFaceRight, film.primIdAt(width / 2, height / 2));
    EXPECT_EQ(eFaceLeft, film.primIdAt(0, height / 2));
    EXPECT_EQ(eFaceFront, film.primIdAt(width / 4, height / 2));
    EXPECT_EQ(eFaceBack, film.primIdAt(3 * width / 4, height / 2));
}

// Around, the cylinder wraps the full turn as the lat-long does; up, it spans the camera's
// vertical field of view, here 90 degrees, so its top and bottom rows look 45 degrees up and down.
TEST_P(NatronUtilityProjection, ACylindricalFrameWrapsTheFullTurnWithinTheCamerasVerticalView)
{
    if (!available()) {
        return;
    }
    const int width = 64;
    const int height = 32;
    UtilityScene scene;

    addCube(scene);

    ProjectionFilm film(width, height);

    ASSERT_TRUE(draw(scene, frameThrough(perspectiveCamera(90., 2.), width, height, WavefrontMapping::Cylindrical), film));
    EXPECT_EQ(eFaceFront, film.primIdAt(width / 2, height / 2));
    EXPECT_EQ(eFaceBack, film.primIdAt(0, height / 2));
    EXPECT_EQ(eFaceBack, film.primIdAt(width - 1, height / 2));
    EXPECT_EQ(eFaceLeft, film.primIdAt(width / 4, height / 2));
    EXPECT_EQ(eFaceRight, film.primIdAt(3 * width / 4, height / 2));

    const auto cylindrical = [](double u, double v) {
        return aroundAt(u) + GfVec3d(0., 2. * v - 1., 0.);
    };

    EXPECT_EQ(0, wrongFaces(film, width, height, cylindrical));
}

// An orthographic camera's cylinder is a ring of horizontal rays, raised by its vertical aperture,
// so even its top row never meets the top face.
TEST_P(NatronUtilityProjection, AnOrthographicCylindricalFrameLooksLevelAtEveryRow)
{
    if (!available()) {
        return;
    }
    const int width = 64;
    const int height = 16;
    UtilityScene scene;
    GfFrustum camera;

    addCube(scene);
    camera.SetOrthographic(-8., 8., -2., 2., 0.1, 100.);
    camera.SetPosition(GfVec3d(0.));

    ProjectionFilm film(width, height);

    ASSERT_TRUE(draw(scene, frameThrough(camera, width, height, WavefrontMapping::Cylindrical), film));
    for (int y = 0; y < height; ++y) {
        EXPECT_EQ(eFaceFront, film.primIdAt(width / 2, y)) << "row " << y;
        EXPECT_EQ(eFaceBack, film.primIdAt(0, y)) << "row " << y;
    }

    const auto level = [](double u, double) {
        return aroundAt(u);
    };

    EXPECT_EQ(0, wrongFaces(film, width, height, level));
}

// The display window spans the projection and overscan lies beyond it: a spherical frame with 16
// pixels on each side wraps on past behind the camera, and its display sees what the frame without
// them does.
TEST_P(NatronUtilityProjection, SphericalOverscanWrapsPastTheDisplayWindow)
{
    if (!available()) {
        return;
    }
    const int width = 64;
    const int height = 32;
    const int overscan = 16;
    UtilityScene scene;

    addCube(scene);

    ProjectionFilm plain(width, height);
    ProjectionFilm wide(width + 2 * overscan, height + 2 * overscan);
    WavefrontFrame frame = frameThrough(perspectiveCamera(60., 1.), width + 2 * overscan, height + 2 * overscan, WavefrontMapping::Spherical);

    frame.display.x = overscan;
    frame.display.y = overscan;
    frame.display.width = width;
    frame.display.height = height;
    ASSERT_TRUE(draw(scene, frameThrough(perspectiveCamera(60., 1.), width, height, WavefrontMapping::Spherical), plain));
    ASSERT_TRUE(draw(scene, frame, wide));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            EXPECT_EQ(plain.primIdAt(x, y), wide.primIdAt(x + overscan, y + overscan)) << "pixel " << x << ", " << y;
        }
    }
    // A quarter turn past behind the camera, the overscan's outer columns see the side faces the
    // display shows a quarter of the way in from the opposite edge.
    EXPECT_EQ(eFaceRight, wide.primIdAt(0, overscan + height / 2));
    EXPECT_EQ(eFaceLeft, wide.primIdAt(width + 2 * overscan - 1, overscan + height / 2));
}

namespace {

const int kBakeSize = 32;
const float kBakeHalf = 4.f;
const int kBakePrimId = 7;

// A quad facing +z, its colours a gradient over its corners, laid out over UV tile uOffset + 1001 so
// that its corners take the tile's.
void
addBakedQuad(UtilityScene& scene,
             float uOffset,
             bool withCoordinates = true)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/quad"), kBakePrimId);
    const float h = kBakeHalf;

    scene.setTopology(id,
                      VtVec3fArray { GfVec3f(-h, -h, 0.f), GfVec3f(h, -h, 0.f), GfVec3f(h, h, 0.f), GfVec3f(-h, h, 0.f) },
                      VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingVertex,
                    VtVec4fArray { GfVec4f(1.f, 0.2f, 0.1f, 1.f), GfVec4f(0.1f, 1.f, 0.2f, 1.f), GfVec4f(0.2f, 0.1f, 1.f, 1.f), GfVec4f(1.f, 1.f, 0.3f, 1.f) });
    if (withCoordinates) {
        scene.setUnwrapSt(id, true, UtilityScene::eColorBindingVertex,
                          VtVec2fArray { GfVec2f(uOffset, 0.f), GfVec2f(uOffset + 1.f, 0.f), GfVec2f(uOffset + 1.f, 1.f), GfVec2f(uOffset, 1.f) });
    }
    scene.setInstances(id, { GfMatrix4d(1.) });

    std::shared_ptr<UtilityLightDesc> light = std::make_shared<UtilityLightDesc>();

    light->type = UtilityLightDesc::eTypeSphere;
    light->treatAsPoint = true;
    light->emission = GfVec3f(40.f);
    light->transform = GfMatrix4d(1.).SetTranslate(GfVec3d(2., 1., 3.));
    scene.setLight(SdfPath("/light"), light);
}

// Looks straight down at the quad, its frame exactly the quad, a pixel per texel of the bake.
GfFrustum
bakeCamera()
{
    GfFrustum camera;

    camera.SetOrthographic(-kBakeHalf, kBakeHalf, -kBakeHalf, kBakeHalf, 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., 10.));

    return camera;
}

WavefrontFrame
bakeFrame(int tile)
{
    WavefrontFrame frame = frameThrough(bakeCamera(), kBakeSize, kBakeSize, WavefrontMapping::UvUnwrap);

    frame.unwrapPrim = SdfPath("/quad");
    frame.unwrapPrimvar = "uv";
    frame.udimTile = tile;

    return frame;
}

int
hitsIn(const ProjectionFilm& film)
{
    int hits = 0;

    for (int y = 0; y < kBakeSize; ++y) {
        for (int x = 0; x < kBakeSize; ++x) {
            hits += film.primIdAt(x, y) == kBakePrimId ? 1 : 0;
        }
    }

    return hits;
}
} // anon namespace

// A quad whose UVs span its tile bakes, texel for texel, what a camera square on to it sees.
TEST_P(NatronUtilityProjection, AUvUnwrapBakesWhatAHeadOnCameraSees)
{
    UtilityScene scene;

    addBakedQuad(scene, 0.f);

    ProjectionFilm baked(kBakeSize, kBakeSize);
    ProjectionFilm seen(kBakeSize, kBakeSize);

    if (!draw(scene, bakeFrame(1001), baked) || !draw(scene, frameThrough(bakeCamera(), kBakeSize, kBakeSize, WavefrontMapping::Camera), seen)) {
        return;
    }
    EXPECT_EQ(kBakeSize * kBakeSize, hitsIn(baked));
    EXPECT_EQ(kBakeSize * kBakeSize, hitsIn(seen));

    int differing = 0;

    for (int y = 0; y < kBakeSize; ++y) {
        for (int x = 0; x < kBakeSize; ++x) {
            const GfVec4f a = baked.colorAt(x, y);
            const GfVec4f b = seen.colorAt(x, y);

            for (int c = 0; c < 4; ++c) {
                if (std::abs(a[c] - b[c]) > 1e-3f * std::max(1.f, std::abs(b[c]))) {
                    if (differing == 0) {
                        ADD_FAILURE() << "texel " << x << ", " << y << " channel " << c << " baked " << a[c] << " where the camera saw " << b[c];
                    }
                    ++differing;
                }
            }
        }
    }
    EXPECT_EQ(0, differing);
    EXPECT_GT((baked.colorAt(kBakeSize - 1, kBakeSize - 1) - baked.colorAt(0, 0)).GetLength(), 0.1f) << "the shading should vary over the quad";
}

// A quad laid out in the second tile fills it and leaves the first empty.
TEST_P(NatronUtilityProjection, AUvUnwrapBakesOnlyTheUdimTileItNames)
{
    UtilityScene scene;

    addBakedQuad(scene, 1.f);

    ProjectionFilm first(kBakeSize, kBakeSize);
    ProjectionFilm second(kBakeSize, kBakeSize);

    if (!draw(scene, bakeFrame(1001), first) || !draw(scene, bakeFrame(1002), second)) {
        return;
    }
    EXPECT_EQ(0, hitsIn(first));
    EXPECT_EQ(kBakeSize * kBakeSize, hitsIn(second));
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityProjection, ::testing::Values(Backend::Cpu, Backend::Gpu));

namespace {

const int kHostWidth = 48;
const int kHostHeight = 32;
const int kHostOverscan = 16;

void
defineQuad(const UsdStageRefPtr& stage,
           const char* path,
           const GfVec3f& centre,
           float halfWidth,
           float halfHeight)
{
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(path));
    const GfVec3f a(halfWidth, 0.f, 0.f);
    const GfVec3f b(0.f, halfHeight, 0.f);

    mesh.CreatePointsAttr(VtValue(VtVec3fArray { centre - a - b, centre + a - b, centre + a + b, centre - a + b }));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray { 4 }));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray { 0, 1, 2, 3 }));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
}

// A backdrop wider than the frame, a box in its middle, and a box outside the frame on the left
// that only overscan reaches.
SdfLayerRefPtr
overscanLayer()
{
    SdfLayerRefPtr layer = SceneOps::createNodeLayer("NatronUtilityProjectionOverscan");
    UsdStageRefPtr stage = UsdStage::Open(layer);

    defineQuad(stage, "/Backdrop", GfVec3f(0.f, 0.f, -20.f), 40.f, 40.f);
    defineQuad(stage, "/Centre", GfVec3f(0.3f, 0.2f, -10.f), 2.f, 1.5f);
    defineQuad(stage, "/Outside", GfVec3f(-4.f, 0.f, -10.f), 0.4f, 3.f);

    return layer;
}

// The frustum frames kHostWidth pixels across 6 world units at the boxes, 8 pixels a unit, so the
// overscan reaches 2 units past each side there.
GfFrustum
overscanCamera()
{
    GfFrustum camera;

    camera.SetPerspective(2. * std::atan(3. / 10.) * 180. / M_PI, false, static_cast<double>(kHostWidth) / kHostHeight, 1., 100.);
    camera.SetPosition(GfVec3d(0.));

    return camera;
}

bool
renderThroughTheHost(const void* owner,
                     int overscan,
                     HydraRender::Result* result,
                     std::string* error)
{
    HydraRender::Request request;

    request.owner = owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), overscanLayer(), 1);
    request.time = UsdTimeCode(1.);
    request.width = kHostWidth + 2 * overscan;
    request.height = kHostHeight + 2 * overscan;
    request.overscanLeft = overscan;
    request.overscanBottom = overscan;
    request.overscanRight = overscan;
    request.overscanTop = overscan;
    request.renderer = kRenderDelegateUtilityId;
    request.frustum = overscanCamera();
    request.settings[kUtilitySettingDevice] = VtValue(TfToken(kUtilityDeviceCpu));
    request.settings[kUtilitySettingPixelSamples] = VtValue(1);

    return HydraRender::render(request, result, error);
}

std::string
pathAt(const HydraRender::Result& result,
       int width,
       int x,
       int y)
{
    const float id = result.primId[static_cast<std::size_t>(y * width + x)];

    for (const auto& entry : result.primPaths) {
        if (HydraRender::hashToFloat(entry.first) == id) {
            return entry.second;
        }
    }

    return std::string();
}
} // anon namespace

// Overscan widens the frame by its pixels on every side and leaves the frame within it as it was:
// the camera still frames the same view there, and the overscan sees past it.
TEST(NatronUtilityProjectionHost, OverscanGrowsTheFrameAndKeepsItsCentre)
{
    const int owner = 0;
    HydraRender::Result plain;
    HydraRender::Result wide;
    std::string error;

    ASSERT_TRUE(renderThroughTheHost(&owner, 0, &plain, &error)) << error;
    ASSERT_TRUE(renderThroughTheHost(&owner, kHostOverscan, &wide, &error)) << error;

    const int wideWidth = kHostWidth + 2 * kHostOverscan;
    const int wideHeight = kHostHeight + 2 * kHostOverscan;

    ASSERT_EQ(static_cast<std::size_t>(kHostWidth * kHostHeight), plain.primId.size());
    ASSERT_EQ(static_cast<std::size_t>(wideWidth * wideHeight), wide.primId.size());

    int differing = 0;

    for (int y = 0; y < kHostHeight; ++y) {
        for (int x = 0; x < kHostWidth; ++x) {
            const std::size_t inner = static_cast<std::size_t>(y * kHostWidth + x);
            const std::size_t outer = static_cast<std::size_t>((y + kHostOverscan) * wideWidth + x + kHostOverscan);

            if (plain.primId[inner] != wide.primId[outer] || std::abs(plain.depth[inner] - wide.depth[outer]) > 1e-4f * std::abs(plain.depth[inner])) {
                ++differing;
            }
        }
    }
    EXPECT_EQ(0, differing);
    EXPECT_EQ(std::string("/Centre"), pathAt(plain, kHostWidth, kHostWidth / 2, kHostHeight / 2));

    int outside = 0;

    for (int y = 0; y < kHostHeight; ++y) {
        for (int x = 0; x < kHostWidth; ++x) {
            outside += pathAt(plain, kHostWidth, x, y) == "/Outside" ? 1 : 0;
        }
    }
    EXPECT_EQ(0, outside);
    EXPECT_EQ(std::string("/Outside"), pathAt(wide, wideWidth, kHostOverscan / 2, wideHeight / 2));
    EXPECT_EQ(std::string("/Backdrop"), pathAt(wide, wideWidth, 1, 1));
    EXPECT_EQ(std::string("/Backdrop"), pathAt(wide, wideWidth, wideWidth - 2, wideHeight - 2));
    HydraRender::releaseAll();
}

// The unwrap names the prim and the primvar it could not find, and draws nothing.
TEST(NatronUtilityProjectionUnwrap, AMissingPrimvarIsAnErrorNamingPrimAndPrimvar)
{
    UtilityScene scene;

    addBakedQuad(scene, 0.f, false);

    const std::atomic<bool> stop(false);
    std::string error;

    EXPECT_FALSE(wavefrontUnwrap(scene.snapshot(), bakeFrame(1001), stop, &error));
    EXPECT_NE(std::string::npos, error.find("/quad")) << error;
    EXPECT_NE(std::string::npos, error.find("\"uv\"")) << error;

    WavefrontFrame missing = bakeFrame(1001);

    missing.unwrapPrim = SdfPath("/elsewhere");
    error.clear();
    EXPECT_FALSE(wavefrontUnwrap(scene.snapshot(), missing, stop, &error));
    EXPECT_NE(std::string::npos, error.find("/elsewhere")) << error;

    WavefrontCpu cpu;
    ProjectionFilm film(kBakeSize, kBakeSize);

    try {
        wavefrontDrawSamples(cpu, scene.snapshot(), bakeFrame(1001), WavefrontMotion(), film, stop, 2, std::function<void(std::size_t)>(), nullptr);
        ADD_FAILURE() << "the frame drew without the unwrap's primvar";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("/quad")) << e.what();
    }
    EXPECT_EQ(0, hitsIn(film));
}

namespace {

SdfLayerRefPtr
bakeLayer(bool withCoordinates)
{
    SdfLayerRefPtr layer = SceneOps::createNodeLayer("NatronUtilityProjectionBake");
    UsdStageRefPtr stage = UsdStage::Open(layer);

    defineQuad(stage, "/Baked", GfVec3f(0.f, 0.f, -10.f), 2.f, 2.f);
    if (withCoordinates) {
        UsdGeomPrimvarsAPI(stage->GetPrimAtPath(SdfPath("/Baked")))
            .CreatePrimvar(TfToken("uv"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying)
            .Set(VtVec2fArray { GfVec2f(0.f, 0.f), GfVec2f(1.f, 0.f), GfVec2f(1.f, 1.f), GfVec2f(0.f, 1.f) });
    }

    return layer;
}

bool
bakeThroughTheHost(const void* owner,
                   bool withCoordinates,
                   HydraRender::Result* result,
                   std::string* error)
{
    HydraRender::Request request;

    request.owner = owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), bakeLayer(withCoordinates), 1);
    request.time = UsdTimeCode(1.);
    request.width = kBakeSize;
    request.height = kBakeSize;
    request.renderer = kRenderDelegateUtilityId;
    request.frustum = overscanCamera();
    request.settings[kUtilitySettingDevice] = VtValue(TfToken(kUtilityDeviceCpu));
    request.settings[kUtilitySettingPixelSamples] = VtValue(1);
    request.settings[kUtilitySettingProjection] = VtValue(TfToken(kUtilityProjectionUvUnwrap));
    request.settings[kUtilitySettingUvPrim] = VtValue(std::string("/Baked"));
    request.settings[kUtilitySettingUvPrimvar] = VtValue(std::string("uv"));

    return HydraRender::render(request, result, error);
}
} // anon namespace

// Through the host, the mesh hands its face-varying UVs to the bake, which then covers the whole tile
// whatever the camera sees; without them the render fails naming the prim and the primvar.
TEST(NatronUtilityProjectionHost, AUvUnwrapBakesThePrimItNamesAndFailsWithoutItsPrimvar)
{
    const int owner = 0;
    HydraRender::Result baked;
    std::string error;

    ASSERT_TRUE(bakeThroughTheHost(&owner, true, &baked, &error)) << error;
    ASSERT_EQ(static_cast<std::size_t>(kBakeSize * kBakeSize), baked.primId.size());

    int elsewhere = 0;

    for (int y = 0; y < kBakeSize; ++y) {
        for (int x = 0; x < kBakeSize; ++x) {
            elsewhere += pathAt(baked, kBakeSize, x, y) == "/Baked" ? 0 : 1;
        }
    }
    EXPECT_EQ(0, elsewhere);

    HydraRender::Result failed;

    error.clear();
    EXPECT_FALSE(bakeThroughTheHost(&owner, false, &failed, &error));
    EXPECT_NE(std::string::npos, error.find("/Baked")) << error;
    EXPECT_NE(std::string::npos, error.find("\"uv\"")) << error;
    HydraRender::releaseAll();
}
