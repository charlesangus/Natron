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

#include "Engine/Nodes/Scene/ScenePrimElements.h"

#include <algorithm>
#include <charconv>
#include <type_traits>
#include <utility>

#include <pxr/base/gf/matrix2d.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/usd/sdf/timeCode.h>
#include <pxr/usd/usdGeom/curves.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/subset.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr std::size_t kCompactArrayElements = 3;

template <typename T>
void
appendNumber(std::string& s,
             T v)
{
    char buf[64];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, r.ptr);
}

template <typename T>
constexpr bool kIsVec = requires { T::dimension; typename T::ScalarType; };

template <typename T>
constexpr bool kIsQuat = requires(const T& q) { q.GetImaginary(); q.GetReal(); };

template <typename T>
constexpr bool kIsMatrix = requires { T::numRows; T::numColumns; };

template <typename T>
constexpr bool kIsText = std::is_same_v<T, std::string> || std::is_same_v<T, PXR_NS::TfToken> || std::is_same_v<T, PXR_NS::SdfAssetPath>;

template <typename T>
constexpr int
tupleSizeOf()
{
    if constexpr (kIsVec<T>) {
        return int(T::dimension);
    } else if constexpr (kIsQuat<T>) {
        return 4;
    } else if constexpr (kIsMatrix<T>) {
        return int(T::numRows * T::numColumns);
    } else {
        return 1;
    }
}

// Quaternions show as x, y, z, w: the imaginary part, then the real one.
template <typename T>
auto
componentOf(const T& v,
            int c)
{
    if constexpr (kIsVec<T>) {
        return v[c];
    } else if constexpr (kIsQuat<T>) {
        return c < 3 ? v.GetImaginary()[c] : v.GetReal();
    } else if constexpr (kIsMatrix<T>) {
        return v[c / int(T::numColumns)][c % int(T::numColumns)];
    } else {
        return v;
    }
}

template <typename S>
void
appendScalar(std::string& s,
             const S& v)
{
    if constexpr (std::is_same_v<S, std::string>) {
        s += v;
    } else if constexpr (std::is_same_v<S, PXR_NS::TfToken>) {
        s += v.GetString();
    } else if constexpr (std::is_same_v<S, PXR_NS::SdfAssetPath>) {
        s += v.GetAssetPath();
    } else if constexpr (std::is_same_v<S, PXR_NS::GfHalf>) {
        appendNumber(s, float(v));
    } else if constexpr (std::is_same_v<S, PXR_NS::SdfTimeCode>) {
        appendNumber(s, v.GetValue());
    } else if constexpr (std::is_same_v<S, bool>) {
        appendNumber(s, int(v));
    } else if constexpr (std::is_same_v<S, unsigned char>) {
        appendNumber(s, unsigned(v));
    } else {
        appendNumber(s, v);
    }
}

template <typename S>
double
scalarNumber(const S& v)
{
    if constexpr (kIsText<S>) {
        return 0.;
    } else if constexpr (std::is_same_v<S, PXR_NS::GfHalf>) {
        return double(float(v));
    } else if constexpr (std::is_same_v<S, PXR_NS::SdfTimeCode>) {
        return v.GetValue();
    } else {
        return double(v);
    }
}

template <typename T, bool Array>
const T&
elementAt(const PXR_NS::VtValue& v,
          std::size_t i)
{
    if constexpr (Array) {
        return v.UncheckedGet<PXR_NS::VtArray<T>>().cdata()[i];
    } else {
        return v.UncheckedGet<T>();
    }
}

} // namespace

struct ScenePrimElementsSource::ValueOps {
    int tupleSize = 1;
    bool isText = false;
    bool isQuaternion = false;
    bool isArray = false;
    std::size_t (*size)(const PXR_NS::VtValue&) = nullptr;
    void (*append)(std::string&, const PXR_NS::VtValue&, std::size_t, int) = nullptr;
    double (*number)(const PXR_NS::VtValue&, std::size_t, int) = nullptr;
};

namespace {

typedef ScenePrimElementsSource::ValueOps ValueOps;

template <typename T, bool Array>
const ValueOps*
opsOf()
{
    static const ValueOps ops {
        tupleSizeOf<T>(),
        kIsText<T>,
        kIsQuat<T>,
        Array,
        [](const PXR_NS::VtValue& v) -> std::size_t {
            if constexpr (Array) {
                return v.UncheckedGet<PXR_NS::VtArray<T>>().size();
            } else {
                return 1;
            }
        },
        [](std::string& s, const PXR_NS::VtValue& v, std::size_t i, int c) {
            appendScalar(s, componentOf(elementAt<T, Array>(v, i), c));
        },
        [](const PXR_NS::VtValue& v, std::size_t i, int c) {
            return scalarNumber(componentOf(elementAt<T, Array>(v, i), c));
        },
    };

    return &ops;
}

template <typename... Ts>
const ValueOps*
findOps(const PXR_NS::VtValue& v)
{
    const ValueOps* found = nullptr;
    ((found || (found = v.IsHolding<PXR_NS::VtArray<Ts>>() ? opsOf<Ts, true>() : (v.IsHolding<Ts>() ? opsOf<Ts, false>() : nullptr))), ...);

    return found;
}

const ValueOps*
valueOps(const PXR_NS::VtValue& v)
{
    using namespace PXR_NS;

    return findOps<bool, unsigned char, int, unsigned int, std::int64_t, std::uint64_t, GfHalf, float, double,
                   SdfTimeCode, std::string, TfToken, SdfAssetPath,
                   GfVec2i, GfVec3i, GfVec4i, GfVec2h, GfVec3h, GfVec4h, GfVec2f, GfVec3f, GfVec4f, GfVec2d, GfVec3d, GfVec4d,
                   GfQuath, GfQuatf, GfQuatd, GfMatrix2d, GfMatrix3d, GfMatrix4d>(v);
}

const char*
className(GeoAttribClassEnum cls)
{
    switch (cls) {
    case eGeoAttribClassPoint:
        return "points";
    case eGeoAttribClassVertex:
        return "vertices";
    case eGeoAttribClassPrimitive:
        return "primitives";
    case eGeoAttribClassDetail:
        return "detail rows";
    }

    return "";
}

std::string
componentName(const std::string& name,
              const PXR_NS::TfToken& role,
              const ValueOps& ops,
              int component)
{
    if (ops.tupleSize == 1) {
        return name;
    }
    static constexpr char kVector[] = { 'x', 'y', 'z', 'w' };
    static constexpr char kColour[] = { 'r', 'g', 'b', 'a' };
    std::string suffix;
    if (ops.tupleSize <= 4) {
        if (role == PXR_NS::SdfValueRoleNames->Color) {
            suffix = std::string(1, kColour[component]);
        } else if (ops.isQuaternion || role == PXR_NS::SdfValueRoleNames->Point || role == PXR_NS::SdfValueRoleNames->Normal
                   || role == PXR_NS::SdfValueRoleNames->Vector) {
            suffix = std::string(1, kVector[component]);
        }
    }
    if (suffix.empty()) {
        suffix = std::to_string(component);
    }

    return name + "[" + suffix + "]";
}

} // namespace

ScenePrimElementsSource::ScenePrimElementsSource(const std::int64_t (&rowCounts)[kGeoAttribClassCount],
                                                 std::vector<Binding> bindings,
                                                 std::vector<ScenePrimElementGroup> groups)
    : _bindings(std::move(bindings))
    , _groups(std::move(groups))
{
    std::copy(rowCounts, rowCounts + kGeoAttribClassCount, _rowCounts);
    _rowCounts[eGeoAttribClassDetail] = 1;
    _ops.reserve(_bindings.size());
    for (std::size_t i = 0; i < _bindings.size(); ++i) {
        const Binding& b = _bindings[i];
        const ValueOps* ops = valueOps(b.values);
        _ops.push_back(ops);
        std::vector<GeoSpreadsheetColumn>& cols = _columns[b.cls];
        std::vector<Column>& binds = _columnBindings[b.cls];
        if (!ops) {
            cols.push_back(GeoSpreadsheetColumn { b.name, false, "Values of type " + b.values.GetTypeName() + " cannot be shown." });
            binds.push_back(Column { i, 0, false, 1, false });
            continue;
        }
        const std::int64_t rows = _rowCounts[b.cls];
        const std::int64_t length = std::int64_t(ops->size(b.values));
        const std::int64_t elementSize = std::max(b.elementSize, 1);
        Column col { i, 0, elementSize > 1, int(elementSize), true };
        std::string problem;
        if (b.indexed) {
            if (std::int64_t(b.indices.size()) != rows) {
                problem = std::to_string(b.indices.size()) + " indices for " + std::to_string(rows) + " " + className(b.cls) + ".";
            } else if (length % elementSize) {
                problem = std::to_string(length) + " values do not make whole elements of " + std::to_string(elementSize) + ".";
            }
        } else if (b.cls == eGeoAttribClassDetail && ops->isArray && length != elementSize) {
            // A constant array primvar is one value, the whole array.
            col.arrayCell = true;
            col.arrayLength = int(length);
        } else if (length != rows * elementSize) {
            problem = std::to_string(length) + " values for " + std::to_string(rows) + " " + className(b.cls);
            if (elementSize > 1) {
                problem += " of " + std::to_string(elementSize) + " values each";
            }
            problem += ".";
        }
        col.valid = problem.empty();
        if (col.arrayCell) {
            cols.push_back(GeoSpreadsheetColumn { b.name, true, problem });
            binds.push_back(col);
            continue;
        }
        for (int k = 0; k < ops->tupleSize; ++k) {
            col.component = k;
            cols.push_back(GeoSpreadsheetColumn { componentName(b.name, b.role, *ops, k), false, problem });
            binds.push_back(col);
        }
    }
}

ScenePrimElementsSource::~ScenePrimElementsSource() = default;

std::int64_t
ScenePrimElementsSource::rowCount(GeoAttribClassEnum cls) const
{
    return _rowCounts[cls];
}

const std::vector<GeoSpreadsheetColumn>&
ScenePrimElementsSource::columns(GeoAttribClassEnum cls) const
{
    return _columns[cls];
}

std::int64_t
ScenePrimElementsSource::elementOf(const Column& col,
                                   std::int64_t row) const
{
    const Binding& b = _bindings[col.binding];
    std::int64_t element = row;
    if (b.indexed) {
        element = b.indices[std::size_t(row)];
    } else if (b.cls == eGeoAttribClassDetail) {
        element = 0;
    }
    const std::int64_t length = std::int64_t(_ops[col.binding]->size(b.values));
    if (element < 0 || (element + 1) * col.arrayLength > length) {
        return -1;
    }

    return element;
}

std::string
ScenePrimElementsSource::cellText(const Column& col,
                                  std::int64_t row,
                                  bool full) const
{
    std::string s;
    if (!col.valid) {
        return s;
    }
    const std::int64_t element = elementOf(col, row);
    if (element < 0) {
        return s;
    }
    const Binding& b = _bindings[col.binding];
    const ValueOps& ops = *_ops[col.binding];
    if (!col.arrayCell) {
        ops.append(s, b.values, std::size_t(element), col.component);

        return s;
    }
    const std::size_t first = std::size_t(element) * std::size_t(col.arrayLength);
    const std::size_t count = std::size_t(col.arrayLength);
    const std::size_t shown = full ? count : std::min(count, kCompactArrayElements);
    s = "(";
    appendNumber(s, count);
    s += ") [";
    for (std::size_t t = 0; t < shown; ++t) {
        if (t) {
            s += ", ";
        }
        if (ops.tupleSize > 1) {
            s += '(';
        }
        for (int c = 0; c < ops.tupleSize; ++c) {
            if (c) {
                s += ", ";
            }
            ops.append(s, b.values, first + t, c);
        }
        if (ops.tupleSize > 1) {
            s += ')';
        }
    }
    if (shown < count) {
        s += ", \xE2\x80\xA6";
    }
    s += ']';

    return s;
}

void
ScenePrimElementsSource::readCells(GeoAttribClassEnum cls,
                                   std::int64_t firstRow,
                                   std::int64_t rowCountWanted,
                                   std::span<const int> columnList,
                                   std::vector<std::string>& out) const
{
    const std::int64_t total = rowCount(cls);
    firstRow = std::clamp<std::int64_t>(firstRow, 0, total);
    rowCountWanted = std::clamp<std::int64_t>(rowCountWanted, 0, total - firstRow);
    const std::size_t rows = std::size_t(rowCountWanted);
    const std::size_t cols = columnList.size();
    out.assign(rows * cols, std::string());
    for (std::size_t c = 0; c < cols; ++c) {
        const Column& col = _columnBindings[cls][std::size_t(columnList[c])];
        for (std::size_t r = 0; r < rows; ++r) {
            out[r * cols + c] = cellText(col, firstRow + std::int64_t(r), false);
        }
    }
}

GeoSpreadsheetSortKey
ScenePrimElementsSource::sortKey(GeoAttribClassEnum cls,
                                 int column,
                                 std::int64_t row) const
{
    if (row < 0 || row >= rowCount(cls)) {
        return 0.;
    }
    const Column& col = _columnBindings[cls][std::size_t(column)];
    if (!col.valid) {
        return 0.;
    }
    if (col.arrayCell) {
        return GeoSpreadsheetArrayLength { col.arrayLength };
    }
    const std::int64_t element = elementOf(col, row);
    if (element < 0) {
        return 0.;
    }
    const ValueOps& ops = *_ops[col.binding];
    if (ops.isText) {
        return cellText(col, row, false);
    }

    return ops.number(_bindings[col.binding].values, std::size_t(element), col.component);
}

std::string
ScenePrimElementsSource::fullValue(GeoAttribClassEnum cls,
                                   int column,
                                   std::int64_t row) const
{
    if (row < 0 || row >= rowCount(cls)) {
        return std::string();
    }

    return cellText(_columnBindings[cls][std::size_t(column)], row, true);
}

struct ScenePrimElementsRead::Implementation {
    struct Step {
        enum KindEnum {
            eValues,
            eIndices,
            eCount,
            eSubset,
        };

        KindEnum kind = eValues;
        // A binding for values and indices, a subset for eSubset.
        std::size_t target = 0;
        PXR_NS::TfToken attribute;
    };

    static constexpr std::size_t kNone = std::size_t(-1);

    std::string path;
    double frame = 0.;
    double framesPerSecond = 24.;
    PXR_NS::UsdTimeCode time;
    ScenePrimElementsStatusEnum status = eScenePrimElementsStatusOk;
    bool listed = false;
    std::vector<ScenePrimElementsSource::Binding> bindings;
    std::vector<PXR_NS::SdfPath> subsets;
    std::vector<ScenePrimElementGroup> groups;
    std::vector<Step> steps;
    std::size_t next = 0;
    std::size_t pointCountBinding = kNone;
    std::size_t fallbackPointCountBinding = kNone;
    std::size_t vertexCountBinding = kNone;
    std::int64_t primitiveCount = 0;

    PXR_NS::UsdPrim findPrim(const SceneStage* stage);
    void list(const PXR_NS::UsdPrim& prim,
              const SceneStage& stage);
    std::size_t addValues(const PXR_NS::UsdAttribute& attribute,
                          const std::string& name,
                          GeoAttribClassEnum cls,
                          int elementSize,
                          const PXR_NS::TfToken& indices);
    void run(const Step& step,
             const PXR_NS::UsdPrim& prim);
};

namespace {

GeoAttribClassEnum
classOfInterpolation(const PXR_NS::TfToken& interpolation)
{
    if (interpolation == PXR_NS::UsdGeomTokens->constant) {
        return eGeoAttribClassDetail;
    }
    if (interpolation == PXR_NS::UsdGeomTokens->uniform) {
        return eGeoAttribClassPrimitive;
    }
    if (interpolation == PXR_NS::UsdGeomTokens->faceVarying) {
        return eGeoAttribClassVertex;
    }

    return eGeoAttribClassPoint;
}

} // namespace

PXR_NS::UsdPrim
ScenePrimElementsRead::Implementation::findPrim(const SceneStage* stage)
{
    if (!stage || !stage->getStage() || !PXR_NS::SdfPath::IsValidPathString(path)) {
        status = eScenePrimElementsStatusMissing;

        return PXR_NS::UsdPrim();
    }
    const PXR_NS::SdfPath primPath = PXR_NS::SdfPath(path).GetAbsoluteRootOrPrimPath();
    if (!primPath.IsAbsolutePath()) {
        status = eScenePrimElementsStatusMissing;

        return PXR_NS::UsdPrim();
    }
    const PXR_NS::UsdStageRefPtr& usdStage = stage->getStage();
    const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(primPath);
    if (prim) {
        if (!prim.IsLoaded()) {
            status = eScenePrimElementsStatusNotLoaded;

            return PXR_NS::UsdPrim();
        }

        return prim;
    }
    // A prim inside a payload that is not loaded is not on the stage at all.
    for (PXR_NS::SdfPath above = primPath.GetParentPath(); !above.IsEmpty() && above != PXR_NS::SdfPath::AbsoluteRootPath(); above = above.GetParentPath()) {
        if (const PXR_NS::UsdPrim ancestor = usdStage->GetPrimAtPath(above)) {
            status = ancestor.IsLoaded() ? eScenePrimElementsStatusMissing : eScenePrimElementsStatusNotLoaded;

            return PXR_NS::UsdPrim();
        }
    }
    status = eScenePrimElementsStatusMissing;

    return PXR_NS::UsdPrim();
}

std::size_t
ScenePrimElementsRead::Implementation::addValues(const PXR_NS::UsdAttribute& attribute,
                                                 const std::string& name,
                                                 GeoAttribClassEnum cls,
                                                 int elementSize,
                                                 const PXR_NS::TfToken& indices)
{
    if (!attribute.IsDefined()) {
        return kNone;
    }
    ScenePrimElementsSource::Binding b;
    b.name = name;
    b.cls = cls;
    b.role = attribute.GetTypeName().GetRole();
    b.elementSize = elementSize;
    b.indexed = !indices.IsEmpty();
    const std::size_t index = bindings.size();
    bindings.push_back(std::move(b));
    steps.push_back(Step { Step::eValues, index, attribute.GetName() });
    if (!indices.IsEmpty()) {
        steps.push_back(Step { Step::eIndices, index, indices });
    }

    return index;
}

void
ScenePrimElementsRead::Implementation::list(const PXR_NS::UsdPrim& prim,
                                            const SceneStage& stage)
{
    using namespace PXR_NS;

    time = UsdTimeCode(SceneOps::frameToTimeCode(stage.getPayload(), frame, framesPerSecond));
    const UsdGeomPrimvarsAPI primvarsAPI(prim);
    const std::vector<UsdGeomPrimvar> primvars = primvarsAPI.GetPrimvars();
    bool primvarNormals = false;
    for (const UsdGeomPrimvar& primvar : primvars) {
        primvarNormals = primvarNormals || primvar.GetPrimvarName() == UsdGeomTokens->normals;
    }
    const auto addBuiltin = [&](const TfToken& name, GeoAttribClassEnum cls) {
        return addValues(prim.GetAttribute(name), name.GetString(), cls, 1, TfToken());
    };
    const auto addInterpolated = [&](const TfToken& name) {
        const UsdAttribute attribute = prim.GetAttribute(name);
        TfToken interpolation = UsdGeomTokens->vertex;
        attribute.GetMetadata(UsdGeomTokens->interpolation, &interpolation);
        addValues(attribute, name.GetString(), classOfInterpolation(interpolation), 1, TfToken());
    };

    if (prim.IsA<UsdGeomPointBased>()) {
        pointCountBinding = addBuiltin(UsdGeomTokens->points, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->velocities, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->accelerations, eGeoAttribClassPoint);
        // primvars:normals overrides the normals attribute.
        if (!primvarNormals) {
            addInterpolated(UsdGeomTokens->normals);
        }
        addInterpolated(UsdGeomTokens->widths);
    }
    if (prim.IsA<UsdGeomPointInstancer>()) {
        pointCountBinding = addBuiltin(UsdGeomTokens->protoIndices, eGeoAttribClassPoint);
        fallbackPointCountBinding = addBuiltin(UsdGeomTokens->positions, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->orientations, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->orientationsf, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->scales, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->ids, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->velocities, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->angularVelocities, eGeoAttribClassPoint);
        addBuiltin(UsdGeomTokens->accelerations, eGeoAttribClassPoint);
    }
    if (prim.IsA<UsdGeomMesh>()) {
        vertexCountBinding = addValues(prim.GetAttribute(UsdGeomTokens->faceVertexIndices), "Point", eGeoAttribClassVertex, 1, TfToken());
        steps.push_back(Step { Step::eCount, 0, UsdGeomTokens->faceVertexCounts });
    } else if (prim.IsA<UsdGeomCurves>()) {
        steps.push_back(Step { Step::eCount, 0, UsdGeomTokens->curveVertexCounts });
    }
    for (const UsdGeomPrimvar& primvar : primvars) {
        const TfToken indices = primvar.IsIndexed() ? primvar.GetIndicesAttr().GetName() : TfToken();
        addValues(primvar.GetAttr(), primvar.GetPrimvarName().GetString(), classOfInterpolation(primvar.GetInterpolation()), primvar.GetElementSize(), indices);
    }
    if (prim.IsA<UsdGeomImageable>()) {
        for (const UsdGeomSubset& subset : UsdGeomSubset::GetAllGeomSubsets(UsdGeomImageable(prim))) {
            steps.push_back(Step { Step::eSubset, subsets.size(), TfToken() });
            subsets.push_back(subset.GetPath());
        }
    }
}

void
ScenePrimElementsRead::Implementation::run(const Step& step,
                                           const PXR_NS::UsdPrim& prim)
{
    using namespace PXR_NS;

    if (step.kind == Step::eSubset) {
        const UsdGeomSubset subset(prim.GetStage()->GetPrimAtPath(subsets[step.target]));
        if (!subset) {
            return;
        }
        TfToken elementType;
        subset.GetElementTypeAttr().Get(&elementType, time);
        ScenePrimElementGroup group;
        if (elementType == UsdGeomTokens->face) {
            group.cls = eGeoAttribClassPrimitive;
        } else if (elementType == UsdGeomTokens->point) {
            group.cls = eGeoAttribClassPoint;
        } else {
            return;
        }
        group.name = subset.GetPrim().GetName().GetString();
        subset.GetIndicesAttr().Get(&group.indices, time);
        groups.push_back(std::move(group));

        return;
    }
    const UsdAttribute attribute = prim.GetAttribute(step.attribute);
    if (!attribute) {
        return;
    }
    switch (step.kind) {
    case Step::eValues:
        attribute.Get(&bindings[step.target].values, time);
        break;
    case Step::eIndices:
        attribute.Get(&bindings[step.target].indices, time);
        break;
    case Step::eCount: {
        VtValue counts;
        if (attribute.Get(&counts, time)) {
            primitiveCount = std::int64_t(counts.GetArraySize());
        }
        break;
    }
    case Step::eSubset:
        break;
    }
}

ScenePrimElementsRead::ScenePrimElementsRead(std::string primPath,
                                             double frame,
                                             double framesPerSecond)
    : _imp(std::make_unique<Implementation>())
{
    _imp->path = std::move(primPath);
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
}

ScenePrimElementsRead::~ScenePrimElementsRead() = default;

bool
ScenePrimElementsRead::step(const SceneStage* stage)
{
    Implementation& imp = *_imp;
    if (imp.status != eScenePrimElementsStatusOk || (imp.listed && imp.next >= imp.steps.size())) {
        return false;
    }
    const PXR_NS::UsdPrim prim = imp.findPrim(stage);
    if (!prim) {
        return false;
    }
    if (!imp.listed) {
        imp.list(prim, *stage);
        imp.listed = true;
    } else {
        imp.run(imp.steps[imp.next++], prim);
    }

    return imp.next < imp.steps.size();
}

ScenePrimElementsStatusEnum
ScenePrimElementsRead::getStatus() const
{
    return _imp->status;
}

std::shared_ptr<const ScenePrimElementsSource>
ScenePrimElementsRead::takeSource()
{
    Implementation& imp = *_imp;
    if (imp.status != eScenePrimElementsStatusOk || !imp.listed || imp.next < imp.steps.size()) {
        return std::shared_ptr<const ScenePrimElementsSource>();
    }
    const auto sizeOf = [&](std::size_t binding) -> std::int64_t {
        return binding == Implementation::kNone ? 0 : std::int64_t(imp.bindings[binding].values.GetArraySize());
    };
    std::int64_t rowCounts[kGeoAttribClassCount] = {};
    rowCounts[eGeoAttribClassPoint] = sizeOf(imp.pointCountBinding);
    if (!rowCounts[eGeoAttribClassPoint]) {
        rowCounts[eGeoAttribClassPoint] = sizeOf(imp.fallbackPointCountBinding);
    }
    rowCounts[eGeoAttribClassVertex] = sizeOf(imp.vertexCountBinding);
    rowCounts[eGeoAttribClassPrimitive] = imp.primitiveCount;
    rowCounts[eGeoAttribClassDetail] = 1;
    std::vector<ScenePrimElementsSource::Binding> bindings;
    bindings.reserve(imp.bindings.size());
    for (ScenePrimElementsSource::Binding& b : imp.bindings) {
        if (!b.values.IsEmpty()) {
            bindings.push_back(std::move(b));
        }
    }
    imp.bindings.clear();

    return std::make_shared<const ScenePrimElementsSource>(rowCounts, std::move(bindings), std::move(imp.groups));
}

NATRON_NAMESPACE_EXIT
