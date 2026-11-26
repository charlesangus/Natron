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

#include "Engine/Geo/GeoGroupOps.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

NATRON_NAMESPACE_ENTER

namespace {
bool
maskHas(const GeoElementMask& mask,
        GeoOffset offset)
{
    return offset >= 0 && std::size_t(offset) < mask.size() && mask[std::size_t(offset)];
}

GeoElementMask
liveOnly(const GeoElementMask& mask,
         const GeoIndexMap& map)
{
    GeoElementMask out(std::size_t(map.offsetSize()), 0);
    map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
        out[std::size_t(offset)] = maskHas(mask, offset) ? 1 : 0;
    });

    return out;
}

void
sortUnique(std::vector<GeoEdge>& edges)
{
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
}

bool
edgeIn(const std::vector<GeoEdge>& sorted,
       const GeoEdge& edge)
{
    return std::binary_search(sorted.begin(), sorted.end(), edge);
}

void
appendPrimitiveEdges(const GeoDetail& detail,
                     GeoOffset prim,
                     std::vector<GeoEdge>& out)
{
    geoForEachPrimitiveEdge(detail, prim, [&](const GeoEdge& edge) {
        out.push_back(edge);
    });
}

std::vector<GeoEdge>
detailEdges(const GeoDetail& detail)
{
    std::vector<GeoEdge> edges;
    geoForEachEdge(detail, [&](const GeoEdge& edge, GeoOffset /*prim*/) {
        edges.push_back(edge);
    });
    sortUnique(edges);

    return edges;
}

std::vector<GeoEdge>
liveEdgesOf(const GeoDetail& detail,
            std::span<const GeoEdge> edges)
{
    const std::vector<GeoEdge> all = detailEdges(detail);
    std::vector<GeoEdge> out;
    for (const GeoEdge& e : edges) {
        if (edgeIn(all, e)) {
            out.push_back(e);
        }
    }
    sortUnique(out);

    return out;
}

class Neighbours {
public:
    explicit Neighbours(const GeoDetail& detail)
        : _detail(detail)
        , _reverse(detail.reverseTopology())
    {
    }

    /** Points sharing an edge with the point, ascending. */
    void points(GeoOffset point,
                std::vector<GeoOffset>& out)
    {
        out.clear();
        primitivesOf(point);
        for (GeoOffset prim : _prims) {
            _edges.clear();
            appendPrimitiveEdges(_detail, prim, _edges);
            for (const GeoEdge& e : _edges) {
                if (e.p0 == point) {
                    out.push_back(e.p1);
                } else if (e.p1 == point) {
                    out.push_back(e.p0);
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    /** Primitives sharing an edge with the primitive, ascending. */
    void primitives(GeoOffset prim,
                    std::vector<GeoOffset>& out)
    {
        out.clear();
        std::vector<GeoEdge> own;
        appendPrimitiveEdges(_detail, prim, own);
        for (const GeoEdge& e : own) {
            primitivesOf(e.p0);
            const std::vector<GeoOffset> candidates = _prims;
            for (GeoOffset other : candidates) {
                if (other == prim || !_detail.primitiveMap().isLive(other)) {
                    continue;
                }
                _edges.clear();
                appendPrimitiveEdges(_detail, other, _edges);
                if (std::find(_edges.begin(), _edges.end(), e) != _edges.end()) {
                    out.push_back(other);
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    /** Edges sharing a point with the edge, itself excluded. */
    void edges(const GeoEdge& edge,
               std::vector<GeoEdge>& out)
    {
        out.clear();
        for (GeoOffset p : { edge.p0, edge.p1 }) {
            points(p, _points);
            for (GeoOffset n : _points) {
                const GeoEdge e(p, n);
                if (e != edge) {
                    out.push_back(e);
                }
            }
        }
        sortUnique(out);
    }

private:
    void primitivesOf(GeoOffset point)
    {
        _prims.clear();
        _reverse->pointPrimitives(point, _detail.topology(), _prims);
    }

    const GeoDetail& _detail;
    std::shared_ptr<const GeoReverseTopology> _reverse;
    std::vector<GeoOffset> _prims;
    std::vector<GeoEdge> _edges;
    std::vector<GeoOffset> _points;
};

template <typename NeighbourFn>
GeoElementMask
expandMask(const GeoElementMask& start,
           const GeoIndexMap& map,
           int steps,
           NeighbourFn&& neighbours)
{
    GeoElementMask cur = start;
    const bool grow = steps > 0;
    std::vector<GeoOffset> list;
    for (int s = std::abs(steps); s > 0; --s) {
        GeoElementMask next = cur;
        map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
            if (!cur[std::size_t(offset)]) {
                return;
            }
            neighbours(offset, list);
            for (GeoOffset n : list) {
                if (!map.isLive(n)) {
                    continue;
                }
                if (grow) {
                    next[std::size_t(n)] = 1;
                } else if (!cur[std::size_t(n)]) {
                    next[std::size_t(offset)] = 0;
                    break;
                }
            }
        });
        cur = std::move(next);
    }

    return cur;
}
} // namespace

GeoElementMask
geoPromotePointsToPrimitives(const GeoDetail& detail,
                             const GeoElementMask& points,
                             bool entirelyContained)
{
    const GeoElementMask in = liveOnly(points, detail.pointMap());
    GeoElementMask out(std::size_t(detail.primitiveMap().offsetSize()), 0);
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange range = detail.primitiveVertices(prim);
        if (range.count <= 0) {
            return;
        }
        GeoOffset inside = 0;
        for (GeoOffset i = 0; i < range.count; ++i) {
            if (maskHas(in, detail.vertexPoint(range.start + i))) {
                ++inside;
            }
        }
        out[std::size_t(prim)] = (entirelyContained ? inside == range.count : inside > 0) ? 1 : 0;
    });

    return out;
}

GeoElementMask
geoPromotePrimitivesToPoints(const GeoDetail& detail,
                             const GeoElementMask& primitives)
{
    GeoElementMask out(std::size_t(detail.pointMap().offsetSize()), 0);
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (!maskHas(primitives, prim)) {
            return;
        }
        const GeoVertexRange range = detail.primitiveVertices(prim);
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset p = detail.vertexPoint(range.start + i);
            if (detail.pointMap().isLive(p)) {
                out[std::size_t(p)] = 1;
            }
        }
    });

    return out;
}

GeoElementMask
geoPromoteEdgesToPoints(const GeoDetail& detail,
                        std::span<const GeoEdge> edges)
{
    GeoElementMask out(std::size_t(detail.pointMap().offsetSize()), 0);
    for (const GeoEdge& e : liveEdgesOf(detail, edges)) {
        out[std::size_t(e.p0)] = 1;
        out[std::size_t(e.p1)] = 1;
    }

    return out;
}

GeoElementMask
geoPromoteEdgesToPrimitives(const GeoDetail& detail,
                            std::span<const GeoEdge> edges)
{
    std::vector<GeoEdge> sorted(edges.begin(), edges.end());
    sortUnique(sorted);
    GeoElementMask out(std::size_t(detail.primitiveMap().offsetSize()), 0);
    geoForEachEdge(detail, [&](const GeoEdge& edge, GeoOffset prim) {
        if (edgeIn(sorted, edge)) {
            out[std::size_t(prim)] = 1;
        }
    });

    return out;
}

std::vector<GeoEdge>
geoPromotePointsToEdges(const GeoDetail& detail,
                        const GeoElementMask& points,
                        bool entirelyContained)
{
    const GeoElementMask in = liveOnly(points, detail.pointMap());
    std::vector<GeoEdge> out;
    for (const GeoEdge& e : detailEdges(detail)) {
        const bool a = maskHas(in, e.p0);
        const bool b = maskHas(in, e.p1);
        if (entirelyContained ? (a && b) : (a || b)) {
            out.push_back(e);
        }
    }

    return out;
}

std::vector<GeoEdge>
geoPromotePrimitivesToEdges(const GeoDetail& detail,
                            const GeoElementMask& primitives)
{
    std::vector<GeoEdge> out;
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (maskHas(primitives, prim)) {
            appendPrimitiveEdges(detail, prim, out);
        }
    });
    sortUnique(out);

    return out;
}

GeoElementMask
geoExpandPoints(const GeoDetail& detail,
                const GeoElementMask& points,
                int steps)
{
    Neighbours nb(detail);

    return expandMask(liveOnly(points, detail.pointMap()), detail.pointMap(), steps,
                      [&](GeoOffset p, std::vector<GeoOffset>& out) { nb.points(p, out); });
}

GeoElementMask
geoExpandPrimitives(const GeoDetail& detail,
                    const GeoElementMask& primitives,
                    int steps)
{
    Neighbours nb(detail);
    const GeoElementMask start = liveOnly(primitives, detail.primitiveMap());

    return expandMask(start, detail.primitiveMap(), steps,
                      [&](GeoOffset prim, std::vector<GeoOffset>& out) { nb.primitives(prim, out); });
}

std::vector<GeoEdge>
geoExpandEdges(const GeoDetail& detail,
               std::span<const GeoEdge> edges,
               int steps)
{
    Neighbours nb(detail);
    std::vector<GeoEdge> cur = liveEdgesOf(detail, edges);
    std::vector<GeoEdge> list;
    const bool grow = steps > 0;
    for (int s = std::abs(steps); s > 0 && !cur.empty(); --s) {
        std::vector<GeoEdge> next;
        for (const GeoEdge& e : cur) {
            nb.edges(e, list);
            if (grow) {
                next.push_back(e);
                next.insert(next.end(), list.begin(), list.end());
            } else if (std::all_of(list.begin(), list.end(), [&](const GeoEdge& n) { return edgeIn(cur, n); })) {
                next.push_back(e);
            }
        }
        sortUnique(next);
        cur = std::move(next);
    }

    return cur;
}

NATRON_NAMESPACE_EXIT
