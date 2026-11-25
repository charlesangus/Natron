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

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/Nodes/Color/ColorWarp/ColorWarpField.h"

NATRON_NAMESPACE_USING

namespace {
const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

// A reach of roughly one neighbourhood in each space's own units.
double
reachFor(ColorWarpSpaceEnum space)
{
    switch (space) {
    case eColorWarpSpaceOklabLog:
    case eColorWarpSpaceLogRgb:
        return 1.5;
    case eColorWarpSpaceOklab:
        return 0.2;
    case eColorWarpSpaceXyz:
    case eColorWarpSpaceLinearRgb:
        break;
    }

    return 0.25;
}

// The pin falloff that reaches warpReach in space's own units.
double
pinFalloff(ColorWarpSpaceEnum space, double warpReach)
{
    return warpReach / colorWarpFalloffScale(space);
}

ColorWarpPin
makePin(const double source[3], const double target[3], double strength, double falloff)
{
    ColorWarpPin pin;
    for (int c = 0; c < 3; ++c) {
        pin.source[c] = source[c];
        pin.target[c] = target[c];
    }
    pin.strength = strength;
    pin.falloff = falloff;

    return pin;
}

void
expectNear3(const double* a, const double* b, double tol)
{
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(a[c], b[c], tol * std::max(1., std::fabs(b[c]))) << "channel " << c;
    }
}

void
expectBitExact(const double* a, const double* b)
{
    EXPECT_EQ(std::memcmp(a, b, 3 * sizeof(double)), 0)
        << a[0] << " " << a[1] << " " << a[2] << " vs " << b[0] << " " << b[1] << " " << b[2];
}

double
norm3(const double* v)
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

double
displacementNorm(const ColorWarpField& field, const double rgb[3])
{
    double w[3], d[3];
    toWarpSpace(field.space(), field.basis(), rgb, w);
    field.displacementAt(w, d);

    return norm3(d);
}

// The RBF kernels extrapolate their affine part, so far from the pins a displacement may grow
// linearly with the distance, scaled by the largest pin displacement over the pins' spread.
double
linearGrowthBound(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, const std::vector<ColorWarpPin>& pins, const double rgb[3])
{
    double anchor[3], probe[3];
    toWarpSpace(space, basis, pins.front().source, anchor);
    toWarpSpace(space, basis, rgb, probe);
    double maxDisplacement = 0., spread = 0.;
    for (const ColorWarpPin& pin : pins) {
        double s[3], t[3], d[3], fromAnchor[3];
        toWarpSpace(space, basis, pin.source, s);
        toWarpSpace(space, basis, pin.target, t);
        for (int c = 0; c < 3; ++c) {
            d[c] = pin.strength * (t[c] - s[c]);
            fromAnchor[c] = s[c] - anchor[c];
        }
        maxDisplacement = std::max(maxDisplacement, norm3(d));
        spread = std::max(spread, norm3(fromAnchor));
    }
    double fromAnchor[3];
    for (int c = 0; c < 3; ++c) {
        fromAnchor[c] = probe[c] - anchor[c];
    }

    return 4. * maxDisplacement * (1. + norm3(fromAnchor) / spread);
}

struct Combo {
    ColorWarpSpaceEnum space;
    ColorWarpKernelEnum kernel;
};

std::vector<Combo>
allCombos()
{
    std::vector<Combo> combos;
    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        for (int k = 0; k < kColorWarpKernelCount; ++k) {
            combos.push_back({ static_cast<ColorWarpSpaceEnum>(s), static_cast<ColorWarpKernelEnum>(k) });
        }
    }

    return combos;
}

std::string
comboName(const Combo& combo)
{
    return std::string(colorWarpSpaceName(combo.space)) + "/" + colorWarpKernelName(combo.kernel);
}
} // anon namespace

TEST(ColorWarpField, NoPinsOrAllDisabledIsBitExactIdentity)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double samples[][3] = { { 0.3, 0.2, 0.1 }, { -0.05, 0.5, 64. }, { 0., 0., 0. } };
    const double src[3] = { 0.3, 0.2, 0.1 };
    const double dst[3] = { 0.1, 0.4, 0.1 };
    ColorWarpPin disabled = makePin(src, dst, 1., 1.);
    disabled.enabled = false;

    for (const Combo& combo : allCombos()) {
        SCOPED_TRACE(comboName(combo));
        for (const std::vector<ColorWarpPin>& pins : { std::vector<ColorWarpPin>(), std::vector<ColorWarpPin> { disabled } }) {
            ColorWarpField field;
            field.prepare(pins, combo.space, basis, combo.kernel);
            EXPECT_TRUE(field.isIdentity());
            for (const double* rgb : samples) {
                double out[3];
                field.apply(rgb, out);
                expectBitExact(out, rgb);
            }
        }
    }
}

TEST(ColorWarpField, OnlyHoldPinsIsIdentity)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double grey[3] = { 0.18, 0.18, 0.18 };
    const double red[3] = { 0.5, 0.1, 0.1 };
    const double green[3] = { 0.1, 0.4, 0.1 };
    const std::vector<ColorWarpPin> pins = { makePin(grey, grey, 1., 1.), makePin(red, green, 0., 1.) };

    for (const Combo& combo : allCombos()) {
        ColorWarpField field;
        field.prepare(pins, combo.space, basis, combo.kernel);
        EXPECT_TRUE(field.isIdentity()) << comboName(combo);
    }
}

TEST(ColorWarpField, LonePinMapsSourceToTargetAndHalfWayAtHalfStrength)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.4, 0.12, 0.06 };
    const double target[3] = { 0.35, 0.3, 0.05 };

    for (const Combo& combo : allCombos()) {
        SCOPED_TRACE(comboName(combo));
        const double reach = reachFor(combo.space);
        ColorWarpField full;
        full.prepare({ makePin(source, target, 1., pinFalloff(combo.space, reach)) }, combo.space, basis, combo.kernel);
        double out[3];
        full.apply(source, out);
        expectNear3(out, target, 1e-9);

        ColorWarpField half;
        half.prepare({ makePin(source, target, 0.5, pinFalloff(combo.space, reach)) }, combo.space, basis, combo.kernel);
        double s[3], t[3], mid[3], expected[3];
        toWarpSpace(combo.space, basis, source, s);
        toWarpSpace(combo.space, basis, target, t);
        for (int c = 0; c < 3; ++c) {
            mid[c] = s[c] + 0.5 * (t[c] - s[c]);
        }
        fromWarpSpace(combo.space, basis, mid, expected);
        half.apply(source, out);
        expectNear3(out, expected, 1e-9);
    }
}

TEST(ColorWarpField, GaussianLeavesColoursBeyondTheFalloffUnchanged)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.4, 0.12, 0.06 };
    const double target[3] = { 0.35, 0.3, 0.05 };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        SCOPED_TRACE(colorWarpSpaceName(space));
        const double reach = reachFor(space) * 0.25;
        ColorWarpField field;
        field.prepare({ makePin(source, target, 1., pinFalloff(space, reach)) }, space, basis, eColorWarpKernelGaussian);

        double ws[3];
        toWarpSpace(space, basis, source, ws);
        const double far = reach * (kColorWarpGaussianCutoff + 1.);
        const double offsets[][3] = { { far, 0., 0. }, { 0., -far, 0. }, { 0., 0., far } };
        for (const double* o : offsets) {
            double w[3], rgb[3], out[3];
            for (int c = 0; c < 3; ++c) {
                w[c] = ws[c] + o[c];
            }
            fromWarpSpace(space, basis, w, rgb);
            field.apply(rgb, out);
            expectBitExact(out, rgb);
        }

        double nearRgb[3], out[3];
        double w[3] = { ws[0] + reach * 0.5, ws[1], ws[2] };
        fromWarpSpace(space, basis, w, nearRgb);
        field.apply(nearRgb, out);
        EXPECT_GT(std::fabs(out[0] - nearRgb[0]) + std::fabs(out[1] - nearRgb[1]) + std::fabs(out[2] - nearRgb[2]), 1e-6);
    }
}

TEST(ColorWarpField, HoldPinStaysFixedWhileANeighbourMoves)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double skin[3] = { 0.54, 0.31, 0.22 };
    const double warmer[3] = { 0.58, 0.30, 0.17 };
    const double grey[3] = { 0.30, 0.30, 0.30 };

    for (const Combo& combo : allCombos()) {
        SCOPED_TRACE(comboName(combo));
        const double reach = reachFor(combo.space);
        ColorWarpField field;
        field.prepare({ makePin(skin, warmer, 1., pinFalloff(combo.space, reach)), makePin(grey, grey, 1., pinFalloff(combo.space, reach)) }, combo.space, basis, combo.kernel);
        double out[3];
        field.apply(grey, out);
        expectNear3(out, grey, 1e-5);
        field.apply(skin, out);
        expectNear3(out, warmer, 1e-5);

        ColorWarpField moving;
        moving.prepare({ makePin(skin, warmer, 1., pinFalloff(combo.space, reach)) }, combo.space, basis, combo.kernel);
        double unheld[3];
        const double* probe = grey;
        double nearGrey[3];
        if (combo.kernel != eColorWarpKernelGaussian) {
            double ws[3], wg[3], wn[3];
            toWarpSpace(combo.space, basis, skin, ws);
            toWarpSpace(combo.space, basis, grey, wg);
            for (int c = 0; c < 3; ++c) {
                wn[c] = ws[c] + 0.05 * (wg[c] - ws[c]);
            }
            fromWarpSpace(combo.space, basis, wn, nearGrey);
            probe = nearGrey;
        }
        moving.apply(probe, unheld);
        EXPECT_GT(std::fabs(unheld[0] - probe[0]) + std::fabs(unheld[1] - probe[1]) + std::fabs(unheld[2] - probe[2]), 1e-6);

        if (combo.kernel == eColorWarpKernelGaussian) {
            double ws[3], wg[3], wm[3], between[3];
            toWarpSpace(combo.space, basis, skin, ws);
            toWarpSpace(combo.space, basis, grey, wg);
            for (int c = 0; c < 3; ++c) {
                wm[c] = 0.5 * (ws[c] + wg[c]);
            }
            fromWarpSpace(combo.space, basis, wm, between);
            const double heldShift = displacementNorm(field, between);
            const double unheldShift = displacementNorm(moving, between);
            EXPECT_GT(unheldShift, 0.);
            EXPECT_LT(heldShift, unheldShift);
        }
    }
}

TEST(ColorWarpField, CoincidentPinsAverageAndAMovedPinBeatsAHoldPin)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.2, 0.3, 0.4 };
    const double a[3] = { 0.3, 0.3, 0.4 };
    const double b[3] = { 0.2, 0.3, 0.2 };
    const double probe[3] = { 0.25, 0.28, 0.35 };

    for (const Combo& combo : allCombos()) {
        SCOPED_TRACE(comboName(combo));
        const double reach = reachFor(combo.space);
        double s[3], ta[3], tb[3], mean[3], expected[3];
        toWarpSpace(combo.space, basis, source, s);
        toWarpSpace(combo.space, basis, a, ta);
        toWarpSpace(combo.space, basis, b, tb);
        for (int c = 0; c < 3; ++c) {
            mean[c] = s[c] + 0.5 * ((ta[c] - s[c]) + (tb[c] - s[c]));
        }
        fromWarpSpace(combo.space, basis, mean, expected);

        ColorWarpField averaged;
        averaged.prepare({ makePin(source, a, 1., pinFalloff(combo.space, reach)), makePin(source, b, 1., pinFalloff(combo.space, reach)), makePin(source, b, 1., pinFalloff(combo.space, reach)) },
                         combo.space, basis, combo.kernel);
        double out[3];
        averaged.apply(source, out);
        double twoToOne[3];
        for (int c = 0; c < 3; ++c) {
            twoToOne[c] = s[c] + ((ta[c] - s[c]) + 2. * (tb[c] - s[c])) / 3.;
        }
        double expected3[3];
        fromWarpSpace(combo.space, basis, twoToOne, expected3);
        expectNear3(out, expected3, 1e-9);
        averaged.apply(probe, out);
        for (int c = 0; c < 3; ++c) {
            EXPECT_TRUE(std::isfinite(out[c]));
        }

        ColorWarpField held;
        held.prepare({ makePin(source, source, 1., pinFalloff(combo.space, reach)), makePin(source, a, 1., pinFalloff(combo.space, reach)), makePin(source, b, 1., pinFalloff(combo.space, reach)) },
                     combo.space, basis, combo.kernel);
        held.apply(source, out);
        expectNear3(out, expected, 1e-9);
    }
}

TEST(ColorWarpField, NearCoincidentAndCoplanarPinsStayFinite)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double p0[3] = { 0.2, 0.3, 0.4 };
    const double p1[3] = { 0.2 + 1e-9, 0.3, 0.4 };
    const double p2[3] = { 0.4, 0.3, 0.4 };
    const double p3[3] = { 0.3, 0.5, 0.4 };
    const double t0[3] = { 0.5, 0.3, 0.4 };
    const double t1[3] = { 0.2, 0.3, 0.1 };
    const double farBright[3] = { 64., 64., 64. };
    const double farNegative[3] = { -0.05, -0.05, -0.05 };

    for (const Combo& combo : allCombos()) {
        SCOPED_TRACE(comboName(combo));
        const double reach = reachFor(combo.space);
        const std::vector<ColorWarpPin> pins = {
            makePin(p0, t0, 1., pinFalloff(combo.space, reach)), makePin(p1, t1, 1., pinFalloff(combo.space, reach)), makePin(p2, p2, 1., pinFalloff(combo.space, reach)), makePin(p3, p3, 1., pinFalloff(combo.space, reach))
        };
        ColorWarpField field;
        field.prepare(pins, combo.space, basis, combo.kernel);
        EXPECT_FALSE(field.isIdentity());
        for (const double* rgb : { p0, p2, p3, t0, t1, farBright, farNegative }) {
            double out[3];
            field.apply(rgb, out);
            for (int c = 0; c < 3; ++c) {
                EXPECT_TRUE(std::isfinite(out[c]));
            }
        }
        for (const double* rgb : { farBright, farNegative }) {
            EXPECT_LE(displacementNorm(field, rgb), linearGrowthBound(combo.space, basis, pins, rgb)) << rgb[0];
        }
        double out[3];
        field.apply(p2, out);
        expectNear3(out, p2, 1e-4);
        field.apply(p3, out);
        expectNear3(out, p3, 1e-4);
    }
}

TEST(ColorWarpField, RbfStaysBoundedOffAlmostCollinearGreyPins)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    std::vector<ColorWarpPin> pins;
    const double greys[][2] = { { 0.002, 0.003 }, { 0.005, 0.005 }, { 0.02, 0.025 }, { 0.18, 0.2 }, { 0.9, 0.9 } };
    for (const double* g : greys) {
        const double source[3] = { g[0], g[0], g[0] };
        const double target[3] = { g[1], g[1], g[1] };
        pins.push_back(makePin(source, target, 1., 1.));
    }
    const double probes[][3] = { { 0.5, 0.1, 0.05 }, { 0.05, 0.3, 0.6 }, { 0.004, 0.001, 0.006 }, { 1., 0., 0. } };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        for (ColorWarpKernelEnum kernel : { eColorWarpKernelRbfLinear, eColorWarpKernelRbfCubic }) {
            const Combo combo = { static_cast<ColorWarpSpaceEnum>(s), kernel };
            SCOPED_TRACE(comboName(combo));
            ColorWarpField field;
            field.prepare(pins, combo.space, basis, kernel);
            for (const double* rgb : probes) {
                double out[3];
                field.apply(rgb, out);
                for (int c = 0; c < 3; ++c) {
                    EXPECT_TRUE(std::isfinite(out[c]));
                }
                EXPECT_LE(displacementNorm(field, rgb), linearGrowthBound(combo.space, basis, pins, rgb))
                    << rgb[0] << " " << rgb[1] << " " << rgb[2];
            }
        }
    }
}

TEST(ColorWarpField, GaussianPinsWithoutPositiveFalloffMoveOnlyTheirSource)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double sourceA[3] = { 0.4, 0.12, 0.06 };
    const double targetA[3] = { 0.35, 0.3, 0.05 };
    const double sourceB[3] = { 0.05, 0.12, 0.35 };
    const double targetB[3] = { 0.03, 0.18, 0.30 };
    const double off[][3] = { { 0.4, 0.12, 0.0601 }, { 0.38, 0.14, 0.06 }, { 0.05, 0.12, 0.36 }, { 0.18, 0.18, 0.18 }, { -0.05, 0.3, 64. } };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        SCOPED_TRACE(colorWarpSpaceName(space));
        ColorWarpField field;
        field.prepare({ makePin(sourceA, targetA, 1., 0.), makePin(sourceB, targetB, 1., -1.) }, space, basis, eColorWarpKernelGaussian);
        EXPECT_FALSE(field.isIdentity());
        double out[3];
        field.apply(sourceA, out);
        expectNear3(out, targetA, 1e-9);
        field.apply(sourceB, out);
        expectNear3(out, targetB, 1e-9);
        for (const double* rgb : off) {
            field.apply(rgb, out);
            for (int c = 0; c < 3; ++c) {
                EXPECT_TRUE(std::isfinite(out[c]));
            }
            expectBitExact(out, rgb);
        }
    }
}

TEST(ColorWarpField, CoincidentPinsMergeOnlyTheirPositiveFalloffs)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.4, 0.12, 0.06 };
    const double target[3] = { 0.35, 0.3, 0.05 };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        SCOPED_TRACE(colorWarpSpaceName(space));
        const double reach = reachFor(space);
        ColorWarpField field;
        field.prepare({ makePin(source, target, 1., -1.), makePin(source, target, 1., pinFalloff(space, reach)), makePin(source, target, 1., 0.) },
                      space, basis, eColorWarpKernelGaussian);
        ASSERT_TRUE(field.pinCount() == 1);
        EXPECT_NEAR(field.invTwoSigma2()[0], 2. / (reach * reach), 1e-12 / (reach * reach));

        ColorWarpField none;
        none.prepare({ makePin(source, target, 1., -1.), makePin(source, target, 1., 0.) }, space, basis, eColorWarpKernelGaussian);
        ASSERT_TRUE(none.pinCount() == 1);
        EXPECT_EQ(none.invTwoSigma2()[0], 0.);
    }
}

TEST(ColorWarpField, RbfIsExactThroughEveryPinOfAFivePinLook)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double sources[5][3] = {
        { 0.54, 0.31, 0.22 }, { 0.05, 0.12, 0.35 }, { 0.10, 0.30, 0.06 }, { 0.18, 0.18, 0.18 }, { 0.80, 0.78, 0.75 }
    };
    const double targets[5][3] = {
        { 0.58, 0.30, 0.17 }, { 0.03, 0.18, 0.30 }, { 0.16, 0.30, 0.05 }, { 0.18, 0.18, 0.18 }, { 0.80, 0.78, 0.75 }
    };
    std::vector<ColorWarpPin> pins;
    for (int i = 0; i < 5; ++i) {
        pins.push_back(makePin(sources[i], targets[i], 1., 1.));
    }

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        for (ColorWarpKernelEnum kernel : { eColorWarpKernelRbfLinear, eColorWarpKernelRbfCubic }) {
            const Combo combo = { static_cast<ColorWarpSpaceEnum>(s), kernel };
            SCOPED_TRACE(comboName(combo));
            ColorWarpField field;
            field.prepare(pins, combo.space, basis, kernel, 0.);
            for (int i = 0; i < 5; ++i) {
                double out[3];
                field.apply(sources[i], out);
                expectNear3(out, targets[i], 1e-7);
            }
        }
    }
}

TEST(ColorWarpField, FadeIsOneInsideTheRadiusAndZeroOneWidthBeyond)
{
    EXPECT_EQ(colorWarpFade(0., 1., 2.), 1.);
    EXPECT_EQ(colorWarpFade(1., 1., 2.), 1.);
    EXPECT_EQ(colorWarpFade(1.5, 1., 2.), 0.);
    EXPECT_EQ(colorWarpFade(7., 1., 2.), 0.);
    EXPECT_EQ(colorWarpFade(INFINITY, 1., 2.), 0.);
    EXPECT_NEAR(colorWarpFade(1.25, 1., 2.), 0.5, 1e-15);
    EXPECT_EQ(colorWarpFade(0., 0., 0.), 1.);
    EXPECT_EQ(colorWarpFade(1e-300, 0., 0.), 0.);
    double previous = 1.;
    for (int i = 0; i <= 100; ++i) {
        const double fade = colorWarpFade(1. + i * 0.005, 1., 2.);
        EXPECT_LE(fade, previous) << i;
        previous = fade;
    }
}

TEST(ColorWarpField, RbfFadesToIdentityFarFromThePinsAndStaysExactAtThem)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double lone[3] = { 0.40, 0.10, 0.06 };
    const double loneTarget[3] = { 0.40, 0.29, 0.06 };
    const double sources[5][3] = {
        { 0.54, 0.31, 0.22 }, { 0.05, 0.12, 0.35 }, { 0.10, 0.30, 0.06 }, { 0.18, 0.18, 0.18 }, { 0.80, 0.78, 0.75 }
    };
    const double targets[5][3] = {
        { 0.58, 0.30, 0.17 }, { 0.03, 0.18, 0.30 }, { 0.16, 0.30, 0.05 }, { 0.18, 0.18, 0.18 }, { 0.80, 0.78, 0.75 }
    };
    struct PinSet {
        const char* name;
        std::vector<ColorWarpPin> pins;
        std::vector<const double*> far;
    };
    const double negative[3] = { -0.5, -0.5, -0.5 };
    const double bright[3] = { 25., 25., 25. };
    const double huge[3] = { 1e4, 1e4, 1e4 };
    const double mixed[3] = { 1e4, 25., 0.18 };
    std::vector<PinSet> sets;
    sets.push_back({ "lone", { makePin(lone, loneTarget, 1., 1.) }, { negative, bright, huge, mixed } });
    sets.push_back({ "identical", { makePin(lone, loneTarget, 1., 1.), makePin(lone, loneTarget, 1., 1.), makePin(lone, loneTarget, 1., 1.) }, { negative, bright, huge, mixed } });
    PinSet look = { "look", {}, { bright, huge, mixed } };
    for (int i = 0; i < 5; ++i) {
        look.pins.push_back(makePin(sources[i], targets[i], 1., 1.));
    }
    sets.push_back(look);

    for (const PinSet& set : sets) {
        for (int s = 0; s < kColorWarpSpaceCount; ++s) {
            for (ColorWarpKernelEnum kernel : { eColorWarpKernelRbfLinear, eColorWarpKernelRbfCubic }) {
                const Combo combo = { static_cast<ColorWarpSpaceEnum>(s), kernel };
                SCOPED_TRACE(std::string(set.name) + " " + comboName(combo));
                ColorWarpField field;
                field.prepare(set.pins, combo.space, basis, kernel, 0.);
                ASSERT_FALSE(field.isIdentity());
                EXPECT_GT(field.fadeInvWidth(), 0.);
                for (const double* rgb : set.far) {
                    double out[3];
                    field.apply(rgb, out);
                    expectNear3(out, rgb, 1e-9);
                }
                for (const ColorWarpPin& pin : set.pins) {
                    double out[3];
                    field.apply(pin.source, out);
                    expectNear3(out, pin.target, 1e-7);
                }
            }
        }
    }
}

TEST(ColorWarpField, RbfPinsOnTheirTargetsStayAnExactIdentityFarOut)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double grey[3] = { 0.18, 0.18, 0.18 };
    const double red[3] = { 0.5, 0.1, 0.1 };
    const double far[][3] = { { -0.5, -0.5, -0.5 }, { 25., 25., 25. }, { 1e4, 1e4, 1e4 }, { 0.3, 0.2, 0.1 } };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        for (ColorWarpKernelEnum kernel : { eColorWarpKernelRbfLinear, eColorWarpKernelRbfCubic }) {
            const Combo combo = { static_cast<ColorWarpSpaceEnum>(s), kernel };
            SCOPED_TRACE(comboName(combo));
            ColorWarpField field;
            field.prepare({ makePin(grey, grey, 1., 1.), makePin(red, red, 1., 1.) }, combo.space, basis, kernel);
            EXPECT_TRUE(field.isIdentity());
            for (const double* rgb : far) {
                double out[3];
                field.apply(rgb, out);
                expectBitExact(out, rgb);
            }
        }
    }
}

TEST(ColorWarpField, NonFiniteInputPassesThrough)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.4, 0.12, 0.06 };
    const double target[3] = { 0.35, 0.3, 0.05 };
    ColorWarpField field;
    field.prepare({ makePin(source, target, 1., 1.) }, eColorWarpSpaceOklabLog, basis, eColorWarpKernelRbfCubic);
    const double bad[3] = { NAN, 0.2, INFINITY };
    double out[3];
    field.apply(bad, out);
    EXPECT_TRUE(std::isnan(out[0]));
    EXPECT_EQ(out[1], 0.2);
    EXPECT_TRUE(std::isinf(out[2]));
}

TEST(ColorWarpField, LightnessFalloffStretchesReachAlongTheNeutralAxisOnly)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.4, 0.12, 0.06 };
    const double target[3] = { 0.35, 0.3, 0.05 };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        SCOPED_TRACE(colorWarpSpaceName(space));
        const double reach = reachFor(space) * 0.25;
        double axis[3];
        colorWarpLightnessAxis(space, basis, axis);
        EXPECT_NEAR(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2], 1., 1e-12);

        double across[3] = { 1., 0., 0. };
        const double along0 = across[0] * axis[0];
        for (int c = 0; c < 3; ++c) {
            across[c] -= along0 * axis[c];
        }
        if ((across[0] * across[0] + across[1] * across[1] + across[2] * across[2]) < 1e-6) {
            across[0] = 0.;
            across[1] = 1.;
            across[2] = 0.;
            const double along1 = axis[1];
            for (int c = 0; c < 3; ++c) {
                across[c] -= along1 * axis[c];
            }
        }
        const double acrossLen = std::sqrt(across[0] * across[0] + across[1] * across[1] + across[2] * across[2]);

        double ws[3];
        toWarpSpace(space, basis, source, ws);
        const double far = reach * (kColorWarpGaussianCutoff + 1.);
        double alongRgb[3], acrossRgb[3];
        {
            double w[3];
            for (int c = 0; c < 3; ++c) {
                w[c] = ws[c] + far * axis[c];
            }
            fromWarpSpace(space, basis, w, alongRgb);
            for (int c = 0; c < 3; ++c) {
                w[c] = ws[c] + far * across[c] / acrossLen;
            }
            fromWarpSpace(space, basis, w, acrossRgb);
        }

        ColorWarpField round, stretched;
        round.prepare({ makePin(source, target, 1., pinFalloff(space, reach)) }, space, basis, eColorWarpKernelGaussian);
        stretched.prepare({ makePin(source, target, 1., pinFalloff(space, reach)) }, space, basis, eColorWarpKernelGaussian, kColorWarpDefaultRegularisation, 4.);

        double out[3];
        round.apply(alongRgb, out);
        expectBitExact(out, alongRgb);
        stretched.apply(alongRgb, out);
        EXPECT_GT(std::fabs(out[0] - alongRgb[0]) + std::fabs(out[1] - alongRgb[1]) + std::fabs(out[2] - alongRgb[2]), 1e-6);
        stretched.apply(acrossRgb, out);
        expectBitExact(out, acrossRgb);

        stretched.apply(source, out);
        expectNear3(out, target, 1e-7);
    }
}

TEST(ColorWarpField, DefaultLightnessFalloffMatchesARoundReach)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double source[3] = { 0.4, 0.12, 0.06 };
    const double target[3] = { 0.35, 0.3, 0.05 };
    const double probe[3] = { 0.5, 0.2, 0.1 };
    const double badFalloffs[] = { 0., -2., NAN, INFINITY };

    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        SCOPED_TRACE(colorWarpSpaceName(space));
        ColorWarpField field;
        field.prepare({ makePin(source, target, 1., pinFalloff(space, reachFor(space))) }, space, basis, eColorWarpKernelGaussian);
        EXPECT_EQ(field.lightnessStretch(), 0.);
        double ref[3];
        field.apply(probe, ref);
        for (double bad : badFalloffs) {
            ColorWarpField other;
            other.prepare({ makePin(source, target, 1., pinFalloff(space, reachFor(space))) }, space, basis, eColorWarpKernelGaussian, kColorWarpDefaultRegularisation, bad);
            double out[3];
            other.apply(probe, out);
            expectBitExact(out, ref);
        }
    }
}

TEST(ColorWarpField, FalloffScaleIsPositivePerSpace)
{
    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        SCOPED_TRACE(colorWarpSpaceName(space));
        EXPECT_GT(colorWarpFalloffScale(space), 0.);
    }
    EXPECT_NEAR(colorWarpFalloffScale(eColorWarpSpaceOklabLog), 1., 0.1);
}

namespace {
double
srgbToLinear(double v)
{
    return (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// A coarse stand-in for the comparison harness's chart, each part weighted by its share of that
// chart's pixels: colour checker, exposure ramps over +-7 stops and a hue/saturation sweep.
struct WeightedChart {
    std::vector<double> rgb;
    std::vector<double> weight;

    void add(const double c[3], double w)
    {
        rgb.insert(rgb.end(), c, c + 3);
        weight.push_back(w);
    }
};

WeightedChart
makeWeightedChart()
{
    static const int kChecker[24][3] = {
        { 115, 82, 68 }, { 194, 150, 130 }, { 98, 122, 157 }, { 87, 108, 67 }, { 133, 128, 177 }, { 103, 189, 170 }, { 214, 126, 44 }, { 80, 91, 166 }, { 193, 90, 99 }, { 94, 60, 108 }, { 157, 188, 64 }, { 224, 163, 46 }, { 56, 61, 150 }, { 70, 148, 73 }, { 175, 54, 60 }, { 231, 199, 31 }, { 187, 86, 149 }, { 8, 133, 161 }, { 243, 243, 242 }, { 200, 200, 200 }, { 160, 160, 160 }, { 122, 122, 121 }, { 85, 85, 85 }, { 52, 52, 52 }
    };
    const double checkerShare = 192., rampShare = 256., sweepShare = 128.;
    WeightedChart chart;

    for (const int* s : kChecker) {
        const double c[3] = { srgbToLinear(s[0] / 255.), srgbToLinear(s[1] / 255.), srgbToLinear(s[2] / 255.) };
        chart.add(c, checkerShare / 24.);
    }

    std::vector<std::vector<double>> bases;
    for (int h = 0; h < 6; ++h) {
        const double hsv[3] = { 60. * h, 0.8, 1. };
        std::vector<double> c(3);
        colorWarpHsvToRgb(hsv, c.data());
        bases.push_back(c);
    }
    bases.push_back({ 0.54, 0.31, 0.22 });
    bases.push_back({ 1., 1., 1. });
    const int stops = 112;
    for (const std::vector<double>& base : bases) {
        const double lum = kRec709ToXYZ[3] * base[0] + kRec709ToXYZ[4] * base[1] + kRec709ToXYZ[5] * base[2];
        for (int i = 0; i < stops; ++i) {
            const double k = 0.18 / lum * std::exp2(-7. + 14. * (i + 0.5) / stops);
            const double c[3] = { base[0] * k, base[1] * k, base[2] * k };
            chart.add(c, rampShare / (bases.size() * stops));
        }
    }

    const int hues = 72, sats = 16;
    for (int si = 0; si < sats; ++si) {
        for (int hi = 0; hi < hues; ++hi) {
            const double hsv[3] = { 360. * (hi + 0.5) / hues, static_cast<double>(si) / (sats - 1), 0.45 };
            double c[3];
            colorWarpHsvToRgb(hsv, c);
            chart.add(c, sweepShare / (hues * sats));
        }
    }

    return chart;
}

double
changedShare(const ColorWarpField& field, const WeightedChart& chart)
{
    double changed = 0., total = 0.;

    for (std::size_t i = 0; i < chart.weight.size(); ++i) {
        const double* in = &chart.rgb[i * 3];
        double out[3];
        field.apply(in, out);
        double diff = 0., mag = 1e-6;
        for (int c = 0; c < 3; ++c) {
            diff = std::max(diff, std::fabs(out[c] - in[c]));
            mag = std::max(mag, std::fabs(in[c]));
        }
        if (diff > 0.01 * mag) {
            changed += chart.weight[i];
        }
        total += chart.weight[i];
    }

    return changed / total;
}
} // anon namespace

TEST(ColorWarpField, OneFalloffChangesAboutTheSameShareOfTheChartInEverySpace)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const WeightedChart chart = makeWeightedChart();
    const double source[3] = { 0.40, 0.10, 0.06 };
    double hsv[3], target[3];
    colorWarpRgbToHsv(source, hsv);
    hsv[0] += 35.;
    colorWarpHsvToRgb(hsv, target);

    double shares[kColorWarpSpaceCount];
    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
        ColorWarpField field;
        field.prepare({ makePin(source, target, 1., 1.) }, space, basis, eColorWarpKernelGaussian);
        shares[s] = changedShare(field, chart);
    }
    const double lo = *std::min_element(shares, shares + kColorWarpSpaceCount);
    const double hi = *std::max_element(shares, shares + kColorWarpSpaceCount);
    for (int s = 0; s < kColorWarpSpaceCount; ++s) {
        SCOPED_TRACE(colorWarpSpaceName(static_cast<ColorWarpSpaceEnum>(s)));
        EXPECT_GT(shares[s], 0.05);
        EXPECT_LT(shares[s], 0.4);
    }
    EXPECT_LT(hi, 2. * lo);
}
