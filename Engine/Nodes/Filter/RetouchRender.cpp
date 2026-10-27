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

#include "RetouchRender.h"

#include <algorithm>

#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"
#include "Engine/Nodes/Filter/RetouchAlgorithms.h"

NATRON_NAMESPACE_ENTER

namespace RetouchCore {
int
returnLayerForOutput(OutputEnum output,
                     int numScales,
                     int currentScale)
{
    switch (output) {
    case eOutputCurrentScale:
        return std::max(0, std::min(currentScale, numScales + 1));
    case eOutputResidual:
        return numScales + 1;
    case eOutputResult:
    default:
        return 0;
    }
}

bool
shapeScaleProcessed(int shapeScale,
                    int numScales)
{
    return shapeScale >= 0 && shapeScale <= numScales + 1;
}

void
processLayer(float* layer,
             int width,
             int height,
             int scale,
             int scales,
             int returnLayer,
             int mergeFromScale,
             const Params& params,
             const std::vector<Shape>& shapes,
             const EdgeAware::ParallelFor& parallel)
{
    if ((mergeFromScale == 0) && (returnLayer > 0) && (scale != returnLayer) && (scale != 0)) {
        return;
    }
    if (scale > scales + 1) {
        return;
    }
    if ((scales < params.numScales) && (returnLayer == 0) && (scale == scales + 1)) {
        scale = params.numScales + 1;
    }

    Region layerRoi;
    layerRoi.width = width;
    layerRoi.height = height;
    std::vector<float> mask;
    for (const Shape& shape : shapes) {
        const ShapeSettings& s = shape.settings;
        if (s.scale != scale) {
            continue;
        }
        int dx = 0;
        int dy = 0;
        if ((s.algorithm != eAlgorithmBlur) && (s.algorithm != eAlgorithmFill)) {
            dx = (int)shape.dx;
            dy = (int)shape.dy;
        }
        Region maskRoi;
        if (!buildRegionMask(shape, layerRoi, dx, dy, &mask, &maskRoi)) {
            continue;
        }
        const bool apply = (dx != 0 || dy != 0 || s.algorithm == eAlgorithmBlur || s.algorithm == eAlgorithmFill)
            && maskRoi.width > 2 && maskRoi.height > 2;
        if (!apply) {
            continue;
        }
        switch (s.algorithm) {
        case eAlgorithmClone:
            retouchClone(layer, layerRoi, mask.data(), maskRoi, dx, dy, s.opacity);
            break;
        case eAlgorithmHeal:
            retouchHeal(layer, layerRoi, mask.data(), maskRoi, dx, dy, s.opacity, params.maxHealIterations, parallel);
            break;
        case eAlgorithmBlur:
            retouchBlur(layer, layerRoi, mask.data(), maskRoi, s.opacity, s.blurType, s.blurRadius, params.renderScale, parallel);
            break;
        case eAlgorithmFill:
            retouchFill(layer, layerRoi, mask.data(), maskRoi, s.opacity, s.fillMode, s.fillColor, s.fillBrightness);
            break;
        }
    }
} // processLayer

void
process(float* image,
        int width,
        int height,
        const Params& params,
        const std::vector<Shape>& shapes,
        const EdgeAware::ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    EdgeAware::DwtParams dwt;
    dwt.width = width;
    dwt.height = height;
    dwt.scales = std::max(0, std::min(params.numScales, kMaxScales));
    dwt.returnLayer = params.returnLayer;
    dwt.mergeFromScale = params.mergeFromScale;
    dwt.previewScale = params.renderScale;

    const EdgeAware::DwtLayerFn layerFn = [&](float* layer, const EdgeAware::DwtParams& p, int scale) {
        processLayer(layer, p.width, p.height, scale, p.scales, p.returnLayer, p.mergeFromScale, params, shapes, parallel);
    };
    EdgeAware::dwtDecompose(image, dwt, layerFn, parallel);

    if ((dwt.returnLayer > 0) && (dwt.returnLayer < dwt.scales + 1)) {
        adjustLevels(image, width, height, params.previewLevels, parallel);
    }
}
} // namespace RetouchCore

NATRON_NAMESPACE_EXIT
