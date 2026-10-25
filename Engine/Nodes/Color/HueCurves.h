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

#ifndef Engine_Nodes_Color_HueCurves_h
#define Engine_Nodes_Color_HueCurves_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

/**
 * @brief Hue helpers shared by HSVTool, HueCorrect and HueKeyer, with hue in turns (0 to 1, red
 * at 0). Each statement rounds once, as hue.slang does, so the CPU and the GPU agree to float
 * rounding.
 *
 * HueCorrect's curves are periodic over 0 to 6 with red at 1, as in OpenFX: a curve is sampled
 * into a table of kLutSize values over that domain and read back linearly.
 **/
namespace HueCurves {
constexpr int kLutIntervals = 1024;
constexpr int kLutSize = kLutIntervals + 1;
// Table entries per unit of curve position.
constexpr float kLutScale = 170.66667175292969f;

static_assert((float)kLutIntervals / 6.f == kLutScale, "kLutScale is kLutIntervals / 6 in float");

// The curve position of a hue: red is at 1 and the domain is 0 to 6.
inline float
curvePosition(float hue)
{
    const float p = hue * 6.f;
    float x = p + 1.f;

    if (x > 6.f) {
        x = x - 6.f;
    }

    return x;
}

/**
 * @brief Fills lut[0..kLutSize) with sample(position) over the curve domain 0 to 6.
 **/
template <typename SAMPLE>
void
sampleLut(SAMPLE&& sample,
          float* lut)
{
    for (int i = 0; i < kLutSize; ++i) {
        lut[i] = (float)sample(6. * (double)i / (double)kLutIntervals);
    }
}

// The table linearly interpolated at curve position x, in 0 to 6.
inline float
lutAt(const float* lut,
      float x)
{
    const float t = x * kLutScale;
    const float fl = std::floor(t);

    if (!(fl >= 0.f)) {
        return lut[0];
    }
    if (fl >= (float)kLutIntervals) {
        return lut[kLutIntervals];
    }
    const int i = (int)fl;
    const float f = t - fl;
    const float a = lut[i];
    const float b = lut[i + 1];
    const float d = b - a;
    const float e = d * f;

    return a + e;
}

// x wrapped into 0 to 1.
inline float
wrapTurn(float x)
{
    const float fl = std::floor(x);

    return x - fl;
}

// x wrapped into -0.5 to 0.5.
inline float
signedTurn(float x)
{
    const float t = x + 0.5f;
    const float fl = std::floor(t);
    const float u = t - fl;

    return u - 0.5f;
}

// Whether hue h lies on the arc from h0 to h1, going up and wrapping past 1 when h1 < h0.
inline bool
withinArc(float h,
          float h0,
          float h1)
{
    return ((h1 < h0) && ((h <= h1) || (h0 <= h))) || ((h0 <= h) && (h <= h1));
}

// 0 at h0 rising to 1 at h1 along the arc from h0 to h1, and 0 off the arc.
inline float
arcRise(float h,
        float h0,
        float h1)
{
    if (!withinArc(h, h0, h1)) {
        return 0.f;
    }
    if (h1 == h0) {
        return 1.f;
    }
    if (h1 < h0) {
        h1 = h1 + 1.f;
        if (h < h0) {
            h = h + 1.f;
        }
    }
    const float n = h - h0;
    const float d = h1 - h0;

    return n / d;
}

// 1 at h0 falling to 0 at h1 along the arc from h0 to h1, and 0 off the arc.
inline float
arcFall(float h,
        float h0,
        float h1)
{
    if (!withinArc(h, h0, h1)) {
        return 0.f;
    }
    if (h1 == h0) {
        return 1.f;
    }
    if (h1 < h0) {
        h1 = h1 + 1.f;
        if (h < h0) {
            h = h + 1.f;
        }
    }
    const float n = h1 - h;
    const float d = h1 - h0;

    return n / d;
}

// 0 below a, rising to 1 from a to b, 1 from b to c, falling to 0 from c to d, 0 above d. The
// points are in order; an equal pair makes a hard edge.
inline float
trapezoid(float x,
          float a,
          float b,
          float c,
          float d)
{
    if (!(x >= a)) {
        return 0.f;
    }
    if (x < b) {
        const float n = x - a;
        const float w = b - a;

        return n / w;
    }
    if (x <= c) {
        return 1.f;
    }
    if (x <= d) {
        const float n = d - x;
        const float w = d - c;

        return n / w;
    }

    return 0.f;
}

/**
 * @brief The trapezoid of hue h whose points are low, then low + rise, low + full and
 * low + fall going up the hue circle, rise <= full <= fall. A trapezoid wider than the circle
 * overlaps itself, and the larger value wins.
 **/
inline float
hueTrapezoid(float h,
             float low,
             float rise,
             float full,
             float fall)
{
    const float d = h - low;
    float t = wrapTurn(d);
    float k = trapezoid(t, 0.f, rise, full, fall);

    for (int turn = 0; turn < 3; ++turn) {
        t = t + 1.f;
        if (!(t <= fall)) {
            break;
        }
        k = (std::max)(k, trapezoid(t, 0.f, rise, full, fall));
    }

    return k;
}
} // namespace HueCurves

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_HueCurves_h
