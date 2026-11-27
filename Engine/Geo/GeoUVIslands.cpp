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

#include "GeoUVIslands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <utility>

#include "Engine/Geo/GeoAttribute.h"
#include "Engine/Geo/GeoPage.h"

NATRON_NAMESPACE_ENTER

namespace {
double
numericComponent(const GeoAttribute& attribute,
                 std::size_t offset,
                 int component)
{
    switch (attribute.storage()) {
    case eGeoStorageInt32:
        return (double)attribute.get<std::int32_t>(offset, component);
    case eGeoStorageInt64:
        return (double)attribute.get<std::int64_t>(offset, component);
    case eGeoStorageFloat32:
        return (double)attribute.get<float>(offset, component);
    case eGeoStorageFloat64:
        return attribute.get<double>(offset, component);
    case eGeoStorageString:
    default:
        return 0.;
    }
}

bool
isNumericScalar(const GeoAttribute& attribute)
{
    return !attribute.isArray() && (attribute.storage() != eGeoStorageString);
}

int
findRoot(std::vector<int>& parent,
         int x)
{
    while (parent[(std::size_t)x] != x) {
        parent[(std::size_t)x] = parent[(std::size_t)parent[(std::size_t)x]];
        x = parent[(std::size_t)x];
    }

    return x;
}

// Whether a polygon has no area to lay uv out over. The threshold is relative to the perimeter
// squared so it does not depend on the model's scale.
bool
isDegenerate(const GeoDetail& detail,
             GeoOffset primitive)
{
    const GeoVertexRange range = detail.primitiveVertices(primitive);
    std::vector<GeoOffset> points((std::size_t)range.count);
    for (GeoOffset i = 0; i < range.count; ++i) {
        points[(std::size_t)i] = detail.vertexPoint(range.start + i);
    }
    std::vector<GeoOffset> distinct = points;
    std::sort(distinct.begin(), distinct.end());
    if (std::unique(distinct.begin(), distinct.end()) - distinct.begin() < 3) {
        return true;
    }
    std::array<double, 3> normal = { 0., 0., 0. };
    double perimeter = 0.;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::array<double, 3> a = detail.pointPosition(points[i]);
        const std::array<double, 3> b = detail.pointPosition(points[(i + 1) % points.size()]);
        normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
        normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
        normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
        perimeter += std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
    }
    const double area = 0.5 * std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);

    return !(area > 1e-12 * perimeter * perimeter);
}

struct VertexEdgeRecord {
    GeoEdge edge;
    GeoOffset vertex;

    bool operator<(const VertexEdgeRecord& other) const
    {
        return (edge < other.edge) || ((edge == other.edge) && (vertex < other.vertex));
    }
};
} // anonymous namespace

GeoOffset
geoVertexEdgeEnd(const GeoDetail& detail,
                 GeoOffset vertex)
{
    const GeoVertexRange range = detail.primitiveVertices(detail.vertexPrimitive(vertex));

    return range.start + (vertex - range.start + 1) % range.count;
}

GeoEdge
geoVertexEdge(const GeoDetail& detail,
              GeoOffset vertex)
{
    return GeoEdge(detail.vertexPoint(vertex), detail.vertexPoint(geoVertexEdgeEnd(detail, vertex)));
}

bool
geoComputeUVIslands(const GeoDetail& detail,
                    const GeoUVIslandOptions& options,
                    GeoUVIslands* out,
                    std::string* errorMessage)
{
    const GeoAttribute* uv = nullptr;
    bool uvOnPoints = false;
    if (options.connectivity == eGeoUVIslandConnectivityUV) {
        uv = detail.findAttribute(eGeoAttribClassVertex, options.uvAttribute);
        if (!uv) {
            uv = detail.findAttribute(eGeoAttribClassPoint, options.uvAttribute);
            uvOnPoints = (uv != nullptr);
        }
        if (!uv) {
            *errorMessage = "There is no vertex or point attribute \"" + options.uvAttribute + "\" to find the islands from.";

            return false;
        }
        if (!isNumericScalar(*uv) || (uv->tupleSize() < 2)) {
            *errorMessage = "The attribute \"" + options.uvAttribute + "\" needs two or more numeric components to be read as uv.";

            return false;
        }
    }
    const GeoAttribute* islandValues = nullptr;
    if (!options.islandAttribute.empty()) {
        islandValues = detail.findAttribute(eGeoAttribClassPrimitive, options.islandAttribute);
        if (!islandValues || islandValues->isArray()
            || ((islandValues->storage() != eGeoStorageInt32) && (islandValues->storage() != eGeoStorageInt64))) {
            *errorMessage = "The island attribute \"" + options.islandAttribute + "\" must be an integer primitive attribute.";

            return false;
        }
    }

    GeoUVIslands result;
    const std::size_t primitiveSize = (std::size_t)detail.primitiveMap().offsetSize();
    const std::size_t vertexSize = (std::size_t)detail.vertexMap().offsetSize();
    result.islandOfPrimitive.assign(primitiveSize, -1);
    result.vertexEdgeTwin.assign(vertexSize, GeoIndexMap::kInvalidOffset);
    result.vertexEdgeCut.assign(vertexSize, 0);

    std::vector<GeoOffset> candidates;
    detail.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        if (detail.primitiveType(primitive) != eGeoPrimitivePolygon) {
            return;
        }
        if (!options.primitives.empty()
            && (((std::size_t)primitive >= options.primitives.size()) || !options.primitives[(std::size_t)primitive])) {
            return;
        }
        candidates.push_back(primitive);
    });
    std::vector<char> degenerate(candidates.size(), 0);
    geoParallelFor(candidates.size(), [&](std::size_t i) {
        degenerate[i] = isDegenerate(detail, candidates[i]) ? 1 : 0;
    });
    std::vector<GeoOffset> polygons;
    std::vector<int> localOf(primitiveSize, -1);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (degenerate[i]) {
            result.degenerate.push_back(candidates[i]);
        } else {
            localOf[(std::size_t)candidates[i]] = (int)polygons.size();
            polygons.push_back(candidates[i]);
        }
    }

    std::vector<VertexEdgeRecord> records;
    for (GeoOffset primitive : polygons) {
        const GeoVertexRange range = detail.primitiveVertices(primitive);
        for (GeoOffset k = 0; k < range.count; ++k) {
            const GeoOffset vertex = range.start + k;
            const GeoEdge edge = geoVertexEdge(detail, vertex);
            // A repeated point gives an edge of no length, which neither joins nor bounds.
            if (edge.p0 != edge.p1) {
                records.push_back({ edge, vertex });
            }
        }
    }
    std::sort(records.begin(), records.end());

    auto uvAt = [&](GeoOffset vertex,
                    int component) {
        const GeoOffset element = uvOnPoints ? detail.vertexPoint(vertex) : vertex;

        return numericComponent(*uv, (std::size_t)element, component);
    };
    auto sameUV = [&](GeoOffset a,
                      GeoOffset b) {
        for (int c = 0; c < 2; ++c) {
            if (std::abs(uvAt(a, c) - uvAt(b, c)) > options.tolerance) {
                return false;
            }
        }

        return true;
    };

    std::vector<int> parent(polygons.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (std::size_t i = 0; i < records.size();) {
        std::size_t j = i + 1;
        while ((j < records.size()) && (records[j].edge == records[i].edge)) {
            ++j;
        }
        const GeoOffset a = records[i].vertex;
        const GeoOffset b = (j - i == 2) ? records[i + 1].vertex : GeoIndexMap::kInvalidOffset;
        const GeoOffset primitiveA = detail.vertexPrimitive(a);
        const bool paired = (b != GeoIndexMap::kInvalidOffset) && (detail.vertexPrimitive(b) != primitiveA);
        bool joined = false;
        if (paired) {
            result.vertexEdgeTwin[(std::size_t)a] = b;
            result.vertexEdgeTwin[(std::size_t)b] = a;
            joined = (options.seams.count(records[i].edge) == 0);
            if (joined && uv && !uvOnPoints) {
                const GeoOffset aEnd = geoVertexEdgeEnd(detail, a);
                const GeoOffset bEnd = geoVertexEdgeEnd(detail, b);
                const bool sameWay = (detail.vertexPoint(a) == detail.vertexPoint(b));
                joined = sameWay ? (sameUV(a, b) && sameUV(aEnd, bEnd)) : (sameUV(a, bEnd) && sameUV(aEnd, b));
            }
        }
        if (joined) {
            const int ra = findRoot(parent, localOf[(std::size_t)primitiveA]);
            const int rb = findRoot(parent, localOf[(std::size_t)detail.vertexPrimitive(b)]);
            parent[(std::size_t)std::max(ra, rb)] = std::min(ra, rb);
        } else {
            for (std::size_t k = i; k < j; ++k) {
                result.vertexEdgeCut[(std::size_t)records[k].vertex] = 1;
            }
        }
        i = j;
    }

    std::vector<int> islandOfRoot(polygons.size(), -1);
    for (std::size_t i = 0; i < polygons.size(); ++i) {
        const int root = findRoot(parent, (int)i);
        if (islandOfRoot[(std::size_t)root] < 0) {
            islandOfRoot[(std::size_t)root] = (int)result.islands.size();
            result.islands.emplace_back();
        }
        const int island = islandOfRoot[(std::size_t)root];
        result.islandOfPrimitive[(std::size_t)polygons[i]] = island;
        result.islands[(std::size_t)island].push_back(polygons[i]);
    }

    result.groupOfIsland.assign(result.islands.size(), -1);
    std::map<std::int64_t, int> groupOfValue;
    for (std::size_t island = 0; island < result.islands.size(); ++island) {
        int group = -1;
        if (islandValues) {
            for (GeoOffset primitive : result.islands[island]) {
                const std::int64_t value = (islandValues->storage() == eGeoStorageInt32)
                    ? (std::int64_t)islandValues->get<std::int32_t>((std::size_t)primitive)
                    : islandValues->get<std::int64_t>((std::size_t)primitive);
                if (value >= 0) {
                    const auto found = groupOfValue.find(value);
                    if (found != groupOfValue.end()) {
                        group = found->second;
                    } else {
                        group = (int)result.groups.size();
                        groupOfValue.emplace(value, group);
                        result.groups.emplace_back();
                    }
                    break;
                }
            }
        }
        if (group < 0) {
            group = (int)result.groups.size();
            result.groups.emplace_back();
        }
        result.groupOfIsland[island] = group;
        result.groups[(std::size_t)group].push_back((int)island);
    }

    // Boundary loops: follow cut vertex edges end to start. Within an island whose polygons
    // disagree on winding an edge may run the other way, so one ending at the point is taken
    // when none starts there.
    std::vector<char> used(vertexSize, 0);
    result.boundaryLoops.resize(result.islands.size());
    for (std::size_t island = 0; island < result.islands.size(); ++island) {
        std::map<GeoOffset, std::vector<GeoOffset>> starting;
        std::map<GeoOffset, std::vector<GeoOffset>> ending;
        std::vector<GeoOffset> cut;
        for (GeoOffset primitive : result.islands[island]) {
            const GeoVertexRange range = detail.primitiveVertices(primitive);
            for (GeoOffset k = 0; k < range.count; ++k) {
                const GeoOffset vertex = range.start + k;
                if (result.vertexEdgeCut[(std::size_t)vertex]) {
                    cut.push_back(vertex);
                    starting[detail.vertexPoint(vertex)].push_back(vertex);
                    ending[detail.vertexPoint(geoVertexEdgeEnd(detail, vertex))].push_back(vertex);
                }
            }
        }
        auto takeFrom = [&](std::map<GeoOffset, std::vector<GeoOffset>>& byPoint,
                            GeoOffset point) {
            const auto found = byPoint.find(point);
            if (found == byPoint.end()) {
                return GeoIndexMap::kInvalidOffset;
            }
            for (GeoOffset vertex : found->second) {
                if (!used[(std::size_t)vertex]) {
                    return vertex;
                }
            }

            return GeoIndexMap::kInvalidOffset;
        };
        // The boundary edge after vertex's, turning round the point it ends at through joined
        // polygons; kInvalidOffset where the turn leaves the island's consistently wound part.
        auto nextRoundPoint = [&](GeoOffset vertex) {
            GeoOffset candidate = geoVertexEdgeEnd(detail, vertex);
            const GeoOffset centre = detail.vertexPoint(candidate);
            for (std::size_t guard = 0; guard < vertexSize; ++guard) {
                if (result.vertexEdgeCut[(std::size_t)candidate]) {
                    return candidate;
                }
                const GeoOffset twin = result.vertexEdgeTwin[(std::size_t)candidate];
                if ((twin == GeoIndexMap::kInvalidOffset) || (detail.vertexPoint(twin) == centre)) {
                    return GeoIndexMap::kInvalidOffset;
                }
                candidate = geoVertexEdgeEnd(detail, twin);
            }

            return GeoIndexMap::kInvalidOffset;
        };
        for (GeoOffset first : cut) {
            if (used[(std::size_t)first]) {
                continue;
            }
            std::vector<GeoOffset> loop;
            const GeoOffset origin = detail.vertexPoint(first);
            GeoOffset vertex = first;
            GeoOffset at = detail.vertexPoint(geoVertexEdgeEnd(detail, first));
            bool forward = true;
            for (;;) {
                used[(std::size_t)vertex] = 1;
                loop.push_back(vertex);
                // Where several boundary edges leave a point, as where a cut branches, the one
                // that follows is found by turning round the point through the polygons joined
                // there; any other would close the loop early. The loop's first point can recur
                // along it, so reaching it closes the loop only when the turn leads back to the
                // first edge or finds nothing.
                GeoOffset next = forward ? nextRoundPoint(vertex) : GeoIndexMap::kInvalidOffset;
                if (next == first) {
                    break;
                }
                if ((at == origin) && ((next == GeoIndexMap::kInvalidOffset) || used[(std::size_t)next])) {
                    break;
                }
                if ((next == GeoIndexMap::kInvalidOffset) || used[(std::size_t)next]) {
                    next = takeFrom(starting, at);
                }
                if (next != GeoIndexMap::kInvalidOffset) {
                    vertex = next;
                    forward = true;
                    at = detail.vertexPoint(geoVertexEdgeEnd(detail, vertex));
                    continue;
                }
                forward = false;
                vertex = takeFrom(ending, at);
                if (vertex == GeoIndexMap::kInvalidOffset) {
                    break;
                }
                at = detail.vertexPoint(vertex);
            }
            result.boundaryLoops[island].push_back(std::move(loop));
        }
    }

    *out = std::move(result);

    return true;
} // geoComputeUVIslands

NATRON_NAMESPACE_EXIT
