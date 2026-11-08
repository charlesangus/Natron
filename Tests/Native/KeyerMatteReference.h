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

#ifndef Tests_Native_KeyerMatteReference_h
#define Tests_Native_KeyerMatteReference_h

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
 * @brief The reference trapezoid matte in double, which the float matte of the render has to match
 * up to its rounding. See trapezoidMatteFloat() for the shape.
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

NATRON_NAMESPACE_EXIT

#endif // Tests_Native_KeyerMatteReference_h
