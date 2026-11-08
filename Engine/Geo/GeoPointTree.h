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

#ifndef Engine_Geo_GeoPointTree_h
#define Engine_Geo_GeoPointTree_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A k-d tree over the live points of a detail, answering by dense point number. Distances
 * are computed in double from the stored positions; equal distances resolve to the lower point
 * number, so results match a brute-force scan exactly. Immutable once built and safe to query
 * from many threads.
 **/
class GeoPointTree {
public:
    explicit GeoPointTree(const GeoDetail& detail);

    /**
     * @brief The tree for detail, shared through a small cache keyed by the detail's content
     * hash, so a detail re-used across renders is indexed once.
     **/
    static std::shared_ptr<const GeoPointTree> forDetail(const GeoDetail& detail);

    std::size_t size() const
    {
        return _points.size();
    }

    /** @brief The nearest point number within maxDistance (inclusive), or -1. */
    GeoIndex nearest(const std::array<double, 3>& position,
                     double maxDistance = std::numeric_limits<double>::infinity()) const;

    /**
     * @brief Point numbers within maxDistance (inclusive), nearest first, into a cleared out.
     * A positive maxCount keeps only that many.
     **/
    void within(const std::array<double, 3>& position, double maxDistance, std::int64_t maxCount,
                std::vector<GeoIndex>& out) const;

private:
    struct Node {
        std::array<double, 3> lo;
        std::array<double, 3> hi;
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        std::int32_t left = -1;
        std::int32_t right = -1;
    };

    struct Point {
        std::array<double, 3> p;
        GeoIndex index;
    };

    std::int32_t build(std::uint32_t begin, std::uint32_t end);
    static double boxDistance2(const Node& node, const std::array<double, 3>& q);

    std::vector<Point> _points;
    std::vector<Node> _nodes;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoPointTree_h
