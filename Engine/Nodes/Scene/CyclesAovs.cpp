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

#include "CyclesAovs.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <vector>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/typeHeaders.h>
#include <pxr/base/vt/value.h>
#include <pxr/base/vt/visitValue.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#include <tbb/task_arena.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace CyclesAovs {
bool
isCycles(const std::string& renderer)
{
    return renderer == kRenderDelegateCyclesId;
}

namespace {
    const TfTokenVector&
    tessellatedTypes()
    {
        static const TfTokenVector types = { HdPrimTypeTokens->cube, HdPrimTypeTokens->sphere, HdPrimTypeTokens->cone, HdPrimTypeTokens->cylinder, HdPrimTypeTokens->capsule, HdPrimTypeTokens->plane };

        return types;
    }

    // hdCycles builds a node of an id that names a Cycles node, bare or after "cycles_" or "cycles:",
    // and maps these of UsdPreviewSurface's set; of the rest, an id Sdr knows is another renderer's or
    // another part of that set, and one it does not cannot be told from a bare Cycles node name.
    // UsdTransform2d reaches Cycles as a mapping node, through LiveTextureSceneIndex's filter.
    bool
    takesShader(const TfToken& id)
    {
        static const std::set<TfToken> mapped = { TfToken("UsdPreviewSurface"), TfToken("UsdUVTexture"), TfToken("UsdTransform2d"), TfToken("UsdPrimvarReader_float"), TfToken("UsdPrimvarReader_float2"), TfToken("UsdPrimvarReader_float3"), TfToken("UsdPrimvarReader_float4"), TfToken("UsdPrimvarReader_int") };
        static const std::set<TfToken> known = [] {
            const SdrIdentifierVec ids = SdrRegistry::GetInstance().GetShaderNodeIdentifiers();

            return std::set<TfToken>(ids.begin(), ids.end());
        }();

        if ((id.GetString().rfind("cycles", 0) == 0) || mapped.count(id)) {
            return true;
        }

        return !known.count(id);
    }

    enum ShadingEnum {
        eShadingAsAuthored = 0,
        eShadingFlat,
        eShadingPointNormals,
        eShadingUnwelded
    };

    // A corner normal within about 1.8 degrees of its face's is taken for the face's.
    const float kHardNormalCosine = 0.9995f;

    VtValue
    sampleAtZero(const HdSampledDataSourceHandle& source)
    {
        return source ? source->GetValue(0.f) : VtValue();
    }

    // Whether every face's corner normals are the face's own, as a box's are: Cycles shading the
    // mesh flat then gives what they do. A face of no area constrains nothing.
    bool
    hardNormals(const VtVec3fArray& normals,
                const VtVec3fArray& points,
                const VtIntArray& counts,
                const VtIntArray& indices,
                bool leftHanded)
    {
        std::size_t corner = 0;
        for (const int count : counts) {
            GfVec3f face(0.f);
            for (int i = 0; i < count; ++i) {
                const GfVec3f& a = points[indices[corner + i]];
                const GfVec3f& b = points[indices[corner + (i + 1) % count]];
                face += GfVec3f((a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]), (a[0] - b[0]) * (a[1] + b[1]));
            }
            const float length = face.GetLength();
            if (length > 0.f) {
                face /= leftHanded ? -length : length;
                for (int i = 0; i < count; ++i) {
                    const GfVec3f& normal = normals[corner + i];
                    const float normalLength = normal.GetLength();
                    if (!(normalLength > 0.f) || !(GfDot(normal, face) >= kHardNormalCosine * normalLength)) {
                        return false;
                    }
                }
            }
            corner += (std::size_t)count;
        }

        return true;
    }

    // Whether the corners of each point all have the same normal, as a sphere's do: they can then
    // be vertex normals.
    bool
    pointNormals(const VtVec3fArray& normals,
                 const VtIntArray& indices,
                 std::size_t pointCount)
    {
        std::vector<int> firstCorner(pointCount, -1);
        for (std::size_t corner = 0; corner < indices.size(); ++corner) {
            int& first = firstCorner[(std::size_t)indices[corner]];
            if (first < 0) {
                first = (int)corner;
                continue;
            }
            const GfVec3f& a = normals[(std::size_t)first];
            const GfVec3f& b = normals[corner];
            if ((a != b) && !(GfDot(a, b) >= kHardNormalCosine * a.GetLength() * b.GetLength())) {
                return false;
            }
        }

        return true;
    }

    // Natron's cube and card author faceVarying normals that are their faces' own, its sphere ones
    // that agree at each point; its cylinder's smooth sides and hard caps need the split. The implicit Cube and Plane tessellate to meshes
    // without normals, and an 8-point cube shaded smooth has normals tilted across each face like a
    // sphere's.
    ShadingEnum
    shading(const HdContainerDataSourceHandle& prim)
    {
        const HdMeshSchema mesh = HdMeshSchema::GetFromParent(prim);
        TfToken scheme = UsdGeomTokens->catmullClark;
        if (HdTokenDataSourceHandle ds = mesh.GetSubdivisionScheme()) {
            scheme = ds->GetTypedValue(0.f);
        }
        if ((scheme != UsdGeomTokens->none) && (scheme != UsdGeomTokens->bilinear)) {
            return eShadingAsAuthored;
        }
        const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim);
        const HdPrimvarSchema normalsPrimvar = primvars.GetPrimvar(HdTokens->normals);
        const VtValue normalsValue = sampleAtZero(normalsPrimvar.GetPrimvarValue());
        if (!normalsValue.IsHolding<VtVec3fArray>() || normalsValue.UncheckedGet<VtVec3fArray>().empty()) {
            return eShadingFlat;
        }
        const HdTokenDataSourceHandle interpolationDs = normalsPrimvar.GetInterpolation();
        const TfToken interpolation = interpolationDs ? interpolationDs->GetTypedValue(0.f) : TfToken();
        if (interpolation == HdPrimvarSchemaTokens->uniform) {
            return eShadingFlat;
        }
        if (interpolation != HdPrimvarSchemaTokens->faceVarying) {
            return eShadingAsAuthored;
        }
        // The delegate reads no normals of a mesh it subdivides.
        if (scheme == UsdGeomTokens->bilinear) {
            const HdIntDataSourceHandle refine = HdLegacyDisplayStyleSchema::GetFromParent(prim).GetRefineLevel();
            if (refine && (refine->GetTypedValue(0.f) > 0)) {
                return eShadingAsAuthored;
            }
        }
        const HdMeshTopologySchema topology = mesh.GetTopology();
        const HdIntArrayDataSourceHandle countsDs = topology.GetFaceVertexCounts();
        const HdIntArrayDataSourceHandle indicesDs = topology.GetFaceVertexIndices();
        const VtValue pointsValue = sampleAtZero(primvars.GetPrimvar(HdTokens->points).GetPrimvarValue());
        if (!countsDs || !indicesDs || !pointsValue.IsHolding<VtVec3fArray>()) {
            return eShadingAsAuthored;
        }
        const VtIntArray counts = countsDs->GetTypedValue(0.f);
        const VtIntArray indices = indicesDs->GetTypedValue(0.f);
        const VtVec3fArray& points = pointsValue.UncheckedGet<VtVec3fArray>();
        const VtVec3fArray& normals = normalsValue.UncheckedGet<VtVec3fArray>();
        std::size_t corners = 0;
        for (const int count : counts) {
            if (count < 0) {
                return eShadingAsAuthored;
            }
            corners += (std::size_t)count;
        }
        if ((corners != indices.size()) || (corners != normals.size())) {
            return eShadingAsAuthored;
        }
        for (const int index : indices) {
            if ((index < 0) || ((std::size_t)index >= points.size())) {
                return eShadingAsAuthored;
            }
        }
        const HdTokenDataSourceHandle orientation = topology.GetOrientation();
        const bool leftHanded = orientation && (orientation->GetTypedValue(0.f) == HdTokens->leftHanded);

        if (hardNormals(normals, points, counts, indices, leftHanded)) {
            return eShadingFlat;
        }

        return pointNormals(normals, indices, points.size()) ? eShadingPointNormals : eShadingUnwelded;
    } // shading

    // A per-point array spread to one entry per face corner, elementSize values each.
    struct ExpandToCorners {
        template <class T>
        VtValue operator()(const VtArray<T>& values,
                           const VtIntArray& indices,
                           std::size_t elementSize) const
        {
            VtArray<T> expanded(indices.size() * elementSize);
            T* out = expanded.data();
            for (const int index : indices) {
                if ((index < 0) || ((std::size_t)index * elementSize + elementSize > values.size())) {
                    return VtValue();
                }
                out = std::copy_n(values.cdata() + (std::size_t)index * elementSize, elementSize, out);
            }

            return VtValue(expanded);
        }

        template <class T>
        VtValue operator()(const T& /*value*/,
                           const VtIntArray& /*indices*/,
                           std::size_t /*elementSize*/) const
        {
            return VtValue();
        }
    };

    class CornerValueDataSource
        : public HdSampledDataSource {
    public:
        HD_DECLARE_DATASOURCE(CornerValueDataSource);

        VtValue GetValue(Time shutterOffset) override
        {
            return VtVisitValue(_values->GetValue(shutterOffset), ExpandToCorners(), _indices, _elementSize);
        }

        bool GetContributingSampleTimesForInterval(Time startTime,
                                                   Time endTime,
                                                   std::vector<Time>* outSampleTimes) override
        {
            return _values->GetContributingSampleTimesForInterval(startTime, endTime, outSampleTimes);
        }

    private:
        CornerValueDataSource(const HdSampledDataSourceHandle& values,
                              const VtIntArray& indices,
                              std::size_t elementSize)
            : _values(values)
            , _indices(indices)
            , _elementSize(elementSize)
        {
        }

        HdSampledDataSourceHandle _values;
        VtIntArray _indices;
        std::size_t _elementSize;
    };

    // faceVarying normals that are the same at every corner of a point, gathered per point. Hydra
    // counts a mesh's points from its face vertices, and an unused point gets +Z.
    class PointNormalsDataSource
        : public HdSampledDataSource {
    public:
        HD_DECLARE_DATASOURCE(PointNormalsDataSource);

        VtValue GetValue(Time shutterOffset) override
        {
            const VtValue value = _normals->GetValue(shutterOffset);
            if (!value.IsHolding<VtVec3fArray>() || (value.UncheckedGet<VtVec3fArray>().size() != _indices.size())) {
                return VtValue();
            }
            const VtVec3fArray& normals = value.UncheckedGet<VtVec3fArray>();
            VtVec3fArray gathered(_pointCount, GfVec3f(0.f, 0.f, 1.f));
            for (std::size_t corner = 0; corner < _indices.size(); ++corner) {
                gathered[(std::size_t)_indices[corner]] = normals[corner];
            }

            return VtValue(gathered);
        }

        bool GetContributingSampleTimesForInterval(Time startTime,
                                                   Time endTime,
                                                   std::vector<Time>* outSampleTimes) override
        {
            return _normals->GetContributingSampleTimesForInterval(startTime, endTime, outSampleTimes);
        }

    private:
        PointNormalsDataSource(const HdSampledDataSourceHandle& normals,
                               const VtIntArray& indices)
            : _normals(normals)
            , _indices(indices)
            , _pointCount(indices.empty() ? 0 : (std::size_t)*std::max_element(indices.cbegin(), indices.cend()) + 1)
        {
        }

        HdSampledDataSourceHandle _normals;
        VtIntArray _indices;
        std::size_t _pointCount;
    };

    // A mesh's primvars once its points are split per face corner: vertex and varying ones spread
    // to the corners, faceVarying normals made vertex normals. A rebuilt primvar has no indices,
    // which an overlay would let through from the original.
    class CornerPrimvarsDataSource
        : public HdContainerDataSource {
    public:
        HD_DECLARE_DATASOURCE(CornerPrimvarsDataSource);

        TfTokenVector GetNames() override
        {
            return _input->GetNames();
        }

        HdDataSourceBaseHandle Get(const TfToken& name) override
        {
            const HdDataSourceBaseHandle child = _input->Get(name);
            const HdPrimvarSchema primvar(HdContainerDataSource::Cast(child));
            const HdTokenDataSourceHandle interpolation = primvar.GetInterpolation();
            const HdSampledDataSourceHandle value = primvar.GetPrimvarValue();
            if (!interpolation || !value) {
                return child;
            }
            const TfToken mode = interpolation->GetTypedValue(0.f);
            HdPrimvarSchema::Builder builder;
            if ((name == HdTokens->normals) && (mode == HdPrimvarSchemaTokens->faceVarying)) {
                builder.SetPrimvarValue(value).SetInterpolation(HdRetainedTypedSampledDataSource<TfToken>::New(HdPrimvarSchemaTokens->vertex));
            } else if ((mode == HdPrimvarSchemaTokens->vertex) || (mode == HdPrimvarSchemaTokens->varying)) {
                const HdIntDataSourceHandle elementSize = primvar.GetElementSize();
                const int size = elementSize ? std::max(1, elementSize->GetTypedValue(0.f)) : 1;
                builder.SetPrimvarValue(CornerValueDataSource::New(value, _indices, (std::size_t)size)).SetInterpolation(interpolation);
            } else {
                return child;
            }

            return builder.SetRole(primvar.GetRole()).SetColorSpace(primvar.GetColorSpace()).SetElementSize(primvar.GetElementSize()).Build();
        }

    private:
        CornerPrimvarsDataSource(const HdContainerDataSourceHandle& input,
                                 const VtIntArray& indices)
            : _input(input)
            , _indices(indices)
        {
        }

        HdContainerDataSourceHandle _input;
        VtIntArray _indices;
    };

    class ReplacedChildDataSource
        : public HdContainerDataSource {
    public:
        HD_DECLARE_DATASOURCE(ReplacedChildDataSource);

        TfTokenVector GetNames() override
        {
            return _input->GetNames();
        }

        HdDataSourceBaseHandle Get(const TfToken& name) override
        {
            return (name == _name) ? _child : _input->Get(name);
        }

    private:
        ReplacedChildDataSource(const HdContainerDataSourceHandle& input,
                                const TfToken& name,
                                const HdDataSourceBaseHandle& child)
            : _input(input)
            , _name(name)
            , _child(child)
        {
        }

        HdContainerDataSourceHandle _input;
        TfToken _name;
        HdDataSourceBaseHandle _child;
    };

    HdContainerDataSourceHandle
    withPointNormals(const HdContainerDataSourceHandle& prim)
    {
        const HdIntArrayDataSourceHandle indicesDs = HdMeshSchema::GetFromParent(prim).GetTopology().GetFaceVertexIndices();
        const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim);
        const HdPrimvarSchema normals = primvars.GetPrimvar(HdTokens->normals);
        if (!indicesDs || !primvars.GetContainer() || !normals.GetPrimvarValue()) {
            return prim;
        }
        const HdContainerDataSourceHandle vertexNormals = HdPrimvarSchema::Builder()
                                                              .SetPrimvarValue(PointNormalsDataSource::New(normals.GetPrimvarValue(), indicesDs->GetTypedValue(0.f)))
                                                              .SetInterpolation(HdRetainedTypedSampledDataSource<TfToken>::New(HdPrimvarSchemaTokens->vertex))
                                                              .SetRole(normals.GetRole())
                                                              .Build();

        return ReplacedChildDataSource::New(prim, HdPrimvarsSchema::GetSchemaToken(), ReplacedChildDataSource::New(primvars.GetContainer(), HdTokens->normals, vertexNormals));
    }

    // The mesh with a point per face corner: face vertex i is point i.
    HdContainerDataSourceHandle
    unwelded(const HdContainerDataSourceHandle& prim)
    {
        const HdIntArrayDataSourceHandle indicesDs = HdMeshSchema::GetFromParent(prim).GetTopology().GetFaceVertexIndices();
        const VtIntArray indices = indicesDs ? indicesDs->GetTypedValue(0.f) : VtIntArray();
        VtIntArray corners(indices.size());
        std::iota(corners.begin(), corners.end(), 0);
        const HdContainerDataSourceHandle topology = HdRetainedContainerDataSource::New(
            HdMeshSchema::GetSchemaToken(),
            HdRetainedContainerDataSource::New(
                HdMeshSchemaTokens->topology,
                HdRetainedContainerDataSource::New(HdMeshTopologySchemaTokens->faceVertexIndices, HdRetainedTypedSampledDataSource<VtIntArray>::New(corners))));
        const HdContainerDataSourceHandle primvars = HdPrimvarsSchema::GetFromParent(prim).GetContainer();
        if (!primvars) {
            return prim;
        }

        return HdOverlayContainerDataSource::New(topology, ReplacedChildDataSource::New(prim, HdPrimvarsSchema::GetSchemaToken(), CornerPrimvarsDataSource::New(primvars, indices)));
    }

    class CyclesMeshSceneIndex;
    typedef TfRefPtr<CyclesMeshSceneIndex> CyclesMeshSceneIndexRefPtr;

    // Hands every mesh to Cycles as shading() says it has to be.
    class CyclesMeshSceneIndex
        : public HdSingleInputFilteringSceneIndexBase {
    public:
        static CyclesMeshSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr& input)
        {
            return TfCreateRefPtr(new CyclesMeshSceneIndex(input));
        }

        HdSceneIndexPrim GetPrim(const SdfPath& path) const override
        {
            HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
            if ((prim.primType != HdPrimTypeTokens->mesh) || !prim.dataSource) {
                return prim;
            }
            switch (cachedShading(path, prim.dataSource)) {
            case eShadingFlat: {
                static const HdContainerDataSourceHandle flat = HdRetainedContainerDataSource::New(
                    HdLegacyDisplayStyleSchema::GetSchemaToken(),
                    HdLegacyDisplayStyleSchema::Builder().SetFlatShadingEnabled(HdRetainedTypedSampledDataSource<bool>::New(true)).Build());
                prim.dataSource = HdOverlayContainerDataSource::New(flat, prim.dataSource);
                break;
            }
            case eShadingPointNormals:
                prim.dataSource = withPointNormals(prim.dataSource);
                break;
            case eShadingUnwelded:
                prim.dataSource = unwelded(prim.dataSource);
                break;
            case eShadingAsAuthored:
                break;
            }

            return prim;
        }

        SdfPathVector GetChildPrimPaths(const SdfPath& path) const override
        {
            return _GetInputSceneIndex()->GetChildPrimPaths(path);
        }

    protected:
        void _PrimsAdded(const HdSceneIndexBase& /*sender*/,
                         const HdSceneIndexObserver::AddedPrimEntries& entries) override
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
                    _shading.erase(entry.primPath);
                }
            }
            _SendPrimsAdded(entries);
        }

        void _PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                           const HdSceneIndexObserver::RemovedPrimEntries& entries) override
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (const HdSceneIndexObserver::RemovedPrimEntry& entry : entries) {
                    for (std::map<SdfPath, ShadingEnum>::iterator it = _shading.begin(); it != _shading.end();) {
                        it = it->first.HasPrefix(entry.primPath) ? _shading.erase(it) : std::next(it);
                    }
                }
            }
            _SendPrimsRemoved(entries);
        }

        // A mesh whose shading changes, or whose face vertices change while its primvars are
        // rebuilt from them, has every primvar change.
        void _PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                           const HdSceneIndexObserver::DirtiedPrimEntries& entries) override
        {
            static const HdDataSourceLocatorSet decides = { HdMeshSchema::GetDefaultLocator(), HdPrimvarsSchema::GetNormalsLocator(), HdPrimvarsSchema::GetPointsLocator(), HdLegacyDisplayStyleSchema::GetRefineLevelLocator() };
            static const HdDataSourceLocatorSet reshaped = { HdMeshSchema::GetTopologyLocator(), HdPrimvarsSchema::GetDefaultLocator(), HdLegacyDisplayStyleSchema::GetFlatShadingEnabledLocator() };
            HdSceneIndexObserver::DirtiedPrimEntries forwarded = entries;
            for (HdSceneIndexObserver::DirtiedPrimEntry& entry : forwarded) {
                if (!entry.dirtyLocators.Intersects(decides)) {
                    continue;
                }
                bool known = false;
                ShadingEnum was = eShadingAsAuthored;
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    std::map<SdfPath, ShadingEnum>::iterator found = _shading.find(entry.primPath);
                    if (found != _shading.end()) {
                        known = true;
                        was = found->second;
                        _shading.erase(found);
                    }
                }
                const HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(entry.primPath);
                if ((prim.primType != HdPrimTypeTokens->mesh) || !prim.dataSource) {
                    continue;
                }
                const ShadingEnum now = cachedShading(entry.primPath, prim.dataSource);
                if (!known || (now != was)) {
                    entry.dirtyLocators.insert(reshaped);
                } else if (((now == eShadingUnwelded) || (now == eShadingPointNormals)) && entry.dirtyLocators.Intersects(HdMeshSchema::GetTopologyLocator())) {
                    entry.dirtyLocators.insert(HdPrimvarsSchema::GetDefaultLocator());
                }
            }
            _SendPrimsDirtied(forwarded);
        }

    private:
        explicit CyclesMeshSceneIndex(const HdSceneIndexBaseRefPtr& input)
            : HdSingleInputFilteringSceneIndexBase(input)
        {
        }

        // Reading a mesh's points and normals costs as much as Cycles' own reading of them, and
        // the delegate asks for the prim many times a sync.
        ShadingEnum cachedShading(const SdfPath& path,
                                  const HdContainerDataSourceHandle& prim) const
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                std::map<SdfPath, ShadingEnum>::const_iterator found = _shading.find(path);
                if (found != _shading.end()) {
                    return found->second;
                }
            }
            const ShadingEnum computed = shading(prim);
            std::lock_guard<std::mutex> lock(_mutex);
            _shading[path] = computed;

            return computed;
        }

        mutable std::mutex _mutex;
        mutable std::map<SdfPath, ShadingEnum> _shading;
    };
} // anon namespace

void
registerSceneIndices()
{
    static std::once_flag once;
    std::call_once(once, []() {
        const TfTokenVector& types = tessellatedTypes();
        const std::size_t count = types.size();
        const HdDataSourceBaseHandle toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        const std::vector<HdDataSourceBaseHandle> modes(count, toMesh);
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            kRenderDelegateCyclesDisplayName,
            [](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& args) -> HdSceneIndexBaseRefPtr {
                return meshShadingSceneIndex(HdsiImplicitSurfaceSceneIndex::New(input, args));
            },
            HdRetainedContainerDataSource::New(count, types.data(), modes.data()),
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

HdSceneIndexBaseRefPtr
meshShadingSceneIndex(const HdSceneIndexBaseRefPtr& input)
{
    return CyclesMeshSceneIndex::New(input);
}

const DelegateSupport::Support&
support()
{
    static const DelegateSupport::Support cycles = [] {
        DelegateSupport::Support probed = DelegateSupport::probe(kRenderDelegateCyclesId);
        probed.tessellated = tessellatedTypes();
        probed.takesShader = takesShader;

        return probed;
    }();

    return cycles;
}

void
configureEngine(UsdImagingGLEngine* engine)
{
    engine->SetRendererSetting(TfToken("stageMetersPerUnit"), VtValue(1.0));
}

float
depthToDistance(float value)
{
    if (value >= 1e10f) {
        return std::numeric_limits<float>::infinity();
    }

    return value;
}

int
instanceIndex(std::int32_t value)
{
    return value + 1;
}

void
withThreadBudget(int threads,
                 const std::function<void()>& job)
{
    if (threads <= 0) {
        job();

        return;
    }
    tbb::task_arena arena(threads);
    arena.execute(job);
}
} // namespace CyclesAovs

NATRON_NAMESPACE_EXIT
