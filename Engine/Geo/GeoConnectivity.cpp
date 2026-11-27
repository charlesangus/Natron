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

#include "GeoConnectivity.h"

#include <cstddef>
#include <utility>

#include "Engine/Geo/GeoOpUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

class UnionFind {
public:
    explicit UnionFind(std::size_t size)
        : _parent(size)
        , _rank(size, 0)
    {
        for (std::size_t i = 0; i < size; ++i) {
            _parent[i] = std::int32_t(i);
        }
    }

    std::int32_t find(std::int32_t x)
    {
        while (_parent[std::size_t(x)] != x) {
            // Path halving keeps the trees flat without recursion.
            _parent[std::size_t(x)] = _parent[std::size_t(_parent[std::size_t(x)])];
            x = _parent[std::size_t(x)];
        }

        return x;
    }

    void unite(std::int32_t a,
               std::int32_t b)
    {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (_rank[std::size_t(a)] < _rank[std::size_t(b)]) {
            std::swap(a, b);
        }
        _parent[std::size_t(b)] = a;
        if (_rank[std::size_t(a)] == _rank[std::size_t(b)]) {
            ++_rank[std::size_t(a)];
        }
    }

private:
    std::vector<std::int32_t> _parent;
    std::vector<unsigned char> _rank;
};

// Points joined through the primitives that use them.
UnionFind
connectPoints(const GeoDetail& detail,
              const std::vector<std::size_t>& primitives)
{
    UnionFind sets(detail.elementOffsetSize(eGeoAttribClassPoint));

    for (std::size_t prim : primitives) {
        const GeoVertexRange range = detail.primitiveVertices(GeoOffset(prim));
        if (range.count <= 1) {
            continue;
        }
        const std::int32_t first = std::int32_t(detail.vertexPoint(range.start));
        for (GeoOffset v = range.start + 1; v < range.start + range.count; ++v) {
            sets.unite(first, std::int32_t(detail.vertexPoint(v)));
        }
    }

    return sets;
}

} // anonymous namespace

GeoConnectivity
geoPointConnectivity(const GeoDetail& detail)
{
    GeoConnectivity out;
    const std::vector<std::size_t> primitives = geoLiveOffsets(detail.primitiveMap());
    UnionFind sets = connectPoints(detail, primitives);
    const std::vector<std::size_t> points = geoLiveOffsets(detail.pointMap());
    std::vector<std::int32_t> rootLabel(detail.elementOffsetSize(eGeoAttribClassPoint), -1);

    out.labels.assign(detail.elementOffsetSize(eGeoAttribClassPoint), -1);
    for (std::size_t p : points) {
        std::int32_t& label = rootLabel[std::size_t(sets.find(std::int32_t(p)))];
        if (label < 0) {
            label = out.count++;
        }
        out.labels[p] = label;
    }

    return out;
}

GeoConnectivity
geoPrimitiveConnectivity(const GeoDetail& detail)
{
    GeoConnectivity out;
    const std::vector<std::size_t> primitives = geoLiveOffsets(detail.primitiveMap());
    UnionFind sets = connectPoints(detail, primitives);
    std::vector<std::int32_t> rootLabel(detail.elementOffsetSize(eGeoAttribClassPoint), -1);

    out.labels.assign(detail.elementOffsetSize(eGeoAttribClassPrimitive), -1);
    for (std::size_t prim : primitives) {
        const GeoVertexRange range = detail.primitiveVertices(GeoOffset(prim));
        if (range.count <= 0) {
            out.labels[prim] = out.count++;
            continue;
        }
        std::int32_t& label = rootLabel[std::size_t(sets.find(std::int32_t(detail.vertexPoint(range.start))))];
        if (label < 0) {
            label = out.count++;
        }
        out.labels[prim] = label;
    }

    return out;
}

NATRON_NAMESPACE_EXIT
