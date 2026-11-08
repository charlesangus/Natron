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

#include "GeoPointTree.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace {

constexpr std::uint32_t kLeafSize = 8;
constexpr std::size_t kCachedTrees = 4;

inline double
distance2(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

inline bool
closer(double d2, GeoIndex index, double bestD2, GeoIndex bestIndex)
{
    return d2 < bestD2 || (d2 == bestD2 && index < bestIndex);
}

} // namespace

GeoPointTree::GeoPointTree(const GeoDetail& detail)
{
    _points.reserve(std::size_t(detail.pointCount()));
    detail.pointMap().forEachLive([&](GeoIndex index, GeoOffset offset) {
        _points.push_back(Point { detail.pointPosition(offset), index });
    });
    if (!_points.empty()) {
        _nodes.reserve(2 * (_points.size() / kLeafSize + 1));
        build(0, std::uint32_t(_points.size()));
    }
}

std::int32_t
GeoPointTree::build(std::uint32_t begin, std::uint32_t end)
{
    Node node;
    node.begin = begin;
    node.end = end;
    node.lo = _points[begin].p;
    node.hi = _points[begin].p;
    for (std::uint32_t k = begin + 1; k < end; ++k) {
        for (int c = 0; c < 3; ++c) {
            node.lo[c] = std::min(node.lo[c], _points[k].p[c]);
            node.hi[c] = std::max(node.hi[c], _points[k].p[c]);
        }
    }
    const std::int32_t self = std::int32_t(_nodes.size());
    _nodes.push_back(node);
    if (end - begin <= kLeafSize) {
        return self;
    }
    int axis = 0;
    for (int c = 1; c < 3; ++c) {
        if (node.hi[c] - node.lo[c] > node.hi[axis] - node.lo[axis]) {
            axis = c;
        }
    }
    const std::uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(_points.begin() + begin, _points.begin() + mid, _points.begin() + end,
                     [axis](const Point& a, const Point& b) {
                         return a.p[axis] < b.p[axis] || (a.p[axis] == b.p[axis] && a.index < b.index);
                     });
    const std::int32_t left = build(begin, mid);
    const std::int32_t right = build(mid, end);
    _nodes[std::size_t(self)].left = left;
    _nodes[std::size_t(self)].right = right;
    return self;
}

double
GeoPointTree::boxDistance2(const Node& node, const std::array<double, 3>& q)
{
    double d2 = 0.;
    for (int c = 0; c < 3; ++c) {
        double d = 0.;
        if (q[c] < node.lo[c]) {
            d = node.lo[c] - q[c];
        } else if (q[c] > node.hi[c]) {
            d = q[c] - node.hi[c];
        }
        d2 += d * d;
    }
    return d2;
}

GeoIndex
GeoPointTree::nearest(const std::array<double, 3>& position, double maxDistance) const
{
    if (_nodes.empty() || !(maxDistance >= 0.)) {
        return -1;
    }
    double bestD2 = maxDistance * maxDistance;
    GeoIndex best = -1;
    std::int32_t stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = _nodes[std::size_t(stack[--top])];
        // Equal distances must still be visited: a lower point number may sit on the boundary.
        if (boxDistance2(node, position) > bestD2) {
            continue;
        }
        if (node.left < 0) {
            for (std::uint32_t k = node.begin; k < node.end; ++k) {
                const double d2 = distance2(_points[k].p, position);
                if (best < 0 ? d2 <= bestD2 : closer(d2, _points[k].index, bestD2, best)) {
                    bestD2 = d2;
                    best = _points[k].index;
                }
            }
            continue;
        }
        const Node& l = _nodes[std::size_t(node.left)];
        const Node& r = _nodes[std::size_t(node.right)];
        const bool leftFirst = boxDistance2(l, position) <= boxDistance2(r, position);
        if (top + 2 > 128) {
            throw std::runtime_error("GeoPointTree: tree too deep");
        }
        stack[top++] = leftFirst ? node.right : node.left;
        stack[top++] = leftFirst ? node.left : node.right;
    }
    return best;
}

void
GeoPointTree::within(const std::array<double, 3>& position, double maxDistance, std::int64_t maxCount,
                     std::vector<GeoIndex>& out) const
{
    out.clear();
    if (_nodes.empty() || !(maxDistance >= 0.)) {
        return;
    }
    const double maxD2 = maxDistance * maxDistance;
    std::vector<std::pair<double, GeoIndex>> hits;
    std::int32_t stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = _nodes[std::size_t(stack[--top])];
        if (boxDistance2(node, position) > maxD2) {
            continue;
        }
        if (node.left < 0) {
            for (std::uint32_t k = node.begin; k < node.end; ++k) {
                const double d2 = distance2(_points[k].p, position);
                if (d2 <= maxD2) {
                    hits.emplace_back(d2, _points[k].index);
                }
            }
            continue;
        }
        if (top + 2 > 128) {
            throw std::runtime_error("GeoPointTree: tree too deep");
        }
        stack[top++] = node.left;
        stack[top++] = node.right;
    }
    const std::size_t keep = maxCount > 0 ? std::min(hits.size(), std::size_t(maxCount)) : hits.size();
    std::partial_sort(hits.begin(), hits.begin() + std::ptrdiff_t(keep), hits.end());
    out.reserve(keep);
    for (std::size_t k = 0; k < keep; ++k) {
        out.push_back(hits[k].second);
    }
}

std::shared_ptr<const GeoPointTree>
GeoPointTree::forDetail(const GeoDetail& detail)
{
    static std::mutex mutex;
    static std::vector<std::pair<U64, std::shared_ptr<const GeoPointTree>>> cache;

    const U64 hash = detail.contentHash();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t k = 0; k < cache.size(); ++k) {
            if (cache[k].first == hash) {
                std::shared_ptr<const GeoPointTree> tree = cache[k].second;
                std::rotate(cache.begin(), cache.begin() + std::ptrdiff_t(k), cache.begin() + std::ptrdiff_t(k) + 1);
                return tree;
            }
        }
    }
    std::shared_ptr<const GeoPointTree> tree = std::make_shared<GeoPointTree>(detail);
    std::lock_guard<std::mutex> lock(mutex);
    cache.insert(cache.begin(), { hash, tree });
    if (cache.size() > kCachedTrees) {
        cache.pop_back();
    }
    return tree;
}

NATRON_NAMESPACE_EXIT
