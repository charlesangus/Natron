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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "Engine/Interpolation.h"

NATRON_NAMESPACE_USING

namespace {

// The Hermite evaluation as it was before tangents had weights, kept verbatim so that unweighted
// segments can be checked to evaluate to the same bits.
namespace Reference {

    void
    hermiteToCubicCoeffs(double P0,
                         double P0pr,
                         double P3pl,
                         double P3,
                         double* c0,
                         double* c1,
                         double* c2,
                         double* c3)
    {
        *c0 = P0;
        *c1 = P0pr;
        *c2 = 3 * (P3 - P0) - 2 * P0pr - P3pl;
        *c3 = -2 * (P3 - P0) + P0pr + P3pl;
    }

    double
    cubicEval(double c0,
              double c1,
              double c2,
              double c3,
              double t)
    {
        const double t2 = t * t;
        const double t3 = t2 * t;

        return c0 + (c1 ? c1 * t : 0.) + (c2 ? c2 * t2 : 0.) + (c3 ? c3 * t3 : 0.);
    }

    double
    cubicIntegrate(double c0,
                   double c1,
                   double c2,
                   double c3,
                   double t)
    {
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double t4 = t3 * t;

        return (c0 ? c0 * t : 0.) + (c1 ? c1 * t2 / 2. : 0.) + (c2 ? c2 * t3 / 3 : 0.) + (c3 ? c3 * t4 / 4 : 0.);
    }

    double
    cubicDerive(double /*c0*/,
                double c1,
                double c2,
                double c3,
                double t)
    {
        const double t2 = t * t;

        return c1 + (c2 ? 2 * c2 * t : 0.) + (c3 ? 3 * c3 * t2 : 0.);
    }

    void
    setup(double& tcur,
          double& P0,
          double& P0pr,
          double& P3pl,
          double& tnext,
          double& P3,
          KeyframeTypeEnum interp,
          KeyframeTypeEnum interpNext)
    {
        if (interp == eKeyframeTypeNone) {
            P0 = P3 - P3pl;
            P0pr = P3pl;
            tcur = tnext - 1.;
        } else if (interp == eKeyframeTypeConstant) {
            P0pr = 0.;
            P3pl = 0.;
            P3 = P0;
        }
        if (interpNext == eKeyframeTypeNone) {
            P3pl = P0pr;
            P3 = P0 + P0pr;
            tnext = tcur + 1;
        }
    }

    double
    interpolate(double tcur,
                const double vcur,
                const double vcurDerivRight,
                const double vnextDerivLeft,
                double tnext,
                const double vnext,
                double currentTime,
                KeyframeTypeEnum interp,
                KeyframeTypeEnum interpNext)
    {
        double P0 = vcur;
        double P3 = vnext;
        double P0pr = vcurDerivRight * (tnext - tcur);
        double P3pl = vnextDerivLeft * (tnext - tcur);

        setup(tcur, P0, P0pr, P3pl, tnext, P3, interp, interpNext);
        double c0, c1, c2, c3;
        hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

        const double t = (currentTime - tcur) / (tnext - tcur);

        return cubicEval(c0, c1, c2, c3, t);
    }

    double
    derive(double tcur,
           const double vcur,
           const double vcurDerivRight,
           const double vnextDerivLeft,
           double tnext,
           const double vnext,
           double currentTime,
           KeyframeTypeEnum interp,
           KeyframeTypeEnum interpNext)
    {
        double P0 = vcur;
        double P3 = vnext;
        double P0pr = vcurDerivRight * (tnext - tcur);
        double P3pl = vnextDerivLeft * (tnext - tcur);

        setup(tcur, P0, P0pr, P3pl, tnext, P3, interp, interpNext);
        double c0, c1, c2, c3;
        hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

        const double t = (currentTime - tcur) / (tnext - tcur);
        double ret = cubicDerive(c0, c1, c2, c3, t);

        return ret / (tnext - tcur);
    }

    double
    integrate(double tcur,
              const double vcur,
              const double vcurDerivRight,
              const double vnextDerivLeft,
              double tnext,
              const double vnext,
              double time1,
              double time2,
              KeyframeTypeEnum interp,
              KeyframeTypeEnum interpNext)
    {
        double P0 = vcur;
        double P3 = vnext;
        double P0pr = vcurDerivRight * (tnext - tcur);
        double P3pl = vnextDerivLeft * (tnext - tcur);

        setup(tcur, P0, P0pr, P3pl, tnext, P3, interp, interpNext);
        double c0, c1, c2, c3;
        hermiteToCubicCoeffs(P0, P0pr, P3pl, P3, &c0, &c1, &c2, &c3);

        const double t2 = (time2 - tcur) / (tnext - tcur);
        double ret = cubicIntegrate(c0, c1, c2, c3, t2);
        if (time1 != tcur) {
            const double t1 = (time1 - tcur) / (tnext - tcur);
            ret -= cubicIntegrate(c0, c1, c2, c3, t1);
        }

        return ret * (tnext - tcur);
    }
} // namespace Reference

struct Segment {
    double tcur, vcur, slopeRight, slopeLeft, tnext, vnext;
};

Segment
randomSegment(std::mt19937& rng)
{
    std::uniform_real_distribution<double> time(-50., 50.);
    std::uniform_real_distribution<double> span(0.5, 20.);
    std::uniform_real_distribution<double> value(-10., 10.);
    Segment s;

    s.tcur = time(rng);
    s.tnext = s.tcur + span(rng);
    s.vcur = value(rng);
    s.vnext = value(rng);
    s.slopeRight = value(rng);
    s.slopeLeft = value(rng);

    return s;
}

const KeyframeTypeEnum kAllTypes[] = {
    eKeyframeTypeConstant, eKeyframeTypeLinear, eKeyframeTypeSmooth, eKeyframeTypeCatmullRom, eKeyframeTypeCubic,
    eKeyframeTypeHorizontal, eKeyframeTypeFree, eKeyframeTypeBroken, eKeyframeTypeNone
};
} // anon namespace

// The Bezier path with both weights at 1/3 is the Hermite cubic, up to rounding.
TEST(WeightedInterpolation, BezierAtDefaultWeightsMatchesHermite)
{
    const double w = Interpolation::kDefaultTangentWeight;
    std::mt19937 rng(96);

    for (int i = 0; i < 500; ++i) {
        const Segment s = randomSegment(rng);
        const double span = s.tnext - s.tcur;
        for (int j = 0; j <= 16; ++j) {
            const double t = s.tcur + span * j / 16.;
            const double hermite = Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, t,
                                                              eKeyframeTypeFree, eKeyframeTypeFree);
            const double bezier = Interpolation::interpolateBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, t);
            EXPECT_NEAR(hermite, bezier, 1e-12 * (1. + std::fabs(hermite))) << "segment " << i << ", t " << t;

            if (j < 16) {
                const double dHermite = Interpolation::derive(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, t,
                                                              eKeyframeTypeFree, eKeyframeTypeFree);
                const double dBezier = Interpolation::deriveBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, t);
                EXPECT_NEAR(dHermite, dBezier, 1e-12 * (1. + std::fabs(dHermite))) << "segment " << i << ", t " << t;
            }

            const double iHermite = Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, s.tcur, t,
                                                             eKeyframeTypeFree, eKeyframeTypeFree);
            const double iBezier = Interpolation::integrateBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, s.tcur, t);
            EXPECT_NEAR(iHermite, iBezier, 1e-12 * (1. + std::fabs(iHermite) + 10. * span)) << "segment " << i << ", t " << t;
        }
    }
}

// Unweighted segments, and segments whose weights are ignored, evaluate to exactly the bits the
// Hermite code gave before weights existed.
TEST(WeightedInterpolation, UnweightedIsBitwiseHermite)
{
    std::mt19937 rng(1996);

    for (KeyframeTypeEnum interp : kAllTypes) {
        for (KeyframeTypeEnum interpNext : kAllTypes) {
            const bool weightsIgnored = (interp == eKeyframeTypeNone) || (interp == eKeyframeTypeConstant) || (interpNext == eKeyframeTypeNone);
            EXPECT_FALSE(Interpolation::isWeightedSegment(interp, interpNext, Interpolation::kDefaultTangentWeight, Interpolation::kDefaultTangentWeight));
            EXPECT_EQ(!weightsIgnored, Interpolation::isWeightedSegment(interp, interpNext, 0.7, 0.2));
            for (int i = 0; i < 20; ++i) {
                const Segment s = randomSegment(rng);
                double wRight = Interpolation::kDefaultTangentWeight;
                double wLeft = Interpolation::kDefaultTangentWeight;
                if (weightsIgnored && (i % 2)) {
                    wRight = 0.7;
                    wLeft = 0.2;
                }
                const double span = s.tnext - s.tcur;
                for (int j = 0; j < 8; ++j) {
                    const double t = s.tcur + span * j / 8.;
                    EXPECT_EQ(Reference::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, s.tnext, s.vnext, t, interp, interpNext),
                              Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, wRight, wLeft, s.tnext, s.vnext, t, interp, interpNext));
                    EXPECT_EQ(Reference::derive(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, s.tnext, s.vnext, t, interp, interpNext),
                              Interpolation::derive(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, wRight, wLeft, s.tnext, s.vnext, t, interp, interpNext));
                    EXPECT_EQ(Reference::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, s.tnext, s.vnext, s.tcur, t, interp, interpNext),
                              Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, wRight, wLeft, s.tnext, s.vnext, s.tcur, t, interp, interpNext));
                }
            }
        }
    }
}

TEST(WeightedInterpolation, ContinuousAtSegmentEnds)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> weight(Interpolation::kMinTangentWeight, Interpolation::kMaxTangentWeight);

    for (int i = 0; i < 200; ++i) {
        const Segment s = randomSegment(rng);
        const double a = weight(rng);
        const double b = weight(rng);
        const double span = s.tnext - s.tcur;

        EXPECT_EQ(s.vcur, Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tcur, eKeyframeTypeFree, eKeyframeTypeBroken));
        EXPECT_EQ(s.vnext, Interpolation::interpolateBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tnext));
        const double nearEnd = s.tnext - 1e-9 * span;
        EXPECT_NEAR(s.vnext, Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, nearEnd, eKeyframeTypeFree, eKeyframeTypeBroken), 1e-6);

        EXPECT_EQ(0., Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tcur, s.tcur, eKeyframeTypeFree, eKeyframeTypeFree));
        const double whole = Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tcur, s.tnext,
                                                      eKeyframeTypeFree, eKeyframeTypeFree);
        const double almost = Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tcur, nearEnd,
                                                       eKeyframeTypeFree, eKeyframeTypeFree);
        EXPECT_NEAR(whole, almost, 1e-6);
        const double tm = s.tcur + 0.37 * span;
        const double split = Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tcur, tm,
                                                      eKeyframeTypeFree, eKeyframeTypeFree)
            + Interpolation::integrate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, tm, s.tnext,
                                       eKeyframeTypeFree, eKeyframeTypeFree);
        EXPECT_NEAR(whole, split, 1e-10 * (1. + std::fabs(whole)));
    }
}

TEST(WeightedInterpolation, DerivativeMatchesFiniteDifference)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> weight(0.05, 0.95);

    for (int i = 0; i < 200; ++i) {
        const Segment s = randomSegment(rng);
        const double a = weight(rng);
        const double b = weight(rng);
        const double span = s.tnext - s.tcur;
        const double h = 1e-6 * span;
        for (int j = 1; j < 16; ++j) {
            const double t = s.tcur + span * j / 16.;
            const double d = Interpolation::derive(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, t,
                                                   eKeyframeTypeBroken, eKeyframeTypeFree);
            const double plus = Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, t + h,
                                                           eKeyframeTypeBroken, eKeyframeTypeFree);
            const double minus = Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, t - h,
                                                            eKeyframeTypeBroken, eKeyframeTypeFree);
            EXPECT_NEAR(d, (plus - minus) / (2. * h), 1e-4 * (1. + std::fabs(d))) << "segment " << i << ", t " << t;
        }
        // At the keys the derivative is the key's slope, whatever the weight.
        EXPECT_NEAR(s.slopeRight, Interpolation::deriveBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tcur),
                    1e-9 * (1. + std::fabs(s.slopeRight)));
        EXPECT_NEAR(s.slopeLeft, Interpolation::deriveBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, s.tnext),
                    1e-9 * (1. + std::fabs(s.slopeLeft)));
    }
}

TEST(WeightedInterpolation, MaximumWeightsKeepTimeMonotonic)
{
    const double w = Interpolation::kMaxTangentWeight;
    std::mt19937 rng(99);

    for (int i = 0; i < 100; ++i) {
        const Segment s = randomSegment(rng);
        const double span = s.tnext - s.tcur;
        double previousIntegral = 0.;
        for (int j = 0; j <= 1000; ++j) {
            const double t = s.tcur + span * j / 1000.;
            const double v = Interpolation::interpolate(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, t,
                                                        eKeyframeTypeFree, eKeyframeTypeFree);
            const double d = Interpolation::deriveBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, w, w, s.tnext, s.vnext, t);
            EXPECT_TRUE(std::isfinite(v)) << "t " << t;
            EXPECT_TRUE(std::isfinite(d)) << "t " << t;

            // A curve that is 1 everywhere integrates to the elapsed time only if every time maps
            // back to the one Bezier parameter at which X reaches it.
            const double elapsed = Interpolation::integrateBezier(s.tcur, 1., 0., 0., w, w, s.tnext, 1., s.tcur, t);
            EXPECT_NEAR(t - s.tcur, elapsed, 1e-12 * span) << "t " << t;
            EXPECT_GE(elapsed, previousIntegral);
            previousIntegral = elapsed;
        }
    }
}

TEST(WeightedInterpolation, ClampedIntegralMatchesQuadrature)
{
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> weight(0.05, 0.95);

    for (int i = 0; i < 50; ++i) {
        const Segment s = randomSegment(rng);
        const double a = weight(rng);
        const double b = weight(rng);
        const double vmin = -3.;
        const double vmax = 4.;
        const double t1 = s.tcur + 0.1 * (s.tnext - s.tcur);
        const double t2 = s.tcur + 0.85 * (s.tnext - s.tcur);
        const int n = 20000;
        const double h = (t2 - t1) / n;
        double quadrature = 0.;
        for (int j = 0; j < n; ++j) {
            const double v = Interpolation::interpolateBezier(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, t1 + (j + 0.5) * h);
            quadrature += (std::min)((std::max)(v, vmin), vmax) * h;
        }
        const double exact = Interpolation::integrate_clamp(s.tcur, s.vcur, s.slopeRight, s.slopeLeft, a, b, s.tnext, s.vnext, t1, t2, vmin, vmax,
                                                            eKeyframeTypeFree, eKeyframeTypeFree);
        EXPECT_NEAR(quadrature, exact, 1e-5 * (t2 - t1) * (1. + std::fabs(vmax))) << "segment " << i;
    }
}
