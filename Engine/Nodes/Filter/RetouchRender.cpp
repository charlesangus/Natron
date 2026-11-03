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
#include <cmath>
#include <limits>

#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"
#include "Engine/Nodes/Filter/RetouchAlgorithms.h"

NATRON_NAMESPACE_ENTER

namespace RetouchCore {
void
applyBrushPressure(BrushPressureEnum mode,
                   float pressure,
                   float minRadius,
                   BrushPoint* point)
{
    switch (mode) {
    case eBrushPressureSizeRelative:
        point->radius = std::max(minRadius, point->radius * pressure);
        break;
    case eBrushPressureHardnessAbsolute:
        point->hardness = std::max(kBrushHardnessMin, pressure);
        break;
    case eBrushPressureHardnessRelative:
        point->hardness = std::max(kBrushHardnessMin, point->hardness * pressure);
        break;
    case eBrushPressureOpacityAbsolute:
        point->density = std::max(kBrushDensityPressureMin, pressure);
        break;
    case eBrushPressureOpacityRelative:
        point->density = std::max(kBrushDensityPressureMin, point->density * pressure);
        break;
    case eBrushPressureOff:
        break;
    }
}

Region
brushBounds(const std::vector<BrushPoint>& points)
{
    Region r;
    if (points.empty()) {
        return r;
    }
    float x1 = points[0].x, y1 = points[0].y, x2 = x1, y2 = y1;
    for (const BrushPoint& p : points) {
        x1 = std::min(x1, p.x - p.radius);
        y1 = std::min(y1, p.y - p.radius);
        x2 = std::max(x2, p.x + p.radius);
        y2 = std::max(y2, p.y + p.radius);
    }
    r.x = (int)std::floor(x1);
    r.y = (int)std::floor(y1);
    r.width = (int)std::ceil(x2) - r.x;
    r.height = (int)std::ceil(y2) - r.y;

    return r;
}

namespace {
    inline float
    brushFalloff(float distance,
                 float radius,
                 float hardness,
                 float density)
    {
        if ((radius <= 0.f) || (distance >= radius)) {
            return 0.f;
        }
        const float u = distance / radius;
        if ((u <= hardness) || (hardness >= 1.f)) {
            return density;
        }

        return density * (1.f - u) / (1.f - hardness);
    }
} // anon namespace

void
rasterizeBrush(const std::vector<BrushPoint>& points,
               const Region& roi,
               std::vector<float>* mask)
{
    mask->assign((std::size_t)std::max(0, roi.width) * (std::size_t)std::max(0, roi.height), 0.f);
    if (points.empty() || (roi.width <= 0) || (roi.height <= 0)) {
        return;
    }
    const std::size_t w = (std::size_t)roi.width;
    // A single point is a round stamp; segments carry the parameters linearly between their ends.
    const std::size_t nSegments = points.size() > 1 ? points.size() - 1 : 1;
    for (std::size_t s = 0; s < nSegments; ++s) {
        const BrushPoint& a = points[s];
        const BrushPoint& b = points.size() > 1 ? points[s + 1] : points[s];
        const float maxRadius = std::max(a.radius, b.radius);
        const int px1 = std::max(roi.x, (int)std::floor(std::min(a.x, b.x) - maxRadius));
        const int py1 = std::max(roi.y, (int)std::floor(std::min(a.y, b.y) - maxRadius));
        const int px2 = std::min(roi.x + roi.width, (int)std::ceil(std::max(a.x, b.x) + maxRadius) + 1);
        const int py2 = std::min(roi.y + roi.height, (int)std::ceil(std::max(a.y, b.y) + maxRadius) + 1);
        const float ex = b.x - a.x;
        const float ey = b.y - a.y;
        const float len2 = ex * ex + ey * ey;
        for (int py = py1; py < py2; ++py) {
            float* row = &(*mask)[(std::size_t)(py - roi.y) * w];
            const float cy = (float)py + 0.5f;
            for (int px = px1; px < px2; ++px) {
                const float cx = (float)px + 0.5f;
                float t = 0.f;
                if (len2 > 0.f) {
                    t = std::clamp(((cx - a.x) * ex + (cy - a.y) * ey) / len2, 0.f, 1.f);
                }
                const float qx = a.x + t * ex - cx;
                const float qy = a.y + t * ey - cy;
                const float d = std::sqrt(qx * qx + qy * qy);
                const float radius = a.radius + t * (b.radius - a.radius);
                const float hardness = a.hardness + t * (b.hardness - a.hardness);
                const float density = a.density + t * (b.density - a.density);
                const float v = brushFalloff(d, radius, hardness, density);
                float& out = row[px - roi.x];
                out = std::max(out, v);
            }
        }
    }
} // rasterizeBrush

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

namespace {
    // The overlap of a and b, which may be empty.
    Region
    intersect(const Region& a,
              const Region& b)
    {
        Region r;
        r.x = std::max(a.x, b.x);
        r.y = std::max(a.y, b.y);
        r.width = std::max(0, std::min(a.x + a.width, b.x + b.width) - r.x);
        r.height = std::max(0, std::min(a.y + a.height, b.y + b.height) - r.y);

        return r;
    }

    bool
    isEmpty(const Region& r)
    {
        return (r.width <= 0) || (r.height <= 0);
    }

    // The bounds of a and b; an empty one adds nothing.
    Region
    unite(const Region& a,
          const Region& b)
    {
        if (isEmpty(a)) {
            return b;
        }
        if (isEmpty(b)) {
            return a;
        }
        Region r;
        r.x = std::min(a.x, b.x);
        r.y = std::min(a.y, b.y);
        r.width = std::max(a.x + a.width, b.x + b.width) - r.x;
        r.height = std::max(a.y + a.height, b.y + b.height) - r.y;

        return r;
    }

    Region
    grow(const Region& r,
         int by)
    {
        Region g = r;
        g.x -= by;
        g.y -= by;
        g.width += 2 * by;
        g.height += 2 * by;

        return g;
    }

    bool
    takesSource(AlgorithmEnum algorithm)
    {
        return (algorithm == eAlgorithmClone) || (algorithm == eAlgorithmHeal);
    }

    /*
     * retouchHeal() on one shape. A draft starts from the shape's warm start where that covers
     * the mask and stops after the draft's cycles; without a warm start it solves in full, as a
     * render that is not a draft always does, so a final render does not depend on the drafts.
     */
    void
    healShape(float* layer,
              const Region& layerRoi,
              const float* mask,
              const Region& maskRoi,
              int dx,
              int dy,
              int scale,
              const Shape& shape,
              const Params& params,
              const EdgeAware::ParallelFor& parallel)
    {
        HealWarmStart* warm = shape.warmStart.get();
        std::vector<float> guess;
        if (warm && (params.draftHealCycles > 0)) {
            std::lock_guard<std::mutex> lock(warm->mutex);
            const Region overlap = intersect(warm->region, maskRoi);
            if ((warm->renderScale == params.renderScale) && (warm->scale == scale) && !isEmpty(overlap)) {
                guess.assign((std::size_t)4 * maskRoi.width * maskRoi.height, std::numeric_limits<float>::quiet_NaN());
                for (int y = overlap.y; y < overlap.y + overlap.height; ++y) {
                    const float* from = &warm->difference[4 * ((std::size_t)(y - warm->region.y) * warm->region.width + (overlap.x - warm->region.x))];
                    std::copy(from, from + 4 * (std::size_t)overlap.width,
                              &guess[4 * ((std::size_t)(y - maskRoi.y) * maskRoi.width + (overlap.x - maskRoi.x))]);
                }
            }
        }
        const int cycles = guess.empty() ? params.maxHealIterations : std::min(params.draftHealCycles, params.maxHealIterations);
        std::vector<float> difference;
        retouchHeal(layer, layerRoi, mask, maskRoi, dx, dy, shape.settings.opacity, cycles, parallel, guess.empty() ? nullptr : guess.data(),
                    warm ? &difference : nullptr);
        if (warm) {
            std::lock_guard<std::mutex> lock(warm->mutex);
            warm->region = maskRoi;
            warm->renderScale = params.renderScale;
            warm->scale = scale;
            warm->difference.swap(difference);
        }
    }
} // anon namespace

Region
processRegion(const Region& window,
              const std::vector<Shape>& shapes,
              const Params& params,
              int fullWidth,
              int fullHeight)
{
    Region full;
    full.width = fullWidth;
    full.height = fullHeight;
    Region region = intersect(window, full);
    if (isEmpty(region)) {
        return region;
    }
    EdgeAware::DwtParams dwt;
    dwt.width = fullWidth;
    dwt.height = fullHeight;
    dwt.previewScale = (params.renderScale > 0.f) ? params.renderScale : 1.f;
    const int scales = std::min(std::max(0, std::min(params.numScales, kMaxScales)), EdgeAware::dwtMaxScale(dwt));
    // buildRegionMask() moves a shape's area by a pixel and grows a heal's by another.
    const int maskReach = 2;
    // The hat filters of scales 1 to scales reach 1 + 2 + ... + 2^(scales - 1) pixels, and so
    // does a crop edge's mirroring, which leaves the layers that far inside it unlike the full
    // image's. A shape reaching into that margin is taken whole, so that every shape processed
    // reads only layers as the full image has them.
    const int margin = maskReach + ((scales > 0) ? (1 << scales) - 1 : 0);
    std::vector<bool> taken(shapes.size(), false);
    bool grew = true;
    while (grew) {
        grew = false;
        const Region reach = grow(region, margin);
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const Shape& shape = shapes[i];
            if (taken[i] || !shapeScaleProcessed(shape.settings.scale, params.numScales)) {
                continue;
            }
            const Region area = grow(shape.maskRoi, maskReach);
            Region source = area;
            if (takesSource(shape.settings.algorithm)) {
                source.x -= (int)shape.dx;
                source.y -= (int)shape.dy;
            }
            if (isEmpty(intersect(area, reach)) && isEmpty(intersect(source, reach))) {
                continue;
            }
            taken[i] = true;
            region = unite(region, intersect(unite(area, source), full));
            grew = true;
        }
    }

    return intersect(grow(region, margin), full);
} // processRegion

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
    layerRoi.x = params.originX;
    layerRoi.y = params.originY;
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
            healShape(layer, layerRoi, mask.data(), maskRoi, dx, dy, scale, shape, params, parallel);
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
    dwt.capWidth = params.fullWidth;
    dwt.capHeight = params.fullHeight;
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
