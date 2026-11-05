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

#ifndef NATRON_TESTS_SPLINEWARPTESTCURVES_H
#define NATRON_TESTS_SPLINEWARPTESTCURVES_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/Bezier.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/RotoItem.h"

NATRON_NAMESPACE_ENTER

namespace SplineWarpTestCurves {

/**
 * @brief The time every curve is keyed at.
 **/
inline constexpr double kTime = 1.;

inline SplineWarpContextPtr
contextOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
}

/**
 * @brief A Bezier through points, keyed at kTime: finished (closed) when `closed`, otherwise open.
 * A single point makes a one-point curve.
 **/
inline BezierPtr
makeCurve(const SplineWarpContextPtr& ctx,
          const std::vector<std::pair<double, double>>& points,
          bool closed = true)
{
    BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        bezier->addControlPoint(points[i].first, points[i].second, kTime);
    }
    if (closed) {
        bezier->setCurveFinished(true);
    }

    return bezier;
}

/**
 * @brief A quadrilateral at (x, y), scaled by s about its centre, then moved by (dx, dy).
 **/
inline BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx = 0.,
         double dy = 0.,
         double s = 1.)
{
    const double base[4][2] = { { x, y }, { x + size, y }, { x + size * 1.2, y + size }, { x - size * 0.1, y + size * 0.8 } };
    const double cx = x + size * 0.5;
    const double cy = y + size * 0.5;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 4; ++i) {
        points.push_back(std::make_pair(cx + s * (base[i][0] - cx) + dx, cy + s * (base[i][1] - cy) + dy));
    }

    return makeCurve(ctx, points);
}

inline BezierPtr
makeRectangle(const SplineWarpContextPtr& ctx,
              double x1,
              double y1,
              double x2,
              double y2)
{
    std::vector<std::pair<double, double>> points;
    points.push_back(std::make_pair(x1, y1));
    points.push_back(std::make_pair(x2, y1));
    points.push_back(std::make_pair(x2, y2));
    points.push_back(std::make_pair(x1, y2));

    return makeCurve(ctx, points);
}

/**
 * @brief A link moving a quadrilateral by (dx, dy) and scaling it by s: with s = 1 and no other
 * link, the warp is that translation everywhere.
 **/
inline SplineWarpLinkPtr
linkQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx,
         double dy,
         double s = 1.)
{
    BezierPtr source = makeQuad(ctx, x, y, size);
    BezierPtr destination = makeQuad(ctx, x, y, size, dx, dy, s);
    if (!source || !destination) {
        return SplineWarpLinkPtr();
    }

    return ctx->linkWithAlignment(source, destination, 0., false);
}

} // namespace SplineWarpTestCurves

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_SPLINEWARPTESTCURVES_H
