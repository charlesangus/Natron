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

#ifndef Gui_NearestMemberLines_h
#define Gui_NearestMemberLines_h

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <QtCore/QPointF>

NATRON_NAMESPACE_ENTER

/*
 * The lines that join the members of one set of linked nodes, given each member's centre: the shortest
 * tree of lines that joins them all. Every member's line to its nearest other member is in it, and a set
 * of n members draws n - 1 lines, where a member spaced as far from two others (as in a row) could
 * otherwise leave the set drawn as unconnected pairs. Each line is (i, j) with i < j, the lines sorted.
 * Ties go to the member with the lower index, so callers that order the members the same way get the
 * same lines. Costs O(n^2).
 */
inline std::vector<std::pair<std::size_t, std::size_t>>
nearestMemberLines(const std::vector<QPointF>& centres)
{
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    const std::size_t n = centres.size();

    if (n < 2) {
        return lines;
    }
    lines.reserve(n - 1);

    // Prim's algorithm: grow the tree from member 0, each step joining the member nearest to it.
    std::vector<bool> inTree(n, false);
    std::vector<double> distance(n, 0.);
    std::vector<std::size_t> nearest(n, 0);
    const auto squaredDistance = [&centres](std::size_t a, std::size_t b) {
        const double dx = centres[a].x() - centres[b].x();
        const double dy = centres[a].y() - centres[b].y();

        return dx * dx + dy * dy;
    };
    inTree[0] = true;
    for (std::size_t i = 1; i < n; ++i) {
        distance[i] = squaredDistance(0, i);
    }
    for (std::size_t step = 1; step < n; ++step) {
        std::size_t next = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (!inTree[i] && ((next == n) || (distance[i] < distance[next]))) {
                next = i;
            }
        }
        inTree[next] = true;
        lines.push_back(std::make_pair(std::min(next, nearest[next]), std::max(next, nearest[next])));
        for (std::size_t i = 0; i < n; ++i) {
            if (inTree[i]) {
                continue;
            }
            const double d = squaredDistance(next, i);
            if ((d < distance[i]) || ((d == distance[i]) && (next < nearest[i]))) {
                distance[i] = d;
                nearest[i] = next;
            }
        }
    }
    std::sort(lines.begin(), lines.end());

    return lines;
}

NATRON_NAMESPACE_EXIT

#endif // Gui_NearestMemberLines_h
