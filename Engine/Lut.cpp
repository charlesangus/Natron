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

#include "Lut.h"

#include <algorithm> // min, max
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace Color {
// r,g,b values are linear values from 0 to 1
// h = [0,OFXS_HUE_CIRCLE], s = [0,1], v = [0,1]
//		if s == 0, then h = 0 (undefined)
// Reference:
// "Color gamut transform pairs", Alvy Ray Smith, Proceeding SIGGRAPH '78
// https://doi.org/10.1145/800248.807361
// http://www.icst.pku.edu.cn/F/course/ImageProcessing/2018/resource/Color78.pdf
void
rgb_to_hsv( float r,
            float g,
            float b,
            float *h,
            float *s,
            float *v )
{
    float min = (std::min)((std::min)(r, g), b);
    float max = (std::max)((std::max)(r, g), b);

    *v = max;                               // v

    float delta = max - min;

    if (max != 0.) {
        *s = delta / max;                       // s
    } else {
        // r = g = b = 0		// s = 0, v is undefined
        *s = 0.f;
        *h = 0.f;

        return;
    }

    if (delta == 0.) {
        *h = 0.f;                 // gray
    } else if (r == max) {
        *h = (g - b) / delta;                       // between yellow & magenta
    } else if (g == max) {
        *h = 2 + (b - r) / delta;                   // between cyan & yellow
    } else {
        *h = 4 + (r - g) / delta;                   // between magenta & cyan
    }
    *h *= NATRON_COLOR_HUE_CIRCLE / 6.;
    if (*h < 0) {
        *h += NATRON_COLOR_HUE_CIRCLE;
    }
}

// r,g,b values are linear values from 0 to 1
// h = [0,OFXS_HUE_CIRCLE], s = [0,1], v = [0,1]
//		if s == 0, then h = 0 (undefined)
// Reference:
// "Color gamut transform pairs", Alvy Ray Smith, Proceeding SIGGRAPH '78
// https://doi.org/10.1145/800248.807361
// http://www.icst.pku.edu.cn/F/course/ImageProcessing/2018/resource/Color78.pdf
void
hsv_to_rgb(float h,
           float s,
           float v,
           float *r,
           float *g,
           float *b)
{
    if (s == 0) {
        // achromatic (grey)
        *r = *g = *b = v;

        return;
    }

    h *= 6. / NATRON_COLOR_HUE_CIRCLE;            // sector 0 to 5
    int i = std::floor(h);
    float f = h - i;          // factorial part of h
    i = (i >= 0) ? (i % 6) : (i % 6) + 6; // take h modulo 360
    float p = v * ( 1 - s );
    float q = v * ( 1 - s * f );
    float t = v * ( 1 - s * ( 1 - f ) );

    switch (i) {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:                    // case 5:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
} // hsv_to_rgb
} // namespace Color {
NATRON_NAMESPACE_EXIT

