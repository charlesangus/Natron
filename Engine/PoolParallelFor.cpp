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

#include "PoolParallelFor.h"

#include <algorithm>
#include <vector>

#include "Engine/AppManager.h"

NATRON_NAMESPACE_ENTER

void
forEachRowBand(const RectI& rect,
               qint64 minArea,
               const std::function<void(int, int)>& fn)
{
    const int width = rect.x2 - rect.x1;
    const int height = rect.y2 - rect.y1;
    int nBands = 1;

    if ((width > 0) && (height > 0) && ((qint64)width * height >= minArea) && appPTR) {
        nBands = std::min(height, appPTR->getNCPUsAvailableForEffect());
    }
    if (nBands <= 1) {
        fn(rect.y1, rect.y2);

        return;
    }

    const int rowsPerBand = (height + nBands - 1) / nBands;
    std::vector<RectI> bands;
    for (int y = rect.y1; y < rect.y2; y += rowsPerBand) {
        bands.push_back(RectI(rect.x1, y, rect.x2, std::min(y + rowsPerBand, rect.y2)));
    }
    parallelForOnGlobalPool((int)bands.size(), nBands, [&](int i) {
        fn(bands[i].y1, bands[i].y2);
    });
}

NATRON_NAMESPACE_EXIT
