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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ostream>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/usd/sdf/path.h>

#include "Engine/Nodes/Scene/Utility/EmbreeTracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Gpu/GpuTestDevice.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const float kBarycentricTolerance = 1e-5f;

enum class Backend {
    Embree,
    Gpu,
};

void
PrintTo(Backend backend,
        std::ostream* os)
{
    *os << (backend == Backend::Embree ? "Embree" : "Gpu");
}

// A unit quad in the z = 0 plane, split along its (0,0)-(1,1) diagonal.
VtVec3fArray
quadPoints(float z = 0.f)
{
    return VtVec3fArray { GfVec3f(0.f, 0.f, z), GfVec3f(1.f, 0.f, z), GfVec3f(1.f, 1.f, z), GfVec3f(0.f, 1.f, z) };
}

VtVec3iArray
quadTriangles()
{
    return VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) };
}

// Rays along -z from z = 5.
class Rays {
public:
    void add(float x,
             float y,
             float tMin = 0.f,
             float tMax = 100.f)
    {
        _originX.push_back(x);
        _originY.push_back(y);
        _originZ.push_back(5.f);
        _directionX.push_back(0.f);
        _directionY.push_back(0.f);
        _directionZ.push_back(-1.f);
        _tMin.push_back(tMin);
        _tMax.push_back(tMax);
    }

    Tracer::RayBatch batch() const
    {
        Tracer::RayBatch rays;

        rays.originX = _originX.data();
        rays.originY = _originY.data();
        rays.originZ = _originZ.data();
        rays.directionX = _directionX.data();
        rays.directionY = _directionY.data();
        rays.directionZ = _directionZ.data();
        rays.tMin = _tMin.data();
        rays.tMax = _tMax.data();
        rays.count = _originX.size();

        return rays;
    }

private:
    std::vector<float> _originX, _originY, _originZ;
    std::vector<float> _directionX, _directionY, _directionZ;
    std::vector<float> _tMin, _tMax;
};

struct Hits {
    explicit Hits(const Rays& rays,
                  const Tracer& tracer)
    {
        const Tracer::RayBatch batch = rays.batch();

        t.resize(batch.count);
        u.resize(batch.count);
        v.resize(batch.count);
        geometry.resize(batch.count);
        instance.resize(batch.count);
        primitive.resize(batch.count);

        Tracer::HitBatch hits;

        hits.t = t.data();
        hits.u = u.data();
        hits.v = v.data();
        hits.geometry = geometry.data();
        hits.instance = instance.data();
        hits.primitive = primitive.data();
        tracer.intersect(batch, hits);
    }

    std::vector<float> t, u, v;
    std::vector<std::uint32_t> geometry, instance, primitive;
};

Hits
shootAt(const Tracer& tracer,
        float x,
        float y)
{
    Rays rays;

    rays.add(x, y);

    return Hits(rays, tracer);
}

UtilityScene::GeometryId
addQuad(UtilityScene& scene,
        const char* path)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath(path), 1);

    scene.setTopology(id, quadPoints(), quadTriangles());

    return id;
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

// Each test runs against every backend; the GPU one skips, naming what is missing, on a device that
// cannot trace.
class NatronUtilityTracer : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() override
    {
        if (GetParam() == Backend::Embree) {
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
        _available = true;
#else
        std::printf("[  SKIPPED ] Natron was built without the Vulkan backend, so there is no GPU tracer\n");
#endif
    }

    void TearDown() override
    {
#ifdef NATRON_HAVE_GPU
        for (const std::shared_ptr<GpuTracer>& tracer : _gpuTracers) {
            const GpuTracer::Outcome outcome = tracer->status();

            EXPECT_TRUE(outcome.status == GpuTracer::Status::Ok) << outcome.message;
        }
        _gpuTracers.clear();
        if (_device) {
            _device->waitIdle();
        }
        _pool.reset();
        _registry.reset();
        _device.reset();
        _check.expectClean("the test");
#endif
    }

    // Null when the backend is unavailable, in which case SetUp has said why.
    std::shared_ptr<Tracer> makeTracer()
    {
        if (!_available) {
            return nullptr;
        }
        if (GetParam() == Backend::Embree) {
            return std::make_shared<EmbreeTracer>();
        }
#ifdef NATRON_HAVE_GPU
        const std::shared_ptr<GpuTracer> tracer = std::make_shared<GpuTracer>(*_registry, *_pool);

        _gpuTracers.push_back(tracer);

        return tracer;
#else
        return nullptr;
#endif
    }

private:
    bool _available = false;
#ifdef NATRON_HAVE_GPU
    gputest::ValidationCheck _check;
    std::unique_ptr<gpu::GpuDevice> _device;
    std::unique_ptr<gpu::GpuKernelRegistry> _registry;
    std::unique_ptr<gpu::GpuBufferPool> _pool;
    // Held past the test body so TearDown can report their status and free them before the pool.
    std::vector<std::shared_ptr<GpuTracer>> _gpuTracers;
#endif
};
} // anon namespace

TEST_P(NatronUtilityTracer, HitsAQuadWithExpectedDistanceAndBarycentrics)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 2);

    Rays rays;

    rays.add(0.75f, 0.25f);
    rays.add(0.25f, 0.75f);
    rays.add(2.f, 2.f);

    const Hits hits(rays, *tracer);

    EXPECT_FLOAT_EQ(5.f, hits.t[0]);
    EXPECT_EQ(0u, hits.primitive[0]);
    EXPECT_EQ(0u, hits.geometry[0]);
    EXPECT_EQ(0u, hits.instance[0]);
    EXPECT_NEAR(0.5f, hits.u[0], kBarycentricTolerance);
    EXPECT_NEAR(0.25f, hits.v[0], kBarycentricTolerance);

    EXPECT_FLOAT_EQ(5.f, hits.t[1]);
    EXPECT_EQ(1u, hits.primitive[1]);
    EXPECT_NEAR(0.25f, hits.u[1], kBarycentricTolerance);
    EXPECT_NEAR(0.5f, hits.v[1], kBarycentricTolerance);

    EXPECT_TRUE(std::isinf(hits.t[2]));
    EXPECT_EQ(Tracer::kNoHit, hits.geometry[2]);
    EXPECT_EQ(Tracer::kNoHit, hits.instance[2]);
    EXPECT_EQ(Tracer::kNoHit, hits.primitive[2]);
}

// A batch longer than a packet has to come back in order, the part-filled last packet included.
TEST_P(NatronUtilityTracer, BatchesLongerThanAPacketKeepTheirOrder)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);

    Rays rays;
    const int count = 37;

    for (int i = 0; i < count; ++i) {
        rays.add(i % 2 ? 0.5f : 7.f, 0.25f);
    }

    const Hits hits(rays, *tracer);

    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(i % 2 ? 0u : Tracer::kNoHit, hits.primitive[i]) << i;
    }
}

TEST_P(NatronUtilityTracer, HonoursTheRayInterval)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);

    Rays rays;

    rays.add(0.75f, 0.25f, 0.f, 4.f);
    rays.add(0.75f, 0.25f, 6.f, 100.f);
    rays.add(0.75f, 0.25f, 4.f, 6.f);

    const Hits hits(rays, *tracer);

    EXPECT_EQ(Tracer::kNoHit, hits.primitive[0]);
    EXPECT_EQ(Tracer::kNoHit, hits.primitive[1]);
    EXPECT_EQ(0u, hits.primitive[2]);
}

TEST_P(NatronUtilityTracer, AnInstanceOffsetMovesTheHit)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { translation(10., 0., 1.), translation(20., 0., 2.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 2);

    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);

    const Hits first = shootAt(*tracer, 10.75f, 0.25f);

    EXPECT_EQ(0u, first.primitive[0]);
    EXPECT_EQ(0u, first.instance[0]);
    EXPECT_FLOAT_EQ(4.f, first.t[0]);
    EXPECT_NEAR(0.5f, first.u[0], kBarycentricTolerance);

    const Hits second = shootAt(*tracer, 20.75f, 0.25f);

    EXPECT_EQ(1u, second.instance[0]);
    EXPECT_EQ(0u, second.geometry[0]);
    EXPECT_FLOAT_EQ(3.f, second.t[0]);
}

TEST_P(NatronUtilityTracer, ReportsGeometriesByTheirPositionInTheSnapshot)
{
    UtilityScene scene;
    const UtilityScene::GeometryId a = addQuad(scene, "/a");
    const UtilityScene::GeometryId b = addQuad(scene, "/b");

    scene.setInstances(a, { translation(10., 0., 0.) });
    scene.setInstances(b, { translation(20., 0., 0.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);

    EXPECT_EQ(0u, shootAt(*tracer, 10.5f, 0.25f).geometry[0]);
    EXPECT_EQ(1u, shootAt(*tracer, 20.5f, 0.25f).geometry[0]);
}

TEST_P(NatronUtilityTracer, EditingPointsRefitsWithoutRebuildingTheInstanceScene)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { translation(10., 0., 0.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 2);
    EXPECT_EQ(1u, tracer->stats().geometryBuilds);
    EXPECT_EQ(1u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(0u, tracer->stats().geometryRefits);
    EXPECT_EQ(0u, tracer->stats().instanceSceneUpdates);

    scene.setPoints(id, quadPoints(2.f));
    tracer->update(scene.snapshot(), 2);
    EXPECT_EQ(1u, tracer->stats().geometryBuilds);
    EXPECT_EQ(1u, tracer->stats().geometryRefits);
    EXPECT_EQ(1u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(1u, tracer->stats().instanceSceneUpdates);
    EXPECT_FLOAT_EQ(3.f, shootAt(*tracer, 10.75f, 0.25f).t[0]);

    // The new points leave the old bounds, which a stale structure would fail to hit.
    VtVec3fArray wider = quadPoints();

    for (GfVec3f& point : wider) {
        point[0] *= 10.f;
    }
    scene.setPoints(id, wider);
    tracer->update(scene.snapshot(), 2);
    EXPECT_EQ(2u, tracer->stats().geometryRefits);
    EXPECT_EQ(1u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(0u, shootAt(*tracer, 15.f, 0.1f).primitive[0]);
}

TEST_P(NatronUtilityTracer, TheSameVersionDoesNoWork)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);

    const Tracer::Stats before = tracer->stats();

    tracer->update(scene.snapshot(), 1);

    const Tracer::Stats after = tracer->stats();

    EXPECT_EQ(before.geometryBuilds, after.geometryBuilds);
    EXPECT_EQ(before.geometryRefits, after.geometryRefits);
    EXPECT_EQ(before.instanceSceneBuilds, after.instanceSceneBuilds);
    EXPECT_EQ(before.instanceSceneUpdates, after.instanceSceneUpdates);
}

TEST_P(NatronUtilityTracer, MovingAnInstanceRefitsTheInstanceScene)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);
    scene.setInstances(id, { translation(10., 0., 0.) });
    tracer->update(scene.snapshot(), 1);

    EXPECT_EQ(1u, tracer->stats().geometryBuilds);
    EXPECT_EQ(0u, tracer->stats().geometryRefits);
    EXPECT_EQ(1u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(1u, tracer->stats().instanceSceneUpdates);
    EXPECT_EQ(0u, shootAt(*tracer, 10.75f, 0.25f).primitive[0]);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);
}

// Moves only refit; adding, removing or swapping what an instance places rebuilds.
TEST_P(NatronUtilityTracer, OnlyMembershipChangesRebuildTheInstanceScene)
{
    UtilityScene scene;
    const UtilityScene::GeometryId a = addQuad(scene, "/a");
    const UtilityScene::GeometryId b = addQuad(scene, "/b");

    scene.setInstances(a, { translation(10., 0., 0.), translation(20., 0., 0.) });
    scene.setInstances(b, { translation(30., 0., 0.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 2);
    EXPECT_EQ(1u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(0u, tracer->stats().instanceSceneUpdates);

    for (int step = 1; step <= 3; ++step) {
        const double offset = 100. * step;

        scene.setInstances(a, { translation(10. + offset, 0., 0.), translation(20., 0., 0.) });
        scene.setInstances(b, { translation(30. + offset, 0., 0.) });
        tracer->update(scene.snapshot(), 2);

        const float x = static_cast<float>(offset);

        EXPECT_EQ(0u, shootAt(*tracer, x + 10.75f, 0.25f).instance[0]) << step;
        EXPECT_EQ(1u, shootAt(*tracer, 20.75f, 0.25f).instance[0]) << step;
        EXPECT_EQ(2u, shootAt(*tracer, x + 30.75f, 0.25f).instance[0]) << step;
        EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, x - 89.25f, 0.25f).primitive[0]) << step;
    }
    EXPECT_EQ(2u, tracer->stats().geometryBuilds);
    EXPECT_EQ(0u, tracer->stats().geometryRefits);
    EXPECT_EQ(1u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(3u, tracer->stats().instanceSceneUpdates);

    scene.setInstances(b, { translation(330., 0., 0.), translation(40., 0., 0.) });
    tracer->update(scene.snapshot(), 2);
    EXPECT_EQ(2u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(3u, tracer->stats().instanceSceneUpdates);
    EXPECT_EQ(3u, shootAt(*tracer, 40.75f, 0.25f).instance[0]);

    scene.setInstances(b, {});
    tracer->update(scene.snapshot(), 2);
    EXPECT_EQ(3u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 40.75f, 0.25f).primitive[0]);
}

TEST_P(NatronUtilityTracer, NewTopologyRebuildsTheGeometryAndItsInstances)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);
    scene.setTopology(id, quadPoints(), VtVec3iArray { GfVec3i(0, 1, 2) });
    tracer->update(scene.snapshot(), 1);

    EXPECT_EQ(2u, tracer->stats().geometryBuilds);
    EXPECT_EQ(2u, tracer->stats().instanceSceneBuilds);
    EXPECT_EQ(0u, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.25f, 0.75f).primitive[0]);
}

TEST_P(NatronUtilityTracer, RemovedGeometryStopsDrawing)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);
    EXPECT_EQ(0u, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);
    scene.removeGeometry(id);
    tracer->update(scene.snapshot(), 1);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);
}

TEST_P(NatronUtilityTracer, AnEmptySceneMissesEverything)
{
    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);

    UtilityScene scene;

    tracer->update(scene.snapshot(), 1);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);

    const UtilityScene::GeometryId empty = scene.addGeometry(SdfPath("/empty"), 1);

    scene.setInstances(empty, { GfMatrix4d(1.) });
    tracer->update(scene.snapshot(), 1);
    EXPECT_EQ(0u, tracer->stats().geometryBuilds);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);
}

TEST_P(NatronUtilityTracer, TrianglesOutsideThePointsDoNotDraw)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/bad"), 1);

    scene.setTopology(id, quadPoints(), VtVec3iArray { GfVec3i(0, 1, 9) });
    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);
    EXPECT_EQ(Tracer::kNoHit, shootAt(*tracer, 0.75f, 0.25f).primitive[0]);
}

TEST_P(NatronUtilityTracer, AnswersOcclusionBatches)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addQuad(scene, "/quad");

    scene.setInstances(id, { GfMatrix4d(1.) });

    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer) {
        return;
    }

    tracer->update(scene.snapshot(), 1);

    Rays rays;

    rays.add(0.75f, 0.25f);
    rays.add(2.f, 2.f);
    rays.add(0.75f, 0.25f, 0.f, 4.f);

    std::vector<std::uint8_t> blocked(3, 7);

    tracer->occluded(rays.batch(), blocked.data());
    EXPECT_EQ(1, blocked[0]);
    EXPECT_EQ(0, blocked[1]);
    EXPECT_EQ(0, blocked[2]);
}

// One thread and many must agree, and a build must finish whatever the budget.
TEST_P(NatronUtilityTracer, AgreesAcrossThreadBudgets)
{
    for (const int threads : { 0, 1, 2, 64 }) {
        UtilityScene scene;
        const UtilityScene::GeometryId id = addQuad(scene, "/quad");

        scene.setInstances(id, { GfMatrix4d(1.) });

        const std::shared_ptr<Tracer> tracer = makeTracer();

        if (!tracer) {
            return;
        }

        tracer->update(scene.snapshot(), threads);
        EXPECT_FLOAT_EQ(5.f, shootAt(*tracer, 0.75f, 0.25f).t[0]) << threads;
    }
}

namespace {
// A 2x2-cell height field: rays falling steeply onto it meet the triangle they aim at before any other.
VtVec3fArray
tentPoints()
{
    VtVec3fArray points;

    for (int y = 0; y <= 2; ++y) {
        for (int x = 0; x <= 2; ++x) {
            points.push_back(GfVec3f(static_cast<float>(x), static_cast<float>(y), 0.1f * static_cast<float>((x * 7 + y * 3) % 5)));
        }
    }

    return points;
}

VtVec3iArray
tentTriangles()
{
    VtVec3iArray triangles;

    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const int a = y * 3 + x;

            triangles.push_back(GfVec3i(a, a + 1, a + 4));
            triangles.push_back(GfVec3i(a, a + 4, a + 3));
        }
    }

    return triangles;
}

struct Target {
    VtVec3fArray points;
    VtVec3iArray triangles;
    GfMatrix4d transform;
};
} // anon namespace

// Rays aim inside triangles, well clear of every edge, so both backends must report the same triangle;
// the misses go between instances or end short of the surface.
TEST_P(NatronUtilityTracer, AgreesWithEmbree)
{
    const std::shared_ptr<Tracer> tracer = makeTracer();

    if (!tracer || GetParam() == Backend::Embree) {
        return;
    }

    UtilityScene scene;
    const UtilityScene::GeometryId tent = scene.addGeometry(SdfPath("/tent"), 1);
    const UtilityScene::GeometryId quad = addQuad(scene, "/quad");
    const GfMatrix4d turned = GfMatrix4d().SetScale(2.) * GfMatrix4d().SetRotate(GfRotation(GfVec3d(0., 0., 1.), 30.)) * translation(40., 5., -1.);
    const std::vector<Target> targets = {
        { tentPoints(), tentTriangles(), GfMatrix4d(1.) },
        { tentPoints(), tentTriangles(), translation(20., 0., 1.) },
        { tentPoints(), tentTriangles(), turned },
        { quadPoints(), quadTriangles(), translation(60., 0., 0.) },
        { quadPoints(), quadTriangles(), translation(80., 0., 0.5) },
    };

    scene.setTopology(tent, tentPoints(), tentTriangles());
    scene.setInstances(tent, { targets[0].transform, targets[1].transform, targets[2].transform });
    scene.setInstances(quad, { targets[3].transform, targets[4].transform });

    std::mt19937 random(280);
    std::uniform_real_distribution<double> corner(0.1, 0.8);
    std::uniform_real_distribution<double> sideways(-0.3, 0.3);
    std::vector<float> originX, originY, originZ, directionX, directionY, directionZ, tMin, tMax;
    std::vector<std::uint32_t> aimedAt;

    auto add = [&](const GfVec3d& origin, const GfVec3d& direction, float nearest, float farthest, std::uint32_t primitive) {
        originX.push_back(static_cast<float>(origin[0]));
        originY.push_back(static_cast<float>(origin[1]));
        originZ.push_back(static_cast<float>(origin[2]));
        directionX.push_back(static_cast<float>(direction[0]));
        directionY.push_back(static_cast<float>(direction[1]));
        directionZ.push_back(static_cast<float>(direction[2]));
        tMin.push_back(nearest);
        tMax.push_back(farthest);
        aimedAt.push_back(primitive);
    };

    for (const Target& target : targets) {
        for (std::size_t triangle = 0; triangle < target.triangles.size(); ++triangle) {
            for (int sample = 0; sample < 4; ++sample) {
                double u = corner(random);
                double v = corner(random);

                while (u + v > 0.9) {
                    u = corner(random);
                    v = corner(random);
                }

                const GfVec3i& corners = target.triangles[triangle];
                const GfVec3d local = (1. - u - v) * GfVec3d(target.points[corners[0]]) + u * GfVec3d(target.points[corners[1]])
                    + v * GfVec3d(target.points[corners[2]]);
                const GfVec3d hit = target.transform.Transform(local);
                const GfVec3d origin = hit + GfVec3d(sideways(random), sideways(random), 6.);
                const std::uint32_t primitive = static_cast<std::uint32_t>(triangle);

                // The hit lies at t = 1.
                add(origin, hit - origin, 0.f, 10.f, primitive);
                if (sample == 3) {
                    add(origin, hit - origin, 0.f, 0.5f, Tracer::kNoHit);
                    add(origin, hit - origin, 1.5f, 10.f, Tracer::kNoHit);
                    add(origin + GfVec3d(10., 0., 0.), GfVec3d(0., 0., -1.), 0.f, 100.f, Tracer::kNoHit);
                }
            }
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
    rays.count = originX.size();

    EmbreeTracer reference;

    ASSERT_TRUE(reference.update(scene.snapshot(), 2));
    ASSERT_TRUE(tracer->update(scene.snapshot(), 2));

    struct Results {
        explicit Results(std::size_t count)
            : t(count)
            , u(count)
            , v(count)
            , geometry(count)
            , instance(count)
            , primitive(count)
            , blocked(count, 7)
        {
            batch.t = t.data();
            batch.u = u.data();
            batch.v = v.data();
            batch.geometry = geometry.data();
            batch.instance = instance.data();
            batch.primitive = primitive.data();
        }

        std::vector<float> t, u, v;
        std::vector<std::uint32_t> geometry, instance, primitive;
        std::vector<std::uint8_t> blocked;
        Tracer::HitBatch batch;
    };

    Results expected(rays.count);
    Results actual(rays.count);

    reference.intersect(rays, expected.batch);
    reference.occluded(rays, expected.blocked.data());
    tracer->intersect(rays, actual.batch);
    tracer->occluded(rays, actual.blocked.data());

    for (std::size_t ray = 0; ray < rays.count; ++ray) {
        ASSERT_EQ(aimedAt[ray], expected.primitive[ray]) << ray;
        EXPECT_EQ(expected.geometry[ray], actual.geometry[ray]) << ray;
        EXPECT_EQ(expected.instance[ray], actual.instance[ray]) << ray;
        EXPECT_EQ(expected.primitive[ray], actual.primitive[ray]) << ray;
        EXPECT_EQ(expected.blocked[ray], actual.blocked[ray]) << ray;
        if (expected.primitive[ray] == Tracer::kNoHit) {
            EXPECT_TRUE(std::isinf(actual.t[ray])) << ray;
        } else {
            EXPECT_NEAR(expected.t[ray], actual.t[ray], 1e-5f * std::abs(expected.t[ray])) << ray;
            EXPECT_NEAR(expected.u[ray], actual.u[ray], 1e-4f) << ray;
            EXPECT_NEAR(expected.v[ray], actual.v[ray], 1e-4f) << ray;
        }
    }
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityTracer, ::testing::Values(Backend::Embree, Backend::Gpu));
