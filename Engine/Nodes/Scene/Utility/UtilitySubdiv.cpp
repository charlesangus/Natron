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
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <numeric>
#include <unordered_map>

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefiner.h>

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
    std::unique_ptr<const Far::StencilTable> vertex;
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

bool
applyStencils(const Far::StencilTable* table,
              const float* src,
              std::size_t count,
              std::size_t expected,
              int width,
              float* dst)
{
    if (!table || count < expected || width <= 0) {
        return false;
    }
    const int* sizes = table->GetSizes().data();
    const Far::Index* offsets = table->GetOffsets().data();
    const Far::Index* indices = table->GetControlIndices().data();
    const float* weights = table->GetWeights().data();
    const int stencils = table->GetNumStencils();

    for (int i = 0; i < stencils; ++i) {
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

// Meshes hold their refinement, so an evicted entry lives on while one still uses it.
struct Cache {
    std::mutex mutex;
    std::list<CacheEntry> entries;
    std::unordered_multimap<std::size_t, std::list<CacheEntry>::iterator> byKey;
    std::size_t bytes = 0;
    std::size_t budget = std::size_t(1) << 30;
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

std::unique_ptr<UtilitySubdivRefinement::Impl>
build(const PxOsdMeshTopology& topology,
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
    imp->refiner = faceVaryingIndices.empty() ? PxOsdRefinerFactory::Create(topology) : PxOsdRefinerFactory::Create(topology, faceVaryingIndices);
    if (!imp->refiner) {
        *error = "OpenSubdiv rejects the topology";

        return nullptr;
    }

    Far::TopologyRefiner::UniformOptions options(level);

    // Limit() needs the last level's full topology.
    options.fullTopologyInLastLevel = true;
    imp->refiner->RefineUniform(options);
    if (imp->refiner->GetMaxLevel() != level) {
        *error = "OpenSubdiv could not refine the topology";

        return nullptr;
    }

    const Far::TopologyRefiner& refiner = *imp->refiner;
    const Far::TopologyLevel& last = refiner.GetLevel(level);

    imp->vertex = makeStencils(refiner, Far::StencilTableFactory::INTERPOLATE_VERTEX, 0);
    imp->varying = makeStencils(refiner, Far::StencilTableFactory::INTERPOLATE_VARYING, 0);
    for (std::size_t channel = 0; channel < faceVaryingIndices.size(); ++channel) {
        imp->faceVarying.push_back(makeStencils(refiner, Far::StencilTableFactory::INTERPOLATE_FACE_VARYING, static_cast<int>(channel)));
    }
    if (!imp->vertex || !imp->varying || imp->vertex->GetNumStencils() != last.GetNumVertices()) {
        *error = "OpenSubdiv could not make the stencils";

        return nullptr;
    }

    std::vector<int> parents(refiner.GetLevel(0).GetNumFaces());

    std::iota(parents.begin(), parents.end(), 0);
    for (int l = 1; l <= level; ++l) {
        std::vector<int> children(refiner.GetLevel(l).GetNumFaces());

        Far::PrimvarRefiner(refiner).InterpolateFaceUniform(l, parents, children);
        parents.swap(children);
    }

    const int faceCount = last.GetNumFaces();
    const std::size_t channels = faceVaryingIndices.size();
    std::vector<GfVec3i> triangles;
    std::vector<int> coarseFaces;
    std::vector<std::vector<GfVec3i>> faceVaryingTriangles(channels);

    triangles.reserve(static_cast<std::size_t>(faceCount) * 2);
    coarseFaces.reserve(static_cast<std::size_t>(faceCount) * 2);
    for (int face = 0; face < faceCount; ++face) {
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

    return imp;
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
    std::string ignored;

    if (!error) {
        error = &ignored;
    }
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
            return found;
        }
    }

    // Built outside the lock, so other prims' lookups do not wait on a long refinement.
    std::unique_ptr<Impl> imp = build(topology, level, faceVaryingIndices, error);

    if (!imp) {
        return nullptr;
    }
    ++c.refinerBuilds;

    UtilitySubdivRefinementPtr made = std::make_shared<const UtilitySubdivRefinement>(std::move(imp));
    std::lock_guard<std::mutex> l(c.mutex);

    if (UtilitySubdivRefinementPtr found = find()) {
        return found;
    }
    c.entries.push_front(CacheEntry { key, made });
    c.byKey.emplace(key, c.entries.begin());
    c.bytes += made->memoryBytes();
    c.evict();

    return made;
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
    const std::size_t count = refinedPointCount();
    std::vector<LimitPoint> refined(count);

    ++cache().evaluations;
    normals->clear();
    if (count == 0 || !interpolateVertex(reinterpret_cast<const float*>(coarse.cdata()), coarse.size(), 3, &refined[0].v[0])) {
        points->clear();

        return;
    }
    points->resize(count);
    if (!_imp->smooth) {
        std::copy(&refined[0].v[0], &refined[0].v[0] + 3 * count, reinterpret_cast<float*>(points->data()));

        return;
    }

    LimitPoint* limit = reinterpret_cast<LimitPoint*>(points->data());
    std::vector<LimitPoint> du(count);
    std::vector<LimitPoint> dv(count);

    Far::PrimvarRefiner(*_imp->refiner).Limit(refined, limit, du, dv);
    normals->resize(count);
    GfVec3f* out = normals->data();
    for (std::size_t i = 0; i < count; ++i) {
        const GfVec3f normal = GfCross(GfVec3f(du[i].v[0], du[i].v[1], du[i].v[2]), GfVec3f(dv[i].v[0], dv[i].v[1], dv[i].v[2]));
        const float length = normal.GetLength();

        out[i] = length > 0.f && std::isfinite(length) ? normal / length : GfVec3f(0.f);
    }
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
