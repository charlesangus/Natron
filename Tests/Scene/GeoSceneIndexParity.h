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

#ifndef Tests_Scene_GeoSceneIndexParity_h
#define Tests_Scene_GeoSceneIndexParity_h

// Compares a scene index against the one UsdImaging builds from the same geometry, prim by prim,
// as the renderer plugins see them. Header-only so any scene test can use it.

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/extentSchema.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/path.h>

namespace GeoSceneIndexParity {

struct Options {
    // Where the compared prims sit in the scene index under test; the reference serves them at
    // the same paths under the absolute root.
    PXR_NS::SdfPath prefix = PXR_NS::SdfPath::AbsoluteRootPath();
    // Hydra prim types both sides must serve at the same paths.
    std::set<PXR_NS::TfToken> types = { PXR_NS::HdPrimTypeTokens->mesh,
                                        PXR_NS::HdPrimTypeTokens->basisCurves,
                                        PXR_NS::HdPrimTypeTokens->points,
                                        PXR_NS::HdPrimTypeTokens->geomSubset };
    double tolerance = 1e-6;
};

namespace detail {

    inline std::map<PXR_NS::SdfPath, PXR_NS::TfToken>
    allPrims(const PXR_NS::HdSceneIndexBase& index,
             const PXR_NS::SdfPath& root)
    {
        std::map<PXR_NS::SdfPath, PXR_NS::TfToken> prims;
        std::vector<PXR_NS::SdfPath> stack { root };
        while (!stack.empty()) {
            const PXR_NS::SdfPath path = stack.back();
            stack.pop_back();
            for (const PXR_NS::SdfPath& child : index.GetChildPrimPaths(path)) {
                prims[child] = index.GetPrim(child).primType;
                stack.push_back(child);
            }
        }

        return prims;
    }

    template <typename T>
    std::string
    show(const T& value)
    {
        std::ostringstream s;
        s << value;
        std::string text = s.str();
        if (text.size() > 160) {
            text = text.substr(0, 160) + "...";
        }

        return text;
    }

    template <typename Handle>
    PXR_NS::VtValue
    valueOf(const Handle& source)
    {
        return source ? source->GetValue(0.f) : PXR_NS::VtValue();
    }

    template <typename T, typename Handle>
    T
    typedOr(const Handle& source,
            const T& fallback)
    {
        return source ? source->GetTypedValue(0.f) : fallback;
    }

    class Report {
    public:
        Report(const PXR_NS::SdfPath& path,
               std::vector<std::string>* mismatches)
            : _path(path)
            , _mismatches(mismatches)
        {
        }

        template <typename T>
        void equal(const std::string& field,
                   const T& ours,
                   const T& reference)
        {
            if (!(ours == reference)) {
                add(field, show(ours), show(reference));
            }
        }

        void add(const std::string& field,
                 const std::string& ours,
                 const std::string& reference)
        {
            _mismatches->push_back(_path.GetString() + ": " + field + " differs (ours " + ours + ", UsdImaging " + reference + ")");
        }

        void missing(const std::string& field,
                     const char* side)
        {
            _mismatches->push_back(_path.GetString() + ": " + field + " missing from " + side);
        }

    private:
        PXR_NS::SdfPath _path;
        std::vector<std::string>* _mismatches;
    };

    inline void
    comparePrimvars(const PXR_NS::HdContainerDataSourceHandle& ours,
                    const PXR_NS::HdContainerDataSourceHandle& reference,
                    Report& report)
    {
        using namespace PXR_NS;
        const HdPrimvarsSchema a = HdPrimvarsSchema::GetFromParent(ours);
        const HdPrimvarsSchema b = HdPrimvarsSchema::GetFromParent(reference);
        std::set<TfToken> names;
        for (const TfToken& n : a.GetPrimvarNames()) {
            names.insert(n);
        }
        const TfTokenVector referenceNames = b.GetPrimvarNames();
        names.insert(referenceNames.begin(), referenceNames.end());
        for (const TfToken& name : names) {
            const std::string field = "primvars/" + name.GetString();
            const HdPrimvarSchema pa = a.GetPrimvar(name);
            const HdPrimvarSchema pb = b.GetPrimvar(name);
            const VtValue va = valueOf(pa.GetFlattenedPrimvarValue());
            const VtValue vb = valueOf(pb.GetFlattenedPrimvarValue());
            if (va.IsEmpty() || vb.IsEmpty()) {
                if (!va.IsEmpty() || !vb.IsEmpty()) {
                    report.missing(field, va.IsEmpty() ? "ours" : "UsdImaging");
                }
                continue;
            }
            report.equal(field + " value", va, vb);
            report.equal(field + " interpolation", typedOr(pa.GetInterpolation(), TfToken()), typedOr(pb.GetInterpolation(), TfToken()));
            report.equal(field + " role", typedOr(pa.GetRole(), TfToken()), typedOr(pb.GetRole(), TfToken()));
            report.equal(field + " elementSize", typedOr(pa.GetElementSize(), 1), typedOr(pb.GetElementSize(), 1));
        }
    }

    inline void
    compareGprim(const PXR_NS::TfToken& type,
                 const PXR_NS::HdContainerDataSourceHandle& ours,
                 const PXR_NS::HdContainerDataSourceHandle& reference,
                 const Options& options,
                 Report& report)
    {
        using namespace PXR_NS;
        if (type == HdPrimTypeTokens->geomSubset) {
            const HdGeomSubsetSchema a = HdGeomSubsetSchema::GetFromParent(ours);
            const HdGeomSubsetSchema b = HdGeomSubsetSchema::GetFromParent(reference);
            report.equal("geomSubset/type", typedOr(a.GetType(), TfToken()), typedOr(b.GetType(), TfToken()));
            report.equal("geomSubset/indices", typedOr(a.GetIndices(), VtIntArray()), typedOr(b.GetIndices(), VtIntArray()));

            return;
        }
        comparePrimvars(ours, reference, report);

        if (type == HdPrimTypeTokens->mesh) {
            const HdMeshSchema a = HdMeshSchema::GetFromParent(ours);
            const HdMeshSchema b = HdMeshSchema::GetFromParent(reference);
            const HdMeshTopologySchema ta = a.GetTopology();
            const HdMeshTopologySchema tb = b.GetTopology();
            report.equal("mesh/topology/faceVertexCounts", typedOr(ta.GetFaceVertexCounts(), VtIntArray()), typedOr(tb.GetFaceVertexCounts(), VtIntArray()));
            report.equal("mesh/topology/faceVertexIndices", typedOr(ta.GetFaceVertexIndices(), VtIntArray()), typedOr(tb.GetFaceVertexIndices(), VtIntArray()));
            report.equal("mesh/topology/holeIndices", typedOr(ta.GetHoleIndices(), VtIntArray()), typedOr(tb.GetHoleIndices(), VtIntArray()));
            report.equal("mesh/topology/orientation",
                         typedOr(ta.GetOrientation(), HdMeshTopologySchemaTokens->rightHanded),
                         typedOr(tb.GetOrientation(), HdMeshTopologySchemaTokens->rightHanded));
            report.equal("mesh/subdivisionScheme", typedOr(a.GetSubdivisionScheme(), TfToken()), typedOr(b.GetSubdivisionScheme(), TfToken()));
            report.equal("mesh/doubleSided", typedOr(a.GetDoubleSided(), false), typedOr(b.GetDoubleSided(), false));
        } else if (type == HdPrimTypeTokens->basisCurves) {
            const HdBasisCurvesTopologySchema ta = HdBasisCurvesSchema::GetFromParent(ours).GetTopology();
            const HdBasisCurvesTopologySchema tb = HdBasisCurvesSchema::GetFromParent(reference).GetTopology();
            report.equal("basisCurves/topology/curveVertexCounts", typedOr(ta.GetCurveVertexCounts(), VtIntArray()), typedOr(tb.GetCurveVertexCounts(), VtIntArray()));
            report.equal("basisCurves/topology/curveIndices", typedOr(ta.GetCurveIndices(), VtIntArray()), typedOr(tb.GetCurveIndices(), VtIntArray()));
            const TfToken curveType = typedOr(tb.GetType(), TfToken());
            report.equal("basisCurves/topology/type", typedOr(ta.GetType(), TfToken()), curveType);
            report.equal("basisCurves/topology/wrap", typedOr(ta.GetWrap(), TfToken()), typedOr(tb.GetWrap(), TfToken()));
            // Linear curves ignore the basis.
            if (curveType == HdTokens->cubic) {
                report.equal("basisCurves/topology/basis", typedOr(ta.GetBasis(), TfToken()), typedOr(tb.GetBasis(), TfToken()));
            }
        }

        const GfMatrix4d wa = typedOr(HdXformSchema::GetFromParent(ours).GetMatrix(), GfMatrix4d(1.0));
        const GfMatrix4d wb = typedOr(HdXformSchema::GetFromParent(reference).GetMatrix(), GfMatrix4d(1.0));
        if (!GfIsClose(wa, wb, options.tolerance)) {
            report.add("xform/matrix", show(wa), show(wb));
        }

        const HdExtentSchema ea = HdExtentSchema::GetFromParent(ours);
        const HdExtentSchema eb = HdExtentSchema::GetFromParent(reference);
        if (bool(ea) != bool(eb)) {
            report.missing("extent", ea ? "UsdImaging" : "ours");
        } else if (ea) {
            const GfVec3d mina = typedOr(ea.GetMin(), GfVec3d(0.)), minb = typedOr(eb.GetMin(), GfVec3d(0.));
            const GfVec3d maxa = typedOr(ea.GetMax(), GfVec3d(0.)), maxb = typedOr(eb.GetMax(), GfVec3d(0.));
            if (!GfIsClose(mina, minb, options.tolerance) || !GfIsClose(maxa, maxb, options.tolerance)) {
                report.add("extent", show(mina) + " " + show(maxa), show(minb) + " " + show(maxb));
            }
        }
    }

} // namespace detail

/**
 * @brief Every difference between @p ours and @p reference (UsdImaging's scene index over the
 * same geometry), one message per field, each naming the reference prim path and the field:
 * prims of options.types present on one side only or with another type, then per prim the
 * primvars (value, interpolation, role, elementSize), topology, world xform, extent and geomSubset
 * type and indices. Empty when they match.
 **/
inline std::vector<std::string>
compare(const PXR_NS::HdSceneIndexBase& ours,
        const PXR_NS::HdSceneIndexBase& reference,
        const Options& options = Options())
{
    using namespace PXR_NS;
    std::vector<std::string> mismatches;
    const SdfPath root = SdfPath::AbsoluteRootPath();
    const std::map<SdfPath, TfToken> referencePrims = detail::allPrims(reference, root);
    std::map<SdfPath, TfToken> ourPrims;
    for (const auto& entry : detail::allPrims(ours, options.prefix)) {
        ourPrims[entry.first.ReplacePrefix(options.prefix, root)] = entry.second;
    }

    for (const auto& entry : ourPrims) {
        detail::Report report(entry.first, &mismatches);
        const auto it = referencePrims.find(entry.first);
        if (it == referencePrims.end()) {
            report.missing("prim", "UsdImaging");
        } else if (!entry.second.IsEmpty() || options.types.count(it->second)) {
            report.equal("type", entry.second, it->second);
        }
    }
    for (const auto& entry : referencePrims) {
        if (options.types.count(entry.second) && !ourPrims.count(entry.first)) {
            detail::Report(entry.first, &mismatches).missing("prim", "ours");
        }
    }

    for (const auto& entry : ourPrims) {
        const auto it = referencePrims.find(entry.first);
        if (it == referencePrims.end() || it->second != entry.second || !options.types.count(entry.second)) {
            continue;
        }
        detail::Report report(entry.first, &mismatches);
        const HdSceneIndexPrim a = ours.GetPrim(entry.first.ReplacePrefix(root, options.prefix));
        const HdSceneIndexPrim b = reference.GetPrim(entry.first);
        if (!a.dataSource || !b.dataSource) {
            report.missing("data source", a.dataSource ? "UsdImaging" : "ours");
            continue;
        }
        detail::compareGprim(entry.second, a.dataSource, b.dataSource, options, report);
    }

    return mismatches;
}

} // namespace GeoSceneIndexParity

#endif // Tests_Scene_GeoSceneIndexParity_h
