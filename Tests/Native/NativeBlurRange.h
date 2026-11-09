/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2021 The Natron developers
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

#ifndef NATRON_TESTS_NATIVEBLURRANGE_H
#define NATRON_TESTS_NATIVEBLURRANGE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <list>
#include <sstream>
#include <string>
#include <vector>

#include "RenderBothWays.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

// A multiply far above display range, so that a read of the wrong pixel or a bad weight is not lost
// in the sum.
constexpr double kBlurRangeBrightMultiply = 1000.;

// How far past the source's range a filter's interpolation may go, as a fraction of that range.
// Rifman, the sharpest filter, weighs a pixel's neighbours by -0.25 and the rest by 1.25 per axis
// at worst, half way between pixels, so -0.625 and 1.625 in two dimensions.
constexpr double kBlurRangeNoLobes = 0.;
constexpr double kBlurRangeLobes = 0.625;

struct BlurRangeFilter {
    const char* id;
    double lobes;
};

const BlurRangeFilter kBlurRangeFilters[] = {
    { "impulse", kBlurRangeNoLobes },
    { "bilinear", kBlurRangeNoLobes },
    { "cubic", kBlurRangeNoLobes },
    { "parzen", kBlurRangeNoLobes },
    { "notch", kBlurRangeNoLobes },
    { "keys", kBlurRangeLobes },
    { "simon", kBlurRangeLobes },
    { "rifman", kBlurRangeLobes },
    { "mitchell", kBlurRangeLobes },
};

// The values a blur of a source may reach. Zero is always in it: outside the source a copy reads black.
struct BlurRange {
    float low = 0.f;
    float high = 0.f;

    void include(const std::vector<float>& pixels)
    {
        for (const float v : pixels) {
            if (std::isfinite(v)) {
                low = std::min(low, v);
                high = std::max(high, v);
            }
        }
    }
};

// Empty when every value is finite and within range, widened by lobes; otherwise what was found.
// The small relative margin is the rounding of the float sum.
inline std::string
blurRangeViolation(const std::vector<float>& pixels,
                   const BlurRange& range,
                   double lobes)
{
    const double span = (double)range.high - (double)range.low;
    const double margin = lobes * span + 1e-4 * std::max(std::fabs((double)range.low), std::fabs((double)range.high));
    const double low = range.low - margin;
    const double high = range.high + margin;
    std::size_t bad = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const double v = pixels[i];
        if (!std::isfinite(pixels[i]) || (v < low) || (v > high)) {
            if (bad == 0) {
                first = i;
            }
            ++bad;
        }
    }
    if (bad == 0) {
        return std::string();
    }
    std::ostringstream out;
    out << bad << " of " << pixels.size() << " values outside [" << low << ", " << high << "], first " << pixels[first] << " at float " << first;

    return out.str();
}

// The colour plane of node over window (pixels at mipmapLevel), as RGBA from the bottom row.
inline bool
renderBlurRangePlane(const NodePtr& node,
                     double time,
                     unsigned int mipmapLevel,
                     const RectI& window,
                     RenderSchedulerModeEnum mode,
                     bool byPassCache,
                     std::vector<float>* pixels,
                     std::string* error)
{
    std::vector<RenderedPlane> planes;
    if (!renderNodePlanesInMode(node, time, ViewIdx(0), mipmapLevel, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), mode, byPassCache, &planes, error)) {
        return false;
    }
    for (const RenderedPlane& plane : planes) {
        if (plane.layer.isColorLayer() && (plane.channels.size() == 4)) {
            *pixels = plane.pixels;

            return true;
        }
    }
    *error = "the render has no RGBA plane";

    return false;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVEBLURRANGE_H
