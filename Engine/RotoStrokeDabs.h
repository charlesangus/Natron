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

#ifndef Engine_RotoStrokeDabs_h
#define Engine_RotoStrokeDabs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <utility>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The shape of one brush dab as the stroke renderer draws it: a disc of radius externalDotRadius, either flat at
 * alpha (opacityStops empty) or a radial ramp from internalDotRadius to externalDotRadius through opacityStops, each a
 * (position in [0, 1], opacity) pair. spacing is the distance to the next dab.
 **/
void getRenderDotParams(double alpha,
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
                        std::vector<std::pair<double, double>>* opacityStops);

/**
 * @brief Places the dabs of a paint stroke along its points, the way RotoPaint's stroke renderer draws them, without
 * drawing anything: each dab goes to a sink, which may rasterise it anywhere.
 **/
namespace RotoStrokeDabs {

typedef std::list<std::pair<Point, double>> StrokePoints;
typedef std::list<StrokePoints> Strokes;

/**
 * @brief A stroke item's brush knobs at one time and mipmap level. brushSizePixel is already scaled to the level and
 * brushSpacing is clamped to the smallest spacing the renderer uses.
 **/
struct BrushParams {
    double brushSizePixel = 0.;
    double brushSpacing = 0.;
    double brushHardness = 0.;
    double writeOnStart = 0.;
    double writeOnEnd = 1.;
    bool pressureAffectsOpacity = false;
    bool pressureAffectsSize = false;
    bool pressureAffectsHardness = false;
};

/**
 * @brief One dab: a disc at center, shaped as getRenderDotParams describes. opacity is the stroke's opacity before
 * pressure, the value a flat dab (opacityStops empty) is drawn at; pressure already scales the stops when the item
 * lets it. buildUp says whether dabs accumulate (over) or keep the brightest (lighten).
 **/
struct Dab {
    Point center;
    double internalRadius;
    double externalRadius;
    double pressure;
    std::vector<std::pair<double, double>> opacityStops;
    bool buildUp;
    double opacity;
};

typedef std::function<void(const Dab& dab)> Sink;

/**
 * @brief Reads item's brush knobs at time. Returns false when the item draws nothing at time: no item, not
 * activated, zero spacing or an empty visible portion.
 **/
bool readBrushParams(const RotoDrawableItem* item, double time, unsigned int mipmapLevel, BrushParams* params);

/**
 * @brief Hands sink every dab of strokes, in drawing order. Points are (position, pressure) in the pixels of the
 * level the params were read at. distToNext is the distance along the first stroke to its first dab, carried over
 * from a previous walk of the same stroke; the return value is the distance to carry into the next one.
 **/
double walk(const Strokes& strokes, double distToNext, const BrushParams& params, bool buildUp, double opacity, const Sink& sink);

/**
 * @brief readBrushParams then walk. Returns distToNext unchanged when strokes is empty or the item draws nothing.
 **/
double walk(const Strokes& strokes,
            double distToNext,
            const RotoDrawableItem* item,
            bool buildUp,
            double opacity,
            double time,
            unsigned int mipmapLevel,
            const Sink& sink);

} // namespace RotoStrokeDabs

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoStrokeDabs_h
