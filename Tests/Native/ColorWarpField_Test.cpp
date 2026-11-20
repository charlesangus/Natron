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
        full.prepare({ makePin(source, target, 1., reach) }, combo.space, basis, combo.kernel);
        double out[3];
        full.apply(source, out);
        expectNear3(out, target, 1e-9);

        ColorWarpField half;
        half.prepare({ makePin(source, target, 0.5, reach) }, combo.space, basis, combo.kernel);
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
        field.prepare({ makePin(source, target, 1., reach) }, space, basis, eColorWarpKernelGaussian);

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
        field.prepare({ makePin(skin, warmer, 1., reach), makePin(grey, grey, 1., reach) }, combo.space, basis, combo.kernel);
        double out[3];
        field.apply(grey, out);
        expectNear3(out, grey, 1e-5);
        field.apply(skin, out);
        expectNear3(out, warmer, 1e-5);

        ColorWarpField moving;
        moving.prepare({ makePin(skin, warmer, 1., reach) }, combo.space, basis, combo.kernel);
        double unheld[3];
        moving.apply(grey, unheld);
        EXPECT_GT(std::fabs(unheld[0] - grey[0]) + std::fabs(unheld[1] - grey[1]) + std::fabs(unheld[2] - grey[2]), 1e-6);

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
        averaged.prepare({ makePin(source, a, 1., reach), makePin(source, b, 1., reach), makePin(source, b, 1., reach) },
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
        held.prepare({ makePin(source, source, 1., reach), makePin(source, a, 1., reach), makePin(source, b, 1., reach) },
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
            makePin(p0, t0, 1., reach), makePin(p1, t1, 1., reach), makePin(p2, p2, 1., reach), makePin(p3, p3, 1., reach)
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
        field.prepare({ makePin(source, target, 1., -1.), makePin(source, target, 1., reach), makePin(source, target, 1., 0.) },
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
