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

#include "GeoUVProject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoPage.h"
#include "Engine/Geo/GeoTopology.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/InputConnectSwitch.h"
#include "Engine/Nodes/Scene/GeoCameraProjection.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
typedef std::array<double, 3> Vec3;
typedef std::array<double, 2> Vec2;

double
wrapUnit(double x)
{
    x -= std::floor(x);

    return (x >= 1.) ? 0. : x;
}

// The longitude about Y, +Z at 0.5 and the seam at -Z.
double
longitude(const Vec3& q)
{
    return wrapUnit(0.5 + std::atan2(q[0], q[2]) / (2. * M_PI));
}

// A point's place on the projection geometry. A coordinate is undefined where the projection
// collapses, at a pole; those are the ones Fix Poles replaces.
struct Place {
    Vec2 s = { 0., 0. };
    std::array<bool, 2> undefined = { false, false };
};

class Projector {
public:
    Projector(const GeoUVProjectParams& params,
              const GeoMatrix4& toLocal)
        : _params(params)
        , _toLocal(toLocal)
        , _ringRadius((0.5 + params.innerRadius) * 0.5)
    {
        // Only the round projections repeat: their longitude, and the torus's tube angle too.
        const GeoUVProjectionEnum p = params.projection;
        _periodic[0] = (p == eGeoUVProjectionPolar) || (p == eGeoUVProjectionCylindrical) || (p == eGeoUVProjectionToroidal);
        _periodic[1] = (p == eGeoUVProjectionToroidal);
        _hasPoles = _periodic[0] || (p == eGeoUVProjectionPlasticWrap);
    }

    bool periodic(int c) const
    {
        return _periodic[(std::size_t)c];
    }

    bool hasPoles() const
    {
        return _hasPoles;
    }

    Place operator()(const Vec3& p) const
    {
        Place out;
        if (_params.projection == eGeoUVProjectionCamera) {
            out.s = cameraPlace(p);

            return out;
        }
        const Vec3 q = geoTransformPoint(_toLocal, p);
        const double radial = std::hypot(q[0], q[2]);
        const double poleRadius = _params.poleRadius;
        switch (_params.projection) {
        case eGeoUVProjectionPolar:
            out.s = { longitude(q), 0.5 + std::atan2(q[1], radial) / M_PI };
            out.undefined[0] = radial < poleRadius;
            break;
        case eGeoUVProjectionCylindrical:
            out.s = { longitude(q), q[1] + 0.5 };
            out.undefined[0] = radial < poleRadius;
            break;
        case eGeoUVProjectionToroidal: {
            const double fromRing = radial - _ringRadius;
            out.s = { longitude(q), wrapUnit(0.5 + std::atan2(q[1], fromRing) / (2. * M_PI)) };
            out.undefined[0] = radial < poleRadius;
            out.undefined[1] = std::hypot(fromRing, q[1]) < poleRadius;
            break;
        }
        case eGeoUVProjectionPlasticWrap: {
            const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            const bool atBackPole = (q[2] < 0.) && (std::hypot(q[0], q[1]) < poleRadius);
            out.undefined = { atBackPole || (length <= 0.), atBackPole || (length <= 0.) };
            if (length <= 0.) {
                out.s = { 0.5, 0.5 };
                break;
            }
            const double dx = q[0] / length;
            const double dy = q[1] / length;
            const double fromFront = std::acos(std::clamp(q[2] / length, -1., 1.)) / M_PI;
            // The max norm rather than the length, so the back pole reaches the square's edge and
            // the whole texture is used, not only the disc inside it.
            const double spread = std::max(std::abs(dx), std::abs(dy));
            if (spread < 1e-12) {
                out.s = { 0.5, 0.5 };
                break;
            }
            out.s = { 0.5 + 0.5 * fromFront * dx / spread, 0.5 + 0.5 * fromFront * dy / spread };
            break;
        }
        case eGeoUVProjectionOrthographic:
        default:
            out.s = { q[0] + 0.5, q[1] + 0.5 };
            break;
        } // switch

        return out;
    } // ()

    Vec2 toUV(const Vec2& s) const
    {
        const double u = (s[0] - _params.uRange[0]) / (_params.uRange[1] - _params.uRange[0]) - 0.5;
        const double v = (s[1] - _params.vRange[0]) / (_params.vRange[1] - _params.vRange[0]) - 0.5;
        const double a = _params.angle * M_PI / 180.;
        const double c = std::cos(a);
        const double sn = std::sin(a);

        return { 0.5 + c * u - sn * v, 0.5 + sn * u + c * v };
    }

private:
    Vec2 cameraPlace(const Vec3& p) const
    {
        const std::array<double, 16>& m = _params.cameraWorldToFrame;
        double r[4];
        for (int c = 0; c < 4; ++c) {
            r[c] = p[0] * m[(std::size_t)c] + p[1] * m[(std::size_t)(4 + c)] + p[2] * m[(std::size_t)(8 + c)] + m[(std::size_t)(12 + c)];
        }
        // A point in the camera's plane has no image; it is pinned to the frame's centre rather
        // than dividing by zero.
        if (std::abs(r[3]) < 1e-12) {
            return { 0.5, 0.5 };
        }

        return { r[0] / r[3], r[1] / r[3] };
    }

    const GeoUVProjectParams& _params;
    GeoMatrix4 _toLocal;
    double _ringRadius;
    std::array<bool, 2> _periodic = { false, false };
    bool _hasPoles = false;
};

// The mean of values, those of a periodic coordinate first unwrapped onto the first so a pair
// straddling the seam averages to the seam rather than to the far side.
double
meanOf(const std::vector<double>& values,
       bool periodic)
{
    double sum = 0.;
    for (double v : values) {
        sum += periodic ? v + std::round(values.front() - v) : v;
    }

    return sum / (double)values.size();
}

// One polygon's or polyline's places, fixed as the params ask. A polygon wraps round; a polyline
// does not.
void
fixPrimitivePlaces(const Projector& projector,
                   const GeoUVProjectParams& params,
                   bool closed,
                   std::vector<Place>& places)
{
    const int n = (int)places.size();
    if (n == 0) {
        return;
    }
    for (int c = 0; c < 2; ++c) {
        auto defined = [&](int i) {
            return !(params.fixPoles && places[(std::size_t)i].undefined[(std::size_t)c]);
        };
        if (projector.periodic(c) && params.fixBoundarySeams) {
            int first = 0;
            while ((first < n) && !defined(first)) {
                ++first;
            }
            if (first == n) {
                continue;
            }
            double previous = places[(std::size_t)first].s[(std::size_t)c];
            double lowest = previous;
            for (int k = 1; k < n; ++k) {
                const int i = (first + k) % n;
                if (!defined(i)) {
                    continue;
                }
                double& s = places[(std::size_t)i].s[(std::size_t)c];
                s += std::round(previous - s);
                previous = s;
                lowest = std::min(lowest, s);
            }
            const double shift = -std::floor(lowest);
            for (int i = 0; i < n; ++i) {
                if (defined(i)) {
                    places[(std::size_t)i].s[(std::size_t)c] += shift;
                }
            }
        }
    }
    if (!params.fixPoles || !projector.hasPoles()) {
        return;
    }
    const std::vector<Place> fixed = places;
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < n; ++i) {
            if (!fixed[(std::size_t)i].undefined[(std::size_t)c]) {
                continue;
            }
            std::vector<double> neighbours;
            for (int step : { -1, 1 }) {
                for (int k = 1; k < n; ++k) {
                    const int j = i + step * k;
                    if (!closed && ((j < 0) || (j >= n))) {
                        break;
                    }
                    const Place& other = fixed[(std::size_t)((j % n + n) % n)];
                    if (!other.undefined[(std::size_t)c]) {
                        neighbours.push_back(other.s[(std::size_t)c]);
                        break;
                    }
                }
            }
            if (neighbours.empty()) {
                continue;
            }
            const bool periodic = projector.periodic(c);
            const double mean = meanOf(neighbours, periodic && !params.fixBoundarySeams);
            places[(std::size_t)i].s[(std::size_t)c] = (periodic && !params.fixBoundarySeams) ? wrapUnit(mean) : mean;
        }
    }
} // fixPrimitivePlaces

bool
primitiveTakesUVs(const GeoDetail& d,
                  GeoOffset primitive)
{
    const GeoPrimitiveTypeEnum type = d.primitiveType(primitive);

    return (type == eGeoPrimitivePolygon) || (type == eGeoPrimitivePolyline);
}

// The class-appropriate selection: primitives for the vertex type, points for the point type.
bool
selectForProjection(const GeoDetail& d,
                    const GeoUVProjectParams& params,
                    GeoElementMask* selected,
                    std::string* errorMessage)
{
    const bool onPoints = (params.groupAttribType == eGeoUVProjectGroupAttribTypePoints);

    return geoSelectElements(d, onPoints ? eGeoAttribClassPoint : eGeoAttribClassPrimitive, params.group, selected, errorMessage);
}

// The vertex attribute to write uv into. An existing vertex attribute of two or more numeric
// components keeps its storage and its other components; anything else makes a float32 x3
// one, seeded from a point attribute of that name, which it would otherwise shadow on the
// vertices outside the group.
GeoAttribute*
uvAttributeForWriting(GeoDetailBuilder& detail,
                      const std::string& name)
{
    if (const GeoAttribute* existing = detail.detail().findAttribute(eGeoAttribClassVertex, name)) {
        if (geoIsNumeric(*existing) && !existing->desc().isArray && (existing->desc().tupleSize >= 2)) {
            return detail.attributeForWriting(eGeoAttribClassVertex, name);
        }
    }
    const GeoAttribute* pointUV = detail.detail().findAttribute(eGeoAttribClassPoint, name);
    std::vector<std::pair<std::size_t, std::array<double, 3>>> seeded;
    if (pointUV && geoIsNumeric(*pointUV) && !pointUV->desc().isArray) {
        const GeoDetail& d = detail.detail();
        const int components = std::min(3, (int)pointUV->desc().tupleSize);
        std::vector<double> value((std::size_t)pointUV->desc().tupleSize);
        d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
            geoReadNumeric(*pointUV, (std::size_t)d.vertexPoint(vertex), std::span<double>(value));
            std::array<double, 3> uvw = { 0., 0., 0. };
            for (int k = 0; k < components; ++k) {
                uvw[(std::size_t)k] = value[(std::size_t)k];
            }
            seeded.emplace_back((std::size_t)vertex, uvw);
        });
    }
    if (pointUV) {
        detail.removeAttribute(eGeoAttribClassPoint, name);
    }
    GeoAttribute& uv = detail.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { name, eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    for (const std::pair<std::size_t, std::array<double, 3>>& s : seeded) {
        geoWriteNumeric(uv, s.first, std::span<const double>(s.second));
    }

    return &uv;
}

// The point attribute to write uv into, the point counterpart of uvAttributeForWriting. A vertex
// attribute of that name would shadow it, so it is removed, a point attribute made here taking
// from it the value of each point's lowest-numbered vertex.
GeoAttribute*
pointUVAttributeForWriting(GeoDetailBuilder& detail,
                           const std::string& name)
{
    const GeoAttribute* existing = detail.detail().findAttribute(eGeoAttribClassPoint, name);
    const bool keep = existing && geoIsNumeric(*existing) && !existing->desc().isArray && (existing->desc().tupleSize >= 2);
    const GeoAttribute* vertexUV = detail.detail().findAttribute(eGeoAttribClassVertex, name);
    std::vector<std::pair<std::size_t, std::array<double, 3>>> seeded;
    if (!keep && vertexUV && geoIsNumeric(*vertexUV) && !vertexUV->desc().isArray) {
        const GeoDetail& d = detail.detail();
        const int components = std::min(3, (int)vertexUV->desc().tupleSize);
        std::vector<double> value((std::size_t)vertexUV->desc().tupleSize);
        GeoElementMask seen((std::size_t)d.pointMap().offsetSize(), 0);
        d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
            const GeoOffset point = d.vertexPoint(vertex);
            if (seen[(std::size_t)point]) {
                return;
            }
            seen[(std::size_t)point] = 1;
            geoReadNumeric(*vertexUV, (std::size_t)vertex, std::span<double>(value));
            std::array<double, 3> uvw = { 0., 0., 0. };
            for (int k = 0; k < components; ++k) {
                uvw[(std::size_t)k] = value[(std::size_t)k];
            }
            seeded.emplace_back((std::size_t)point, uvw);
        });
    }
    if (vertexUV) {
        detail.removeAttribute(eGeoAttribClassVertex, name);
    }
    if (keep) {
        return detail.attributeForWriting(eGeoAttribClassPoint, name);
    }
    if (existing) {
        detail.removeAttribute(eGeoAttribClassPoint, name);
    }
    GeoAttribute& uv = detail.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { name, eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    for (const std::pair<std::size_t, std::array<double, 3>>& s : seeded) {
        geoWriteNumeric(uv, s.first, std::span<const double>(s.second));
    }

    return &uv;
}
} // anonymous namespace

GeoMatrix4
geoUVProjectMatrix(const GeoUVProjectTransform& transform)
{
    GeoMatrix4 scaling = geoMatrixIdentity();
    scaling[0] = transform.scale[0];
    scaling[5] = transform.scale[1];
    scaling[10] = transform.scale[2];
    const GeoMatrix4 rotation = geoComposeTransform({ 0., 0., 0. }, transform.rotate, { 1., 1., 1. }, { 0., 0., 0. }, transform.rotateOrder);
    GeoMatrix4 translation = geoMatrixIdentity();
    translation[3] = transform.translate[0];
    translation[7] = transform.translate[1];
    translation[11] = transform.translate[2];

    // Each order's steps, first applied first.
    static const char kOrders[6][3] = {
        { 's', 'r', 't' },
        { 's', 't', 'r' },
        { 'r', 's', 't' },
        { 'r', 't', 's' },
        { 't', 's', 'r' },
        { 't', 'r', 's' },
    };
    const int order = ((transform.transformOrder >= eGeoTransformOrderSRT) && (transform.transformOrder <= eGeoTransformOrderTRS)) ? (int)transform.transformOrder : 0;
    GeoMatrix4 m = geoMatrixIdentity();
    for (char step : kOrders[order]) {
        const GeoMatrix4& next = (step == 's') ? scaling : ((step == 'r') ? rotation : translation);
        m = geoMatrixMultiply(next, m);
    }
    GeoMatrix4 toPivot = geoMatrixIdentity();
    GeoMatrix4 fromPivot = geoMatrixIdentity();
    for (int k = 0; k < 3; ++k) {
        toPivot[(std::size_t)(4 * k + 3)] = transform.pivot[(std::size_t)k];
        fromPivot[(std::size_t)(4 * k + 3)] = -transform.pivot[(std::size_t)k];
    }

    return geoMatrixMultiply(toPivot, geoMatrixMultiply(m, fromPivot));
} // geoUVProjectMatrix

bool
projectGeoUVs(GeoDetailBuilder& detail,
              const GeoUVProjectParams& params,
              std::string* errorMessage)
{
    if (params.uvAttribute.empty()) {
        *errorMessage = "The UV Attribute name is empty.";

        return false;
    }
    if ((std::abs(params.uRange[1] - params.uRange[0]) < 1e-12) || (std::abs(params.vRange[1] - params.vRange[0]) < 1e-12)) {
        *errorMessage = "The U Range and V Range must not start and end at the same place.";

        return false;
    }
    GeoMatrix4 toLocal = geoMatrixIdentity();
    if (params.projection != eGeoUVProjectionCamera) {
        const std::optional<GeoMatrix4> inverse = geoMatrixInverse(geoUVProjectMatrix(params.transform));
        if (!inverse) {
            *errorMessage = "The projection transform is singular: no Scale may be 0.";

            return false;
        }
        toLocal = *inverse;
    }
    GeoElementMask selected;
    if (!selectForProjection(detail.detail(), params, &selected, errorMessage)) {
        return false;
    }
    const Projector projector(params, toLocal);
    const bool onPoints = (params.groupAttribType == eGeoUVProjectGroupAttribTypePoints);

    // Element offsets and their uv; every element is computed on its own, so the work splits
    // across threads without locks and only the attribute writes stay serial.
    std::vector<GeoOffset> elements;
    std::vector<Vec2> uvs;
    {
        const GeoDetail& d = detail.detail();
        if (onPoints) {
            d.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
                if (selected[(std::size_t)point]) {
                    elements.push_back(point);
                }
            });
            uvs.resize(elements.size());
            std::shared_ptr<const GeoReverseTopology> reverse;
            if (params.fixPoles && projector.hasPoles()) {
                reverse = d.reverseTopology();
            }
            geoParallelFor(elements.size(), [&](std::size_t i) {
                const GeoOffset point = elements[i];
                Place place = projector(d.pointPosition(point));
                if (reverse && (place.undefined[0] || place.undefined[1])) {
                    std::vector<GeoOffset> neighbours;
                    for (GeoOffset vertex : reverse->pointVertices(point)) {
                        const GeoOffset primitive = d.vertexPrimitive(vertex);
                        if (!primitiveTakesUVs(d, primitive)) {
                            continue;
                        }
                        const GeoVertexRange range = d.primitiveVertices(primitive);
                        const GeoOffset k = vertex - range.start;
                        const bool closed = (d.primitiveType(primitive) == eGeoPrimitivePolygon);
                        if (closed || (k > 0)) {
                            neighbours.push_back(d.vertexPoint(range.start + (k + range.count - 1) % range.count));
                        }
                        if (closed || (k + 1 < range.count)) {
                            neighbours.push_back(d.vertexPoint(range.start + (k + 1) % range.count));
                        }
                    }
                    std::sort(neighbours.begin(), neighbours.end());
                    neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
                    std::array<std::vector<double>, 2> values;
                    for (GeoOffset other : neighbours) {
                        if (other == point) {
                            continue;
                        }
                        const Place near = projector(d.pointPosition(other));
                        for (int c = 0; c < 2; ++c) {
                            if (place.undefined[(std::size_t)c] && !near.undefined[(std::size_t)c]) {
                                values[(std::size_t)c].push_back(near.s[(std::size_t)c]);
                            }
                        }
                    }
                    for (int c = 0; c < 2; ++c) {
                        if (!values[(std::size_t)c].empty()) {
                            const double mean = meanOf(values[(std::size_t)c], projector.periodic(c));
                            place.s[(std::size_t)c] = projector.periodic(c) ? wrapUnit(mean) : mean;
                        }
                    }
                }
                uvs[i] = projector.toUV(place.s);
            });
        } else {
            std::vector<GeoOffset> primitives;
            std::vector<std::size_t> firstElement;
            d.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
                if (!selected[(std::size_t)primitive] || !primitiveTakesUVs(d, primitive)) {
                    return;
                }
                const GeoVertexRange range = d.primitiveVertices(primitive);
                primitives.push_back(primitive);
                firstElement.push_back(elements.size());
                for (GeoOffset i = 0; i < range.count; ++i) {
                    elements.push_back(range.start + i);
                }
            });
            uvs.resize(elements.size());
            geoParallelFor(primitives.size(), [&](std::size_t i) {
                const GeoOffset primitive = primitives[i];
                const GeoVertexRange range = d.primitiveVertices(primitive);
                std::vector<Place> places((std::size_t)range.count);
                for (GeoOffset k = 0; k < range.count; ++k) {
                    places[(std::size_t)k] = projector(d.pointPosition(d.vertexPoint(range.start + k)));
                }
                fixPrimitivePlaces(projector, params, d.primitiveType(primitive) == eGeoPrimitivePolygon, places);
                for (GeoOffset k = 0; k < range.count; ++k) {
                    uvs[firstElement[i] + (std::size_t)k] = projector.toUV(places[(std::size_t)k].s);
                }
            });
        }
    }
    if (elements.empty()) {
        return true;
    }

    GeoAttribute* attribute = onPoints ? pointUVAttributeForWriting(detail, params.uvAttribute) : uvAttributeForWriting(detail, params.uvAttribute);
    std::vector<double> value(attribute->desc().tupleSize);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const std::size_t offset = (std::size_t)elements[i];
        geoReadNumeric(*attribute, offset, std::span<double>(value));
        value[0] = uvs[i][0];
        value[1] = uvs[i][1];
        geoWriteNumeric(*attribute, offset, std::span<const double>(value));
    }

    return true;
} // projectGeoUVs

bool
geoUVProjectInitialize(const GeoDetail& detail,
                       const GeoUVProjectParams& params,
                       GeoUVProjectInitializeMethodEnum method,
                       GeoUVProjectTransform* transform,
                       std::string* errorMessage)
{
    GeoElementMask selected;
    if (!selectForProjection(detail, params, &selected, errorMessage)) {
        return false;
    }
    Vec3 lo = { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity() };
    Vec3 hi = { -lo[0], -lo[1], -lo[2] };
    bool any = false;
    auto include = [&](GeoOffset point) {
        const Vec3 p = detail.pointPosition(point);
        for (int k = 0; k < 3; ++k) {
            lo[(std::size_t)k] = std::min(lo[(std::size_t)k], p[(std::size_t)k]);
            hi[(std::size_t)k] = std::max(hi[(std::size_t)k], p[(std::size_t)k]);
        }
        any = true;
    };
    if (params.groupAttribType == eGeoUVProjectGroupAttribTypePoints) {
        detail.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
            if (selected[(std::size_t)point]) {
                include(point);
            }
        });
    } else {
        detail.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
            if (selected[(std::size_t)primitive] && primitiveTakesUVs(detail, primitive)) {
                const GeoVertexRange range = detail.primitiveVertices(primitive);
                for (GeoOffset k = 0; k < range.count; ++k) {
                    include(detail.vertexPoint(range.start + k));
                }
            }
        });
    }
    if (!any) {
        *errorMessage = "The Group selects no point to fit the projection to.";

        return false;
    }
    const double largest = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
    GeoUVProjectTransform out;
    out.transformOrder = params.transform.transformOrder;
    out.rotateOrder = params.transform.rotateOrder;
    // Translating before scaling scales the translate too.
    const GeoTransformOrderEnum order = out.transformOrder;
    const bool translateBeforeScale = (order == eGeoTransformOrderRTS) || (order == eGeoTransformOrderTSR) || (order == eGeoTransformOrderTRS);
    for (int k = 0; k < 3; ++k) {
        const double extent = hi[(std::size_t)k] - lo[(std::size_t)k];
        double scale = (method == eGeoUVProjectInitializeUniformBoundingBox) ? largest : extent;
        if (!(scale > 0.)) {
            scale = 1.;
        }
        const double centre = 0.5 * (lo[(std::size_t)k] + hi[(std::size_t)k]);
        out.scale[(std::size_t)k] = scale;
        out.translate[(std::size_t)k] = translateBeforeScale ? centre / scale : centre;
    }
    *transform = out;

    return true;
} // geoUVProjectInitialize

GeoUVProject::GeoUVProject(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoUVProject::~GeoUVProject()
{
}

const NodeSpec*
GeoUVProject::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoUVProject::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    auto createString = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      const std::string& defaultValue) {
        KnobStringPtr knob = createKnob<KnobString>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createToggle = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      bool defaultValue) {
        KnobBoolPtr knob = createKnob<KnobBool>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createDouble = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      std::initializer_list<double> defaults) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label.toStdString(), (int)defaults.size());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        int i = 0;
        for (double value : defaults) {
            knob->setDefaultValue(value, i++);
        }
        page->addKnob(knob);

        return knob;
    };
    auto createChoice = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      const std::vector<ChoiceOption>& options,
                                      int defaultValue) {
        KnobChoicePtr knob = createKnob<KnobChoice>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->populateChoices(options);
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };

    _uvAttribute = createString(tr("UV Attribute"), kGeoUVProjectParamUVAttribute,
                                tr("The attribute the texture coordinates are written to, of the class Group/Attrib "
                                   "Type sets. An existing one keeps its other components; one of that name of the "
                                   "other class is replaced by it."),
                                "uv");
    _group = createString(tr("Group"), kGeoUVProjectParamGroup,
                          tr("The elements that get texture coordinates, as a group pattern: group names, element "
                             "numbers, @attribute comparisons, ! and ^. Empty selects every element. Anything outside "
                             "the selection keeps its uv."),
                          std::string());
    _groupAttribType = createChoice(tr("Group/Attrib Type"), kGeoUVProjectParamGroupAttribType,
                                    tr("The class the Group selects, and the class the uv is written on."),
                                    { ChoiceOption("points", tr("Points").toStdString(),
                                                   tr("The Group selects points, and uv is written on them, including "
                                                      "points no primitive uses. A point shared by faces on either side of "
                                                      "a seam gets one uv, so the round projections smear across it.")
                                                       .toStdString()),
                                      ChoiceOption("vertices", tr("Vertices").toStdString(),
                                                   tr("The Group selects primitives, and uv is written on the vertices of "
                                                      "the selected polygons and polylines, so Fix Boundary Seams can give "
                                                      "each face its own side of the seam.")
                                                       .toStdString()) },
                                    (int)eGeoUVProjectGroupAttribTypeVertices);
    _projection = createChoice(tr("Projection"), kGeoUVProjectParamProjection,
                               tr("The projection geometry the points are mapped onto, placed by the transform. "
                                  "Connecting the Camera input switches it to Camera."),
                               { ChoiceOption("orthographic", tr("Orthographic").toStdString(),
                                              tr("Straight down Z onto the unit square centred on the origin.").toStdString()),
                                 ChoiceOption("polar", tr("Polar").toStdString(),
                                              tr("Out from the origin onto a sphere with its poles on Y: u is the longitude, "
                                                 "with the seam at -Z, and v the latitude.")
                                                  .toStdString()),
                                 ChoiceOption("cylindrical", tr("Cylindrical").toStdString(),
                                              tr("Out from Y onto the cylinder of radius 0.5 and height 1 about it: u is the "
                                                 "longitude, with the seam at -Z, and v the height.")
                                                  .toStdString()),
                                 ChoiceOption("toroidal", tr("Toroidal").toStdString(),
                                              tr("Onto a torus about Y of outer radius 0.5 and Inner Radius: u is the "
                                                 "longitude, v the angle round the tube, with its seam on the inside.")
                                                  .toStdString()),
                                 ChoiceOption("plasticWrap", tr("Plastic Wrap").toStdString(),
                                              tr("The direction from the origin: +Z lands in the centre and the texture "
                                                 "wraps back to meet itself at -Z.")
                                                  .toStdString()),
                                 ChoiceOption("camera", tr("Camera").toStdString(),
                                              tr("Through the camera at Camera Path on the Camera input: its frame spans 0 "
                                                 "to 1. The transform is not used.")
                                                  .toStdString()) },
                               (int)eGeoUVProjectionOrthographic);
    KnobDoublePtr innerRadius = createDouble(tr("Inner Radius"), kGeoUVProjectParamInnerRadius,
                                             tr("For Toroidal: the torus's inner radius. Its outer radius is always 0.5."),
                                             { 0.2 });
    innerRadius->setMinimum(0.);
    innerRadius->setMaximum(0.5);
    innerRadius->setDisplayMinimum(0.);
    innerRadius->setDisplayMaximum(0.5);
    _innerRadius = innerRadius;

    std::vector<ChoiceOption> transformOrders;
    {
        const char* const ids[] = { "srt", "str", "rst", "rts", "tsr", "trs" };
        const QString labels[] = { tr("Scale Rot Trans"), tr("Scale Trans Rot"), tr("Rot Scale Trans"),
                                   tr("Rot Trans Scale"), tr("Trans Scale Rot"), tr("Trans Rot Scale") };
        for (int i = 0; i < 6; ++i) {
            transformOrders.push_back(ChoiceOption(ids[i], labels[i].toStdString(), ""));
        }
    }
    _transformOrder = createChoice(tr("Transform Order"), kGeoUVProjectParamTransformOrder,
                                   tr("The order scale, rotate and translate place the projection geometry in, the "
                                      "first applied first."),
                                   transformOrders, (int)eGeoTransformOrderSRT);
    std::vector<ChoiceOption> rotateOrders;
    for (const char* order : { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" }) {
        rotateOrders.push_back(ChoiceOption(order, order, ""));
    }
    _rotateOrder = createChoice(tr("Rotate Order"), kGeoUVProjectParamRotateOrder,
                                tr("The order the three rotations are applied in: XYZ turns about X first, then Y, then Z."),
                                rotateOrders, (int)eGeoRotateOrderXYZ);
    _translate = createDouble(tr("Translate"), kGeoUVProjectParamTranslate,
                              tr("Moves the projection geometry along X, Y and Z."), { 0., 0., 0. });
    _rotate = createDouble(tr("Rotate"), kGeoUVProjectParamRotate,
                           tr("Turns the projection geometry about X, Y and Z, in degrees."), { 0., 0., 0. });
    _scale = createDouble(tr("Scale"), kGeoUVProjectParamScale,
                          tr("Stretches the projection geometry along X, Y and Z. None may be 0."), { 1., 1., 1. });
    _pivot = createDouble(tr("Pivot"), kGeoUVProjectParamPivot,
                          tr("The point the rotation and scale are about."), { 0., 0., 0. });

    _initializeMethod = createChoice(tr("Initialize Method"), kGeoUVProjectParamInitializeMethod,
                                     tr("How Initialize fits the projection geometry to the Group's bounds."),
                                     { ChoiceOption("boundingBox", tr("Bounding Box").toStdString(),
                                                    tr("Scales each axis to the bounds' size along it, so the texture "
                                                       "stretches to fill them.")
                                                        .toStdString()),
                                       ChoiceOption("uniformBoundingBox", tr("Uniform Bounding Box").toStdString(),
                                                    tr("Scales every axis to the bounds' largest side, so the texture keeps "
                                                       "its proportions.")
                                                        .toStdString()) },
                                     (int)eGeoUVProjectInitializeBoundingBox);
    KnobButtonPtr initialize = createKnob<KnobButton>(tr("Initialize").toStdString());
    initialize->setName(kGeoUVProjectParamInitialize);
    initialize->setHintToolTip(tr("Fits the projection geometry to the bounds of what the Group selects at the current "
                                  "frame: Translate to their centre, Scale by Initialize Method, no Rotate and the Pivot "
                                  "at the origin. Undoable.")
                                   .toStdString());
    page->addKnob(initialize);
    _initialize = initialize;

    _uRange = createDouble(tr("U Range"), kGeoUVProjectParamURange,
                           tr("Where the texture's left and right edges sit across the projection geometry, 0 to 1. "
                              "Swapping them mirrors the texture."),
                           { 0., 1. });
    _vRange = createDouble(tr("V Range"), kGeoUVProjectParamVRange,
                           tr("Where the texture's bottom and top edges sit across the projection geometry, 0 to 1. "
                              "Swapping them mirrors the texture."),
                           { 0., 1. });
    _angle = createDouble(tr("Angle"), kGeoUVProjectParamAngle,
                          tr("Turns the texture coordinates about (0.5, 0.5), in degrees."), { 0. });
    _fixBoundarySeams = createToggle(tr("Fix Boundary Seams"), kGeoUVProjectParamFixBoundarySeams,
                                     tr("Keeps a face that crosses a round projection's seam from stretching back across "
                                        "the whole texture, by letting its uv run past 1. Only vertex uv can do this."),
                                     true);
    _fixPoles = createToggle(tr("Fix Poles"), kGeoUVProjectParamFixPoles,
                             tr("Gives a vertex at a pole of the projection geometry, where the projection is undefined, "
                                "the uv of its neighbours instead."),
                             true);
    KnobDoublePtr poleRadius = createDouble(tr("Pole Radius"), kGeoUVProjectParamPoleRadius,
                                            tr("How close to a pole, in the projection geometry's own space, a vertex is "
                                               "treated as at it."),
                                            { 0.01 });
    poleRadius->setMinimum(0.);
    poleRadius->setDisplayMinimum(0.);
    poleRadius->setDisplayMaximum(0.1);
    _poleRadius = poleRadius;

    _cameraPath = createString(tr("Camera Path"), kGeoUVProjectParamCameraPath,
                               tr("For Camera: the path of the camera prim in the scene on the Camera input. Empty uses "
                                  "the scene's first camera."),
                               std::string());

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    formatKnobs.choice->setHintToolTip(tr("For Camera: the format of the image the texture coordinates will map, which the "
                                          "camera's aperture is conformed to by Fit Mode.")
                                           .toStdString());
    _format = formatKnobs.choice;
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;

    _fitMode = createChoice(tr("Fit Mode"), kGeoUVProjectParamFitMode,
                            tr("For Camera: how the camera's aperture is conformed to the Format when their shapes differ."),
                            { ChoiceOption("fill", tr("Fill").toStdString(),
                                           tr("The aperture fills the format: the side that does not fit is cropped.").toStdString()),
                              ChoiceOption("horizontal", tr("Horizontal").toStdString(),
                                           tr("The aperture's width fills the format's; its height is cropped or extended.").toStdString()),
                              ChoiceOption("vertical", tr("Vertical").toStdString(),
                                           tr("The aperture's height fills the format's; its width is cropped or extended.").toStdString()) },
                            0);
    _useFrameHold = createToggle(tr("Use Frame Hold"), kGeoUVProjectParamUseFrameHold,
                                 tr("For Camera: projects through the camera as it is at Frame Hold on every frame, so a "
                                    "moving camera's texture stays stuck to the geometry."),
                                 false);
    KnobDoublePtr frameHold = createKnob<KnobDouble>(tr("Frame Hold").toStdString());
    frameHold->setName(kGeoUVProjectParamFrameHold);
    frameHold->setHintToolTip(tr("The frame the camera is held at when Use Frame Hold is on.").toStdString());
    frameHold->setDefaultValue(1.);
    frameHold->setAnimationEnabled(false);
    page->addKnob(frameHold);
    _frameHold = frameHold;

    updateEnabled();
} // GeoUVProject::initializeKnobs

void
GeoUVProject::updateEnabled()
{
    const KnobChoicePtr projection = _projection.lock();
    const int value = projection ? projection->getValue() : (int)eGeoUVProjectionOrthographic;
    const bool camera = (value == (int)eGeoUVProjectionCamera);
    const bool round = (value == (int)eGeoUVProjectionPolar) || (value == (int)eGeoUVProjectionCylindrical)
        || (value == (int)eGeoUVProjectionToroidal);
    const bool poles = round || (value == (int)eGeoUVProjectionPlasticWrap);
    const KnobChoicePtr type = _groupAttribType.lock();
    const bool onVertices = !type || (type->getValue() == (int)eGeoUVProjectGroupAttribTypeVertices);
    const KnobBoolPtr fixPoles = _fixPoles.lock();
    const KnobBoolPtr useFrameHold = _useFrameHold.lock();
    auto enable = [](const KnobIPtr& knob,
                     bool enabled) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    enable(_innerRadius.lock(), value == (int)eGeoUVProjectionToroidal);
    for (const KnobIPtr& knob : { KnobIPtr(_transformOrder.lock()), KnobIPtr(_rotateOrder.lock()), KnobIPtr(_translate.lock()),
                                  KnobIPtr(_rotate.lock()), KnobIPtr(_scale.lock()), KnobIPtr(_pivot.lock()),
                                  KnobIPtr(_initializeMethod.lock()), KnobIPtr(_initialize.lock()) }) {
        enable(knob, !camera);
    }
    enable(_fixBoundarySeams.lock(), round && onVertices);
    enable(_fixPoles.lock(), poles);
    enable(_poleRadius.lock(), poles && fixPoles && fixPoles->getValue());
    enable(_cameraPath.lock(), camera);
    enable(_format.lock(), camera);
    enable(_formatSize.lock(), camera);
    enable(_formatPar.lock(), camera);
    enable(_fitMode.lock(), camera);
    enable(_useFrameHold.lock(), camera);
    enable(_frameHold.lock(), camera && useFrameHold && useFrameHold->getValue());
} // GeoUVProject::updateEnabled

void
GeoUVProject::onKnobsLoaded()
{
    updateEnabled();
}

void
GeoUVProject::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    switchChoiceOnInputConnected(*this, inputNo, 1, _projection.lock(), (int)eGeoUVProjectionCamera);
}

bool
GeoUVProject::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    const KnobButtonPtr initialize = _initialize.lock();
    if (initialize && (k == initialize.get())) {
        std::string error;
        if (!initializeTransform(&error)) {
            message(eMessageTypeWarning, error);
        }

        return true;
    }
    for (const KnobIPtr& knob : { KnobIPtr(_projection.lock()), KnobIPtr(_groupAttribType.lock()), KnobIPtr(_fixPoles.lock()),
                                  KnobIPtr(_useFrameHold.lock()) }) {
        if (knob && (k == knob.get())) {
            updateEnabled();

            return true;
        }
    }

    return false;
}

bool
GeoUVProject::initializeTransform(std::string* errorMessage)
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
    if (!input) {
        *errorMessage = tr("There is no geometry to fit the projection to.").toStdString();

        return false;
    }
    const GeoUVProjectParams params = readParams(time, view);
    const KnobChoicePtr method = _initializeMethod.lock();
    const GeoUVProjectInitializeMethodEnum fit = (method && (method->getValue() == (int)eGeoUVProjectInitializeUniformBoundingBox))
        ? eGeoUVProjectInitializeUniformBoundingBox
        : eGeoUVProjectInitializeBoundingBox;
    GeoUVProjectTransform transform;
    if (!geoUVProjectInitialize(*input, params, fit, &transform, errorMessage)) {
        return false;
    }
    const KnobDoublePtr translate = _translate.lock();
    const KnobDoublePtr rotate = _rotate.lock();
    const KnobDoublePtr scale = _scale.lock();
    const KnobDoublePtr pivot = _pivot.lock();
    if (!translate || !rotate || !scale || !pivot) {
        return true;
    }
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    for (int d = 0; d < 3; ++d) {
        translate->setValueFromPlugin(transform.translate[(std::size_t)d], ViewSpec::all(), d);
        rotate->setValueFromPlugin(transform.rotate[(std::size_t)d], ViewSpec::all(), d);
        scale->setValueFromPlugin(transform.scale[(std::size_t)d], ViewSpec::all(), d);
        pivot->setValueFromPlugin(transform.pivot[(std::size_t)d], ViewSpec::all(), d);
    }
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);

    return true;
} // GeoUVProject::initializeTransform

StatusEnum
GeoUVProject::getPreferredMetadataAt(double /*time*/,
                                     ViewIdx /*view*/,
                                     NodeMetadata& metadata)
{
    // The scene payload is one per layer stack, not per frame, so nothing upstream tells the
    // cache that an animated camera changes the result from frame to frame.
    const KnobChoicePtr projection = _projection.lock();
    const KnobBoolPtr useFrameHold = _useFrameHold.lock();
    if (projection && (projection->getValue() == (int)eGeoUVProjectionCamera) && getInput(1)
        && !(useFrameHold && useFrameHold->getValue())) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

double
GeoUVProject::getFormatAspect() const
{
    const KnobIntPtr size = _formatSize.lock();
    const KnobDoublePtr par = _formatPar.lock();

    if (size && par) {
        const int width = size->getValue(0);
        const int height = size->getValue(1);
        const double pixelAspect = par->getValue();
        if ((width > 0) && (height > 0) && (pixelAspect > 0.)) {
            return width * pixelAspect / height;
        }
    }
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);

    return (format.height() > 0) ? format.width() * format.getPixelAspectRatio() / format.height() : 1.;
}

StatusEnum
GeoUVProject::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    GeoUVProjectParams params = readParams(args.time, args.view);

    if (params.projection == eGeoUVProjectionCamera) {
        const EffectInstancePtr cameraInput = getInput(1);
        if (!cameraInput) {
            if (errorMessage) {
                *errorMessage = tr("The Camera projection needs a scene with a camera on the Camera input.").toStdString();
            }

            return eStatusFailed;
        }
        std::string error;
        ScenePayload scene;
        if (cameraInput->evaluateScene(EvaluateSceneArgs(args.time, args.view), &scene, &error) != eRenderRoIRetCodeOk) {
            if (errorMessage) {
                *errorMessage = error;
            }

            return eStatusFailed;
        }
        const KnobBoolPtr useFrameHold = _useFrameHold.lock();
        const KnobDoublePtr frameHold = _frameHold.lock();
        const double frame = (useFrameHold && frameHold && useFrameHold->getValue()) ? frameHold->getValue() : args.time;
        const KnobStringPtr cameraPath = _cameraPath.lock();
        const KnobChoicePtr fitMode = _fitMode.lock();
        std::string usedPath;
        if (!GeoCameraProjection::worldToFrame(scene, cameraPath ? cameraPath->getValue() : std::string(), frame,
                                               getApp()->getProjectFrameRate(), getFormatAspect(),
                                               fitMode ? fitMode->getValue() : 0, &params.cameraWorldToFrame, &usedPath, &error)) {
            if (errorMessage) {
                *errorMessage = error;
            }

            return eStatusFailed;
        }
    }

    return renderGeometryFromInput(args, 0, [&params](GeoDetailBuilder& detail, std::string* error) {
        std::string message;
        if (!projectGeoUVs(detail, params, &message)) {
            if (error) {
                *error = message;
            }

            return eStatusFailed;
        }

        return eStatusOK; }, outputDetail, errorMessage);
} // GeoUVProject::renderGeometry

GeoUVProjectParams
GeoUVProject::readParams(double time,
                         ViewSpec view) const
{
    GeoUVProjectParams p;
    auto text = [time, view](const KnobStringWPtr& weak,
                             const std::string& fallback) {
        const KnobStringPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto toggle = [time, view](const KnobBoolWPtr& weak,
                               bool fallback) {
        const KnobBoolPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto choice = [time, view](const KnobChoiceWPtr& weak,
                               int fallback,
                               int count) {
        const KnobChoicePtr knob = weak.lock();
        const int value = knob ? knob->getValueAtTime(time, 0, view) : fallback;

        return ((value >= 0) && (value < count)) ? value : fallback;
    };
    auto number = [time, view](const KnobDoubleWPtr& weak,
                               double* out,
                               int dimensions) {
        if (const KnobDoublePtr knob = weak.lock()) {
            for (int i = 0; i < dimensions; ++i) {
                out[i] = knob->getValueAtTime(time, i, view);
            }
        }
    };
    p.uvAttribute = text(_uvAttribute, p.uvAttribute);
    p.group = text(_group, p.group);
    p.groupAttribType = (GeoUVProjectGroupAttribTypeEnum)choice(_groupAttribType, (int)p.groupAttribType, 2);
    p.projection = (GeoUVProjectionEnum)choice(_projection, (int)p.projection, (int)eGeoUVProjectionCamera + 1);
    number(_innerRadius, &p.innerRadius, 1);
    p.transform.transformOrder = (GeoTransformOrderEnum)choice(_transformOrder, (int)p.transform.transformOrder, 6);
    p.transform.rotateOrder = (GeoRotateOrderEnum)choice(_rotateOrder, (int)p.transform.rotateOrder, 6);
    number(_translate, p.transform.translate.data(), 3);
    number(_rotate, p.transform.rotate.data(), 3);
    number(_scale, p.transform.scale.data(), 3);
    number(_pivot, p.transform.pivot.data(), 3);
    number(_uRange, p.uRange.data(), 2);
    number(_vRange, p.vRange.data(), 2);
    number(_angle, &p.angle, 1);
    p.fixBoundarySeams = toggle(_fixBoundarySeams, p.fixBoundarySeams);
    p.fixPoles = toggle(_fixPoles, p.fixPoles);
    number(_poleRadius, &p.poleRadius, 1);

    return p;
} // GeoUVProject::readParams

NATRON_NAMESPACE_EXIT
