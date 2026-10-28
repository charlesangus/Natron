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

#include <cairo/cairo.h>

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
                          double time)
{
    std::list<BezierPtr> curves;
    const std::list<BezierPtr> cutters = context.getCookieCutters();

    for (std::list<BezierPtr>::const_iterator it = cutters.begin(); it != cutters.end(); ++it) {
        if (!(*it)->isActivated(time) || (*it)->isDeactivatedRecursive()) {
            continue;
        }
        BezierPtr curve = *it;
        SplineWarpLinkPtr link = context.getLink(curve);
        if (link && link->getSource()) {
            curve = link->getSource();
        }
        if (std::find(curves.begin(), curves.end(), curve) == curves.end()) {
            curves.push_back(curve);
        }
    }

    return curves;
}

RectI
splineWarpCookieBounds(const std::list<BezierPtr>& curves,
                       double time,
                       double scaleX,
                       double scaleY)
{
    RectI bounds;

    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        const RectD box = (*it)->getBoundingBox(time);
        if (box.isNull() || box.isInfinite()) {
            continue;
        }
        // A pixel of margin for the antialiased edge.
        const RectI pixels((int)std::floor(box.x1 * scaleX) - 1, (int)std::floor(box.y1 * scaleY) - 1,
                           (int)std::ceil(box.x2 * scaleX) + 1, (int)std::ceil(box.y2 * scaleY) + 1);
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
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, window.width(), window.height());
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);

        return mask;
    }
    cairo_surface_set_device_offset(surface, -window.x1, -window.y1);
    cairo_t* cr = cairo_create(surface);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    // As Roto renders a shape: an antialiased fill leaves seams where the inner polygon meets the
    // feather's mesh, so the edge's softness comes from the feather.
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    // The curves are drawn in canonical coordinates at full scale; the matrix takes them to pixels.
    cairo_scale(cr, scaleX, scaleY);
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        RotoContextPrivate::renderBezier(cr, it->get(), 1., time, time, time, 1., 0);
    }
    cairo_surface_flush(surface);

    mask.bounds = window;
    mask.pixels.assign((std::size_t)window.width() * window.height(), 0.f);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    if (data) {
        for (int y = 0; y < window.height(); ++y) {
            const unsigned char* row = data + (std::size_t)y * stride;
            float* dst = &mask.pixels[(std::size_t)y * window.width()];
            for (int x = 0; x < window.width(); ++x) {
                dst[x] = row[x] * (1.f / 255.f);
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return mask;
}

NATRON_NAMESPACE_EXIT
