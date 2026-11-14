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

#include "GpuTracer.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <pxr/base/tf/diagnostic.h>

#include "Engine/Gpu/GpuAccel.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Scene/Utility/UtilityGpuLimits.h"

#include "utility_intersect_spirv.h"
#include "utility_occluded_spirv.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Mirrors IntersectParams in Kernels/Intersect.slang.
struct KernelIntersectParams {
    std::uint32_t sceneLow;
    std::uint32_t sceneHigh;
    std::uint32_t stride;
    std::uint32_t first;
    std::uint32_t end;
    std::uint32_t maskStride;
    std::uint32_t maskOffset;
};

static_assert(sizeof(KernelIntersectParams) == utility_intersect_push_bytes);
static_assert(sizeof(KernelIntersectParams) == utility_occluded_push_bytes);
static_assert(utility_intersect_storage_buffers == 6 && utility_occluded_storage_buffers == 6);

// The device form of an OcclusionFilter.
struct DeviceFilter {
    const gpu::GpuBuffer* rayLights = nullptr;
    const gpu::GpuBuffer* casterMasks = nullptr;
    std::uint32_t maskStride = 0;
    std::uint32_t maskOffset = 0;
};

const std::size_t kRayArrays = kUtilityRayArrays;
const std::size_t kHitArrays = kUtilityHitArrays;
const std::uint64_t kMaxDispatchRays = kUtilityMaxDispatchRays;

// Bounds the host-visible memory one trace call stages.
const std::size_t kMaxRaysPerSubmit = std::size_t(1) << 20;

// Instance custom indices have 24 bits.
const std::size_t kMaxInstances = std::size_t(1) << 24;

enum class Entry {
    Intersect,
    Occluded,
};

bool
wasStopped(const std::atomic<bool>* stop)
{
    return stop && stop->load(std::memory_order_relaxed);
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

GpuTracer::Status
classify(const gpu::GpuStatus& status,
         const gpu::GpuDevice& device)
{
    return classifyUtilityGpuStatus(status, device);
}

// GfMatrix4d maps row vectors with its translation in the last row; the structure wants the transpose.
std::array<float, 12>
objectToWorld(const GfMatrix4d& matrix)
{
    std::array<float, 12> rows;

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            rows[row * 4 + column] = static_cast<float>(matrix[column][row]);
        }
    }

    return rows;
}

void
writeMisses(const Tracer::HitBatch& hits,
            std::size_t first,
            std::size_t end)
{
    for (std::size_t ray = first; ray < end; ++ray) {
        hits.t[ray] = std::numeric_limits<float>::infinity();
        hits.u[ray] = 0.f;
        hits.v[ray] = 0.f;
        hits.geometry[ray] = Tracer::kNoHit;
        hits.instance[ray] = Tracer::kNoHit;
        hits.primitive[ray] = Tracer::kNoHit;
    }
}

struct GeometryEntry {
    std::uint64_t topologyVersion = 0;
    std::uint64_t pointsVersion = 0;
    std::size_t pointCount = 0;
    std::size_t triangleCount = 0;
    // Host-visible, because the record context has no upload of its own; the builds read them in place.
    gpu::GpuBuffer vertices;
    gpu::GpuBuffer indices;
    // Empty for geometry that does not draw.
    gpu::GpuBlas blas;

    bool needsRebuild(const UtilityScene::Geometry& source) const
    {
        return source.topologyVersion != topologyVersion || source.points.size() != pointCount;
    }

    gpu::GpuTriangles triangles() const
    {
        gpu::GpuTriangles result;

        result.vertices = &vertices;
        result.vertexCount = static_cast<std::uint32_t>(pointCount);
        result.indices = &indices;
        result.triangleCount = static_cast<std::uint32_t>(triangleCount);

        return result;
    }
};

using GeometryEntryPtr = std::shared_ptr<GeometryEntry>;

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

// What a trace reads. Contexts that record traces hold it, so an update can replace it under them.
struct SceneState {
    gpu::GpuTlas tlas;
    gpu::GpuBuffer instanceGeometry;
    std::vector<GeometryEntryPtr> placed;
};

using SceneStatePtr = std::shared_ptr<const SceneState>;

struct Failure {
    GpuTracer::Status status;
    std::string message;
};
} // anon namespace

struct GpuTracer::Implementation {
    Implementation(gpu::GpuKernelRegistry& registry,
                   gpu::GpuBufferPool& pool)
        : registry(registry)
        , pool(pool)
        , device(registry.device())
    {
    }

    gpu::GpuStatus createContext(std::unique_ptr<gpu::GpuRecordContext>& ctx) const
    {
        return gpu::GpuRecordContext::create(registry, ctx, &pool);
    }

    Failure failure(const std::string& what,
                    const gpu::GpuStatus& status) const
    {
        return Failure { classify(status, device), what + ": " + status.message };
    }

    void record(const Failure& failure) const
    {
        std::lock_guard<std::mutex> lock(outcomeMutex);

        if (outcome.status == Status::Ok) {
            outcome.status = failure.status;
            outcome.message = failure.message;
        }
    }

    bool abandonUpdate(const Failure& failure)
    {
        scene.reset();
        synced = false;
        record(failure);

        return false;
    }

    // A failed submission leaves unknown which builds and refits ran, so nothing built is trusted.
    bool abandonSubmission(const std::string& what,
                           const gpu::GpuStatus& status)
    {
        Failure failed = failure(what, status);

        if (failed.status != Status::DeviceLost) {
            failed.status = Status::Failed;
        }
        geometries.clear();
        placements.clear();
        uncompacted.clear();

        return abandonUpdate(failed);
    }

    gpu::GpuStatus recordTrace(gpu::GpuRecordContext& ctx,
                               Entry entry,
                               const SceneStatePtr& state,
                               const gpu::GpuBuffer& rays,
                               std::uint32_t count,
                               const gpu::GpuBuffer& values,
                               const gpu::GpuBuffer& ids,
                               const DeviceFilter& filter = DeviceFilter()) const;

    void trace(Entry entry,
               const RayBatch& rays,
               const HitBatch* hits,
               std::uint8_t* blocked,
               const OcclusionFilter* filter = nullptr) const;

    gpu::GpuKernelRegistry& registry;
    gpu::GpuBufferPool& pool;
    gpu::GpuDevice& device;
    std::shared_ptr<gpu::GpuKernel> intersectKernel;
    std::shared_ptr<gpu::GpuKernel> occludedKernel;
    bool usable = false;

    std::map<UtilityScene::GeometryId, GeometryEntryPtr> geometries;
    // Built but not yet compacted, because the update that built them was stopped first.
    std::set<UtilityScene::GeometryId> uncompacted;
    std::vector<Placement> placements;
    SceneStatePtr scene;
    std::uint64_t version = 0;
    bool synced = false;
    Stats counters;

    mutable std::mutex outcomeMutex;
    mutable Outcome outcome;
};

gpu::GpuStatus
GpuTracer::Implementation::recordTrace(gpu::GpuRecordContext& ctx,
                                       Entry entry,
                                       const SceneStatePtr& state,
                                       const gpu::GpuBuffer& rays,
                                       std::uint32_t count,
                                       const gpu::GpuBuffer& values,
                                       const gpu::GpuBuffer& ids,
                                       const DeviceFilter& filter) const
{
    if (!usable) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, outcome.message };
    }

    VkDeviceAddress address = 0;

    if (state && state->tlas) {
        address = state->tlas.deviceAddress();
        ctx.hold(std::const_pointer_cast<SceneState>(state));
    }

    // Without a scene the kernel reads nothing past the rays, so they stand in for the instance table.
    const gpu::GpuBuffer& instanceGeometry = address ? state->instanceGeometry : rays;
    const bool filtered = filter.maskStride != 0 && filter.rayLights && filter.casterMasks;
    // Unfiltered, the kernel never reads the light tables, so the rays stand in for them.
    const std::array<gpu::GpuBinding, 6> bindings = {
        gpu::GpuBinding::storage(0, rays),
        gpu::GpuBinding::storage(1, instanceGeometry),
        gpu::GpuBinding::storage(2, values),
        gpu::GpuBinding::storage(3, ids),
        gpu::GpuBinding::storage(4, filtered ? *filter.rayLights : rays),
        gpu::GpuBinding::storage(5, filtered ? *filter.casterMasks : rays),
    };
    const std::shared_ptr<gpu::GpuKernel>& kernel = entry == Entry::Intersect ? intersectKernel : occludedKernel;

    for (std::uint64_t first = 0; first < count; first += kMaxDispatchRays) {
        const std::uint64_t end = std::min<std::uint64_t>(count, first + kMaxDispatchRays);
        const KernelIntersectParams params {
            static_cast<std::uint32_t>(address & 0xffffffffull),
            static_cast<std::uint32_t>(address >> 32),
            count,
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(end),
            filtered ? filter.maskStride : 0u,
            filtered ? filter.maskOffset : 0u,
        };
        const std::array<std::uint32_t, 3> threads = { static_cast<std::uint32_t>(end - first), 1, 1 };

        if (gpu::GpuStatus s = ctx.dispatchBindings(kernel, bindings, std::as_bytes(std::span(&params, 1)), threads); !s) {
            return s;
        }
    }

    return {};
}

void
GpuTracer::Implementation::trace(Entry entry,
                                 const RayBatch& rays,
                                 const HitBatch* hits,
                                 std::uint8_t* blocked,
                                 const OcclusionFilter* filter) const
{
    const SceneStatePtr state = scene;

    if (rays.count == 0) {
        return;
    }

    auto missFrom = [&](std::size_t first) {
        if (hits) {
            writeMisses(*hits, first, rays.count);
        } else {
            std::fill(blocked + first, blocked + rays.count, std::uint8_t(0));
        }
    };

    if (!state || !state->tlas) {
        missFrom(0);

        return;
    }

    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (gpu::GpuStatus s = createContext(ctx); !s) {
        record(failure("GpuTracer could not create a record context", s));
        missFrom(0);

        return;
    }

    const std::size_t bindable = static_cast<std::size_t>(ctx->maxStorageBufferBytes() / (kRayArrays * sizeof(float)));
    const std::size_t chunk = std::min({ rays.count, kMaxRaysPerSubmit, bindable });

    if (chunk == 0) {
        record(Failure { Status::Failed, "GpuTracer: the device binds no storage buffer large enough for one ray" });
        missFrom(0);

        return;
    }

    const std::size_t outputArrays = hits ? kHitArrays : 1;
    gpu::GpuBuffer rayBuffer, valueBuffer, idBuffer;
    const VkDeviceSize rayBytes = kRayArrays * chunk * sizeof(float);
    const VkDeviceSize outputBytes = outputArrays * chunk * sizeof(std::uint32_t);

    for (gpu::GpuBuffer* buffer : { &rayBuffer, &valueBuffer, &idBuffer }) {
        const VkDeviceSize bytes = buffer == &rayBuffer ? rayBytes : outputBytes;

        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(device, bytes, gpu::kGpuBufferDefaultUsage, *buffer); !s) {
            record(failure("GpuTracer could not stage a batch of " + std::to_string(chunk) + " rays in "
                               + std::to_string(bytes) + " bytes",
                           s));
            missFrom(0);

            return;
        }
    }

    const float* const sources[kRayArrays] = { rays.originX, rays.originY, rays.originZ,
                                               rays.directionX, rays.directionY, rays.directionZ,
                                               rays.tMin, rays.tMax };
    const bool filtered = !hits && filter && filter->casterMasks && filter->stride != 0;
    gpu::GpuBuffer lightBuffer, maskBuffer;
    DeviceFilter deviceFilter;

    if (filtered) {
        const std::size_t maskWords = static_cast<std::size_t>(filter->stride) * filter->casterCount;
        const VkDeviceSize lightBytes = chunk * sizeof(std::uint32_t);
        const VkDeviceSize maskBytes = std::max<std::size_t>(1, maskWords) * sizeof(std::uint32_t);

        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(device, lightBytes, gpu::kGpuBufferDefaultUsage, lightBuffer); !s) {
            record(failure("GpuTracer could not stage the lights of " + std::to_string(chunk) + " shadow rays", s));
            missFrom(0);

            return;
        }
        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(device, maskBytes, gpu::kGpuBufferDefaultUsage, maskBuffer); !s) {
            record(failure("GpuTracer could not stage " + std::to_string(maskWords) + " words of shadow-link masks", s));
            missFrom(0);

            return;
        }
        std::memcpy(maskBuffer.mapped(), filter->casterMasks, maskWords * sizeof(std::uint32_t));
        deviceFilter = DeviceFilter { &lightBuffer, &maskBuffer, filter->stride, 0u };
    }

    for (std::size_t start = 0; start < rays.count; start += chunk) {
        const std::size_t n = std::min(chunk, rays.count - start);
        float* staged = static_cast<float*>(rayBuffer.mapped());

        for (std::size_t array = 0; array < kRayArrays; ++array) {
            std::memcpy(staged + array * n, sources[array] + start, n * sizeof(float));
        }
        if (filtered) {
            std::memcpy(lightBuffer.mapped(), filter->rayLights + start, n * sizeof(std::uint32_t));
        }
        if (start > 0) {
            if (gpu::GpuStatus s = createContext(ctx); !s) {
                record(failure("GpuTracer could not create a record context", s));
                missFrom(start);

                return;
            }
        }

        gpu::GpuStatus s = recordTrace(*ctx, entry, state, rayBuffer, static_cast<std::uint32_t>(n), valueBuffer, idBuffer, deviceFilter);

        if (s) {
            s = ctx->submitAndWait();
        }
        ctx.reset();
        if (!s) {
            record(failure(entry == Entry::Intersect ? "GpuTracer could not intersect" : "GpuTracer could not answer occlusion", s));
            missFrom(start);

            return;
        }

        const std::uint32_t* ids = static_cast<const std::uint32_t*>(idBuffer.mapped());

        if (hits) {
            const float* values = static_cast<const float*>(valueBuffer.mapped());

            // The arrays follow the order of Kernels/UtilityTypes.slang.
            std::memcpy(hits->t + start, values, n * sizeof(float));
            std::memcpy(hits->u + start, values + n, n * sizeof(float));
            std::memcpy(hits->v + start, values + 2 * n, n * sizeof(float));
            std::memcpy(hits->geometry + start, ids, n * sizeof(std::uint32_t));
            std::memcpy(hits->instance + start, ids + n, n * sizeof(std::uint32_t));
            std::memcpy(hits->primitive + start, ids + 2 * n, n * sizeof(std::uint32_t));
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                blocked[start + i] = ids[i] ? 1 : 0;
            }
        }
    }
}

GpuTracer::GpuTracer(gpu::GpuKernelRegistry& registry,
                     gpu::GpuBufferPool& pool)
    : _imp(new Implementation(registry, pool))
{
    Implementation& imp = *_imp;
    const std::string missing = missingRayQuerySupport(imp.device);

    if (!missing.empty()) {
        imp.outcome = Outcome { Status::Unsupported, missing };

        return;
    }

    const gpu::GpuKernelDesc intersectDesc {
        utility_intersect_spirv, utility_intersect_spirv_words, utility_intersect_entry, utility_intersect_storage_buffers, utility_intersect_push_bytes, { utility_intersect_group_size[0], utility_intersect_group_size[1], utility_intersect_group_size[2] }
    };
    const gpu::GpuKernelDesc occludedDesc {
        utility_occluded_spirv, utility_occluded_spirv_words, utility_occluded_entry, utility_occluded_storage_buffers, utility_occluded_push_bytes, { utility_occluded_group_size[0], utility_occluded_group_size[1], utility_occluded_group_size[2] }
    };
    std::unique_ptr<gpu::GpuKernel> kernel;

    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, intersectDesc, kernel); !s) {
        imp.outcome = Outcome { Status::Unsupported, "GpuTracer could not create its intersection kernel: " + s.message };

        return;
    }
    imp.intersectKernel = std::move(kernel);
    if (gpu::GpuStatus s = gpu::GpuKernel::create(imp.device, occludedDesc, kernel); !s) {
        imp.outcome = Outcome { Status::Unsupported, "GpuTracer could not create its occlusion kernel: " + s.message };

        return;
    }
    imp.occludedKernel = std::move(kernel);
    imp.usable = true;
}

GpuTracer::~GpuTracer()
{
}

std::string
GpuTracer::missingRayQuerySupport(const gpu::GpuDevice& device)
{
    if (device.supportsRayQuery()) {
        return std::string();
    }

    return device.info().name + " lacks VK_KHR_acceleration_structure, VK_KHR_ray_query, VK_KHR_deferred_host_operations "
                                "or the bufferDeviceAddress feature, so it cannot trace rays";
}

bool
GpuTracer::update(const UtilityScene::Snapshot& snapshot,
                  int /*threads*/,
                  const std::atomic<bool>* stop)
{
    Implementation& imp = *_imp;

    if (!imp.usable) {
        return false;
    }
    if (imp.synced && imp.version == snapshot.version) {
        return true;
    }
    imp.synced = false;
    {
        std::lock_guard<std::mutex> lock(imp.outcomeMutex);

        imp.outcome = Outcome();
    }

    const Failure stopped { Status::Stopped, "GpuTracer: the update was stopped" };
    bool geometryReplaced = false;

    for (auto it = imp.geometries.begin(); it != imp.geometries.end();) {
        if (snapshot.geometries.count(it->first)) {
            ++it;
        } else {
            it = imp.geometries.erase(it);
            geometryReplaced = true;
        }
    }

    struct PendingBuild {
        UtilityScene::GeometryId id;
        GeometryEntryPtr entry;
    };
    struct PendingRefit {
        GeometryEntry* entry;
        const UtilityScene::Geometry* source;
    };
    std::vector<PendingBuild> builds;
    std::vector<PendingRefit> refits;

    for (const auto& item : snapshot.geometries) {
        if (wasStopped(stop)) {
            return imp.abandonUpdate(stopped);
        }

        const UtilityScene::Geometry& source = item.second;
        const auto existing = imp.geometries.find(item.first);

        if (existing == imp.geometries.end() || existing->second->needsRebuild(source)) {
            GeometryEntryPtr entry = std::make_shared<GeometryEntry>();

            entry->topologyVersion = source.topologyVersion;
            entry->pointsVersion = source.pointsVersion;
            entry->pointCount = source.points.size();
            geometryReplaced = true;
            if (source.points.empty() || source.triangles.empty()) {
                imp.geometries[item.first] = entry;
                continue;
            }
            if (!trianglesAreInRange(source)) {
                TF_WARN("%s has triangles outside its %zu points and will not draw", source.path.GetText(), source.points.size());
                imp.geometries[item.first] = entry;
                continue;
            }
            entry->triangleCount = source.triangles.size();

            const VkDeviceSize vertexBytes = entry->pointCount * sizeof(GfVec3f);
            const VkDeviceSize indexBytes = entry->triangleCount * sizeof(GfVec3i);

            if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, vertexBytes, gpu::kGpuAccelInputUsage, entry->vertices); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not hold the " + std::to_string(vertexBytes) + " bytes of points of "
                                                         + source.path.GetString(),
                                                     s));
            }
            if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, indexBytes, gpu::kGpuAccelInputUsage, entry->indices); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not hold the " + std::to_string(indexBytes) + " bytes of triangles of "
                                                         + source.path.GetString(),
                                                     s));
            }
            std::memcpy(entry->vertices.mapped(), source.points.cdata(), vertexBytes);
            // GfVec3i holds three ints, which the range check made valid as uint32 indices.
            std::memcpy(entry->indices.mapped(), source.triangles.cdata(), indexBytes);
            builds.push_back(PendingBuild { item.first, std::move(entry) });
        } else if (source.pointsVersion != existing->second->pointsVersion) {
            if (existing->second->blas) {
                refits.push_back(PendingRefit { existing->second.get(), &source });
            } else {
                existing->second->pointsVersion = source.pointsVersion;
            }
        }
    }

    // The builds of one submission share a scratch buffer that grows only when a build outsizes it, and the
    // scratch a build needs grows with its triangles, so the largest goes first.
    std::stable_sort(builds.begin(), builds.end(), [](const PendingBuild& a, const PendingBuild& b) {
        return a.entry->triangleCount > b.entry->triangleCount;
    });

    if (!builds.empty() || !refits.empty()) {
        std::unique_ptr<gpu::GpuRecordContext> ctx;

        if (gpu::GpuStatus s = imp.createContext(ctx); !s) {
            return imp.abandonUpdate(imp.failure("GpuTracer could not create a record context", s));
        }

        gpu::GpuBlasOptions options;

        options.allowUpdate = true;
        options.allowCompaction = true;
        for (PendingBuild& build : builds) {
            if (wasStopped(stop)) {
                return imp.abandonUpdate(stopped);
            }
            if (gpu::GpuStatus s = gpu::GpuBlas::build(*ctx, imp.pool, build.entry->triangles(), options, build.entry->blas); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not build a geometry's structure", s));
            }
        }
        for (const PendingRefit& refit : refits) {
            if (wasStopped(stop)) {
                return imp.abandonUpdate(stopped);
            }
            std::memcpy(refit.entry->vertices.mapped(), refit.source->points.cdata(), refit.entry->pointCount * sizeof(GfVec3f));
            if (gpu::GpuStatus s = refit.entry->blas.refit(*ctx, refit.entry->triangles()); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not refit a geometry's structure", s));
            }
        }
        if (wasStopped(stop)) {
            return imp.abandonUpdate(stopped);
        }
        if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
            return imp.abandonSubmission("GpuTracer could not build its geometry", s);
        }
        for (PendingBuild& build : builds) {
            imp.geometries[build.id] = std::move(build.entry);
            imp.uncompacted.insert(build.id);
            ++imp.counters.geometryBuilds;
        }
        for (const PendingRefit& refit : refits) {
            refit.entry->pointsVersion = refit.source->pointsVersion;
            ++imp.counters.geometryRefits;
        }
    }

    for (auto it = imp.uncompacted.begin(); it != imp.uncompacted.end();) {
        const auto entry = imp.geometries.find(*it);

        it = (entry == imp.geometries.end() || !entry->second->blas) ? imp.uncompacted.erase(it) : std::next(it);
    }
    // Compaction gets its own submission: the copy replaces each structure as it is recorded, so it has to have
    // run before anything is built over them. A stopped update leaves the list for the next one.
    if (!imp.uncompacted.empty() && !wasStopped(stop)) {
        std::unique_ptr<gpu::GpuRecordContext> ctx;

        if (gpu::GpuStatus s = imp.createContext(ctx); !s) {
            return imp.abandonUpdate(imp.failure("GpuTracer could not create a record context", s));
        }
        for (const UtilityScene::GeometryId id : imp.uncompacted) {
            GeometryEntry& entry = *imp.geometries[id];

            if (gpu::GpuStatus s = entry.blas.compact(*ctx, imp.pool); !s && classify(s, imp.device) != Status::OutOfMemory) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not compact a geometry's structure", s));
            }
        }
        if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
            return imp.abandonSubmission("GpuTracer could not compact its geometry", s);
        }
        // A structure the pool had no room to compact into stays as built.
        imp.uncompacted.clear();
        geometryReplaced = true;
    }
    const bool geometryRefitted = !refits.empty();

    std::map<UtilityScene::GeometryId, std::uint32_t> denseIndices;
    std::uint32_t nextIndex = 0;

    for (const auto& item : snapshot.geometries) {
        denseIndices[item.first] = nextIndex++;
    }

    std::vector<Placement> placements;

    placements.reserve(snapshot.instances.size());
    for (const UtilityScene::Instance& instance : snapshot.instances) {
        placements.push_back(Placement { instance.geometry, instance.transform });
    }

    auto drawnBy = [&](std::size_t instance) -> const GeometryEntry* {
        const auto it = imp.geometries.find(placements[instance].geometry);

        return it != imp.geometries.end() && it->second->blas ? it->second.get() : nullptr;
    };

    const bool rebuildInstances = !imp.scene || geometryReplaced || !sameGeometries(placements, imp.placements);
    bool instancesChanged = rebuildInstances;

    for (std::size_t i = 0; i < placements.size() && !instancesChanged; ++i) {
        if (drawnBy(i) && (geometryRefitted || placements[i].transform != imp.placements[i].transform)) {
            instancesChanged = true;
        }
    }

    if (instancesChanged) {
        if (wasStopped(stop)) {
            return imp.abandonUpdate(stopped);
        }
        if (placements.size() > kMaxInstances) {
            return imp.abandonUpdate(Failure { Status::Failed, "GpuTracer: " + std::to_string(placements.size()) + " instances exceed the 2^24 a ray query can tell apart" });
        }

        std::shared_ptr<SceneState> state = std::make_shared<SceneState>();
        std::vector<gpu::GpuAccelInstance> instances;
        const VkDeviceSize tableBytes = std::max<std::size_t>(1, placements.size()) * sizeof(std::uint32_t);

        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, tableBytes, gpu::kGpuBufferDefaultUsage, state->instanceGeometry); !s) {
            return imp.abandonUpdate(imp.failure(("GpuTracer could not hold its " + std::to_string(tableBytes) + "-byte instance table"), s));
        }

        std::uint32_t* table = static_cast<std::uint32_t*>(state->instanceGeometry.mapped());

        table[0] = kNoHit;
        for (std::size_t i = 0; i < placements.size(); ++i) {
            const auto dense = denseIndices.find(placements[i].geometry);

            table[i] = dense == denseIndices.end() ? kNoHit : dense->second;

            const GeometryEntry* geometry = drawnBy(i);

            if (!geometry) {
                continue;
            }

            gpu::GpuAccelInstance instance;

            instance.transform = objectToWorld(placements[i].transform);
            instance.customIndex = static_cast<std::uint32_t>(i);
            instance.blas = &geometry->blas;
            instances.push_back(instance);
            state->placed.push_back(imp.geometries[placements[i].geometry]);
        }

        if (!instances.empty()) {
            std::unique_ptr<gpu::GpuRecordContext> ctx;

            if (gpu::GpuStatus s = imp.createContext(ctx); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not create a record context", s));
            }
            if (gpu::GpuStatus s = gpu::GpuTlas::build(*ctx, imp.pool, instances, state->tlas); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not build its instance structure", s));
            }
            if (wasStopped(stop)) {
                return imp.abandonUpdate(stopped);
            }
            if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
                return imp.abandonSubmission("GpuTracer could not build its instance structure", s);
            }
        }
        imp.scene = std::move(state);
        if (rebuildInstances) {
            ++imp.counters.instanceSceneBuilds;
        } else {
            ++imp.counters.instanceSceneUpdates;
        }
    }
    imp.placements.swap(placements);
    if (wasStopped(stop)) {
        return imp.abandonUpdate(stopped);
    }

    imp.version = snapshot.version;
    imp.synced = true;

    return true;
}

void
GpuTracer::intersect(const RayBatch& rays,
                     const HitBatch& hits) const
{
    _imp->trace(Entry::Intersect, rays, &hits, nullptr);
}

void
GpuTracer::occluded(const RayBatch& rays,
                    std::uint8_t* blocked,
                    const OcclusionFilter* filter) const
{
    _imp->trace(Entry::Occluded, rays, nullptr, blocked, filter);
}

Tracer::Stats
GpuTracer::stats() const
{
    return _imp->counters;
}

GpuTracer::Outcome
GpuTracer::status() const
{
    std::lock_guard<std::mutex> lock(_imp->outcomeMutex);

    return _imp->outcome;
}

gpu::GpuStatus
GpuTracer::recordIntersect(gpu::GpuRecordContext& ctx,
                           const gpu::GpuBuffer& rays,
                           std::uint32_t count,
                           const gpu::GpuBuffer& hitValues,
                           const gpu::GpuBuffer& hitIds) const
{
    return _imp->recordTrace(ctx, Entry::Intersect, _imp->scene, rays, count, hitValues, hitIds);
}

gpu::GpuStatus
GpuTracer::recordOccluded(gpu::GpuRecordContext& ctx,
                          const gpu::GpuBuffer& rays,
                          std::uint32_t count,
                          const gpu::GpuBuffer& blocked) const
{
    return _imp->recordTrace(ctx, Entry::Occluded, _imp->scene, rays, count, blocked, blocked);
}

gpu::GpuStatus
GpuTracer::recordOccludedFiltered(gpu::GpuRecordContext& ctx,
                                  const gpu::GpuBuffer& rays,
                                  std::uint32_t count,
                                  const gpu::GpuBuffer& blocked,
                                  const gpu::GpuBuffer& rayLights,
                                  const gpu::GpuBuffer& casterMasks,
                                  std::uint32_t maskStride,
                                  std::uint32_t maskOffset) const
{
    return _imp->recordTrace(ctx, Entry::Occluded, _imp->scene, rays, count, blocked, blocked, DeviceFilter { &rayLights, &casterMasks, maskStride, maskOffset });
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
