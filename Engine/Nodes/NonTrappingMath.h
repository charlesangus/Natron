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

#ifndef Engine_Nodes_NonTrappingMath_h
#define Engine_Nodes_NonTrappingMath_h

#include "Global/Macros.h"

#include <cmath>
#include <limits>

NATRON_NAMESPACE_ENTER

// The double operations below give the infinity of the exact result's sign where the double result
// would overflow, which debug builds trap. Operations that would be invalid on infinite operands
// give the quiet NaN they would produce; NaN operands pass through the plain operation.

// The double operations below give the infinity of the exact result's sign where the double result
// would overflow, which debug builds trap. Operations that would be invalid on infinite operands
// give the quiet NaN they would produce; NaN operands pass through the plain operation.

// Operands below 2^500 cannot overflow, which keeps per-pixel loops on the plain product. Past
// that the exponent sum separates the certain cases; in between, a quarter of the product is compared
// against 2^1022, which is exact for the normal operands that reach it.
inline double
productOrInfinity(double a,
                  double b)
{
    if ((std::fabs(a) < 0x1p500) && (std::fabs(b) < 0x1p500)) {
        return a * b;
    }
    if (std::isnan(a) || std::isnan(b)) {
        return a * b;
    }
    if (std::isinf(a) || std::isinf(b)) {
        return ((a == 0.) || (b == 0.)) ? std::numeric_limits<double>::quiet_NaN() : a * b;
    }
    if ((a == 0.) || (b == 0.)) {
        return a * b;
    }
    int ea = 0;
    int eb = 0;
    std::frexp(a, &ea);
    std::frexp(b, &eb);
    const int sum = ea + eb;
    const bool overflows = (sum >= 1026) || ((sum >= 1024) && (std::fabs((0.25 * a) * b) >= 0x1p1022));

    return overflows ? std::copysign(std::numeric_limits<double>::infinity(), ((a < 0.) != (b < 0.)) ? -1. : 1.) : a * b;
}

// The exponent difference separates the certain cases; at a difference of 1024 a quarter of the
// quotient, taken from the exact quarter of a normal a, is compared against 2^1022. b is nonzero.
inline double
quotientOrInfinity(double a,
                   double b)
{
    if (std::isnan(a) || std::isnan(b) || (b == 0.)) {
        return a / b;
    }
    if (std::isinf(a) || std::isinf(b)) {
        return (std::isinf(a) && std::isinf(b)) ? std::numeric_limits<double>::quiet_NaN() : a / b;
    }
    if (a == 0.) {
        return a / b;
    }
    int ea = 0;
    int eb = 0;
    std::frexp(a, &ea);
    std::frexp(b, &eb);
    const int diff = ea - eb;
    const bool overflows = (diff >= 1025) || ((diff == 1024) && (std::fabs((0.25 * a) / b) >= 0x1p1022));

    return overflows ? std::copysign(std::numeric_limits<double>::infinity(), ((a < 0.) != (b < 0.)) ? -1. : 1.) : a / b;
}

// Halving is exact, so half the sum reaches 2^1023 exactly when the sum rounds to infinity.
inline double
sumOrInfinity(double p,
              double q)
{
    if (!std::isfinite(p) || !std::isfinite(q)) {
        return (std::isinf(p) && std::isinf(q) && (std::signbit(p) != std::signbit(q))) ? std::numeric_limits<double>::quiet_NaN() : p + q;
    }
    const double half = (0.5 * p) + (0.5 * q);

    return (std::fabs(half) >= 0x1p1023) ? std::copysign(std::numeric_limits<double>::infinity(), half) : p + q;
}

// Not p + (-q), which would flip the sign of a NaN q.
inline double
differenceOrInfinity(double p,
                     double q)
{
    if (!std::isfinite(p) || !std::isfinite(q)) {
        return (std::isinf(p) && std::isinf(q) && (std::signbit(p) == std::signbit(q))) ? std::numeric_limits<double>::quiet_NaN() : p - q;
    }
    const double half = (0.5 * p) - (0.5 * q);

    return (std::fabs(half) >= 0x1p1023) ? std::copysign(std::numeric_limits<double>::infinity(), half) : p - q;
}

// x^e for x > 0 and e >= 0. Clear of overflow, log2 decides. Near it, the square of x^(e/2) decides
// with a margin of 2^-45, far wider than the error of either pow, so only a power within 2^-45 of
// DBL_MAX may be taken as infinity.
inline double
powOrInfinity(double x,
              double e)
{
    if (std::isfinite(x) && std::isfinite(e) && (x > 1.)) {
        const double l = std::log2(x);
        if (e > 1030. / l) {
            return std::numeric_limits<double>::infinity();
        }
        if (e > 1022. / l) {
            const double h = std::pow(x, 0.5 * e);
            if ((0x1p-8 * h) * h >= 0x1p1016 * (1. - 0x1p-45)) {
                return std::numeric_limits<double>::infinity();
            }
        }
    }

    return std::pow(x, e);
}

// A reciprocal below 2^-1024 would overflow, which debug builds trap; 1 / +-0 gives the infinity of
// the zero's sign, so that is what every operand in that range gives.
inline double
reciprocalOrInfinity(double g)
{
    return (std::fabs(g) <= 0x1p-1024) ? std::copysign(std::numeric_limits<double>::infinity(), g) : 1. / g;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_NonTrappingMath_h
