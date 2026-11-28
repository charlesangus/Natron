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
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <pxr/base/gf/range3d.h>
#include <pxr/base/tf/diagnostic.h>

#include "Engine/Gpu/GpuAccel.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRaster.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Scene/Utility/UtilityGpuLimits.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"

#include "utility_intersect_spirv.h"
#include "utility_occluded_spirv.h"
#include "utility_raster_visibility_spirv.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Mirrors IntersectParams in Kernels/Intersect.slang.
struct KernelIntersectParams {
    std::uint32_t structuresLow;
    std::uint32_t structuresHigh;
    std::uint32_t orderLow;
    std::uint32_t orderHigh;
    std::uint32_t casterMasksLow;
    std::uint32_t casterMasksHigh;
    std::uint32_t moversLow;
    std::uint32_t moversHigh;
    std::uint32_t moverRecordsLow;
    std::uint32_t moverRecordsHigh;
    std::uint32_t moverFirstLow;
    std::uint32_t moverFirstHigh;
    std::uint32_t structureCount;
    std::uint32_t stride;
    std::uint32_t first;
    std::uint32_t end;
    std::uint32_t maskStride;
    std::uint32_t maskOffset;
    std::uint32_t segments;
    std::uint32_t unused;
};

static_assert(sizeof(KernelIntersectParams) == utility_intersect_push_bytes);
static_assert(sizeof(KernelIntersectParams) == utility_occluded_push_bytes);
static_assert(utility_intersect_storage_buffers == 5 && utility_occluded_storage_buffers == 5);

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
// A trace submission slower than this halves the rays of those after it, down to kMinRaysPerSubmit, so that an
// expensive scene stays well inside the time a driver gives one job before it resets the device.
const std::chrono::milliseconds kSlowSubmission(1000);
const std::size_t kMinRaysPerSubmit = std::size_t(1) << 12;

// Instance custom indices have 24 bits, so past this many a scene's slots are listed in an order table.
const std::size_t kMaxInstances = std::size_t(1) << 24;

// What the kernels read through device addresses need that usage on top of the binding's.
const VkBufferUsageFlags kAddressedUsage = gpu::kGpuBufferDefaultUsage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

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

// The time steps of the points a geometry deforms through over the shutter, or 1 when it holds still.
std::size_t
deformSteps(const UtilityScene::Geometry& geometry,
            const UtilityScene::Snapshot& snapshot)
{
    const std::size_t steps = geometry.pointSteps.size();

    if (steps < 2 || steps != static_cast<std::size_t>(snapshot.timeSteps)) {
        return 1;
    }
    for (const VtVec3fArray& points : geometry.pointSteps) {
        if (points.size() != geometry.points.size()) {
            return 1;
        }
    }

    return steps;
}

struct GeometryEntry {
    std::uint64_t topologyVersion = 0;
    std::uint64_t pointsVersion = 0;
    std::size_t pointCount = 0;
    std::size_t triangleCount = 0;
    // Above 1 for a deforming geometry, whose vertices hold each step's points one after another and which a
    // ray tests exactly at its time inside boxes swept over each segment, instead of in a triangle structure.
    std::size_t pointSteps = 1;
    // Host-visible, because the record context has no upload of its own; the builds read them in place.
    gpu::GpuBuffer vertices;
    gpu::GpuBuffer indices;
    // Empty for geometry that does not draw, for a deforming one, and for one only raster updates have seen.
    gpu::GpuBlas blas;
    // The points moved under a raster update, which leaves the structure for the next traced update to refit.
    bool refitPending = false;
    // A deforming geometry's per-segment boxes, structures over them and one-instance structures over those,
    // built by the first update that places it.
    gpu::GpuBuffer sweptBoxes;
    std::vector<gpu::GpuBlas> swept;
    std::vector<gpu::GpuTlas> sweptScenes;

    bool deforms() const { return pointSteps > 1 && triangleCount > 0; }

    bool draws() const { return triangleCount > 0; }

    bool needsRebuild(const UtilityScene::Geometry& source,
                      const UtilityScene::Snapshot& snapshot) const
    {
        return source.topologyVersion != topologyVersion || source.points.size() != pointCount
            || deformSteps(source, snapshot) != pointSteps || (pointSteps > 1 && source.pointsVersion != pointsVersion);
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
    // The instance's transform steps over the shutter; empty when it holds still.
    std::vector<UtilityScene::Decomposition> steps;
};

bool
sameDecomposition(const UtilityScene::Decomposition& a,
                  const UtilityScene::Decomposition& b)
{
    return a.translation == b.translation && a.rotation == b.rotation && std::equal(a.stretch, a.stretch + 6, b.stretch);
}

bool
samePlace(const Placement& a,
          const Placement& b)
{
    return a.transform == b.transform && a.steps.size() == b.steps.size()
        && std::equal(a.steps.begin(), a.steps.end(), b.steps.begin(), sameDecomposition);
}

bool
sameGeometries(const std::vector<Placement>& a,
               const std::vector<Placement>& b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](const Placement& x, const Placement& y) { return x.geometry == y.geometry; });
}

// A RasterScene and what it points into: its own tables, and the geometry it draws, which a later update may
// replace in the tracer while a context still holds this.
struct RasterTables : GpuTracer::RasterScene {
    gpu::GpuBuffer instanceTable;
    gpu::GpuBuffer transformTable;
    gpu::GpuBuffer commandTable;
    gpu::GpuBuffer hitTable;
    std::vector<GeometryEntryPtr> drawn;
};

// What a trace reads. Contexts that record traces hold it, so an update can replace it under them.
struct SceneState {
    std::vector<gpu::GpuTlas> structures;
    // Four words per structure, as Kernels/Intersect.slang reads them, then four zero words. Llvmpipe runs a
    // loop's body once more after its last lane breaks, and loads an address every lane shares even with no
    // lane active, so the kernel reads the entry past the last structure; zero is a structure it never traces.
    gpu::GpuBuffer structureTable;
    // The placement in each slot; empty when each slot is its placement.
    gpu::GpuBuffer order;
    gpu::GpuBuffer instanceGeometry;
    std::vector<GeometryEntryPtr> placed;
    // The moving and deforming instances, as Kernels/Intersect.slang reads them; segments is zero without any.
    std::uint32_t segments = 0;
    gpu::GpuBuffer moverBoxes;
    std::vector<gpu::GpuBlas> moverBlases;
    std::vector<gpu::GpuTlas> moverScenes;
    // Four words per segment: the address of its mover structure, low first.
    gpu::GpuBuffer moverTable;
    gpu::GpuBuffer moverRecords;
    gpu::GpuBuffer moverFirst;
    // One-instance structures over the rigid geometries that movers place.
    std::vector<gpu::GpuTlas> rigidScenes;
    // The scene as raster passes draw it, built on the first request: an update that keeps this state keeps
    // every drawn placement and geometry, so the tables stay right for as long as the state does.
    mutable std::mutex rasterMutex;
    mutable std::shared_ptr<const RasterTables> raster;

    bool traces() const { return !structures.empty() || segments != 0; }
};

// Interleaves the low 21 bits of each coordinate.
std::uint64_t
mortonCode(std::uint32_t x,
           std::uint32_t y,
           std::uint32_t z)
{
    auto spread = [](std::uint64_t v) {
        v &= 0x1fffffull;
        v = (v | v << 32) & 0x1f00000000ffffull;
        v = (v | v << 16) & 0x1f0000ff0000ffull;
        v = (v | v << 8) & 0x100f00f00f00f00full;
        v = (v | v << 4) & 0x10c30c30c30c30c3ull;
        v = (v | v << 2) & 0x1249249249249249ull;

        return v;
    };

    return spread(x) | spread(y) << 1 | spread(z) << 2;
}

// Orders the drawn placements along a curve through their centres, so that each structure a split scene gets
// covers a compact region and a ray's search of the ones it misses ends at their roots.
void
sortSpatially(std::vector<std::uint32_t>& drawn,
              const std::vector<Placement>& placements,
              const UtilityScene::Snapshot& snapshot)
{
    std::map<UtilityScene::GeometryId, GfVec3d> centres;

    for (const auto& item : snapshot.geometries) {
        GfRange3d bounds;

        for (const GfVec3f& point : item.second.points) {
            bounds.UnionWith(GfVec3d(point));
        }
        centres[item.first] = bounds.IsEmpty() ? GfVec3d(0.) : bounds.GetMidpoint();
    }

    std::vector<GfVec3d> positions(drawn.size());
    GfRange3d extent;

    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const Placement& placement = placements[drawn[i]];

        positions[i] = placement.transform.Transform(centres[placement.geometry]);
        extent.UnionWith(positions[i]);
    }

    const GfVec3d size = extent.GetSize();
    std::vector<std::pair<std::uint64_t, std::uint32_t>> keyed(drawn.size());

    for (std::size_t i = 0; i < drawn.size(); ++i) {
        std::uint32_t cell[3];

        for (int axis = 0; axis < 3; ++axis) {
            const double unit = size[axis] > 0. ? (positions[i][axis] - extent.GetMin()[axis]) / size[axis] : 0.;

            cell[axis] = static_cast<std::uint32_t>(std::min(std::max(unit, 0.), 1.) * 2097151.);
        }
        keyed[i] = { mortonCode(cell[0], cell[1], cell[2]), drawn[i] };
    }
    std::sort(keyed.begin(), keyed.end());
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        drawn[i] = keyed[i].second;
    }
}

using SceneStatePtr = std::shared_ptr<const SceneState>;

struct Failure {
    GpuTracer::Status status;
    std::string message;
};

// How often an update waiting for the geometry's submission looks at its stop flag.
const std::chrono::milliseconds kSubmissionPoll(20);

/**
 * @brief A submission of the geometry's builds, refits or compaction, recorded, submitted and waited for on a
 * thread of its own, since a record context must be destroyed on the thread that made it. What it built is
 * kept here until an update adopts it.
 **/
struct GeometrySubmission {
    enum class Kind {
        Builds,
        Compaction,
    };

    Kind kind = Kind::Builds;
    // Records part `part` of the work, each submitted and waited for before the next is recorded; on failure
    // returns the status and sets what to the failure's description.
    std::function<gpu::GpuStatus(gpu::GpuRecordContext& ctx, std::size_t part, std::string& what)> record;
    std::size_t parts = 1;
    std::vector<std::pair<UtilityScene::GeometryId, GeometryEntryPtr>> built;
    // The entries refit, with the points version they were refit to.
    std::vector<std::pair<GeometryEntryPtr, std::uint64_t>> refitted;
    // What the submission failed with, if it did not run.
    std::string submitWhat;

    std::mutex mutex;
    std::condition_variable cond;
    bool finished = false;
    bool recorded = false;
    gpu::GpuStatus status;
    std::string what;
    std::thread thread;
};

using GeometrySubmissionPtr = std::shared_ptr<GeometrySubmission>;

// The triangles one geometry submission builds, refits or compacts at most, besides a geometry larger on its
// own. A device that takes longer than its driver's job timeout over one submission is reset, which a whole
// large scene in one submission reached on a slower ray-tracing device.
const std::size_t kTrianglesPerSubmission = std::size_t(1) << 22;

// Consecutive runs of entries, each a submission's worth of triangles.
std::vector<std::vector<GeometryEntryPtr>>
submissionParts(const std::vector<GeometryEntryPtr>& entries)
{
    std::vector<std::vector<GeometryEntryPtr>> parts;
    std::size_t triangles = 0;

    for (const GeometryEntryPtr& entry : entries) {
        if (parts.empty() || (triangles + entry->triangleCount > kTrianglesPerSubmission && !parts.back().empty())) {
            parts.emplace_back();
            triangles = 0;
        }
        parts.back().push_back(entry);
        triangles += entry->triangleCount;
    }

    return parts;
}

void
runGeometrySubmission(gpu::GpuKernelRegistry* registry,
                      gpu::GpuBufferPool* pool,
                      GeometrySubmissionPtr job)
{
    gpu::GpuStatus status;
    std::string what;
    bool recorded = false;

    for (std::size_t part = 0; part < job->parts && status; ++part) {
        std::unique_ptr<gpu::GpuRecordContext> ctx;

        status = gpu::GpuRecordContext::create(*registry, ctx, pool);
        if (!status) {
            what = "GpuTracer could not create a record context";
        } else {
            status = job->record(*ctx, part, what);
            if (status) {
                recorded = true;
                status = ctx->submitAndWait();
                if (!status) {
                    what = job->submitWhat;
                }
            }
        }
    }
    job->record = nullptr;
    {
        const std::lock_guard<std::mutex> l(job->mutex);

        job->recorded = recorded;
        job->status = status;
        job->what = what;
        job->finished = true;
    }
    job->cond.notify_all();
}
} // anon namespace

struct GpuTracer::Implementation {
    Implementation(gpu::GpuKernelRegistry& registry,
                   gpu::GpuBufferPool& pool)
        : registry(registry)
        , pool(pool)
        , device(registry.device())
    {
    }

    ~Implementation()
    {
        if (inFlight && inFlight->thread.joinable()) {
            inFlight->thread.join();
        }
    }

    gpu::GpuStatus createContext(std::unique_ptr<gpu::GpuRecordContext>& ctx) const
    {
        return gpu::GpuRecordContext::create(registry, ctx, &pool);
    }

    // Starts job on its thread as the submission in flight. False, with the failure recorded, when it cannot.
    bool startSubmission(GeometrySubmissionPtr job)
    {
        try {
            job->thread = std::thread(runGeometrySubmission, &registry, &pool, job);
        } catch (const std::system_error& e) {
            return abandonUpdate(Failure { Status::Failed, std::string("GpuTracer could not start building its geometry: ") + e.what() });
        }
        inFlight = std::move(job);

        return true;
    }

    // Waits for the submission in flight, if any, and keeps what it did. False on a stop, which leaves it
    // running, or when it failed, with the failure recorded.
    bool finishSubmission(const std::atomic<bool>* stop,
                          const Failure& stopped)
    {
        if (!inFlight) {
            return true;
        }
        {
            std::unique_lock<std::mutex> l(inFlight->mutex);

            while (!inFlight->finished) {
                if (wasStopped(stop)) {
                    return abandonUpdate(stopped);
                }
                inFlight->cond.wait_for(l, kSubmissionPoll);
            }
        }
        inFlight->thread.join();

        const GeometrySubmissionPtr job = std::move(inFlight);

        if (!job->status) {
            return job->recorded ? abandonSubmission(job->what, job->status) : abandonUpdate(failure(job->what, job->status));
        }
        if (job->kind == GeometrySubmission::Kind::Compaction) {
            // A structure the pool had no room to compact into stays as built.
            uncompacted.clear();

            return true;
        }
        for (std::pair<UtilityScene::GeometryId, GeometryEntryPtr>& build : job->built) {
            geometries[build.first] = std::move(build.second);
            uncompacted.insert(build.first);
            ++counters.geometryBuilds;
        }
        for (const std::pair<GeometryEntryPtr, std::uint64_t>& refit : job->refitted) {
            refit.first->pointsVersion = refit.second;
            refit.first->refitPending = false;
            ++counters.geometryRefits;
        }

        return true;
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
        structured = false;
        record(failure);

        return false;
    }

    // A failed submission leaves unknown which builds and refits ran, so nothing built is trusted. What ran out
    // of memory or past a device limit keeps saying so, since the scene would do the same on another try.
    bool abandonSubmission(const std::string& what,
                           const gpu::GpuStatus& status)
    {
        Failure failed = failure(what, status);

        if (failed.status != Status::DeviceLost && failed.status != Status::OutOfMemory && failed.status != Status::Unsupported) {
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

    // The raster tables of the current scene, from its placements. Called with the state's rasterMutex held.
    gpu::GpuStatus buildRasterTables(std::shared_ptr<const RasterTables>& out) const;

    // Sweeps the movers into the state's per-segment structures. False, with the failure recorded, when it
    // cannot or is stopped.
    bool buildMovers(const UtilityScene::Snapshot& snapshot,
                     const std::vector<Placement>& placements,
                     const std::vector<std::uint32_t>& movers,
                     const std::map<UtilityScene::GeometryId, std::uint32_t>& denseIndices,
                     SceneState& state,
                     const std::atomic<bool>* stop,
                     const Failure& stopped);

    // Halves the rays of the submissions after one of `submitted` rays that ran slow.
    void shrinkSubmissions(std::size_t submitted) const
    {
        const std::size_t halved = std::max(kMinRaysPerSubmit, submitted / 2);
        std::size_t current = raysPerSubmit.load(std::memory_order_relaxed);

        while (halved < current && !raysPerSubmit.compare_exchange_weak(current, halved, std::memory_order_relaxed)) {
        }
    }

    // The device-local heap budget VK_EXT_memory_budget reports, lowered by memoryBudgetLimit; 0 when neither
    // gives one.
    std::uint64_t memoryBudget() const
    {
        std::uint64_t budget = 0;
        std::vector<VmaBudget> budgets;

        if (device.info().memoryBudget && device.queryBudget(budgets)) {
            const VkPhysicalDeviceMemoryProperties* props = nullptr;

            vmaGetMemoryProperties(device.allocator(), &props);
            for (std::uint32_t i = 0; props && i < props->memoryHeapCount && i < budgets.size(); ++i) {
                if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    budget = std::max<std::uint64_t>(budget, budgets[i].budget);
                }
            }
        }
        if (memoryBudgetLimit != 0) {
            budget = budget ? std::min(budget, memoryBudgetLimit) : memoryBudgetLimit;
        }

        return budget;
    }

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
    // Registered with registry when the device has GpuDeviceInfo::rasterVisibility.
    gpu::GpuRasterId visibilityRaster {};
    bool rasterizes = false;
    bool usable = false;
    std::size_t maxInstancesPerStructure = 0;
    mutable std::atomic<std::size_t> raysPerSubmit { kMaxRaysPerSubmit };
    std::function<std::chrono::steady_clock::time_point()> now = &std::chrono::steady_clock::now;
    // Lowers the device's memory budget, for tests; 0 leaves it.
    std::uint64_t memoryBudgetLimit = 0;
    // The structure table of a trace without structures: only the zero entry the kernel reads past the last.
    gpu::GpuBuffer noStructures;

    std::map<UtilityScene::GeometryId, GeometryEntryPtr> geometries;
    // Built but not yet compacted, because the update that built them was stopped first.
    std::set<UtilityScene::GeometryId> uncompacted;
    std::vector<Placement> placements;
    SceneStatePtr scene;
    std::uint64_t version = 0;
    bool synced = false;
    // Whether the scene's structures match its geometry and placements; a raster update builds none.
    bool structured = false;
    Stats counters;
    // The geometry's submission a stopped update left running; only an update touches the geometry meanwhile.
    GeometrySubmissionPtr inFlight;

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
    // Every ray would silently miss, or hit where the geometry was; a caller that traces must update for it.
    if (state && !structured) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuTracer: the scene was updated for raster passes only, so it has no structures to trace" };
    }

    const bool traced = state && state->traces();
    const VkDeviceAddress structures = (traced && state->structureTable ? state->structureTable : noStructures).deviceAddress(device);
    const VkDeviceAddress order = traced && state->order ? state->order.deviceAddress(device) : 0;
    const bool moves = traced && state->segments != 0;
    const VkDeviceAddress movers = moves ? state->moverTable.deviceAddress(device) : 0;
    const VkDeviceAddress moverRecords = moves ? state->moverRecords.deviceAddress(device) : 0;
    const VkDeviceAddress moverFirst = moves ? state->moverFirst.deviceAddress(device) : 0;

    if (traced) {
        ctx.hold(std::const_pointer_cast<SceneState>(state));
    }

    // Without a scene the kernel reads nothing past the rays, so they stand in for the instance table.
    const gpu::GpuBuffer& instanceGeometry = traced ? state->instanceGeometry : rays;
    const bool filtered = filter.maskStride != 0 && filter.rayLights && filter.casterMasks;
    // Unfiltered, the kernel never reads the light tables, so the rays stand in for them.
    const std::array<gpu::GpuBinding, 5> bindings = {
        gpu::GpuBinding::storage(0, rays),
        gpu::GpuBinding::storage(1, instanceGeometry),
        gpu::GpuBinding::storage(2, values),
        gpu::GpuBinding::storage(3, ids),
        gpu::GpuBinding::storage(4, filtered ? *filter.rayLights : rays),
    };
    const VkDeviceAddress casterMasks = filtered ? filter.casterMasks->deviceAddress(device) : 0;
    const std::shared_ptr<gpu::GpuKernel>& kernel = entry == Entry::Intersect ? intersectKernel : occludedKernel;

    for (std::uint64_t first = 0; first < count; first += kMaxDispatchRays) {
        const std::uint64_t end = std::min<std::uint64_t>(count, first + kMaxDispatchRays);
        const KernelIntersectParams params {
            static_cast<std::uint32_t>(structures & 0xffffffffull),
            static_cast<std::uint32_t>(structures >> 32),
            static_cast<std::uint32_t>(order & 0xffffffffull),
            static_cast<std::uint32_t>(order >> 32),
            static_cast<std::uint32_t>(casterMasks & 0xffffffffull),
            static_cast<std::uint32_t>(casterMasks >> 32),
            static_cast<std::uint32_t>(movers & 0xffffffffull),
            static_cast<std::uint32_t>(movers >> 32),
            static_cast<std::uint32_t>(moverRecords & 0xffffffffull),
            static_cast<std::uint32_t>(moverRecords >> 32),
            static_cast<std::uint32_t>(moverFirst & 0xffffffffull),
            static_cast<std::uint32_t>(moverFirst >> 32),
            traced ? static_cast<std::uint32_t>(state->structures.size()) : 0u,
            count,
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(end),
            filtered ? filter.maskStride : 0u,
            filtered ? filter.maskOffset : 0u,
            moves ? state->segments : 0u,
            0u,
        };
        const std::array<std::uint32_t, 3> threads = { static_cast<std::uint32_t>(end - first), 1, 1 };

        if (gpu::GpuStatus s = ctx.dispatchBindings(kernel, bindings, std::as_bytes(std::span(&params, 1)), threads); !s) {
            return s;
        }
    }

    return {};
}

namespace {
// Samples per segment for bounding a mover's sweep; each sample's box is padded by the largest corner
// travel between neighbouring samples, which covers the arc a rotation traces between them.
const int kSweepSamples = 16;

void
padRange(GfRange3d& range,
         double pad)
{
    const GfVec3d size = range.GetSize();
    const double largest = std::max({ size[0], size[1], size[2], range.GetMin().GetLength(), range.GetMax().GetLength() });
    // Float boxes and the kernels' float interpolation round on either side of the double bounds.
    const GfVec3d grow(pad + largest * 1e-5 + 1e-7);

    range = GfRange3d(range.GetMin() - grow, range.GetMax() + grow);
}

GfRange3d
sweepOf(const std::vector<UtilityScene::Decomposition>& steps,
        double from,
        double to,
        const GfRange3d& box)
{
    GfRange3d swept;
    GfVec3d previous[8];
    double pad = 0.;

    for (int sample = 0; sample <= kSweepSamples; ++sample) {
        const double time = from + (to - from) * sample / kSweepSamples;
        int span = 0;
        double fraction = 0.;

        UtilityScene::stepAt(time, static_cast<int>(steps.size()), &span, &fraction);

        const GfMatrix4d transform = UtilityScene::compose(UtilityScene::blend(steps[span], steps[span + 1], fraction));

        for (std::size_t corner = 0; corner < 8; ++corner) {
            const GfVec3d point = transform.Transform(box.GetCorner(corner));

            if (sample > 0) {
                pad = std::max(pad, (point - previous[corner]).GetLength());
            }
            previous[corner] = point;
            swept.UnionWith(point);
        }
    }
    padRange(swept, pad);

    return swept;
}

void
writeBox(const GfRange3d& range,
         float* out)
{
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = static_cast<float>(range.GetMin()[axis]);
        out[3 + axis] = static_cast<float>(range.GetMax()[axis]);
    }
}

void
appendWords(std::vector<float>& rows,
            std::uint32_t a,
            std::uint32_t b,
            std::uint32_t c,
            std::uint32_t d)
{
    for (std::uint32_t word : { a, b, c, d }) {
        rows.push_back(std::bit_cast<float>(word));
    }
}

void
appendAddress(std::vector<float>& rows,
              VkDeviceAddress a,
              VkDeviceAddress b = 0)
{
    appendWords(rows, static_cast<std::uint32_t>(a & 0xffffffffull), static_cast<std::uint32_t>(a >> 32),
                static_cast<std::uint32_t>(b & 0xffffffffull), static_cast<std::uint32_t>(b >> 32));
}

void
appendStep(std::vector<float>& rows,
           const UtilityScene::Decomposition& step)
{
    const double* u = step.stretch;
    const double values[16] = {
        u[0],
        u[1],
        u[2],
        0.,
        u[3],
        u[4],
        u[5],
        0.,
        step.translation[0],
        step.translation[1],
        step.translation[2],
        0.,
        step.rotation[1],
        step.rotation[2],
        step.rotation[3],
        step.rotation[0],
    };

    for (double value : values) {
        rows.push_back(static_cast<float>(value));
    }
}

gpu::GpuStatus
stageWith(gpu::GpuDevice& device,
          const void* data,
          std::size_t bytes,
          VkBufferUsageFlags usage,
          gpu::GpuBuffer& out)
{
    if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(device, std::max<std::size_t>(bytes, 16), usage, out); !s) {
        return s;
    }
    std::memcpy(out.mapped(), data, bytes);

    return {};
}

const std::uint32_t kMoverDeforms = 1;
} // anon namespace

bool
GpuTracer::Implementation::buildMovers(const UtilityScene::Snapshot& snapshot,
                                       const std::vector<Placement>& placements,
                                       const std::vector<std::uint32_t>& movers,
                                       const std::map<UtilityScene::GeometryId, std::uint32_t>& denseIndices,
                                       SceneState& state,
                                       const std::atomic<bool>* stop,
                                       const Failure& stopped)
{
    const std::size_t segments = static_cast<std::size_t>(std::max(snapshot.timeSteps, 2) - 1);
    const VkBufferUsageFlags boxUsage = kAddressedUsage | gpu::kGpuAccelInputUsage;
    std::unique_ptr<gpu::GpuRecordContext> ctx;

    if (gpu::GpuStatus s = createContext(ctx); !s) {
        return abandonUpdate(failure("GpuTracer could not create a record context", s));
    }

    gpu::GpuBlasOptions boxOptions;

    boxOptions.allowUpdate = false;
    boxOptions.allowCompaction = false;

    auto oneInstance = [&](const gpu::GpuBlas& blas, gpu::GpuTlas& out) {
        gpu::GpuAccelInstance instance;

        instance.blas = &blas;

        return gpu::GpuTlas::build(*ctx, pool, std::span(&instance, 1), out);
    };

    // Each geometry's object bounds over each segment, and the structures it is traced in.
    struct Shape {
        GeometryEntryPtr entry;
        std::vector<GfRange3d> bounds;
        std::size_t rigidScene = 0;
        gpu::GpuBuffer sweptBoxes;
        std::vector<gpu::GpuBlas> swept;
        std::vector<gpu::GpuTlas> sweptScenes;
    };
    std::map<UtilityScene::GeometryId, Shape> shapes;

    for (const std::uint32_t i : movers) {
        const UtilityScene::GeometryId id = placements[i].geometry;

        if (shapes.count(id)) {
            continue;
        }
        if (wasStopped(stop)) {
            return abandonUpdate(stopped);
        }

        Shape& shape = shapes[id];
        const UtilityScene::Geometry& source = snapshot.geometries.at(id);

        shape.entry = geometries.at(id);
        if (!shape.entry->deforms()) {
            GfRange3d bounds;

            for (const GfVec3f& point : source.points) {
                bounds.UnionWith(GfVec3d(point));
            }
            shape.bounds.assign(segments, bounds);
            shape.rigidScene = state.rigidScenes.size();
            state.rigidScenes.emplace_back();
            if (gpu::GpuStatus s = oneInstance(shape.entry->blas, state.rigidScenes.back()); !s) {
                return abandonUpdate(failure("GpuTracer could not build a moving geometry's structure", s));
            }
            continue;
        }

        const std::size_t triangles = shape.entry->triangleCount;
        const bool built = !shape.entry->swept.empty();
        std::vector<float> boxes(built ? 0 : segments * triangles * 6);

        shape.bounds.resize(segments);
        for (std::size_t segment = 0; segment < segments; ++segment) {
            const VtVec3fArray& from = source.pointSteps[segment];
            const VtVec3fArray& to = source.pointSteps[segment + 1];

            for (std::size_t t = 0; t < triangles && !built; ++t) {
                GfRange3d box;

                for (int corner = 0; corner < 3; ++corner) {
                    box.UnionWith(GfVec3d(from[source.triangles[t][corner]]));
                    box.UnionWith(GfVec3d(to[source.triangles[t][corner]]));
                }
                padRange(box, 0.);
                writeBox(box, boxes.data() + (segment * triangles + t) * 6);
            }
            for (std::size_t p = 0; p < from.size(); ++p) {
                shape.bounds[segment].UnionWith(GfVec3d(from[p]));
                shape.bounds[segment].UnionWith(GfVec3d(to[p]));
            }
            if (wasStopped(stop)) {
                return abandonUpdate(stopped);
            }
        }
        if (built) {
            continue;
        }
        if (gpu::GpuStatus s = stageWith(device, boxes.data(), boxes.size() * sizeof(float), boxUsage, shape.sweptBoxes); !s) {
            return abandonUpdate(failure("GpuTracer could not hold the swept boxes of " + source.path.GetString(), s));
        }
        shape.swept.resize(segments);
        shape.sweptScenes.resize(segments);
        for (std::size_t segment = 0; segment < segments; ++segment) {
            gpu::GpuAabbs aabbs;

            aabbs.boxes = &shape.sweptBoxes;
            aabbs.offset = segment * triangles * 6 * sizeof(float);
            aabbs.count = static_cast<std::uint32_t>(triangles);
            if (gpu::GpuStatus s = gpu::GpuBlas::build(*ctx, pool, aabbs, boxOptions, shape.swept[segment]); !s) {
                return abandonUpdate(failure("GpuTracer could not build the swept structure of " + source.path.GetString(), s));
            }
            if (gpu::GpuStatus s = oneInstance(shape.swept[segment], shape.sweptScenes[segment]); !s) {
                return abandonUpdate(failure("GpuTracer could not build the swept structure of " + source.path.GetString(), s));
            }
        }
    }

    std::vector<float> boxes(segments * movers.size() * 6);
    std::vector<float> rows;
    std::vector<std::uint32_t> first;

    first.reserve(movers.size());
    for (std::size_t m = 0; m < movers.size(); ++m) {
        if (wasStopped(stop)) {
            return abandonUpdate(stopped);
        }

        const std::uint32_t i = movers[m];
        const Placement& placement = placements[i];
        const Shape& shape = shapes.at(placement.geometry);
        const GeometryEntry& entry = *shape.entry;
        const std::vector<UtilityScene::Decomposition> held(2, UtilityScene::decompose(placement.transform));
        const std::vector<UtilityScene::Decomposition>& steps = placement.steps.empty() ? held : placement.steps;
        const bool deforms = entry.deforms();

        for (std::size_t segment = 0; segment < segments; ++segment) {
            const GfRange3d swept = sweepOf(steps, double(segment) / segments, double(segment + 1) / segments, shape.bounds[segment]);

            writeBox(swept, boxes.data() + (segment * movers.size() + m) * 6);
        }

        const std::size_t row = rows.size() / 4;
        const std::size_t header = row + 3 + (deforms ? segments : 0);

        first.push_back(static_cast<std::uint32_t>(row));
        appendWords(rows, i, denseIndices.at(placement.geometry), deforms ? kMoverDeforms : 0u, static_cast<std::uint32_t>(header));
        if (deforms) {
            appendAddress(rows, entry.vertices.deviceAddress(device), entry.indices.deviceAddress(device));
        } else {
            appendAddress(rows, state.rigidScenes[shape.rigidScene].deviceAddress());
        }
        appendWords(rows, static_cast<std::uint32_t>(entry.pointCount), 0u, 0u, 0u);
        if (deforms) {
            const std::vector<gpu::GpuTlas>& scenes = entry.swept.empty() ? shape.sweptScenes : entry.sweptScenes;

            for (const gpu::GpuTlas& scene : scenes) {
                appendAddress(rows, scene.deviceAddress());
            }
        }
        appendWords(rows, static_cast<std::uint32_t>(steps.size()), 0u, 0u, 0u);
        for (const UtilityScene::Decomposition& step : steps) {
            appendStep(rows, step);
        }
    }

    if (gpu::GpuStatus s = stageWith(device, boxes.data(), boxes.size() * sizeof(float), boxUsage, state.moverBoxes); !s) {
        return abandonUpdate(failure("GpuTracer could not hold the boxes of its moving instances", s));
    }
    state.moverBlases.resize(segments);
    state.moverScenes.resize(segments);
    for (std::size_t segment = 0; segment < segments; ++segment) {
        gpu::GpuAabbs aabbs;

        aabbs.boxes = &state.moverBoxes;
        aabbs.offset = segment * movers.size() * 6 * sizeof(float);
        aabbs.count = static_cast<std::uint32_t>(movers.size());
        if (gpu::GpuStatus s = gpu::GpuBlas::build(*ctx, pool, aabbs, boxOptions, state.moverBlases[segment]); !s) {
            return abandonUpdate(failure("GpuTracer could not build the structure of its moving instances", s));
        }
        if (gpu::GpuStatus s = oneInstance(state.moverBlases[segment], state.moverScenes[segment]); !s) {
            return abandonUpdate(failure("GpuTracer could not build the structure of its moving instances", s));
        }
    }

    std::vector<float> table;

    for (const gpu::GpuTlas& scene : state.moverScenes) {
        appendAddress(table, scene.deviceAddress());
    }
    if (gpu::GpuStatus s = stageWith(device, table.data(), table.size() * sizeof(float), kAddressedUsage, state.moverTable); !s) {
        return abandonUpdate(failure("GpuTracer could not hold its mover table", s));
    }
    if (gpu::GpuStatus s = stageWith(device, rows.data(), rows.size() * sizeof(float), kAddressedUsage, state.moverRecords); !s) {
        return abandonUpdate(failure("GpuTracer could not hold the records of its moving instances", s));
    }
    if (gpu::GpuStatus s = stageWith(device, first.data(), first.size() * sizeof(std::uint32_t), kAddressedUsage, state.moverFirst); !s) {
        return abandonUpdate(failure("GpuTracer could not hold the records of its moving instances", s));
    }
    if (wasStopped(stop)) {
        return abandonUpdate(stopped);
    }
    if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
        return abandonSubmission("GpuTracer could not build the structures of its moving instances", s);
    }
    // Kept only once built, so that a stopped or failed update never leaves an unbuilt structure behind.
    for (auto& item : shapes) {
        Shape& shape = item.second;

        if (!shape.swept.empty()) {
            shape.entry->sweptBoxes = std::move(shape.sweptBoxes);
            shape.entry->swept = std::move(shape.swept);
            shape.entry->sweptScenes = std::move(shape.sweptScenes);
        }
    }
    state.segments = static_cast<std::uint32_t>(segments);

    return true;
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

    if (state && !structured) {
        record(Failure { Status::Failed, "GpuTracer: the scene was updated for raster passes only, so it has no structures to trace" });
        missFrom(0);

        return;
    }
    if (!state || !state->traces()) {
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
    const std::size_t chunk = std::min({ rays.count, raysPerSubmit.load(std::memory_order_relaxed), bindable });

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

    // A batch without times traces at the shutter's centre, as the CPU tracers do.
    std::vector<float> centre(rays.time ? 0 : chunk, 0.5f);
    const float* const sources[kRayArrays] = { rays.originX, rays.originY, rays.originZ,
                                               rays.directionX, rays.directionY, rays.directionZ,
                                               rays.tMin, rays.tMax, rays.time };
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
        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(device, maskBytes, gpu::kGpuBufferDefaultUsage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, maskBuffer); !s) {
            record(failure("GpuTracer could not stage " + std::to_string(maskWords) + " words of shadow-link masks", s));
            missFrom(0);

            return;
        }
        std::memcpy(maskBuffer.mapped(), filter->casterMasks, maskWords * sizeof(std::uint32_t));
        deviceFilter = DeviceFilter { &lightBuffer, &maskBuffer, filter->stride, 0u };
    }

    std::size_t step = chunk;

    for (std::size_t start = 0; start < rays.count; start += step) {
        step = std::min(step, raysPerSubmit.load(std::memory_order_relaxed));

        const std::size_t n = std::min(step, rays.count - start);
        float* staged = static_cast<float*>(rayBuffer.mapped());

        for (std::size_t array = 0; array < kRayArrays; ++array) {
            const float* source = sources[array] ? sources[array] + start : centre.data();

            std::memcpy(staged + array * n, source, n * sizeof(float));
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
            const std::chrono::steady_clock::time_point began = now();

            s = ctx->submitAndWait();
            if (s && now() - began > kSlowSubmission) {
                shrinkSubmissions(n);
            }
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
    imp.maxInstancesPerStructure = gpu::GpuTlas::maxInstancesPerStructure(imp.device);
    if (imp.maxInstancesPerStructure == 0) {
        imp.outcome = Outcome { Status::Unsupported, "GpuTracer: the device builds no top-level structure with an instance in it" };

        return;
    }

    const std::uint32_t noStructure[4] = {};

    if (gpu::GpuStatus s = stageWith(imp.device, noStructure, sizeof(noStructure), kAddressedUsage, imp.noStructures); !s) {
        imp.outcome = Outcome { Status::Unsupported, "GpuTracer could not hold its empty structure table: " + s.message };

        return;
    }
    if (imp.device.info().rasterVisibility) {
        const gpu::GpuRasterDesc visibilityDesc {
            utility_raster_visibility_vertex_spirv, utility_raster_visibility_vertex_spirv_words,
            utility_raster_visibility_fragment_spirv, utility_raster_visibility_fragment_spirv_words,
            utility_raster_visibility_entry, utility_raster_visibility_storage_buffers, utility_raster_visibility_push_bytes
        };

        imp.visibilityRaster = imp.registry.registerRaster(visibilityDesc);
        imp.rasterizes = true;
    }
    imp.usable = true;
}

void
GpuTracer::limitInstancesPerStructure(std::size_t count)
{
    _imp->maxInstancesPerStructure = std::max<std::size_t>(1, std::min(_imp->maxInstancesPerStructure, count));
}

std::size_t
GpuTracer::raysPerSubmission() const
{
    return _imp->raysPerSubmit.load(std::memory_order_relaxed);
}

void
GpuTracer::limitRaysPerSubmission(std::size_t count)
{
    _imp->raysPerSubmit.store(std::max<std::size_t>(1, std::min(_imp->raysPerSubmit.load(std::memory_order_relaxed), count)), std::memory_order_relaxed);
}

void
GpuTracer::useClock(std::function<std::chrono::steady_clock::time_point()> now)
{
    _imp->now = std::move(now);
}

void
GpuTracer::limitMemoryBudget(std::uint64_t bytes)
{
    _imp->memoryBudgetLimit = bytes;
}

std::size_t
GpuTracer::instanceStructureCount() const
{
    const SceneStatePtr state = _imp->scene;

    return state ? state->structures.size() : 0;
}

std::size_t
GpuTracer::moverStructureCount() const
{
    const SceneStatePtr state = _imp->scene;

    return state ? state->moverScenes.size() : 0;
}

GpuTracer::~GpuTracer()
{
}

std::vector<std::shared_ptr<gpu::GpuKernel>>
GpuTracer::kernels() const
{
    if (!_imp->usable) {
        return {};
    }

    return { _imp->intersectKernel, _imp->occludedKernel };
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
    return updateScene(snapshot, stop, true);
}

bool
GpuTracer::updateForRaster(const UtilityScene::Snapshot& snapshot,
                           const std::atomic<bool>* stop)
{
    return updateScene(snapshot, stop, false);
}

bool
GpuTracer::updateScene(const UtilityScene::Snapshot& snapshot,
                       const std::atomic<bool>* stop,
                       bool structures)
{
    Implementation& imp = *_imp;

    if (!imp.usable) {
        return false;
    }
    if (imp.synced && imp.version == snapshot.version && (imp.structured || !structures)) {
        return true;
    }
    imp.synced = false;
    {
        std::lock_guard<std::mutex> lock(imp.outcomeMutex);

        imp.outcome = Outcome();
    }

    const Failure stopped { Status::Stopped, "GpuTracer: the update was stopped" };

    if (!imp.finishSubmission(stop, stopped)) {
        return false;
    }

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
        GeometryEntryPtr entry;
        const UtilityScene::Geometry* source;
    };
    std::vector<PendingBuild> builds;
    std::vector<PendingRefit> refits;
    // Points a raster update moved under a structure, which the scene's structures no longer match.
    bool staled = false;

    for (const auto& item : snapshot.geometries) {
        if (wasStopped(stop)) {
            return imp.abandonUpdate(stopped);
        }

        const UtilityScene::Geometry& source = item.second;
        const auto existing = imp.geometries.find(item.first);

        if (existing == imp.geometries.end() || existing->second->needsRebuild(source, snapshot)) {
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
            entry->pointSteps = deformSteps(source, snapshot);

            const VkDeviceSize vertexBytes = entry->pointSteps * entry->pointCount * sizeof(GfVec3f);
            const VkDeviceSize indexBytes = entry->triangleCount * sizeof(GfVec3i);

            if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, vertexBytes, gpu::kGpuAccelInputUsage, entry->vertices); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not hold the " + std::to_string(vertexBytes) + " bytes of points of "
                                                         + source.path.GetString(),
                                                     s));
            }
            // Raster passes draw with the very indices the triangle structure is built from.
            if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, indexBytes, gpu::kGpuAccelInputUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, entry->indices); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not hold the " + std::to_string(indexBytes) + " bytes of triangles of "
                                                         + source.path.GetString(),
                                                     s));
            }
            if (entry->deforms()) {
                char* vertices = static_cast<char*>(entry->vertices.mapped());
                const std::size_t stepBytes = entry->pointCount * sizeof(GfVec3f);

                for (std::size_t step = 0; step < entry->pointSteps; ++step) {
                    std::memcpy(vertices + step * stepBytes, source.pointSteps[step].cdata(), stepBytes);
                }
            } else {
                std::memcpy(entry->vertices.mapped(), source.points.cdata(), vertexBytes);
            }
            // GfVec3i holds three ints, which the range check made valid as uint32 indices.
            std::memcpy(entry->indices.mapped(), source.triangles.cdata(), indexBytes);
            if (entry->deforms() || !structures) {
                // A deforming one is swept into boxes once an update places it.
                imp.geometries[item.first] = std::move(entry);
            } else {
                builds.push_back(PendingBuild { item.first, std::move(entry) });
            }
        } else {
            const GeometryEntryPtr& entry = existing->second;
            const bool moved = source.pointsVersion != entry->pointsVersion;

            if (structures && entry->triangleCount > 0 && !entry->deforms() && !entry->blas) {
                if (moved) {
                    std::memcpy(entry->vertices.mapped(), source.points.cdata(), entry->pointCount * sizeof(GfVec3f));
                    entry->pointsVersion = source.pointsVersion;
                }
                builds.push_back(PendingBuild { item.first, entry });
                geometryReplaced = true;
            } else if (structures && entry->blas && (moved || entry->refitPending)) {
                refits.push_back(PendingRefit { entry, &source });
            } else if (moved) {
                // The raster passes draw from these vertices, so they move now even when the structure waits.
                if (entry->triangleCount > 0) {
                    if (wasStopped(stop)) {
                        return imp.abandonUpdate(stopped);
                    }
                    std::memcpy(entry->vertices.mapped(), source.points.cdata(), entry->pointCount * sizeof(GfVec3f));
                }
                if (entry->blas) {
                    entry->refitPending = true;
                    staled = true;
                }
                entry->pointsVersion = source.pointsVersion;
            }
        }
    }

    // The builds of one submission share a scratch buffer that grows only when a build outsizes it, and the
    // scratch a build needs grows with its triangles, so the largest goes first.
    std::stable_sort(builds.begin(), builds.end(), [](const PendingBuild& a, const PendingBuild& b) {
        return a.entry->triangleCount > b.entry->triangleCount;
    });

    // The structures are compared with the whole budget, not what is left of it, as the pool can evict what
    // else it holds; a scene whose structures alone exceed it fails here rather than in the driver.
    if (const std::uint64_t budget = structures ? imp.memoryBudget() : 0; budget != 0) {
        std::set<UtilityScene::GeometryId> rebuilt;
        std::uint64_t structureBytes = 0;
        std::uint64_t scratchBytes = 0;
        gpu::GpuBlasOptions options;

        options.allowUpdate = true;
        options.allowCompaction = true;
        for (const PendingBuild& build : builds) {
            gpu::GpuAccelSizes sizes;

            if (gpu::GpuStatus s = gpu::GpuBlas::querySizes(imp.device, build.entry->triangles(), options, sizes); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not size the structure of " + snapshot.geometries.at(build.id).path.GetString(), s));
            }
            rebuilt.insert(build.id);
            structureBytes += sizes.structureBytes;
            scratchBytes = std::max<std::uint64_t>(scratchBytes, sizes.buildScratchBytes);
        }
        for (const auto& kept : imp.geometries) {
            if (!rebuilt.count(kept.first) && snapshot.geometries.count(kept.first)) {
                structureBytes += kept.second->blas.size();
            }
        }

        const std::size_t instances = snapshot.instances.size();
        const std::size_t perStructure = std::max<std::size_t>(1, imp.maxInstancesPerStructure);
        // Full structures of perStructure instances, then one of the rest.
        const std::pair<std::size_t, std::size_t> runs[] = { { perStructure, instances / perStructure },
                                                             { instances % perStructure, std::size_t(instances % perStructure != 0) } };

        for (const auto& [count, structures] : runs) {
            gpu::GpuAccelSizes sizes;

            if (structures == 0) {
                continue;
            }
            if (gpu::GpuStatus s = gpu::GpuTlas::querySizes(imp.device, static_cast<std::uint32_t>(count), sizes); !s) {
                return imp.abandonUpdate(imp.failure("GpuTracer could not size its instance structures", s));
            }
            structureBytes += structures * sizes.structureBytes;
            scratchBytes = std::max<std::uint64_t>(scratchBytes, sizes.buildScratchBytes);
        }
        if (structureBytes + scratchBytes > budget) {
            return imp.abandonUpdate(Failure { Status::OutOfMemory, "GpuTracer: the scene's acceleration structures need " + std::to_string(structureBytes + scratchBytes) + " bytes, more than the GPU's " + std::to_string(budget) + "-byte memory budget" });
        }
    }

    if (!builds.empty() || !refits.empty()) {
        GeometrySubmissionPtr job = std::make_shared<GeometrySubmission>();
        std::vector<GeometryEntryPtr> toBuild;
        std::vector<GeometryEntryPtr> toRefit;

        for (PendingBuild& build : builds) {
            toBuild.push_back(build.entry);
            job->built.emplace_back(build.id, std::move(build.entry));
        }
        for (const PendingRefit& refit : refits) {
            if (wasStopped(stop)) {
                return imp.abandonUpdate(stopped);
            }
            std::memcpy(refit.entry->vertices.mapped(), refit.source->points.cdata(), refit.entry->pointCount * sizeof(GfVec3f));
            toRefit.push_back(refit.entry);
            job->refitted.emplace_back(refit.entry, refit.source->pointsVersion);
        }
        if (wasStopped(stop)) {
            return imp.abandonUpdate(stopped);
        }
        job->submitWhat = "GpuTracer could not build its geometry";
        const std::vector<std::vector<GeometryEntryPtr>> buildParts = submissionParts(toBuild);
        const std::vector<std::vector<GeometryEntryPtr>> refitParts = submissionParts(toRefit);

        job->parts = std::max<std::size_t>(1, buildParts.size() + refitParts.size());
        job->record = [pool = &imp.pool, buildParts, refitParts](gpu::GpuRecordContext& ctx, std::size_t part, std::string& what) -> gpu::GpuStatus {
            gpu::GpuBlasOptions options;
            static const std::vector<GeometryEntryPtr> none;
            const std::vector<GeometryEntryPtr>& toBuild = part < buildParts.size() ? buildParts[part] : none;
            const std::vector<GeometryEntryPtr>& toRefit = part >= buildParts.size() && part - buildParts.size() < refitParts.size() ? refitParts[part - buildParts.size()] : none;

            options.allowUpdate = true;
            options.allowCompaction = true;
            for (const GeometryEntryPtr& entry : toBuild) {
                if (gpu::GpuStatus s = gpu::GpuBlas::build(ctx, *pool, entry->triangles(), options, entry->blas); !s) {
                    what = "GpuTracer could not build a geometry's structure";

                    return s;
                }
            }
            for (const GeometryEntryPtr& entry : toRefit) {
                if (gpu::GpuStatus s = entry->blas.refit(ctx, entry->triangles()); !s) {
                    what = "GpuTracer could not refit a geometry's structure";

                    return s;
                }
            }

            return gpu::GpuStatus();
        };
        if (!imp.startSubmission(std::move(job)) || !imp.finishSubmission(stop, stopped)) {
            return false;
        }
    }

    for (auto it = imp.uncompacted.begin(); it != imp.uncompacted.end();) {
        const auto entry = imp.geometries.find(*it);

        it = (entry == imp.geometries.end() || !entry->second->blas) ? imp.uncompacted.erase(it) : std::next(it);
    }
    // Compaction gets its own submission: the copy replaces each structure as it is recorded, so it has to have
    // run before anything is built over them. A stopped update leaves the list for the next one.
    if (structures && !imp.uncompacted.empty() && !wasStopped(stop)) {
        GeometrySubmissionPtr job = std::make_shared<GeometrySubmission>();
        std::vector<GeometryEntryPtr> toCompact;

        for (const UtilityScene::GeometryId id : imp.uncompacted) {
            toCompact.push_back(imp.geometries[id]);
        }
        job->kind = GeometrySubmission::Kind::Compaction;
        job->submitWhat = "GpuTracer could not compact its geometry";
        const std::vector<std::vector<GeometryEntryPtr>> compactParts = submissionParts(toCompact);

        job->parts = compactParts.size();
        job->record = [pool = &imp.pool, device = &imp.device, compactParts](gpu::GpuRecordContext& ctx, std::size_t part, std::string& what) -> gpu::GpuStatus {
            for (const GeometryEntryPtr& entry : compactParts[part]) {
                if (gpu::GpuStatus s = entry->blas.compact(ctx, *pool); !s && classify(s, *device) != Status::OutOfMemory) {
                    what = "GpuTracer could not compact a geometry's structure";

                    return s;
                }
            }

            return gpu::GpuStatus();
        };
        if (!imp.startSubmission(std::move(job)) || !imp.finishSubmission(stop, stopped)) {
            return false;
        }
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
        placements.push_back(Placement { instance.geometry, instance.transform,
                                         instance.transformSteps.size() > 1 ? instance.transformSteps : std::vector<UtilityScene::Decomposition>() });
    }

    auto drawnBy = [&](std::size_t instance) -> const GeometryEntry* {
        const auto it = imp.geometries.find(placements[instance].geometry);

        return it != imp.geometries.end() && it->second->draws() ? it->second.get() : nullptr;
    };
    auto moves = [&](std::size_t instance) {
        return !placements[instance].steps.empty() || drawnBy(instance)->deforms();
    };

    const bool rebuildInstances = !imp.scene || geometryReplaced || !sameGeometries(placements, imp.placements) || (structures && !imp.structured);
    bool instancesChanged = rebuildInstances;

    for (std::size_t i = 0; i < placements.size() && !instancesChanged; ++i) {
        if (drawnBy(i) && (geometryRefitted || !samePlace(placements[i], imp.placements[i]))) {
            instancesChanged = true;
        }
    }

    if (instancesChanged) {
        if (wasStopped(stop)) {
            return imp.abandonUpdate(stopped);
        }
        if (placements.size() >= kNoHit) {
            return imp.abandonUpdate(Failure { Status::Failed, "GpuTracer: " + std::to_string(placements.size()) + " instances exceed the 32-bit ids a hit reports" });
        }

        std::shared_ptr<SceneState> state = std::make_shared<SceneState>();
        const VkDeviceSize tableBytes = std::max<std::size_t>(1, placements.size()) * sizeof(std::uint32_t);

        if (tableBytes > imp.device.info().maxStorageBufferRange) {
            return imp.abandonUpdate(Failure { Status::OverStorageRange, "GpuTracer: the " + std::to_string(tableBytes) + "-byte instance table exceeds the GPU's " + std::to_string(imp.device.info().maxStorageBufferRange) + "-byte storage buffer range" });
        }
        if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, tableBytes, gpu::kGpuBufferDefaultUsage, state->instanceGeometry); !s) {
            return imp.abandonUpdate(imp.failure(("GpuTracer could not hold its " + std::to_string(tableBytes) + "-byte instance table"), s));
        }

        std::uint32_t* table = static_cast<std::uint32_t*>(state->instanceGeometry.mapped());
        std::vector<std::uint32_t> drawn;
        std::vector<std::uint32_t> movers;

        table[0] = kNoHit;
        for (std::size_t i = 0; i < placements.size(); ++i) {
            const auto dense = denseIndices.find(placements[i].geometry);

            table[i] = dense == denseIndices.end() ? kNoHit : dense->second;
            if (drawnBy(i)) {
                (moves(i) ? movers : drawn).push_back(static_cast<std::uint32_t>(i));
                state->placed.push_back(imp.geometries[placements[i].geometry]);
            }
        }

        // Raster passes draw from the instance table and the geometry alone.
        if (structures) {
            const std::size_t perStructure = imp.maxInstancesPerStructure;
            // A scene one structure holds keeps each placement's index as its custom index, so it needs no order table.
            const bool split = placements.size() > kMaxInstances || drawn.size() > perStructure;

            if (split) {
                sortSpatially(drawn, placements, snapshot);
                if (wasStopped(stop)) {
                    return imp.abandonUpdate(stopped);
                }

                const VkDeviceSize orderBytes = drawn.size() * sizeof(std::uint32_t);

                if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, orderBytes, kAddressedUsage, state->order); !s) {
                    return imp.abandonUpdate(imp.failure(("GpuTracer could not hold its " + std::to_string(orderBytes) + "-byte instance order"), s));
                }
                std::memcpy(state->order.mapped(), drawn.data(), orderBytes);
            }

            std::vector<std::uint32_t> firstSlots;

            // One submission per structure, so that only one structure's instances are staged at a time.
            for (std::size_t first = 0; first < drawn.size(); first += perStructure) {
                const std::size_t end = std::min(drawn.size(), first + perStructure);
                std::vector<gpu::GpuAccelInstance> instances;

                instances.reserve(end - first);
                for (std::size_t slot = first; slot < end; ++slot) {
                    const std::uint32_t i = drawn[slot];
                    gpu::GpuAccelInstance instance;

                    instance.transform = objectToWorld(placements[i].transform);
                    instance.customIndex = split ? static_cast<std::uint32_t>(slot - first) : i;
                    instance.blas = &drawnBy(i)->blas;
                    instances.push_back(instance);
                }

                std::unique_ptr<gpu::GpuRecordContext> ctx;
                gpu::GpuTlas structure;

                if (gpu::GpuStatus s = imp.createContext(ctx); !s) {
                    return imp.abandonUpdate(imp.failure("GpuTracer could not create a record context", s));
                }
                if (gpu::GpuStatus s = gpu::GpuTlas::build(*ctx, imp.pool, instances, structure); !s) {
                    return imp.abandonUpdate(imp.failure("GpuTracer could not build its instance structure", s));
                }
                if (wasStopped(stop)) {
                    return imp.abandonUpdate(stopped);
                }
                if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
                    return imp.abandonSubmission("GpuTracer could not build its instance structure", s);
                }
                state->structures.push_back(std::move(structure));
                firstSlots.push_back(split ? static_cast<std::uint32_t>(first) : 0u);
            }
            if (!state->structures.empty()) {
                const VkDeviceSize structureBytes = (state->structures.size() + 1) * 4 * sizeof(std::uint32_t);

                if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(imp.device, structureBytes, kAddressedUsage, state->structureTable); !s) {
                    return imp.abandonUpdate(imp.failure("GpuTracer could not hold its structure table", s));
                }

                std::uint32_t* words = static_cast<std::uint32_t*>(state->structureTable.mapped());

                std::memset(words, 0, structureBytes);
                for (std::size_t i = 0; i < state->structures.size(); ++i) {
                    const VkDeviceAddress address = state->structures[i].deviceAddress();

                    words[i * 4] = static_cast<std::uint32_t>(address & 0xffffffffull);
                    words[i * 4 + 1] = static_cast<std::uint32_t>(address >> 32);
                    words[i * 4 + 2] = firstSlots[i];
                    words[i * 4 + 3] = 0;
                }
            }
            if (!movers.empty() && !imp.buildMovers(snapshot, placements, movers, denseIndices, *state, stop, stopped)) {
                return false;
            }
        }
        imp.scene = std::move(state);
        if (structures) {
            ++(rebuildInstances ? imp.counters.instanceSceneBuilds : imp.counters.instanceSceneUpdates);
        }
    }
    imp.placements.swap(placements);
    if (wasStopped(stop)) {
        return imp.abandonUpdate(stopped);
    }

    imp.version = snapshot.version;
    imp.synced = true;
    imp.structured = structures || (imp.structured && !instancesChanged && !staled);

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

static_assert(sizeof(KernelVisibilityParams) == utility_raster_visibility_push_bytes, "KernelVisibilityParams must match VisibilityParams in Kernels/RasterVisibility.slang");

gpu::GpuStatus
GpuTracer::Implementation::buildRasterTables(std::shared_ptr<const RasterTables>& out) const
{
    auto tables = std::make_shared<RasterTables>();
    // The drawn placements of each geometry, in the order the geometries first appear.
    std::vector<std::pair<GeometryEntryPtr, std::vector<std::uint32_t>>> groups;
    std::map<const GeometryEntry*, std::size_t> groupOf;

    for (std::size_t i = 0; i < placements.size(); ++i) {
        const auto it = geometries.find(placements[i].geometry);

        if (it == geometries.end() || !it->second->draws()) {
            continue;
        }

        const auto group = groupOf.emplace(it->second.get(), groups.size());

        if (group.second) {
            groups.emplace_back(it->second, std::vector<std::uint32_t>());
        }
        groups[group.first->second].second.push_back(static_cast<std::uint32_t>(i));
    }

    std::vector<std::uint32_t> instances;
    std::vector<float> rows;
    std::vector<VkDrawIndexedIndirectCommand> commands;

    for (const auto& group : groups) {
        const GeometryEntry& entry = *group.first;
        // The vertex stage addresses each float of the points with 32 bits, and a draw counts its indices in 32.
        const std::size_t floats = 3 * entry.pointSteps * entry.pointCount;

        if (floats > std::numeric_limits<std::uint32_t>::max() || 3 * entry.triangleCount > std::numeric_limits<std::uint32_t>::max()) {
            return { VK_ERROR_OUT_OF_DEVICE_MEMORY, "GpuTracer: a geometry of " + std::to_string(entry.pointCount) + " points and " + std::to_string(entry.triangleCount) + " triangles is past what a raster draw addresses" };
        }

        GpuTracer::RasterGeometry geometry;

        geometry.points = &entry.vertices;
        geometry.indices = &entry.indices;
        geometry.pointCount = static_cast<std::uint32_t>(entry.pointCount);
        geometry.triangleCount = static_cast<std::uint32_t>(entry.triangleCount);
        geometry.pointSteps = static_cast<std::uint32_t>(entry.pointSteps);
        geometry.firstInstance = static_cast<std::uint32_t>(instances.size() / 2);
        geometry.instanceCount = static_cast<std::uint32_t>(group.second.size());
        for (std::uint32_t i : group.second) {
            const Placement& placement = placements[i];

            instances.push_back(i);
            instances.push_back(static_cast<std::uint32_t>(rows.size() / 4));
            appendWords(rows, static_cast<std::uint32_t>(placement.steps.size()), 0u, 0u, 0u);
            if (placement.steps.empty()) {
                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        rows.push_back(static_cast<float>(placement.transform[row][column]));
                    }
                }
            } else {
                for (const UtilityScene::Decomposition& step : placement.steps) {
                    appendStep(rows, step);
                }
            }
        }

        VkDrawIndexedIndirectCommand command {};

        command.indexCount = 3 * geometry.triangleCount;
        command.instanceCount = geometry.instanceCount;
        command.firstInstance = geometry.firstInstance;
        commands.push_back(command);
        tables->geometries.push_back(geometry);
        tables->drawn.push_back(group.first);
    }
    if (rows.size() / 4 > std::numeric_limits<std::uint32_t>::max()) {
        return { VK_ERROR_OUT_OF_DEVICE_MEMORY, "GpuTracer: the transforms of " + std::to_string(instances.size() / 2) + " instances are past what a raster draw addresses" };
    }
    if (!commands.empty()) {
        if (gpu::GpuStatus s = stageWith(device, instances.data(), instances.size() * sizeof(std::uint32_t), kAddressedUsage, tables->instanceTable); !s) {
            return { s.result, "GpuTracer could not hold the instance table of its raster scene: " + s.message };
        }
        if (gpu::GpuStatus s = stageWith(device, rows.data(), rows.size() * sizeof(float), kAddressedUsage, tables->transformTable); !s) {
            return { s.result, "GpuTracer could not hold the transforms of its raster scene: " + s.message };
        }
        if (gpu::GpuStatus s = stageWith(device, commands.data(), commands.size() * sizeof(VkDrawIndexedIndirectCommand), gpu::kGpuBufferDefaultUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, tables->commandTable); !s) {
            return { s.result, "GpuTracer could not hold the draws of its raster scene: " + s.message };
        }
        tables->instances = &tables->instanceTable;
        tables->transforms = &tables->transformTable;
        tables->commands = &tables->commandTable;

        const std::size_t geometryRow = 1 + placements.size();
        const std::size_t hitRows = geometryRow + 2 * tables->geometries.size();

        if (hitRows > std::numeric_limits<std::uint32_t>::max() / 4) {
            return { VK_ERROR_OUT_OF_DEVICE_MEMORY, "GpuTracer: the hit tables of " + std::to_string(placements.size()) + " instances are past what a raster pass addresses" };
        }

        std::vector<std::uint32_t> hits(4 * hitRows, 0u);
        const VkDeviceAddress transforms = tables->transformTable.deviceAddress(device);
        const std::uint32_t* denseGeometry = static_cast<const std::uint32_t*>(scene->instanceGeometry.mapped());

        hits[0] = static_cast<std::uint32_t>(transforms & 0xffffffffull);
        hits[1] = static_cast<std::uint32_t>(transforms >> 32);
        for (std::size_t i = 0; i < placements.size(); ++i) {
            std::fill_n(hits.begin() + 4 * (1 + i), 3, kNoHit);
        }
        for (std::size_t g = 0; g < tables->geometries.size(); ++g) {
            const GpuTracer::RasterGeometry& geometry = tables->geometries[g];
            const std::size_t record = geometryRow + 2 * g;
            const VkDeviceAddress points = geometry.points->deviceAddress(device);
            const VkDeviceAddress indices = geometry.indices->deviceAddress(device);

            hits[4 * record] = static_cast<std::uint32_t>(points & 0xffffffffull);
            hits[4 * record + 1] = static_cast<std::uint32_t>(points >> 32);
            hits[4 * record + 2] = static_cast<std::uint32_t>(indices & 0xffffffffull);
            hits[4 * record + 3] = static_cast<std::uint32_t>(indices >> 32);
            hits[4 * record + 4] = geometry.pointCount;
            hits[4 * record + 5] = geometry.pointSteps;
            for (std::uint32_t k = 0; k < geometry.instanceCount; ++k) {
                const std::uint32_t slot = geometry.firstInstance + k;
                const std::uint32_t placement = instances[2 * slot];
                std::uint32_t* row = &hits[4 * (1 + placement)];

                row[0] = denseGeometry[placement];
                row[1] = instances[2 * slot + 1];
                row[2] = static_cast<std::uint32_t>(record);
                row[3] = 0u;
            }
        }
        if (gpu::GpuStatus s = stageWith(device, hits.data(), hits.size() * sizeof(std::uint32_t), kAddressedUsage, tables->hitTable); !s) {
            return { s.result, "GpuTracer could not hold the hit tables of its raster scene: " + s.message };
        }
        tables->hitTables = &tables->hitTable;
    }
    out = std::move(tables);

    return {};
}

gpu::GpuStatus
GpuTracer::rasterScene(std::shared_ptr<const RasterScene>& out) const
{
    const Implementation& imp = *_imp;

    out.reset();
    if (!imp.usable) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, imp.outcome.message };
    }
    if (!imp.scene) {
        out = std::make_shared<const RasterScene>();

        return {};
    }

    std::lock_guard<std::mutex> lock(imp.scene->rasterMutex);

    if (!imp.scene->raster) {
        if (gpu::GpuStatus s = imp.buildRasterTables(imp.scene->raster); !s) {
            return s;
        }
    }
    out = imp.scene->raster;

    return {};
}

gpu::GpuStatus
GpuTracer::recordVisibility(gpu::GpuRecordContext& ctx,
                            const VisibilityPass& pass) const
{
    const Implementation& imp = *_imp;

    if (!imp.usable) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, imp.outcome.message };
    }
    if (!imp.rasterizes) {
        return { VK_ERROR_FEATURE_NOT_PRESENT, "GpuTracer: the GPU cannot draw a visibility buffer" };
    }
    if (&ctx.registry() != &imp.registry) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuTracer::recordVisibility needs a context of the tracer's own kernel registry" };
    }

    const gpu::GpuTexture* attachment = ctx.rasterAttachment();

    // Without depth the last triangle drawn would win rather than the nearest, so the ids would be wrong.
    if (!attachment || attachment->format() != VK_FORMAT_R32G32_UINT || !ctx.rasterDepthAttachment()) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuTracer::recordVisibility needs an open raster pass into an R32G32_UINT attachment with depth" };
    }

    std::shared_ptr<const RasterScene> scene;

    if (gpu::GpuStatus s = rasterScene(scene); !s) {
        return s;
    }
    if (scene->geometries.empty()) {
        return {};
    }
    ctx.hold(std::const_pointer_cast<RasterScene>(scene));

    KernelVisibilityParams params {};

    std::memcpy(params.viewProjection, pass.viewProjection, sizeof(params.viewProjection));
    params.jitter[0] = 2.f * pass.jitter[0] / static_cast<float>(attachment->desc().width);
    params.jitter[1] = 2.f * pass.jitter[1] / static_cast<float>(attachment->desc().height);
    params.time = pass.time;

    const VkDeviceAddress instances = scene->instances->deviceAddress(imp.device);
    const VkDeviceAddress transforms = scene->transforms->deviceAddress(imp.device);

    params.instancesLow = static_cast<std::uint32_t>(instances & 0xffffffffull);
    params.instancesHigh = static_cast<std::uint32_t>(instances >> 32);
    params.rowsLow = static_cast<std::uint32_t>(transforms & 0xffffffffull);
    params.rowsHigh = static_cast<std::uint32_t>(transforms >> 32);

    const std::span<const std::byte> push(reinterpret_cast<const std::byte*>(&params), sizeof(params));

    for (std::size_t g = 0; g < scene->geometries.size(); ++g) {
        const RasterGeometry& geometry = scene->geometries[g];
        const VkDeviceAddress points = geometry.points->deviceAddress(imp.device);

        params.pointCount = geometry.pointCount;
        params.pointSteps = geometry.pointSteps;
        params.pointsLow = static_cast<std::uint32_t>(points & 0xffffffffull);
        params.pointsHigh = static_cast<std::uint32_t>(points >> 32);
        // The context's abort check runs before each draw, so a cancelled render stops between geometries.
        if (gpu::GpuStatus s = ctx.drawIndexedIndirect(imp.visibilityRaster, gpu::GpuRasterBlend::Replace, std::span<const gpu::GpuBuffer* const>(), push, *geometry.indices, *scene->commands, 1, static_cast<std::uint32_t>(g)); !s) {
            return s;
        }
    }

    return {};
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
