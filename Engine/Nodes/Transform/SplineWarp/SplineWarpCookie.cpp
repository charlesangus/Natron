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

#include "SplineWarpCookie.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

#include "Engine/Bezier.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoLayer.h"

NATRON_NAMESPACE_ENTER

float
SplineWarpCookieMask::at(int x,
                         int y) const
{
    if (!bounds.contains(x, y) || pixels.empty()) {
        return 0.f;
    }

    return pixels[(std::size_t)(y - bounds.y1) * bounds.width() + (x - bounds.x1)];
}

std::list<BezierPtr>
getSplineWarpCookieCurves(const SplineWarpContext& context,
                          double time,
                          bool invert)
{
    std::list<BezierPtr> curves;
    const std::list<BezierPtr> cutters = context.getCookieCutters();

    for (std::list<BezierPtr>::const_iterator it = cutters.begin(); it != cutters.end(); ++it) {
        if (!(*it)->isActivated(time) || (*it)->isDeactivatedRecursive()) {
            continue;
        }
        BezierPtr curve = *it;
        SplineWarpLinkPtr link = context.getLink(curve);
        if (link) {
            BezierPtr origin = invert ? link->getDestination() : link->getSource();
            if (origin) {
                curve = origin;
            }
        }
        if (std::find(curves.begin(), curves.end(), curve) == curves.end()) {
            curves.push_back(curve);
        }
    }

    return curves;
}

namespace {

// The box pads the feather by its distance, which shrinks it for a negative distance although the
// feather is drawn outward by the distance's magnitude either way; whole draws that out to the
// pixels the curve can actually reach.
bool
curvePixels(const Bezier& curve,
            double time,
            double scaleX,
            double scaleY,
            bool whole,
            RectI* pixels)
{
    RectD box = curve.getBoundingBox(time);
    if (box.isNull() || box.isInfinite()) {
        return false;
    }
    if (whole) {
        const double featherDistance = curve.getFeatherDistance(time);
        if (featherDistance < 0.) {
            const double pad = -2. * featherDistance;
            box.x1 -= pad;
            box.y1 -= pad;
            box.x2 += pad;
            box.y2 += pad;
        }
    }
    // A pixel of margin for the antialiased edge.
    *pixels = RectI((int)std::floor(box.x1 * scaleX) - 1, (int)std::floor(box.y1 * scaleY) - 1,
                    (int)std::ceil(box.x2 * scaleX) + 1, (int)std::ceil(box.y2 * scaleY) + 1);

    return true;
}

} // namespace

RectI
splineWarpCookieBounds(const std::list<BezierPtr>& curves,
                       double time,
                       double scaleX,
                       double scaleY)
{
    RectI bounds;

    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        RectI pixels;
        if (!curvePixels(**it, time, scaleX, scaleY, false, &pixels)) {
            continue;
        }
        if (bounds.isNull()) {
            bounds = pixels;
        } else {
            bounds.merge(pixels);
        }
    }

    return bounds;
}

SplineWarpCookieMask
renderSplineWarpCookieMask(const std::list<BezierPtr>& curves,
                           double time,
                           const RectI& window,
                           double scaleX,
                           double scaleY)
{
    SplineWarpCookieMask mask;

    if (window.isNull() || curves.empty()) {
        return mask;
    }
    try {
        mask.pixels.assign((std::size_t)window.width() * window.height(), 0.f);
    } catch (const std::bad_alloc&) {
        return SplineWarpCookieMask();
    }
    mask.bounds = window;
    const std::size_t stride = (std::size_t)window.width();
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        RectI pixels = window;
        if (curvePixels(**it, time, scaleX, scaleY, true, &pixels)) {
            pixels = pixels.intersect(window);
        }
        if (pixels.isNull()) {
            continue;
        }
        // The curves are in canonical coordinates at full scale; the scale takes them to pixels.
        const bool rendered = RotoContextPrivate::renderBezier(it->get(), time, time, time, 1., 0, scaleX, scaleY, pixels, [&](const RectI& tile, const float* alpha) {
            const std::size_t width = (std::size_t)tile.width();
            for (int y = tile.y1; y < tile.y2; ++y) {
                const float* src = alpha + (std::size_t)(y - tile.y1) * width;
                float* dst = &mask.pixels[(std::size_t)(y - window.y1) * stride + (std::size_t)(tile.x1 - window.x1)];
                for (std::size_t x = 0; x < width; ++x) {
                    dst[x] = src[x] + dst[x] * (1.f - src[x]);
                }
            }
        });
        if (!rendered) {
            return SplineWarpCookieMask();
        }
    }

    return mask;
}

NATRON_NAMESPACE_EXIT
