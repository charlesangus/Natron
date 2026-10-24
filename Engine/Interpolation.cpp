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

#include "Interpolation.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
#include <algorithm> // min, max

#ifndef M_PI
#define M_PI        3.14159265358979323846264338327950288   /* pi             */
#endif

NATRON_NAMESPACE_ENTER

using std::cbrt;
using std::sqrt;
using std::cos;
using std::acos;
using std::sqrt;
using std::fabs;

static void
hermiteToCubicCoeffs(double P0,
                     double P0pr,
                     double P3pl,
                     double P3,
                     double *c0,
                     double *c1,
                     double *c2,
                     double *c3)
{
    *c0 = P0;
    *c1 = P0pr;
    *c2 = 3 * (P3 - P0) - 2 * P0pr - P3pl;
    *c3 = -2 * (P3 - P0) + P0pr + P3pl;
    assert(P0 == P0 && P0pr == P0pr && P3pl == P3pl && P3 == P3 && *c0 == *c0 && *c1 == *c1 && *c2 == *c2 && *c3 == *c3);
}

// evaluate at t
static double
cubicEval(double c0,
          double c1,
          double c2,
          double c3,
          double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    assert(t == t && t2 == t2 && t3 == t3 && c0 == c0 && c1 == c1 && c2 == c2 && c3 == c3);

    return c0 + (c1 ? c1 * t : 0.) + (c2 ? c2 * t2 : 0.) + (c3 ? c3 * t3 : 0.);
}

// integrate from 0 to t
static double
cubicIntegrate(double c0,
               double c1,
               double c2,
               double c3,
               double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double t4 = t3 * t;
    assert(t == t && t2 == t2 && t3 == t3 && t4 == t4 && c0 == c0 && c1 == c1 && c2 == c2 && c3 == c3);

    return (c0 ? c0 * t : 0.) + (c1 ? c1 * t2 / 2. : 0.) + (c2 ? c2 * t3 / 3 : 0.) + (c3 ? c3 * t4 / 4 : 0.);
}

// derive at t
static double
cubicDerive(double /*c0*/,
            double c1,
            double c2,
            double c3,
            double t)
{
    const double t2 = t * t;
    assert(t == t && t2 == t2 && c1 == c1 && c2 == c2 && c3 == c3);

    return c1 + (c2 ? 2 * c2 * t : 0.) + (c3 ? 3 * c3 * t2 : 0.);
}

#define EQN_EPS 1e-9

/********************************************************
 *							*
 * This function determines if a double is small enough	*
 * to be zero. The purpose of the subroutine is to try	*
 * to overcome precision problems in math routines.	*
 *							*
 ********************************************************/
static int
isZero(double x)
{
    assert(x == x);
    return (x > -EQN_EPS && x < EQN_EPS);
}

/// solve linear equation c0 + c1*x = 0.
/// @returns the number of solutions.
/// solutions an and their order are put in s and o
int
Interpolation::solveLinear(double c0,
                           double c1,
                           double s[1],
                           int o[1])
{
    assert(c0 == c0 && c1 == c1);
    if ( (c1 == 0.) || isZero(c1) ) {
        // it's a constant equation

        // there may be an infinity of solutions (if b=0) , but we always return none
        return 0; // no solution
    } else {
        const double a = c1;
        const double b = c0;
        // solve ax+b = 0
        s[0] = -b / a;
        o[0] = 1;

        return 1;
    }
}

/// solve quadric c0 + c1*x + c2*x2 = 0.
/// @returns the number of solutions.
/// solutions an and their order are put in s and o
int
Interpolation::solveQuadric(double c0,
                            double c1,
                            double c2,
                            double s[2],
                            int o[2])
{
    assert(c0 == c0 && c1 == c1 && c2 == c2);
    if ( (c2 == 0.) || isZero(c2) ) {
        // it's at most a linear equation
        return Interpolation::solveLinear(c0, c1, s, o);
    }


    // normal for: x^2 + px + q
    double p = c1 / (2.0 * c2);
    double q = c0 / c2;
    double D = p * p - q;

    if ( (D == 0.) || isZero(D) ) {
        // one double root
        s[0] = -p;
        o[0] = 2;

        return 1;
    } else if (D < 0.0) {
        // no real root
        return 0;
    } else {
        // two real roots
        double sqrt_D = sqrt(D);
        s[0] = sqrt_D - p;
        o[0] = 1;
        s[1] = -sqrt_D - p;
        o[1] = 1;

        return 2;
    }
}

/// solve cubic c0 + c1*x + c2*x2 + c3*x3 = 0.
/// @returns the number of solutions.
/// solutions an and their order are put in s and o
int
Interpolation::solveCubic(double c0,
                          double c1,
                          double c2,
                          double c3,
                          double s[3],
                          int o[3])
{
    assert(c0 == c0 && c1 == c1 && c2 == c2 && c3 == c3);
    if ( (c3 == 0.) || isZero(c3) ) {
        // it's at most a second-degree polynomial
        return Interpolation::solveQuadric(c0, c1, c2, s, o);
    }

    // normalize the equation:x ^ 3 + Ax ^ 2 + Bx  + C = 0
    double A = c2 / c3;
    double B = c1 / c3;
    double C = c0 / c3;

    // substitute x = y - A / 3 to eliminate the quadric term: x^3 + px + q = 0
    double sq_A = A * A;
    double p = 1.0 / 3.0 * (-1.0 / 3.0 * sq_A + B);
    double q = 1.0 / 2.0 * (2.0 / 27.0 * A * sq_A - 1.0 / 3.0 * A * B + C);

    // use Cardano's formula
    double cb_p = p * p * p;
    double D = q * q + cb_p;
    int num;
    if ( (D == 0.) || isZero(D) ) {
        if ( (q == 0.) || isZero(q) ) {
            // one triple solution
            s[0] = 0.;
            o[0] = 3;
            num = 1;
        } else {
            // one single and one double solution
            double u = cbrt(-q);
            s[0] = 2.0 * u;
            o[0] = 1;
            s[1] = -u;
            o[1] = 2;
            num = 2;
        }
    } else if (D < 0.0) {
        // casus irreductibilis: three real solutions
        double phi = 1.0 / 3.0 * acos( -q / sqrt(-cb_p) );
        double t = 2.0 * sqrt(-p);
        s[0] = t * cos(phi);
        o[0] = 1;
        s[1] = -t * cos(phi + M_PI / 3.0);
        o[1] = 1;
        s[2] = -t * cos(phi - M_PI / 3.0);
        o[2] = 1;
        num = 3;
    } else { // D > 0.0
        // one real solution
        double sqrt_D = sqrt(D);
        double u = cbrt( sqrt_D + fabs(q) );
        if (q > 0.0) {
            s[0] = -u + p / u;
        } else {
            s[0] = u - p / u;
        }
        o[0] = 1;
        num = 1;
    }
    // resubstitute
    double sub = 1.0 / 3.0 * A;
    for (int i = 0; i < num; ++i) {
        s[i] -= sub;
    }

    return num;
} // solveCubic

/// compute one root from the cubic c0 + c1*x + c2*x2 + c3*x3 = 0, with c3 != 0.
/// @returns one solution.
static double
getOneCubicRoot(double c0,
                double c1,
                double c2,
                double c3)
{
    assert(c0 == c0 && c1 == c1 && c2 == c2 && c3 == c3);
    assert (c3 != 0.);

    // normalize the equation:x ^ 3 + Ax ^ 2 + Bx  + C = 0
    double A = c2 / c3;
    double B = c1 / c3;
    double C = c0 / c3;

    // substitute x = y - A / 3 to eliminate the quadric term: x^3 + px + q = 0
    double sq_A = A * A;
    double p = 1.0 / 3.0 * (-1.0 / 3.0 * sq_A + B);
    double q = 1.0 / 2.0 * (2.0 / 27.0 * A * sq_A - 1.0 / 3.0 * A * B + C);
    double s;

    // use Cardano's formula
    double cb_p = p * p * p;
    double D = q * q + cb_p;
    if ( (D == 0.) || isZero(D) ) {
        if ( (q == 0.) || isZero(q) ) {
            // one triple solution
            s = 0.;
        } else {
            // one single and one double solution
            double u = cbrt(-q);
            s = 2.0 * u;
        }
    } else if (D < 0.0) {
        // casus irreductibilis: three real solutions
        double phi = 1.0 / 3.0 * acos( -q / sqrt(-cb_p) );
        double t = 2.0 * sqrt(-p);
        s = t * cos(phi);
    } else { // D > 0.0
        // one real solution
        double sqrt_D = sqrt(D);
        double u = cbrt( sqrt_D + fabs(q) );
        if (q > 0.0) {
            s = -u + p / u;
        } else {
            s = u - p / u;
        }
    }

    // resubstitute
    return s - 1.0 / 3.0 * A;
}

/// solve quartic c0 + c1*x + c2*x2 + c3*x3 +c4*x4 = 0.
/// @returns the number of solutions.
/// solutions an and their order are put in s and o
int
Interpolation::solveQuartic(double c0,
                            double c1,
                            double c2,
                            double c3,
                            double c4,
                            double s[4],
                            int o[4])
{
    assert(c0 == c0 && c1 == c1 && c2 == c2 && c3 == c3 && c4 == c4);
    if ( (c4 == 0.) || isZero(c4) ) {
        // it's at most a third-degree polynomial
        return Interpolation::solveCubic(c0, c1, c2, c3, s, o);
    }

    // normalize the equation:x ^ 4 + Ax ^ 3 + Bx ^ 2 + Cx + D = 0

    double A = c3 / c4;
    double B = c2 / c4;
    double C = c1 / c4;
    double D = c0 / c4;

    // substitute x = y - A / 4 to eliminate the cubic term: x^4 + px^2 + qx + r = 0
    double sq_A = A * A;
    double p = -3.0 / 8.0 * sq_A + B;
    double q = 1.0 / 8.0 * sq_A * A - 1.0 / 2.0 * A * B + C;
    double r = -3.0 / 256.0 * sq_A * sq_A + 1.0 / 16.0 * sq_A * B - 1.0 / 4.0 * A * C + D;
    int num;

    if ( (r == 0.) || isZero(r) ) {
        // no absolute term:y(y ^ 3 + py + q) = 0
        num = Interpolation::solveCubic(q, p, 0., 1., s, o);
        // if q = 0, this should be within the previously computed solutions,
        // but we just add another solution with order 1
        s[num] = 0.,
        o[num] = 1;
        ++num;
    } else {
        // solve the resolvent cubic...
        // ...and take one real solution... (there may be more)
        double z = getOneCubicRoot(1.0 / 2.0 * r * p - 1.0 / 8.0 * q * q, -r, -1.0 / 2.0 * p, -1);

        // ...to build two quadratic equations
        double u = z * z - r;
        double v = 2.0 * z - p;

        if ( (u == 0.) || isZero(u) ) {
            u = 0.0;
        } else if (u > 0.0) {
            u = sqrt(u);
        } else {
            return 0;
        }

        if ( isZero(v) ) {
            v = 0;
        } else if (v > 0.0) {
            v = sqrt(v);
        } else {
            return 0;
        }

        num = Interpolation::solveQuadric(z - u, q < 0 ? -v : v, 1.0, s, o);
        num += Interpolation::solveQuadric(z + u, q < 0 ? v : -v, 1.0, s + num, o + num);
    }

    // resubstitute
    double sub = 1.0 / 4 * A;
    for (int i = 0; i < num; i++) {
        s[i] -= sub;
    }

    return num;
} // solveQuartic

double
Interpolation::clampTangentWeight(double w)
{
    if (std::isnan(w)) {
        return kDefaultTangentWeight;
    }

    return (std::min)((std::max)(w, kMinTangentWeight), kMaxTangentWeight);
}

bool
Interpolation::isWeightedSegment(KeyframeTypeEnum interp,
                                 KeyframeTypeEnum interpNext,
                                 double vcurWeightRight,
                                 double vnextWeightLeft)
{
    if ((interp == eKeyframeTypeNone) || (interp == eKeyframeTypeConstant) || (interpNext == eKeyframeTypeNone)) {
        return false;
    }

    return (vcurWeightRight != kDefaultTangentWeight) || (vnextWeightLeft != kDefaultTangentWeight);
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// One segment as Bezier curves X(s) and Y(s), s in [0,1], X in units of the span.
struct BezierSegment {
    BezierSegment(double tcur,
                  double vcur,
                  double vcurDerivRight,
                  double vnextDerivLeft,
                  double vcurWeightRight,
                  double vnextWeightLeft,
                  double tnext,
                  double vnext)
        : origin(tcur)
        , span(tnext - tcur)
    {
        const double a = Interpolation::clampTangentWeight(vcurWeightRight);
        const double b = Interpolation::clampTangentWeight(vnextWeightLeft);

        // The control polygon's legs, so that the power coefficients below are differences of
        // legs rather than of the possibly large control values.
        const double xd0 = a;
        const double xd1 = 1. - a - b;
        const double xd2 = b;
        const double yd0 = vcurDerivRight * a * span;
        const double yd2 = vnextDerivLeft * b * span;
        const double yd1 = (vnext - yd2) - (vcur + yd0);

        x[0] = 0.;
        x[1] = a;
        x[2] = 1. - b;
        x[3] = 1.;
        y[0] = vcur;
        y[1] = vcur + yd0;
        y[2] = vnext - yd2;
        y[3] = vnext;

        xc[0] = 0.;
        xc[1] = 3. * xd0;
        xc[2] = 3. * (xd1 - xd0);
        xc[3] = xd2 - 2. * xd1 + xd0;
        yc[0] = vcur;
        yc[1] = 3. * yd0;
        yc[2] = 3. * (yd1 - yd0);
        yc[3] = yd2 - 2. * yd1 + yd0;

        // dt = span X'(s) ds, so the integral over time is span times that of Y X'.
        const double dx[3] = { xc[1], 2. * xc[2], 3. * xc[3] };
        for (int k = 0; k < 6; ++k) {
            yxc[k] = 0.;
        }
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 3; ++j) {
                yxc[i + j] += yc[i] * dx[j];
            }
        }
    }

    // The Bernstein form returns the end control points exactly at s = 0 and s = 1, which keeps
    // the curve continuous at its keys.
    static double bernstein(const double p[4],
                            double s)
    {
        const double r = 1. - s;

        return r * r * r * p[0] + 3. * r * s * (r * p[1] + s * p[2]) + s * s * s * p[3];
    }

    static double bernsteinDerive(const double p[4],
                                  double s)
    {
        const double r = 1. - s;

        return 3. * (r * r * (p[1] - p[0]) + 2. * r * s * (p[2] - p[1]) + s * s * (p[3] - p[2]));
    }

    double X(double s) const
    {
        return bernstein(x, s);
    }

    double dX(double s) const
    {
        return bernsteinDerive(x, s);
    }

    double Y(double s) const
    {
        return bernstein(y, s);
    }

    double dY(double s) const
    {
        return bernsteinDerive(y, s);
    }

    // The integral of Y(s) X'(s) from 0 to s.
    double integralYdX(double s) const
    {
        double ret = 0.;
        for (int k = 5; k >= 0; --k) {
            ret = (ret + yxc[k] / (k + 1)) * s;
        }

        return ret;
    }

    // The s at which X(s) = u. X is strictly increasing on [0,1] for weights in (0,1): X'/3 has
    // Bernstein coefficients a, 1-a-b and b, and when the middle one is negative it is still
    // above -sqrt(ab), which keeps that quadratic positive. A Newton iteration from the cubic's
    // closed-form root, bisecting whenever a step leaves the bracket, so it always converges.
    double paramAt(double u) const
    {
        if (!(u > 0.)) {
            return 0.;
        }
        if (!(u < 1.)) {
            return 1.;
        }
        double s = u;
        double roots[3];
        int orders[3];
        const int n = Interpolation::solveCubic(-u, xc[1], xc[2], xc[3], roots, orders);
        double bestDistance = std::numeric_limits<double>::infinity();
        for (int i = 0; i < n; ++i) {
            const double r = roots[i];
            const double distance = (r < 0.) ? -r : ((r > 1.) ? r - 1. : 0.);
            if ((r == r) && (distance < bestDistance)) {
                bestDistance = distance;
                s = (std::min)((std::max)(r, 0.), 1.);
            }
        }
        double lo = 0.;
        double hi = 1.;
        for (int i = 0; i < 100; ++i) {
            const double f = X(s) - u;
            if (f == 0.) {
                break;
            }
            if (f < 0.) {
                lo = s;
            } else {
                hi = s;
            }
            double next = s - f / dX(s);
            if (!((next > lo) && (next < hi))) {
                next = 0.5 * (lo + hi);
            }
            if ((next == s) || (hi - lo <= std::numeric_limits<double>::epsilon() * hi)) {
                s = next;
                break;
            }
            s = next;
        }

        return s;
    }

    double paramAtTime(double t) const
    {
        return paramAt((t - origin) / span);
    }

    double origin;
    double span;
    double x[4], y[4];
    double xc[4], yc[4];
    double yxc[6];
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

double
Interpolation::interpolateBezier(double tcur,
                                 double vcur,
                                 double vcurDerivRight,
                                 double vnextDerivLeft,
                                 double vcurWeightRight,
                                 double vnextWeightLeft,
                                 double tnext,
                                 double vnext,
                                 double currentTime)
{
    const BezierSegment seg(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext);

    return seg.Y(seg.paramAtTime(currentTime));
}

double
Interpolation::deriveBezier(double tcur,
                            double vcur,
                            double vcurDerivRight,
                            double vnextDerivLeft,
                            double vcurWeightRight,
                            double vnextWeightLeft,
                            double tnext,
                            double vnext,
                            double currentTime)
{
    const BezierSegment seg(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext);
    const double s = seg.paramAtTime(currentTime);

    return seg.dY(s) / (seg.dX(s) * seg.span);
}

double
Interpolation::integrateBezier(double tcur,
                               double vcur,
                               double vcurDerivRight,
                               double vnextDerivLeft,
                               double vcurWeightRight,
                               double vnextWeightLeft,
                               double tnext,
                               double vnext,
                               double time1,
                               double time2)
{
    const BezierSegment seg(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext);
    const double s1 = seg.paramAtTime(time1);
    const double s2 = seg.paramAtTime(time2);

    return (seg.integralYdX(s2) - seg.integralYdX(s1)) * seg.span;
}

double
Interpolation::integrateBezierClamp(double tcur,
                                    double vcur,
                                    double vcurDerivRight,
                                    double vnextDerivLeft,
                                    double vcurWeightRight,
                                    double vnextWeightLeft,
                                    double tnext,
                                    double vnext,
                                    double time1,
                                    double time2,
                                    double vmin,
                                    double vmax)
{
    const BezierSegment seg(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext);
    const double s1 = seg.paramAtTime(time1);
    const double s2 = seg.paramAtTime(time2);

    // Split [s1,s2] where Y crosses vmin or vmax; each piece is then either clamped or not
    // throughout, which its midpoint tells.
    std::vector<double> cuts;
    cuts.push_back(s1);
    const double bounds[2] = { vmin, vmax };
    for (int b = 0; b < 2; ++b) {
        const double bound = bounds[b];
        if (!(std::fabs(bound) < std::numeric_limits<double>::infinity())) {
            continue;
        }
        double roots[3];
        int orders[3];
        const int n = Interpolation::solveCubic(seg.yc[0] - bound, seg.yc[1], seg.yc[2], seg.yc[3], roots, orders);
        for (int i = 0; i < n; ++i) {
            double r = roots[i];
            for (int k = 0; k < 2; ++k) {
                const double d = seg.dY(r);
                if (d != 0.) {
                    r -= (seg.Y(r) - bound) / d;
                }
            }
            if ((r > s1) && (r < s2)) {
                cuts.push_back(r);
            }
        }
    }
    cuts.push_back(s2);
    std::sort(cuts.begin(), cuts.end());

    double ret = 0.;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double sa = cuts[i];
        const double sb = cuts[i + 1];
        if (!(sb > sa)) {
            continue;
        }
        const double mid = seg.Y(0.5 * (sa + sb));
        if (mid >= vmax) {
            ret += vmax * (seg.X(sb) - seg.X(sa));
        } else if (mid <= vmin) {
            ret += vmin * (seg.X(sb) - seg.X(sa));
        } else {
            ret += seg.integralYdX(sb) - seg.integralYdX(sa);
        }
    }

    return ret * seg.span;
}

/**
 * @brief Interpolates using the control points P0(t0,v0) , P3(t3,v3)
 * and the derivatives P1(t1,v1) (being the derivative at P0 with respect to
 * t \in [t1,t2]) and P2(t2,v2) (being the derivative at P3 with respect to
 * t \in [t1,t2]) the value at 'currentTime' using the
 * interpolation method "interp".
 * Note that for CATMULL-ROM you must use the function interpolate_catmullRom
 * which will compute the derivatives for you.
 **/
double
Interpolation::interpolate(double tcur,
                           const double vcur, // start control point
                           const double vcurDerivRight, // being the derivative dv/dt at tcur
                           const double vnextDerivLeft, // being the derivative dv/dt at tnext
                           const double vcurWeightRight,
                           const double vnextWeightLeft,
                           double tnext,
                           const double vnext, // end control point
                           double currentTime,
                           KeyframeTypeEnum interp,
                           KeyframeTypeEnum interpNext)
{
    if (isWeightedSegment(interp, interpNext, vcurWeightRight, vnextWeightLeft)) {
        return interpolateBezier(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext, currentTime);
    }
    double P0 = vcur;
    double P3 = vnext;
    // Hermite coefficients P0' and P3' are the derivatives with respect to x \in [0,1]
    double P0pr = vcurDerivRight * (tnext - tcur); // normalize for x \in [0,1]
    double P3pl = vnextDerivLeft * (tnext - tcur); // normalize for x \in [0,1]

    // if the following is true, this makes the special case for eKeyframeTypeConstant at tnext useless, and we can always use a cubic - the strict "currentTime < tnext" is the key
    // commented-out: the following assert is not true for periodic curves and passing the flag to interpolate would only be required in NDEBUG
    //assert( ( (interp == eKeyframeTypeNone) || (tcur <= currentTime) ) && ( (currentTime < tnext) || (interpNext == eKeyframeTypeNone) ) );
    // after the last / before the first keyframe, derivatives are wrt currentTime (i.e. non-normalized)
    if (interp == eKeyframeTypeNone) {
        // virtual previous frame at t-1
        P0 = P3 - P3pl;
        P0pr = P3pl;
        tcur = tnext - 1.;
    } else if (interp == eKeyframeTypeConstant) {
        P0pr = 0.;
        P3pl = 0.;
        P3 = P0;
    }
    if (interpNext == eKeyframeTypeNone) {
        // virtual next frame at t+1
        P3pl = P0pr;
        P3 = P0 + P0pr;
        tnext = tcur + 1;
    }
    double c0, c1, c2, c3;
    hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

    const double t = (currentTime - tcur) / (tnext - tcur);
    double ret = cubicEval(c0, c1, c2, c3, t);

    // cubicDerive: divide the result by (tnext-tcur)

    // cubicIntegrate: multiply the result by (tnext-tcur)
    return ret;
}

/// derive at currentTime. The derivative is with respect to currentTime
double
Interpolation::derive(double tcur,
                      const double vcur, // start control point
                      const double vcurDerivRight, // being the derivative dv/dt at tcur
                      const double vnextDerivLeft, // being the derivative dv/dt at tnext
                      const double vcurWeightRight,
                      const double vnextWeightLeft,
                      double tnext,
                      const double vnext, // end control point
                      double currentTime,
                      KeyframeTypeEnum interp,
                      KeyframeTypeEnum interpNext)
{
    if (isWeightedSegment(interp, interpNext, vcurWeightRight, vnextWeightLeft)) {
        return deriveBezier(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext, currentTime);
    }
    double P0 = vcur;
    double P3 = vnext;
    // Hermite coefficients P0' and P3' are the derivatives with respect to x \in [0,1]
    double P0pr = vcurDerivRight * (tnext - tcur); // normalize for x \in [0,1]
    double P3pl = vnextDerivLeft * (tnext - tcur); // normalize for x \in [0,1]

    // if the following is true, this makes the special case for eKeyframeTypeConstant at tnext useless, and we can always use a cubic - the strict "currentTime < tnext" is the key
    assert( ( (interp == eKeyframeTypeNone) || (tcur <= currentTime) ) && ( (currentTime < tnext) || (interpNext == eKeyframeTypeNone) ) );
    // after the last / before the first keyframe, derivatives are wrt currentTime (i.e. non-normalized)
    if (interp == eKeyframeTypeNone) {
        // virtual previous frame at t-1
        P0 = P3 - P3pl;
        P0pr = P3pl;
        tcur = tnext - 1.;
    } else if (interp == eKeyframeTypeConstant) {
        P0pr = 0.;
        P3pl = 0.;
        P3 = P0;
    }
    if (interpNext == eKeyframeTypeNone) {
        // virtual next frame at t+1
        P3pl = P0pr;
        P3 = P0 + P0pr;
        tnext = tcur + 1;
    }
    double c0, c1, c2, c3;
    hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

    const double t = (currentTime - tcur) / (tnext - tcur);
    double ret = cubicDerive(c0, c1, c2, c3, t);

    // cubicDerive: divide the result by (tnext-tcur)

    // cubicIntegrate: multiply the result by (tnext-tcur)
    return ret / (tnext - tcur);
}

/// interpolate and derive at currentTime. The derivative is with respect to currentTime
double
Interpolation::derive_clamp(double tcur,
                            const double vcur, // start control point
                            const double vcurDerivRight, // being the derivative dv/dt at tcur
                            const double vnextDerivLeft, // being the derivative dv/dt at tnext
                            const double vcurWeightRight,
                            const double vnextWeightLeft,
                            double tnext,
                            const double vnext, // end control point
                            double currentTime,
                            double vmin,
                            double vmax,
                            KeyframeTypeEnum interp,
                            KeyframeTypeEnum interpNext)
{
    if (isWeightedSegment(interp, interpNext, vcurWeightRight, vnextWeightLeft)) {
        const double v = interpolateBezier(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext, currentTime);
        if ((vmin < v) && (v < vmax)) {
            return deriveBezier(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext, currentTime);
        }

        return 0.;
    }
    double P0 = vcur;
    double P3 = vnext;
    // Hermite coefficients P0' and P3' are the derivatives with respect to x \in [0,1]
    double P0pr = vcurDerivRight * (tnext - tcur); // normalize for x \in [0,1]
    double P3pl = vnextDerivLeft * (tnext - tcur); // normalize for x \in [0,1]

    // if the following is true, this makes the special case for eKeyframeTypeConstant at tnext useless, and we can always use a cubic - the strict "currentTime < tnext" is the key
    assert( ( (interp == eKeyframeTypeNone) || (tcur <= currentTime) ) && ( (currentTime < tnext) || (interpNext == eKeyframeTypeNone) ) );
    // after the last / before the first keyframe, derivatives are wrt currentTime (i.e. non-normalized)
    if (interp == eKeyframeTypeNone) {
        // virtual previous frame at t-1
        P0 = P3 - P3pl;
        P0pr = P3pl;
        tcur = tnext - 1.;
    } else if (interp == eKeyframeTypeConstant) {
        P0pr = 0.;
        P3pl = 0.;
        P3 = P0;
    }
    if (interpNext == eKeyframeTypeNone) {
        // virtual next frame at t+1
        P3pl = P0pr;
        P3 = P0 + P0pr;
        tnext = tcur + 1;
    }
    double c0, c1, c2, c3;
    hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

    const double t = (currentTime - tcur) / (tnext - tcur);
    double v = cubicEval(c0, c1, c2, c3, t);
    if ( (vmin < v) && (v < vmax) ) {
        // cubicDerive: divide the result by (tnext-tcur)
        return cubicDerive(c0, c1, c2, c3, t) / (tnext - tcur);
    }

    // function is clamped at t, derivative is 0.
    return 0.;
}

// integrate from time1 to time2
double
Interpolation::integrate(double tcur,
                         const double vcur, // start control point
                         const double vcurDerivRight, // being the derivative dv/dt at tcur
                         const double vnextDerivLeft, // being the derivative dv/dt at tnext
                         const double vcurWeightRight,
                         const double vnextWeightLeft,
                         double tnext,
                         const double vnext, // end control point
                         double time1,
                         double time2,
                         KeyframeTypeEnum interp,
                         KeyframeTypeEnum interpNext)
{
    if (isWeightedSegment(interp, interpNext, vcurWeightRight, vnextWeightLeft)) {
        return integrateBezier(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext, time1, time2);
    }
    double P0 = vcur;
    double P3 = vnext;
    // Hermite coefficients P0' and P3' are the derivatives with respect to x \in [0,1]
    double P0pr = vcurDerivRight * (tnext - tcur); // normalize for x \in [0,1]
    double P3pl = vnextDerivLeft * (tnext - tcur); // normalize for x \in [0,1]

    // in the next expression, the correct test is t2 <= tnext (not <), in order to integrate from tcur to tnext
    assert( ( (interp == eKeyframeTypeNone) || (tcur <= time1) ) && (time1 <= time2) && ( (time2 <= tnext) || (interpNext == eKeyframeTypeNone) ) );
    // after the last / before the first keyframe, derivatives are wrt currentTime (i.e. non-normalized)
    if (interp == eKeyframeTypeNone) {
        // virtual previous frame at t-1
        P0 = P3 - P3pl;
        P0pr = P3pl;
        tcur = tnext - 1.;
    } else if (interp == eKeyframeTypeConstant) {
        P0pr = 0.;
        P3pl = 0.;
        P3 = P0;
    }
    if (interpNext == eKeyframeTypeNone) {
        // virtual next frame at t+1
        P3pl = P0pr;
        P3 = P0 + P0pr;
        tnext = tcur + 1;
    }
    double c0, c1, c2, c3;
    hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

    const double t2 = (time2 - tcur) / (tnext - tcur);
    double ret = cubicIntegrate(c0, c1, c2, c3, t2);
    if (time1 != tcur) {
        const double t1 = (time1 - tcur) / (tnext - tcur);
        ret -= cubicIntegrate(c0, c1, c2, c3, t1);
    }
    // cubicDerive: divide the result by (tnext-tcur)

    // cubicIntegrate: multiply the result by (tnext-tcur)
    return ret * (tnext - tcur);
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

enum SolTypeEnum
{
    eSolTypeMin,
    eSolTypeMax
};

enum FuncTypeEnum
{
    eFuncTypeClampMin,
    eFuncTypeClampMax,
    eFuncTypeCubic
};

struct Sol
{
    Sol(SolTypeEnum _type,
        double _t,
        int _order,
        double _deriv)
        : type(_type), t(_t), order(_order), deriv(_deriv)
    {
    }

    SolTypeEnum type;
    double t;
    int order;
    double deriv;
};

struct Sol_less_than_t
{
    inline bool operator() (const Sol & struct1,
                            const Sol & struct2)
    {
        return struct1.t < struct2.t;
    }
};

NATRON_NAMESPACE_ANONYMOUS_EXIT


// comptute the function type after sol, from the function type before sol
static FuncTypeEnum
statusUpdate(FuncTypeEnum status,
             const Sol & sol)
{
    switch (status) {
    case eFuncTypeClampMin:
        assert(sol.type == eSolTypeMin);
        assert(sol.deriv >= /*0*/ -EQN_EPS);
        if (sol.order % 2) {
            // only odd solution orders may change the status
            return eFuncTypeCubic;
        }
        break;
    case eFuncTypeClampMax:
        assert(sol.type == eSolTypeMax);
        assert(sol.deriv <= /*0*/ EQN_EPS);
        if (sol.order % 2) {
            // only odd solution orders may change the status
            return eFuncTypeCubic;
        }
        break;
    case eFuncTypeCubic:
        if (sol.type == eSolTypeMin) {
            assert(sol.deriv <= /*0*/ EQN_EPS);
            if (sol.order % 2) {
                // only odd solution orders may change the status
                return eFuncTypeClampMin;
            }
        } else {
            assert(sol.deriv >= /*0*/ -EQN_EPS);
            if (sol.order % 2) {
                // only odd solution orders may change the status
                return eFuncTypeClampMax;
            }
        }
        break;
    }
    // status is unchanged
    assert( (sol.order % 2) == 0 );

    return status;
}

// integrate from time1 to time2 with clamping of the function values in [vmin,vmax]
double
Interpolation::integrate_clamp(double tcur,
                               const double vcur, // start control point
                               const double vcurDerivRight, // being the derivative dv/dt at tcur
                               const double vnextDerivLeft, // being the derivative dv/dt at tnext
                               const double vcurWeightRight,
                               const double vnextWeightLeft,
                               double tnext,
                               const double vnext, // end control point
                               double time1,
                               double time2,
                               double vmin,
                               double vmax,
                               KeyframeTypeEnum interp,
                               KeyframeTypeEnum interpNext)
{
    if (isWeightedSegment(interp, interpNext, vcurWeightRight, vnextWeightLeft)) {
        return integrateBezierClamp(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft, tnext, vnext, time1, time2, vmin, vmax);
    }
    if ( vmin == -std::numeric_limits<double>::infinity() &&
         vmax == +std::numeric_limits<double>::infinity() ) {
        return integrate(tcur,
                         vcur, // start control point
                         vcurDerivRight, // being the derivative dv/dt at tcur
                         vnextDerivLeft, // being the derivative dv/dt at tnext
                         vcurWeightRight,
                         vnextWeightLeft,
                         tnext,
                         vnext, // end control point
                         time1,
                         time2,
                         interp,
                         interpNext);
    }
    double P0 = vcur;
    double P3 = vnext;
    // Hermite coefficients P0' and P3' are the derivatives with respect to x \in [0,1]
    double P0pr = vcurDerivRight * (tnext - tcur); // normalize for x \in [0,1]
    double P3pl = vnextDerivLeft * (tnext - tcur); // normalize for x \in [0,1]

    // in the next expression, the correct test is t2 <= tnext (not <), in order to integrate from tcur to tnext
    assert( ( (interp == eKeyframeTypeNone) || (tcur <= time1) ) && (time1 <= time2) && ( (time2 <= tnext) || (interpNext == eKeyframeTypeNone) ) );
    // after the last / before the first keyframe, derivatives are wrt currentTime (i.e. non-normalized)
    if (interp == eKeyframeTypeNone) {
        // virtual previous frame at t-1
        P0 = P3 - P3pl;
        P0pr = P3pl;
        tcur = tnext - 1.;
    } else if (interp == eKeyframeTypeConstant) {
        P0pr = 0.;
        P3pl = 0.;
        P3 = P0;
    }
    if (interpNext == eKeyframeTypeNone) {
        // virtual next frame at t+1
        P3pl = P0pr;
        P3 = P0 + P0pr;
        tnext = tcur + 1;
    }
    double c0, c1, c2, c3;
    hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

    // solve cubic = vmax
    double tmax[3];
    int omax[3];
    int nmax = (vmax < std::numeric_limits<double>::max()) ? Interpolation::solveCubic(c0 - vmax, c1, c2, c3, tmax, omax) : 0;
    // solve cubic = vmin
    double tmin[3];
    int omin[3];
    int nmin = (vmin > std::numeric_limits<double>::lowest()) ? Interpolation::solveCubic(c0 - vmin, c1, c2, c3, tmin, omin) : 0;

    // now, find out on which intervals the function is constant/clamped, and on which intervals it is a cubic.
    // ignore the solutions with an order of 2 (which means the tangent is horizontal and the polynomial doesn't change sign)
    // algorithm: order the solutions, sort them wrt time. The cubic sections are where there are transitions between min and max solutions.
    std::vector<Sol> sols;
    for (int i = 0; i < nmax; ++i) {
        double deriv = cubicDerive(c0, c1, c2, c3, tmax[i]);
        assert(deriv == deriv);
        sols.push_back( Sol(eSolTypeMax, tmax[i], omax[i], deriv) );
    }
    for (int i = 0; i < nmin; ++i) {
        double deriv = cubicDerive(c0, c1, c2, c3, tmin[i]);
        assert(deriv == deriv);
        sols.push_back( Sol(eSolTypeMin, tmin[i], omin[i], deriv) );
    }

    const double t2 = (time2 - tcur) / (tnext - tcur);
    const double t1 = (time1 - tcur) / (tnext - tcur);

    // special case: no solution, do the same as Interpolation::integrate()
    if ( sols.empty() ) {
        // no solution.
        // function never crosses vmin or vmax:
        // - either it's entirely below vmin or above vmax
        // - or it' constant

        double ret = cubicIntegrate(c0, c1, c2, c3, t2);
        if (time1 != tcur) {
            ret -= cubicIntegrate(c0, c1, c2, c3, t1);
        }
        // cubicDerive: divide the result by (tnext-tcur)

        // cubicIntegrate: multiply the result by (tnext-tcur)
        return ret * (tnext - tcur);
    }

    // sort the solutions wrt time
    std::sort( sols.begin(), sols.end(), Sol_less_than_t() );
    // find out the status before the first solution
    FuncTypeEnum status;
    if (sols[0].type == eSolTypeMax) {
        // a non-constant cubic cannot remain within [vmin,vmax] at -infinity
        assert(sols[0].deriv < /*0*/ EQN_EPS);
        status = eFuncTypeClampMax;
    } else {
        // a non-constant cubic cannot remain within [vmin,vmax] at -infinity
        assert(sols[0].deriv > /*0*/ -EQN_EPS);
        status = eFuncTypeClampMin;
    }

    // find out the status at t1
    std::vector<Sol>::const_iterator it = sols.begin();
    while (it != sols.end() && it->t <= t1) {
        status = statusUpdate(status, *it);
        ++it;
    }
    double t = t1;
    double ret = 0.;
    // it is now pointing to the first solution after t1, or end()
    while (it != sols.end() && it->t < t2) {
        // integrate from t to it->t
        switch (status) {
        case eFuncTypeClampMax:
            ret += (it->t - t) * vmax;
            break;
        case eFuncTypeClampMin:
            ret += (it->t - t) * vmin;
            break;
        case eFuncTypeCubic:
            ret += cubicIntegrate(c0, c1, c2, c3, it->t) - cubicIntegrate(c0, c1, c2, c3, t);
            break;
        }
        status = statusUpdate(status, *it);
        t = it->t;
        ++it;
    }
    // integrate from t to t2
    switch (status) {
    case eFuncTypeClampMax:
        ret += (t2 - t) * vmax;
        break;
    case eFuncTypeClampMin:
        ret += (t2 - t) * vmin;
        break;
    case eFuncTypeCubic:
        ret += cubicIntegrate(c0, c1, c2, c3, t2) - cubicIntegrate(c0, c1, c2, c3, t);
        break;
    }

    // cubicIntegrate: multiply the result by (tnext-tcur)
    return ret * (tnext - tcur);
} // integrate_clamp

/**
 * @brief This function will set the left and right derivative of 'cur', depending on the interpolation method 'interp' and the
 * previous and next key frames.
 * ----------------------------------------------------------------------------
 * Using the Bezier cubic equation, its 2nd derivative can be expressed as such:
 * B''(t) = 6(1-t)(P2 - 2P1 + P0) + 6t(P3 - 2P2 + P1)
 * We have P1 = P0 + P0'_r / 3
 * and Q2 = Q3 - Q3'_l / 3
 * We can insert it in the 2nd derivative form, which yields:
 * B''(t) = 6(1-t)(P3 - P3'_l/3 - P0 - 2P0'_r/3) + 6t(P0 - P3 + 2P3'_l/3 + P0'_r/3)
 *
 * So for t = 0, we have:
 * B''(0) = 6(P3 - P0 - P3'_l / 3 - 2P0'_r / 3)
 * and for t = 1 , we have:
 * Q''(1) = 6(Q0 - Q3 + 2Q3'_l / 3 + Q0'_r / 3)
 *
 * We also know that the 1st derivative of B(t) at 0 is the derivative to P0
 * and the 1st derivative of B(t) at 1 is the derivative to P3, i.e:
 * B'(0) = P0'_r
 * B'(1) = P3'_l
 **/
/*
   Maple code to compute the values for each case:
   with(CodeGeneration):

   P := t -> (1-t)**3 * P0 + 3 * (1-t)**2 * t * P1 + 3 * (1-t) * t**2 * P2 + t**3 * P3:
   Q := t -> (1-t)**3 * Q0 + 3 * (1-t)**2 * t * Q1 + 3 * (1-t) * t**2 * Q2 + t**3 * Q3:

   dP := D(P):
   dP2 := D(dP):
   dQ := D(Q):
   dQ2 := D(dQ):

   P1 := P0 + P0pr / 3:
   Q2 := Q3 - Q3pl / 3:
   Q1 := Q0 + Q0pr / 3:
   P2 := P3 - P3pl / 3:
   Q3 := P0:

   derivativeAtCurRight := dP(0)/(tnext-tcur):
   curvatureAtCurRight := dP2(0)/(tnext-tcur)**2:
   curvatureAtNextLeft:= dP2(1)/(tnext - tcur)**2:
   derivativeAtCurLeft := dQ(1)/(tcur-tprev):
   curvatureAtCurLeft:= dQ2(1)/(tcur - tprev)**2:
   curvatureAtPrevRight:= dQ2(0)/(tcur - tprev)**2:

   printf("linear, general case:"):
   solve( {curvatureAtCurRight = 0, curvatureAtCurLeft = 0}, { P0pr, Q3pl });
   map(C,%):

   printf("linear, prev is linear:"):
   solve({curvatureAtCurRight = 0, curvatureAtCurLeft = 0, curvatureAtPrevRight = 0}, { P0pr, Q3pl, Q0pr});
   map(C,%):

   printf("linear, next is linear:"):
   solve({curvatureAtCurRight = 0, curvatureAtCurLeft = 0, curvatureAtNextLeft = 0}, {P0pr, Q3pl, P3pl});
   map(C,%):

   printf("linear, prev and next are linear:"):
   solve({curvatureAtCurRight = 0, curvatureAtCurLeft = 0, curvatureAtPrevRight = 0, curvatureAtNextLeft = 0}, {P0pr, Q3pl, Q0pr, P3pl});
   map(C,%):

   printf("cubic, general case:"):
   solve({curvatureAtCurRight = curvatureAtCurLeft, derivativeAtCurRight = derivativeAtCurLeft}, {P0pr, Q3pl});
   map(C,%):

   printf("cubic, prev is linear:"):
   solve({curvatureAtCurRight = curvatureAtCurLeft, derivativeAtCurRight = derivativeAtCurLeft, curvatureAtPrevRight = 0},{P0pr, Q3pl, Q0pr});
   map(C,%):

   printf("cubic, next is linear:"):
   solve({curvatureAtCurRight = curvatureAtCurLeft, derivativeAtCurRight = derivativeAtCurLeft, curvatureAtNextLeft = 0}, {P0pr, Q3pl, P3pl});
   map(C,%):

   printf("cubic, prev and next are linear"):
   solve({curvatureAtCurRight = curvatureAtCurLeft, derivativeAtCurRight = derivativeAtCurLeft, curvatureAtPrevRight = 0, curvatureAtNextLeft = 0},{P0pr, Q3pl, Q0pr, P3pl});
   map(C,%):

 */
void
Interpolation::autoComputeDerivatives(KeyframeTypeEnum interpPrev,
                                      KeyframeTypeEnum interp,
                                      KeyframeTypeEnum interpNext,
                                      double tprev,
                                      const double vprev,             // vprev = Q0
                                      double tcur,
                                      const double vcur,            // vcur = Q3 = P0
                                      double tnext,
                                      const double vnext,             // vnext = P3
                                      const double vprevDerivRight, // Q0'_r
                                      const double vnextDerivLeft, // P3'_l
                                      double *vcurDerivLeft, // Q3'_l
                                      double *vcurDerivRight) // P0'_r
{
    const double Q0 = vprev;
    const double Q3 = vcur;
    const double P0 = vcur;
    const double P3 = vnext;

    // Hermite coefficients P0' and P3' are the derivatives with respect to x \in [0,1]
    if (interpPrev == eKeyframeTypeNone) {
        tprev = tcur - 1.;
    }
    if (interpNext == eKeyframeTypeNone) {
        tnext = tcur + 1.;
    }
    const double Q0pr = vprevDerivRight * (tcur - tprev); // normalize for x \in [0,1]
    const double P3pl = vnextDerivLeft * (tnext - tcur); // normalize for x \in [0,1]
    double P0pr = double();
    double Q3pl = double();

    // if there are no keyframes before and after, the derivatives are zero
    if ( (interpPrev == eKeyframeTypeNone) && (interpNext == eKeyframeTypeNone) ) {
        *vcurDerivRight = 0.;
        *vcurDerivLeft = 0.;
    }

    // If there is no next/previous keyframe, should there be a continuous derivative?
    bool keyframe_none_same_derivative = false;

    // if there is no next/previous keyframe, use LINEAR interpolation (except for horizontal, see bug #1050), and set keyframe_none_same_derivative
    if ( (interp != eKeyframeTypeHorizontal) && ( (interpPrev == eKeyframeTypeNone) || (interpNext == eKeyframeTypeNone) ) ) {
        // Do this before modifying interp (next line)
        keyframe_none_same_derivative = (interp == eKeyframeTypeCatmullRom || interp == eKeyframeTypeCubic);
        interp = eKeyframeTypeLinear;
    }

    switch (interp) {
    case eKeyframeTypeLinear:
        /* Linear means the the 2nd derivative of the cubic curve at the point 'cur' is zero. */
        if (interpNext == eKeyframeTypeNone) {
            P0pr = 0.;
        } else if (interpNext == eKeyframeTypeLinear) {
            P0pr = -P0 + P3;
        } else {
            P0pr = -0.3e1 / 0.2e1 * P0 + 0.3e1 / 0.2e1 * P3 - P3pl / 0.2e1;
        }

        if (interpPrev == eKeyframeTypeNone) {
            Q3pl = 0.;
        } else if (interpPrev == eKeyframeTypeLinear) {
            Q3pl = -Q0 + P0;
        } else {
            Q3pl = -0.3e1 / 0.2e1 * Q0 - Q0pr / 0.2e1 + 0.3e1 / 0.2e1 * P0;
        }

        if (keyframe_none_same_derivative) {
            if (interpNext == eKeyframeTypeNone) {
                P0pr = Q3pl / (tcur - tprev);
            } else if (interpPrev == eKeyframeTypeNone) {
                Q3pl = P0pr / (tnext - tcur);
            }
        }
        break;

    case eKeyframeTypeCatmullRom: {
        /* http://en.wikipedia.org/wiki/Cubic_Hermite_spline We use the formula given to compute the derivatives*/
        double deriv = (vnext - vprev) / (tnext - tprev);
        P0pr = deriv * (tnext - tcur);
        Q3pl = deriv * (tcur - tprev);
        break;
    }

    case eKeyframeTypeSmooth:

        // If vcur is outside of the range [vprev,vnext], then interpolation is horizontal
        if ( ( ( vprev > vcur) && ( vcur < vnext) ) || ( ( vprev < vcur) && ( vcur > vnext) ) ) {
            P0pr = 0.;
            Q3pl = 0.;
        } else {
            // Catmull-Rom interpolatio, see above
            /* http://en.wikipedia.org/wiki/Cubic_Hermite_spline We use the formula given to compute the derivatives*/
            double deriv = (vnext - vprev) / (tnext - tprev);
            P0pr = deriv * (tnext - tcur);
            Q3pl = deriv * (tcur - tprev);

            /*Now that we have the derivative by catmull-rom's formula, we compute the bezier
               point on the left and on the right from the derivatives (i.e: P1 and Q2, Q being the segment before P)
             */
            double P1 = P0 + P0pr / 3.;
            double Q2 = Q3 - Q3pl / 3.;

            /*We clamp Q2 to Q0(aka vprev) and Q3(aka vcur)
               and P1 to P0(aka vcur) and P3(aka vnext)*/
            double prevMax = std::max(vprev, vcur);
            double prevMin = std::min(vprev, vcur);
            if ( ( Q2 < prevMin) || ( Q2 > prevMax) ) {
                double Q2new = std::max( prevMin, std::min(Q2, prevMax) );
                P1 = P0 + (P1 - P0) * (Q3 - Q2new) / (Q3 - Q2);
                Q2 = Q2new;
            }

            double nextMax = std::max(vcur, vnext);
            double nextMin = std::min(vcur, vnext);
            if ( ( P1 < nextMin) || ( P1 > nextMax) ) {
                double P1new = std::max( nextMin, std::min(P1, nextMax) );
                Q2 = Q3 - (Q3 - Q2) * (P1new - P0) / (P1 - P0);
                P1 = P1new;
            }

            /*We recompute the derivatives from the new clamped control points*/

            P0pr = 3. * (P1 - P0);
            Q3pl = 3. * (Q3 - Q2);
        }
        break;

    case eKeyframeTypeHorizontal:
    case eKeyframeTypeConstant:
        /*The values are the same than the keyframe they belong. */
        P0pr = 0.;
        Q3pl = 0.;
        break;

    case eKeyframeTypeCubic:
        /* Cubic means the the 2nd derivative of the cubic curve at the point 'cur' are equal. */
        if ( ( interpPrev == eKeyframeTypeLinear) && ( interpNext == eKeyframeTypeLinear) ) {
            // cubic, prev and next are linear
            P0pr = -(Q0 * tnext * tnext - 2 * Q0 * tnext * tcur - 2 * P0 * tcur * tprev + 2 * P3 * tcur * tprev + 2 * P0 * tnext * tcur - P3 * tprev * tprev + P0 * tprev * tprev - P0 * tnext * tnext + Q0 * tcur * tcur - P3 * tcur * tcur) / (tcur - tprev) / (tnext - tprev);
            Q3pl = (Q0 * tnext * tnext - 2 * Q0 * tnext * tcur - 2 * P0 * tcur * tprev + 2 * P3 * tcur * tprev + 2 * P0 * tnext * tcur - P3 * tprev * tprev + P0 * tprev * tprev - P0 * tnext * tnext + Q0 * tcur * tcur - P3 * tcur * tcur) / (-tnext * tnext + tnext * tcur + tprev * tnext - tcur * tprev);
        } else if (interpPrev == eKeyframeTypeLinear) {
            // cubic, prev is linear:
            P0pr = -(2 * P3pl * tcur * tcur + 3 * P0 * tcur * tcur - 6 * P3 * tcur * tcur + 3 * Q0 * tcur * tcur - 6 * Q0 * tnext * tcur - 4 * P3pl * tcur * tprev - 12 * P0 * tcur * tprev + 6 * P0 * tnext * tcur + 12 * P3 * tcur * tprev + 2 * P3pl * tprev * tprev + 3 * Q0 * tnext * tnext - 6 * P3 * tprev * tprev + 6 * P0 * tprev * tprev - 3 * P0 * tnext * tnext) / (tcur - tprev) / (tcur - 4 * tprev + 3 * tnext);
            Q3pl = (2 * P3pl * tcur * tcur + 3 * P0 * tcur * tcur - 6 * P3 * tcur * tcur + 3 * Q0 * tcur * tcur - 6 * Q0 * tnext * tcur - 4 * P3pl * tcur * tprev - 12 * P0 * tcur * tprev + 6 * P0 * tnext * tcur + 12 * P3 * tcur * tprev + 2 * P3pl * tprev * tprev + 3 * Q0 * tnext * tnext - 6 * P3 * tprev * tprev + 6 * P0 * tprev * tprev - 3 * P0 * tnext * tnext) / (2 * tnext * tcur - 4 * tcur * tprev + 4 * tprev * tnext + tcur * tcur - 3 * tnext * tnext);
        } else if (interpNext == eKeyframeTypeLinear) {
            // cubic, next is linear:
            P0pr = -(6 * P0 * tcur * tprev - 6 * P3 * tcur * tprev + 12 * Q0 * tnext * tcur + 4 * Q0pr * tnext * tcur - 12 * P0 * tnext * tcur - 2 * Q0pr * tnext * tnext + 3 * P3 * tcur * tcur + 3 * P3 * tprev * tprev - 2 * Q0pr * tcur * tcur + 6 * P0 * tnext * tnext + 3 * P0 * tcur * tcur - 3 * P0 * tprev * tprev - 6 * Q0 * tnext * tnext - 6 * Q0 * tcur * tcur) / (tcur - tprev) / (tcur - 4 * tnext + 3 * tprev);
            Q3pl = (6 * P0 * tcur * tprev - 6 * P3 * tcur * tprev + 12 * Q0 * tnext * tcur + 4 * Q0pr * tnext * tcur - 12 * P0 * tnext * tcur - 2 * Q0pr * tnext * tnext + 3 * P3 * tcur * tcur + 3 * P3 * tprev * tprev - 2 * Q0pr * tcur * tcur + 6 * P0 * tnext * tnext + 3 * P0 * tcur * tcur - 3 * P0 * tprev * tprev - 6 * Q0 * tnext * tnext - 6 * Q0 * tcur * tcur) / (-5 * tnext * tcur - 3 * tprev * tnext + 3 * tcur * tprev + 4 * tnext * tnext + tcur * tcur);
        } else {
            // cubic, general case:
            P0pr = -(6 * P0 * tcur * tprev - 6 * P3 * tcur * tprev + 12 * Q0 * tnext * tcur + 4 * Q0pr * tnext * tcur - 12 * P0 * tnext * tcur - 2 * Q0pr * tnext * tnext + 3 * P3 * tcur * tcur + 3 * P3 * tprev * tprev - 2 * Q0pr * tcur * tcur + 6 * P0 * tnext * tnext + 3 * P0 * tcur * tcur - 3 * P0 * tprev * tprev - 6 * Q0 * tnext * tnext - 6 * Q0 * tcur * tcur) / (tcur - tprev) / (tcur - 4 * tnext + 3 * tprev);
            Q3pl = (6 * P0 * tcur * tprev - 6 * P3 * tcur * tprev + 12 * Q0 * tnext * tcur + 4 * Q0pr * tnext * tcur - 12 * P0 * tnext * tcur - 2 * Q0pr * tnext * tnext + 3 * P3 * tcur * tcur + 3 * P3 * tprev * tprev - 2 * Q0pr * tcur * tcur + 6 * P0 * tnext * tnext + 3 * P0 * tcur * tcur - 3 * P0 * tprev * tprev - 6 * Q0 * tnext * tnext - 6 * Q0 * tcur * tcur) / (-5 * tnext * tcur - 3 * tprev * tnext + 3 * tcur * tprev + 4 * tnext * tnext + tcur * tcur);
        }
        break;

    case eKeyframeTypeNone:
    case eKeyframeTypeFree:
    case eKeyframeTypeBroken:
        throw std::runtime_error("Cannot compute derivatives at eKeyframeTypeNone, eKeyframeTypeFree or eKeyframeTypeBroken");
    } // switch

    *vcurDerivRight = P0pr / (tnext - tcur); // denormalize for t \in [tcur,tnext]
    *vcurDerivLeft = Q3pl / (tcur - tprev); // denormalize for t \in [tprev,tcur]
    assert( !std::isnan(*vcurDerivRight) && !(std::isnan)(*vcurDerivLeft) );
} // autoComputeDerivatives

NATRON_NAMESPACE_EXIT

