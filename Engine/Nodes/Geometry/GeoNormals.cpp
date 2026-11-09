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

#include "GeoNormals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {

typedef std::array<double, 3> Vec3;

Vec3
sub(const Vec3& a,
    const Vec3& b)
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

double
dot(const Vec3& a,
    const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3
normalized(const Vec3& v)
{
    const double len = std::sqrt(dot(v, v));
    if (len <= 0.) {
        return { 0., 0., 0. };
    }

    return { v[0] / len, v[1] / len, v[2] / len };
}

template <typename PointAt>
Vec3
newellNormal(const GeoDetail& detail,
             GeoOffset primitive,
             PointAt&& pointAt)
{
    Vec3 n { 0., 0., 0. };
    if (detail.primitiveType(primitive) != eGeoPrimitivePolygon) {
        return n;
    }
    const GeoVertexRange range = detail.primitiveVertices(primitive);
    if (range.count < 3) {
        return n;
    }
    for (GeoOffset i = 0; i < range.count; ++i) {
        const Vec3 a = pointAt(detail.vertexPoint(range.start + i));
        const Vec3 b = pointAt(detail.vertexPoint(range.start + (i + 1) % range.count));
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }

    return n;
}

class NormalBuilder {
public:
    NormalBuilder(const GeoDetail& detail,
                  const GeoNormalsParams& params,
                  const GeoElementMask& selected)
        : _detail(detail)
        , _params(params)
        , _selected(selected)
        , _positions(geoPointPositions(detail))
        , _areaNormals(selected.size())
        , _unitNormals(selected.size())
    {
        for (std::size_t prim = 0; prim < selected.size(); ++prim) {
            if (selected[prim]) {
                _areaNormals[prim] = newellNormal(detail, GeoOffset(prim), [this](GeoOffset p) {
                    return _positions[std::size_t(p)];
                });
                _unitNormals[prim] = normalized(_areaNormals[prim]);
            }
        }
    }

    bool contributes(GeoOffset prim) const
    {
        return _selected[std::size_t(prim)] && _detail.primitiveType(prim) == eGeoPrimitivePolygon && _detail.primitiveVertices(prim).count >= 3;
    }

    const Vec3& unitNormal(GeoOffset prim) const
    {
        return _unitNormals[std::size_t(prim)];
    }

    // What the polygon owning @p vertex adds to the normal at that vertex's point.
    Vec3 contribution(GeoOffset vertex) const
    {
        const GeoOffset prim = _detail.vertexPrimitive(vertex);
        switch (_params.weighting) {
        case eGeoNormalsWeightingEqual:
            return _unitNormals[std::size_t(prim)];
        case eGeoNormalsWeightingArea:
            return _areaNormals[std::size_t(prim)];
        case eGeoNormalsWeightingAngle: {
            const double angle = cornerAngle(prim, vertex);
            const Vec3& n = _unitNormals[std::size_t(prim)];

            return { n[0] * angle, n[1] * angle, n[2] * angle };
        }
        }

        return _unitNormals[std::size_t(prim)];
    }

private:
    double cornerAngle(GeoOffset prim,
                       GeoOffset vertex) const
    {
        const GeoVertexRange range = _detail.primitiveVertices(prim);
        const GeoOffset i = vertex - range.start;
        const Vec3& p = position(_detail.vertexPoint(vertex));
        const Vec3 a = normalized(sub(position(_detail.vertexPoint(range.start + (i + range.count - 1) % range.count)), p));
        const Vec3 b = normalized(sub(position(_detail.vertexPoint(range.start + (i + 1) % range.count)), p));

        return std::acos(std::clamp(dot(a, b), -1., 1.));
    }

    const Vec3& position(GeoOffset point) const
    {
        return _positions[std::size_t(point)];
    }

    const GeoDetail& _detail;
    const GeoNormalsParams& _params;
    const GeoElementMask& _selected;
    std::vector<Vec3> _positions;
    std::vector<Vec3> _areaNormals;
    std::vector<Vec3> _unitNormals;
};

GeoAttributeDesc
normalDesc()
{
    GeoAttributeDesc desc;
    desc.name = "N";
    desc.storage = eGeoStorageFloat32;
    desc.tupleSize = 3;
    desc.typeInfo = eGeoTypeInfoNormal;

    return desc;
}

void
writeNormals(GeoAttribute& attribute,
             const std::vector<std::size_t>& offsets,
             const std::vector<Vec3>& normals,
             bool reverse)
{
    std::vector<float> values(offsets.size() * 3);
    const double sign = reverse ? -1. : 1.;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        const Vec3 n = normalized(normals[i]);
        for (int c = 0; c < 3; ++c) {
            values[i * 3 + std::size_t(c)] = float(sign * n[std::size_t(c)]);
        }
    }
    attribute.scatterDense<float>(std::span<const std::size_t>(offsets), std::span<const float>(values));
}

template <typename T>
T
valueAt(const std::weak_ptr<Knob<T>>& knob,
        T fallback,
        double time,
        ViewSpec view)
{
    if (const std::shared_ptr<Knob<T>> locked = knob.lock()) {
        return locked->getValueAtTime(time, 0, view);
    }

    return fallback;
}
} // anonymous namespace

std::array<double, 3>
GeoNormals::polygonAreaNormal(const GeoDetail& detail,
                              GeoOffset primitive)
{
    return newellNormal(detail, primitive, [&detail](GeoOffset p) {
        return detail.pointPosition(p);
    });
}

bool
GeoNormals::computeNormals(GeoDetailBuilder& builder,
                           const GeoNormalsParams& params,
                           std::string* error)
{
    GeoElementMask selected;
    if (!geoSelectElements(builder.detail(), eGeoAttribClassPrimitive, params.group, &selected, error)) {
        return false;
    }
    const GeoAttribClassEnum targetClass = params.target == eGeoNormalsTargetVertices ? eGeoAttribClassVertex
        : params.target == eGeoNormalsTargetPrimitives                                ? eGeoAttribClassPrimitive
                                                                                      : eGeoAttribClassPoint;
    if (params.target == eGeoNormalsTargetVertices && !builder.detail().findAttribute(eGeoAttribClassVertex, "N")) {
        // Seeded from the point normals so that vertices outside the group keep what they showed.
        if (const GeoAttribute* pointN = builder.detail().findAttribute(eGeoAttribClassPoint, "N")) {
            const GeoAttribute source = *pointN;
            const GeoDetail& d = builder.detail();
            const std::vector<std::size_t> vertices = geoLiveOffsets(d.vertexMap());
            std::vector<std::size_t> points(vertices.size());
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                points[i] = std::size_t(d.vertexPoint(GeoOffset(vertices[i])));
            }
            GeoAttribute& seeded = builder.addAttribute(eGeoAttribClassVertex, normalDesc());
            geoCopyAttributeElements(source, points, seeded, vertices);
        }
    }
    for (GeoAttribClassEnum cls : { eGeoAttribClassPoint, eGeoAttribClassVertex, eGeoAttribClassPrimitive }) {
        if (cls != targetClass) {
            builder.removeAttribute(cls, "N");
        }
    }
    GeoAttribute& normals = geoConformAttribute(builder, targetClass, normalDesc());
    const GeoDetail& detail = builder.detail();
    const NormalBuilder faces(detail, params, selected);

    std::vector<std::size_t> offsets;
    std::vector<Vec3> sums;
    switch (params.target) {
    case eGeoNormalsTargetPoints: {
        std::vector<Vec3> accum(std::size_t(detail.pointMap().offsetSize()), Vec3 { 0., 0., 0. });
        std::vector<char> touched(accum.size(), 0);
        detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
            if (!faces.contributes(prim)) {
                return;
            }
            const GeoVertexRange range = detail.primitiveVertices(prim);
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                const std::size_t point = std::size_t(detail.vertexPoint(v));
                const Vec3 c = faces.contribution(v);
                for (int k = 0; k < 3; ++k) {
                    accum[point][std::size_t(k)] += c[std::size_t(k)];
                }
                touched[point] = 1;
            }
        });
        for (std::size_t p = 0; p < accum.size(); ++p) {
            if (touched[p]) {
                offsets.push_back(p);
                sums.push_back(accum[p]);
            }
        }
        break;
    }
    case eGeoNormalsTargetVertices: {
        const std::shared_ptr<const GeoReverseTopology> reverse = detail.reverseTopology();
        const double cosCusp = std::cos(std::clamp(params.cuspAngleDegrees, 0., 180.) * M_PI / 180.);
        detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
            if (!faces.contributes(prim)) {
                return;
            }
            const Vec3& own = faces.unitNormal(prim);
            const GeoVertexRange range = detail.primitiveVertices(prim);
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                Vec3 sum { 0., 0., 0. };
                for (GeoOffset w : reverse->pointVertices(detail.vertexPoint(v))) {
                    const GeoOffset other = detail.vertexPrimitive(w);
                    if (!faces.contributes(other)) {
                        continue;
                    }
                    // A small tolerance keeps faces exactly at the cusp angle smooth.
                    if (other != prim && dot(own, faces.unitNormal(other)) < cosCusp - 1e-9) {
                        continue;
                    }
                    const Vec3 c = faces.contribution(w);
                    for (int k = 0; k < 3; ++k) {
                        sum[std::size_t(k)] += c[std::size_t(k)];
                    }
                }
                offsets.push_back(std::size_t(v));
                sums.push_back(sum);
            }
        });
        break;
    }
    case eGeoNormalsTargetPrimitives:
        detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
            if (faces.contributes(prim)) {
                offsets.push_back(std::size_t(prim));
                sums.push_back(faces.unitNormal(prim));
            }
        });
        break;
    }
    writeNormals(normals, offsets, sums, params.reverse);

    return true;
}

GeoNormals::GeoNormals(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoNormals::~GeoNormals()
{
}

const NodeSpec*
GeoNormals::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoNormals::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoNormalsParamGroup);
    group->setHintToolTip(tr("The polygons whose normals are computed, as a primitive group pattern: numbers "
                             "and ranges (0-10, 0-20:2), group names, @attribute comparisons, ^ to remove "
                             "from the selection and ! to invert a term. Empty for every polygon.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr target = createKnob<KnobChoice>(tr("Add Normals To").toStdString());
    target->setName(kGeoNormalsParamAddNormalsTo);
    target->setHintToolTip(tr("The element class that receives N. N is removed from the other classes.").toStdString());
    std::vector<ChoiceOption> targets;
    targets.push_back(ChoiceOption("points", tr("Points").toStdString(),
                                   tr("One smooth normal per point, averaged over the polygons around it.").toStdString()));
    targets.push_back(ChoiceOption("vertices", tr("Vertices").toStdString(),
                                   tr("One normal per polygon corner; edges sharper than the cusp angle stay hard.").toStdString()));
    targets.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(),
                                   tr("One flat normal per polygon.").toStdString()));
    target->populateChoices(targets);
    target->setDefaultValue((int)eGeoNormalsTargetPoints);
    target->setAnimationEnabled(false);
    page->addKnob(target);
    _addNormalsTo = target;

    KnobChoicePtr weighting = createKnob<KnobChoice>(tr("Weighting").toStdString());
    weighting->setName(kGeoNormalsParamWeighting);
    weighting->setHintToolTip(tr("How much each polygon around a point counts in its normal.").toStdString());
    std::vector<ChoiceOption> weights;
    weights.push_back(ChoiceOption("equal", tr("Equal").toStdString(), tr("Every polygon counts the same.").toStdString()));
    weights.push_back(ChoiceOption("angle", tr("Corner Angle").toStdString(),
                                   tr("Each polygon counts by its corner angle at the point.").toStdString()));
    weights.push_back(ChoiceOption("area", tr("Area").toStdString(), tr("Each polygon counts by its area.").toStdString()));
    weighting->populateChoices(weights);
    weighting->setDefaultValue((int)eGeoNormalsWeightingArea);
    weighting->setAnimationEnabled(false);
    page->addKnob(weighting);
    _weighting = weighting;

    KnobDoublePtr cusp = createKnob<KnobDouble>(tr("Cusp Angle").toStdString());
    cusp->setName(kGeoNormalsParamCuspAngle);
    cusp->setHintToolTip(tr("For vertex normals: adjacent polygons whose normals differ by more than this "
                            "angle, in degrees, are not averaged, leaving a hard edge between them.")
                             .toStdString());
    cusp->setDefaultValue(60.);
    cusp->setMinimum(0.);
    cusp->setMaximum(180.);
    cusp->setDisplayMinimum(0.);
    cusp->setDisplayMaximum(180.);
    page->addKnob(cusp);
    _cuspAngle = cusp;

    KnobBoolPtr reverse = createKnob<KnobBool>(tr("Reverse").toStdString());
    reverse->setName(kGeoNormalsParamReverse);
    reverse->setHintToolTip(tr("Points the computed normals the other way.").toStdString());
    reverse->setDefaultValue(false);
    reverse->setAnimationEnabled(false);
    page->addKnob(reverse);
    _reverse = reverse;

    updateVisibility();
}

void
GeoNormals::updateVisibility()
{
    const KnobChoicePtr target = _addNormalsTo.lock();
    const KnobDoublePtr cusp = _cuspAngle.lock();
    if (target && cusp) {
        cusp->setSecret(target->getValue() != (int)eGeoNormalsTargetVertices);
    }
}

void
GeoNormals::onKnobsLoaded()
{
    updateVisibility();
}

bool
GeoNormals::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr target = _addNormalsTo.lock();
    if (target && (k == target.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

StatusEnum
GeoNormals::renderGeometry(const GeoRenderActionArgs& args,
                           GeoDetailConstPtr* outputDetail,
                           std::string* errorMessage)
{
    GeoNormalsParams params;
    params.group = valueAt<std::string>(_group, params.group, args.time, args.view);
    const int target = valueAt<int>(_addNormalsTo, (int)params.target, args.time, args.view);
    if (target >= eGeoNormalsTargetPoints && target <= eGeoNormalsTargetPrimitives) {
        params.target = (GeoNormalsTargetEnum)target;
    }
    const int weighting = valueAt<int>(_weighting, (int)params.weighting, args.time, args.view);
    if (weighting >= eGeoNormalsWeightingEqual && weighting <= eGeoNormalsWeightingArea) {
        params.weighting = (GeoNormalsWeightingEnum)weighting;
    }
    params.cuspAngleDegrees = valueAt<double>(_cuspAngle, params.cuspAngleDegrees, args.time, args.view);
    params.reverse = valueAt<bool>(_reverse, params.reverse, args.time, args.view);

    return renderGeometryFromInput(args, 0, [&params](GeoDetailBuilder& detail, std::string* error) {
        try {
            return computeNormals(detail, params, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        } }, outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
