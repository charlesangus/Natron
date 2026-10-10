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

#include "ColorWheelMath.h"

NATRON_NAMESPACE_ENTER

namespace {
// Pure R, G, B sit at hue 0/120/240 degrees here whatever the working space, because F does
// not depend on w; only E (built below from the basis's own w) carries the working space.
const double kF[2][3] = {
    { 1.0, -0.5, -0.5 },
    { 0.0, 0.8660254037844386, -0.8660254037844386 }
};

const double kFreezeThreshold = 1e-6;

void
normalizeWeights(const double weights[3],
                 double out[3])
{
    double sum = weights[0] + weights[1] + weights[2];

    if (sum == 0.0) {
        out[0] = out[1] = out[2] = 0.0;

        return;
    }
    out[0] = weights[0] / sum;
    out[1] = weights[1] / sum;
    out[2] = weights[2] / sum;
}

void
computeE(const double w[3],
         double E[3][2])
{
    // E = (2/3)(I - 1.w^T) F^T, written component-wise: E[i][j] = (2/3)(F[j][i] - w.F[j]).
    for (int j = 0; j < 2; ++j) {
        double dot = w[0] * kF[j][0] + w[1] * kF[j][1] + w[2] * kF[j][2];
        for (int i = 0; i < 3; ++i) {
            E[i][j] = (2.0 / 3.0) * (kF[j][i] - dot);
        }
    }
}

void
copyF(double F[2][3])
{
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 3; ++i) {
            F[j][i] = kF[j][i];
        }
    }
}

void
zeroMprime(double M[3][3])
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            M[i][j] = 0.0;
        }
    }
}
} // namespace

LumaBasis::LumaBasis(const double weights[3])
    : hasMatrix(false)
{
    normalizeWeights(weights, w);
    copyF(F);
    computeE(w, E);
    zeroMprime(Mprime);
}

LumaBasis::LumaBasis(const double weights[3],
                     const double workingToXYZ[9])
    : hasMatrix(false)
{
    normalizeWeights(weights, w);
    copyF(F);
    computeE(w, E);
    zeroMprime(Mprime);

    // Rows are X, Y, Z over r, g, b; the Y row's own sum is exactly the scale that turns it
    // into w (that is how w is derived from it in the first place), so one scalar on the
    // whole matrix is enough to line up Mprime's Y row with w, with xy left unchanged.
    double yRowSum = workingToXYZ[3] + workingToXYZ[4] + workingToXYZ[5];

    if (yRowSum == 0.0) {
        return;
    }

    double k = 1.0 / yRowSum;

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            Mprime[i][j] = workingToXYZ[3 * i + j] * k;
        }
    }
    hasMatrix = true;
}

bool
isRatioRole(ColorKnobRoleEnum role)
{
    return role != eColorKnobRoleOffset;
}

bool
isFrozen(ColorKnobRoleEnum role,
         double Y)
{
    return isRatioRole(role) && Y <= kFreezeThreshold;
}

double
luma(const LumaBasis& basis,
     const double rgb[3])
{
    return basis.w[0] * rgb[0] + basis.w[1] * rgb[1] + basis.w[2] * rgb[2];
}

LumaChroma
split(const LumaBasis& basis,
      const double rgb[3])
{
    LumaChroma result;

    result.Y = luma(basis, rgb);
    result.a = basis.F[0][0] * rgb[0] + basis.F[0][1] * rgb[1] + basis.F[0][2] * rgb[2];
    result.b = basis.F[1][0] * rgb[0] + basis.F[1][1] * rgb[1] + basis.F[1][2] * rgb[2];

    return result;
}

void
join(const LumaBasis& basis,
     double Y,
     double a,
     double b,
     double rgb[3])
{
    for (int i = 0; i < 3; ++i) {
        rgb[i] = Y + basis.E[i][0] * a + basis.E[i][1] * b;
    }
}

static KnobColorValue
applyValueEdit(ColorKnobRoleEnum role,
               const LumaBasis& basis,
               const KnobColorValue& v,
               double Y1)
{
    KnobColorValue out = v;
    double Y0 = luma(basis, v.rgb);

    if (isRatioRole(role) && !isFrozen(role, Y0)) {
        double factor = Y1 / Y0;
        for (int i = 0; i < 3; ++i) {
            out.rgb[i] = v.rgb[i] * factor;
        }
    } else {
        double delta = Y1 - Y0;
        for (int i = 0; i < 3; ++i) {
            out.rgb[i] = v.rgb[i] + delta;
        }
    }

    if (out.hasAlpha) {
        if (isRatioRole(role) && !isFrozen(role, Y0) && !isFrozen(role, Y1)) {
            out.a = v.a * (Y1 / Y0);
        } else {
            out.a = v.a + (Y1 - Y0);
        }
    }

    return out;
}

KnobColorValue
scaleValue(ColorKnobRoleEnum role,
           const LumaBasis& basis,
           const KnobColorValue& v,
           double factor)
{
    double Y0 = luma(basis, v.rgb);

    return applyValueEdit(role, basis, v, Y0 * factor);
}

KnobColorValue
addValue(ColorKnobRoleEnum role,
         const LumaBasis& basis,
         const KnobColorValue& v,
         double delta)
{
    double Y0 = luma(basis, v.rgb);

    return applyValueEdit(role, basis, v, Y0 + delta);
}

KnobColorValue
setValue(ColorKnobRoleEnum role,
         const LumaBasis& basis,
         const KnobColorValue& v,
         double Y)
{
    return applyValueEdit(role, basis, v, Y);
}

std::optional<double>
resetValue(ColorKnobRoleEnum role)
{
    switch (role) {
    case eColorKnobRoleGain:
    case eColorKnobRolePower:
        return 1.0;
    case eColorKnobRoleOffset:
        return 0.0;
    case eColorKnobRoleColour:
    case eColorKnobRoleAuto:
        break;
    }

    return std::nullopt;
}

NATRON_NAMESPACE_EXIT
