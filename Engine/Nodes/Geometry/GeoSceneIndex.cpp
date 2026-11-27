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

#include "GeoSceneIndex.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef emit
#define NATRON_GEOSCENEINDEX_CPP_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/extentSchema.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/purposeSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usdImaging/usdImaging/primvarUtils.h>
#ifdef NATRON_GEOSCENEINDEX_CPP_RESTORE_EMIT
#undef NATRON_GEOSCENEINDEX_CPP_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

const std::string kPrimvarsPrefix("primvars:");
const std::string kIndicesSuffix(":indices");

// The Hydra prim type of a spec's typeName; empty for the prims only served as ancestors.
TfToken
hydraType(const TfToken& usdType)
{
    static const TfToken mesh("Mesh"), curves("BasisCurves"), points("Points"), subset("GeomSubset");
    if (usdType == mesh) {
        return HdPrimTypeTokens->mesh;
    }
    if (usdType == curves) {
        return HdPrimTypeTokens->basisCurves;
    }
    if (usdType == points) {
        return HdPrimTypeTokens->points;
    }
    if (usdType == subset) {
        return HdPrimTypeTokens->geomSubset;
    }

    return TfToken();
}

bool
servedKind(GeoUsdPrimKindEnum kind)
{
    return kind == eGeoUsdPrimMesh || kind == eGeoUsdPrimCurves || kind == eGeoUsdPrimPoints;
}

template <typename T>
bool
fieldAs(const GeoUsdConvert& convert,
        const SdfPath& path,
        const TfToken& name,
        T* out)
{
    VtValue v;
    if (!convert.field(path, name, &v) || !v.IsHolding<T>()) {
        return false;
    }
    *out = v.UncheckedGet<T>();

    return true;
}

// One attribute's default, from the partition's shared values when it is built from the detail.
VtValue
attributeValue(const GeoUsdConvert& convert,
               const GeoUsdValueMap* values,
               const SdfPath& attrPath)
{
    if (values) {
        const auto it = values->find(attrPath);
        if (it != values->end()) {
            return it->second;
        }
    }
    VtValue v;
    if (convert.specType(attrPath) != SdfSpecTypeAttribute || !convert.field(attrPath, SdfFieldKeys->Default, &v) || v.IsHolding<SdfValueBlock>()) {
        return VtValue();
    }

    return v;
}

template <typename T>
bool
attributeAs(const GeoUsdConvert& convert,
            const GeoUsdValueMap* values,
            const SdfPath& attrPath,
            T* out)
{
    const VtValue v = attributeValue(convert, values, attrPath);
    if (!v.IsHolding<T>()) {
        return false;
    }
    *out = v.UncheckedGet<T>();

    return true;
}

// The flattened local transform of the prim spec at @p path, from its xformOpOrder.
GfMatrix4d
localTransform(const GeoUsdConvert& convert,
               const SdfPath& path,
               bool* resetsXformStack)
{
    static const std::string invertPrefix("!invert!");
    GfMatrix4d local(1.0);
    *resetsXformStack = false;
    VtTokenArray order;
    if (!attributeAs(convert, nullptr, path.AppendProperty(UsdGeomTokens->xformOpOrder), &order)) {
        return local;
    }
    for (const TfToken& op : order) {
        if (op == UsdGeomXformOpTypes->resetXformStack) {
            *resetsXformStack = true;
            local.SetIdentity();
            continue;
        }
        std::string name = op.GetString();
        const bool inverse = TfStringStartsWith(name, invertPrefix);
        if (inverse) {
            name.erase(0, invertPrefix.size());
        }
        // xformOp:<opType>[:<suffix>]
        const std::vector<std::string> parts = TfStringSplit(name, ":");
        if (parts.size() < 2) {
            continue;
        }
        const UsdGeomXformOp::Type type = UsdGeomXformOp::GetOpTypeEnum(TfToken(parts[1]));
        const VtValue value = attributeValue(convert, nullptr, path.AppendProperty(TfToken(name)));
        if (type == UsdGeomXformOp::TypeInvalid || value.IsEmpty()) {
            continue;
        }
        // Ops apply in order to row vectors, so each one goes on the left of those before it.
        local = UsdGeomXformOp::GetOpTransform(type, value, inverse) * local;
    }

    return local;
}

HdDataSourceLocator
primvarLocator(const TfToken& name)
{
    return HdPrimvarsSchema::GetDefaultLocator().Append(name);
}

// The Hydra primvar an attribute feeds, and the interpolation it has without the metadata.
bool
primvarOf(const TfToken& property,
          TfToken* primvar,
          TfToken* fallbackInterpolation)
{
    if (property == UsdGeomTokens->points || property == UsdGeomTokens->normals || property == UsdGeomTokens->widths) {
        *primvar = property;
        *fallbackInterpolation = UsdGeomTokens->vertex;

        return true;
    }
    const std::string& s = property.GetString();
    if (!TfStringStartsWith(s, kPrimvarsPrefix) || TfStringEndsWith(s, kIndicesSuffix)) {
        return false;
    }
    *primvar = TfToken(s.substr(kPrimvarsPrefix.size()));
    *fallbackInterpolation = UsdGeomTokens->constant;

    return true;
}

// Where a change of @p property shows in a prim of Hydra type @p type. False when nothing
// served depends on it (the flattened xform, visibility and purpose are compared on their own).
bool
locatorOf(const TfToken& type,
          const TfToken& property,
          HdDataSourceLocator* locator)
{
    const std::string& s = property.GetString();
    TfToken primvar, interpolation;
    if (primvarOf(property, &primvar, &interpolation)) {
        *locator = primvarLocator(primvar);

        return true;
    }
    if (TfStringStartsWith(s, kPrimvarsPrefix) && TfStringEndsWith(s, kIndicesSuffix)) {
        *locator = primvarLocator(TfToken(s.substr(kPrimvarsPrefix.size(), s.size() - kPrimvarsPrefix.size() - kIndicesSuffix.size())));

        return true;
    }
    if (property == UsdGeomTokens->xformOpOrder || TfStringStartsWith(s, "xformOp:") || property == UsdGeomTokens->visibility || property == UsdGeomTokens->purpose || property == UsdGeomTokens->familyName || TfStringStartsWith(s, "subsetFamily:")) {
        return false;
    }
    if (property == UsdGeomTokens->extent) {
        *locator = HdExtentSchema::GetDefaultLocator();

        return true;
    }
    if (type == HdPrimTypeTokens->mesh) {
        if (property == UsdGeomTokens->faceVertexCounts || property == UsdGeomTokens->faceVertexIndices || property == UsdGeomTokens->holeIndices || property == UsdGeomTokens->orientation) {
            *locator = HdMeshSchema::GetTopologyLocator();

            return true;
        }
        if (property == UsdGeomTokens->subdivisionScheme) {
            *locator = HdMeshSchema::GetSubdivisionSchemeLocator();

            return true;
        }
        if (property == UsdGeomTokens->doubleSided) {
            *locator = HdMeshSchema::GetDoubleSidedLocator();

            return true;
        }
    } else if (type == HdPrimTypeTokens->basisCurves) {
        if (property == UsdGeomTokens->curveVertexCounts || property == UsdGeomTokens->basis || property == UsdGeomTokens->type || property == UsdGeomTokens->wrap) {
            *locator = HdBasisCurvesSchema::GetTopologyLocator();

            return true;
        }
    } else if (type == HdPrimTypeTokens->geomSubset) {
        if (property == UsdGeomTokens->elementType || property == UsdGeomTokens->indices) {
            *locator = HdGeomSubsetSchema::GetDefaultLocator();

            return true;
        }
    }
    *locator = HdDataSourceLocator::EmptyLocator();

    return true;
}

// Every locator a prim of @p type serves from its partition.
HdDataSourceLocatorSet
partitionLocators(const TfToken& type)
{
    HdDataSourceLocatorSet set;
    if (type == HdPrimTypeTokens->geomSubset) {
        set.insert(HdGeomSubsetSchema::GetDefaultLocator());

        return set;
    }
    set.insert(HdPrimvarsSchema::GetDefaultLocator());
    set.insert(HdExtentSchema::GetDefaultLocator());
    if (type == HdPrimTypeTokens->mesh) {
        set.insert(HdMeshSchema::GetDefaultLocator());
    } else if (type == HdPrimTypeTokens->basisCurves) {
        set.insert(HdBasisCurvesSchema::GetDefaultLocator());
    }

    return set;
}

bool
sameField(const GeoUsdConvert& a,
          const SdfPath& pa,
          const GeoUsdConvert& b,
          const SdfPath& pb,
          const TfToken& name)
{
    VtValue va, vb;
    const bool ha = a.field(pa, name, &va);
    const bool hb = b.field(pb, name, &vb);

    return ha == hb && va == vb;
}

bool
sameAttribute(const GeoUsdConvert& a,
              const GeoUsdValueMap* va,
              const SdfPath& pa,
              const GeoUsdConvert& b,
              const GeoUsdValueMap* vb,
              const SdfPath& pb)
{
    if (a.specType(pa) != b.specType(pb)) {
        return false;
    }
    if (a.specType(pa) != SdfSpecTypeAttribute) {
        return true;
    }

    return sameField(a, pa, b, pb, SdfFieldKeys->TypeName) && sameField(a, pa, b, pb, UsdGeomTokens->interpolation) && sameField(a, pa, b, pb, UsdGeomTokens->elementSize) && attributeValue(a, va, pa) == attributeValue(b, vb, pb);
}

// Whether two converts of the same detail author the same specs and values.
bool
sameOptions(const GeoUsdConvertOptions& a,
            const GeoUsdConvertOptions& b)
{
    return a.rootPath == b.rootPath && a.packedMode == b.packedMode && a.defaultPointWidth == b.defaultPointWidth && a.inputScene == b.inputScene;
}

HdContainerDataSourceHandle
primOriginSource(const SdfPath& source)
{
    return HdRetainedContainerDataSource::New(
        HdPrimOriginSchemaTokens->scenePath,
        HdRetainedTypedSampledDataSource<HdPrimOriginSchema::OriginPath>::New(HdPrimOriginSchema::OriginPath(source)));
}

} // anon namespace

struct GeoSceneIndex::Prim {
    // The convert path; empty for the prefix's own ancestors.
    SdfPath source;
    TfToken type;
    int partition = -1;
    GfMatrix4d world { 1.0 };
    bool visible = true;
    TfToken purpose;
    SdfPathVector children;
    TfTokenVector properties;

    mutable std::once_flag built;
    mutable HdContainerDataSourceHandle dataSource;
};

struct GeoSceneIndex::Snapshot {
    std::shared_ptr<GeoUsdConvert> convert;
    // Ordered, so a walk visits parents before children.
    std::map<SdfPath, std::unique_ptr<Prim>> prims;

    const Prim* find(const SdfPath& path) const
    {
        const auto it = prims.find(path);

        return it == prims.end() ? nullptr : it->second.get();
    }

    HdContainerDataSourceHandle build(const Prim& prim) const;
    HdContainerDataSourceHandle buildPrimvars(const Prim& prim, const GeoUsdValueMap* values) const;
};

HdContainerDataSourceHandle
GeoSceneIndex::Snapshot::buildPrimvars(const Prim& prim,
                                       const GeoUsdValueMap* values) const
{
    const GeoUsdConvert& c = *convert;
    TfTokenVector names;
    std::vector<HdDataSourceBaseHandle> sources;
    for (const TfToken& property : prim.properties) {
        TfToken primvar, interpolation;
        if (!primvarOf(property, &primvar, &interpolation)) {
            continue;
        }
        const SdfPath attrPath = prim.source.AppendProperty(property);
        const VtValue value = attributeValue(c, values, attrPath);
        if (value.IsEmpty()) {
            continue;
        }
        fieldAs(c, attrPath, UsdGeomTokens->interpolation, &interpolation);
        TfToken typeName;
        fieldAs(c, attrPath, SdfFieldKeys->TypeName, &typeName);
        const TfToken role = UsdImagingUsdToHdRole(SdfSchema::GetInstance().FindType(typeName).GetRole());

        HdPrimvarSchema::Builder builder;
        const VtValue indices = attributeValue(c, values, prim.source.AppendProperty(TfToken(property.GetString() + kIndicesSuffix)));
        if (indices.IsHolding<VtIntArray>()) {
            builder.SetIndexedPrimvarValue(HdRetainedSampledDataSource::New(value))
                .SetIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices.UncheckedGet<VtIntArray>()));
        } else {
            builder.SetPrimvarValue(HdRetainedSampledDataSource::New(value));
        }
        builder.SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(UsdImagingUsdToHdInterpolationToken(interpolation)));
        if (!role.IsEmpty()) {
            builder.SetRole(HdPrimvarSchema::BuildRoleDataSource(role));
        }
        int elementSize = 1;
        if (fieldAs(c, attrPath, UsdGeomTokens->elementSize, &elementSize) && elementSize > 1) {
            builder.SetElementSize(HdRetainedTypedSampledDataSource<int>::New(elementSize));
        }
        names.push_back(primvar);
        sources.push_back(builder.Build());
    }

    return HdPrimvarsSchema::BuildRetained(names.size(), names.data(), sources.data());
}

HdContainerDataSourceHandle
GeoSceneIndex::Snapshot::build(const Prim& prim) const
{
    if (prim.source.IsEmpty()) {
        return HdRetainedContainerDataSource::New();
    }
    const GeoUsdConvert& c = *convert;
    TfTokenVector names;
    std::vector<HdDataSourceBaseHandle> sources;
    names.push_back(HdPrimOriginSchema::GetSchemaToken());
    sources.push_back(primOriginSource(prim.source));
    if (prim.type == HdPrimTypeTokens->geomSubset) {
        TfToken elementType = UsdGeomTokens->face;
        VtIntArray indices;
        attributeAs(c, nullptr, prim.source.AppendProperty(UsdGeomTokens->elementType), &elementType);
        attributeAs(c, nullptr, prim.source.AppendProperty(UsdGeomTokens->indices), &indices);
        const TfToken subsetType = elementType == UsdGeomTokens->point ? HdGeomSubsetSchemaTokens->typePointSet : HdGeomSubsetSchemaTokens->typeFaceSet;
        names.push_back(HdGeomSubsetSchema::GetSchemaToken());
        sources.push_back(HdGeomSubsetSchema::Builder()
                              .SetType(HdGeomSubsetSchema::BuildTypeDataSource(subsetType))
                              .SetIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices))
                              .Build());

        return HdRetainedContainerDataSource::New(names.size(), names.data(), sources.data());
    }

    names.push_back(HdXformSchema::GetSchemaToken());
    sources.push_back(HdXformSchema::Builder()
                          .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(prim.world))
                          .SetResetXformStack(HdRetainedTypedSampledDataSource<bool>::New(true))
                          .Build());
    names.push_back(HdVisibilitySchema::GetSchemaToken());
    sources.push_back(HdVisibilitySchema::Builder().SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(prim.visible)).Build());
    names.push_back(HdPurposeSchema::GetSchemaToken());
    sources.push_back(HdPurposeSchema::Builder().SetPurpose(HdRetainedTypedSampledDataSource<TfToken>::New(prim.purpose)).Build());
    if (prim.type.IsEmpty()) {
        return HdRetainedContainerDataSource::New(names.size(), names.data(), sources.data());
    }

    // The data sources share the convert's arrays by VtArray refcount, so the map can go after this.
    const std::shared_ptr<const GeoUsdValueMap> values = c.partitionValues(std::size_t(prim.partition));
    const GeoUsdValueMap* v = values.get();
    auto path = [&](const TfToken& property) {
        return prim.source.AppendProperty(property);
    };

    VtVec3fArray extent;
    if (attributeAs(c, v, path(UsdGeomTokens->extent), &extent) && extent.size() == 2) {
        names.push_back(HdExtentSchema::GetSchemaToken());
        sources.push_back(HdExtentSchema::Builder()
                              .SetMin(HdRetainedTypedSampledDataSource<GfVec3d>::New(GfVec3d(extent[0])))
                              .SetMax(HdRetainedTypedSampledDataSource<GfVec3d>::New(GfVec3d(extent[1])))
                              .Build());
    }
    names.push_back(HdPrimvarsSchema::GetSchemaToken());
    sources.push_back(buildPrimvars(prim, v));

    if (prim.type == HdPrimTypeTokens->mesh) {
        VtIntArray counts, indices, holes;
        TfToken orientation = UsdGeomTokens->rightHanded, scheme = UsdGeomTokens->catmullClark;
        bool doubleSided = false;
        attributeAs(c, v, path(UsdGeomTokens->faceVertexCounts), &counts);
        attributeAs(c, v, path(UsdGeomTokens->faceVertexIndices), &indices);
        attributeAs(c, v, path(UsdGeomTokens->holeIndices), &holes);
        attributeAs(c, v, path(UsdGeomTokens->orientation), &orientation);
        attributeAs(c, v, path(UsdGeomTokens->subdivisionScheme), &scheme);
        attributeAs(c, v, path(UsdGeomTokens->doubleSided), &doubleSided);
        names.push_back(HdMeshSchema::GetSchemaToken());
        sources.push_back(HdMeshSchema::Builder()
                              .SetTopology(HdMeshTopologySchema::Builder()
                                               .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(counts))
                                               .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices))
                                               .SetHoleIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(holes))
                                               .SetOrientation(HdMeshTopologySchema::BuildOrientationDataSource(orientation))
                                               .Build())
                              .SetSubdivisionScheme(HdRetainedTypedSampledDataSource<TfToken>::New(scheme))
                              .SetDoubleSided(HdRetainedTypedSampledDataSource<bool>::New(doubleSided))
                              .Build());
    } else if (prim.type == HdPrimTypeTokens->basisCurves) {
        VtIntArray counts;
        TfToken basis = UsdGeomTokens->bezier, type = UsdGeomTokens->cubic, wrap = UsdGeomTokens->nonperiodic;
        attributeAs(c, v, path(UsdGeomTokens->curveVertexCounts), &counts);
        attributeAs(c, v, path(UsdGeomTokens->basis), &basis);
        attributeAs(c, v, path(UsdGeomTokens->type), &type);
        attributeAs(c, v, path(UsdGeomTokens->wrap), &wrap);
        names.push_back(HdBasisCurvesSchema::GetSchemaToken());
        sources.push_back(HdBasisCurvesSchema::Builder()
                              .SetTopology(HdBasisCurvesTopologySchema::Builder()
                                               .SetCurveVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(counts))
                                               .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(basis))
                                               .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(type))
                                               .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(wrap))
                                               .Build())
                              .Build());
    }

    return HdRetainedContainerDataSource::New(names.size(), names.data(), sources.data());
}

GeoSceneIndexRefPtr
GeoSceneIndex::New(const SdfPath& prefix)
{
    return TfCreateRefPtr(new GeoSceneIndex(prefix));
}

GeoSceneIndex::GeoSceneIndex(const SdfPath& prefix)
    : _prefix(prefix.IsEmpty() ? SdfPath::AbsoluteRootPath() : prefix)
{
    auto empty = std::make_shared<Snapshot>();
    empty->prims.emplace(SdfPath::AbsoluteRootPath(), std::make_unique<Prim>());
    _snapshot = empty;
}

GeoSceneIndex::~GeoSceneIndex() = default;

std::shared_ptr<const GeoSceneIndex::Snapshot>
GeoSceneIndex::snapshot() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _snapshot;
}

std::shared_ptr<const GeoUsdConvert>
GeoSceneIndex::convert() const
{
    return snapshot()->convert;
}

SdfPath
GeoSceneIndex::indexPath(const SdfPath& convertPath) const
{
    return convertPath.ReplacePrefix(SdfPath::AbsoluteRootPath(), _prefix);
}

HdSceneIndexPrim
GeoSceneIndex::GetPrim(const SdfPath& primPath) const
{
    const std::shared_ptr<const Snapshot> s = snapshot();
    const Prim* prim = s->find(primPath);
    if (!prim || primPath.IsAbsoluteRootPath()) {
        return HdSceneIndexPrim();
    }
    std::call_once(prim->built, [&] {
        prim->dataSource = s->build(*prim);
    });

    return HdSceneIndexPrim { prim->type, prim->dataSource };
}

SdfPathVector
GeoSceneIndex::GetChildPrimPaths(const SdfPath& primPath) const
{
    const std::shared_ptr<const Snapshot> s = snapshot();
    const Prim* prim = s->find(primPath);

    return prim ? prim->children : SdfPathVector();
}

void
GeoSceneIndex::update(std::shared_ptr<GeoUsdConvert> next)
{
    if (next && next->options().inputScene) {
        TF_CODING_ERROR("GeoSceneIndex cannot serve a convert authored over an input scene");
        next.reset();
    }
    const std::shared_ptr<const Snapshot> before = snapshot();
    if (next == before->convert) {
        return;
    }
    // Partition signatures hash the detail only, so they say nothing across differing options.
    const bool sameSource = next && before->convert && sameOptions(next->options(), before->convert->options());
    if (sameSource) {
        next->adoptUnchangedPartitions(*before->convert);
    }

    auto after = std::make_shared<Snapshot>();
    after->convert = next;
    after->prims.emplace(SdfPath::AbsoluteRootPath(), std::make_unique<Prim>());
    if (next) {
        const GeoUsdConvert& c = *next;
        // Served prims and the ancestors they need, by convert path.
        std::set<SdfPath> kept;
        c.visitSpecs([&](const SdfPath& path) {
            if (!path.IsPrimPath()) {
                return true;
            }
            const int partition = c.specPartition(path);
            if (partition >= 0 && servedKind(c.partitionKind(std::size_t(partition)))) {
                TfToken usdType;
                fieldAs(c, path, SdfFieldKeys->TypeName, &usdType);
                if (!hydraType(usdType).IsEmpty()) {
                    for (SdfPath p = path; p.IsPrimPath() && kept.insert(p).second; p = p.GetParentPath()) {
                    }
                }
            }

            return true;
        });
        if (!kept.empty()) {
            for (SdfPath p = _prefix; p.IsPrimPath(); p = p.GetParentPath()) {
                after->prims.emplace(p, std::make_unique<Prim>());
            }
        }
        // std::set order puts every parent before its children.
        for (const SdfPath& source : kept) {
            auto prim = std::make_unique<Prim>();
            prim->source = source;
            const int partition = c.specPartition(source);
            TfToken usdType;
            fieldAs(c, source, SdfFieldKeys->TypeName, &usdType);
            if (partition >= 0 && servedKind(c.partitionKind(std::size_t(partition)))) {
                prim->type = hydraType(usdType);
                prim->partition = partition;
            }
            fieldAs(c, source, SdfChildrenKeys->PropertyChildren, &prim->properties);

            const SdfPath path = indexPath(source);
            const Prim* parent = after->find(path.GetParentPath());
            bool resets = false;
            const GfMatrix4d local = localTransform(c, source, &resets);
            prim->world = (resets || !parent) ? local : local * parent->world;
            TfToken visibility;
            prim->visible = (!parent || parent->visible) && !(attributeAs(c, nullptr, source.AppendProperty(UsdGeomTokens->visibility), &visibility) && visibility == UsdGeomTokens->invisible);
            TfToken purpose;
            if (attributeAs(c, nullptr, source.AppendProperty(UsdGeomTokens->purpose), &purpose) && !purpose.IsEmpty()) {
                // Hydra names the default purpose after its render tag.
                prim->purpose = purpose == UsdGeomTokens->default_ ? HdRenderTagTokens->geometry : purpose;
            } else {
                prim->purpose = (parent && !parent->purpose.IsEmpty()) ? parent->purpose : HdRenderTagTokens->geometry;
            }
            after->prims.emplace(path, std::move(prim));
        }
        for (auto& entry : after->prims) {
            if (!entry.first.IsAbsoluteRootPath()) {
                after->prims.at(entry.first.GetParentPath())->children.push_back(entry.first);
            }
        }
    }

    HdSceneIndexObserver::RemovedPrimEntries removed;
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    if (_IsObserved()) {
        auto retyped = [&](const Prim& a, const Prim& b) {
            return a.type != b.type || (a.partition >= 0) != (b.partition >= 0) || a.source.IsEmpty() != b.source.IsEmpty();
        };
        // Hydra drops the whole subtree under a removed prim, so its survivors are added again.
        SdfPath removedRoot;
        for (const auto& entry : before->prims) {
            const SdfPath& path = entry.first;
            if (path.IsAbsoluteRootPath() || (!removedRoot.IsEmpty() && path.HasPrefix(removedRoot))) {
                continue;
            }
            const Prim* b = after->find(path);
            if (!b || retyped(*entry.second, *b)) {
                removed.emplace_back(path);
                removedRoot = path;
            }
        }
        auto underRemoved = [&](const SdfPath& path) {
            const auto it = std::upper_bound(removed.begin(), removed.end(), path, [](const SdfPath& p, const HdSceneIndexObserver::RemovedPrimEntry& e) {
                return p < e.primPath;
            });

            return it != removed.begin() && path.HasPrefix(std::prev(it)->primPath);
        };
        for (const auto& entry : after->prims) {
            const SdfPath& path = entry.first;
            if (path.IsAbsoluteRootPath()) {
                continue;
            }
            const Prim& b = *entry.second;
            const Prim* a = before->find(path);
            if (!a || underRemoved(path)) {
                added.emplace_back(path, b.type);
                continue;
            }
            HdDataSourceLocatorSet locators;
            if (a->world != b.world) {
                locators.insert(HdXformSchema::GetDefaultLocator());
            }
            if (a->visible != b.visible) {
                locators.insert(HdVisibilitySchema::GetDefaultLocator());
            }
            if (a->purpose != b.purpose) {
                locators.insert(HdPurposeSchema::GetDefaultLocator());
            }
            if (b.partition >= 0) {
                const GeoUsdConvert& oc = *before->convert;
                const GeoUsdConvert& nc = *next;
                const std::size_t op = std::size_t(a->partition), np = std::size_t(b.partition);
                if (!sameSource || oc.partitionSignature(op) != nc.partitionSignature(np)) {
                    if (!oc.isPartitionBuilt(op)) {
                        // Nothing read the old values, so there is nothing to compare them with.
                        locators.insert(partitionLocators(b.type));
                    } else {
                        const std::shared_ptr<const GeoUsdValueMap> ov = oc.partitionValues(op), nv = nc.partitionValues(np);
                        std::set<TfToken> properties(a->properties.begin(), a->properties.end());
                        properties.insert(b.properties.begin(), b.properties.end());
                        for (const TfToken& property : properties) {
                            HdDataSourceLocator locator;
                            if (!locatorOf(b.type, property, &locator) || locators.Contains(locator)) {
                                continue;
                            }
                            if (!sameAttribute(oc, ov.get(), a->source.AppendProperty(property), nc, nv.get(), b.source.AppendProperty(property))) {
                                locators.insert(locator);
                            }
                        }
                    }
                }
            }
            if (!locators.IsEmpty()) {
                dirtied.emplace_back(path, locators);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _snapshot = after;
    }
    if (!removed.empty()) {
        _SendPrimsRemoved(removed);
    }
    if (!added.empty()) {
        _SendPrimsAdded(added);
    }
    if (!dirtied.empty()) {
        _SendPrimsDirtied(dirtied);
    }
}

NATRON_NAMESPACE_EXIT
