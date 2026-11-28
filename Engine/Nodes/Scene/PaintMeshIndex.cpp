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

#include "PaintMeshIndex.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>

#include <QThreadPool>

#include <embree4/rtcore.h>
#include <tbb/task_arena.h>
#include <xmmintrin.h>

#include <pxr/base/tf/diagnostic.h>

#include "Engine/AppManager.h"
#include "Global/FloatingPointExceptions.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const std::size_t kPacketSize = 16;

// Embree's traversal is much slower on denormals, and its maths raises floating-point exceptions that
// debug builds trap; the caller's mode is restored on the way out.
class ScopedEmbreeFpMode {
public:
    ScopedEmbreeFpMode()
        : _noTraps()
        , _saved(_mm_getcsr())
    {
        const unsigned int flushToZero = 1u << 15;
        const unsigned int denormalsAreZero = 1u << 6;

        _mm_setcsr(_saved | flushToZero | denormalsAreZero);
    }

    ~ScopedEmbreeFpMode()
    {
        _mm_setcsr(_saved);
    }

private:
    const boost_adaptbx::floating_point::FpTrapGuard _noTraps;
    const unsigned int _saved;
};

void
reportEmbreeError(void* /*userData*/,
                  RTCError code,
                  const char* message)
{
    if (code == RTC_ERROR_CANCELLED) {
        return;
    }
    TF_WARN("Embree error %d: %s", static_cast<int>(code), message ? message : "");
}

// One device for the process: each holds its own memory pools and thread state, and indices are
// built for every paint session and render. Device calls are thread-safe and their errors per thread.
// Threads stay unset: on Embree's TBB build a device's thread count applies to the whole process,
// so each commit runs in an arena of its budget instead.
RTCDevice
sharedDevice()
{
    static std::mutex mutex;
    static RTCDevice device = nullptr;
    std::lock_guard<std::mutex> lock(mutex);

    if (!device) {
        device = rtcNewDevice(nullptr);
        if (device) {
            rtcSetDeviceErrorFunction(device, reportEmbreeError, nullptr);
        }
    }
    if (device) {
        rtcRetainDevice(device);
    }

    return device;
}

int
effectThreads()
{
    return appPTR ? appPTR->getNCPUsAvailableForEffect() : QThreadPool::globalInstance()->maxThreadCount();
}

// The point of the triangle nearest p (Ericson, Real-Time Collision Detection, 5.1.5).
GfVec3d
closestPointOnTriangle(const GfVec3d& p,
                       const GfVec3d& a,
                       const GfVec3d& b,
                       const GfVec3d& c)
{
    const GfVec3d ab = b - a;
    const GfVec3d ac = c - a;
    const GfVec3d ap = p - a;
    const double d1 = GfDot(ab, ap);
    const double d2 = GfDot(ac, ap);
    if ((d1 <= 0.) && (d2 <= 0.)) {
        return a;
    }
    const GfVec3d bp = p - b;
    const double d3 = GfDot(ab, bp);
    const double d4 = GfDot(ac, bp);
    if ((d3 >= 0.) && (d4 <= d3)) {
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if ((vc <= 0.) && (d1 >= 0.) && (d3 <= 0.)) {
        return a + ab * (d1 / (d1 - d3));
    }
    const GfVec3d cp = p - c;
    const double d5 = GfDot(ab, cp);
    const double d6 = GfDot(ac, cp);
    if ((d6 >= 0.) && (d5 <= d6)) {
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if ((vb <= 0.) && (d2 >= 0.) && (d6 <= 0.)) {
        return a + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if ((va <= 0.) && ((d4 - d3) >= 0.) && ((d5 - d6) >= 0.)) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double sum = va + vb + vc;
    if (!(sum != 0.)) {
        return a;
    }
    const double v = vb / sum;
    const double w = vc / sum;

    return a + ab * v + ac * w;
}

struct SphereQuery {
    const std::vector<SceneRaster::TriangulatedMesh>* targets;
    GfVec3d centre;
    double radiusSquared;
    std::vector<PaintMeshIndex::TriangleRef>* found;
};

bool
collectTriangle(RTCPointQueryFunctionArguments* args)
{
    SphereQuery* query = static_cast<SphereQuery*>(args->userPtr);

    if ((args->geomID >= query->targets->size()) || (args->primID == RTC_INVALID_GEOMETRY_ID)) {
        return false;
    }
    const SceneRaster::TriangulatedMesh& mesh = (*query->targets)[args->geomID];
    if (args->primID >= mesh.triangles.size()) {
        return false;
    }
    const GfVec3i& corners = mesh.triangles[args->primID];
    const GfVec3d closest = closestPointOnTriangle(query->centre, GfVec3d(mesh.worldPoints[corners[0]]), GfVec3d(mesh.worldPoints[corners[1]]), GfVec3d(mesh.worldPoints[corners[2]]));
    if ((closest - query->centre).GetLengthSq() <= query->radiusSquared) {
        PaintMeshIndex::TriangleRef ref;
        ref.mesh = (int)args->geomID;
        ref.triangle = (int)args->primID;
        query->found->push_back(ref);
    }

    // The query's radius stays as it is: every triangle within it is wanted, not the nearest.
    return false;
}
} // anonymous namespace

struct PaintMeshIndex::Implementation {
    RTCDevice device = nullptr;
    RTCScene scene = nullptr;
    std::vector<SceneRaster::TriangulatedMesh> targets;
    std::vector<SceneRaster::TriangulatedMesh> occluders;
    Key key;
    bool builtFromKey = false;

    ~Implementation()
    {
        release();
        if (device) {
            rtcReleaseDevice(device);
        }
    }

    void release()
    {
        if (scene) {
            rtcReleaseScene(scene);
            scene = nullptr;
        }
        targets.clear();
        occluders.clear();
        builtFromKey = false;
    }

    bool attach(const SceneRaster::TriangulatedMesh& mesh,
                unsigned int geomID)
    {
        if (mesh.triangles.empty() || mesh.worldPoints.empty()) {
            return true;
        }
        RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        if (!geometry) {
            return false;
        }
        float* vertices = static_cast<float*>(rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), mesh.worldPoints.size()));
        unsigned int* indices = static_cast<unsigned int*>(rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned int), mesh.triangles.size()));
        if (!vertices || !indices) {
            rtcReleaseGeometry(geometry);

            return false;
        }
        for (std::size_t i = 0; i < mesh.worldPoints.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                vertices[3 * i + c] = mesh.worldPoints[i][c];
            }
        }
        for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
            for (int k = 0; k < 3; ++k) {
                indices[3 * t + k] = (unsigned int)mesh.triangles[t][k];
            }
        }
        rtcCommitGeometry(geometry);
        rtcAttachGeometryByID(scene, geometry, geomID);
        rtcReleaseGeometry(geometry);

        return true;
    }

    bool fillHit(const RTCRayHit& rayHit,
                 Hit* hit) const
    {
        if ((rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) || (rayHit.hit.geomID >= targets.size())) {
            return false;
        }
        const SceneRaster::TriangulatedMesh& mesh = targets[rayHit.hit.geomID];
        const unsigned int t = rayHit.hit.primID;
        if (t >= mesh.triangles.size()) {
            return false;
        }
        const GfVec3i& corners = mesh.triangles[t];
        const float b1 = rayHit.hit.u;
        const float b2 = rayHit.hit.v;
        const float b0 = 1.f - b1 - b2;
        hit->mesh = (int)rayHit.hit.geomID;
        hit->triangle = (int)t;
        hit->barycentrics = GfVec3f(b0, b1, b2);
        hit->position = mesh.worldPoints[corners[0]] * b0 + mesh.worldPoints[corners[1]] * b1 + mesh.worldPoints[corners[2]] * b2;
        hit->normal = mesh.normals.size() > t ? mesh.normals[t] : GfVec3f(0.f);
        hit->hasSt = (mesh.hasSt.size() > t) && (mesh.hasSt[t] != 0);
        if (hit->hasSt) {
            hit->st = mesh.st[3 * t] * b0 + mesh.st[3 * t + 1] * b1 + mesh.st[3 * t + 2] * b2;
            hit->udimTile = SceneRaster::udimTileOf(hit->st[0], hit->st[1]);
        } else {
            hit->st = GfVec2f(0.f);
            hit->udimTile = 0;
        }
        hit->distance = rayHit.ray.tfar;

        return true;
    }
};

PaintMeshIndex::PaintMeshIndex()
    : _imp(new Implementation)
{
}

PaintMeshIndex::~PaintMeshIndex()
{
}

void
PaintMeshIndex::clear()
{
    _imp->release();
}

bool
PaintMeshIndex::build(std::vector<SceneRaster::TriangulatedMesh> targets,
                      std::vector<SceneRaster::TriangulatedMesh> occluders,
                      int threads,
                      std::string* errorMessage)
{
    Implementation& imp = *_imp;

    imp.release();
    if (!imp.device) {
        imp.device = sharedDevice();
        if (!imp.device) {
            *errorMessage = "The ray tracer could not start.";

            return false;
        }
    }
    imp.scene = rtcNewScene(imp.device);
    if (!imp.scene) {
        *errorMessage = "The ray tracer could not make a scene.";

        return false;
    }
    // Without ROBUST, rays can slip through the edge two triangles share.
    rtcSetSceneFlags(imp.scene, RTC_SCENE_FLAG_ROBUST);
    rtcSetSceneBuildQuality(imp.scene, RTC_BUILD_QUALITY_HIGH);

    imp.targets = std::move(targets);
    imp.occluders = std::move(occluders);
    for (std::size_t i = 0; i < imp.targets.size(); ++i) {
        if (!imp.attach(imp.targets[i], (unsigned int)i)) {
            *errorMessage = "The ray tracer could not hold " + imp.targets[i].path.GetString() + ".";
            imp.release();

            return false;
        }
    }
    for (std::size_t i = 0; i < imp.occluders.size(); ++i) {
        if (!imp.attach(imp.occluders[i], (unsigned int)(imp.targets.size() + i))) {
            *errorMessage = "The ray tracer could not hold " + imp.occluders[i].path.GetString() + ".";
            imp.release();

            return false;
        }
    }

    // Embree's TBB build spawns into whichever arena commits it, which holds it to the budget. The
    // arena takes its workers' floating-point mode from this thread, so the traps are off before it starts.
    const boost_adaptbx::floating_point::FpTrapGuard noTraps;
    tbb::task_arena arena(std::max(1, threads > 0 ? threads : effectThreads()));
    arena.execute([&imp]() {
        const ScopedEmbreeFpMode fpMode;
        rtcCommitScene(imp.scene);
    });
    if (rtcGetDeviceError(imp.device) != RTC_ERROR_NONE) {
        *errorMessage = "The ray tracer could not build its index of the meshes.";
        imp.release();

        return false;
    }

    return true;
}

bool
PaintMeshIndex::buildFromStage(const UsdStageRefPtr& stage,
                               const Key& key,
                               int threads,
                               std::string* errorMessage)
{
    _imp->release();
    if (!stage) {
        *errorMessage = "There is no scene to paint on.";

        return false;
    }
    std::vector<SceneRaster::TriangulatedMesh> targets;
    if (!SceneRaster::triangulateMeshes(stage, key.meshes, key.stName, key.time, &targets, errorMessage)) {
        return false;
    }
    const std::set<SdfPath> targetPaths(key.meshes.begin(), key.meshes.end());
    std::vector<SdfPath> occluderPaths;
    for (const SdfPath& path : SceneRaster::drawnMeshes(stage, std::vector<SdfPath>(), key.time)) {
        if (targetPaths.find(path) == targetPaths.end()) {
            occluderPaths.push_back(path);
        }
    }
    // An occluder only hides paint, so one that cannot be triangulated costs some occlusion rather than the stroke.
    std::vector<SceneRaster::TriangulatedMesh> occluders;
    occluders.reserve(occluderPaths.size());
    for (const SdfPath& path : occluderPaths) {
        SceneRaster::TriangulatedMesh mesh;
        std::string error;
        if (SceneRaster::triangulateMesh(stage, path, TfToken(), key.time, &mesh, &error)) {
            occluders.push_back(std::move(mesh));
        } else {
            TF_WARN("Painting ignores %s as an occluder: %s", path.GetText(), error.c_str());
        }
    }
    if (!build(std::move(targets), std::move(occluders), threads, errorMessage)) {
        return false;
    }
    _imp->key = key;
    _imp->builtFromKey = true;

    return true;
}

bool
PaintMeshIndex::isBuiltFrom(const Key& key) const
{
    return _imp->builtFromKey && (_imp->key == key);
}

const std::vector<SceneRaster::TriangulatedMesh>&
PaintMeshIndex::targets() const
{
    return _imp->targets;
}

const std::vector<SceneRaster::TriangulatedMesh>&
PaintMeshIndex::occluders() const
{
    return _imp->occluders;
}

bool
PaintMeshIndex::raycast(const GfFrustum& camera,
                        double u,
                        double v,
                        Hit* hit) const
{
    const GfRay ray = camera.ComputeRay(GfVec2d(2. * u - 1., 2. * v - 1.));
    GfVec3d direction = ray.GetDirection();
    if (!(direction.Normalize() > 0.)) {
        return false;
    }
    const double cosine = GfDot(direction, camera.ComputeViewDirection());
    if (!(cosine > 0.)) {
        return false;
    }
    const GfRange1d nearFar = camera.GetNearFar();

    return raycast(GfVec3f(ray.GetStartPoint()), GfVec3f(direction), (float)(nearFar.GetMin() / cosine), (float)(nearFar.GetMax() / cosine), hit);
}

bool
PaintMeshIndex::raycast(const GfVec3f& origin,
                        const GfVec3f& direction,
                        float tNear,
                        float tFar,
                        Hit* hit) const
{
    const Implementation& imp = *_imp;
    if (!imp.scene || !(tFar >= tNear)) {
        return false;
    }
    const ScopedEmbreeFpMode fpMode;
    RTCRayHit rayHit;
    std::memset(&rayHit, 0, sizeof(rayHit));
    rayHit.ray.org_x = origin[0];
    rayHit.ray.org_y = origin[1];
    rayHit.ray.org_z = origin[2];
    rayHit.ray.dir_x = direction[0];
    rayHit.ray.dir_y = direction[1];
    rayHit.ray.dir_z = direction[2];
    rayHit.ray.tnear = std::max(0.f, tNear);
    rayHit.ray.tfar = tFar;
    rayHit.ray.time = 0.f;
    rayHit.ray.mask = 0xffffffffu;
    rayHit.ray.flags = 0;
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(imp.scene, &rayHit, nullptr);

    return imp.fillHit(rayHit, hit);
}

std::vector<PaintMeshIndex::TriangleRef>
PaintMeshIndex::trianglesInSphere(const GfVec3f& centre,
                                  float radius) const
{
    std::vector<TriangleRef> found;
    const Implementation& imp = *_imp;
    if (!imp.scene || !(radius >= 0.f)) {
        return found;
    }
    const ScopedEmbreeFpMode fpMode;
    RTCPointQuery query;
    query.x = centre[0];
    query.y = centre[1];
    query.z = centre[2];
    query.time = 0.f;
    query.radius = radius;
    RTCPointQueryContext context;
    rtcInitPointQueryContext(&context);
    SphereQuery sphere;
    sphere.targets = &imp.targets;
    sphere.centre = GfVec3d(centre);
    sphere.radiusSquared = (double)radius * (double)radius;
    sphere.found = &found;
    rtcPointQuery(imp.scene, &query, &context, collectTriangle, &sphere);

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());

    return found;
}

void
PaintMeshIndex::visible(const GfVec3f* points,
                        const GfVec3f* normals,
                        std::size_t count,
                        const GfFrustum& camera,
                        double bias,
                        std::uint8_t* visible) const
{
    const Implementation& imp = *_imp;
    const GfMatrix4d view = camera.ComputeViewMatrix();
    const GfMatrix4d viewProjection = view * camera.ComputeProjectionMatrix();
    const bool perspective = camera.GetProjectionType() == GfFrustum::Perspective;
    const GfVec3d eye = camera.GetPosition();
    const GfVec3d towardCamera = -camera.ComputeViewDirection();
    const double nearClip = camera.GetNearFar().GetMin();
    const double occlusionBias = std::max(bias, kMinimumBias);
    const ScopedEmbreeFpMode fpMode;

    for (std::size_t start = 0; start < count; start += kPacketSize) {
        const std::size_t n = std::min(kPacketSize, count - start);
        alignas(64) int valid[kPacketSize];
        RTCRay16 packet;
        bool anyRay = false;
        for (std::size_t i = 0; i < kPacketSize; ++i) {
            valid[i] = 0;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t index = start + i;
            visible[index] = 0;
            const GfVec3d world(points[index]);
            const GfVec4d p(world[0], world[1], world[2], 1.);
            const double depth = -(p * view)[2];
            if (!(depth > 0.)) {
                continue;
            }
            if (normals && (GfDot(GfVec3d(normals[index]), perspective ? (eye - world) : towardCamera) <= 0.)) {
                continue;
            }
            const GfVec4d clip = p * viewProjection;
            if (clip[3] <= 0.) {
                continue;
            }
            const double ndcX = clip[0] / clip[3];
            const double ndcY = clip[1] / clip[3];
            if ((ndcX < -1.) || (ndcX > 1.) || (ndcY < -1.) || (ndcY > 1.)) {
                continue;
            }
            visible[index] = 1;

            GfVec3d direction;
            double tNear;
            double tFar;
            if (perspective) {
                direction = eye - world;
                const double distance = direction.Normalize();
                tNear = distance * occlusionBias;
                tFar = distance * (1. - nearClip / depth);
            } else {
                direction = towardCamera;
                tNear = depth * occlusionBias;
                tFar = depth - nearClip;
            }
            if (!imp.scene || !(tFar > tNear)) {
                continue;
            }
            valid[i] = -1;
            anyRay = true;
            packet.org_x[i] = (float)world[0];
            packet.org_y[i] = (float)world[1];
            packet.org_z[i] = (float)world[2];
            packet.dir_x[i] = (float)direction[0];
            packet.dir_y[i] = (float)direction[1];
            packet.dir_z[i] = (float)direction[2];
            packet.tnear[i] = (float)tNear;
            packet.tfar[i] = (float)tFar;
            packet.time[i] = 0.f;
            packet.mask[i] = 0xffffffffu;
            packet.id[i] = (unsigned int)i;
            packet.flags[i] = 0;
        }
        if (!anyRay) {
            continue;
        }
        rtcOccluded16(valid, imp.scene, &packet, nullptr);
        // Embree marks an occluded ray by setting its tfar to negative infinity.
        for (std::size_t i = 0; i < n; ++i) {
            if (valid[i] && (packet.tfar[i] == -std::numeric_limits<float>::infinity())) {
                visible[start + i] = 0;
            }
        }
    }
}

NATRON_NAMESPACE_EXIT
