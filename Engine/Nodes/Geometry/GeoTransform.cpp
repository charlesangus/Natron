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

#include "GeoTransform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoTopology.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {

enum TransformKindEnum {
    eTransformKindNone = 0,
    eTransformKindPoint,
    eTransformKindVector,
    eTransformKindNormal,
    eTransformKindQuaternion,
    eTransformKindMatrix3,
    eTransformKindMatrix4,
};

typedef std::array<double, 9> Matrix3;

struct TransformContext {
    GeoMatrix4 m;
    Matrix3 linear;
    // Cofactor of the linear part with the determinant's sign: the inverse transpose up to a
    // positive scale, defined even for a singular transform. Normals are renormalized after.
    Matrix3 normal;
    std::array<double, 4> rotation; // x, y, z, w
    bool keepVectorLengths;
};

Matrix3
linearPart(const GeoMatrix4& m)
{
    return { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
}

Matrix3
signedCofactor(const Matrix3& a)
{
    Matrix3 c = {
        a[4] * a[8] - a[5] * a[7],
        a[5] * a[6] - a[3] * a[8],
        a[3] * a[7] - a[4] * a[6],
        a[2] * a[7] - a[1] * a[8],
        a[0] * a[8] - a[2] * a[6],
        a[1] * a[6] - a[0] * a[7],
        a[1] * a[5] - a[2] * a[4],
        a[2] * a[3] - a[0] * a[5],
        a[0] * a[4] - a[1] * a[3],
    };
    const double det = a[0] * c[0] + a[1] * c[1] + a[2] * c[2];
    if (det < 0.) {
        for (double& v : c) {
            v = -v;
        }
    }

    return c;
}

std::array<double, 4>
quaternionFromRotation(const GeoMatrix4& r)
{
    const double r00 = r[0], r01 = r[1], r02 = r[2];
    const double r10 = r[4], r11 = r[5], r12 = r[6];
    const double r20 = r[8], r21 = r[9], r22 = r[10];
    const double trace = r00 + r11 + r22;
    std::array<double, 4> q;
    if (trace > 0.) {
        const double s = std::sqrt(trace + 1.) * 2.;
        q = { (r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25 * s };
    } else if (r00 > r11 && r00 > r22) {
        const double s = std::sqrt(1. + r00 - r11 - r22) * 2.;
        q = { 0.25 * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s };
    } else if (r11 > r22) {
        const double s = std::sqrt(1. + r11 - r00 - r22) * 2.;
        q = { (r01 + r10) / s, 0.25 * s, (r12 + r21) / s, (r02 - r20) / s };
    } else {
        const double s = std::sqrt(1. + r22 - r00 - r11) * 2.;
        q = { (r02 + r20) / s, (r12 + r21) / s, 0.25 * s, (r10 - r01) / s };
    }

    return q;
}

void
multiply3(const Matrix3& a,
          const double* v,
          double* out)
{
    for (int r = 0; r < 3; ++r) {
        out[r] = a[r * 3] * v[0] + a[r * 3 + 1] * v[1] + a[r * 3 + 2] * v[2];
    }
}

double
length3(const double* v)
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void
rescale3(double* v,
         double length)
{
    const double current = length3(v);
    if (current > 0.) {
        for (int c = 0; c < 3; ++c) {
            v[c] *= length / current;
        }
    }
}

void
transformTuple(TransformKindEnum kind,
               const TransformContext& ctx,
               double* v)
{
    double out[16];
    switch (kind) {
    case eTransformKindNone:
        return;
    case eTransformKindPoint:
        for (int r = 0; r < 3; ++r) {
            out[r] = ctx.m[r * 4] * v[0] + ctx.m[r * 4 + 1] * v[1] + ctx.m[r * 4 + 2] * v[2] + ctx.m[r * 4 + 3];
        }
        std::copy(out, out + 3, v);
        return;
    case eTransformKindVector: {
        const double length = length3(v);
        multiply3(ctx.linear, v, out);
        std::copy(out, out + 3, v);
        if (ctx.keepVectorLengths) {
            rescale3(v, length);
        }
        return;
    }
    case eTransformKindNormal: {
        const double length = length3(v);
        multiply3(ctx.normal, v, out);
        std::copy(out, out + 3, v);
        rescale3(v, length);
        return;
    }
    case eTransformKindQuaternion: {
        const double rx = ctx.rotation[0], ry = ctx.rotation[1], rz = ctx.rotation[2], rw = ctx.rotation[3];
        const double qx = v[0], qy = v[1], qz = v[2], qw = v[3];
        v[0] = rw * qx + rx * qw + ry * qz - rz * qy;
        v[1] = rw * qy - rx * qz + ry * qw + rz * qx;
        v[2] = rw * qz + rx * qy - ry * qx + rz * qw;
        v[3] = rw * qw - rx * qx - ry * qy - rz * qz;
        return;
    }
    case eTransformKindMatrix3:
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r * 3 + c] = ctx.linear[r * 3] * v[c] + ctx.linear[r * 3 + 1] * v[3 + c] + ctx.linear[r * 3 + 2] * v[6 + c];
            }
        }
        std::copy(out, out + 9, v);
        return;
    case eTransformKindMatrix4: {
        GeoMatrix4 m;
        std::copy(v, v + 16, m.begin());
        m = geoMatrixMultiply(ctx.m, m);
        std::copy(m.begin(), m.end(), v);
        return;
    }
    }
}

TransformKindEnum
transformKind(const GeoAttribute& attribute)
{
    if (attribute.isArray() || (attribute.storage() != eGeoStorageFloat32 && attribute.storage() != eGeoStorageFloat64)) {
        return eTransformKindNone;
    }
    const int ts = attribute.tupleSize();
    switch (attribute.typeInfo()) {
    case eGeoTypeInfoPoint:
        return ts >= 3 ? eTransformKindPoint : eTransformKindNone;
    case eGeoTypeInfoVector:
        return ts >= 3 ? eTransformKindVector : eTransformKindNone;
    case eGeoTypeInfoNormal:
        return ts >= 3 ? eTransformKindNormal : eTransformKindNone;
    case eGeoTypeInfoQuaternion:
        return ts == 4 ? eTransformKindQuaternion : eTransformKindNone;
    case eGeoTypeInfoMatrix:
        return ts == 9 ? eTransformKindMatrix3 : ts == 16 ? eTransformKindMatrix4
                                                          : eTransformKindNone;
    default:
        return eTransformKindNone;
    }
}

template <typename T>
void
transformElements(GeoAttribute& attribute,
                  std::span<const std::size_t> offsets,
                  TransformKindEnum kind,
                  const TransformContext& ctx)
{
    const std::size_t ts = std::size_t(attribute.tupleSize());
    std::vector<T> values(offsets.size() * ts);
    attribute.copyDense<T>(offsets, std::span<T>(values));
    double tuple[GeoAttribute::kMaxTupleSize];
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        T* element = values.data() + i * ts;
        for (std::size_t c = 0; c < ts; ++c) {
            tuple[c] = double(element[c]);
        }
        transformTuple(kind, ctx, tuple);
        for (std::size_t c = 0; c < ts; ++c) {
            element[c] = T(tuple[c]);
        }
    }
    attribute.scatterDense<T>(offsets, std::span<const T>(values));
}

bool
isBlank(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isspace((unsigned char)c);
    });
}

struct Selection {
    GeoElementMask points;
    GeoElementMask vertices;
    GeoElementMask primitives;
    bool everything = false;
};

bool
selectForTransform(const GeoDetail& detail,
                   const std::string& group,
                   GeoGroupTypeEnum groupType,
                   Selection* sel,
                   std::string* error)
{
    sel->everything = isBlank(group);
    if (sel->everything) {
        geoSelectElements(detail, eGeoAttribClassPoint, "", &sel->points, error);
        geoSelectElements(detail, eGeoAttribClassVertex, "", &sel->vertices, error);
        geoSelectElements(detail, eGeoAttribClassPrimitive, "", &sel->primitives, error);

        return true;
    }
    sel->vertices.assign(std::size_t(detail.vertexMap().offsetSize()), 0);
    if (groupType == eGeoGroupTypePoints) {
        if (!geoSelectElements(detail, eGeoAttribClassPoint, group, &sel->points, error)) {
            return false;
        }
        detail.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
            sel->vertices[std::size_t(v)] = sel->points[std::size_t(detail.vertexPoint(v))];
        });
        sel->primitives.assign(std::size_t(detail.primitiveMap().offsetSize()), 0);
        detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
            const GeoVertexRange range = detail.primitiveVertices(prim);
            bool all = range.count > 0;
            for (GeoOffset v = range.start; all && v < range.start + range.count; ++v) {
                all = sel->vertices[std::size_t(v)];
            }
            sel->primitives[std::size_t(prim)] = all;
        });

        return true;
    }
    if (!geoSelectElements(detail, eGeoAttribClassPrimitive, group, &sel->primitives, error)) {
        return false;
    }
    sel->points.assign(std::size_t(detail.pointMap().offsetSize()), 0);
    for (GeoOffset prim : geoMaskedOffsets(sel->primitives)) {
        const GeoVertexRange range = detail.primitiveVertices(prim);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            sel->vertices[std::size_t(v)] = 1;
            sel->points[std::size_t(detail.vertexPoint(v))] = 1;
        }
    }

    return true;
}

std::vector<std::size_t>
maskedSizeOffsets(const GeoElementMask& mask)
{
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i]) {
            offsets.push_back(i);
        }
    }

    return offsets;
}

bool
isPackedIntrinsic(GeoAttribClassEnum cls,
                  const std::string& name)
{
    return cls == eGeoAttribClassPrimitive && name.rfind("packed", 0) == 0;
}

template <typename T>
T
valueAt(const std::weak_ptr<Knob<T>>& knob,
        T fallback,
        double time,
        ViewSpec view,
        int dimension = 0)
{
    if (const std::shared_ptr<Knob<T>> locked = knob.lock()) {
        return locked->getValueAtTime(time, dimension, view);
    }

    return fallback;
}

KnobDoublePtr
createVectorKnob(GeoTransform* holder,
                 const KnobPagePtr& page,
                 const QString& label,
                 const char* name,
                 const QString& toolTip,
                 int dimension,
                 double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString(), dimension);
    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    for (int d = 0; d < dimension; ++d) {
        knob->setDefaultValue(defaultValue, d);
    }
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

std::optional<GeoMatrix4>
GeoTransform::matrix(const GeoTransformParams& params)
{
    const std::array<double, 3> scale = { params.scale[0] * params.uniformScale,
                                          params.scale[1] * params.uniformScale,
                                          params.scale[2] * params.uniformScale };
    const GeoMatrix4 m = geoComposeTransform(params.translate, params.rotate, scale, params.pivot, params.rotateOrder);
    if (!params.invert) {
        return m;
    }

    return geoMatrixInverse(m);
}

bool
GeoTransform::apply(GeoDetailBuilder& builder,
                    const GeoTransformParams& params,
                    std::string* error)
{
    const std::optional<GeoMatrix4> m = matrix(params);
    if (!m) {
        if (error) {
            *error = "the transform has no inverse: a scale is zero";
        }

        return false;
    }
    GeoMatrix4 rotation = geoComposeTransform({ 0., 0., 0. }, params.rotate, { 1., 1., 1. }, { 0., 0., 0. }, params.rotateOrder);
    if (params.invert) {
        GeoMatrix4 transposed = rotation;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                transposed[r * 4 + c] = rotation[c * 4 + r];
            }
        }
        rotation = transposed;
    }

    return applyMatrix(builder, *m, rotation, params.group, params.groupType, params.attributes, params.keepVectorLengths, error);
}

bool
GeoTransform::applyMatrix(GeoDetailBuilder& builder,
                          const GeoMatrix4& transform,
                          const GeoMatrix4& rotation,
                          const std::string& group,
                          GeoGroupTypeEnum groupType,
                          const std::string& attributes,
                          bool keepVectorLengths,
                          std::string* error)
{
    Selection sel;
    if (!selectForTransform(builder.detail(), group, groupType, &sel, error)) {
        return false;
    }
    TransformContext ctx;
    ctx.m = transform;
    ctx.linear = linearPart(transform);
    ctx.normal = signedCofactor(ctx.linear);
    ctx.rotation = quaternionFromRotation(rotation);
    ctx.keepVectorLengths = keepVectorLengths;

    const std::pair<GeoAttribClassEnum, const GeoElementMask*> classes[] = {
        { eGeoAttribClassPoint, &sel.points },
        { eGeoAttribClassVertex, &sel.vertices },
        { eGeoAttribClassPrimitive, &sel.primitives },
        { eGeoAttribClassDetail, nullptr },
    };
    for (const auto& [cls, mask] : classes) {
        std::vector<std::size_t> offsets;
        if (mask) {
            offsets = maskedSizeOffsets(*mask);
        } else if (sel.everything) {
            offsets.push_back(0);
        }
        if (offsets.empty()) {
            continue;
        }
        std::vector<std::string> names;
        const GeoDetail& detail = builder.detail();
        for (std::size_t i = 0; i < detail.attributeCount(cls); ++i) {
            const GeoAttribute& a = detail.attribute(cls, i);
            const bool isP = cls == eGeoAttribClassPoint && a.name() == GeoDetail::kPositionName;
            if (transformKind(a) == eTransformKindNone || isPackedIntrinsic(cls, a.name())) {
                continue;
            }
            if (isP || geoNameMatchesPattern(attributes, a.name())) {
                names.push_back(a.name());
            }
        }
        for (const std::string& name : names) {
            GeoAttribute* a = builder.attributeForWriting(cls, name);
            if (!a) {
                continue;
            }
            const TransformKindEnum kind = transformKind(*a);
            if (a->storage() == eGeoStorageFloat64) {
                transformElements<double>(*a, offsets, kind, ctx);
            } else {
                transformElements<float>(*a, offsets, kind, ctx);
            }
        }
    }

    const GeoDetail& detail = builder.detail();
    std::vector<std::pair<GeoOffset, GeoMatrix4>> packed;
    for (GeoOffset prim : geoMaskedOffsets(sel.primitives)) {
        if (geoPrimitiveIsPacked(detail.primitiveType(prim))) {
            packed.emplace_back(prim, geoMatrixMultiply(transform, geoPackedTransform(detail, prim)));
        }
    }
    for (const auto& [prim, m] : packed) {
        geoSetPackedTransform(builder, prim, m);
    }

    return true;
}

GeoTransform::GeoTransform(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoTransform::~GeoTransform()
{
}

const NodeSpec*
GeoTransform::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoTransform::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoTransformParamGroup);
    group->setHintToolTip(tr("The part of the geometry to transform, as a group pattern over the group type: numbers "
                             "and ranges (0-10, 0-20:2), group names, @attribute comparisons, ^ to remove from the "
                             "selection and ! to invert a term. Empty for everything.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr groupType = createKnob<KnobChoice>(tr("Group Type").toStdString());
    groupType->setName(kGeoTransformParamGroupType);
    groupType->setHintToolTip(tr("Whether the group pattern selects points or primitives.").toStdString());
    std::vector<ChoiceOption> types;
    types.push_back(ChoiceOption("points", tr("Points").toStdString(),
                                 tr("The selected points and the vertices and packed primitives on them.").toStdString()));
    types.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(),
                                 tr("The selected primitives and their vertices and points.").toStdString()));
    groupType->populateChoices(types);
    groupType->setDefaultValue((int)eGeoGroupTypePrimitives);
    groupType->setAnimationEnabled(false);
    page->addKnob(groupType);
    _groupType = groupType;

    _translate = createVectorKnob(this, page, tr("Translate"), kGeoTransformParamTranslate,
                                  tr("The move along the X, Y and Z axes, applied last."), 3, 0.);
    _rotate = createVectorKnob(this, page, tr("Rotate"), kGeoTransformParamRotate,
                               tr("Rotations about the X, Y and Z axes, in degrees, applied in the rotate order."), 3, 0.);
    _scale = createVectorKnob(this, page, tr("Scale"), kGeoTransformParamScale,
                              tr("Scale along the X, Y and Z axes, multiplied by the uniform scale."), 3, 1.);
    _uniformScale = createVectorKnob(this, page, tr("Uniform Scale"), kGeoTransformParamUniformScale,
                                     tr("Scale along every axis, multiplied into the scale."), 1, 1.);

    KnobChoicePtr rotateOrder = createKnob<KnobChoice>(tr("Rotate Order").toStdString());
    rotateOrder->setName(kGeoTransformParamRotateOrder);
    rotateOrder->setHintToolTip(tr("The order the three rotations are applied in: XYZ turns about X first, then Y, "
                                   "then Z.")
                                    .toStdString());
    std::vector<ChoiceOption> orders;
    for (const char* order : { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" }) {
        orders.push_back(ChoiceOption(order, order, ""));
    }
    rotateOrder->populateChoices(orders);
    rotateOrder->setDefaultValue((int)eGeoRotateOrderXYZ);
    rotateOrder->setAnimationEnabled(false);
    page->addKnob(rotateOrder);
    _rotateOrder = rotateOrder;

    _pivot = createVectorKnob(this, page, tr("Pivot"), kGeoTransformParamPivot,
                              tr("The point the rotation and scale are about."), 3, 0.);

    KnobButtonPtr centre = createKnob<KnobButton>(tr("Centre Pivot").toStdString());
    centre->setName(kGeoTransformParamCentrePivot);
    centre->setHintToolTip(tr("Sets the pivot to the centre of the bounds of the points the group selects, at the "
                              "current frame.")
                               .toStdString());
    page->addKnob(centre);
    _centrePivot = centre;

    KnobBoolPtr invert = createKnob<KnobBool>(tr("Invert Transform").toStdString());
    invert->setName(kGeoTransformParamInvertTransform);
    invert->setHintToolTip(tr("Applies the inverse of the transform, undoing what the same settings do.").toStdString());
    invert->setDefaultValue(false);
    invert->setAnimationEnabled(false);
    page->addKnob(invert);
    _invert = invert;

    KnobStringPtr attributes = createKnob<KnobString>(tr("Attributes").toStdString());
    attributes->setName(kGeoTransformParamAttributes);
    attributes->setHintToolTip(tr("The attributes besides P that follow the transform, as space-separated name "
                                  "patterns (* matches any run of characters, ^ excludes). Only attributes tagged as "
                                  "points, vectors, normals, quaternions or matrices can follow it.")
                                   .toStdString());
    attributes->setDefaultValue("*");
    attributes->setAnimationEnabled(false);
    page->addKnob(attributes);
    _attributes = attributes;

    KnobBoolPtr keep = createKnob<KnobBool>(tr("Keep Vector Lengths").toStdString());
    keep->setName(kGeoTransformParamKeepVectorLengths);
    keep->setHintToolTip(tr("Vector attributes keep their length and only turn; otherwise the scale stretches "
                            "them too. Normals always keep their length.")
                             .toStdString());
    keep->setDefaultValue(true);
    keep->setAnimationEnabled(false);
    page->addKnob(keep);
    _keepVectorLengths = keep;
}

bool
GeoTransform::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    const KnobButtonPtr button = _centrePivot.lock();
    if (!button || (k != button.get())) {
        return false;
    }
    std::string error;
    if (!centrePivot(&error)) {
        message(eMessageTypeWarning, error);
    }

    return true;
}

bool
GeoTransform::centrePivot(std::string* errorMessage)
{
    const double time = getCurrentTime();
    const ViewIdx view = getCurrentView();
    GeoDetailConstPtr input;
    if (const EffectInstancePtr source = getInput(0)) {
        std::string inputError;
        if (source->evaluateGeometry(EvaluateGeometryArgs(time, view), &input, &inputError) != eRenderRoIRetCodeOk) {
            *errorMessage = inputError.empty() ? tr("The input geometry could not be computed.").toStdString() : inputError;

            return false;
        }
    }
    const KnobDoublePtr pivot = _pivot.lock();
    const GeoTransformParams params = readParams(time, view);
    Selection sel;
    if (!pivot || !input) {
        *errorMessage = tr("There is no geometry to centre the pivot on.").toStdString();

        return false;
    }
    if (!selectForTransform(*input, params.group, params.groupType, &sel, errorMessage)) {
        return false;
    }
    std::array<double, 3> lo = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
    std::array<double, 3> hi = { -lo[0], -lo[1], -lo[2] };
    bool any = false;
    for (GeoOffset point : geoMaskedOffsets(sel.points)) {
        const std::array<double, 3> p = input->pointPosition(point);
        for (int d = 0; d < 3; ++d) {
            lo[d] = std::min(lo[d], p[d]);
            hi[d] = std::max(hi[d], p[d]);
        }
        any = true;
    }
    if (!any) {
        *errorMessage = tr("The group selects no point to centre the pivot on.").toStdString();

        return false;
    }
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    for (int d = 0; d < 3; ++d) {
        pivot->setValueFromPlugin((lo[d] + hi[d]) * 0.5, ViewSpec::all(), d);
    }
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);

    return true;
}

GeoTransformParams
GeoTransform::readParams(double time,
                         ViewSpec view) const
{
    GeoTransformParams p;
    p.group = valueAt<std::string>(_group, p.group, time, view);
    const int groupType = valueAt<int>(_groupType, (int)p.groupType, time, view);
    p.groupType = groupType == eGeoGroupTypePoints ? eGeoGroupTypePoints : eGeoGroupTypePrimitives;
    for (int d = 0; d < 3; ++d) {
        p.translate[d] = valueAt<double>(_translate, p.translate[d], time, view, d);
        p.rotate[d] = valueAt<double>(_rotate, p.rotate[d], time, view, d);
        p.scale[d] = valueAt<double>(_scale, p.scale[d], time, view, d);
        p.pivot[d] = valueAt<double>(_pivot, p.pivot[d], time, view, d);
    }
    p.uniformScale = valueAt<double>(_uniformScale, p.uniformScale, time, view);
    const int order = valueAt<int>(_rotateOrder, (int)p.rotateOrder, time, view);
    if (order >= eGeoRotateOrderXYZ && order <= eGeoRotateOrderZYX) {
        p.rotateOrder = (GeoRotateOrderEnum)order;
    }
    p.invert = valueAt<bool>(_invert, p.invert, time, view);
    p.attributes = valueAt<std::string>(_attributes, p.attributes, time, view);
    p.keepVectorLengths = valueAt<bool>(_keepVectorLengths, p.keepVectorLengths, time, view);

    return p;
}

StatusEnum
GeoTransform::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    const GeoTransformParams params = readParams(args.time, args.view);

    return renderGeometryFromInput(args, 0, [&params](GeoDetailBuilder& detail, std::string* error) {
        try {
            return apply(detail, params, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        } }, outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
