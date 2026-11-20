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

#include "Engine/Nodes/Scene/SceneBudgetProbe.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_SCENEBUDGETPROBE_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/usd/sdf/path.h>
#ifdef NATRON_SCENEBUDGETPROBE_RESTORE_EMIT
#undef NATRON_SCENEBUDGETPROBE_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
using Clock = std::chrono::steady_clock;
using SceneBudgetProbe::KindEnum;
using SceneBudgetProbe::Result;

const char kStormDisplayName[] = "GL";

// Past every phase the renderer's own filters use, so prim types are the ones Storm syncs.
constexpr int kProbePhase = 1000;

// The scene index's own child under a probed mesh. The scene index adapter turns a geomSubset child
// whose visibility is false into the topology's invisibleFaces: HdMeshTopologySchema has no field
// for them, and the legacy path names its own such child __invisibleFaces (libusd_hd).
const char kFacesChildName[] = "__natronBudgetProbeFaces";

constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;

double
msSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t
mix(std::uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;

    return x;
}

// From the path's text, so a choice is the same from one run to the next.
std::uint64_t
pathKey(const SdfPath& path)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const char c : path.GetString()) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001B3ull;
    }

    return mix(hash);
}

double
unitOf(std::uint64_t hash)
{
    return double(hash >> 11) * (1. / 9007199254740992.);
}

double
elementUnit(std::uint64_t key,
            std::size_t index)
{
    return unitOf(mix(key ^ ((std::uint64_t(index) + 1) * kGolden)));
}

const HdDataSourceLocatorSet&
visibilityLocators()
{
    static const HdDataSourceLocatorSet locators { HdVisibilitySchema::GetDefaultLocator() };

    return locators;
}

const HdDataSourceLocatorSet&
maskLocators()
{
    static const HdDataSourceLocatorSet locators { HdInstancerTopologySchema::GetDefaultLocator().Append(HdInstancerTopologySchemaTokens->mask) };

    return locators;
}

const HdDataSourceLocatorSet&
topologyLocators()
{
    static const HdDataSourceLocatorSet locators { HdMeshSchema::GetTopologyLocator() };

    return locators;
}

const HdContainerDataSourceHandle&
invisibleData()
{
    static const HdContainerDataSourceHandle data = HdRetainedContainerDataSource::New(
        HdVisibilitySchema::GetSchemaToken(),
        HdVisibilitySchema::Builder().SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(false)).Build());

    return data;
}

const TfToken&
facesChildToken()
{
    static const TfToken token(kFacesChildName);

    return token;
}

bool
isInstanced(const HdContainerDataSourceHandle& data)
{
    const HdPathArrayDataSourceHandle paths = HdInstancedBySchema::GetFromParent(data).GetPaths();

    return paths && !paths->GetTypedValue(0.f).empty();
}

struct Request {
    KindEnum kind = SceneBudgetProbe::eKindNone;
    double share = 0.;
    bool prepareOnly = false;
};

std::mutex g_requestMutex;
bool g_pending = false;
Request g_request;
Result g_last;
std::uint64_t g_serial = 0;

class ProbeSceneIndex;

// Held while a scene index is used through it, so it cannot be destroyed meanwhile.
std::mutex g_indicesMutex;
std::map<std::string, ProbeSceneIndex*> g_indices;

class ProbeSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase {
public:
    static TfRefPtr<ProbeSceneIndex> New(const std::string& renderInstanceId,
                                         const HdSceneIndexBaseRefPtr& input)
    {
        return TfCreateRefPtr(new ProbeSceneIndex(renderInstanceId, input));
    }

    ~ProbeSceneIndex() override
    {
        std::lock_guard<std::mutex> lock(g_indicesMutex);
        const std::map<std::string, ProbeSceneIndex*>::iterator found = g_indices.find(_instanceId);
        if ((found != g_indices.end()) && (found->second == this)) {
            g_indices.erase(found);
        }
    }

    HdSceneIndexPrim GetPrim(const SdfPath& primPath) const override
    {
        if (!_faces.empty() && (primPath.GetNameToken() == facesChildToken())) {
            const auto found = _faces.find(primPath.GetParentPath());
            if (found != _faces.end()) {
                return facesSubset(found->second);
            }
        }
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
        if (!prim.dataSource) {
            return prim;
        }
        if (!_invisible.empty() && HdPrimTypeIsGprim(prim.primType) && (_invisible.count(primPath) != 0)) {
            prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(invisibleData(), prim.dataSource);
        } else if (!_kept.empty() && (prim.primType == HdPrimTypeTokens->instancer)) {
            const auto found = _kept.find(primPath);
            if (found != _kept.end()) {
                prim.dataSource = withKept(prim.dataSource, found->second);
            }
        }

        return prim;
    }

    SdfPathVector GetChildPrimPaths(const SdfPath& primPath) const override
    {
        SdfPathVector children = _GetInputSceneIndex()->GetChildPrimPaths(primPath);
        if (!_faces.empty() && (_faces.count(primPath) != 0)) {
            children.push_back(primPath.AppendChild(facesChildToken()));
        }

        return children;
    }

    Result run(const Request& request)
    {
        Result result;
        result.kind = request.kind;
        result.share = request.share;
        result.prepareOnly = request.prepareOnly;
        result.found = true;
        if ((request.kind == SceneBudgetProbe::eKindNone) || (request.kind == SceneBudgetProbe::eKindClear)) {
            if (request.kind == SceneBudgetProbe::eKindClear) {
                Changes changes;
                clearInto(&changes);
                send(changes, &result);
            }

            return result;
        }
        if ((_prepared.kind != request.kind) || (_prepared.share != request.share) || _prepared.stale) {
            const Clock::time_point start = Clock::now();
            prepare(request.kind, request.share);
            result.prepareMs = msSince(start);
        }
        result.candidates = _prepared.candidates;
        result.selected = _prepared.selected;
        result.faces = _prepared.faceCount;
        result.instancedMeshes = _prepared.instancedMeshes;
        if (request.prepareOnly) {
            return result;
        }
        Changes changes;
        clearInto(&changes);
        for (const SdfPath& path : _prepared.gprims) {
            _invisible.insert(path);
            changes.dirtied.emplace_back(path, visibilityLocators());
        }
        for (const std::pair<SdfPath, VtArray<bool>>& entry : _prepared.kept) {
            _kept[entry.first] = entry.second;
            changes.dirtied.emplace_back(entry.first, maskLocators());
        }
        for (const std::pair<SdfPath, VtIntArray>& entry : _prepared.faces) {
            _faces[entry.first] = entry.second;
            changes.added.emplace_back(entry.first.AppendChild(facesChildToken()), HdPrimTypeTokens->geomSubset);
            changes.dirtied.emplace_back(entry.first, topologyLocators());
        }
        send(changes, &result);

        return result;
    }

protected:
    void _PrimsAdded(const HdSceneIndexBase& /*sender*/,
                     const HdSceneIndexObserver::AddedPrimEntries& entries) override
    {
        for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
            if (HdPrimTypeIsGprim(entry.primType) || (entry.primType == HdPrimTypeTokens->instancer)) {
                _prims[entry.primPath] = entry.primType;
            } else {
                _prims.erase(entry.primPath);
            }
        }
        _units.stale = true;
        _prepared.stale = true;
        _SendPrimsAdded(entries);
    }

    void _PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                       const HdSceneIndexObserver::RemovedPrimEntries& entries) override
    {
        for (const HdSceneIndexObserver::RemovedPrimEntry& entry : entries) {
            std::map<SdfPath, TfToken>::iterator it = _prims.lower_bound(entry.primPath);
            while ((it != _prims.end()) && it->first.HasPrefix(entry.primPath)) {
                it = _prims.erase(it);
            }
            forgetBelow(entry.primPath);
        }
        _units.stale = true;
        _prepared.stale = true;
        _SendPrimsRemoved(entries);
    }

    void _PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                       const HdSceneIndexObserver::DirtiedPrimEntries& entries) override
    {
        _SendPrimsDirtied(entries);
    }

private:
    struct Changes {
        HdSceneIndexObserver::AddedPrimEntries added;
        HdSceneIndexObserver::RemovedPrimEntries removed;
        HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    };

    // Gprims no instancer draws, and the meshes among them and among those an instancer draws.
    struct Units {
        std::vector<SdfPath> gprims;
        std::vector<SdfPath> meshes;
        std::vector<SdfPath> instancedMeshes;
        bool stale = true;
    };

    struct Prepared {
        KindEnum kind = SceneBudgetProbe::eKindNone;
        double share = -1.;
        bool stale = true;
        std::vector<SdfPath> gprims;
        std::vector<std::pair<SdfPath, VtArray<bool>>> kept;
        std::vector<std::pair<SdfPath, VtIntArray>> faces;
        std::size_t candidates = 0;
        std::size_t selected = 0;
        std::size_t faceCount = 0;
        std::size_t instancedMeshes = 0;
    };

    ProbeSceneIndex(const std::string& renderInstanceId,
                    const HdSceneIndexBaseRefPtr& input)
        : HdSingleInputFilteringSceneIndexBase(input)
        , _instanceId(renderInstanceId)
    {
        std::lock_guard<std::mutex> lock(g_indicesMutex);
        g_indices[_instanceId] = this;
    }

    static HdSceneIndexPrim facesSubset(const VtIntArray& faces)
    {
        HdSceneIndexPrim prim;
        prim.primType = HdPrimTypeTokens->geomSubset;
        prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(
            invisibleData(),
            HdRetainedContainerDataSource::New(
                HdGeomSubsetSchema::GetSchemaToken(),
                HdGeomSubsetSchema::Builder()
                    .SetType(HdGeomSubsetSchema::BuildTypeDataSource(HdGeomSubsetSchemaTokens->typeFaceSet))
                    .SetIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(faces))
                    .Build()));

        return prim;
    }

    // Over the mask the input authors, as SceneDrawFilter's own mask is.
    static HdContainerDataSourceHandle withKept(const HdContainerDataSourceHandle& data,
                                                const VtArray<bool>& kept)
    {
        VtArray<bool> mask;
        if (const HdBoolArrayDataSourceHandle authored = HdInstancerTopologySchema::GetFromParent(data).GetMask()) {
            mask = authored->GetTypedValue(0.f);
        }
        if (mask.size() < kept.size()) {
            mask.resize(kept.size(), true);
        }
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (!kept[i]) {
                mask[i] = false;
            }
        }
        const HdContainerDataSourceHandle overlay = HdRetainedContainerDataSource::New(
            HdInstancerTopologySchema::GetSchemaToken(),
            HdInstancerTopologySchema::Builder().SetMask(HdRetainedTypedSampledDataSource<VtArray<bool>>::New(mask)).Build());

        return HdOverlayContainerDataSource::OverlayedContainerDataSources(overlay, data);
    }

    void refreshUnits()
    {
        if (!_units.stale) {
            return;
        }
        _units = Units();
        const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
        for (const std::pair<const SdfPath, TfToken>& entry : _prims) {
            if (entry.second == HdPrimTypeTokens->instancer) {
                continue;
            }
            const bool instanced = isInstanced(input->GetPrim(entry.first).dataSource);
            const bool mesh = (entry.second == HdPrimTypeTokens->mesh);
            if (!instanced) {
                _units.gprims.push_back(entry.first);
            }
            if (mesh) {
                (instanced ? _units.instancedMeshes : _units.meshes).push_back(entry.first);
            }
        }
        _units.stale = false;
    }

    void prepare(KindEnum kind,
                 double share)
    {
        _prepared = Prepared();
        _prepared.kind = kind;
        _prepared.share = share;
        _prepared.stale = false;
        switch (kind) {
        case SceneBudgetProbe::eKindGprims:
            prepareGprims(share);
            break;
        case SceneBudgetProbe::eKindInstances:
            prepareInstances(share);
            break;
        case SceneBudgetProbe::eKindFaces:
            prepareFaces(share);
            break;
        case SceneBudgetProbe::eKindNone:
        case SceneBudgetProbe::eKindClear:
            break;
        }
    }

    void prepareGprims(double share)
    {
        refreshUnits();
        _prepared.candidates = _units.gprims.size();
        for (const SdfPath& path : _units.gprims) {
            if (unitOf(pathKey(path)) < share) {
                _prepared.gprims.push_back(path);
            }
        }
        _prepared.selected = _prepared.gprims.size();
    }

    void prepareInstances(double share)
    {
        const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
        for (const std::pair<const SdfPath, TfToken>& entry : _prims) {
            if (entry.second != HdPrimTypeTokens->instancer) {
                continue;
            }
            const HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(input->GetPrim(entry.first).dataSource);
            std::size_t count = 0;
            const HdIntArrayVectorSchema indices = topology.GetInstanceIndices();
            for (std::size_t p = 0; p < indices.GetNumElements(); ++p) {
                if (const HdIntArrayDataSourceHandle prototype = indices.GetElement(p)) {
                    for (const int index : prototype->GetTypedValue(0.f)) {
                        count = (std::max)(count, std::size_t((std::max)(index, -1) + 1));
                    }
                }
            }
            if (const HdBoolArrayDataSourceHandle authored = topology.GetMask()) {
                count = (std::max)(count, authored->GetTypedValue(0.f).size());
            }
            if (count == 0) {
                continue;
            }
            const std::uint64_t key = pathKey(entry.first);
            VtArray<bool> kept(count, true);
            std::size_t masked = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (elementUnit(key, i) < share) {
                    kept[i] = false;
                    ++masked;
                }
            }
            _prepared.candidates += count;
            _prepared.selected += masked;
            if (masked != 0) {
                _prepared.kept.emplace_back(entry.first, std::move(kept));
            }
        }
    }

    void prepareFaces(double share)
    {
        refreshUnits();
        std::vector<std::pair<std::uint64_t, const SdfPath*>> ranked;
        ranked.reserve(_units.meshes.size());
        for (const SdfPath& path : _units.meshes) {
            ranked.emplace_back(pathKey(path), &path);
        }
        const std::size_t own = ranked.size();
        // Meshes an instancer draws only make up the count where the scene has too few others.
        if (own < SceneBudgetProbe::kFaceMeshes) {
            for (const SdfPath& path : _units.instancedMeshes) {
                ranked.emplace_back(pathKey(path), &path);
            }
        }
        _prepared.candidates = ranked.size();
        const std::size_t chosen = (std::min)(ranked.size(), SceneBudgetProbe::kFaceMeshes);
        if (own >= SceneBudgetProbe::kFaceMeshes) {
            std::partial_sort(ranked.begin(), ranked.begin() + chosen, ranked.end());
        } else {
            std::sort(ranked.begin() + own, ranked.end());
        }
        const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
        for (std::size_t r = 0; r < chosen; ++r) {
            const SdfPath& path = *ranked[r].second;
            const HdIntArrayDataSourceHandle counts = HdMeshSchema::GetFromParent(input->GetPrim(path).dataSource).GetTopology().GetFaceVertexCounts();
            const std::size_t faceCount = counts ? counts->GetTypedValue(0.f).size() : 0;
            VtIntArray faces;
            for (std::size_t f = 0; f < faceCount; ++f) {
                if (elementUnit(ranked[r].first, f) < share) {
                    faces.push_back(int(f));
                }
            }
            if (faces.empty()) {
                continue;
            }
            _prepared.faceCount += faces.size();
            if (r >= own) {
                ++_prepared.instancedMeshes;
            }
            _prepared.faces.emplace_back(path, std::move(faces));
        }
        _prepared.selected = _prepared.faces.size();
    }

    void clearInto(Changes* changes)
    {
        for (const SdfPath& path : _invisible) {
            changes->dirtied.emplace_back(path, visibilityLocators());
        }
        for (const auto& entry : _kept) {
            changes->dirtied.emplace_back(entry.first, maskLocators());
        }
        for (const auto& entry : _faces) {
            changes->removed.emplace_back(entry.first.AppendChild(facesChildToken()));
            changes->dirtied.emplace_back(entry.first, topologyLocators());
        }
        _invisible.clear();
        _kept.clear();
        _faces.clear();
    }

    void forgetBelow(const SdfPath& root)
    {
        for (auto it = _invisible.begin(); it != _invisible.end();) {
            it = it->HasPrefix(root) ? _invisible.erase(it) : std::next(it);
        }
        for (auto it = _kept.begin(); it != _kept.end();) {
            it = it->first.HasPrefix(root) ? _kept.erase(it) : std::next(it);
        }
        for (auto it = _faces.begin(); it != _faces.end();) {
            it = it->first.HasPrefix(root) ? _faces.erase(it) : std::next(it);
        }
    }

    void send(const Changes& changes,
              Result* result)
    {
        const Clock::time_point start = Clock::now();
        // A topology dirtied before its subsets are in place would be read without them.
        if (!changes.removed.empty()) {
            _SendPrimsRemoved(changes.removed);
        }
        if (!changes.added.empty()) {
            _SendPrimsAdded(changes.added);
        }
        if (!changes.dirtied.empty()) {
            _SendPrimsDirtied(changes.dirtied);
        }
        result->sendMs = msSince(start);
        result->notices = changes.added.size() + changes.removed.size() + changes.dirtied.size();
    }

    const std::string _instanceId;
    std::map<SdfPath, TfToken> _prims;
    Units _units;
    Prepared _prepared;
    std::unordered_set<SdfPath, SdfPath::Hash> _invisible;
    std::unordered_map<SdfPath, VtArray<bool>, SdfPath::Hash> _kept;
    std::unordered_map<SdfPath, VtIntArray, SdfPath::Hash> _faces;
};
} // anonymous namespace

namespace SceneBudgetProbe {
bool
isEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("NATRON_BUDGET_PROBE");

        return value && (std::strcmp(value, "1") == 0);
    }();

    return enabled;
}

void
registerForStorm()
{
    if (!isEnabled()) {
        return;
    }
    static std::once_flag once;
    std::call_once(once, []() {
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            kStormDisplayName,
            [](const std::string& renderInstanceId, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
                return ProbeSceneIndex::New(renderInstanceId, input);
            },
            HdContainerDataSourceHandle(),
            kProbePhase,
            HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
    });
}

bool
kindFromName(const std::string& name,
             KindEnum* kind)
{
    for (const KindEnum candidate : { eKindNone, eKindGprims, eKindInstances, eKindFaces, eKindClear }) {
        if (name == kindName(candidate)) {
            *kind = candidate;

            return true;
        }
    }

    return false;
}

const char*
kindName(KindEnum kind)
{
    switch (kind) {
    case eKindNone:
        return "none";
    case eKindGprims:
        return "gprims";
    case eKindInstances:
        return "instances";
    case eKindFaces:
        return "faces";
    case eKindClear:
        return "clear";
    }

    return "none";
}

void
request(KindEnum kind,
        double share,
        bool prepareOnly)
{
    std::lock_guard<std::mutex> lock(g_requestMutex);
    g_request.kind = kind;
    g_request.share = (std::min)((std::max)(share, 0.), 1.);
    g_request.prepareOnly = prepareOnly;
    g_pending = true;
}

void
service(const std::string& renderInstanceId)
{
    Request taken;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        if (!g_pending) {
            return;
        }
        taken = g_request;
        g_pending = false;
    }
    Result result;
    {
        std::lock_guard<std::mutex> lock(g_indicesMutex);
        const std::map<std::string, ProbeSceneIndex*>::iterator found = g_indices.find(renderInstanceId);
        if (found != g_indices.end()) {
            result = found->second->run(taken);
        } else {
            result.kind = taken.kind;
            result.share = taken.share;
            result.prepareOnly = taken.prepareOnly;
        }
    }
    std::lock_guard<std::mutex> lock(g_requestMutex);
    result.serial = ++g_serial;
    g_last = result;
}

Result
lastResult()
{
    std::lock_guard<std::mutex> lock(g_requestMutex);

    return g_last;
}
} // namespace SceneBudgetProbe

NATRON_NAMESPACE_EXIT
