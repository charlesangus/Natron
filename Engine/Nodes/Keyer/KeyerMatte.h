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

#ifndef Engine_Nodes_Keyer_KeyerMatte_h
#define Engine_Nodes_Keyer_KeyerMatte_h

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The four breakpoints of a trapezoid matte: the matte rises from 0 at a to 1 at b, stays
 * at 1 up to c and falls back to 0 at d. trapezoidMatte() assumes a <= b <= c <= d.
 **/
struct KeyerTrapezoid {
    double a;
    double b;
    double c;
    double d;
};

/**
 * @brief The trapezoid matte of the key value k: 0 for k <= a or k >= d, 1 for b <= k <= c, and
 * linear between. An edge whose two breakpoints are equal is a hard step, and the plateau wins
 * when it touches an edge, so an all-equal trapezoid is 1 at that value and 0 everywhere else.
 **/
inline float
trapezoidMatte(double k,
               double a,
               double b,
               double c,
               double d)
{
    if ((k >= b) && (k <= c)) {
        return 1.f;
    }
    if ((k <= a) || (k >= d)) {
        return 0.f;
    }
    if (k < b) {
        return (float)((k - a) / (b - a));
    }

    return (float)((d - k) / (d - c));
}

inline float
trapezoidMatte(double k,
               const KeyerTrapezoid& t)
{
    return trapezoidMatte(k, t.a, t.b, t.c, t.d);
}

/**
 * @brief trapezoidMatte() in float, with one rounding per operation, as the GPU kernel
 * computes it; range holds a, b, c and d.
 **/
inline float
trapezoidMatteFloat(float k,
                    const float range[4])
{
    if ((k >= range[1]) && (k <= range[2])) {
        return 1.f;
    }
    if ((k <= range[0]) || (k >= range[3])) {
        return 0.f;
    }
    if (k < range[1]) {
        const float num = k - range[0];
        const float den = range[1] - range[0];

        return num / den;
    }
    const float num = range[3] - k;
    const float den = range[3] - range[2];

    return num / den;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_KeyerMatte_h
