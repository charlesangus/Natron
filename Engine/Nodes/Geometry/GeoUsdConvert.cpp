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

#include "GeoUsdConvert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef emit
#define NATRON_GEOUSDCONVERT_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/curves.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/subset.h>
#ifdef NATRON_GEOUSDCONVERT_RESTORE_EMIT
#undef NATRON_GEOUSDCONVERT_RESTORE_EMIT
#define emit
#endif

#include "Engine/Hash64.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

typedef std::unordered_map<SdfPath, VtValue, SdfPath::Hash> GeoUsdValueMap;

namespace {

struct Tokens {
    const TfToken mesh { "Mesh" };
    const TfToken basisCurves { "BasisCurves" };
    const TfToken points { "Points" };
    const TfToken pointInstancer { "PointInstancer" };
    const TfToken geomSubset { "GeomSubset" };
    const TfToken xform { "Xform" };
    const TfToken scope { "Scope" };
    const TfToken prototypes { "Prototypes" };
    const TfToken xformOpTransform { "xformOp:transform" };
    const TfToken pathAttr { "path" };
};

const Tokens&
tokens()
{
    static const Tokens t;

    return t;
}

std::size_t
storageBytes(GeoStorageEnum storage)
{
    switch (storage) {
    case eGeoStorageInt32:
    case eGeoStorageFloat32:
    case eGeoStorageString:
        return 4;
    case eGeoStorageInt64:
    case eGeoStorageFloat64:
        return 8;
    }

    return 4;
}

template <class Elem>
VtValue
packArray(const void* flat,
          std::size_t count)
{
    VtArray<Elem> a(count);
    if (count) {
        std::memcpy(static_cast<void*>(a.data()), flat, count * sizeof(Elem));
    }

    return VtValue(std::move(a));
}

// Accepts the array type and, for constant primvars authored as scalars, the element type.
template <class Elem>
bool
unpackArray(const VtValue& value,
            std::vector<std::uint64_t>* flat,
            std::size_t* count)
{
    const Elem* data = nullptr;
    std::size_t n = 0;
    if (value.IsHolding<VtArray<Elem>>()) {
        const VtArray<Elem>& a = value.UncheckedGet<VtArray<Elem>>();
        data = a.cdata();
        n = a.size();
    } else if (value.IsHolding<Elem>()) {
        data = &value.UncheckedGet<Elem>();
        n = 1;
    } else {
        return false;
    }
    flat->assign((n * sizeof(Elem) + 7) / 8, 0);
    if (n) {
        std::memcpy(static_cast<void*>(flat->data()), static_cast<const void*>(data), n * sizeof(Elem));
    }
    *count = n;

    return true;
}

struct GeoUsdType {
    GeoStorageEnum storage;
    int tupleSize;
    GeoTypeInfoEnum typeInfo;
    SdfValueTypeName scalar;
    std::size_t elementBytes;
    VtValue (*pack)(const void*, std::size_t);
    bool (*unpack)(const VtValue&, std::vector<std::uint64_t>*, std::size_t*);
    // Export only picks a row whose exportable flag is set; the others are read-only aliases.
    bool exportable;
};

template <class Elem>
GeoUsdType
row(GeoStorageEnum storage,
    int tupleSize,
    GeoTypeInfoEnum typeInfo,
    const SdfValueTypeName& scalar,
    bool exportable = true)
{
    return GeoUsdType { storage, tupleSize, typeInfo, scalar, sizeof(Elem), &packArray<Elem>, &unpackArray<Elem>, exportable };
}

const std::vector<GeoUsdType>&
typeTable()
{
    static const std::vector<GeoUsdType> table = [] {
        const auto& n = SdfValueTypeNames;

        return std::vector<GeoUsdType> {
            row<float>(eGeoStorageFloat32, 1, eGeoTypeInfoNone, n->Float),
            row<GfVec2f>(eGeoStorageFloat32, 2, eGeoTypeInfoNone, n->Float2),
            row<GfVec3f>(eGeoStorageFloat32, 3, eGeoTypeInfoNone, n->Float3),
            row<GfVec4f>(eGeoStorageFloat32, 4, eGeoTypeInfoNone, n->Float4),
            row<GfVec3f>(eGeoStorageFloat32, 3, eGeoTypeInfoColour, n->Color3f),
            row<GfVec4f>(eGeoStorageFloat32, 4, eGeoTypeInfoColour, n->Color4f),
            row<GfVec3f>(eGeoStorageFloat32, 3, eGeoTypeInfoNormal, n->Normal3f),
            row<GfVec3f>(eGeoStorageFloat32, 3, eGeoTypeInfoPoint, n->Point3f),
            row<GfVec3f>(eGeoStorageFloat32, 3, eGeoTypeInfoVector, n->Vector3f),
            // GfQuatf stores the imaginary part first, the attribute's (x, y, z, w) order.
            row<GfQuatf>(eGeoStorageFloat32, 4, eGeoTypeInfoQuaternion, n->Quatf),
            row<GfVec2f>(eGeoStorageFloat32, 2, eGeoTypeInfoNone, n->TexCoord2f, false),
            row<GfVec3f>(eGeoStorageFloat32, 3, eGeoTypeInfoNone, n->TexCoord3f, false),
            row<double>(eGeoStorageFloat64, 1, eGeoTypeInfoNone, n->Double),
            row<GfVec2d>(eGeoStorageFloat64, 2, eGeoTypeInfoNone, n->Double2),
            row<GfVec3d>(eGeoStorageFloat64, 3, eGeoTypeInfoNone, n->Double3),
            row<GfVec4d>(eGeoStorageFloat64, 4, eGeoTypeInfoNone, n->Double4),
            row<GfVec3d>(eGeoStorageFloat64, 3, eGeoTypeInfoColour, n->Color3d),
            row<GfVec4d>(eGeoStorageFloat64, 4, eGeoTypeInfoColour, n->Color4d),
            row<GfVec3d>(eGeoStorageFloat64, 3, eGeoTypeInfoNormal, n->Normal3d),
            row<GfVec3d>(eGeoStorageFloat64, 3, eGeoTypeInfoPoint, n->Point3d),
            row<GfVec3d>(eGeoStorageFloat64, 3, eGeoTypeInfoVector, n->Vector3d),
            row<GfQuatd>(eGeoStorageFloat64, 4, eGeoTypeInfoQuaternion, n->Quatd),
            row<GfMatrix4d>(eGeoStorageFloat64, 16, eGeoTypeInfoMatrix, n->Matrix4d),
            row<GfVec2d>(eGeoStorageFloat64, 2, eGeoTypeInfoNone, n->TexCoord2d, false),
            row<int>(eGeoStorageInt32, 1, eGeoTypeInfoNone, n->Int),
            row<GfVec2i>(eGeoStorageInt32, 2, eGeoTypeInfoNone, n->Int2),
            row<GfVec3i>(eGeoStorageInt32, 3, eGeoTypeInfoNone, n->Int3),
            row<GfVec4i>(eGeoStorageInt32, 4, eGeoTypeInfoNone, n->Int4),
            row<std::int64_t>(eGeoStorageInt64, 1, eGeoTypeInfoNone, n->Int64),
        };
    }();

    return table;
}

const GeoUsdType*
findExportType(GeoStorageEnum storage,
               int tupleSize,
               GeoTypeInfoEnum typeInfo)
{
    for (const GeoUsdType& t : typeTable()) {
        if (t.exportable && t.storage == storage && t.tupleSize == tupleSize && t.typeInfo == typeInfo) {
            return &t;
        }
    }
    for (const GeoUsdType& t : typeTable()) {
        if (t.exportable && t.storage == storage && t.tupleSize == tupleSize && t.typeInfo == eGeoTypeInfoNone) {
            return &t;
        }
    }

    return nullptr;
}

const GeoUsdType*
findReadType(const SdfValueTypeName& scalar)
{
    for (const GeoUsdType& t : typeTable()) {
        if (t.scalar == scalar) {
            return &t;
        }
    }

    return nullptr;
}

bool
isPackedIntrinsic(const std::string& name)
{
    return name == kGeoPackedXformAttr || name == kGeoPackedBoundsAttr || name == kGeoPackedDetailAttr || name == kGeoPackedUSDPathAttr || name == kGeoPackedUSDParentXformAttr;
}

std::string
kindName(GeoUsdPrimKindEnum kind)
{
    switch (kind) {
    case eGeoUsdPrimMesh:
        return "mesh";
    case eGeoUsdPrimCurves:
        return "curves";
    case eGeoUsdPrimPoints:
        return "points";
    case eGeoUsdPrimInstancer:
        return "instancer";
    case eGeoUsdPrimNativeInstances:
        return "instances";
    case eGeoUsdPrimOverride:
        return "override";
    }

    return "prim";
}

TfToken
kindTypeName(GeoUsdPrimKindEnum kind)
{
    const Tokens& tk = tokens();
    switch (kind) {
    case eGeoUsdPrimMesh:
        return tk.mesh;
    case eGeoUsdPrimCurves:
        return tk.basisCurves;
    case eGeoUsdPrimPoints:
        return tk.points;
    case eGeoUsdPrimInstancer:
        return tk.pointInstancer;
    case eGeoUsdPrimNativeInstances:
        return tk.xform;
    case eGeoUsdPrimOverride:
        break;
    }

    return TfToken();
}

// The local transform that puts a prim under @p parent at @p world, both USD (row-vector)
// matrices; @p world itself when the parent cannot be inverted.
GfMatrix4d
relativeTo(const GfMatrix4d& world,
           const GfMatrix4d& parent)
{
    double det = 0.;
    const GfMatrix4d inverse = parent.GetInverse(&det);
    if (std::abs(det) < 1e-12) {
        return world;
    }

    return world * inverse;
}

bool
isDirection(GeoTypeInfoEnum typeInfo)
{
    return (typeInfo == eGeoTypeInfoNormal) || (typeInfo == eGeoTypeInfoVector);
}

// The row-vector matrix that carries directions of @p typeInfo as @p m carries points: the
// inverse transpose for normals, @p m itself for vectors, of which TransformDir() uses the
// linear part only.
GfMatrix4d
directionMatrix(const GfMatrix4d& m,
                GeoTypeInfoEnum typeInfo)
{
    if (typeInfo != eGeoTypeInfoNormal) {
        return m;
    }
    double det = 0.;
    const GfMatrix4d inverse = m.GetInverse(&det);
    if (std::abs(det) < 1e-12) {
        return m;
    }

    return inverse.GetTranspose();
}

// Carries @p count float or double 3-vectors of @p typeInfo in place by @p directions, from
// directionMatrix(); normals come out unit length.
void
carryDirections(void* data,
                GeoStorageEnum storage,
                std::size_t count,
                const GfMatrix4d& directions,
                GeoTypeInfoEnum typeInfo)
{
    const auto carry = [&](GfVec3d v) {
        v = directions.TransformDir(v);
        const double length = v.GetLength();
        if ((typeInfo == eGeoTypeInfoNormal) && (length > 1e-12)) {
            v /= length;
        }

        return v;
    };
    if (storage == eGeoStorageFloat32) {
        float* f = static_cast<float*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            const GfVec3d v = carry(GfVec3d(f[i * 3], f[i * 3 + 1], f[i * 3 + 2]));
            f[i * 3] = float(v[0]);
            f[i * 3 + 1] = float(v[1]);
            f[i * 3 + 2] = float(v[2]);
        }
    } else if (storage == eGeoStorageFloat64) {
        double* d = static_cast<double*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            const GfVec3d v = carry(GfVec3d(d[i * 3], d[i * 3 + 1], d[i * 3 + 2]));
            d[i * 3] = v[0];
            d[i * 3 + 1] = v[1];
            d[i * 3 + 2] = v[2];
        }
    }
}

// Whether the upper 3x3 of a USD (row-vector) matrix shears: its rows are not orthogonal.
bool
shears(const GfMatrix4d& m)
{
    const GfVec3d rows[3] = { m.GetRow3(0), m.GetRow3(1), m.GetRow3(2) };
    const double lengths[3] = { rows[0].GetLength(), rows[1].GetLength(), rows[2].GetLength() };
    const int pairs[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };
    for (const auto& pair : pairs) {
        const double scale = lengths[pair[0]] * lengths[pair[1]];
        if ((scale > 1e-24) && (std::abs(GfDot(rows[pair[0]], rows[pair[1]])) > 1e-6 * scale)) {
            return true;
        }
    }

    return false;
}

// A shear-free USD matrix as a point instancer's scale, orientation and position, whose product
// scale * rotation * translation is the matrix again: a reflection is a negative X scale.
void
decomposeInstance(const GfMatrix4d& m,
                  GfVec3f* scale,
                  GfQuath* orientation,
                  GfVec3f* position)
{
    *position = GfVec3f(m.ExtractTranslation());
    const GfVec3d rows[3] = { m.GetRow3(0), m.GetRow3(1), m.GetRow3(2) };
    double lengths[3] = { rows[0].GetLength(), rows[1].GetLength(), rows[2].GetLength() };
    if (m.GetDeterminant3() < 0.) {
        lengths[0] = -lengths[0];
    }
    *scale = GfVec3f(float(lengths[0]), float(lengths[1]), float(lengths[2]));
    if ((std::abs(lengths[0]) < 1e-12) || (std::abs(lengths[1]) < 1e-12) || (std::abs(lengths[2]) < 1e-12)) {
        *orientation = GfQuath(m.RemoveScaleShear().ExtractRotationQuat());

        return;
    }
    GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const GfVec3d row = rows[i] / lengths[i];
        rotation.SetRow(i, GfVec4d(row[0], row[1], row[2], 0.));
    }
    *orientation = GfQuath(rotation.ExtractRotationQuat());
}

bool
isWorldSpace(const GeoDetail& d)
{
    const GeoAttribute* a = d.findAttribute(eGeoAttribClassDetail, kGeoUsdWorldSpaceAttr);

    return a && (a->storage() == eGeoStorageInt32) && !a->isArray() && (a->get<std::int32_t>(0) != 0);
}

const GeoAttribute*
usdSourceAttribute(const GeoDetail& d,
                   GeoAttribClassEnum cls)
{
    const GeoAttribute* a = d.findAttribute(cls, kGeoUsdSourceAttr);

    return (a && (a->storage() == eGeoStorageInt32) && (a->tupleSize() == 1) && !a->isArray()) ? a : nullptr;
}

bool
readFromUsd(const GeoAttribute* source,
            GeoOffset off)
{
    return source && (source->get<std::int32_t>(std::size_t(off)) != 0);
}

// The point attribute that places loose points, when it is a single string.
const GeoAttribute*
pointPathAttribute(const GeoDetail& d)
{
    const GeoAttribute* a = d.findAttribute(eGeoAttribClassPoint, "path");

    return (a && (a->storage() == eGeoStorageString) && (a->tupleSize() == 1) && !a->isArray()) ? a : nullptr;
}

void
hashMatrix(Hash64& h,
           const GfMatrix4d& m)
{
    const double* data = m.GetArray();
    for (int i = 0; i < 16; ++i) {
        h.append(data[i]);
    }
}

// A path attribute value as a prim path: components made valid identifiers, relative values
// under @p root. Empty when the value names nothing.
SdfPath
resolvePathValue(std::string_view value,
                 const SdfPath& root)
{
    SdfPath path = (!value.empty() && value.front() == '/') ? SdfPath::AbsoluteRootPath() : root;
    bool any = false;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        std::size_t end = value.find('/', begin);
        if (end == std::string_view::npos) {
            end = value.size();
        }
        if (end > begin) {
            const std::string component(value.substr(begin, end - begin));
            path = path.AppendChild(TfToken(TfMakeValidIdentifier(component)));
            any = true;
        }
        begin = end + 1;
    }

    return any ? path : SdfPath();
}

void
transpose16(double* m)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = r + 1; c < 4; ++c) {
            std::swap(m[r * 4 + c], m[c * 4 + r]);
        }
    }
}

std::vector<std::size_t>
pagesOf(const std::vector<std::size_t>& offsets)
{
    std::vector<std::size_t> pages;
    pages.reserve(offsets.size() / kGeoPageSize + 1);
    for (std::size_t o : offsets) {
        const std::size_t p = o >> kGeoPageBits;
        if (pages.empty() || pages.back() != p) {
            pages.push_back(p);
        }
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    return pages;
}

void
hashPages(Hash64& h,
          const GeoAttribute& a,
          const std::vector<std::size_t>& pages)
{
    if (a.storage() == eGeoStorageString || a.isArray()) {
        h.append(a.contentHash());

        return;
    }
    const GeoPagedColumn& column = a.values();
    for (std::size_t p : pages) {
        h.append(U64(p));
        h.append(p < column.pageCount() ? column.page(p)->contentHash() : U64(0));
    }
}

// Raw component storage for one attribute over @p offsets, 8-byte aligned.
std::vector<std::uint64_t>
gatherComponents(const GeoAttribute& a,
                 const std::vector<std::size_t>& offsets)
{
    const std::size_t scalars = offsets.size() * std::size_t(a.tupleSize());
    std::vector<std::uint64_t> flat((scalars * storageBytes(a.storage()) + 7) / 8, 0);
    const std::span<const std::size_t> at(offsets.data(), offsets.size());
    switch (a.storage()) {
    case eGeoStorageInt32:
        a.copyDense<std::int32_t>(at, std::span<std::int32_t>(reinterpret_cast<std::int32_t*>(flat.data()), scalars));
        break;
    case eGeoStorageInt64:
        a.copyDense<std::int64_t>(at, std::span<std::int64_t>(reinterpret_cast<std::int64_t*>(flat.data()), scalars));
        break;
    case eGeoStorageFloat32:
        a.copyDense<float>(at, std::span<float>(reinterpret_cast<float*>(flat.data()), scalars));
        break;
    case eGeoStorageFloat64:
        a.copyDense<double>(at, std::span<double>(reinterpret_cast<double*>(flat.data()), scalars));
        break;
    case eGeoStorageString:
        break;
    }

    return flat;
}

VtVec3fArray
gatherPositions(const GeoDetail& d,
                const std::vector<std::size_t>& offsets)
{
    VtVec3fArray out(offsets.size());
    const GeoAttribute& p = d.position();
    if (p.storage() == eGeoStorageFloat32) {
        p.copyDense<float>(std::span<const std::size_t>(offsets.data(), offsets.size()),
                           std::span<float>(reinterpret_cast<float*>(out.data()), offsets.size() * 3));
    } else {
        std::vector<double> tmp(offsets.size() * 3);
        p.copyDense<double>(std::span<const std::size_t>(offsets.data(), offsets.size()), std::span<double>(tmp));
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            out[i] = GfVec3f(float(tmp[i * 3]), float(tmp[i * 3 + 1]), float(tmp[i * 3 + 2]));
        }
    }

    return out;
}

VtVec3fArray
extentOf(const VtVec3fArray& points)
{
    VtVec3fArray extent(2, GfVec3f(0.f));
    if (points.empty()) {
        return extent;
    }
    GfVec3f lo = points[0];
    GfVec3f hi = points[0];
    for (const GfVec3f& p : points) {
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
    }
    extent[0] = lo;
    extent[1] = hi;

    return extent;
}

template <class F>
void
forEachLive(const GeoIndexMap& map,
            F&& f)
{
    const GeoIndex n = map.size();
    for (GeoIndex i = 0; i < n; ++i) {
        f(map.offsetFromIndex(i));
    }
}

} // namespace

GfMatrix4d
geoUsdMatrix(const GeoMatrix4& m)
{
    GfMatrix4d out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = m[std::size_t(c * 4 + r)];
        }
    }

    return out;
}

GeoMatrix4
geoMatrixFromUsd(const GfMatrix4d& m)
{
    GeoMatrix4 out {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[std::size_t(r * 4 + c)] = m[c][r];
        }
    }

    return out;
}

enum GeoUsdPlanSourceEnum {
    eGeoUsdSourceBuiltin = 0,
    eGeoUsdSourceStatic,
    eGeoUsdSourceAttribute,
    eGeoUsdSourceGroup,
};

struct GeoUsdConvert::PropertyPlan {
    TfToken name;
    SdfValueTypeName typeName;
    SdfVariability variability = SdfVariabilityVarying;
    TfToken interpolation;
    int elementSize = 0;
    GeoUsdPlanSourceEnum source = eGeoUsdSourceBuiltin;
    GeoAttribClassEnum cls = eGeoAttribClassPoint;
    std::string attribute;
    const GeoUsdType* type = nullptr;
    bool toTexCoord2 = false;
    VtValue staticDefault;
    bool relationship = false;
    SdfPathVector targets;
};

struct GeoUsdConvert::SpecEntry {
    SdfSpecType type = SdfSpecTypeUnknown;
    std::int32_t partition = -1;
    // When set, the spec belongs to that prototype of the partition, which answers for it.
    std::int32_t prototype = -1;
    std::vector<std::pair<TfToken, VtValue>> fields;
    bool lazyDefault = false;
    TfTokenVector primChildren;
    TfTokenVector properties;

    void set(const TfToken& name,
             VtValue value)
    {
        for (auto& f : fields) {
            if (f.first == name) {
                f.second = std::move(value);

                return;
            }
        }
        fields.emplace_back(name, std::move(value));
    }

    const VtValue* get(const TfToken& name) const
    {
        for (const auto& f : fields) {
            if (f.first == name) {
                return &f.second;
            }
        }

        return nullptr;
    }
};

struct GeoUsdConvert::Partition {
    SdfPath path;
    GeoUsdPrimKindEnum kind = eGeoUsdPrimMesh;
    // The path value before per-type children were split off, for the warning.
    SdfPath requestedPath;
    std::vector<std::size_t> primitives;
    std::vector<std::size_t> points;
    std::vector<PropertyPlan> properties;
    std::vector<std::pair<TfToken, std::string>> subsets;
    std::vector<std::int32_t> instancePrototype;
    std::vector<GeoDetailConstPtr> prototypeDetails;
    std::vector<std::shared_ptr<const GeoUsdConvert>> prototypes;
    // The input holds this prim, and with the type this partition authors.
    bool onInput = false;
    bool sameTypeOnInput = false;

    mutable std::mutex mutex;
    mutable std::shared_ptr<const GeoUsdValueMap> values;
    mutable bool hasSignature = false;
    mutable U64 signature = 0;
};

GeoUsdConvert::GeoUsdConvert(GeoDetailConstPtr detail,
                             GeoUsdConvertOptions options)
    : GeoUsdConvert(std::move(detail), std::move(options), false)
{
}

GeoUsdConvert::GeoUsdConvert(GeoDetailConstPtr detail,
                             GeoUsdConvertOptions options,
                             bool nested)
    : _detail(std::move(detail))
    , _options(std::move(options))
{
    if (!_detail) {
        _detail = std::make_shared<const GeoDetail>();
    }
    if (!_options.rootPath.IsAbsolutePath() || !_options.rootPath.IsPrimPath()) {
        _warnings.push_back("The root path " + _options.rootPath.GetString() + " is not an absolute prim path; /geo is used");
        _options.rootPath = SdfPath("/geo");
    }
    build(nested);
}

GeoUsdConvert::~GeoUsdConvert()
{
}

void
GeoUsdConvert::build(bool nested)
{
    const Tokens& tk = tokens();

    if (_options.packedMode == eGeoUsdPackedUnpack) {
        bool hasPacked = false;
        const GeoDetail& source = *_detail;
        forEachLive(source.primitiveMap(), [&](GeoOffset off) {
            hasPacked = hasPacked || source.primitiveType(off) == eGeoPrimitivePackedGeometry;
        });
        if (hasPacked) {
            _detail = std::make_shared<const GeoDetail>(geoUnpackPackedGeometry(source, &_warnings));
        }
    }
    const GeoDetail& d = *_detail;

    if (nested) {
        _topPath = _options.rootPath;
        SpecEntry root;
        root.type = SdfSpecTypePrim;
        root.set(SdfFieldKeys->Specifier, VtValue(SdfSpecifierDef));
        root.set(SdfFieldKeys->TypeName, VtValue(tk.xform));
        _specs.emplace(_topPath, std::move(root));
    } else {
        _topPath = SdfPath::AbsoluteRootPath();
        SpecEntry root;
        root.type = SdfSpecTypePseudoRoot;
        _specs.emplace(_topPath, std::move(root));
    }

    const GeoAttribute* pathAttr = nullptr;
    for (const char* name : { "path", "name" }) {
        const GeoAttribute* a = d.findAttribute(eGeoAttribClassPrimitive, name);
        if (!a) {
            continue;
        }
        if (a->storage() != eGeoStorageString || a->tupleSize() != 1 || a->isArray()) {
            _warnings.push_back(std::string("The primitive attribute ") + name + " is not a single string; it does not place primitives");
            continue;
        }
        pathAttr = a;
        break;
    }

    // Partitions keyed by (path, kind) in first-use order; an empty path means unpathed.
    std::map<std::pair<SdfPath, int>, std::size_t> byKey;
    std::vector<SdfPath> resolvedByString;
    std::vector<char> resolvedKnown;
    std::set<SdfPath> overridePaths;
    std::set<SdfPath> fromUsd;
    bool warnedBadUSDPath = false;
    const GeoAttribute* primitiveSource = usdSourceAttribute(d, eGeoAttribClassPrimitive);

    auto partitionFor = [&](const SdfPath& path, GeoUsdPrimKindEnum kind) -> Partition& {
        const auto key = std::make_pair(path, int(kind));
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            it = byKey.emplace(key, _partitions.size()).first;
            auto p = std::make_unique<Partition>();
            p->kind = kind;
            p->requestedPath = path;
            _partitions.push_back(std::move(p));
        }

        return *_partitions[it->second];
    };

    forEachLive(d.primitiveMap(), [&](GeoOffset off) {
        const GeoPrimitiveTypeEnum type = d.primitiveType(off);
        if (type == eGeoPrimitivePackedUSD) {
            const SdfPath source(std::string(geoPackedUSDPath(d, off)));
            if (source.IsEmpty() || !source.IsAbsolutePath() || !source.IsPrimPath()) {
                if (!warnedBadUSDPath) {
                    _warnings.push_back("A packed USD primitive has no valid source path; it is not exported");
                    warnedBadUSDPath = true;
                }

                return;
            }
            if (!overridePaths.insert(source).second) {
                _warnings.push_back("Several packed USD primitives override " + source.GetString() + "; the first one is kept");

                return;
            }
            Partition& p = partitionFor(source, eGeoUsdPrimOverride);
            p.primitives.push_back(std::size_t(off));

            return;
        }
        GeoUsdPrimKindEnum kind = eGeoUsdPrimMesh;
        if (type == eGeoPrimitivePolyline) {
            kind = eGeoUsdPrimCurves;
        } else if (type == eGeoPrimitivePackedGeometry) {
            kind = _options.packedMode == eGeoUsdPackedNativeInstances ? eGeoUsdPrimNativeInstances : eGeoUsdPrimInstancer;
        }
        SdfPath path;
        if (pathAttr) {
            const std::int32_t s = pathAttr->get<std::int32_t>(std::size_t(off));
            if (s >= 0) {
                if (std::size_t(s) >= resolvedByString.size()) {
                    resolvedByString.resize(std::size_t(s) + 1);
                    resolvedKnown.resize(std::size_t(s) + 1, 0);
                }
                if (!resolvedKnown[std::size_t(s)]) {
                    resolvedByString[std::size_t(s)] = resolvePathValue(pathAttr->getString(std::size_t(off)), _options.rootPath);
                    resolvedKnown[std::size_t(s)] = 1;
                }
                path = resolvedByString[std::size_t(s)];
            }
        }
        if (!path.IsEmpty() && readFromUsd(primitiveSource, off)) {
            fromUsd.insert(path);
        }
        partitionFor(path, kind).primitives.push_back(std::size_t(off));
    });

    {
        std::vector<char> used(d.elementOffsetSize(eGeoAttribClassPoint), 0);
        forEachLive(d.vertexMap(), [&](GeoOffset v) {
            const GeoOffset p = d.vertexPoint(v);
            if (p >= 0 && std::size_t(p) < used.size()) {
                used[std::size_t(p)] = 1;
            }
        });
        const GeoAttribute* pointPath = pointPathAttribute(d);
        const GeoAttribute* pointSource = usdSourceAttribute(d, eGeoAttribClassPoint);
        std::vector<SdfPath> pointPathByString;
        std::vector<char> pointPathKnown;
        forEachLive(d.pointMap(), [&](GeoOffset p) {
            if (used[std::size_t(p)]) {
                return;
            }
            SdfPath path;
            if (pointPath) {
                const std::int32_t s = pointPath->get<std::int32_t>(std::size_t(p));
                if (s >= 0) {
                    if (std::size_t(s) >= pointPathByString.size()) {
                        pointPathByString.resize(std::size_t(s) + 1);
                        pointPathKnown.resize(std::size_t(s) + 1, 0);
                    }
                    if (!pointPathKnown[std::size_t(s)]) {
                        pointPathByString[std::size_t(s)] = resolvePathValue(pointPath->getString(std::size_t(p)), _options.rootPath);
                        pointPathKnown[std::size_t(s)] = 1;
                    }
                    path = pointPathByString[std::size_t(s)];
                }
            }
            if (!path.IsEmpty() && readFromUsd(pointSource, p)) {
                fromUsd.insert(path);
            }
            partitionFor(path, eGeoUsdPrimPoints).points.push_back(std::size_t(p));
        });
    }

    for (const auto& p : _partitions) {
        if ((p->kind == eGeoUsdPrimOverride) || p->requestedPath.IsEmpty()) {
            continue;
        }
        GeoUsdPlacementEnum placement = eGeoUsdPlacedAsRequested;
        const SdfPath placed = geoUsdPlacedPath(p->requestedPath, _options.rootPath, _options.inputScene.get(), fromUsd.count(p->requestedPath) != 0, &placement);
        if (placement == eGeoUsdPlacedSourceGone) {
            _warnings.push_back(p->requestedPath.GetString() + " is not in the input scene; it goes under " + _options.rootPath.GetString());
        } else if (placement == eGeoUsdPlacedCollision) {
            _warnings.push_back(p->requestedPath.GetString() + " is already in the input scene and the geometry was not read from it; it goes under " + _options.rootPath.GetString());
        }
        p->requestedPath = placed;
    }

    std::vector<const Partition*> sheared;
    for (const auto& p : _partitions) {
        if (p->kind != eGeoUsdPrimInstancer) {
            continue;
        }
        // A generated child path is never on the input, so it shares the frame of its base.
        const GfMatrix4d frame = frameOf(p->requestedPath.IsEmpty() ? _options.rootPath : p->requestedPath);
        for (std::size_t off : p->primitives) {
            if (shears(relativeTo(geoUsdMatrix(geoPackedTransform(d, GeoOffset(off))), frame))) {
                p->kind = eGeoUsdPrimNativeInstances;
                sheared.push_back(p.get());
                break;
            }
        }
    }

    // Final prim paths: unpathed partitions get <root>/<kind>_0, a path shared by several kinds
    // (or by a packed USD override) gets one <kind>_0 child per kind.
    std::map<SdfPath, int> kindsPerPath;
    for (const auto& p : _partitions) {
        if (!p->requestedPath.IsEmpty()) {
            kindsPerPath[p->requestedPath] += 1;
        }
    }
    std::set<SdfPath> taken;
    for (const auto& p : _partitions) {
        if (p->kind == eGeoUsdPrimOverride) {
            taken.insert(p->requestedPath);
        }
    }
    for (const auto& p : _partitions) {
        if (p->kind == eGeoUsdPrimOverride) {
            p->path = p->requestedPath;
            p->onInput = inputPrim(p->path) != nullptr;
            continue;
        }
        SdfPath base;
        if (p->requestedPath.IsEmpty()) {
            base = _options.rootPath;
        } else if (kindsPerPath[p->requestedPath] > 1) {
            base = p->requestedPath;
            _warnings.push_back("Primitives of different types share the path " + p->requestedPath.GetString() + "; each type becomes a child prim of it");
        } else {
            p->path = p->requestedPath;
            taken.insert(p->path);
            continue;
        }
        for (int i = 0;; ++i) {
            const SdfPath candidate = base.AppendChild(TfToken(kindName(p->kind) + "_" + std::to_string(i)));
            if (taken.insert(candidate).second) {
                p->path = candidate;
                break;
            }
        }
    }
    for (const Partition* p : sheared) {
        _shearedInstances.push_back(p->path);
        _warnings.push_back("Packed geometry at " + p->path.GetString() + " is sheared, which a point instancer cannot hold; it is written as native instances");
    }
    for (const auto& p : _partitions) {
        if (p->kind == eGeoUsdPrimOverride) {
            continue;
        }
        const GeoUsdInputPrim* input = inputPrim(p->path);
        p->onInput = input != nullptr;
        p->sameTypeOnInput = input && input->typeName == kindTypeName(p->kind);
    }

    {
        bool warnedVertexGroups = false;
        if (!d.groupNames(eGeoAttribClassVertex).empty()) {
            _warnings.push_back("Vertex groups have no USD equivalent and are not exported");
            warnedVertexGroups = true;
        }
        if (!d.edgeGroupNames().empty()) {
            _warnings.push_back("Edge groups have no USD equivalent and are not exported");
        }
        (void)warnedVertexGroups;
    }

    if (!nested) {
        bool underRoot = false;
        for (const auto& p : _partitions) {
            underRoot = underRoot || p->path.HasPrefix(_options.rootPath);
        }
        if (underRoot) {
            ensurePrim(_options.rootPath, SdfSpecifierDef, TfToken());
        }
    }

    for (std::size_t i = 0; i < _partitions.size(); ++i) {
        Partition& p = *_partitions[i];
        if (p.kind == eGeoUsdPrimInstancer || p.kind == eGeoUsdPrimNativeInstances) {
            std::map<const GeoDetail*, std::int32_t> index;
            p.instancePrototype.reserve(p.primitives.size());
            for (std::size_t off : p.primitives) {
                const GeoDetailConstPtr contained = geoPackedDetail(d, GeoOffset(off));
                auto it = index.find(contained.get());
                if (it == index.end()) {
                    it = index.emplace(contained.get(), std::int32_t(p.prototypeDetails.size())).first;
                    p.prototypeDetails.push_back(contained);
                }
                p.instancePrototype.push_back(it->second);
            }
            for (std::size_t k = 0; k < p.prototypeDetails.size(); ++k) {
                GeoUsdConvertOptions sub = _options;
                sub.inputScene.reset();
                sub.rootPath = p.path.AppendChild(tk.prototypes).AppendChild(TfToken("proto_" + std::to_string(k)));
                p.prototypes.push_back(std::shared_ptr<const GeoUsdConvert>(new GeoUsdConvert(p.prototypeDetails[k], sub, true)));
            }
        }
        planProperties(p);
        trimInputOverrides(p);
        addPartitionSpecs(i);
    }
    finishSpecs();
}

GeoUsdConvert::SpecEntry&
GeoUsdConvert::ensurePrim(const SdfPath& path,
                          SdfSpecifier specifier,
                          const TfToken& typeName)
{
    const Tokens& tk = tokens();
    // A def would retype a prim the input already holds, so the input's own prims stay overs.
    if (specifier == SdfSpecifierDef && path != _topPath && inputPrim(path)) {
        specifier = SdfSpecifierOver;
    }
    auto it = _specs.find(path);
    if (it != _specs.end()) {
        SpecEntry& e = it->second;
        if (e.type == SdfSpecTypePrim) {
            const VtValue* current = e.get(SdfFieldKeys->Specifier);
            if (specifier == SdfSpecifierDef && current && current->Get<SdfSpecifier>() == SdfSpecifierOver) {
                e.set(SdfFieldKeys->Specifier, VtValue(SdfSpecifierDef));
                if (!e.get(SdfFieldKeys->TypeName) && path.HasPrefix(_options.rootPath)) {
                    e.set(SdfFieldKeys->TypeName, VtValue(tk.xform));
                }
                if (path != _topPath) {
                    ensurePrim(path.GetParentPath(), SdfSpecifierDef, TfToken());
                }
            }
            if (!typeName.IsEmpty()) {
                e.set(SdfFieldKeys->TypeName, VtValue(typeName));
            }
        }

        return e;
    }
    const SdfPath parent = path.GetParentPath();
    SpecEntry& parentEntry = ensurePrim(parent, specifier == SdfSpecifierOver ? SdfSpecifierOver : SdfSpecifierDef, TfToken());
    parentEntry.primChildren.push_back(path.GetNameToken());

    SpecEntry e;
    e.type = SdfSpecTypePrim;
    e.set(SdfFieldKeys->Specifier, VtValue(specifier));
    if (!typeName.IsEmpty()) {
        e.set(SdfFieldKeys->TypeName, VtValue(typeName));
    } else if (specifier == SdfSpecifierDef && path.HasPrefix(_options.rootPath)) {
        e.set(SdfFieldKeys->TypeName, VtValue(tk.xform));
    }

    return _specs.emplace(path, std::move(e)).first->second;
}

GeoUsdConvert::SpecEntry&
GeoUsdConvert::addProperty(const SdfPath& primPath,
                           const PropertyPlan& plan)
{
    _specs[primPath].properties.push_back(plan.name);
    SpecEntry e;
    if (plan.relationship) {
        e.type = SdfSpecTypeRelationship;
        e.set(SdfFieldKeys->Custom, VtValue(false));
        e.set(SdfFieldKeys->Variability, VtValue(plan.variability));
        e.set(SdfFieldKeys->TargetPaths, VtValue(SdfPathListOp::CreateExplicit(plan.targets)));
    } else {
        e.type = SdfSpecTypeAttribute;
        e.set(SdfFieldKeys->TypeName, VtValue(plan.typeName.GetAsToken()));
        e.set(SdfFieldKeys->Custom, VtValue(false));
        e.set(SdfFieldKeys->Variability, VtValue(plan.variability));
        if (!plan.interpolation.IsEmpty()) {
            e.set(UsdGeomTokens->interpolation, VtValue(plan.interpolation));
        }
        if (plan.elementSize > 1) {
            e.set(UsdGeomTokens->elementSize, VtValue(plan.elementSize));
        }
        if (plan.source == eGeoUsdSourceStatic) {
            e.set(SdfFieldKeys->Default, plan.staticDefault);
        } else {
            e.lazyDefault = true;
        }
    }

    return _specs.emplace(primPath.AppendProperty(plan.name), std::move(e)).first->second;
}

void
GeoUsdConvert::planProperties(Partition& p)
{
    const GeoDetail& d = *_detail;
    const auto& n = SdfValueTypeNames;
    std::set<TfToken> used;

    auto builtin = [&](const TfToken& name, const SdfValueTypeName& type) {
        PropertyPlan plan;
        plan.name = name;
        plan.typeName = type;
        used.insert(name);
        p.properties.push_back(std::move(plan));
    };
    auto uniformToken = [&](const TfToken& name, const TfToken& value) {
        PropertyPlan plan;
        plan.name = name;
        plan.typeName = n->Token;
        plan.variability = SdfVariabilityUniform;
        plan.source = eGeoUsdSourceStatic;
        plan.staticDefault = VtValue(value);
        used.insert(name);
        p.properties.push_back(std::move(plan));
    };

    std::vector<std::pair<GeoAttribClassEnum, TfToken>> classes;
    switch (p.kind) {
    case eGeoUsdPrimMesh:
        builtin(UsdGeomTokens->points, n->Point3fArray);
        builtin(UsdGeomTokens->faceVertexCounts, n->IntArray);
        builtin(UsdGeomTokens->faceVertexIndices, n->IntArray);
        builtin(UsdGeomTokens->extent, n->Float3Array);
        uniformToken(UsdGeomTokens->subdivisionScheme, UsdGeomTokens->none);
        classes = { { eGeoAttribClassDetail, UsdGeomTokens->constant },
                    { eGeoAttribClassPrimitive, UsdGeomTokens->uniform },
                    { eGeoAttribClassPoint, UsdGeomTokens->vertex },
                    { eGeoAttribClassVertex, UsdGeomTokens->faceVarying } };
        break;
    case eGeoUsdPrimCurves:
        builtin(UsdGeomTokens->points, n->Point3fArray);
        builtin(UsdGeomTokens->curveVertexCounts, n->IntArray);
        builtin(UsdGeomTokens->extent, n->Float3Array);
        uniformToken(UsdGeomTokens->type, UsdGeomTokens->linear);
        uniformToken(UsdGeomTokens->wrap, UsdGeomTokens->nonperiodic);
        classes = { { eGeoAttribClassDetail, UsdGeomTokens->constant },
                    { eGeoAttribClassPrimitive, UsdGeomTokens->uniform },
                    { eGeoAttribClassPoint, UsdGeomTokens->vertex },
                    { eGeoAttribClassVertex, UsdGeomTokens->vertex } };
        break;
    case eGeoUsdPrimPoints:
        builtin(UsdGeomTokens->points, n->Point3fArray);
        builtin(UsdGeomTokens->extent, n->Float3Array);
        classes = { { eGeoAttribClassDetail, UsdGeomTokens->constant },
                    { eGeoAttribClassPoint, UsdGeomTokens->vertex } };
        break;
    case eGeoUsdPrimInstancer: {
        builtin(UsdGeomTokens->protoIndices, n->IntArray);
        builtin(UsdGeomTokens->positions, n->Point3fArray);
        builtin(UsdGeomTokens->orientations, n->QuathArray);
        builtin(UsdGeomTokens->scales, n->Float3Array);
        PropertyPlan rel;
        rel.name = UsdGeomTokens->prototypes;
        rel.relationship = true;
        for (const auto& proto : p.prototypes) {
            rel.targets.push_back(proto->_options.rootPath);
        }
        p.properties.push_back(std::move(rel));

        return;
    }
    case eGeoUsdPrimNativeInstances:
        return;
    case eGeoUsdPrimOverride: {
        builtin(tokens().xformOpTransform, n->Matrix4d);
        PropertyPlan order;
        order.name = UsdGeomTokens->xformOpOrder;
        order.typeName = n->TokenArray;
        order.variability = SdfVariabilityUniform;
        order.source = eGeoUsdSourceStatic;
        // The over replaces the input's op order, which would otherwise lose its reset.
        const GeoUsdInputPrim* input = inputPrim(p.path);
        if (input && input->resetsXformStack) {
            order.staticDefault = VtValue(VtTokenArray { UsdGeomXformOpTypes->resetXformStack, tokens().xformOpTransform });
        } else {
            order.staticDefault = VtValue(VtTokenArray { tokens().xformOpTransform });
        }
        p.properties.push_back(std::move(order));

        return;
    }
    }

    const bool widthsAllowed = p.kind == eGeoUsdPrimCurves || p.kind == eGeoUsdPrimPoints;
    for (const auto& [cls, interpolation] : classes) {
        for (std::size_t i = 0; i < d.attributeCount(cls); ++i) {
            const GeoAttribute& a = d.attribute(cls, i);
            const std::string& name = a.name();
            if (cls == eGeoAttribClassPoint && name == GeoDetail::kPositionName) {
                continue;
            }
            if (cls == eGeoAttribClassPrimitive && (name == "path" || name == "name" || isPackedIntrinsic(name))) {
                continue;
            }
            if ((cls == eGeoAttribClassPoint && name == "path") || (cls == eGeoAttribClassDetail && name == kGeoUsdWorldSpaceAttr) || ((cls == eGeoAttribClassPoint || cls == eGeoAttribClassPrimitive) && name == kGeoUsdSourceAttr)) {
                continue;
            }
            if (a.isArray()) {
                _warnings.push_back("The array attribute " + name + " is not exported");
                continue;
            }
            PropertyPlan plan;
            plan.source = eGeoUsdSourceAttribute;
            plan.cls = cls;
            plan.attribute = name;
            plan.interpolation = interpolation;
            const bool f32 = a.storage() == eGeoStorageFloat32;
            if (name == "N" && f32 && a.tupleSize() == 3) {
                plan.name = UsdGeomTokens->normals;
                plan.typeName = n->Normal3fArray;
                plan.type = findExportType(eGeoStorageFloat32, 3, eGeoTypeInfoNormal);
            } else if (name == "uv" && f32 && (a.tupleSize() == 2 || a.tupleSize() == 3)) {
                plan.name = TfToken("primvars:st");
                plan.typeName = n->TexCoord2fArray;
                plan.type = findExportType(eGeoStorageFloat32, 2, eGeoTypeInfoNone);
                plan.toTexCoord2 = a.tupleSize() == 3;
            } else if (name == "Cd" && f32 && a.tupleSize() == 3) {
                plan.name = TfToken("primvars:displayColor");
                plan.typeName = n->Color3fArray;
                plan.type = findExportType(eGeoStorageFloat32, 3, eGeoTypeInfoColour);
            } else if (name == "pscale" && f32 && a.tupleSize() == 1 && widthsAllowed) {
                plan.name = UsdGeomTokens->widths;
                plan.typeName = n->FloatArray;
                plan.type = findExportType(eGeoStorageFloat32, 1, eGeoTypeInfoNone);
            } else {
                plan.name = TfToken("primvars:" + TfMakeValidIdentifier(name));
                if (a.storage() == eGeoStorageString) {
                    if (a.tupleSize() != 1) {
                        _warnings.push_back("The string attribute " + name + " has several components and is not exported");
                        continue;
                    }
                    plan.typeName = n->StringArray;
                } else {
                    plan.type = findExportType(a.storage(), a.tupleSize(), a.typeInfo());
                    if (!plan.type) {
                        plan.type = findExportType(a.storage(), 1, eGeoTypeInfoNone);
                        plan.elementSize = a.tupleSize();
                    }
                    if (!plan.type) {
                        _warnings.push_back("The attribute " + name + " has no USD type and is not exported");
                        continue;
                    }
                    plan.typeName = plan.type->scalar.GetArrayType();
                }
            }
            if (!used.insert(plan.name).second) {
                _warnings.push_back("The attribute " + name + " maps to " + plan.name.GetString() + ", which another attribute already fills; it is not exported");
                continue;
            }
            p.properties.push_back(std::move(plan));
        }
    }

    auto groupPlans = [&](GeoAttribClassEnum cls, const TfToken& interpolation) {
        for (const std::string& g : d.groupNames(cls)) {
            PropertyPlan plan;
            plan.source = eGeoUsdSourceGroup;
            plan.cls = cls;
            plan.attribute = g;
            plan.interpolation = interpolation;
            plan.name = TfToken("primvars:" + TfMakeValidIdentifier(g));
            plan.typeName = n->BoolArray;
            if (!used.insert(plan.name).second) {
                _warnings.push_back("The group " + g + " maps to " + plan.name.GetString() + ", which an attribute already fills; it is not exported");
                continue;
            }
            p.properties.push_back(std::move(plan));
        }
    };
    groupPlans(eGeoAttribClassPoint, UsdGeomTokens->vertex);
    if (p.kind == eGeoUsdPrimCurves) {
        groupPlans(eGeoAttribClassPrimitive, UsdGeomTokens->uniform);
    } else if (p.kind == eGeoUsdPrimMesh) {
        for (const std::string& g : d.groupNames(eGeoAttribClassPrimitive)) {
            const GeoElementGroup* group = d.findGroup(eGeoAttribClassPrimitive, g);
            bool any = false;
            for (std::size_t off : p.primitives) {
                if (group->contains(GeoOffset(off))) {
                    any = true;
                    break;
                }
            }
            if (any) {
                p.subsets.emplace_back(TfToken(TfMakeValidIdentifier(g)), g);
            }
        }
    }
}

void
GeoUsdConvert::trimInputOverrides(Partition& p)
{
    if (!p.sameTypeOnInput || (p.kind != eGeoUsdPrimMesh && p.kind != eGeoUsdPrimCurves && p.kind != eGeoUsdPrimPoints)) {
        return;
    }
    const GeoUsdInputPrim& input = *inputPrim(p.path);
    auto values = std::make_shared<GeoUsdValueMap>();
    buildValues(p, *values);

    const auto inputValue = [&](const TfToken& name) -> const VtValue* {
        const auto it = input.values.find(name);

        return it == input.values.end() ? nullptr : &it->second;
    };
    const auto unchanged = [&](const TfToken& name) {
        const auto it = values->find(p.path.AppendProperty(name));
        const VtValue* before = inputValue(name);

        return it != values->end() && before && *before == it->second;
    };
    const TfToken topologyNames[] = { UsdGeomTokens->faceVertexCounts, UsdGeomTokens->faceVertexIndices, UsdGeomTokens->curveVertexCounts };
    const auto isTopology = [&](const TfToken& name) {
        return std::find(std::begin(topologyNames), std::end(topologyNames), name) != std::end(topologyNames);
    };
    bool sameTopology = true;
    for (const PropertyPlan& plan : p.properties) {
        if (isTopology(plan.name) && !unchanged(plan.name)) {
            sameTopology = false;
        }
    }

    std::vector<PropertyPlan> kept;
    std::vector<PropertyPlan> blocks;
    for (PropertyPlan& plan : p.properties) {
        bool keep;
        if (plan.name == UsdGeomTokens->points) {
            keep = true;
        } else if (isTopology(plan.name) || plan.source == eGeoUsdSourceStatic) {
            keep = !sameTopology;
        } else {
            keep = !unchanged(plan.name);
        }
        if (!keep) {
            values->erase(p.path.AppendProperty(plan.name));
            continue;
        }
        // The input's indices would otherwise re-index the flattened values written here.
        const TfToken indices(plan.name.GetString() + ":indices");
        if (!plan.relationship && inputValue(indices)) {
            PropertyPlan block;
            block.name = indices;
            block.typeName = SdfValueTypeNames->IntArray;
            block.source = eGeoUsdSourceStatic;
            block.staticDefault = VtValue(SdfValueBlock());
            blocks.push_back(std::move(block));
        }
        kept.push_back(std::move(plan));
    }
    for (PropertyPlan& block : blocks) {
        kept.push_back(std::move(block));
    }
    p.properties = std::move(kept);
    if (sameTopology) {
        p.subsets.clear();
    }
    p.values = values;
}

const GeoUsdInputPrim*
GeoUsdConvert::inputPrim(const SdfPath& path) const
{
    return _options.inputScene ? _options.inputScene->find(path) : nullptr;
}

GfMatrix4d
GeoUsdConvert::frameOf(const SdfPath& path) const
{
    for (SdfPath at = path; !at.IsEmpty() && at != SdfPath::AbsoluteRootPath(); at = at.GetParentPath()) {
        if (const GeoUsdInputPrim* input = inputPrim(at)) {
            return input->world;
        }
    }

    return GfMatrix4d(1.);
}

GfMatrix4d
GeoUsdConvert::overrideParent(const Partition& p) const
{
    if (const GeoUsdInputPrim* input = inputPrim(p.path)) {
        return input->parentWorld;
    }

    return geoUsdMatrix(geoPackedUSDParentTransform(*_detail, GeoOffset(p.primitives.front())));
}

void
GeoUsdConvert::addPartitionSpecs(std::size_t index)
{
    const Tokens& tk = tokens();
    Partition& p = *_partitions[index];
    const std::int32_t pi = std::int32_t(index);

    TfToken typeName;
    SdfSpecifier specifier = SdfSpecifierDef;
    switch (p.kind) {
    case eGeoUsdPrimMesh:
        typeName = tk.mesh;
        break;
    case eGeoUsdPrimCurves:
        typeName = tk.basisCurves;
        break;
    case eGeoUsdPrimPoints:
        typeName = tk.points;
        break;
    case eGeoUsdPrimInstancer:
        typeName = tk.pointInstancer;
        break;
    case eGeoUsdPrimNativeInstances:
        typeName = tk.xform;
        break;
    case eGeoUsdPrimOverride:
        specifier = SdfSpecifierOver;
        break;
    }
    if (p.onInput) {
        specifier = SdfSpecifierOver;
        if (p.sameTypeOnInput) {
            typeName = TfToken();
        }
    }
    ensurePrim(p.path, specifier, typeName).partition = pi;
    for (const PropertyPlan& plan : p.properties) {
        addProperty(p.path, plan).partition = pi;
    }

    for (const auto& [name, group] : p.subsets) {
        const SdfPath subsetPath = p.path.AppendChild(name);
        ensurePrim(subsetPath, SdfSpecifierDef, tk.geomSubset).partition = pi;
        PropertyPlan elementType;
        elementType.name = UsdGeomTokens->elementType;
        elementType.typeName = SdfValueTypeNames->Token;
        elementType.variability = SdfVariabilityUniform;
        elementType.source = eGeoUsdSourceStatic;
        elementType.staticDefault = VtValue(UsdGeomTokens->face);
        addProperty(subsetPath, elementType).partition = pi;
        PropertyPlan indices;
        indices.name = UsdGeomTokens->indices;
        indices.typeName = SdfValueTypeNames->IntArray;
        addProperty(subsetPath, indices).partition = pi;
    }

    if (p.prototypes.empty()) {
        return;
    }
    const SdfPath container = p.path.AppendChild(tk.prototypes);
    // Native instances keep their prototypes under a class so they are not drawn in place;
    // a point instancer's prototypes are only drawn through the instancer.
    const bool asClass = p.kind == eGeoUsdPrimNativeInstances;
    SpecEntry& containerEntry = ensurePrim(container, SdfSpecifierDef, asClass ? TfToken() : tk.scope);
    containerEntry.partition = pi;
    if (asClass) {
        containerEntry.set(SdfFieldKeys->Specifier, VtValue(SdfSpecifierClass));
        containerEntry.fields.erase(std::remove_if(containerEntry.fields.begin(), containerEntry.fields.end(),
                                                   [](const auto& f) { return f.first == SdfFieldKeys->TypeName; }),
                                    containerEntry.fields.end());
    }
    for (std::size_t k = 0; k < p.prototypes.size(); ++k) {
        const GeoUsdConvert& proto = *p.prototypes[k];
        _specs[container].primChildren.push_back(proto._topPath.GetNameToken());
        for (const auto& [path, entry] : proto._specs) {
            SpecEntry marker;
            marker.type = entry.type;
            marker.partition = pi;
            marker.prototype = std::int32_t(k);
            _specs[path] = std::move(marker);
        }
    }

    if (p.kind != eGeoUsdPrimNativeInstances) {
        return;
    }
    for (std::size_t i = 0; i < p.primitives.size(); ++i) {
        const SdfPath instancePath = p.path.AppendChild(TfToken("instance_" + std::to_string(i)));
        SpecEntry& instance = ensurePrim(instancePath, SdfSpecifierDef, tk.xform);
        instance.partition = pi;
        instance.set(SdfFieldKeys->Instanceable, VtValue(true));
        SdfReferenceListOp refs;
        refs.SetPrependedItems({ SdfReference(std::string(), p.prototypes[std::size_t(p.instancePrototype[i])]->_topPath) });
        instance.set(SdfFieldKeys->References, VtValue(refs));
        PropertyPlan xform;
        xform.name = tk.xformOpTransform;
        xform.typeName = SdfValueTypeNames->Matrix4d;
        addProperty(instancePath, xform).partition = pi;
        PropertyPlan order;
        order.name = UsdGeomTokens->xformOpOrder;
        order.typeName = SdfValueTypeNames->TokenArray;
        order.variability = SdfVariabilityUniform;
        order.source = eGeoUsdSourceStatic;
        order.staticDefault = VtValue(VtTokenArray { tk.xformOpTransform });
        addProperty(instancePath, order).partition = pi;
    }
}

void
GeoUsdConvert::finishSpecs()
{
    for (auto& [path, e] : _specs) {
        if (e.prototype >= 0) {
            continue;
        }
        if (!e.primChildren.empty()) {
            e.set(SdfChildrenKeys->PrimChildren, VtValue(e.primChildren));
        }
        if (!e.properties.empty()) {
            e.set(SdfChildrenKeys->PropertyChildren, VtValue(e.properties));
        }
    }
}

const GeoUsdConvert::SpecEntry*
GeoUsdConvert::findEntry(const SdfPath& path) const
{
    const auto it = _specs.find(path);

    return it == _specs.end() ? nullptr : &it->second;
}

const GeoUsdConvert*
GeoUsdConvert::delegateFor(const SpecEntry& entry) const
{
    return _partitions[std::size_t(entry.partition)]->prototypes[std::size_t(entry.prototype)].get();
}

std::size_t
GeoUsdConvert::partitionCount() const
{
    return _partitions.size();
}

const SdfPath&
GeoUsdConvert::partitionPath(std::size_t partition) const
{
    return _partitions.at(partition)->path;
}

GeoUsdPrimKindEnum
GeoUsdConvert::partitionKind(std::size_t partition) const
{
    return _partitions.at(partition)->kind;
}

bool
GeoUsdConvert::isPartitionBuilt(std::size_t partition) const
{
    const Partition& p = *_partitions.at(partition);
    std::lock_guard<std::mutex> lock(p.mutex);

    return p.values != nullptr;
}

U64
GeoUsdConvert::partitionSignature(std::size_t partition) const
{
    const Partition& p = *_partitions.at(partition);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.hasSignature) {
            return p.signature;
        }
    }
    const U64 s = computeSignature(p);
    std::lock_guard<std::mutex> lock(p.mutex);
    p.signature = s;
    p.hasSignature = true;

    return s;
}

U64
GeoUsdConvert::computeSignature(const Partition& p) const
{
    const GeoDetail& d = *_detail;
    Hash64 h;
    h.append(U64(p.kind));
    h.append(U64(SdfPath::Hash()(p.path)));
    h.append(U64(p.primitives.size()));
    h.append(U64(p.points.size()));

    std::vector<std::size_t> vertices;
    std::vector<std::size_t> pointOffsets = p.points;
    for (std::size_t off : p.primitives) {
        h.append(U64(off));
        const GeoVertexRange r = d.primitiveVertices(GeoOffset(off));
        for (GeoOffset v = r.start; v < r.start + r.count; ++v) {
            vertices.push_back(std::size_t(v));
            pointOffsets.push_back(std::size_t(d.vertexPoint(v)));
        }
    }
    for (std::size_t off : p.points) {
        h.append(U64(off));
    }
    const std::vector<std::size_t> primPages = pagesOf(p.primitives);
    const std::vector<std::size_t> vertexPages = pagesOf(vertices);
    const std::vector<std::size_t> pointPages = pagesOf(pointOffsets);

    const GeoTopology& topo = d.topology();
    hashPages(h, topo.primitiveTypeColumn(), primPages);
    hashPages(h, topo.primitiveVertexStartColumn(), primPages);
    hashPages(h, topo.primitiveVertexCountColumn(), primPages);
    hashPages(h, topo.vertexPointColumn(), vertexPages);
    const std::pair<GeoAttribClassEnum, const std::vector<std::size_t>*> classes[] = {
        { eGeoAttribClassPoint, &pointPages },
        { eGeoAttribClassVertex, &vertexPages },
        { eGeoAttribClassPrimitive, &primPages },
    };
    for (const auto& [cls, pages] : classes) {
        for (std::size_t i = 0; i < d.attributeCount(cls); ++i) {
            const GeoAttribute& a = d.attribute(cls, i);
            h.append(U64(std::hash<std::string>()(a.name())));
            h.append(U64(a.storage()));
            h.append(U64(a.tupleSize()));
            h.append(U64(a.typeInfo()));
            h.append(U64(a.isArray()));
            hashPages(h, a, *pages);
        }
        if (cls != eGeoAttribClassVertex) {
            for (const std::string& g : d.groupNames(cls)) {
                h.append(U64(std::hash<std::string>()(g)));
                h.append(d.findGroup(cls, g)->contentHash());
            }
        }
    }
    for (std::size_t i = 0; i < d.attributeCount(eGeoAttribClassDetail); ++i) {
        const GeoAttribute& a = d.attribute(eGeoAttribClassDetail, i);
        h.append(U64(std::hash<std::string>()(a.name())));
        h.append(a.contentHash());
    }
    for (const std::string& g : d.groupNames(eGeoAttribClassVertex)) {
        h.append(U64(std::hash<std::string>()(g)));
    }
    for (const GeoDetailConstPtr& proto : p.prototypeDetails) {
        h.append(proto ? proto->contentHash() : U64(0));
    }
    if (const GeoUsdInputPrim* input = inputPrim(p.path)) {
        h.append(input->hash);
    }
    if (p.kind == eGeoUsdPrimOverride) {
        hashMatrix(h, overrideParent(p));
    } else {
        hashMatrix(h, frameOf(p.path));
    }
    h.computeHash();

    return h.value();
}

std::size_t
GeoUsdConvert::adoptUnchangedPartitions(const GeoUsdConvert& previous)
{
    std::map<SdfPath, std::size_t> before;
    for (std::size_t i = 0; i < previous._partitions.size(); ++i) {
        before.emplace(previous._partitions[i]->path, i);
    }
    std::size_t adopted = 0;
    for (std::size_t i = 0; i < _partitions.size(); ++i) {
        Partition& p = *_partitions[i];
        const auto it = before.find(p.path);
        if (it == before.end()) {
            continue;
        }
        const Partition& old = *previous._partitions[it->second];
        if (old.kind != p.kind || old.prototypes.size() != p.prototypes.size()) {
            continue;
        }
        std::shared_ptr<const GeoUsdValueMap> values;
        {
            std::lock_guard<std::mutex> lock(old.mutex);
            values = old.values;
        }
        if (!values || previous.partitionSignature(it->second) != partitionSignature(i)) {
            continue;
        }
        std::lock_guard<std::mutex> lock(p.mutex);
        p.values = values;
        p.prototypes = old.prototypes;
        ++adopted;
    }

    return adopted;
}

std::shared_ptr<const GeoUsdValueMap>
GeoUsdConvert::partitionValues(std::size_t partition) const
{
    const Partition& p = *_partitions[partition];
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.values) {
        auto values = std::make_shared<GeoUsdValueMap>();
        buildValues(p, *values);
        p.values = values;
    }

    return p.values;
}

void
GeoUsdConvert::buildValues(const Partition& p,
                           GeoUsdValueMap& values) const
{
    const Tokens& tk = tokens();
    const GeoDetail& d = *_detail;

    if (p.kind == eGeoUsdPrimOverride || p.kind == eGeoUsdPrimNativeInstances || p.kind == eGeoUsdPrimInstancer) {
        if (p.kind == eGeoUsdPrimOverride) {
            const GfMatrix4d world = geoUsdMatrix(geoPackedTransform(d, GeoOffset(p.primitives.front())));
            values[p.path.AppendProperty(tk.xformOpTransform)] = VtValue(relativeTo(world, overrideParent(p)));

            return;
        }
        const GfMatrix4d frame = frameOf(p.path);
        if (p.kind == eGeoUsdPrimNativeInstances) {
            for (std::size_t i = 0; i < p.primitives.size(); ++i) {
                const SdfPath instancePath = p.path.AppendChild(TfToken("instance_" + std::to_string(i)));
                const GfMatrix4d world = geoUsdMatrix(geoPackedTransform(d, GeoOffset(p.primitives[i])));
                values[instancePath.AppendProperty(tk.xformOpTransform)] = VtValue(relativeTo(world, frame));
            }

            return;
        }
        const std::size_t n = p.primitives.size();
        VtIntArray protoIndices(n);
        VtVec3fArray positions(n);
        VtQuathArray orientations(n);
        VtVec3fArray scales(n);
        for (std::size_t i = 0; i < n; ++i) {
            const GfMatrix4d m = relativeTo(geoUsdMatrix(geoPackedTransform(d, GeoOffset(p.primitives[i]))), frame);
            protoIndices[i] = p.instancePrototype[i];
            decomposeInstance(m, &scales[i], &orientations[i], &positions[i]);
        }
        values[p.path.AppendProperty(UsdGeomTokens->protoIndices)] = VtValue(protoIndices);
        values[p.path.AppendProperty(UsdGeomTokens->positions)] = VtValue(positions);
        values[p.path.AppendProperty(UsdGeomTokens->orientations)] = VtValue(orientations);
        values[p.path.AppendProperty(UsdGeomTokens->scales)] = VtValue(scales);

        return;
    }

    std::vector<std::size_t> vertices;
    VtIntArray counts;
    for (std::size_t off : p.primitives) {
        const GeoVertexRange r = d.primitiveVertices(GeoOffset(off));
        counts.push_back(int(r.count));
        for (GeoOffset v = r.start; v < r.start + r.count; ++v) {
            vertices.push_back(std::size_t(v));
        }
    }

    // The offset behind each USD point.
    std::vector<std::size_t> pointElems;
    if (p.kind == eGeoUsdPrimPoints) {
        pointElems = p.points;
    } else if (p.kind == eGeoUsdPrimCurves) {
        pointElems.reserve(vertices.size());
        for (std::size_t v : vertices) {
            pointElems.push_back(std::size_t(d.vertexPoint(GeoOffset(v))));
        }
    } else {
        pointElems.reserve(vertices.size());
        for (std::size_t v : vertices) {
            pointElems.push_back(std::size_t(d.vertexPoint(GeoOffset(v))));
        }
        std::sort(pointElems.begin(), pointElems.end());
        pointElems.erase(std::unique(pointElems.begin(), pointElems.end()), pointElems.end());
        VtIntArray indices(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t pt = std::size_t(d.vertexPoint(GeoOffset(vertices[i])));
            indices[i] = int(std::lower_bound(pointElems.begin(), pointElems.end(), pt) - pointElems.begin());
        }
        values[p.path.AppendProperty(UsdGeomTokens->faceVertexIndices)] = VtValue(indices);
    }

    VtVec3fArray points = gatherPositions(d, pointElems);
    const bool worldSpace = isWorldSpace(d);
    const GfMatrix4d toPrim = worldSpace ? relativeTo(GfMatrix4d(1.), frameOf(p.path)) : GfMatrix4d(1.);
    if (worldSpace) {
        for (GfVec3f& point : points) {
            point = GfVec3f(toPrim.Transform(GfVec3d(point)));
        }
    }
    values[p.path.AppendProperty(UsdGeomTokens->points)] = VtValue(points);
    values[p.path.AppendProperty(UsdGeomTokens->extent)] = VtValue(extentOf(points));
    if (p.kind == eGeoUsdPrimMesh) {
        values[p.path.AppendProperty(UsdGeomTokens->faceVertexCounts)] = VtValue(counts);
    } else if (p.kind == eGeoUsdPrimCurves) {
        values[p.path.AppendProperty(UsdGeomTokens->curveVertexCounts)] = VtValue(counts);
    }

    const std::vector<std::size_t> detailElems { 0 };
    auto elementsOf = [&](GeoAttribClassEnum cls, const TfToken& interpolation) -> const std::vector<std::size_t>& {
        switch (cls) {
        case eGeoAttribClassPoint:
            return pointElems;
        case eGeoAttribClassVertex:
            // Curves carry vertex attributes per USD point, which is per vertex.
            (void)interpolation;
            return vertices;
        case eGeoAttribClassPrimitive:
            return p.primitives;
        case eGeoAttribClassDetail:
            break;
        }

        return detailElems;
    };

    for (const PropertyPlan& plan : p.properties) {
        const SdfPath path = p.path.AppendProperty(plan.name);
        if (plan.source == eGeoUsdSourceGroup) {
            const GeoElementGroup* group = d.findGroup(plan.cls, plan.attribute);
            const std::vector<std::size_t>& elems = elementsOf(plan.cls, plan.interpolation);
            VtBoolArray flags(elems.size());
            for (std::size_t i = 0; i < elems.size(); ++i) {
                flags[i] = group && group->contains(GeoOffset(elems[i]));
            }
            values[path] = VtValue(flags);
            continue;
        }
        if (plan.source != eGeoUsdSourceAttribute) {
            continue;
        }
        const GeoAttribute* a = d.findAttribute(plan.cls, plan.attribute);
        if (!a) {
            continue;
        }
        const std::vector<std::size_t>& elems = elementsOf(plan.cls, plan.interpolation);
        if (a->storage() == eGeoStorageString) {
            VtStringArray strings(elems.size());
            for (std::size_t i = 0; i < elems.size(); ++i) {
                strings[i] = std::string(a->getString(elems[i]));
            }
            values[path] = VtValue(strings);
            continue;
        }
        std::vector<std::uint64_t> flat = gatherComponents(*a, elems);
        const std::size_t tuple = std::size_t(a->tupleSize());
        if (plan.toTexCoord2) {
            float* f = reinterpret_cast<float*>(flat.data());
            for (std::size_t i = 0; i < elems.size(); ++i) {
                f[i * 2] = f[i * tuple];
                f[i * 2 + 1] = f[i * tuple + 1];
            }
        }
        if (plan.type->typeInfo == eGeoTypeInfoMatrix && plan.type->tupleSize == 16) {
            double* m = reinterpret_cast<double*>(flat.data());
            for (std::size_t i = 0; i < elems.size(); ++i) {
                transpose16(m + i * 16);
            }
        }
        if (worldSpace && !plan.toTexCoord2 && (plan.elementSize <= 1) && (tuple == 3) && isDirection(plan.type->typeInfo)) {
            carryDirections(flat.data(), a->storage(), elems.size(), directionMatrix(toPrim, plan.type->typeInfo), plan.type->typeInfo);
        }
        const std::size_t scalars = elems.size() * (plan.toTexCoord2 ? 2 : tuple);
        const std::size_t count = scalars * storageBytes(a->storage()) / plan.type->elementBytes;
        values[path] = plan.type->pack(flat.data(), count);
    }

    for (const auto& [name, groupName] : p.subsets) {
        const GeoElementGroup* group = d.findGroup(eGeoAttribClassPrimitive, groupName);
        VtIntArray indices;
        for (std::size_t i = 0; i < p.primitives.size(); ++i) {
            if (group && group->contains(GeoOffset(p.primitives[i]))) {
                indices.push_back(int(i));
            }
        }
        values[p.path.AppendChild(name).AppendProperty(UsdGeomTokens->indices)] = VtValue(indices);
    }
}

int
GeoUsdConvert::specPartition(const SdfPath& path) const
{
    const SpecEntry* e = findEntry(path);

    return e ? int(e->partition) : -1;
}

bool
GeoUsdConvert::hasSpec(const SdfPath& path) const
{
    return findEntry(path) != nullptr;
}

SdfSpecType
GeoUsdConvert::specType(const SdfPath& path) const
{
    const SpecEntry* e = findEntry(path);

    return e ? e->type : SdfSpecTypeUnknown;
}

bool
GeoUsdConvert::field(const SdfPath& path,
                     const TfToken& fieldName,
                     VtValue* value) const
{
    const SpecEntry* e = findEntry(path);
    if (!e) {
        return false;
    }
    if (e->prototype >= 0) {
        return delegateFor(*e)->field(path, fieldName, value);
    }
    if (const VtValue* v = e->get(fieldName)) {
        if (value) {
            *value = *v;
        }

        return true;
    }
    if (!e->lazyDefault || fieldName != SdfFieldKeys->Default) {
        return false;
    }
    const std::shared_ptr<const GeoUsdValueMap> values = partitionValues(std::size_t(e->partition));
    const auto it = values->find(path);
    if (it == values->end()) {
        return false;
    }
    if (value) {
        *value = it->second;
    }

    return true;
}

std::vector<TfToken>
GeoUsdConvert::fieldNames(const SdfPath& path) const
{
    std::vector<TfToken> names;
    const SpecEntry* e = findEntry(path);
    if (!e) {
        return names;
    }
    if (e->prototype >= 0) {
        return delegateFor(*e)->fieldNames(path);
    }
    names.reserve(e->fields.size() + 1);
    for (const auto& f : e->fields) {
        names.push_back(f.first);
    }
    if (e->lazyDefault) {
        names.push_back(SdfFieldKeys->Default);
    }

    return names;
}

void
GeoUsdConvert::visitPrim(const SdfPath& path,
                         const std::function<bool(const SdfPath&)>& visitor,
                         bool* stop) const
{
    const SpecEntry* e = findEntry(path);
    if (!e || *stop) {
        return;
    }
    if (e->prototype >= 0) {
        delegateFor(*e)->visitPrim(path, visitor, stop);

        return;
    }
    if (!visitor(path)) {
        *stop = true;

        return;
    }
    for (const TfToken& prop : e->properties) {
        if (!visitor(path.AppendProperty(prop))) {
            *stop = true;

            return;
        }
    }
    for (const TfToken& child : e->primChildren) {
        visitPrim(path.AppendChild(child), visitor, stop);
        if (*stop) {
            return;
        }
    }
}

void
GeoUsdConvert::visitSpecs(const std::function<bool(const SdfPath&)>& visitor) const
{
    bool stop = false;
    const SpecEntry* top = findEntry(_topPath);
    if (!top) {
        return;
    }
    if (top->type != SdfSpecTypePseudoRoot) {
        visitPrim(_topPath, visitor, &stop);

        return;
    }
    if (!visitor(_topPath)) {
        return;
    }
    for (const TfToken& child : top->primChildren) {
        visitPrim(_topPath.AppendChild(child), visitor, &stop);
        if (stop) {
            return;
        }
    }
}

bool
GeoUsdConvert::author(const SdfLayerHandle& layer,
                      UsdTimeCode time,
                      std::string* errorMessage) const
{
    if (!layer) {
        if (errorMessage) {
            *errorMessage = "No layer to author geometry into";
        }

        return false;
    }
    bool ok = true;
    SdfChangeBlock block;
    visitSpecs([&](const SdfPath& path) {
        ok = authorSpec(layer, path, time, errorMessage);

        return ok;
    });

    return ok;
}

bool
GeoUsdConvert::authorSpec(const SdfLayerHandle& layer,
                          const SdfPath& path,
                          UsdTimeCode time,
                          std::string* errorMessage) const
{
    const SdfSpecType type = specType(path);
    if (type == SdfSpecTypePseudoRoot) {
        return true;
    }
    SdfVariability variability = SdfVariabilityVarying;
    VtValue v;
    if (field(path, SdfFieldKeys->Variability, &v) && v.IsHolding<SdfVariability>()) {
        variability = v.UncheckedGet<SdfVariability>();
    }
    bool created = false;
    if (type == SdfSpecTypePrim) {
        created = SdfJustCreatePrimInLayer(layer, path);
    } else if (type == SdfSpecTypeAttribute) {
        VtValue typeName;
        field(path, SdfFieldKeys->TypeName, &typeName);
        const SdfValueTypeName valueType = SdfSchema::GetInstance().FindType(typeName.GetWithDefault<TfToken>());
        created = valueType && SdfJustCreatePrimAttributeInLayer(layer, path, valueType, variability, false);
    } else if (type == SdfSpecTypeRelationship) {
        created = SdfJustCreateRelationshipInLayer(layer, path, variability, false);
    }
    if (!created) {
        if (errorMessage) {
            *errorMessage = "Could not author " + path.GetString();
        }

        return false;
    }
    for (const TfToken& name : fieldNames(path)) {
        if (name == SdfChildrenKeys->PrimChildren || name == SdfChildrenKeys->PropertyChildren) {
            continue;
        }
        VtValue value;
        if (!field(path, name, &value)) {
            continue;
        }
        if (name == SdfFieldKeys->Default && !time.IsDefault() && variability == SdfVariabilityVarying) {
            layer->SetTimeSample(path, time.GetValue(), value);
        } else {
            layer->SetField(path, name, value);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------------------------
// Unpacking

namespace {

void
copyAcross(const GeoAttribute& src,
           std::size_t srcOffset,
           GeoAttribute& dst,
           std::size_t dstOffset)
{
    const int tuple = src.tupleSize();
    if (src.isArray()) {
        switch (src.storage()) {
        case eGeoStorageInt32: {
            std::vector<std::int32_t> v;
            src.getArray<std::int32_t>(srcOffset, v);
            dst.setArray<std::int32_t>(dstOffset, v);
            break;
        }
        case eGeoStorageInt64: {
            std::vector<std::int64_t> v;
            src.getArray<std::int64_t>(srcOffset, v);
            dst.setArray<std::int64_t>(dstOffset, v);
            break;
        }
        case eGeoStorageFloat32: {
            std::vector<float> v;
            src.getArray<float>(srcOffset, v);
            dst.setArray<float>(dstOffset, v);
            break;
        }
        case eGeoStorageFloat64: {
            std::vector<double> v;
            src.getArray<double>(srcOffset, v);
            dst.setArray<double>(dstOffset, v);
            break;
        }
        case eGeoStorageString: {
            std::vector<std::string> v;
            src.getStringArray(srcOffset, v);
            dst.setStringArray(dstOffset, v);
            break;
        }
        }

        return;
    }
    switch (src.storage()) {
    case eGeoStorageInt32: {
        std::int32_t buf[GeoAttribute::kMaxTupleSize];
        src.getTuple<std::int32_t>(srcOffset, std::span<std::int32_t>(buf, std::size_t(tuple)));
        dst.setTuple<std::int32_t>(dstOffset, std::span<const std::int32_t>(buf, std::size_t(tuple)));
        break;
    }
    case eGeoStorageInt64: {
        std::int64_t buf[GeoAttribute::kMaxTupleSize];
        src.getTuple<std::int64_t>(srcOffset, std::span<std::int64_t>(buf, std::size_t(tuple)));
        dst.setTuple<std::int64_t>(dstOffset, std::span<const std::int64_t>(buf, std::size_t(tuple)));
        break;
    }
    case eGeoStorageFloat32: {
        float buf[GeoAttribute::kMaxTupleSize];
        src.getTuple<float>(srcOffset, std::span<float>(buf, std::size_t(tuple)));
        dst.setTuple<float>(dstOffset, std::span<const float>(buf, std::size_t(tuple)));
        break;
    }
    case eGeoStorageFloat64: {
        double buf[GeoAttribute::kMaxTupleSize];
        src.getTuple<double>(srcOffset, std::span<double>(buf, std::size_t(tuple)));
        dst.setTuple<double>(dstOffset, std::span<const double>(buf, std::size_t(tuple)));
        break;
    }
    case eGeoStorageString:
        for (int c = 0; c < tuple; ++c) {
            dst.setString(dstOffset, src.getString(srcOffset, c), c);
        }
        break;
    }
}

// Normals take the inverse transpose of the linear part, vectors the linear part itself.
void
transformDirections(GeoAttribute& a,
                    const std::vector<std::size_t>& offsets,
                    const GeoMatrix4& m)
{
    if (a.tupleSize() != 3 || a.isArray() || (a.typeInfo() != eGeoTypeInfoNormal && a.typeInfo() != eGeoTypeInfoVector)) {
        return;
    }
    GeoMatrix4 linear = m;
    linear[3] = linear[7] = linear[11] = 0.;
    if (a.typeInfo() == eGeoTypeInfoNormal) {
        const std::optional<GeoMatrix4> inv = geoMatrixInverse(linear);
        if (!inv) {
            return;
        }
        GeoMatrix4 t = *inv;
        transpose16(t.data());
        linear = t;
    }
    for (std::size_t off : offsets) {
        double v[3];
        if (a.storage() == eGeoStorageFloat32) {
            float f[3];
            a.getTuple<float>(off, std::span<float>(f, 3));
            v[0] = f[0];
            v[1] = f[1];
            v[2] = f[2];
        } else if (a.storage() == eGeoStorageFloat64) {
            a.getTuple<double>(off, std::span<double>(v, 3));
        } else {
            return;
        }
        double r[3];
        for (int i = 0; i < 3; ++i) {
            r[i] = linear[std::size_t(i * 4)] * v[0] + linear[std::size_t(i * 4 + 1)] * v[1] + linear[std::size_t(i * 4 + 2)] * v[2];
        }
        if (a.storage() == eGeoStorageFloat32) {
            const float f[3] = { float(r[0]), float(r[1]), float(r[2]) };
            a.setTuple<float>(off, std::span<const float>(f, 3));
        } else {
            a.setTuple<double>(off, std::span<const double>(r, 3));
        }
    }
}

} // namespace

GeoDetail
geoUnpackPackedGeometry(const GeoDetail& source,
                        std::vector<std::string>* warnings)
{
    std::vector<GeoOffset> packed;
    forEachLive(source.primitiveMap(), [&](GeoOffset off) {
        if (source.primitiveType(off) == eGeoPrimitivePackedGeometry) {
            packed.push_back(off);
        }
    });
    if (packed.empty()) {
        return source;
    }
    bool warnedUSD = false;
    GeoDetailBuilder b(source);
    const std::vector<std::string> outerGroups = source.groupNames(eGeoAttribClassPrimitive);
    for (GeoOffset outer : packed) {
        const GeoDetailConstPtr contained = geoPackedDetail(source, outer);
        if (!contained) {
            continue;
        }
        const GeoDetail flat = geoUnpackPackedGeometry(*contained, warnings);
        const GeoMatrix4 xform = geoPackedTransform(source, outer);

        std::vector<GeoOffset> pointMap(flat.elementOffsetSize(eGeoAttribClassPoint), GeoIndexMap::kInvalidOffset);
        std::vector<std::size_t> srcPoints;
        std::vector<std::size_t> dstPoints;
        const GeoOffset firstPoint = b.appendPoints(flat.pointCount());
        GeoOffset next = firstPoint;
        forEachLive(flat.pointMap(), [&](GeoOffset p) {
            pointMap[std::size_t(p)] = next;
            srcPoints.push_back(std::size_t(p));
            dstPoints.push_back(std::size_t(next));
            const std::array<double, 3> pos = geoTransformPoint(xform, flat.pointPosition(p));
            b.setPointPosition(next, pos[0], pos[1], pos[2]);
            ++next;
        });

        std::vector<std::size_t> srcPrims;
        std::vector<std::size_t> dstPrims;
        std::vector<std::size_t> srcVertices;
        std::vector<std::size_t> dstVertices;
        forEachLive(flat.primitiveMap(), [&](GeoOffset prim) {
            const GeoPrimitiveTypeEnum type = flat.primitiveType(prim);
            if (type != eGeoPrimitivePolygon && type != eGeoPrimitivePolyline) {
                if (!warnedUSD && warnings) {
                    warnings->push_back("Packed USD primitives inside packed geometry are dropped when unpacking");
                    warnedUSD = true;
                }

                return;
            }
            const GeoVertexRange r = flat.primitiveVertices(prim);
            std::vector<GeoOffset> corners;
            corners.reserve(std::size_t(r.count));
            for (GeoOffset v = r.start; v < r.start + r.count; ++v) {
                corners.push_back(pointMap[std::size_t(flat.vertexPoint(v))]);
            }
            const GeoOffset created = b.appendPrimitive(type, corners);
            srcPrims.push_back(std::size_t(prim));
            dstPrims.push_back(std::size_t(created));
            const GeoVertexRange nr = b.detail().primitiveVertices(created);
            for (GeoOffset k = 0; k < r.count; ++k) {
                srcVertices.push_back(std::size_t(r.start + k));
                dstVertices.push_back(std::size_t(nr.start + k));
            }
        });

        const std::pair<GeoAttribClassEnum, std::pair<const std::vector<std::size_t>*, const std::vector<std::size_t>*>> classes[] = {
            { eGeoAttribClassPoint, { &srcPoints, &dstPoints } },
            { eGeoAttribClassVertex, { &srcVertices, &dstVertices } },
            { eGeoAttribClassPrimitive, { &srcPrims, &dstPrims } },
        };
        for (const auto& [cls, maps] : classes) {
            for (std::size_t i = 0; i < flat.attributeCount(cls); ++i) {
                const GeoAttribute& src = flat.attribute(cls, i);
                if ((cls == eGeoAttribClassPoint && src.name() == GeoDetail::kPositionName) || (cls == eGeoAttribClassPrimitive && isPackedIntrinsic(src.name()))) {
                    continue;
                }
                GeoAttribute* dst = b.attributeForWriting(cls, src.name());
                if (dst && !(dst->desc() == src.desc())) {
                    if (warnings) {
                        warnings->push_back("The attribute " + src.name() + " differs in type between packed contents and their container; it is not unpacked");
                    }
                    continue;
                }
                if (!dst) {
                    dst = &b.addAttribute(cls, src.desc());
                }
                for (std::size_t k = 0; k < maps.first->size(); ++k) {
                    copyAcross(src, (*maps.first)[k], *dst, (*maps.second)[k]);
                }
                transformDirections(*dst, *maps.second, xform);
            }
        }

        for (std::size_t i = 0; i < source.attributeCount(eGeoAttribClassPrimitive); ++i) {
            const std::string& name = source.attribute(eGeoAttribClassPrimitive, i).name();
            if (isPackedIntrinsic(name)) {
                continue;
            }
            GeoAttribute* a = b.attributeForWriting(eGeoAttribClassPrimitive, name);
            if (!a) {
                continue;
            }
            for (std::size_t prim : dstPrims) {
                a->copyElement(prim, std::size_t(outer));
            }
        }
        for (const std::string& g : outerGroups) {
            const GeoElementGroup* group = source.findGroup(eGeoAttribClassPrimitive, g);
            if (!group || !group->contains(outer)) {
                continue;
            }
            GeoElementGroup* writable = b.groupForWriting(eGeoAttribClassPrimitive, g);
            for (std::size_t prim : dstPrims) {
                writable->add(GeoOffset(prim));
            }
        }
    }
    b.deletePrimitives(packed, true);

    return b.finish();
}

// ---------------------------------------------------------------------------------------------
// Reading USD back

namespace {

struct ReadTarget {
    GeoDetailBuilder& builder;
    std::vector<std::string>* warnings;
    std::string primPath;

    void warn(const std::string& message) const
    {
        if (warnings) {
            warnings->push_back(primPath + ": " + message);
        }
    }
};

GeoAttribute*
writableAttribute(ReadTarget& t,
                  GeoAttribClassEnum cls,
                  const GeoAttributeDesc& desc)
{
    GeoAttribute* a = t.builder.attributeForWriting(cls, desc.name);
    if (a && !(a->desc() == desc)) {
        t.warn("the attribute " + desc.name + " already exists with another type; its values are skipped");

        return nullptr;
    }

    return a ? a : &t.builder.addAttribute(cls, desc);
}

void
scatterComponents(GeoAttribute& a,
                  const std::vector<std::size_t>& offsets,
                  const std::vector<std::uint64_t>& flat)
{
    const std::span<const std::size_t> at(offsets.data(), offsets.size());
    const std::size_t scalars = offsets.size() * std::size_t(a.tupleSize());
    switch (a.storage()) {
    case eGeoStorageInt32:
        a.scatterDense<std::int32_t>(at, std::span<const std::int32_t>(reinterpret_cast<const std::int32_t*>(flat.data()), scalars));
        break;
    case eGeoStorageInt64:
        a.scatterDense<std::int64_t>(at, std::span<const std::int64_t>(reinterpret_cast<const std::int64_t*>(flat.data()), scalars));
        break;
    case eGeoStorageFloat32:
        a.scatterDense<float>(at, std::span<const float>(reinterpret_cast<const float*>(flat.data()), scalars));
        break;
    case eGeoStorageFloat64:
        a.scatterDense<double>(at, std::span<const double>(reinterpret_cast<const double*>(flat.data()), scalars));
        break;
    case eGeoStorageString:
        break;
    }
}

// Writes @p value, one element per offset (only the first for the detail class), into the
// attribute @p name. uv widens texture coordinates to three components.
void
readValue(ReadTarget& t,
          GeoAttribClassEnum cls,
          const std::string& name,
          const SdfValueTypeName& typeName,
          int elementSize,
          const VtValue& value,
          const std::vector<std::size_t>& offsets)
{
    const SdfValueTypeName scalar = typeName.GetScalarType();
    const std::size_t expected = offsets.size();

    if (scalar == SdfValueTypeNames->Bool) {
        VtBoolArray flags;
        if (value.IsHolding<VtBoolArray>()) {
            flags = value.UncheckedGet<VtBoolArray>();
        } else if (value.IsHolding<bool>()) {
            flags = VtBoolArray(1, value.UncheckedGet<bool>());
        }
        if (cls == eGeoAttribClassDetail || flags.size() != expected) {
            t.warn("the bool primvar " + name + " does not match its interpolation and is skipped");

            return;
        }
        GeoElementGroup& group = t.builder.addGroup(cls, name);
        for (std::size_t i = 0; i < expected; ++i) {
            if (flags[i]) {
                group.add(GeoOffset(offsets[i]));
            }
        }

        return;
    }

    if (scalar == SdfValueTypeNames->String || scalar == SdfValueTypeNames->Token) {
        std::vector<std::string> strings;
        if (value.IsHolding<VtStringArray>()) {
            const VtStringArray& a = value.UncheckedGet<VtStringArray>();
            strings.assign(a.begin(), a.end());
        } else if (value.IsHolding<VtTokenArray>()) {
            for (const TfToken& tok : value.UncheckedGet<VtTokenArray>()) {
                strings.push_back(tok.GetString());
            }
        } else if (value.IsHolding<std::string>()) {
            strings.push_back(value.UncheckedGet<std::string>());
        } else if (value.IsHolding<TfToken>()) {
            strings.push_back(value.UncheckedGet<TfToken>().GetString());
        }
        if (strings.size() < expected || (cls != eGeoAttribClassDetail && strings.size() != expected)) {
            t.warn("the primvar " + name + " does not match its interpolation and is skipped");

            return;
        }
        GeoAttributeDesc desc;
        desc.name = name;
        desc.storage = eGeoStorageString;
        GeoAttribute* a = writableAttribute(t, cls, desc);
        if (!a) {
            return;
        }
        for (std::size_t i = 0; i < expected; ++i) {
            a->setString(offsets[i], strings[i]);
        }

        return;
    }

    const GeoUsdType* type = findReadType(scalar);
    if (!type) {
        t.warn("the primvar " + name + " has the unsupported type " + scalar.GetAsToken().GetString() + " and is skipped");

        return;
    }
    std::vector<std::uint64_t> flat;
    std::size_t count = 0;
    if (!type->unpack(value, &flat, &count)) {
        t.warn("the primvar " + name + " holds an unexpected value and is skipped");

        return;
    }
    const bool texCoord = name == "uv" && type->storage == eGeoStorageFloat32 && type->tupleSize == 2 && elementSize <= 1;
    int tuple = type->tupleSize * std::max(1, elementSize);
    if (tuple > GeoAttribute::kMaxTupleSize || (elementSize > 1 && type->tupleSize == 16)) {
        t.warn("the primvar " + name + " has too many components and is skipped");

        return;
    }
    const std::size_t elements = count / std::size_t(std::max(1, elementSize));
    if (elements < expected || (cls != eGeoAttribClassDetail && elements != expected)) {
        t.warn("the primvar " + name + " does not match its interpolation and is skipped");

        return;
    }
    if (type->typeInfo == eGeoTypeInfoMatrix) {
        double* m = reinterpret_cast<double*>(flat.data());
        for (std::size_t i = 0; i < count; ++i) {
            transpose16(m + i * 16);
        }
    }
    if (texCoord) {
        std::vector<std::uint64_t> widened((expected * 3 * 4 + 7) / 8, 0);
        const float* in = reinterpret_cast<const float*>(flat.data());
        float* out = reinterpret_cast<float*>(widened.data());
        for (std::size_t i = 0; i < expected; ++i) {
            out[i * 3] = in[i * 2];
            out[i * 3 + 1] = in[i * 2 + 1];
            out[i * 3 + 2] = 0.f;
        }
        flat.swap(widened);
        tuple = 3;
    }
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = type->storage;
    desc.tupleSize = tuple;
    desc.typeInfo = (texCoord || elementSize > 1) ? eGeoTypeInfoNone : type->typeInfo;
    GeoAttribute* a = writableAttribute(t, cls, desc);
    if (!a) {
        return;
    }
    scatterComponents(*a, offsets, flat);
}

} // namespace

bool
geoUsdReadPrim(const UsdPrim& prim,
               UsdTimeCode time,
               const GfMatrix4d* worldTransform,
               GeoDetailBuilder& builder,
               std::vector<std::string>* warnings,
               std::string* errorMessage)
{
    auto fail = [&](const std::string& message) {
        if (errorMessage) {
            *errorMessage = prim.GetPath().GetString() + ": " + message;
        }

        return false;
    };
    const bool isMesh = prim.IsA<UsdGeomMesh>();
    const bool isCurves = prim.IsA<UsdGeomBasisCurves>();
    const bool isPoints = prim.IsA<UsdGeomPoints>();
    if (!isMesh && !isCurves && !isPoints) {
        return fail("only meshes, basis curves and points can be unpacked, not " + prim.GetTypeName().GetString());
    }
    const UsdGeomPointBased pointBased(prim);
    VtVec3fArray points;
    pointBased.GetPointsAttr().Get(&points, time);
    VtIntArray counts;
    VtIntArray indices;
    if (isMesh) {
        const UsdGeomMesh mesh(prim);
        mesh.GetFaceVertexCountsAttr().Get(&counts, time);
        mesh.GetFaceVertexIndicesAttr().Get(&indices, time);
        std::size_t total = 0;
        for (int c : counts) {
            if (c < 3) {
                return fail("a face has fewer than three vertices");
            }
            total += std::size_t(c);
        }
        if (total != indices.size()) {
            return fail("faceVertexCounts and faceVertexIndices disagree");
        }
        for (int i : indices) {
            if (i < 0 || std::size_t(i) >= points.size()) {
                return fail("a face vertex index is out of range");
            }
        }
    } else if (isCurves) {
        const UsdGeomBasisCurves curves(prim);
        curves.GetCurveVertexCountsAttr().Get(&counts, time);
        std::size_t total = 0;
        for (int c : counts) {
            if (c < 2) {
                return fail("a curve has fewer than two vertices");
            }
            total += std::size_t(c);
        }
        if (total != points.size()) {
            return fail("curveVertexCounts does not match the point count");
        }
        TfToken curveType;
        curves.GetTypeAttr().Get(&curveType, time);
        if (curveType != UsdGeomTokens->linear) {
            if (warnings) {
                warnings->push_back(prim.GetPath().GetString() + ": cubic curves are read as polylines through their control points");
            }
        }
        indices.resize(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            indices[i] = int(i);
        }
    }

    ReadTarget target { builder, warnings, prim.GetPath().GetString() };
    const GeoOffset firstPoint = builder.appendPoints(GeoIndex(points.size()));
    for (std::size_t i = 0; i < points.size(); ++i) {
        GfVec3d p(points[i]);
        if (worldTransform) {
            p = worldTransform->Transform(p);
        }
        builder.setPointPosition(firstPoint + GeoOffset(i), p[0], p[1], p[2]);
    }
    std::vector<std::size_t> pointOffsets(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        pointOffsets[i] = std::size_t(firstPoint + GeoOffset(i));
    }
    std::vector<std::size_t> primOffsets;
    std::vector<std::size_t> vertexOffsets;
    GeoAttributeDesc sourceDesc;
    sourceDesc.name = kGeoUsdSourceAttr;
    sourceDesc.storage = eGeoStorageInt32;
    if (!counts.empty()) {
        const GeoOffset firstVertex = builder.detail().vertexMap().offsetSize();
        std::vector<GeoOffset> corners(indices.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            corners[i] = firstPoint + GeoOffset(indices[i]);
        }
        const std::vector<std::int32_t> vertexCounts(counts.begin(), counts.end());
        const GeoOffset firstPrim = builder.appendPrimitives(isMesh ? eGeoPrimitivePolygon : eGeoPrimitivePolyline, vertexCounts, corners);
        primOffsets.resize(counts.size());
        for (std::size_t i = 0; i < counts.size(); ++i) {
            primOffsets[i] = std::size_t(firstPrim + GeoOffset(i));
        }
        vertexOffsets.resize(indices.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            vertexOffsets[i] = std::size_t(firstVertex + GeoOffset(i));
        }
        GeoAttributeDesc pathDesc;
        pathDesc.name = "path";
        pathDesc.storage = eGeoStorageString;
        if (GeoAttribute* path = writableAttribute(target, eGeoAttribClassPrimitive, pathDesc)) {
            const std::string value = prim.GetPath().GetString();
            for (std::size_t off : primOffsets) {
                path->setString(off, value);
            }
        }
        if (GeoAttribute* source = writableAttribute(target, eGeoAttribClassPrimitive, sourceDesc)) {
            for (std::size_t off : primOffsets) {
                source->set<std::int32_t>(off, 1);
            }
        }
    } else if (isPoints) {
        GeoAttributeDesc pathDesc;
        pathDesc.name = "path";
        pathDesc.storage = eGeoStorageString;
        if (GeoAttribute* path = writableAttribute(target, eGeoAttribClassPoint, pathDesc)) {
            const std::string value = prim.GetPath().GetString();
            for (std::size_t off : pointOffsets) {
                path->setString(off, value);
            }
        }
        if (GeoAttribute* source = writableAttribute(target, eGeoAttribClassPoint, sourceDesc)) {
            for (std::size_t off : pointOffsets) {
                source->set<std::int32_t>(off, 1);
            }
        }
    }
    const std::vector<std::size_t> detailOffsets { 0 };

    auto classFor = [&](const TfToken& interpolation, GeoAttribClassEnum* cls) -> const std::vector<std::size_t>* {
        if (interpolation == UsdGeomTokens->constant) {
            *cls = eGeoAttribClassDetail;

            return &detailOffsets;
        }
        if (interpolation == UsdGeomTokens->uniform && !isPoints) {
            *cls = eGeoAttribClassPrimitive;

            return &primOffsets;
        }
        if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying || (interpolation == UsdGeomTokens->faceVarying && isCurves)) {
            *cls = eGeoAttribClassPoint;

            return &pointOffsets;
        }
        if (interpolation == UsdGeomTokens->faceVarying && isMesh) {
            *cls = eGeoAttribClassVertex;

            return &vertexOffsets;
        }

        return nullptr;
    };

    const UsdGeomPrimvarsAPI primvars(prim);
    std::vector<std::string> detailNames;
    for (const UsdGeomPrimvar& pv : primvars.GetPrimvarsWithValues()) {
        VtValue value;
        if (!pv.ComputeFlattened(&value, time)) {
            continue;
        }
        std::string name = pv.GetPrimvarName().GetString();
        if (name == "st") {
            name = "uv";
        } else if (name == "displayColor") {
            name = "Cd";
        }
        GeoAttribClassEnum cls = eGeoAttribClassDetail;
        const std::vector<std::size_t>* offsets = classFor(pv.GetInterpolation(), &cls);
        if (!offsets) {
            target.warn("the primvar " + name + " has an interpolation geometry cannot hold and is skipped");
            continue;
        }
        readValue(target, cls, name, pv.GetTypeName(), pv.GetElementSize(), value, *offsets);
        if (cls == eGeoAttribClassDetail) {
            detailNames.push_back(name);
        }
    }

    const UsdAttribute normals = pointBased.GetNormalsAttr();
    if (normals.HasAuthoredValue()) {
        VtValue value;
        normals.Get(&value, time);
        GeoAttribClassEnum cls = eGeoAttribClassPoint;
        if (const std::vector<std::size_t>* offsets = classFor(pointBased.GetNormalsInterpolation(), &cls)) {
            readValue(target, cls, "N", normals.GetTypeName(), 1, value, *offsets);
            if (cls == eGeoAttribClassDetail) {
                detailNames.push_back("N");
            }
        }
    }
    if (worldTransform) {
        // The detail element is shared by every prim read, so only what this prim wrote to it moves.
        const std::pair<GeoAttribClassEnum, const std::vector<std::size_t>*> classes[] = {
            { eGeoAttribClassPoint, &pointOffsets },
            { eGeoAttribClassVertex, &vertexOffsets },
            { eGeoAttribClassPrimitive, &primOffsets },
            { eGeoAttribClassDetail, &detailOffsets },
        };
        for (const auto& [cls, offsets] : classes) {
            if (offsets->empty()) {
                continue;
            }
            std::vector<std::string> names;
            const GeoDetail& d = builder.detail();
            for (std::size_t i = 0; i < d.attributeCount(cls); ++i) {
                const GeoAttribute& a = d.attribute(cls, i);
                if (!a.isArray() && (a.tupleSize() == 3) && isDirection(a.typeInfo()) && ((cls != eGeoAttribClassDetail) || (std::find(detailNames.begin(), detailNames.end(), a.name()) != detailNames.end()))) {
                    names.push_back(a.name());
                }
            }
            for (const std::string& name : names) {
                GeoAttribute* a = builder.attributeForWriting(cls, name);
                if (!a) {
                    continue;
                }
                const GfMatrix4d directions = directionMatrix(*worldTransform, a->typeInfo());
                for (std::size_t off : *offsets) {
                    if (a->storage() == eGeoStorageFloat32) {
                        float v[3];
                        a->getTuple<float>(off, std::span<float>(v, 3));
                        carryDirections(v, eGeoStorageFloat32, 1, directions, a->typeInfo());
                        a->setTuple<float>(off, std::span<const float>(v, 3));
                    } else if (a->storage() == eGeoStorageFloat64) {
                        double v[3];
                        a->getTuple<double>(off, std::span<double>(v, 3));
                        carryDirections(v, eGeoStorageFloat64, 1, directions, a->typeInfo());
                        a->setTuple<double>(off, std::span<const double>(v, 3));
                    }
                }
            }
        }
    }
    if (isCurves || isPoints) {
        const UsdAttribute widths = isCurves ? UsdGeomCurves(prim).GetWidthsAttr() : UsdGeomPoints(prim).GetWidthsAttr();
        const TfToken interpolation = isCurves ? UsdGeomCurves(prim).GetWidthsInterpolation() : UsdGeomPoints(prim).GetWidthsInterpolation();
        if (widths.HasAuthoredValue()) {
            VtValue value;
            widths.Get(&value, time);
            GeoAttribClassEnum cls = eGeoAttribClassPoint;
            if (const std::vector<std::size_t>* offsets = classFor(interpolation, &cls)) {
                readValue(target, cls, "pscale", widths.GetTypeName(), 1, value, *offsets);
            }
        }
    }
    if (isMesh) {
        for (const UsdGeomSubset& subset : UsdGeomSubset::GetAllGeomSubsets(UsdGeomImageable(prim))) {
            TfToken elementType;
            subset.GetElementTypeAttr().Get(&elementType, time);
            if (elementType != UsdGeomTokens->face) {
                target.warn("the subset " + subset.GetPrim().GetName().GetString() + " is not a face subset and is skipped");
                continue;
            }
            VtIntArray members;
            subset.GetIndicesAttr().Get(&members, time);
            GeoElementGroup& group = builder.addGroup(eGeoAttribClassPrimitive, subset.GetPrim().GetName().GetString());
            for (int i : members) {
                if (i >= 0 && std::size_t(i) < primOffsets.size()) {
                    group.add(GeoOffset(primOffsets[std::size_t(i)]));
                }
            }
        }
    }

    return true;
}

GeoUsdDetailPaths
geoUsdDetailPaths(const GeoDetail& d,
                  const GeoUsdConvertOptions& options)
{
    GeoUsdDetailPaths out;
    std::unordered_set<SdfPath, SdfPath::Hash> seen;
    std::unordered_map<SdfPath, std::size_t, SdfPath::Hash> pathIndex;
    const auto add = [&](const SdfPath& path, bool source, bool fromUsd) {
        if (path.IsEmpty()) {
            return;
        }
        if (seen.insert(path).second) {
            if (source) {
                out.sourcePaths.push_back(path);
            } else {
                pathIndex.emplace(path, out.paths.size());
                out.paths.push_back(path);
                out.pathsFromUsd.push_back(0);
            }
        }
        if (fromUsd) {
            const auto it = pathIndex.find(path);
            if (it != pathIndex.end()) {
                out.pathsFromUsd[it->second] = 1;
            }
        }
    };
    const SdfPath root = (options.rootPath.IsAbsolutePath() && options.rootPath.IsPrimPath()) ? options.rootPath : SdfPath("/geo");
    add(root, false, false);

    const auto resolver = [&root](const GeoAttribute* a) {
        return [a, &root, byString = std::vector<SdfPath>(), known = std::vector<char>()](GeoOffset off) mutable -> SdfPath {
            const std::int32_t s = a->get<std::int32_t>(std::size_t(off));
            if (s < 0) {
                return SdfPath();
            }
            if (std::size_t(s) >= byString.size()) {
                byString.resize(std::size_t(s) + 1);
                known.resize(std::size_t(s) + 1, 0);
            }
            if (!known[std::size_t(s)]) {
                byString[std::size_t(s)] = resolvePathValue(a->getString(std::size_t(off)), root);
                known[std::size_t(s)] = 1;
            }

            return byString[std::size_t(s)];
        };
    };

    const GeoAttribute* pathAttr = nullptr;
    for (const char* name : { "path", "name" }) {
        const GeoAttribute* a = d.findAttribute(eGeoAttribClassPrimitive, name);
        if (a && a->storage() == eGeoStorageString && a->tupleSize() == 1 && !a->isArray()) {
            pathAttr = a;
            break;
        }
    }
    {
        const GeoAttribute* primitiveSource = usdSourceAttribute(d, eGeoAttribClassPrimitive);
        auto primitivePath = resolver(pathAttr);
        forEachLive(d.primitiveMap(), [&](GeoOffset off) {
            if (d.primitiveType(off) == eGeoPrimitivePackedUSD) {
                const SdfPath source(std::string(geoPackedUSDPath(d, off)));
                if (!source.IsEmpty() && source.IsAbsolutePath() && source.IsPrimPath()) {
                    add(source, true, false);
                }

                return;
            }
            if (pathAttr) {
                add(primitivePath(off), false, readFromUsd(primitiveSource, off));
            }
        });
    }

    if (const GeoAttribute* pointPath = pointPathAttribute(d)) {
        const GeoAttribute* pointSource = usdSourceAttribute(d, eGeoAttribClassPoint);
        auto pathOf = resolver(pointPath);
        forEachLive(d.pointMap(), [&](GeoOffset off) {
            add(pathOf(off), false, readFromUsd(pointSource, off));
        });
    }

    return out;
}

SdfPath
geoUsdPlacedPath(const SdfPath& requested,
                 const SdfPath& root,
                 const GeoUsdInputScene* scene,
                 bool fromUsd,
                 GeoUsdPlacementEnum* placement)
{
    GeoUsdPlacementEnum why = eGeoUsdPlacedAsRequested;
    if (scene && !requested.IsEmpty() && !requested.HasPrefix(root)) {
        const bool onInput = scene->find(requested) != nullptr;
        if (fromUsd && !onInput) {
            why = eGeoUsdPlacedSourceGone;
        } else if (!fromUsd && onInput) {
            why = eGeoUsdPlacedCollision;
        }
    }
    if (placement) {
        *placement = why;
    }

    return (why == eGeoUsdPlacedAsRequested) ? requested : root.AppendPath(requested.MakeRelativePath(SdfPath::AbsoluteRootPath()));
}

GeoUsdInputSceneConstPtr
geoUsdCaptureInputScene(const UsdStageRefPtr& stage,
                        UsdTimeCode time,
                        const std::vector<SdfPath>& paths,
                        const std::vector<SdfPath>& transformOnly)
{
    auto scene = std::make_shared<GeoUsdInputScene>();
    if (!stage) {
        return scene;
    }
    UsdGeomXformCache xforms(time);
    const auto capture = [&](const UsdPrim& prim) -> GeoUsdInputPrim& {
        const auto it = scene->prims.find(prim.GetPath());
        if (it != scene->prims.end()) {
            return it->second;
        }
        GeoUsdInputPrim input;
        input.typeName = prim.GetTypeName();
        bool resets = false;
        const GfMatrix4d local = xforms.GetLocalTransformation(prim, &resets);
        input.resetsXformStack = resets;
        input.parentWorld = resets ? GfMatrix4d(1.) : xforms.GetParentToWorldTransform(prim);
        input.world = local * input.parentWorld;

        return scene->prims.emplace(prim.GetPath(), std::move(input)).first->second;
    };
    std::unordered_set<SdfPath, SdfPath::Hash> walked;
    const auto walk = [&](const SdfPath& start) {
        for (SdfPath at = start; !at.IsEmpty() && at != SdfPath::AbsoluteRootPath(); at = at.GetParentPath()) {
            if (!walked.insert(at).second) {
                return;
            }
            // A pruned (inactive) prim is not on the stage for the override to land on.
            const UsdPrim prim = stage->GetPrimAtPath(at);
            if (prim && prim.IsActive()) {
                capture(prim);
            }
        }
    };
    for (const SdfPath& path : transformOnly) {
        walk(path);
    }
    const std::unordered_set<SdfPath, SdfPath::Hash> skipValues(transformOnly.begin(), transformOnly.end());
    for (const SdfPath& path : paths) {
        walk(path);
        const UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim || !prim.IsActive() || skipValues.count(path)) {
            continue;
        }
        GeoUsdInputPrim& input = capture(prim);
        for (const TfToken& name : { UsdGeomTokens->points, UsdGeomTokens->extent, UsdGeomTokens->normals, UsdGeomTokens->widths,
                                     UsdGeomTokens->faceVertexCounts, UsdGeomTokens->faceVertexIndices, UsdGeomTokens->curveVertexCounts }) {
            const UsdAttribute attribute = prim.GetAttribute(name);
            VtValue value;
            if (attribute && attribute.Get(&value, time)) {
                input.values[name] = value;
            }
        }
        for (const UsdGeomPrimvar& primvar : UsdGeomPrimvarsAPI(prim).GetPrimvarsWithValues()) {
            VtValue value;
            if (primvar.ComputeFlattened(&value, time)) {
                input.values[primvar.GetAttr().GetName()] = value;
            }
            VtIntArray indices;
            if (primvar.IsIndexed() && primvar.GetIndices(&indices, time)) {
                input.values[TfToken(primvar.GetAttr().GetName().GetString() + ":indices")] = VtValue(indices);
            }
        }
    }

    std::vector<const std::pair<const SdfPath, GeoUsdInputPrim>*> ordered;
    ordered.reserve(scene->prims.size());
    for (auto& entry : scene->prims) {
        GeoUsdInputPrim& input = entry.second;
        Hash64 h;
        h.append(U64(TfToken::HashFunctor()(input.typeName)));
        hashMatrix(h, input.world);
        hashMatrix(h, input.parentWorld);
        h.append(U64(input.resetsXformStack));
        std::vector<TfToken> names;
        for (const auto& value : input.values) {
            names.push_back(value.first);
        }
        std::sort(names.begin(), names.end());
        for (const TfToken& name : names) {
            h.append(U64(TfToken::HashFunctor()(name)));
            h.append(U64(input.values.at(name).GetHash()));
        }
        h.computeHash();
        input.hash = h.value();
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    Hash64 h;
    for (const auto* entry : ordered) {
        h.append(U64(SdfPath::Hash()(entry->first)));
        h.append(entry->second.hash);
    }
    h.computeHash();
    scene->hash = h.value();

    return scene;
}

NATRON_NAMESPACE_EXIT
