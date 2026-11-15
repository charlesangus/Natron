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

#include "EmbreeTracer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

#include <embree4/rtcore.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <xmmintrin.h>

#include <pxr/base/tf/diagnostic.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const std::size_t kPacketSize = 16;

// Without ROBUST, rays can slip through the edge two triangles share. FILTER_FUNCTION_IN_ARGUMENTS lets
// a shadow query skip the casters its light is not shadow-linked to.
const RTCSceneFlags kSceneFlags = static_cast<RTCSceneFlags>(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);

// The context of a filtered occlusion query; Embree hands the filter the first member.
struct OcclusionContext {
    RTCRayQueryContext context;
    std::size_t start;
    const OcclusionFilter* filter;
};

void
filterOccluders(const RTCFilterFunctionNArguments* args)
{
    const OcclusionContext* context = reinterpret_cast<const OcclusionContext*>(args->context);

    for (unsigned int i = 0; i < args->N; ++i) {
        if (args->valid[i] == 0) {
            continue;
        }

        const std::size_t ray = context->start + RTCRayN_id(args->ray, args->N, i);
        const unsigned int instance = RTCHitN_instID(args->hit, args->N, i, 0);

        if (instance != RTC_INVALID_GEOMETRY_ID && !context->filter->blocks(ray, instance)) {
            args->valid[i] = 0;
        }
    }
}

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

// Refitting is only honoured on a two-level scene, which is what low build quality selects.
RTCScene
newRefittableScene(RTCDevice device)
{
    RTCScene scene = rtcNewScene(device);

    rtcSetSceneFlags(scene, kSceneFlags);
    rtcSetSceneBuildQuality(scene, RTC_BUILD_QUALITY_LOW);

    return scene;
}

bool
trianglesAreInRange(const UtilityScene::Geometry& geometry)
{
    const int pointCount = static_cast<int>(geometry.points.size());

    for (const GfVec3i& triangle : geometry.triangles) {
        for (int corner = 0; corner < 3; ++corner) {
            if (triangle[corner] < 0 || triangle[corner] >= pointCount) {
                return false;
            }
        }
    }

    return true;
}

// Traversal slows down badly on denormals. The flags go back afterwards because the calling thread
// belongs to the render pool and runs other nodes' work.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals()
        : _saved(_mm_getcsr())
    {
        const unsigned int flushToZero = 1u << 15;
        const unsigned int denormalsAreZero = 1u << 6;

        _mm_setcsr(_saved | flushToZero | denormalsAreZero);
    }

    ~ScopedFlushDenormals()
    {
        _mm_setcsr(_saved);
    }

private:
    const unsigned int _saved;
};

bool
wasStopped(const std::atomic<bool>* stop)
{
    return stop && stop->load(std::memory_order_relaxed);
}

bool
continueBuild(void* stop,
              double /*progress*/)
{
    return !static_cast<const std::atomic<bool>*>(stop)->load(std::memory_order_relaxed);
}

// A commit that stop can cut short. The scene then holds nothing usable: its caller rebuilds it.
void
commitCancellable(RTCScene scene,
                  const std::atomic<bool>* stop)
{
    if (stop) {
        rtcSetSceneProgressMonitorFunction(scene, continueBuild, const_cast<std::atomic<bool>*>(stop));
    }
    rtcCommitScene(scene);
    if (stop) {
        rtcSetSceneProgressMonitorFunction(scene, nullptr, nullptr);
    }
}

// One geometry's structure. A geometry with nothing to trace has a null scene and no instances.
class GeometryScene {
public:
    GeometryScene(RTCDevice device,
                  const UtilityScene::Geometry& source)
        : _triangles(source.triangles)
        , _pointCount(source.points.size())
        , _topologyVersion(source.topologyVersion)
        , _pointsVersion(source.pointsVersion)
        , _committed(false)
    {
        if (source.points.empty() || source.triangles.empty()) {
            return;
        }
        if (!trianglesAreInRange(source)) {
            TF_WARN("%s has triangles outside its %zu points and will not draw", source.path.GetText(), source.points.size());

            return;
        }
        _geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        // Embree reads vertices in 16-byte loads, so they need the padding it allocates; a VtArray has none.
        _vertices = static_cast<GfVec3f*>(rtcSetNewGeometryBuffer(_geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                                                  sizeof(GfVec3f), _pointCount));
        std::memcpy(_vertices, source.points.cdata(), _pointCount * sizeof(GfVec3f));
        rtcSetSharedGeometryBuffer(_geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, _triangles.cdata(), 0,
                                   sizeof(GfVec3i), _triangles.size());
        rtcSetGeometryBuildQuality(_geometry, RTC_BUILD_QUALITY_REFIT);
        rtcCommitGeometry(_geometry);
        _scene = newRefittableScene(device);
        rtcAttachGeometry(_scene, _geometry);
    }

    ~GeometryScene()
    {
        if (_geometry) {
            rtcReleaseGeometry(_geometry);
        }
        if (_scene) {
            rtcReleaseScene(_scene);
        }
    }

    GeometryScene(const GeometryScene&) = delete;
    GeometryScene& operator=(const GeometryScene&) = delete;

    RTCScene scene() const
    {
        return _scene;
    }

    bool needsRebuild(const UtilityScene::Geometry& source) const
    {
        return source.topologyVersion != _topologyVersion || source.points.size() != _pointCount || (_scene && !_committed);
    }

    void setCommitted()
    {
        _committed = true;
    }

    bool needsRefit(const UtilityScene::Geometry& source) const
    {
        return source.pointsVersion != _pointsVersion;
    }

    // Returns whether anything will need committing.
    bool refit(const UtilityScene::Geometry& source)
    {
        _pointsVersion = source.pointsVersion;
        if (!_scene) {
            return false;
        }
        _committed = false;
        std::memcpy(_vertices, source.points.cdata(), _pointCount * sizeof(GfVec3f));
        rtcUpdateGeometryBuffer(_geometry, RTC_BUFFER_TYPE_VERTEX, 0);
        rtcCommitGeometry(_geometry);

        return true;
    }

private:
    RTCGeometry _geometry = nullptr;
    RTCScene _scene = nullptr;
    GfVec3f* _vertices = nullptr;
    // Embree reads the triangles from this array's storage, so it has to outlive the geometry.
    VtVec3iArray _triangles;
    std::size_t _pointCount;
    std::uint64_t _topologyVersion;
    std::uint64_t _pointsVersion;
    bool _committed;
};

struct Placement {
    UtilityScene::GeometryId geometry;
    GfMatrix4d transform;
};

bool
sameGeometries(const std::vector<Placement>& a,
               const std::vector<Placement>& b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](const Placement& x, const Placement& y) { return x.geometry == y.geometry; });
}

void
setInstanceTransform(RTCGeometry instance,
                     const GfMatrix4d& transform)
{
    float matrix[16];
    const double* source = transform.data();

    // GfMatrix4d's row vectors lie in memory exactly as Embree's columns do.
    std::copy(source, source + 16, matrix);
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matrix);
}

void
fillRays(const Tracer::RayBatch& rays,
         std::size_t start,
         std::size_t count,
         int* valid,
         RTCRay16& packet)
{
    for (std::size_t i = 0; i < kPacketSize; ++i) {
        valid[i] = 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t ray = start + i;

        valid[i] = -1;
        packet.org_x[i] = rays.originX[ray];
        packet.org_y[i] = rays.originY[ray];
        packet.org_z[i] = rays.originZ[ray];
        packet.dir_x[i] = rays.directionX[ray];
        packet.dir_y[i] = rays.directionY[ray];
        packet.dir_z[i] = rays.directionZ[ray];
        packet.tnear[i] = rays.tMin[ray];
        packet.tfar[i] = rays.tMax[ray];
        packet.time[i] = 0.f;
        packet.mask[i] = 0xffffffffu;
        packet.id[i] = static_cast<unsigned int>(i);
        packet.flags[i] = 0;
    }
}
} // anon namespace

struct EmbreeTracer::Implementation {
    Implementation()
        // Threads stay unset here: on Embree's TBB build a thread count applies to the whole process,
        // USD's work included. update() caps each build with an arena instead.
        : device(rtcNewDevice(nullptr))
    {
        rtcSetDeviceErrorFunction(device, reportEmbreeError, nullptr);
    }

    ~Implementation()
    {
        releaseInstanceScene();
        geometries.clear();
        rtcReleaseDevice(device);
    }

    void releaseInstanceScene()
    {
        for (RTCGeometry instance : instances) {
            if (instance) {
                rtcReleaseGeometry(instance);
            }
        }
        instances.clear();
        if (instanceScene) {
            rtcReleaseScene(instanceScene);
            instanceScene = nullptr;
        }
    }

    // Returns false when stop ended it early.
    bool buildInstanceScene(const std::vector<Placement>& newPlacements,
                            const std::atomic<bool>* stop)
    {
        releaseInstanceScene();
        instanceScene = newRefittableScene(device);
        instances.assign(newPlacements.size(), nullptr);
        for (std::size_t i = 0; i < newPlacements.size(); ++i) {
            if (wasStopped(stop)) {
                return false;
            }

            const auto found = geometries.find(newPlacements[i].geometry);

            if (found == geometries.end() || !found->second->scene()) {
                continue;
            }
            RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);

            rtcSetGeometryInstancedScene(instance, found->second->scene());
            setInstanceTransform(instance, newPlacements[i].transform);
            rtcSetGeometryBuildQuality(instance, RTC_BUILD_QUALITY_REFIT);
            rtcCommitGeometry(instance);
            // The id Embree reports a hit under is the index into the snapshot's instances.
            rtcAttachGeometryByID(instanceScene, instance, static_cast<unsigned int>(i));
            instances[i] = instance;
        }

        return true;
    }

    // After a stop, whose update may have left any of the instance scene's parts half done. Geometry
    // scenes left uncommitted say so themselves.
    bool abandonUpdate()
    {
        releaseInstanceScene();
        placements.clear();
        synced = false;
        stopped = true;

        return false;
    }

    RTCDevice device;
    RTCScene instanceScene = nullptr;
    // Indexed like placements; null where the geometry has nothing to trace.
    std::vector<RTCGeometry> instances;
    std::map<UtilityScene::GeometryId, std::unique_ptr<GeometryScene>> geometries;
    std::vector<Placement> placements;
    // The dense geometry index of each entry of the snapshot's instances.
    std::vector<std::uint32_t> instanceGeometry;
    std::uint64_t version = 0;
    bool synced = false;
    // Embree's errors go to reportEmbreeError, which warns and lets the update go on, so only a stop
    // ends one early.
    bool stopped = false;
    Stats counters;
};

EmbreeTracer::EmbreeTracer()
    : _imp(new Implementation())
{
}

EmbreeTracer::~EmbreeTracer()
{
}

bool
EmbreeTracer::update(const UtilityScene::Snapshot& snapshot,
                     int threads,
                     const std::atomic<bool>* stop)
{
    Implementation& imp = *_imp;

    imp.stopped = false;
    if (imp.synced && imp.version == snapshot.version) {
        return true;
    }
    imp.synced = false;

    bool geometryReplaced = false;

    for (auto it = imp.geometries.begin(); it != imp.geometries.end();) {
        if (snapshot.geometries.count(it->first)) {
            ++it;
        } else {
            it = imp.geometries.erase(it);
            geometryReplaced = true;
        }
    }

    std::vector<GeometryScene*> toCommit;
    bool geometryRefitted = false;
    std::map<UtilityScene::GeometryId, std::uint32_t> denseIndices;
    std::uint32_t nextIndex = 0;

    for (const auto& entry : snapshot.geometries) {
        if (wasStopped(stop)) {
            return imp.abandonUpdate();
        }
        denseIndices[entry.first] = nextIndex++;

        std::unique_ptr<GeometryScene>& scene = imp.geometries[entry.first];

        if (!scene || scene->needsRebuild(entry.second)) {
            scene.reset(new GeometryScene(imp.device, entry.second));
            if (scene->scene()) {
                toCommit.push_back(scene.get());
                ++imp.counters.geometryBuilds;
            }
            geometryReplaced = true;
        } else if (scene->needsRefit(entry.second) && scene->refit(entry.second)) {
            toCommit.push_back(scene.get());
            ++imp.counters.geometryRefits;
            geometryRefitted = true;
        }
    }

    std::vector<Placement> placements;

    placements.reserve(snapshot.instances.size());
    imp.instanceGeometry.resize(snapshot.instances.size());
    for (std::size_t i = 0; i < snapshot.instances.size(); ++i) {
        const UtilityScene::Instance& instance = snapshot.instances[i];
        const auto dense = denseIndices.find(instance.geometry);

        placements.push_back(Placement { instance.geometry, instance.transform });
        imp.instanceGeometry[i] = dense == denseIndices.end() ? kNoHit : dense->second;
    }

    const bool rebuildInstances = !imp.instanceScene || geometryReplaced || !sameGeometries(placements, imp.placements);

    // Embree's TBB builds spawn into whichever arena commits them, which holds them to the budget.
    tbb::task_arena arena(std::max(1, threads));

    arena.execute([&]() {
        tbb::parallel_for(std::size_t(0), toCommit.size(), [&](std::size_t i) {
            if (!wasStopped(stop)) {
                commitCancellable(toCommit[i]->scene(), stop);
            }
        });
    });
    // Embree does not say which commits a stop cut short, so none of them counts.
    if (wasStopped(stop)) {
        return imp.abandonUpdate();
    }
    for (GeometryScene* scene : toCommit) {
        scene->setCommitted();
    }

    if (rebuildInstances) {
        if (!imp.buildInstanceScene(placements, stop)) {
            return imp.abandonUpdate();
        }
        imp.placements.swap(placements);
        arena.execute([&]() {
            commitCancellable(imp.instanceScene, stop);
        });
        ++imp.counters.instanceSceneBuilds;
    } else {
        bool instancesChanged = false;

        for (std::size_t i = 0; i < placements.size(); ++i) {
            if (wasStopped(stop)) {
                return imp.abandonUpdate();
            }

            RTCGeometry instance = imp.instances[i];
            const bool moved = placements[i].transform != imp.placements[i].transform;

            if (!instance || !(moved || geometryRefitted)) {
                continue;
            }
            if (moved) {
                setInstanceTransform(instance, placements[i].transform);
            }
            // Without moving, an instance still caches the bounds of what it places, which a refit moved.
            rtcCommitGeometry(instance);
            instancesChanged = true;
        }
        imp.placements.swap(placements);
        if (instancesChanged) {
            arena.execute([&]() {
                commitCancellable(imp.instanceScene, stop);
            });
            ++imp.counters.instanceSceneUpdates;
        }
    }
    if (wasStopped(stop)) {
        return imp.abandonUpdate();
    }

    imp.version = snapshot.version;
    imp.synced = true;

    return true;
}

void
EmbreeTracer::intersect(const RayBatch& rays,
                        const HitBatch& hits) const
{
    const Implementation& imp = *_imp;
    const ScopedFlushDenormals flush;

    for (std::size_t start = 0; start < rays.count; start += kPacketSize) {
        const std::size_t count = std::min(kPacketSize, rays.count - start);
        alignas(64) int valid[kPacketSize];
        RTCRayHit16 packet;

        fillRays(rays, start, count, valid, packet.ray);
        for (std::size_t i = 0; i < count; ++i) {
            packet.hit.geomID[i] = RTC_INVALID_GEOMETRY_ID;
            packet.hit.primID[i] = RTC_INVALID_GEOMETRY_ID;
            packet.hit.instID[0][i] = RTC_INVALID_GEOMETRY_ID;
        }
        if (imp.instanceScene) {
            rtcIntersect16(valid, imp.instanceScene, &packet, nullptr);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t ray = start + i;

            if (packet.hit.geomID[i] == RTC_INVALID_GEOMETRY_ID) {
                hits.t[ray] = std::numeric_limits<float>::infinity();
                hits.u[ray] = 0.f;
                hits.v[ray] = 0.f;
                hits.geometry[ray] = kNoHit;
                hits.instance[ray] = kNoHit;
                hits.primitive[ray] = kNoHit;
            } else {
                const unsigned int instance = packet.hit.instID[0][i];

                hits.t[ray] = packet.ray.tfar[i];
                hits.u[ray] = packet.hit.u[i];
                hits.v[ray] = packet.hit.v[i];
                hits.geometry[ray] = imp.instanceGeometry[instance];
                hits.instance[ray] = instance;
                hits.primitive[ray] = packet.hit.primID[i];
            }
        }
    }
}

void
EmbreeTracer::occluded(const RayBatch& rays,
                       std::uint8_t* blocked,
                       const OcclusionFilter* filter) const
{
    const Implementation& imp = *_imp;
    const ScopedFlushDenormals flush;

    for (std::size_t start = 0; start < rays.count; start += kPacketSize) {
        const std::size_t count = std::min(kPacketSize, rays.count - start);
        alignas(64) int valid[kPacketSize];
        RTCRay16 packet;

        fillRays(rays, start, count, valid, packet);
        if (imp.instanceScene) {
            if (filter && filter->casterMasks) {
                OcclusionContext context;
                RTCOccludedArguments arguments;

                rtcInitRayQueryContext(&context.context);
                context.start = start;
                context.filter = filter;
                rtcInitOccludedArguments(&arguments);
                arguments.flags = static_cast<RTCRayQueryFlags>(arguments.flags | RTC_RAY_QUERY_FLAG_INVOKE_ARGUMENT_FILTER);
                arguments.context = &context.context;
                arguments.filter = filterOccluders;
                rtcOccluded16(valid, imp.instanceScene, &packet, &arguments);
            } else {
                rtcOccluded16(valid, imp.instanceScene, &packet, nullptr);
            }
        }
        // Embree marks an occluded ray by setting its tfar to negative infinity.
        for (std::size_t i = 0; i < count; ++i) {
            blocked[start + i] = packet.tfar[i] == -std::numeric_limits<float>::infinity() ? 1 : 0;
        }
    }
}

Tracer::Stats
EmbreeTracer::stats() const
{
    return _imp->counters;
}

Tracer::Outcome
EmbreeTracer::status() const
{
    if (_imp->stopped) {
        return Outcome { Status::Stopped, "EmbreeTracer: the update was stopped" };
    }

    return Outcome();
}

NATRON_NAMESPACE_EXIT
