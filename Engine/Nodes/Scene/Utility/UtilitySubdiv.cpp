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

#include "UtilitySubdiv.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <numeric>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <pxr/base/tf/hash.h>
#include <pxr/imaging/pxOsd/refinerFactory.h>
#include <pxr/imaging/pxOsd/tokens.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace Far = OpenSubdiv::Far;

NATRON_NAMESPACE_ENTER

struct UtilitySubdivRefinement::Impl {
    PxOsdMeshTopology topology;
    int level = 0;
    std::vector<VtIntArray> faceVaryingIndices;
    std::vector<std::size_t> coarseFaceVaryingCounts;
    PxOsdTopologyRefinerSharedPtr refiner;
    // Shared with an evaluation, which may outlive the refinement once stopped.
    std::shared_ptr<const Far::StencilTable> vertex;
    std::unique_ptr<const Far::StencilTable> varying;
    std::vector<std::unique_ptr<const Far::StencilTable>> faceVarying;
    VtVec3iArray triangles;
    VtIntArray coarseFaces;
    std::vector<VtVec3iArray> faceVaryingTriangles;
    bool smooth = true;
    std::size_t bytes = 0;
};

namespace {
struct LimitPoint {
    float v[3];

    void Clear(void* = nullptr)
    {
        v[0] = v[1] = v[2] = 0.f;
    }

    void AddWithWeight(const LimitPoint& src,
                       float weight)
    {
        v[0] += weight * src.v[0];
        v[1] += weight * src.v[1];
        v[2] += weight * src.v[2];
    }
};

std::size_t
stencilBytes(const Far::StencilTable* table)
{
    if (!table) {
        return 0;
    }

    return table->GetSizes().size() * sizeof(int) + table->GetOffsets().size() * sizeof(Far::Index)
        + table->GetControlIndices().size() * sizeof(Far::Index) + table->GetWeights().size() * sizeof(float);
}

typedef UtilitySubdivRefinement::StopCheck StopCheck;
typedef UtilitySubdivRefinement::Outcome Outcome;

const std::chrono::milliseconds kStopPoll(5);
// The work in a chunk of a long loop, between two stop checks.
const int kChunk = 1 << 14;
// Below these the work takes less than a thread does to start, and runs on the caller's.
const std::size_t kThreadedTriangles = std::size_t(1) << 17;
const std::size_t kThreadedPoints = std::size_t(1) << 16;

bool
never()
{
    return false;
}

struct StoppableJob {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::atomic<bool> abandoned { false };
    std::exception_ptr failure;
};

/**
 * OpenSubdiv's limit evaluation cannot be interrupted, so when threaded the work runs on a thread of
 * its own while the caller polls stopped: a stop then returns within a poll however long the call in
 * progress takes. The abandoned work sees its halt check turn true and ends at its next
 * one, dropping what it made, so it must own everything it touches. work returns false when halted.
 **/
bool
runStoppable(bool threaded,
             const std::function<bool(const StopCheck&)>& work,
             const StopCheck& stopped)
{
    if (!stopped) {
        return work(StopCheck(never));
    }
    if (!threaded) {
        return work(stopped);
    }
    if (stopped()) {
        return false;
    }

    std::shared_ptr<StoppableJob> job = std::make_shared<StoppableJob>();
    std::thread thread;

    try {
        thread = std::thread([job, work]() {
            try {
                work([job]() { return job->abandoned.load(); });
            } catch (...) {
                job->failure = std::current_exception();
            }
            {
                const std::lock_guard<std::mutex> l(job->mutex);

                job->finished = true;
            }
            job->done.notify_all();
        });
    } catch (const std::system_error&) {
        return work(stopped);
    }

    std::unique_lock<std::mutex> l(job->mutex);

    while (!job->finished) {
        if (stopped()) {
            job->abandoned.store(true);
            l.unlock();
            thread.detach();

            return false;
        }
        job->done.wait_for(l, kStopPoll);
    }
    l.unlock();
    thread.join();
    if (job->failure) {
        std::rethrow_exception(job->failure);
    }

    return true;
}

// False, with *halted set when halt turned true, if dst is not filled.
bool
applyStencils(const Far::StencilTable* table,
              const float* src,
              std::size_t count,
              std::size_t expected,
              int width,
              float* dst,
              const StopCheck& halt = StopCheck(never),
              bool* halted = nullptr)
{
    if (!table || count < expected || width <= 0) {
        return false;
    }
    const int* sizes = table->GetSizes().data();
    const Far::Index* offsets = table->GetOffsets().data();
    const Far::Index* indices = table->GetControlIndices().data();
    const float* weights = table->GetWeights().data();
    const int stencils = table->GetNumStencils();
    std::atomic<bool> stop { false };

    tbb::parallel_for(tbb::blocked_range<int>(0, stencils, kChunk), [&](const tbb::blocked_range<int>& range) {
        if (stop.load() || halt()) {
            stop.store(true);

            return;
        }
        for (int i = range.begin(); i < range.end(); ++i) {
            float* out = dst + static_cast<std::size_t>(i) * width;

            std::fill(out, out + width, 0.f);
            for (int j = offsets[i]; j < offsets[i] + sizes[i]; ++j) {
                const float* in = src + static_cast<std::size_t>(indices[j]) * width;
                const float weight = weights[j];

                for (int k = 0; k < width; ++k) {
                    out[k] += weight * in[k];
                }
            }
        }
    });
    if (stop.load()) {
        if (halted) {
            *halted = true;
        }

        return false;
    }

    return true;
}

// The limit points, and normals when smooth, of coarse; both empty when coarse is too short. False
// when halted.
bool
evaluateLimit(const Far::TopologyRefiner& refiner,
              const Far::StencilTable* vertex,
              const VtVec3fArray& coarse,
              std::size_t coarseCount,
              bool smooth,
              const StopCheck& halt,
              VtVec3fArray* points,
              VtVec3fArray* normals)
{
    const std::size_t count = vertex ? static_cast<std::size_t>(vertex->GetNumStencils()) : 0;
    bool halted = false;

    points->clear();
    normals->clear();
    if (count == 0) {
        return true;
    }

    std::vector<LimitPoint> refined(count);

    if (!applyStencils(vertex, reinterpret_cast<const float*>(coarse.cdata()), coarse.size(), coarseCount, 3, &refined[0].v[0], halt, &halted)) {
        return !halted;
    }

    VtVec3fArray limitPoints(count);

    if (!smooth) {
        std::copy(&refined[0].v[0], &refined[0].v[0] + 3 * count, reinterpret_cast<float*>(limitPoints.data()));
        *points = limitPoints;

        return true;
    }
    if (halt()) {
        return false;
    }

    LimitPoint* limit = reinterpret_cast<LimitPoint*>(limitPoints.data());
    std::vector<LimitPoint> du(count);
    std::vector<LimitPoint> dv(count);

    Far::PrimvarRefiner(refiner).Limit(refined, limit, du, dv);
    if (halt()) {
        return false;
    }

    VtVec3fArray limitNormals(count);
    GfVec3f* out = limitNormals.data();
    std::atomic<bool> stop { false };

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, count, kChunk), [&](const tbb::blocked_range<std::size_t>& range) {
        if (stop.load() || halt()) {
            stop.store(true);

            return;
        }
        for (std::size_t i = range.begin(); i < range.end(); ++i) {
            const GfVec3f normal = GfCross(GfVec3f(du[i].v[0], du[i].v[1], du[i].v[2]), GfVec3f(dv[i].v[0], dv[i].v[1], dv[i].v[2]));
            const float length = normal.GetLength();

            out[i] = length > 0.f && std::isfinite(length) ? normal / length : GfVec3f(0.f);
        }
    });
    if (stop.load()) {
        return false;
    }
    *points = limitPoints;
    *normals = limitNormals;

    return true;
}

std::unique_ptr<const Far::StencilTable>
makeStencils(const Far::TopologyRefiner& refiner,
             Far::StencilTableFactory::Mode mode,
             int channel)
{
    Far::StencilTableFactory::Options options;

    options.interpolationMode = mode;
    options.generateOffsets = true;
    options.generateControlVerts = false;
    options.generateIntermediateLevels = false;
    options.fvarChannel = static_cast<unsigned int>(channel);

    return std::unique_ptr<const Far::StencilTable>(Far::StencilTableFactory::Create(refiner, options));
}

std::vector<bool>
holeFaces(const PxOsdMeshTopology& topology)
{
    std::vector<bool> holes(topology.GetFaceVertexCounts().size(), false);

    for (const int hole : topology.GetHoleIndices()) {
        if (hole >= 0 && static_cast<std::size_t>(hole) < holes.size()) {
            holes[hole] = true;
        }
    }

    return holes;
}

std::size_t
keyOf(const PxOsdMeshTopology& topology,
      int level,
      const std::vector<VtIntArray>& faceVaryingIndices)
{
    std::size_t key = TfHash::Combine(topology.ComputeHash(), level, faceVaryingIndices.size());

    for (const VtIntArray& indices : faceVaryingIndices) {
        key = TfHash::Combine(key, TfHash()(indices));
    }

    return key;
}

struct CacheEntry {
    std::size_t key;
    UtilitySubdivRefinementPtr refinement;
};

/**
 * A refinement too long to make on the caller's thread, made on one of its own that every caller
 * asking for it joins while it runs. A stop only lets the caller go: the work halts once no caller
 * waits on it, so a frame stopped by an edit and synced again at once resumes the refinement rather
 * than starting it over, and an abandoned one ends at its next halt check.
 **/
struct Job {
    std::size_t key = 0;
    std::unique_ptr<UtilitySubdivRefinement::Impl> imp;
    std::atomic<int> waiters { 0 };
    std::mutex mutex;
    std::condition_variable done;
    // Under mutex.
    bool finished = false;
    // Written before finished, read after it.
    UtilitySubdivRefinement::Outcome outcome = UtilitySubdivRefinement::Outcome::Failed;
    std::string error;
    // Under the cache's mutex, which also guards imp once the job has finished.
    UtilitySubdivRefinementPtr made;
};

// Meshes hold their refinement, so an evicted entry lives on while one still uses it.
struct Cache {
    std::mutex mutex;
    std::list<CacheEntry> entries;
    std::unordered_multimap<std::size_t, std::list<CacheEntry>::iterator> byKey;
    std::size_t bytes = 0;
    std::size_t budget = std::size_t(1) << 30;
    std::list<std::shared_ptr<Job>> building;
    std::atomic<std::size_t> refinementsStarted { 0 };
    std::atomic<std::size_t> refinerBuilds { 0 };
    std::atomic<std::size_t> evaluations { 0 };

    void evict()
    {
        while (bytes > budget && entries.size() > 1) {
            const CacheEntry& last = entries.back();
            auto range = byKey.equal_range(last.key);

            for (auto it = range.first; it != range.second; ++it) {
                if (it->second == std::prev(entries.end())) {
                    byKey.erase(it);
                    break;
                }
            }
            bytes -= last.refinement->memoryBytes();
            entries.pop_back();
        }
    }
};

Cache&
cache()
{
    static Cache instance;

    return instance;
}

bool
sameRefinement(const UtilitySubdivRefinement::Impl& imp,
               const PxOsdMeshTopology& topology,
               int level,
               const std::vector<VtIntArray>& faceVaryingIndices)
{
    return imp.level == level && imp.topology == topology && imp.faceVaryingIndices == faceVaryingIndices;
}

// Null, with error set, when OpenSubdiv is not to be asked to refine topology.
std::unique_ptr<UtilitySubdivRefinement::Impl>
prepare(const PxOsdMeshTopology& topology,
        int level,
        const std::vector<VtIntArray>& faceVaryingIndices,
        std::string* error)
{
    const TfToken scheme = topology.GetScheme();

    if (scheme == PxOsdOpenSubdivTokens->none) {
        *error = "the scheme is none";

        return nullptr;
    }
    if (scheme != PxOsdOpenSubdivTokens->catmullClark && scheme != PxOsdOpenSubdivTokens->loop && scheme != PxOsdOpenSubdivTokens->bilinear) {
        *error = "unknown subdivision scheme " + scheme.GetString();

        return nullptr;
    }
    const VtIntArray& counts = topology.GetFaceVertexCounts();

    if (scheme == PxOsdOpenSubdivTokens->loop && std::any_of(counts.begin(), counts.end(), [](int n) { return n != 3; })) {
        *error = "Loop subdivision needs a mesh of triangles";

        return nullptr;
    }
    const PxOsdMeshTopologyValidation validation = topology.Validate();

    if (!validation) {
        *error = "OpenSubdiv rejects the topology";
        for (const PxOsdMeshTopologyValidation::Invalidation& invalidation : validation) {
            *error += ": " + invalidation.message;
            break;
        }

        return nullptr;
    }
    const std::size_t faceVertexCount = topology.GetFaceVertexIndices().size();

    for (const VtIntArray& indices : faceVaryingIndices) {
        if (indices.size() != faceVertexCount || std::any_of(indices.begin(), indices.end(), [](int i) { return i < 0; })) {
            *error = "a face-varying channel does not index every face-vertex";

            return nullptr;
        }
    }

    std::unique_ptr<UtilitySubdivRefinement::Impl> imp(new UtilitySubdivRefinement::Impl);

    imp->topology = topology;
    imp->level = level;
    imp->faceVaryingIndices = faceVaryingIndices;
    imp->smooth = scheme != PxOsdOpenSubdivTokens->bilinear;
    for (const VtIntArray& indices : faceVaryingIndices) {
        imp->coarseFaceVaryingCounts.push_back(indices.empty() ? 0 : static_cast<std::size_t>(*std::max_element(indices.begin(), indices.end())) + 1);
    }

    return imp;
}

// Fills what prepare() left of imp, checking halt between OpenSubdiv's calls and through the loops.
Outcome
refineInto(UtilitySubdivRefinement::Impl* imp,
           const StopCheck& halt,
           std::string* error)
{
    const PxOsdMeshTopology& topology = imp->topology;
    const std::vector<VtIntArray>& faceVaryingIndices = imp->faceVaryingIndices;
    const int level = imp->level;

    imp->refiner = faceVaryingIndices.empty() ? PxOsdRefinerFactory::Create(topology) : PxOsdRefinerFactory::Create(topology, faceVaryingIndices);
    if (!imp->refiner) {
        *error = "OpenSubdiv rejects the topology";

        return Outcome::Failed;
    }
    if (halt()) {
        return Outcome::Stopped;
    }

    Far::TopologyRefiner::UniformOptions options(level);

    // Limit() needs the last level's full topology.
    options.fullTopologyInLastLevel = true;
    imp->refiner->RefineUniform(options);
    if (imp->refiner->GetMaxLevel() != level) {
        *error = "OpenSubdiv could not refine the topology";

        return Outcome::Failed;
    }
    if (halt()) {
        return Outcome::Stopped;
    }

    const Far::TopologyRefiner& refiner = *imp->refiner;
    const Far::TopologyLevel& last = refiner.GetLevel(level);
    const std::size_t channels = faceVaryingIndices.size();
    std::vector<std::unique_ptr<const Far::StencilTable>> tables(2 + channels);

    // Each table only reads the refined refiner, so they are made side by side.
    tbb::parallel_for(std::size_t(0), tables.size(), [&](std::size_t table) {
        if (halt()) {
            return;
        }
        if (table == 0) {
            tables[table] = makeStencils(refiner, Far::StencilTableFactory::INTERPOLATE_VERTEX, 0);
        } else if (table == 1) {
            tables[table] = makeStencils(refiner, Far::StencilTableFactory::INTERPOLATE_VARYING, 0);
        } else {
            tables[table] = makeStencils(refiner, Far::StencilTableFactory::INTERPOLATE_FACE_VARYING, static_cast<int>(table - 2));
        }
    });
    if (halt()) {
        return Outcome::Stopped;
    }
    imp->vertex = std::move(tables[0]);
    imp->varying = std::move(tables[1]);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        imp->faceVarying.push_back(std::move(tables[2 + channel]));
    }
    if (!imp->vertex || !imp->varying || imp->vertex->GetNumStencils() != last.GetNumVertices()) {
        *error = "OpenSubdiv could not make the stencils";

        return Outcome::Failed;
    }

    std::vector<int> parents(refiner.GetLevel(0).GetNumFaces());

    std::iota(parents.begin(), parents.end(), 0);
    for (int l = 1; l <= level; ++l) {
        std::vector<int> children(refiner.GetLevel(l).GetNumFaces());

        Far::PrimvarRefiner(refiner).InterpolateFaceUniform(l, parents, children);
        parents.swap(children);
        if (halt()) {
            return Outcome::Stopped;
        }
    }

    const int faceCount = last.GetNumFaces();
    std::vector<GfVec3i> triangles;
    std::vector<int> coarseFaces;
    std::vector<std::vector<GfVec3i>> faceVaryingTriangles(channels);

    triangles.reserve(static_cast<std::size_t>(faceCount) * 2);
    coarseFaces.reserve(static_cast<std::size_t>(faceCount) * 2);
    for (int face = 0; face < faceCount; ++face) {
        if (face % kChunk == 0 && halt()) {
            return Outcome::Stopped;
        }
        if (last.IsFaceHole(face)) {
            continue;
        }
        const Far::ConstIndexArray vertices = last.GetFaceVertices(face);
        const int n = vertices.size();

        for (int corner = 1; corner + 1 < n; ++corner) {
            triangles.emplace_back(vertices[0], vertices[corner], vertices[corner + 1]);
            coarseFaces.push_back(parents[face]);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const Far::ConstIndexArray values = last.GetFaceFVarValues(face, static_cast<int>(channel));

                faceVaryingTriangles[channel].emplace_back(values[0], values[corner], values[corner + 1]);
            }
        }
    }
    imp->triangles.assign(triangles.begin(), triangles.end());
    imp->coarseFaces.assign(coarseFaces.begin(), coarseFaces.end());
    for (const std::vector<GfVec3i>& values : faceVaryingTriangles) {
        imp->faceVaryingTriangles.emplace_back(values.begin(), values.end());
    }

    imp->bytes = stencilBytes(imp->vertex.get()) + stencilBytes(imp->varying.get())
        + imp->triangles.size() * (sizeof(GfVec3i) * (1 + channels) + sizeof(int))
        // The refiner's own levels: a rough count of its per-vertex and per-face records.
        + static_cast<std::size_t>(last.GetNumVertices() + last.GetNumFaces()) * 64;
    for (const std::unique_ptr<const Far::StencilTable>& table : imp->faceVarying) {
        imp->bytes += stencilBytes(table.get());
    }

    return halt() ? Outcome::Stopped : Outcome::Done;
}

// Hydra's sync must not see OpenSubdiv's exceptions, so they fail the refinement instead.
void
runJob(const std::shared_ptr<Job>& job,
       const StopCheck& halt)
{
    Outcome outcome = Outcome::Failed;
    std::string error;

    try {
        outcome = refineInto(job->imp.get(), halt, &error);
    } catch (const std::exception& e) {
        error = std::string("OpenSubdiv failed: ") + e.what();
    } catch (...) {
        error = "OpenSubdiv failed";
    }
    {
        const std::lock_guard<std::mutex> l(job->mutex);

        job->outcome = outcome;
        job->error = error;
        job->finished = true;
    }
    job->done.notify_all();
}
} // anon namespace

UtilitySubdivRefinement::UtilitySubdivRefinement(std::unique_ptr<Impl> impl)
    : _imp(std::move(impl))
{
}

UtilitySubdivRefinement::~UtilitySubdivRefinement()
{
}

UtilitySubdivRefinementPtr
UtilitySubdivRefinement::get(const PxOsdMeshTopology& topology,
                             int level,
                             const std::vector<VtIntArray>& faceVaryingIndices,
                             std::string* error)
{
    return get(topology, level, faceVaryingIndices, StopCheck(), nullptr, error);
}

UtilitySubdivRefinementPtr
UtilitySubdivRefinement::get(const PxOsdMeshTopology& topology,
                             int level,
                             const std::vector<VtIntArray>& faceVaryingIndices,
                             const StopCheck& stopped,
                             Outcome* outcome,
                             std::string* error)
{
    std::string ignored;
    Outcome ignoredOutcome;

    if (!error) {
        error = &ignored;
    }
    if (!outcome) {
        outcome = &ignoredOutcome;
    }
    *outcome = Outcome::Failed;
    if (level < 1) {
        *error = "the level is below 1";

        return nullptr;
    }
    level = std::min(level, kMaxLevel);

    Cache& c = cache();
    const std::size_t key = keyOf(topology, level, faceVaryingIndices);
    const auto find = [&]() -> UtilitySubdivRefinementPtr {
        auto range = c.byKey.equal_range(key);

        for (auto it = range.first; it != range.second; ++it) {
            if (sameRefinement(*it->second->refinement->_imp, topology, level, faceVaryingIndices)) {
                c.entries.splice(c.entries.begin(), c.entries, it->second);

                return it->second->refinement;
            }
        }

        return nullptr;
    };

    {
        std::lock_guard<std::mutex> l(c.mutex);

        if (UtilitySubdivRefinementPtr found = find()) {
            *outcome = Outcome::Done;

            return found;
        }
    }

    const auto publish = [&](const UtilitySubdivRefinementPtr& made) -> UtilitySubdivRefinementPtr {
        if (UtilitySubdivRefinementPtr found = find()) {
            return found;
        }
        c.entries.push_front(CacheEntry { key, made });
        c.byKey.emplace(key, c.entries.begin());
        c.bytes += made->memoryBytes();
        c.evict();

        return made;
    };
    const auto stoppedNow = [&]() -> UtilitySubdivRefinementPtr {
        *outcome = Outcome::Stopped;
        *error = "the refinement was stopped";

        return nullptr;
    };

    if (estimateTriangles(topology, level) < kThreadedTriangles) {
        std::unique_ptr<Impl> imp = prepare(topology, level, faceVaryingIndices, error);

        if (!imp) {
            return nullptr;
        }

        ++c.refinementsStarted;

        const Outcome refined = refineInto(imp.get(), stopped ? stopped : StopCheck(never), error);

        if (refined == Outcome::Stopped) {
            return stoppedNow();
        }
        if (refined == Outcome::Failed) {
            return nullptr;
        }
        ++c.refinerBuilds;
        *outcome = Outcome::Done;

        const UtilitySubdivRefinementPtr made = std::make_shared<const UtilitySubdivRefinement>(std::move(imp));
        const std::lock_guard<std::mutex> l(c.mutex);

        return publish(made);
    }

    for (;;) {
        // Made outside the lock, as the refinement is, so other prims' lookups do not wait on it.
        std::unique_ptr<Impl> prepared = prepare(topology, level, faceVaryingIndices, error);

        if (!prepared) {
            return nullptr;
        }

        std::shared_ptr<Job> job;
        bool started = false;

        {
            const std::lock_guard<std::mutex> l(c.mutex);

            if (UtilitySubdivRefinementPtr found = find()) {
                *outcome = Outcome::Done;

                return found;
            }
            for (auto it = c.building.begin(); it != c.building.end();) {
                const std::shared_ptr<Job>& other = *it;

                if (!job && other->key == key && sameRefinement(*other->imp, topology, level, faceVaryingIndices)) {
                    job = other;
                    ++job->waiters;
                    ++it;
                    continue;
                }

                bool idle = false;

                {
                    const std::lock_guard<std::mutex> jl(other->mutex);

                    idle = other->finished && other->waiters.load() == 0;
                }
                it = idle ? c.building.erase(it) : std::next(it);
            }
            if (!job) {
                job = std::make_shared<Job>();
                job->key = key;
                job->imp = std::move(prepared);
                job->waiters.store(1);
                c.building.push_back(job);
                ++c.refinementsStarted;
                started = true;
            }
        }
        if (started) {
            try {
                std::thread([job]() { runJob(job, [job]() { return job->waiters.load() == 0; }); }).detach();
            } catch (const std::system_error&) {
                runJob(job, stopped ? stopped : StopCheck(never));
            }
        }
        {
            std::unique_lock<std::mutex> l(job->mutex);

            while (!job->finished) {
                if (stopped && stopped()) {
                    --job->waiters;

                    return stoppedNow();
                }
                if (stopped) {
                    job->done.wait_for(l, kStopPoll);
                } else {
                    job->done.wait(l);
                }
            }
        }
        --job->waiters;

        const std::lock_guard<std::mutex> l(c.mutex);

        c.building.remove(job);
        if (job->outcome == Outcome::Stopped) {
            // It halted for want of a caller before this one joined it, so the work starts again.
            if (stopped && stopped()) {
                return stoppedNow();
            }
            continue;
        }
        if (job->outcome == Outcome::Failed) {
            *error = job->error;

            return nullptr;
        }
        if (!job->made) {
            ++c.refinerBuilds;
            job->made = publish(std::make_shared<const UtilitySubdivRefinement>(std::move(job->imp)));
        }
        *outcome = Outcome::Done;

        return job->made;
    }
}

std::size_t
UtilitySubdivRefinement::estimateTriangles(const PxOsdMeshTopology& topology,
                                           int level)
{
    const VtIntArray& counts = topology.GetFaceVertexCounts();
    const std::vector<bool> holes = holeFaces(topology);
    const bool loop = topology.GetScheme() == PxOsdOpenSubdivTokens->loop;
    const double growth = level > 0 ? std::pow(4., level - 1) : 0.;
    double total = 0.;

    for (std::size_t face = 0; face < counts.size(); ++face) {
        const int n = counts[face];

        if (holes[face] || n < 3) {
            continue;
        }
        if (level <= 0) {
            total += n - 2;
        } else if (loop) {
            total += 4. * growth;
        } else {
            total += 2. * n * growth;
        }
    }

    return total >= static_cast<double>(std::numeric_limits<std::size_t>::max()) ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(total);
}

int
UtilitySubdivRefinement::fitLevel(const PxOsdMeshTopology& topology,
                                  int level,
                                  std::size_t maxTriangles)
{
    level = std::max(0, std::min(level, kMaxLevel));
    while (level > 0 && estimateTriangles(topology, level) > maxTriangles) {
        --level;
    }

    return level;
}

UtilitySubdivRefinement::Stats
UtilitySubdivRefinement::stats()
{
    Cache& c = cache();
    Stats stats;

    stats.refinementsStarted = c.refinementsStarted.load();
    stats.refinerBuilds = c.refinerBuilds.load();
    stats.evaluations = c.evaluations.load();

    std::lock_guard<std::mutex> l(c.mutex);

    stats.cachedEntries = c.entries.size();
    stats.cachedBytes = c.bytes;

    return stats;
}

void
UtilitySubdivRefinement::setCacheBudget(std::size_t bytes)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> l(c.mutex);

    c.budget = bytes;
    c.evict();
}

void
UtilitySubdivRefinement::clearCache()
{
    Cache& c = cache();
    std::lock_guard<std::mutex> l(c.mutex);

    c.entries.clear();
    c.byKey.clear();
    c.bytes = 0;
}

int
UtilitySubdivRefinement::level() const
{
    return _imp->level;
}

std::size_t
UtilitySubdivRefinement::coarsePointCount() const
{
    return static_cast<std::size_t>(_imp->refiner->GetLevel(0).GetNumVertices());
}

std::size_t
UtilitySubdivRefinement::refinedPointCount() const
{
    return static_cast<std::size_t>(_imp->vertex->GetNumStencils());
}

const VtVec3iArray&
UtilitySubdivRefinement::triangles() const
{
    return _imp->triangles;
}

const VtIntArray&
UtilitySubdivRefinement::coarseFaces() const
{
    return _imp->coarseFaces;
}

int
UtilitySubdivRefinement::faceVaryingChannelCount() const
{
    return static_cast<int>(_imp->faceVarying.size());
}

std::size_t
UtilitySubdivRefinement::refinedFaceVaryingCount(int channel) const
{
    if (channel < 0 || channel >= faceVaryingChannelCount() || !_imp->faceVarying[channel]) {
        return 0;
    }

    return static_cast<std::size_t>(_imp->faceVarying[channel]->GetNumStencils());
}

const VtVec3iArray&
UtilitySubdivRefinement::faceVaryingTriangles(int channel) const
{
    static const VtVec3iArray none;

    return channel >= 0 && channel < faceVaryingChannelCount() ? _imp->faceVaryingTriangles[channel] : none;
}

std::size_t
UtilitySubdivRefinement::memoryBytes() const
{
    return _imp->bytes;
}

void
UtilitySubdivRefinement::refine(const VtVec3fArray& coarse,
                                VtVec3fArray* refined) const
{
    ++cache().evaluations;
    refined->resize(refinedPointCount());
    if (!interpolateVertex(reinterpret_cast<const float*>(coarse.cdata()), coarse.size(), 3, reinterpret_cast<float*>(refined->data()))) {
        refined->clear();
    }
}

void
UtilitySubdivRefinement::evaluate(const VtVec3fArray& coarse,
                                  VtVec3fArray* points,
                                  VtVec3fArray* normals) const
{
    (void)evaluate(coarse, points, normals, StopCheck());
}

bool
UtilitySubdivRefinement::evaluate(const VtVec3fArray& coarse,
                                  VtVec3fArray* points,
                                  VtVec3fArray* normals,
                                  const StopCheck& stopped) const
{
    struct Evaluation {
        VtVec3fArray points;
        VtVec3fArray normals;
    };

    std::shared_ptr<Evaluation> evaluation = std::make_shared<Evaluation>();
    const PxOsdTopologyRefinerSharedPtr refiner = _imp->refiner;
    const std::shared_ptr<const Far::StencilTable> vertex = _imp->vertex;
    const std::size_t coarseCount = coarsePointCount();
    const bool smooth = _imp->smooth;

    ++cache().evaluations;

    const auto work = [evaluation, refiner, vertex, coarse, coarseCount, smooth](const StopCheck& halt) {
        return evaluateLimit(*refiner, vertex.get(), coarse, coarseCount, smooth, halt, &evaluation->points, &evaluation->normals);
    };
    const bool finished = runStoppable(refinedPointCount() >= kThreadedPoints, work, stopped);

    if (!finished) {
        return false;
    }
    *points = evaluation->points;
    *normals = evaluation->normals;

    return true;
}

bool
UtilitySubdivRefinement::interpolateVertex(const float* src,
                                           std::size_t count,
                                           int width,
                                           float* dst) const
{
    return applyStencils(_imp->vertex.get(), src, count, coarsePointCount(), width, dst);
}

bool
UtilitySubdivRefinement::interpolateVarying(const float* src,
                                            std::size_t count,
                                            int width,
                                            float* dst) const
{
    return applyStencils(_imp->varying.get(), src, count, coarsePointCount(), width, dst);
}

bool
UtilitySubdivRefinement::interpolateFaceVarying(int channel,
                                                const float* src,
                                                std::size_t count,
                                                int width,
                                                float* dst) const
{
    if (channel < 0 || channel >= faceVaryingChannelCount()) {
        return false;
    }

    return applyStencils(_imp->faceVarying[channel].get(), src, count, _imp->coarseFaceVaryingCounts[channel], width, dst);
}

NATRON_NAMESPACE_EXIT
