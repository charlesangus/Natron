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

#ifndef Gui_NodeGraphStacking_h
#define Gui_NodeGraphStacking_h

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

NATRON_NAMESPACE_ENTER

// Nodes stack by z rather than by their place among their siblings: Qt moves a sibling with a
// linear walk over all of them, while a new z only marks the siblings for one sort before the
// next paint. A node raised later gets a higher serial, and its z is its base depth plus the
// serial's offset, which stays below the next whole depth. Serial 0 is a node never raised; such
// nodes tie and stack by creation order, below every raised node of their depth.
namespace NodeGraphStacking {

const std::uint64_t kSerialLimit = (std::uint64_t(1) << 20) - 1;

inline double
offsetOf(std::uint64_t serial)
{
    return (double)serial / (double)(kSerialLimit + 1);
}

// Whether the nodes holding serials, in order, are already the last ones raised, each right
// above the one before it, lastSerial being the highest serial handed out.
inline bool
areTopmost(const std::vector<std::uint64_t>& serials,
           std::uint64_t lastSerial)
{
    if (serials.empty()) {
        return true;
    }
    if ((serials.front() == 0) || (serials.back() != lastSerial)) {
        return false;
    }
    for (std::size_t i = 1; i < serials.size(); ++i) {
        if (serials[i] != serials[i - 1] + 1) {
            return false;
        }
    }

    return true;
}

// Serials in the same order as the given ones with the raised nodes numbered 1, 2, 3 and so on,
// so the stacking is unchanged and the range above them can be handed out again. Nodes never
// raised keep 0.
inline std::vector<std::uint64_t>
compacted(const std::vector<std::uint64_t>& serials)
{
    std::vector<std::size_t> order(serials.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_sort(order.begin(), order.end(), [&serials](std::size_t a, std::size_t b) {
        return serials[a] < serials[b];
    });
    std::vector<std::uint64_t> ret(serials.size(), 0);
    std::uint64_t next = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (serials[order[i]] != 0) {
            ret[order[i]] = ++next;
        }
    }

    return ret;
}

} // namespace NodeGraphStacking

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphStacking_h
