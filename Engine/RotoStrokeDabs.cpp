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

#include "RotoStrokeDabs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>

#include "Engine/KnobTypes.h"
#include "Engine/RotoDrawableItem.h"

NATRON_NAMESPACE_ENTER

static inline double
hardnessGaussLookup(double f)
{
    // 2 hyperbolas + 1 parabola to approximate a gauss function
    if (f < -0.5) {
        f = -1. - f;

        return (2. * f * f);
    }

    if (f < 0.5) {
        return (1. - 2. * f * f);
    }
    f = 1. - f;

    return (2. * f * f);
}

void
getRenderDotParams(double alpha,
                   double brushSizePixel,
                   double brushHardness,
                   double brushSpacing,
                   double pressure,
                   bool pressureAffectsOpacity,
                   bool pressureAffectsSize,
                   bool pressureAffectsHardness,
                   double* internalDotRadius,
                   double* externalDotRadius,
                   double* spacing,
                   std::vector<std::pair<double, double>>* opacityStops)
{
    if (pressureAffectsSize) {
        brushSizePixel *= pressure;
    }
    if (pressureAffectsHardness) {
        brushHardness *= pressure;
    }
    if (pressureAffectsOpacity) {
        alpha *= pressure;
    }

    *internalDotRadius = std::max(brushSizePixel * brushHardness, 1.) / 2.;
    *externalDotRadius = std::max(brushSizePixel, 1.) / 2.;
    *spacing = *externalDotRadius * 2. * brushSpacing;

    opacityStops->clear();

    double exp = brushHardness != 1.0 ? 0.4 / (1.0 - brushHardness) : 0.;
    const int maxStops = 8;
    double incr = 1. / maxStops;

    if (brushHardness != 1.) {
        for (double d = 0; d <= 1.; d += incr) {
            double o = hardnessGaussLookup(std::pow(d, exp));
            opacityStops->push_back(std::make_pair(d, o * alpha));
        }
    }
}

namespace RotoStrokeDabs {

bool
readBrushParams(const RotoDrawableItem* item,
                double time,
                unsigned int mipmapLevel,
                BrushParams* params)
{
    if (!item || !item->isActivated(time)) {
        return false;
    }

    KnobDoublePtr brushSizeKnob = item->getBrushSizeKnob();
    double brushSize = brushSizeKnob->getValueAtTime(time);
    KnobDoublePtr brushSpacingKnob = item->getBrushSpacingKnob();
    double brushSpacing = brushSpacingKnob->getValueAtTime(time);
    if (brushSpacing == 0.) {
        return false;
    }

    brushSpacing = std::max(brushSpacing, 0.05);

    KnobDoublePtr brushHardnessKnob = item->getBrushHardnessKnob();
    double brushHardness = brushHardnessKnob->getValueAtTime(time);
    KnobDoublePtr visiblePortionKnob = item->getBrushVisiblePortionKnob();
    double writeOnStart = visiblePortionKnob->getValueAtTime(time, 0);
    double writeOnEnd = visiblePortionKnob->getValueAtTime(time, 1);
    if ((writeOnEnd - writeOnStart) <= 0.) {
        return false;
    }

    KnobBoolPtr pressureOpacityKnob = item->getPressureOpacityKnob();
    KnobBoolPtr pressureSizeKnob = item->getPressureSizeKnob();
    KnobBoolPtr pressureHardnessKnob = item->getPressureHardnessKnob();
    double brushSizePixel = brushSize;
    if (mipmapLevel != 0) {
        brushSizePixel = std::max(1., brushSizePixel / (1 << mipmapLevel));
    }

    params->brushSizePixel = brushSizePixel;
    params->brushSpacing = brushSpacing;
    params->brushHardness = brushHardness;
    params->writeOnStart = writeOnStart;
    params->writeOnEnd = writeOnEnd;
    params->pressureAffectsOpacity = pressureOpacityKnob->getValueAtTime(time);
    params->pressureAffectsSize = pressureSizeKnob->getValueAtTime(time);
    params->pressureAffectsHardness = pressureHardnessKnob->getValueAtTime(time);

    return true;
}

double
walk(const Strokes& strokes,
     double distToNext,
     const BrushParams& params,
     bool buildUp,
     double opacity,
     const Sink& sink)
{
    const double alpha = opacity;
    const double brushSizePixel = params.brushSizePixel;
    const double brushSpacing = params.brushSpacing;
    const double brushHardness = params.brushHardness;
    const double writeOnStart = params.writeOnStart;
    const double writeOnEnd = params.writeOnEnd;
    const bool pressureAffectsOpacity = params.pressureAffectsOpacity;
    const bool pressureAffectsSize = params.pressureAffectsSize;
    const bool pressureAffectsHardness = params.pressureAffectsHardness;

    for (Strokes::const_iterator strokeIt = strokes.begin(); strokeIt != strokes.end(); ++strokeIt) {
        int firstPoint = (int)std::floor((strokeIt->size() * writeOnStart));
        int endPoint = (int)std::ceil((strokeIt->size() * writeOnEnd));
        assert(firstPoint >= 0 && firstPoint < (int)strokeIt->size() && endPoint > firstPoint && endPoint <= (int)strokeIt->size());

        StrokePoints visiblePortion;
        StrokePoints::const_iterator startingIt = strokeIt->begin();
        StrokePoints::const_iterator endingIt = strokeIt->begin();
        std::advance(startingIt, firstPoint);
        std::advance(endingIt, endPoint);
        for (StrokePoints::const_iterator it = startingIt; it != endingIt; ++it) {
            visiblePortion.push_back(*it);
        }
        if (visiblePortion.empty()) {
            return distToNext;
        }

        StrokePoints::iterator it = visiblePortion.begin();

        if (visiblePortion.size() == 1) {
            Dab dab;
            double spacing;
            getRenderDotParams(alpha, brushSizePixel, brushHardness, brushSpacing, it->second, pressureAffectsOpacity, pressureAffectsSize, pressureAffectsHardness, &dab.internalRadius, &dab.externalRadius, &spacing, &dab.opacityStops);
            dab.center = it->first;
            dab.pressure = it->second;
            dab.buildUp = buildUp;
            dab.opacity = alpha;
            sink(dab);
            continue;
        }

        StrokePoints::iterator next = it;
        ++next;

        while (next != visiblePortion.end()) {
            // Spacing is a percentage of brushSize:
            // Spacing at 1 means no dot is overlapping another (so the spacing is in fact brushSize)
            // Spacing at 0 we do not render the stroke

            double dist = std::sqrt((next->first.x - it->first.x) * (next->first.x - it->first.x) + (next->first.y - it->first.y) * (next->first.y - it->first.y));

            while (distToNext <= dist) {
                double a = dist == 0. ? 0. : distToNext / dist;
                Point center = {
                    it->first.x * (1 - a) + next->first.x * a,
                    it->first.y * (1 - a) + next->first.y * a
                };
                double pressure = it->second * (1 - a) + next->second * a;

                Dab dab;
                double spacing;
                getRenderDotParams(alpha, brushSizePixel, brushHardness, brushSpacing, pressure, pressureAffectsOpacity, pressureAffectsSize, pressureAffectsHardness, &dab.internalRadius, &dab.externalRadius, &spacing, &dab.opacityStops);
                dab.center = center;
                dab.pressure = pressure;
                dab.buildUp = buildUp;
                dab.opacity = alpha;
                sink(dab);

                distToNext += spacing;
            }

            distToNext -= dist;
            ++next;
            ++it;
        }
    }

    return distToNext;
}

double
walk(const Strokes& strokes,
     double distToNext,
     const RotoDrawableItem* item,
     bool buildUp,
     double opacity,
     double time,
     unsigned int mipmapLevel,
     const Sink& sink)
{
    if (strokes.empty()) {
        return distToNext;
    }
    BrushParams params;
    if (!readBrushParams(item, time, mipmapLevel, &params)) {
        return distToNext;
    }

    return walk(strokes, distToNext, params, buildUp, opacity, sink);
}

} // namespace RotoStrokeDabs

NATRON_NAMESPACE_EXIT
