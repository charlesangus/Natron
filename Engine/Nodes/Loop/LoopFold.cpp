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

#include "LoopFold.h"

#include <algorithm>
#include <limits>

NATRON_NAMESPACE_ENTER

namespace LoopFold {

float
initialValue(OperationEnum operation)
{
    switch (operation) {
    case eOperationMin:
        return std::numeric_limits<float>::infinity();
    case eOperationMax:
        return -std::numeric_limits<float>::infinity();
    case eOperationMean:
    case eOperationSum:
    case eOperationOver:
    case eOperationUnder:
        break;
    }

    return 0.f;
}

void
foldRow(OperationEnum operation,
        const float* src,
        int srcComps,
        int srcX1,
        int srcX2,
        float* acc,
        int nComps,
        int alphaIndex,
        int x1,
        int x2)
{
    const bool srcHasAlpha = (alphaIndex >= 0) && (alphaIndex < srcComps);
    const bool accHasAlpha = (alphaIndex >= 0) && (alphaIndex < nComps);

    for (int x = x1; x < x2; ++x) {
        const float* s = (src && (x >= srcX1) && (x < srcX2)) ? src + (std::ptrdiff_t)(x - srcX1) * srcComps : 0;
        float* a = acc + (std::ptrdiff_t)(x - x1) * nComps;
        const float srcAlpha = (s && srcHasAlpha) ? s[alphaIndex] : 0.f;
        // Read before the alpha channel itself is folded.
        const float accAlpha = accHasAlpha ? a[alphaIndex] : 0.f;
        for (int c = 0; c < nComps; ++c) {
            const float value = (s && (c < srcComps)) ? s[c] : 0.f;
            switch (operation) {
            case eOperationMean:
            case eOperationSum:
                a[c] = a[c] + value;
                break;
            case eOperationMin:
                a[c] = std::min(a[c], value);
                break;
            case eOperationMax:
                a[c] = std::max(a[c], value);
                break;
            case eOperationOver:
                a[c] = value + a[c] * (1.f - srcAlpha);
                break;
            case eOperationUnder:
                a[c] = a[c] + value * (1.f - accAlpha);
                break;
            }
        }
    }
}

void
finishRow(OperationEnum operation,
          int count,
          float* acc,
          std::size_t n)
{
    if ((operation != eOperationMean) || (count <= 1)) {
        return;
    }
    const float divisor = (float)count;
    for (std::size_t i = 0; i < n; ++i) {
        acc[i] = acc[i] / divisor;
    }
}

} // namespace LoopFold

NATRON_NAMESPACE_EXIT
