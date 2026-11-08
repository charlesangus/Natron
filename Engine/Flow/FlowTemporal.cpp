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

#include "FlowTemporal.h"

#include <algorithm>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace {
// Keeps a pixel no field is confident about at its own vector, rather than dividing by zero.
const float kMinOwnWeight = 1e-3f;
} // anonymous namespace

void
flowSmoothTemporal(const FlowField& current,
                   const FlowTemporalNeighbour* neighbours,
                   int neighbourCount,
                   float strength,
                   const FlowContext& context,
                   FlowField& out)
{
    out = current;
    if (strength <= 0.f) {
        return;
    }
    std::vector<FlowField> carried(neighbourCount);
    for (int n = 0; n < neighbourCount; ++n) {
        flowWarpField(*neighbours[n].field, *neighbours[n].through, context, carried[n]);
    }
    context.parallel(current.height, [&](int y) {
        for (int x = 0; x < current.width; ++x) {
            const float own = std::max(current.confidence.at(x, y), kMinOwnWeight);
            float sumWeight = own;
            float sumU = own * current.u.at(x, y);
            float sumV = own * current.v.at(x, y);
            for (int n = 0; n < neighbourCount; ++n) {
                const float weight = strength * carried[n].confidence.at(x, y) * neighbours[n].through->confidence.at(x, y);
                sumWeight += weight;
                sumU += weight * carried[n].u.at(x, y);
                sumV += weight * carried[n].v.at(x, y);
            }
            out.u.at(x, y) = sumU / sumWeight;
            out.v.at(x, y) = sumV / sumWeight;
        }
    });
}

NATRON_NAMESPACE_EXIT
