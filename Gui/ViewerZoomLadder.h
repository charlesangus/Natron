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

#ifndef Gui_ViewerZoomLadder_h
#define Gui_ViewerZoomLadder_h

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

NATRON_NAMESPACE_ENTER

/// The viewer's zoom menu presets, in percent, in menu order.
inline constexpr int kViewerZoomPresetsPercent[] = {
    10, 25, 50, 75, 100, 125, 150, 200, 400, 800, 1600, 2400, 3200, 6400
};

namespace ViewerZoomLadderDetail {
constexpr int kPresetCount = static_cast<int>(std::size(kViewerZoomPresetsPercent));

// A level within this relative distance of a rung counts as on it, so that floating-point noise
// in the zoom factor never makes a step land on the rung it started from.
constexpr double kOnRungTolerance = 0.005;

/// Rung i of the ladder as a zoom factor: the presets, halving below the first and doubling above
/// the last.
inline double
rung(int i)
{
    if (i < 0) {
        return std::ldexp(kViewerZoomPresetsPercent[0] / 100., i);
    }
    if (i >= kPresetCount) {
        return std::ldexp(kViewerZoomPresetsPercent[kPresetCount - 1] / 100., i - (kPresetCount - 1));
    }

    return kViewerZoomPresetsPercent[i] / 100.;
}

inline bool
isOnRung(double factor,
         int i)
{
    const double r = rung(i);

    return std::fabs(r - factor) < kOnRungTolerance * r;
}
} // namespace ViewerZoomLadderDetail

/**
 * @brief The zoom factor `steps` rungs away from `currentFactor` on the preset ladder, clamped to
 * [minFactor, maxFactor]. Positive steps zoom in. A level between rungs takes the next rung in the
 * step's direction as its first step.
 **/
inline double
stepZoomPreset(double currentFactor,
               int steps,
               double minFactor,
               double maxFactor)
{
    using namespace ViewerZoomLadderDetail;

    if ((steps == 0) || !std::isfinite(currentFactor) || (currentFactor <= 0.)) {
        return currentFactor;
    }
    const double first = rung(0);
    const double last = rung(kPresetCount - 1);
    int target;
    if (steps > 0) {
        // Start on a rung safely below the current level, then climb to the first one above it.
        int i = -1;
        if (currentFactor < first) {
            i = static_cast<int>(std::floor(std::log2(currentFactor / first))) - 1;
        } else if (currentFactor > last) {
            i = kPresetCount - 1 + static_cast<int>(std::floor(std::log2(currentFactor / last))) - 1;
        }
        while (!(rung(i) > currentFactor && !isOnRung(currentFactor, i))) {
            ++i;
        }
        target = i + steps - 1;
    } else {
        int i = kPresetCount;
        if (currentFactor < first) {
            i = static_cast<int>(std::ceil(std::log2(currentFactor / first))) + 1;
        } else if (currentFactor > last) {
            i = kPresetCount - 1 + static_cast<int>(std::ceil(std::log2(currentFactor / last))) + 1;
        }
        while (!(rung(i) < currentFactor && !isOnRung(currentFactor, i))) {
            --i;
        }
        target = i + steps + 1;
    }

    return std::min(maxFactor, std::max(minFactor, rung(target)));
}

NATRON_NAMESPACE_EXIT

#endif // Gui_ViewerZoomLadder_h
