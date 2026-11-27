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

#include "GeoUVLayout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoUVFlattenSolver.h"
#include "Engine/Geo/GeoUVIslands.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Geometry/GeoUVFlatten.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_ENTER

namespace {
typedef std::array<double, 3> Vec3;

std::string
trimmed(const std::string& s)
{
    std::size_t begin = 0, end = s.size();
    while ((begin < end) && std::isspace((unsigned char)s[begin])) {
        ++begin;
    }
    while ((end > begin) && std::isspace((unsigned char)s[end - 1])) {
        --end;
    }

    return s.substr(begin, end - begin);
}

/** @brief The uv attribute of a geometry, read by vertex: u, v and the third component. */
struct UVField {
    const GeoAttribute* attribute = nullptr;
    bool onVertices = true;
    std::vector<Vec3> uvw;
};

bool
readUVField(const GeoDetail& d,
            const std::string& name,
            UVField* field)
{
    const GeoAttribute* attribute = d.findAttribute(eGeoAttribClassVertex, name);
    bool onVertices = true;
    if (!attribute || !geoIsNumeric(*attribute) || (attribute->desc().tupleSize < 2)) {
        attribute = d.findAttribute(eGeoAttribClassPoint, name);
        onVertices = false;
    }
    if (!attribute || !geoIsNumeric(*attribute) || (attribute->desc().tupleSize < 2)) {
        return false;
    }
    field->attribute = attribute;
    field->onVertices = onVertices;
    field->uvw.assign((std::size_t)d.vertexMap().offsetSize(), Vec3 { 0., 0., 0. });
    std::vector<double> value((std::size_t)attribute->desc().tupleSize);
    d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        geoReadNumeric(*attribute, (std::size_t)(onVertices ? vertex : d.vertexPoint(vertex)), std::span<double>(value));
        field->uvw[(std::size_t)vertex] = { value[0], value[1], (value.size() > 2) ? value[2] : 0. };
    });

    return true;
}

/** @brief The packing units of a geometry: islands merged by the island attribute. */
struct Units {
    GeoUVIslands islands;
    // By unit: its polygons in ascending offset.
    std::vector<std::vector<GeoOffset>> polygons;
    // By unit: the number of islands merged into it.
    std::vector<int> islandCount;
};

bool
findUnits(const GeoDetail& d,
          const GeoUVLayoutIslandSource& source,
          double tolerance,
          const GeoElementMask& primitives,
          Units* units,
          std::string* errorMessage)
{
    GeoUVIslandOptions options;
    options.uvAttribute = source.uvAttribute;
    options.tolerance = tolerance;
    options.islandAttribute = trimmed(source.islandAttribute);
    options.primitives = primitives;
    if (!trimmed(source.separatorEdges).empty()) {
        options.connectivity = eGeoUVIslandConnectivityTopology;
        if (!geoSelectEdges(d, source.separatorEdges, &options.seams, errorMessage)) {
            return false;
        }
    }
    if (!geoComputeUVIslands(d, options, &units->islands, errorMessage)) {
        return false;
    }
    for (const std::vector<int>& group : units->islands.groups) {
        std::vector<GeoOffset> polygons;
        for (int island : group) {
            const std::vector<GeoOffset>& own = units->islands.islands[(std::size_t)island];
            polygons.insert(polygons.end(), own.begin(), own.end());
        }
        std::sort(polygons.begin(), polygons.end());
        units->polygons.push_back(std::move(polygons));
        units->islandCount.push_back((int)group.size());
    }

    return true;
}

int
unitOfPrimitive(const Units& units,
                GeoOffset primitive)
{
    if ((primitive < 0) || ((std::size_t)primitive >= units.islands.islandOfPrimitive.size())) {
        return -1;
    }
    const int island = units.islands.islandOfPrimitive[(std::size_t)primitive];

    return (island < 0) ? -1 : units.islands.groupOfIsland[(std::size_t)island];
}

std::vector<std::vector<GeoUV>>
ringsOf(const GeoDetail& d,
        const std::vector<GeoOffset>& polygons,
        const std::vector<Vec3>& uvw)
{
    std::vector<std::vector<GeoUV>> rings;
    for (GeoOffset primitive : polygons) {
        const GeoVertexRange range = d.primitiveVertices(primitive);
        std::vector<GeoUV> ring;
        for (GeoOffset k = 0; k < range.count; ++k) {
            const Vec3& p = uvw[(std::size_t)(range.start + k)];
            ring.push_back({ p[0], p[1] });
        }
        rings.push_back(std::move(ring));
    }

    return rings;
}

// The value of a primitive attribute for each unit, from its lowest-numbered polygon with one
// at or above minimum; unset where there is none.
bool
unitValues(const GeoDetail& d,
           const Units& units,
           const std::string& name,
           double minimum,
           std::vector<double>* values,
           std::vector<char>* set,
           std::string* errorMessage)
{
    if (trimmed(name).empty()) {
        return true;
    }
    const GeoAttribute* attribute = d.findAttribute(eGeoAttribClassPrimitive, trimmed(name));
    if (!attribute || !geoIsNumeric(*attribute)) {
        if (errorMessage) {
            *errorMessage = "There is no numeric primitive attribute " + trimmed(name) + ".";
        }

        return false;
    }
    std::vector<double> value((std::size_t)attribute->desc().tupleSize);
    for (std::size_t u = 0; u < units.polygons.size(); ++u) {
        for (GeoOffset primitive : units.polygons[u]) {
            geoReadNumeric(*attribute, (std::size_t)primitive, std::span<double>(value));
            if (value[0] >= minimum) {
                (*values)[u] = value[0];
                (*set)[u] = 1;
                break;
            }
        }
    }

    return true;
}

// Applies "polygon:value" tokens in order; returns the number of tokens that name no unit or
// do not parse.
int
applyOverrides(const GeoDetail& d,
               const Units& units,
               const std::string& text,
               std::vector<double>* values,
               std::vector<char>* set)
{
    int ignored = 0;
    std::istringstream in(text);
    std::string token;
    while (in >> token) {
        const std::size_t colon = token.find(':');
        if ((colon == std::string::npos) || (colon == 0) || (colon + 1 >= token.size())) {
            ++ignored;
            continue;
        }
        const std::string polygonText = token.substr(0, colon);
        const std::string valueText = token.substr(colon + 1);
        char* end = nullptr;
        const long polygon = std::strtol(polygonText.c_str(), &end, 10);
        if (!end || (*end != '\0') || (polygon < 0) || ((std::size_t)polygon >= (std::size_t)d.primitiveMap().size())) {
            ++ignored;
            continue;
        }
        end = nullptr;
        const double value = std::strtod(valueText.c_str(), &end);
        if (!end || (*end != '\0')) {
            ++ignored;
            continue;
        }
        const int unit = unitOfPrimitive(units, d.primitiveMap().offsetFromIndex((GeoIndex)polygon));
        if (unit < 0) {
            ++ignored;
            continue;
        }
        (*values)[(std::size_t)unit] = value;
        (*set)[(std::size_t)unit] = 1;
    }

    return ignored;
}

double
surfaceArea(const GeoDetail& d,
            GeoOffset primitive)
{
    const GeoVertexRange range = d.primitiveVertices(primitive);
    Vec3 n = { 0., 0., 0. };
    for (GeoOffset k = 0; k < range.count; ++k) {
        const Vec3 a = d.pointPosition(d.vertexPoint(range.start + k));
        const Vec3 b = d.pointPosition(d.vertexPoint(range.start + (k + 1) % range.count));
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }

    return 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

double
ringArea(const std::vector<GeoUV>& ring)
{
    double area = 0.;
    for (std::size_t k = 0; k < ring.size(); ++k) {
        const GeoUV& a = ring[k];
        const GeoUV& b = ring[(k + 1) % ring.size()];
        area += 0.5 * (a[0] * b[1] - a[1] * b[0]);
    }

    return area;
}

void
rotateRings(std::vector<std::vector<GeoUV>>* rings,
            double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    for (std::vector<GeoUV>& ring : *rings) {
        for (GeoUV& p : ring) {
            p = { c * p[0] - s * p[1], s * p[0] + c * p[1] };
        }
    }
}

double
symmetryAngle(const std::vector<std::vector<GeoUV>>& rings)
{
    std::vector<GeoUV> points;
    for (const std::vector<GeoUV>& ring : rings) {
        points.insert(points.end(), ring.begin(), ring.end());
    }
    if (points.size() < 3) {
        return 0.;
    }
    double angle = geoUVMinimumAreaRotation(points);
    const double c = std::cos(angle), s = std::sin(angle);
    double lo[2] = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
    double hi[2] = { -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max() };
    for (const GeoUV& p : points) {
        const double x = c * p[0] - s * p[1], y = s * p[0] + c * p[1];
        lo[0] = std::min(lo[0], x);
        hi[0] = std::max(hi[0], x);
        lo[1] = std::min(lo[1], y);
        hi[1] = std::max(hi[1], y);
    }
    if ((hi[1] - lo[1]) > (hi[0] - lo[0]) * (1. + 1e-9)) {
        angle += 0.5 * M_PI;
    }

    return angle;
}

// The turn bringing, in each triangle, the uv direction of a coordinate axis lying in its
// plane onto u or v; the four-fold mean, weighted by surface area, settles the disagreements.
double
positionAngle(const GeoDetail& d,
              const std::vector<GeoOffset>& polygons,
              const std::vector<Vec3>& uvw)
{
    double sumC = 0., sumS = 0.;
    for (GeoOffset primitive : polygons) {
        const GeoVertexRange range = d.primitiveVertices(primitive);
        for (GeoOffset k = 1; k + 1 < range.count; ++k) {
            const GeoOffset v[3] = { range.start, range.start + k, range.start + k + 1 };
            Vec3 p[3];
            GeoUV t[3];
            for (int i = 0; i < 3; ++i) {
                p[i] = d.pointPosition(d.vertexPoint(v[i]));
                t[i] = { uvw[(std::size_t)v[i]][0], uvw[(std::size_t)v[i]][1] };
            }
            const Vec3 e1 = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
            const Vec3 e2 = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
            const double d1[2] = { t[1][0] - t[0][0], t[1][1] - t[0][1] };
            const double d2[2] = { t[2][0] - t[0][0], t[2][1] - t[0][1] };
            const double det = d1[0] * d2[1] - d1[1] * d2[0];
            Vec3 n = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const double area2 = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if ((std::fabs(det) < 1e-300) || (area2 < 1e-300)) {
                continue;
            }
            for (double& c : n) {
                c /= area2;
            }
            // J maps uv steps to surface steps: [e1 e2] = J [d1 d2].
            double J[3][2];
            for (int r = 0; r < 3; ++r) {
                J[r][0] = (e1[(std::size_t)r] * d2[1] - e2[(std::size_t)r] * d1[1]) / det;
                J[r][1] = (e2[(std::size_t)r] * d1[0] - e1[(std::size_t)r] * d2[0]) / det;
            }
            int axis = 0;
            for (int a = 1; a < 3; ++a) {
                if (std::fabs(n[(std::size_t)a]) < std::fabs(n[(std::size_t)axis]) - 1e-12) {
                    axis = a;
                }
            }
            Vec3 target = { 0., 0., 0. };
            target[(std::size_t)axis] = 1.;
            for (int r = 0; r < 3; ++r) {
                target[(std::size_t)r] -= n[(std::size_t)axis] * n[(std::size_t)r];
            }
            const double jtj[3] = { J[0][0] * J[0][0] + J[1][0] * J[1][0] + J[2][0] * J[2][0],
                                    J[0][0] * J[0][1] + J[1][0] * J[1][1] + J[2][0] * J[2][1],
                                    J[0][1] * J[0][1] + J[1][1] * J[1][1] + J[2][1] * J[2][1] };
            const double jta[2] = { J[0][0] * target[0] + J[1][0] * target[1] + J[2][0] * target[2],
                                    J[0][1] * target[0] + J[1][1] * target[1] + J[2][1] * target[2] };
            const double m = jtj[0] * jtj[2] - jtj[1] * jtj[1];
            if (std::fabs(m) < 1e-300) {
                continue;
            }
            const double dx = (jtj[2] * jta[0] - jtj[1] * jta[1]) / m;
            const double dy = (jtj[0] * jta[1] - jtj[1] * jta[0]) / m;
            const double psi = std::atan2(dy, dx);
            sumC += 0.5 * area2 * std::cos(4. * psi);
            sumS += 0.5 * area2 * std::sin(4. * psi);
        }
    }
    if ((sumC == 0.) && (sumS == 0.)) {
        return 0.;
    }

    return -0.25 * std::atan2(sumS, sumC);
} // positionAngle
} // anonymous namespace

bool
layoutGeoUVs(GeoDetailBuilder& detail,
             const GeoDetail* targetGeometry,
             const GeoUVLayoutParams& params,
             GeoUVLayoutReport* report,
             std::string* errorMessage)
{
    GeoUVLayoutReport local;
    GeoUVLayoutReport* r = report ? report : &local;
    *r = GeoUVLayoutReport();
    std::string error;
    auto fail = [&](const std::string& message) {
        if (errorMessage) {
            *errorMessage = message;
        }

        return false;
    };
    const std::string uvName = trimmed(params.islands.uvAttribute);
    if (uvName.empty()) {
        return fail("The UV Attribute is empty.");
    }
    const GeoDetail& d = detail.detail();
    UVField uv;
    if (!readUVField(d, uvName, &uv)) {
        return fail("There is no point or vertex attribute " + uvName + " with two or more numeric components.");
    }
    GeoElementMask inGroup;
    if (!geoSelectElements(d, eGeoAttribClassPrimitive, params.islands.group, &inGroup, &error)) {
        return fail(error);
    }
    Units units;
    if (!findUnits(d, params.islands, params.connectivityTolerance, inGroup, &units, &error)) {
        return fail(error);
    }
    const std::size_t count = units.polygons.size();

    std::vector<double> target(count, -1.), scale(count, 1.), offset(count, 0.);
    std::vector<char> hasTarget(count, 0), hasScale(count, 0), hasOffset(count, 0);
    if (!unitValues(d, units, params.targetAttribute, 0., &target, &hasTarget, &error)
        || !unitValues(d, units, params.islandScaleAttribute, 0., &scale, &hasScale, &error)
        || !unitValues(d, units, params.islandOffsetAttribute, -std::numeric_limits<double>::max(), &offset, &hasOffset, &error)) {
        return fail(error);
    }
    r->ignoredOverrides += applyOverrides(d, units, params.targetOverrides, &target, &hasTarget);
    r->ignoredOverrides += applyOverrides(d, units, params.scaleOverrides, &scale, &hasScale);
    r->ignoredOverrides += applyOverrides(d, units, params.offsetOverrides, &offset, &hasOffset);

    GeoUVPackParams pack = params.pack;
    pack.aborted = params.aborted;
    pack.avoidObstacles = params.avoidPackingOverNonGroupIslands;
    pack.stackOnObstacles = params.allowStackingOnNonGroupIslands && pack.stackIdentical;
    std::vector<double> targetLift;
    if (pack.targetType == eGeoUVPackTargetIslands) {
        if (!targetGeometry) {
            return fail("Packing into islands from the second input needs geometry on the Targets input.");
        }
        UVField targetUV;
        if (!readUVField(*targetGeometry, trimmed(params.targets.uvAttribute), &targetUV)) {
            return fail("The Targets input has no point or vertex attribute " + trimmed(params.targets.uvAttribute)
                        + " with two or more numeric components.");
        }
        GeoElementMask targetMask;
        if (!geoSelectElements(*targetGeometry, eGeoAttribClassPrimitive, params.targets.group, &targetMask, &error)) {
            return fail(error);
        }
        Units targets;
        if (!findUnits(*targetGeometry, params.targets, params.connectivityTolerance, targetMask, &targets, &error)) {
            return fail(error);
        }
        pack.targetIslands.clear();
        for (const std::vector<GeoOffset>& polygons : targets.polygons) {
            GeoUVPackIsland island;
            island.polygons = ringsOf(*targetGeometry, polygons, targetUV.uvw);
            pack.targetIslands.push_back(std::move(island));
            double lowest = std::numeric_limits<double>::max();
            for (GeoOffset primitive : polygons) {
                const GeoVertexRange range = targetGeometry->primitiveVertices(primitive);
                for (GeoOffset k = 0; k < range.count; ++k) {
                    lowest = std::min(lowest, targetUV.uvw[(std::size_t)(range.start + k)][2]);
                }
            }
            targetLift.push_back(polygons.empty() ? 0. : lowest);
        }
    }

    std::vector<GeoUVPackIsland> islands;
    std::vector<double> turn(count, 0.);
    for (std::size_t u = 0; u < count; ++u) {
        GeoUVPackIsland island;
        island.polygons = ringsOf(d, units.polygons[u], uv.uvw);
        if (params.scaleIslandsToMatchSurfaceArea) {
            double surface = 0., texture = 0.;
            for (std::size_t p = 0; p < units.polygons[u].size(); ++p) {
                surface += surfaceArea(d, units.polygons[u][p]);
                texture += std::fabs(ringArea(island.polygons[p]));
            }
            if ((surface > 0.) && (texture > 0.)) {
                island.scale = std::sqrt(surface / texture);
            }
        }
        if (hasScale[u]) {
            island.scale *= scale[u];
        }
        if (params.axisAlignment == eGeoUVLayoutAxisAlignmentIslandSymmetry) {
            turn[u] = symmetryAngle(island.polygons);
        } else if (params.axisAlignment == eGeoUVLayoutAxisAlignmentIslandPosition) {
            turn[u] = positionAngle(d, units.polygons[u], uv.uvw);
        }
        rotateRings(&island.polygons, turn[u]);
        island.target = hasTarget[u] ? (int)std::lround(target[u]) : -1;
        island.stackable = (units.islandCount[u] == 1);
        islands.push_back(std::move(island));
    }
    if (params.avoidPackingOverNonGroupIslands || pack.stackOnObstacles) {
        GeoElementMask outside(inGroup.size(), 0);
        for (std::size_t i = 0; i < inGroup.size(); ++i) {
            outside[i] = inGroup[i] ? 0 : 1;
        }
        GeoUVLayoutIslandSource source = params.islands;
        source.islandAttribute.clear();
        Units others;
        if (!findUnits(d, source, params.connectivityTolerance, outside, &others, &error)) {
            return fail(error);
        }
        for (const std::vector<GeoOffset>& polygons : others.polygons) {
            GeoUVPackIsland island;
            island.polygons = ringsOf(d, polygons, uv.uvw);
            island.obstacle = true;
            islands.push_back(std::move(island));
        }
    }
    if (count == 0) {
        return true;
    }
    GeoUVPackResult packed;
    if (!geoPackUVIslands(islands, pack, &packed, &error)) {
        return fail(error);
    }
    if (packed.aborted || (params.aborted && params.aborted())) {
        r->aborted = true;

        return fail(std::string());
    }
    r->nonpacked = packed.nonpacked;
    r->maxUdim = packed.maxUdim;

    std::vector<std::pair<GeoOffset, Vec3>> written;
    for (std::size_t u = 0; u < count; ++u) {
        const GeoUVPackPlacement& placement = packed.placements[u];
        double lift = hasOffset[u] ? offset[u] : 0.;
        if (placement.packed && (placement.target >= 0) && ((std::size_t)placement.target < targetLift.size())) {
            lift += targetLift[(std::size_t)placement.target];
        }
        const double c = std::cos(turn[u]), s = std::sin(turn[u]);
        for (GeoOffset primitive : units.polygons[u]) {
            const GeoVertexRange range = d.primitiveVertices(primitive);
            for (GeoOffset k = 0; k < range.count; ++k) {
                const GeoOffset vertex = range.start + k;
                const Vec3& p = uv.uvw[(std::size_t)vertex];
                const GeoUV moved = geoUVPackApply(placement, GeoUV { c * p[0] - s * p[1], s * p[0] + c * p[1] });
                written.emplace_back(vertex, Vec3 { moved[0], moved[1], p[2] + lift });
            }
        }
    }

    const GeoAttribClassEnum cls = uv.onVertices ? eGeoAttribClassVertex : eGeoAttribClassPoint;
    GeoAttribute* out = detail.attributeForWriting(cls, uvName);
    std::vector<double> value((std::size_t)out->desc().tupleSize);
    for (const std::pair<GeoOffset, Vec3>& w : written) {
        const std::size_t element = (std::size_t)(uv.onVertices ? w.first : detail.detail().vertexPoint(w.first));
        geoReadNumeric(*out, element, std::span<double>(value));
        for (std::size_t k = 0; k < std::min<std::size_t>(3, value.size()); ++k) {
            value[k] = w.second[k];
        }
        geoWriteNumeric(*out, element, std::span<const double>(value));
    }
    if (params.outputNonpacked && !trimmed(params.nonpackedGroup).empty()) {
        GeoElementGroup& group = detail.addGroup(eGeoAttribClassPrimitive, trimmed(params.nonpackedGroup));
        group.clear();
        for (std::size_t u = 0; u < count; ++u) {
            if (!packed.placements[u].packed) {
                group.add(std::span<const GeoOffset>(units.polygons[u]));
            }
        }
    }
    auto writeNumbers = [&](const std::string& name,
                            const std::function<int(std::size_t)>& numberOf) {
        const std::string attributeName = trimmed(name);
        if (attributeName.empty()) {
            return;
        }
        detail.removeAttribute(eGeoAttribClassPrimitive, attributeName);
        GeoAttribute& attribute = detail.addAttribute(eGeoAttribClassPrimitive, GeoAttributeDesc { attributeName, eGeoStorageInt32, 1, eGeoTypeInfoNone, false });
        detail.detail().primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
            const double none = -1.;
            geoWriteNumeric(attribute, (std::size_t)primitive, std::span<const double>(&none, 1));
        });
        for (std::size_t u = 0; u < count; ++u) {
            const double number = numberOf(u);
            for (GeoOffset primitive : units.polygons[u]) {
                geoWriteNumeric(attribute, (std::size_t)primitive, std::span<const double>(&number, 1));
            }
        }
    };
    if (params.outputIslandNumber) {
        writeNumbers(params.islandNumberAttribute, [](std::size_t u) {
            return (int)u;
        });
    }
    if (params.outputAssignedTarget) {
        writeNumbers(params.assignedTargetAttribute, [&packed](std::size_t u) {
            return packed.placements[u].packed ? packed.placements[u].target : -1;
        });
    }

    return true;
} // layoutGeoUVs

namespace {
const int kRotationDivisions[] = { 1, 2, 4, 8, 16, 32 };
const int kRotationStepCustom = 6;
} // anonymous namespace

GeoUVLayout::GeoUVLayout(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoUVLayout::~GeoUVLayout()
{
}

const NodeSpec*
GeoUVLayout::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoUVLayout::initializeKnobs()
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
                                      double defaultValue,
                                      double minimum) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setMinimum(minimum);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createInt = [this, &page](const QString& label,
                                   const char* name,
                                   const QString& toolTip,
                                   int defaultValue,
                                   int minimum) {
        KnobIntPtr knob = createKnob<KnobInt>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setMinimum(minimum);
        knob->setAnimationEnabled(false);
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
    const QString overrideFormat = tr(" Space-separated polygon:value pairs, the polygon's number naming its island; a "
                                      "later pair wins, and the pairs win over the attribute.");

    _uvAttribute = createString(tr("UV Attribute"), kGeoUVLayoutParamUVAttribute,
                                tr("The point or vertex attribute holding the texture coordinates to pack. It stays of "
                                   "its class."),
                                "uv");
    _group = createString(tr("Group"), kGeoUVLayoutParamGroup,
                          tr("The polygons to pack, as a primitive group pattern. Empty packs every polygon. The "
                             "islands of the other polygons stay where they are, and can be avoided or stacked on."),
                          std::string());
    _separatorEdges = createString(tr("Separator Edges"), kGeoUVLayoutParamSeparatorEdges,
                                   tr("Edges the islands are cut along, as an edge pattern, instead of where the uv "
                                      "breaks. Empty finds the islands from uv connectivity."),
                                   std::string());
    _islandAttribute = createString(tr("Island Attribute"), kGeoUVLayoutParamIslandAttribute,
                                    tr("A primitive attribute; islands sharing a value of it are packed as one, keeping "
                                       "their places relative to each other. Empty merges nothing."),
                                    std::string());
    _targetAttribute = createString(tr("Target Attribute"), kGeoUVLayoutParamTargetAttribute,
                                    tr("A primitive attribute naming the target of each island: a rectangle's number, a "
                                       "UDIM tile, or a target island's number. A negative value, or no attribute, lets "
                                       "the packer choose."),
                                    std::string());
    _targetOverrides = createString(tr("Target Overrides"), kGeoUVLayoutParamTargetOverrides,
                                    tr("Targets for single islands, as with Target Attribute.") + overrideFormat,
                                    std::string());
    _islandScaleAttribute = createString(tr("Island Scale Attribute"), kGeoUVLayoutParamIslandScaleAttribute,
                                         tr("A primitive attribute scaling each island relative to the others."),
                                         std::string());
    _scaleOverrides = createString(tr("Scale Overrides"), kGeoUVLayoutParamScaleOverrides,
                                   tr("Relative scales for single islands.") + overrideFormat, std::string());
    _islandOffsetAttribute = createString(tr("Island Offset Attribute"), kGeoUVLayoutParamIslandOffsetAttribute,
                                          tr("A primitive attribute added to the third uv component of each island, to "
                                             "keep stacked or layered islands apart."),
                                          std::string());
    _offsetOverrides = createString(tr("Offset Overrides"), kGeoUVLayoutParamOffsetOverrides,
                                    tr("Third-component offsets for single islands.") + overrideFormat, std::string());
    _scaleIslandsToMatchSurfaceArea = createToggle(tr("Scale Islands To Match Surface Area"), kGeoUVLayoutParamScaleIslandsToMatchSurfaceArea,
                                                   tr("Scales each island so its uv area follows the area of its "
                                                      "polygons, giving every island the same texel density."),
                                                   false);
    _axisAlignment = createChoice(tr("Axis Alignment"), kGeoUVLayoutParamAxisAlignment,
                                  tr("How each island is turned before packing; the rotation step turns it further."),
                                  { ChoiceOption("none", tr("None").toStdString(), tr("Keeps each island's orientation.").toStdString()),
                                    ChoiceOption("symmetry", tr("By Island Symmetry").toStdString(),
                                                 tr("Turns the smallest rectangle round each island level, its long side "
                                                    "along u.")
                                                     .toStdString()),
                                    ChoiceOption("position", tr("By Island Position In 3D").toStdString(),
                                                 tr("Turns each island so the coordinate axes lying in its polygons run "
                                                    "along u and v.")
                                                     .toStdString()) },
                                  (int)eGeoUVLayoutAxisAlignmentIslandSymmetry);
    _scale = createChoice(tr("Scale"), kGeoUVLayoutParamScale,
                          tr("How large the islands are packed."),
                          { ChoiceOption("largest", tr("Largest Packable").toStdString(),
                                         tr("As large as they all still fit, searched to Scale Search Tolerance.").toStdString()),
                            ChoiceOption("fixed", tr("Fixed").toStdString(),
                                         tr("At Fixed Scale; the islands that do not fit go beside the targets and into "
                                            "the nonpacked group.")
                                             .toStdString()) },
                          (int)eGeoUVPackScaleLargest);
    _fixedScale = createDouble(tr("Fixed Scale"), kGeoUVLayoutParamFixedScale,
                               tr("The scale of every island with Scale at Fixed."), 1., 0.);
    _rotationStep = createChoice(tr("Rotation Step"), kGeoUVLayoutParamRotationStep,
                                 tr("The turns tried for each island while packing."),
                                 { ChoiceOption("none", tr("No Rotation").toStdString(), std::string()),
                                   ChoiceOption("180", tr("180").toStdString(), std::string()),
                                   ChoiceOption("90", tr("90").toStdString(), std::string()),
                                   ChoiceOption("45", tr("45").toStdString(), std::string()),
                                   ChoiceOption("22.5", tr("22.5").toStdString(), std::string()),
                                   ChoiceOption("11.25", tr("11.25").toStdString(), std::string()),
                                   ChoiceOption("custom", tr("Custom").toStdString(),
                                                tr("Custom Rotation Divisions turns of the full circle.").toStdString()) },
                                 2);
    _customRotationDivisions = createInt(tr("Custom Rotation Divisions"), kGeoUVLayoutParamCustomRotationDivisions,
                                         tr("The number of equal turns of the full circle tried with Rotation Step at "
                                            "Custom."),
                                         4, 1);
    _avoidPackingOverNonGroupIslands = createToggle(tr("Avoid Packing Over Non Group Islands"), kGeoUVLayoutParamAvoidPackingOverNonGroupIslands,
                                                    tr("Packs round the islands of the polygons outside the group."), false);
    _packInCavities = createToggle(tr("Pack In Cavities"), kGeoUVLayoutParamPackInCavities,
                                   tr("Lets islands go into the holes and notches of others."), true);
    _islandPadding = createInt(tr("Island Padding"), kGeoUVLayoutParamIslandPadding,
                               tr("The space kept between islands, in pixels of the search resolution."), 1, 0);
    _padBoundaries = createToggle(tr("Pad Boundaries"), kGeoUVLayoutParamPadBoundaries,
                                  tr("Keeps the padding between the islands and the target edges too."), false);
    _spreadIslands = createToggle(tr("Spread Islands"), kGeoUVLayoutParamSpreadIslands,
                                  tr("Spreads the islands evenly over the target instead of packing them into a corner."),
                                  false);
    _iterations = createInt(tr("Iterations"), kGeoUVLayoutParamIterations,
                            tr("The number of packings tried with shuffled orders, keeping the tightest. They run in "
                               "parallel."),
                            1, 1);
    _searchResolution = createInt(tr("Search Resolution"), kGeoUVLayoutParamSearchResolution,
                                  tr("The pixels along the long side of the largest target, or of a tile, that islands "
                                     "are rasterized at. Higher packs tighter and slower."),
                                  1024, 1);
    _connectivityTolerance = createDouble(tr("Connectivity Tolerance"), kGeoUVLayoutParamConnectivityTolerance,
                                          tr("How close in uv two vertices on one point must be to join their polygons "
                                             "in one island."),
                                          1e-5, 0.);
    _scaleSearchTolerance = createDouble(tr("Scale Search Tolerance"), kGeoUVLayoutParamScaleSearchTolerance,
                                         tr("How finely the largest packable scale is searched, as a fraction of it."), 0.005, 0.);
    _randomSeed = createInt(tr("Random Seed"), kGeoUVLayoutParamRandomSeed,
                            tr("The seed of the shuffled orders; one seed always gives one layout."), 5489, 0);
    _packInto = createChoice(tr("Pack Into"), kGeoUVLayoutParamPackInto,
                             tr("What the islands are packed into."),
                             { ChoiceOption("rectangles", tr("Rectangles").toStdString(),
                                            tr("The Target Rectangles, numbered from 0.").toStdString()),
                               ChoiceOption("udim", tr("UDIM Tiles").toStdString(),
                                            tr("A grid of tiles numbered as UDIMs, filled from Starting UDIM.").toStdString()),
                               ChoiceOption("islands", tr("Islands From Second Input").toStdString(),
                                            tr("The uv islands of the Targets input, numbered as found.").toStdString()) },
                             (int)eGeoUVPackTargetRectangles);
    _useDefaultTarget = createToggle(tr("Use Default Target"), kGeoUVLayoutParamUseDefaultTarget,
                                     tr("Sends the islands with no target to Default Target instead of letting the "
                                        "packer choose."),
                                     false);
    _defaultTarget = createInt(tr("Default Target"), kGeoUVLayoutParamDefaultTarget,
                               tr("The target of the islands with no target, with Use Default Target on."), 1001, 0);
    {
        KnobRowTablePtr knob = createKnob<KnobRowTable>(tr("Target Rectangles").toStdString());
        knob->setName(kGeoUVLayoutParamTargetRectangles);
        knob->setHintToolTip(tr("The rectangles packed into with Pack Into at Rectangles, each a Center and a Size in "
                                "uv, as \"u v\".")
                                 .toStdString());
        knob->setColumns({ KnobRowTable::Column { "Center", "center", "0.5 0.5" },
                           KnobRowTable::Column { "Size", "size", "1 1" } });
        knob->setDefaultValue(knob->encodeToKnobTableFormat({ { "0.5 0.5", "1 1" } }));
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        _targetRectangles = knob;
    }
    _tileSize = createDouble(tr("Tile Size"), kGeoUVLayoutParamTileSize,
                             tr("The width and height of a UDIM tile, in uv."), 1., 0.);
    _columns = createInt(tr("Columns"), kGeoUVLayoutParamColumns,
                         tr("The tiles in a row of the UDIM grid."), 10, 1);
    _startingUdim = createInt(tr("Starting UDIM"), kGeoUVLayoutParamStartingUdim,
                              tr("The first tile filled."), 1001, 1001);
    _targetUVAttribute = createString(tr("Target UV Attribute"), kGeoUVLayoutParamTargetUVAttribute,
                                      tr("The uv attribute of the Targets input whose islands are packed into."), "uv");
    _targetGroup = createString(tr("Target Group"), kGeoUVLayoutParamTargetGroup,
                                tr("The polygons of the Targets input to take islands from, as a primitive group "
                                   "pattern. Empty takes every polygon."),
                                std::string());
    _targetSeparatorEdges = createString(tr("Target Separator Edges"), kGeoUVLayoutParamTargetSeparatorEdges,
                                         tr("Edges of the Targets input its islands are cut along, instead of where its "
                                            "uv breaks."),
                                         std::string());
    _targetIslandAttribute = createString(tr("Target Island Attribute"), kGeoUVLayoutParamTargetIslandAttribute,
                                          tr("A primitive attribute of the Targets input; islands sharing a value of it "
                                             "are one target."),
                                          std::string());
    _stackIdenticalIslands = createToggle(tr("Stack Identical Islands"), kGeoUVLayoutParamStackIdenticalIslands,
                                          tr("Places islands of the same shape on top of each other instead of side by "
                                             "side."),
                                          false);
    _matchMirroredIslands = createToggle(tr("Match Mirrored Islands"), kGeoUVLayoutParamMatchMirroredIslands,
                                         tr("Also stacks islands that are mirror images of each other."), false);
    _allowStackingOnNonGroupIslands = createToggle(tr("Allow Stacking On Non Group Islands"), kGeoUVLayoutParamAllowStackingOnNonGroupIslands,
                                                   tr("Lets islands stack on identical islands outside the group."), false);
    _shapeTolerance = createDouble(tr("Shape Tolerance"), kGeoUVLayoutParamShapeTolerance,
                                   tr("How much two island shapes may differ, as a fraction of their size, and still "
                                      "stack."),
                                   0.01, 0.);
    _outputNonpacked = createToggle(tr("Output Nonpacked"), kGeoUVLayoutParamOutputNonpacked,
                                    tr("Writes the polygons of the islands that did not fit as a primitive group."), true);
    _nonpackedGroup = createString(tr("Nonpacked Group"), kGeoUVLayoutParamNonpackedGroup,
                                   tr("The name of the nonpacked primitive group."), "nonpacked");
    _outputIslandNumber = createToggle(tr("Output Island Number"), kGeoUVLayoutParamOutputIslandNumber,
                                       tr("Writes the number of each polygon's island as a primitive attribute; -1 "
                                          "outside the group."),
                                       false);
    _islandNumberAttribute = createString(tr("Island Number Attribute"), kGeoUVLayoutParamIslandNumberAttribute,
                                          tr("The name of the island number attribute."), "island");
    _outputAssignedTarget = createToggle(tr("Output Assigned Target"), kGeoUVLayoutParamOutputAssignedTarget,
                                         tr("Writes the target each polygon's island was packed into as a primitive "
                                            "attribute; -1 when it was not packed or is outside the group."),
                                         false);
    _assignedTargetAttribute = createString(tr("Assigned Target Attribute"), kGeoUVLayoutParamAssignedTargetAttribute,
                                            tr("The name of the assigned target attribute."), "target");
    updateEnabled();
} // GeoUVLayout::initializeKnobs

void
GeoUVLayout::updateEnabled()
{
    auto on = [](const KnobBoolWPtr& weak) {
        const KnobBoolPtr knob = weak.lock();

        return knob && knob->getValue();
    };
    auto choice = [](const KnobChoiceWPtr& weak) {
        const KnobChoicePtr knob = weak.lock();

        return knob ? knob->getValue() : 0;
    };
    auto enable = [](const KnobIPtr& knob,
                     bool enabled) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    const int packInto = choice(_packInto);
    const bool stacking = on(_stackIdenticalIslands);
    enable(_fixedScale.lock(), choice(_scale) == (int)eGeoUVPackScaleFixed);
    enable(_customRotationDivisions.lock(), choice(_rotationStep) == kRotationStepCustom);
    enable(_defaultTarget.lock(), on(_useDefaultTarget));
    enable(_targetRectangles.lock(), packInto == (int)eGeoUVPackTargetRectangles);
    for (const KnobIPtr& knob : { KnobIPtr(_tileSize.lock()), KnobIPtr(_columns.lock()), KnobIPtr(_startingUdim.lock()) }) {
        enable(knob, packInto == (int)eGeoUVPackTargetUDIM);
    }
    for (const KnobIPtr& knob : { KnobIPtr(_targetUVAttribute.lock()), KnobIPtr(_targetGroup.lock()),
                                  KnobIPtr(_targetSeparatorEdges.lock()), KnobIPtr(_targetIslandAttribute.lock()) }) {
        enable(knob, packInto == (int)eGeoUVPackTargetIslands);
    }
    enable(_matchMirroredIslands.lock(), stacking);
    enable(_allowStackingOnNonGroupIslands.lock(), stacking);
    enable(_shapeTolerance.lock(), stacking);
    enable(_nonpackedGroup.lock(), on(_outputNonpacked));
    enable(_islandNumberAttribute.lock(), on(_outputIslandNumber));
    enable(_assignedTargetAttribute.lock(), on(_outputAssignedTarget));
} // GeoUVLayout::updateEnabled

void
GeoUVLayout::onKnobsLoaded()
{
    updateEnabled();
}

bool
GeoUVLayout::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    for (const KnobIPtr& knob : { KnobIPtr(_scale.lock()), KnobIPtr(_rotationStep.lock()), KnobIPtr(_useDefaultTarget.lock()),
                                  KnobIPtr(_packInto.lock()), KnobIPtr(_stackIdenticalIslands.lock()),
                                  KnobIPtr(_outputNonpacked.lock()), KnobIPtr(_outputIslandNumber.lock()),
                                  KnobIPtr(_outputAssignedTarget.lock()) }) {
        if (knob && (k == knob.get())) {
            updateEnabled();

            return true;
        }
    }

    return false;
}

StatusEnum
GeoUVLayout::renderGeometry(const GeoRenderActionArgs& args,
                            GeoDetailConstPtr* outputDetail,
                            std::string* errorMessage)
{
    GeoUVLayoutParams params = readParams(args.time, args.view);
    const ParallelRenderArgsPtr frameArgs = getFrameArgs();
    const AbortableRenderInfoPtr render = frameArgs ? frameArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    if (render) {
        params.aborted = [render]() {
            return render->isAborted();
        };
    }
    GeoDetailConstPtr targets;
    if (params.pack.targetType == eGeoUVPackTargetIslands) {
        targets = args.getInputDetail(1);
    }
    GeoUVLayoutReport report;
    const StatusEnum status = renderGeometryFromInput(args, 0, [&params, &report, &targets](GeoDetailBuilder& detail, std::string* error) {
        std::string message;
        if (!layoutGeoUVs(detail, targets.get(), params, &report, &message)) {
            if (error) {
                *error = message;
            }

            return report.aborted ? eStatusOK : eStatusFailed;
        }

        return eStatusOK; }, outputDetail, errorMessage);
    if (status != eStatusOK) {
        return status;
    }
    std::vector<std::string> warnings;
    if (report.nonpacked > 0) {
        warnings.push_back(tr("%1 island(s) did not fit and were placed beside the targets.").arg(report.nonpacked).toStdString());
    }
    if (report.maxUdim > 1100) {
        warnings.push_back(tr("The layout reaches UDIM tile %1, past 1100.").arg(report.maxUdim).toStdString());
    }
    if (report.ignoredOverrides > 0) {
        warnings.push_back(tr("%1 override(s) are malformed or name no polygon in the group and were ignored.")
                               .arg(report.ignoredOverrides)
                               .toStdString());
    }
    if (warnings.empty()) {
        clearPersistentMessage(false);
    } else {
        std::string text;
        for (const std::string& w : warnings) {
            text += (text.empty() ? "" : "\n") + w;
        }
        setPersistentMessage(eMessageTypeWarning, text);
    }

    return eStatusOK;
} // GeoUVLayout::renderGeometry

GeoUVLayoutParams
GeoUVLayout::readParams(double time,
                        ViewSpec view) const
{
    GeoUVLayoutParams p;
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
    auto integer = [time, view](const KnobIntWPtr& weak,
                                int fallback) {
        const KnobIntPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto real = [time, view](const KnobDoubleWPtr& weak,
                             double fallback) {
        const KnobDoublePtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto choice = [time, view](const KnobChoiceWPtr& weak,
                               int fallback) {
        const KnobChoicePtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    p.islands.uvAttribute = text(_uvAttribute, p.islands.uvAttribute);
    p.islands.group = text(_group, p.islands.group);
    p.islands.separatorEdges = text(_separatorEdges, p.islands.separatorEdges);
    p.islands.islandAttribute = text(_islandAttribute, p.islands.islandAttribute);
    p.targetAttribute = text(_targetAttribute, p.targetAttribute);
    p.targetOverrides = text(_targetOverrides, p.targetOverrides);
    p.islandScaleAttribute = text(_islandScaleAttribute, p.islandScaleAttribute);
    p.scaleOverrides = text(_scaleOverrides, p.scaleOverrides);
    p.islandOffsetAttribute = text(_islandOffsetAttribute, p.islandOffsetAttribute);
    p.offsetOverrides = text(_offsetOverrides, p.offsetOverrides);
    p.scaleIslandsToMatchSurfaceArea = toggle(_scaleIslandsToMatchSurfaceArea, p.scaleIslandsToMatchSurfaceArea);
    const int alignment = choice(_axisAlignment, (int)p.axisAlignment);
    p.axisAlignment = ((alignment >= 0) && (alignment <= (int)eGeoUVLayoutAxisAlignmentIslandPosition)) ? (GeoUVLayoutAxisAlignmentEnum)alignment : eGeoUVLayoutAxisAlignmentNone;
    p.pack.scaleMode = (choice(_scale, 0) == (int)eGeoUVPackScaleFixed) ? eGeoUVPackScaleFixed : eGeoUVPackScaleLargest;
    p.pack.fixedScale = real(_fixedScale, p.pack.fixedScale);
    const int step = choice(_rotationStep, 2);
    p.pack.rotationDivisions = (step == kRotationStepCustom) ? std::max(1, integer(_customRotationDivisions, 4))
                                                             : kRotationDivisions[std::clamp(step, 0, kRotationStepCustom - 1)];
    p.avoidPackingOverNonGroupIslands = toggle(_avoidPackingOverNonGroupIslands, p.avoidPackingOverNonGroupIslands);
    p.pack.packInCavities = toggle(_packInCavities, p.pack.packInCavities);
    p.pack.padding = integer(_islandPadding, p.pack.padding);
    p.pack.padBoundaries = toggle(_padBoundaries, p.pack.padBoundaries);
    p.pack.spread = toggle(_spreadIslands, p.pack.spread);
    p.pack.iterations = integer(_iterations, p.pack.iterations);
    p.pack.resolution = integer(_searchResolution, p.pack.resolution);
    p.connectivityTolerance = real(_connectivityTolerance, p.connectivityTolerance);
    p.pack.scaleTolerance = real(_scaleSearchTolerance, p.pack.scaleTolerance);
    p.pack.seed = (std::uint32_t)std::max(0, integer(_randomSeed, (int)p.pack.seed));
    const int packInto = choice(_packInto, 0);
    p.pack.targetType = (packInto == (int)eGeoUVPackTargetUDIM) ? eGeoUVPackTargetUDIM
        : (packInto == (int)eGeoUVPackTargetIslands)            ? eGeoUVPackTargetIslands
                                                                : eGeoUVPackTargetRectangles;
    p.pack.defaultTarget = toggle(_useDefaultTarget, false) ? integer(_defaultTarget, 1001) : -1;
    p.pack.rectangles.clear();
    if (const KnobRowTablePtr knob = _targetRectangles.lock()) {
        std::list<std::vector<std::string>> table;
        try {
            knob->getTable(&table);
        } catch (const std::exception&) {
            table.clear();
        }
        for (const std::vector<std::string>& row : table) {
            GeoUV center = { 0.5, 0.5 }, size = { 1., 1. };
            if (((row.size() > 0) && !geoUVFlattenParseUV(row[0], &center)) || ((row.size() > 1) && !geoUVFlattenParseUV(row[1], &size))) {
                continue;
            }
            GeoUVPackRect rect;
            rect.x0 = center[0] - 0.5 * size[0];
            rect.x1 = center[0] + 0.5 * size[0];
            rect.y0 = center[1] - 0.5 * size[1];
            rect.y1 = center[1] + 0.5 * size[1];
            p.pack.rectangles.push_back(rect);
        }
    }
    p.pack.tileSize = real(_tileSize, p.pack.tileSize);
    p.pack.columns = integer(_columns, p.pack.columns);
    p.pack.startingUdim = integer(_startingUdim, p.pack.startingUdim);
    p.targets.uvAttribute = text(_targetUVAttribute, p.targets.uvAttribute);
    p.targets.group = text(_targetGroup, p.targets.group);
    p.targets.separatorEdges = text(_targetSeparatorEdges, p.targets.separatorEdges);
    p.targets.islandAttribute = text(_targetIslandAttribute, p.targets.islandAttribute);
    p.pack.stackIdentical = toggle(_stackIdenticalIslands, p.pack.stackIdentical);
    p.pack.stackMirrored = toggle(_matchMirroredIslands, p.pack.stackMirrored);
    p.allowStackingOnNonGroupIslands = toggle(_allowStackingOnNonGroupIslands, p.allowStackingOnNonGroupIslands);
    p.pack.stackTolerance = real(_shapeTolerance, p.pack.stackTolerance);
    p.outputNonpacked = toggle(_outputNonpacked, p.outputNonpacked);
    p.nonpackedGroup = text(_nonpackedGroup, p.nonpackedGroup);
    p.outputIslandNumber = toggle(_outputIslandNumber, p.outputIslandNumber);
    p.islandNumberAttribute = text(_islandNumberAttribute, p.islandNumberAttribute);
    p.outputAssignedTarget = toggle(_outputAssignedTarget, p.outputAssignedTarget);
    p.assignedTargetAttribute = text(_assignedTargetAttribute, p.assignedTargetAttribute);

    return p;
} // GeoUVLayout::readParams

NATRON_NAMESPACE_EXIT
