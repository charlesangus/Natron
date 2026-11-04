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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace {
// Pure R, G, B sit at hue 0/120/240 degrees here whatever the working space, because F does
// not depend on w; only E (built below from the basis's own w) carries the working space.
const double kF[2][3] = {
    { 1.0, -0.5, -0.5 },
    { 0.0, 0.8660254037844386, -0.8660254037844386 }
};

const double kFreezeThreshold = 1e-6;

const double kPivotGrey = kColorPivotGrey;

// Below this distance from white a value has no meaningful hue of its own, so saturation
// edits take the caller's remembered hue instead of reading noise off a rounding residue.
const double kNeutralSaturation = 1e-12;

// The matrix-less linear plane is J = k*F. Real working spaces have a Jacobian at white of
// roughly 0.07-0.14 xy per unit of F, so this keeps the wheel's rim and rates in the same
// range whether or not the config gave us a matrix.
const double kMatrixlessPlaneScale = 0.1;

const double kDegreesPerRadian = 57.29577951308232;

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
void
chromaticityRows(ChromaticityKindEnum kind,
                 double num[2][3],
                 double den[3])
{
    for (int k = 0; k < 2; ++k) {
        for (int i = 0; i < 3; ++i) {
            num[k][i] = 0.0;
        }
    }
    if (kind == eChromaticityKindUPrimeVPrime) {
        num[0][0] = 4.0;
        num[1][1] = 9.0;
        den[0] = 1.0;
        den[1] = 15.0;
        den[2] = 3.0;
    } else {
        num[0][0] = 1.0;
        num[1][1] = 1.0;
        den[0] = den[1] = den[2] = 1.0;
    }
}

double
dot3(const double a[3],
     const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void
computeJEinv(LumaBasis& basis)
{
    double JE[2][2];

    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            JE[k][j] = basis.J[k][0] * basis.E[0][j] + basis.J[k][1] * basis.E[1][j] + basis.J[k][2] * basis.E[2][j];
        }
    }

    double det = JE[0][0] * JE[1][1] - JE[0][1] * JE[1][0];

    if (det == 0.0) {
        basis.JEinv[0][0] = basis.JEinv[0][1] = basis.JEinv[1][0] = basis.JEinv[1][1] = 0.0;

        return;
    }
    basis.JEinv[0][0] = JE[1][1] / det;
    basis.JEinv[0][1] = -JE[0][1] / det;
    basis.JEinv[1][0] = -JE[1][0] / det;
    basis.JEinv[1][1] = JE[0][0] / det;
}

void
initMatrixlessPlane(LumaBasis& basis)
{
    for (int k = 0; k < 2; ++k) {
        for (int i = 0; i < 3; ++i) {
            basis.chromaNum[k][i] = 0.0;
            basis.J[k][i] = kMatrixlessPlaneScale * basis.F[k][i];
        }
        basis.white[k] = 0.0;
    }
    basis.chromaDen[0] = basis.chromaDen[1] = basis.chromaDen[2] = 0.0;
    computeJEinv(basis);
}

void
initProjectivePlane(LumaBasis& basis,
                    ChromaticityKindEnum kind)
{
    double num[2][3];
    double den[3];

    chromaticityRows(kind, num, den);
    for (int i = 0; i < 3; ++i) {
        basis.chromaDen[i] = den[0] * basis.Mprime[0][i] + den[1] * basis.Mprime[1][i] + den[2] * basis.Mprime[2][i];
        for (int k = 0; k < 2; ++k) {
            basis.chromaNum[k][i] = num[k][0] * basis.Mprime[0][i] + num[k][1] * basis.Mprime[1][i] + num[k][2] * basis.Mprime[2][i];
        }
    }

    double denWhite = basis.chromaDen[0] + basis.chromaDen[1] + basis.chromaDen[2];

    if (denWhite <= 0.0) {
        basis.hasMatrix = false;
        zeroMprime(basis.Mprime);
        initMatrixlessPlane(basis);

        return;
    }
    for (int k = 0; k < 2; ++k) {
        basis.white[k] = (basis.chromaNum[k][0] + basis.chromaNum[k][1] + basis.chromaNum[k][2]) / denWhite;
        for (int i = 0; i < 3; ++i) {
            basis.J[k][i] = (basis.chromaNum[k][i] - basis.white[k] * basis.chromaDen[i]) / denWhite;
        }
    }
    computeJEinv(basis);
}

void initTempTintCache(LumaBasis& basis);
} // namespace

LumaBasis::LumaBasis(const double weights[3])
    : hasMatrix(false)
    , hasLocusWhite(false)
    , whiteMired(0.0)
    , whiteDuv(0.0)
    , hasTempTintAxes(false)
    , tempAxis { 0.0, 0.0 }
    , tintAxis { 0.0, 0.0 }
{
    normalizeWeights(weights, w);
    copyF(F);
    computeE(w, E);
    zeroMprime(Mprime);
    initMatrixlessPlane(*this);
}

LumaBasis::LumaBasis(const double weights[3],
                     const double workingToXYZ[9],
                     ChromaticityKindEnum kind)
    : hasMatrix(false)
    , hasLocusWhite(false)
    , whiteMired(0.0)
    , whiteDuv(0.0)
    , hasTempTintAxes(false)
    , tempAxis { 0.0, 0.0 }
    , tintAxis { 0.0, 0.0 }
{
    normalizeWeights(weights, w);
    copyF(F);
    computeE(w, E);
    zeroMprime(Mprime);
    initMatrixlessPlane(*this);

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
    initProjectivePlane(*this, kind);
    initTempTintCache(*this);
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

namespace {
bool
allDefaultsEqual(const std::vector<double>& defaults,
                 double target)
{
    if (defaults.empty()) {
        return false;
    }
    for (double d : defaults) {
        if (d != target) {
            return false;
        }
    }

    return true;
}

bool
scriptNameMentionsGamma(const std::string& scriptName)
{
    std::string lower = scriptName;

    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return lower.find("gamma") != std::string::npos;
}
} // namespace

ColorKnobRoleEnum
inferColorRole(const std::vector<double>& defaults,
               double displayMin,
               const std::string& scriptName)
{
    if (allDefaultsEqual(defaults, 0.0) && displayMin < 0.0) {
        return eColorKnobRoleOffset;
    }
    if (allDefaultsEqual(defaults, 1.0) && scriptNameMentionsGamma(scriptName)) {
        return eColorKnobRolePower;
    }
    if (allDefaultsEqual(defaults, 1.0)) {
        return eColorKnobRoleGain;
    }

    return eColorKnobRoleColour;
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
        if (isRatioRole(role) && !isFrozen(role, Y0)) {
            out.a = v.a * (Y1 / Y0);
        } else {
            out.a = v.a + (Y1 - Y0);
        }
    }

    return out;
}

PowerEffectResult
powerToEffect(double gamma)
{
    PowerEffectResult result;

    if (!(gamma > 0.0)) {
        return result;
    }
    result.effect = std::pow(kPivotGrey, 1.0 / gamma);
    result.valid = std::isfinite(result.effect);

    return result;
}

PowerGammaResult
effectToGamma(double effect)
{
    PowerGammaResult result;

    if (!(effect > 0.0) || !(effect < 1.0)) {
        return result;
    }
    result.gamma = std::log(kPivotGrey) / std::log(effect);
    result.valid = result.gamma > 0.0 && std::isfinite(result.gamma);

    return result;
}

namespace {
// Every Power-role edit is the matching Gain edit run on this effect vector: gamma has no
// meaning to the basis's luma weights on its own, but 0.18^(1/gamma) does, so edits run there
// and are mapped back afterwards. Invalid (false) on any channel whose gamma is out of
// powerToEffect's domain; the caller then leaves the value untouched.
bool
toEffectValue(const KnobColorValue& gammaValue,
              KnobColorValue& effectValue)
{
    effectValue.hasAlpha = gammaValue.hasAlpha;
    for (int i = 0; i < 3; ++i) {
        PowerEffectResult r = powerToEffect(gammaValue.rgb[i]);

        if (!r.valid) {
            return false;
        }
        effectValue.rgb[i] = r.effect;
    }
    if (gammaValue.hasAlpha) {
        PowerEffectResult r = powerToEffect(gammaValue.a);

        if (!r.valid) {
            return false;
        }
        effectValue.a = r.effect;
    } else {
        effectValue.a = 0.0;
    }

    return true;
}

// An effect at or past either end of the gamma range parks on that end, so an edit that
// brightens or darkens too far holds there instead of being refused and snapping back to the
// value it started from.
bool
cappedEffectToGamma(double effect,
                    double& gamma)
{
    if (std::isnan(effect)) {
        return false;
    }

    static const double effectAtMin = std::pow(kPivotGrey, 1.0 / kPowerGammaMin);
    static const double effectAtMax = std::pow(kPivotGrey, 1.0 / kPowerGammaMax);
    PowerGammaResult r = effectToGamma(std::min(std::max(effect, effectAtMin), effectAtMax));

    if (!r.valid) {
        return false;
    }
    gamma = std::min(std::max(r.gamma, kPowerGammaMin), kPowerGammaMax);

    return true;
}

bool
fromEffectValue(const KnobColorValue& effectValue,
                KnobColorValue& gammaValue)
{
    gammaValue.hasAlpha = effectValue.hasAlpha;
    for (int i = 0; i < 3; ++i) {
        if (!cappedEffectToGamma(effectValue.rgb[i], gammaValue.rgb[i])) {
            return false;
        }
    }
    if (effectValue.hasAlpha) {
        if (!cappedEffectToGamma(effectValue.a, gammaValue.a)) {
            return false;
        }
    } else {
        gammaValue.a = 0.0;
    }

    return true;
}

// The luma a chroma edit on v should hold fixed: v's own rgb for every role but Power, whose
// rgb is gamma and so is meaningless to basis.w directly, and the effect vector's luma otherwise
// (0.0 on an out-of-domain gamma, which the Power wrapper below then refuses on anyway).
double
heldLuma(ColorKnobRoleEnum role,
         const LumaBasis& basis,
         const KnobColorValue& v)
{
    if (role == eColorKnobRolePower) {
        KnobColorValue effect;

        if (!toEffectValue(v, effect)) {
            return 0.0;
        }

        return luma(basis, effect.rgb);
    }

    return luma(basis, v.rgb);
}

// Runs edit (one of scaleValue/addValue/setValue, already resolved to Gain) on v's effect
// vector and maps the result back to gamma, capped to the gamma range; leaves v untouched only
// when v itself has no effect vector or the edit produced a NaN.
template <typename Edit>
KnobColorValue
powerValueEdit(const LumaBasis& basis,
               const KnobColorValue& v,
               Edit edit)
{
    KnobColorValue effect;

    if (!toEffectValue(v, effect)) {
        return v;
    }

    KnobColorValue out;

    if (!fromEffectValue(edit(basis, effect), out)) {
        return v;
    }

    return out;
}
} // namespace

KnobColorValue
scaleValue(ColorKnobRoleEnum role,
           const LumaBasis& basis,
           const KnobColorValue& v,
           double factor)
{
    if (role == eColorKnobRolePower) {
        return powerValueEdit(basis, v, [factor](const LumaBasis& b, const KnobColorValue& e) {
            return scaleValue(eColorKnobRoleGain, b, e, factor);
        });
    }

    double Y0 = luma(basis, v.rgb);

    return applyValueEdit(role, basis, v, Y0 * factor);
}

KnobColorValue
addValue(ColorKnobRoleEnum role,
         const LumaBasis& basis,
         const KnobColorValue& v,
         double delta)
{
    if (role == eColorKnobRolePower) {
        return powerValueEdit(basis, v, [delta](const LumaBasis& b, const KnobColorValue& e) {
            return addValue(eColorKnobRoleGain, b, e, delta);
        });
    }

    double Y0 = luma(basis, v.rgb);

    return applyValueEdit(role, basis, v, Y0 + delta);
}

KnobColorValue
setValue(ColorKnobRoleEnum role,
         const LumaBasis& basis,
         const KnobColorValue& v,
         double Y)
{
    if (role == eColorKnobRolePower) {
        return powerValueEdit(basis, v, [Y](const LumaBasis& b, const KnobColorValue& e) {
            return setValue(eColorKnobRoleGain, b, e, Y);
        });
    }

    return applyValueEdit(role, basis, v, Y);
}

KnobColorValue
neutralGainValue(bool hasAlpha)
{
    KnobColorValue v;

    v.rgb[0] = v.rgb[1] = v.rgb[2] = 1.0;
    v.a = 1.0;
    v.hasAlpha = hasAlpha;

    return v;
}

double
valueStopPivot(ColorKnobRoleEnum role)
{
    if (role == eColorKnobRolePower) {
        return kPivotGrey;
    }

    return isRatioRole(role) ? 1.0 : 0.0;
}

std::optional<double>
resetValue(ColorKnobRoleEnum role)
{
    switch (role) {
    case eColorKnobRoleGain:
        return 1.0;
    case eColorKnobRolePower:
        return kPivotGrey;
    case eColorKnobRoleOffset:
        return 0.0;
    case eColorKnobRoleColour:
    case eColorKnobRoleAuto:
        break;
    }

    return std::nullopt;
}

namespace {
bool
usesLinearPlane(ColorKnobRoleEnum role,
                const LumaBasis& basis,
                const KnobColorValue& v)
{
    if (!basis.hasMatrix) {
        return true;
    }
    if (isRatioRole(role)) {
        return false;
    }

    double p[3] = { kPivotGrey + v.rgb[0], kPivotGrey + v.rgb[1], kPivotGrey + v.rgb[2] };

    return luma(basis, p) <= 0.0 || dot3(basis.chromaDen, p) <= 0.0;
}

void
applyJ(const LumaBasis& basis,
       const double c[3],
       double scale,
       PlanePoint& q)
{
    q.x = dot3(basis.J[0], c) * scale;
    q.y = dot3(basis.J[1], c) * scale;
}

// rgb = Y*1 + E*(J*E)^-1 * (scale*q): w.E is the zero row and w sums to 1, so the luma is Y
// exactly, and J*1 = 0 (chromaticity is scale invariant), so J*rgb/scale is q again.
void
linearInverse(const LumaBasis& basis,
              const PlanePoint& q,
              double scale,
              double Y,
              double rgb[3])
{
    double qx = q.x * scale;
    double qy = q.y * scale;
    double a = basis.JEinv[0][0] * qx + basis.JEinv[0][1] * qy;
    double b = basis.JEinv[1][0] * qx + basis.JEinv[1][1] * qy;

    join(basis, Y, a, b, rgb);
}

bool
projectiveInverse(const LumaBasis& basis,
                  const PlanePoint& q,
                  double Y,
                  double rgb[3])
{
    double u = basis.white[0] + q.x;
    double v = basis.white[1] + q.y;
    double rows[3][3];

    for (int i = 0; i < 3; ++i) {
        rows[0][i] = basis.w[i];
        rows[1][i] = basis.chromaNum[0][i] - u * basis.chromaDen[i];
        rows[2][i] = basis.chromaNum[1][i] - v * basis.chromaDen[i];
    }

    double cof0 = rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1];
    double cof1 = rows[1][2] * rows[2][0] - rows[1][0] * rows[2][2];
    double cof2 = rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0];
    double det = rows[0][0] * cof0 + rows[0][1] * cof1 + rows[0][2] * cof2;

    if (det == 0.0 || !std::isfinite(det)) {
        return false;
    }

    // The right-hand side is (Y, 0, 0), so Cramer's rule only needs the first row's cofactors.
    rgb[0] = Y * cof0 / det;
    rgb[1] = Y * cof1 / det;
    rgb[2] = Y * cof2 / det;

    if (dot3(basis.chromaDen, rgb) <= 0.0) {
        return false;
    }

    // The solve's rounding leaves w.rgb a few ulps off Y. Since w sums to 1, a shift along
    // (1, 1, 1) by the residual cancels it while moving chromaticity only by that same few
    // ulps, and luma being exact is the invariant every chroma edit relies on.
    double residual = Y - luma(basis, rgb);

    for (int i = 0; i < 3; ++i) {
        rgb[i] += residual;
    }

    return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
}

ChromaEditResult
refused(const KnobColorValue& v)
{
    ChromaEditResult result;

    result.value = v;
    result.valid = false;

    return result;
}

PlanePoint
polar(double s,
      double thetaDegrees)
{
    PlanePoint q;
    double theta = thetaDegrees / kDegreesPerRadian;

    q.x = s * std::cos(theta);
    q.y = s * std::sin(theta);
    q.valid = true;

    return q;
}

ChromaEditResult
rebuildAt(ColorKnobRoleEnum role,
          const LumaBasis& basis,
          const KnobColorValue& v,
          const PlanePoint& q)
{
    return fromPlane(role, basis, v, q, heldLuma(role, basis, v));
}
} // namespace

PlanePoint
toPlane(ColorKnobRoleEnum role,
        const LumaBasis& basis,
        const KnobColorValue& v)
{
    if (role == eColorKnobRolePower) {
        KnobColorValue effect;

        if (!toEffectValue(v, effect)) {
            return PlanePoint();
        }

        return toPlane(eColorKnobRoleGain, basis, effect);
    }

    PlanePoint q;
    double Y = luma(basis, v.rgb);

    if (isFrozen(role, Y)) {
        return q;
    }

    bool ratio = isRatioRole(role);

    if (usesLinearPlane(role, basis, v)) {
        q.linear = true;
        applyJ(basis, v.rgb, ratio ? 1.0 / Y : 1.0 / kPivotGrey, q);
        q.valid = true;

        return q;
    }

    double c[3];

    for (int i = 0; i < 3; ++i) {
        c[i] = ratio ? v.rgb[i] : kPivotGrey + v.rgb[i];
    }

    double den = dot3(basis.chromaDen, c);

    if (den <= 0.0) {
        return q;
    }
    q.x = dot3(basis.chromaNum[0], c) / den - basis.white[0];
    q.y = dot3(basis.chromaNum[1], c) / den - basis.white[1];
    q.valid = true;

    return q;
}

ChromaEditResult
fromPlane(ColorKnobRoleEnum role,
          const LumaBasis& basis,
          const KnobColorValue& v,
          const PlanePoint& q,
          double Yheld)
{
    if (role == eColorKnobRolePower) {
        KnobColorValue effect;

        if (!toEffectValue(v, effect)) {
            return refused(v);
        }

        ChromaEditResult effectResult = fromPlane(eColorKnobRoleGain, basis, effect, q, Yheld);

        if (!effectResult.valid) {
            return refused(v);
        }

        ChromaEditResult result;

        if (!fromEffectValue(effectResult.value, result.value)) {
            return refused(v);
        }
        result.valid = true;

        return result;
    }

    if (!q.valid || !std::isfinite(q.x) || !std::isfinite(q.y) || isFrozen(role, Yheld)) {
        return refused(v);
    }

    ChromaEditResult result;

    result.value = v;
    result.valid = true;

    bool ratio = isRatioRole(role);

    if (usesLinearPlane(role, basis, v)) {
        if (ratio) {
            linearInverse(basis, q, Yheld, Yheld, result.value.rgb);
        } else {
            linearInverse(basis, q, kPivotGrey, Yheld, result.value.rgb);
        }

        return result;
    }

    if (ratio) {
        if (!projectiveInverse(basis, q, Yheld, result.value.rgb)) {
            return refused(v);
        }

        return result;
    }

    double pivotY = kPivotGrey + Yheld;
    double p[3];

    if (pivotY <= 0.0 || !projectiveInverse(basis, q, pivotY, p)) {
        return refused(v);
    }

    // The pivot's own luma is exact, but subtracting 0.18 rounds again; re-centre on Yheld
    // along (1, 1, 1) so the offset's luma is exact too.
    for (int i = 0; i < 3; ++i) {
        result.value.rgb[i] = p[i] - kPivotGrey;
    }

    double residual = Yheld - luma(basis, result.value.rgb);

    for (int i = 0; i < 3; ++i) {
        result.value.rgb[i] += residual;
    }

    return result;
}

ChromaEditResult
translate(ColorKnobRoleEnum role,
          const LumaBasis& basis,
          const KnobColorValue& v,
          double dx,
          double dy)
{
    PlanePoint q = toPlane(role, basis, v);

    if (!q.valid) {
        return refused(v);
    }
    q.x += dx;
    q.y += dy;

    return rebuildAt(role, basis, v, q);
}

ChromaEditResult
rotateHueAboutWhite(ColorKnobRoleEnum role,
                    const LumaBasis& basis,
                    const KnobColorValue& v,
                    double dThetaDegrees)
{
    PlanePoint q = toPlane(role, basis, v);

    if (!q.valid) {
        return refused(v);
    }

    double theta = dThetaDegrees / kDegreesPerRadian;
    double c = std::cos(theta);
    double s = std::sin(theta);
    PlanePoint rotated = q;

    rotated.x = c * q.x - s * q.y;
    rotated.y = s * q.x + c * q.y;

    return rebuildAt(role, basis, v, rotated);
}

ChromaEditResult
setHue(ColorKnobRoleEnum role,
       const LumaBasis& basis,
       const KnobColorValue& v,
       double thetaDegrees)
{
    PlanePoint q = toPlane(role, basis, v);

    if (!q.valid) {
        return refused(v);
    }

    double s = planeSaturation(q);

    if (s <= kNeutralSaturation) {
        ChromaEditResult result;
        result.value = v;
        result.valid = true;

        return result;
    }

    return rebuildAt(role, basis, v, polar(s, thetaDegrees));
}

ChromaEditResult
setSaturation(ColorKnobRoleEnum role,
              const LumaBasis& basis,
              const KnobColorValue& v,
              double s,
              double rememberedHueDegrees)
{
    PlanePoint q = toPlane(role, basis, v);

    if (!q.valid || !std::isfinite(s)) {
        return refused(v);
    }

    double hue = planeSaturation(q) > kNeutralSaturation ? planeHue(q) : rememberedHueDegrees;

    return rebuildAt(role, basis, v, polar(std::max(0.0, s), hue));
}

ChromaEditResult
moveSaturation(ColorKnobRoleEnum role,
               const LumaBasis& basis,
               const KnobColorValue& v,
               double ds,
               double rememberedHueDegrees)
{
    PlanePoint q = toPlane(role, basis, v);

    if (!q.valid) {
        return refused(v);
    }

    return setSaturation(role, basis, v, planeSaturation(q) + ds, rememberedHueDegrees);
}

double
planeHue(const PlanePoint& q)
{
    if (q.x == 0.0 && q.y == 0.0) {
        return 0.0;
    }

    double degrees = std::atan2(q.y, q.x) * kDegreesPerRadian;

    if (degrees < 0.0) {
        degrees += 360.0;
    }

    return degrees >= 360.0 ? degrees - 360.0 : degrees;
}

double
planeSaturation(const PlanePoint& q)
{
    return std::hypot(q.x, q.y);
}

GamutTriangle
gamutTriangle(const LumaBasis& basis)
{
    GamutTriangle triangle;

    triangle.valid = basis.hasMatrix;
    for (int i = 0; i < 3; ++i) {
        PlanePoint& vertex = triangle.vertex[i];
        double den = basis.chromaDen[i];

        if (!basis.hasMatrix || den <= 0.0) {
            triangle.valid = false;
            continue;
        }
        vertex.x = basis.chromaNum[0][i] / den - basis.white[0];
        vertex.y = basis.chromaNum[1][i] / den - basis.white[1];
        vertex.valid = true;
    }

    return triangle;
}

PlanePoint
whitePoint(const PlanePoint& anchor)
{
    PlanePoint white;

    white.x = -anchor.x;
    white.y = -anchor.y;
    white.valid = anchor.valid;
    white.linear = anchor.linear;

    return white;
}

DiscClamp
clampToDisc(const PlanePoint& p,
            double rim)
{
    DiscClamp result;
    double r = std::hypot(p.x, p.y);

    result.point = p;
    result.offDisc = r > rim;
    result.direction.valid = p.valid;
    if (r > 0.0) {
        result.direction.x = p.x / r;
        result.direction.y = p.y / r;
    }
    if (result.offDisc) {
        result.point.x = result.direction.x * rim;
        result.point.y = result.direction.y * rim;
    }

    return result;
}

namespace {
// The Kim et al. cubic fit to the Planckian locus (1667-25000 K), worked in CIE 1960 uv so the
// locus's own tangent and normal give temperature and tint directly. None of this depends on
// any basis: it is the same physical curve for every working space.
const double kMiredMin = 40.0;
const double kMiredMax = 600.0;
const double kDuvDomain = 0.05;
const double kNewtonSeedMired = 166.0;
const int kNewtonIterations = 25;
const double kNewtonStepMired = 0.05;
const double kNewtonConvergeTol = 1e-12;
const double kNewtonResidualTol = 1e-7;
const double kAxisStepMired = 0.5;

void
kimXY(double kelvin,
      double& x,
      double& y)
{
    double T = std::min(std::max(kelvin, 1667.0), 25000.0);
    double T2 = T * T;
    double T3 = T2 * T;

    if (T <= 4000.0) {
        x = -0.2661239e9 / T3 - 0.2343589e6 / T2 + 0.8776956e3 / T + 0.179910;
    } else {
        x = -3.0258469e9 / T3 + 2.1070379e6 / T2 + 0.2226347e3 / T + 0.240390;
    }

    double x2 = x * x;
    double x3 = x2 * x;

    if (T <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (T <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
}

void
xyToUV60(double x,
         double y,
         double& u,
         double& v)
{
    double d = -2.0 * x + 12.0 * y + 3.0;

    u = 4.0 * x / d;
    v = 6.0 * y / d;
}

void
uv60ToXY(double u,
         double v,
         double& x,
         double& y)
{
    double d = 2.0 * u - 8.0 * v + 4.0;

    x = 3.0 * u / d;
    y = 2.0 * v / d;
}

void
locusUV60(double mired,
          double& u,
          double& v)
{
    double x, y;

    kimXY(1.0e6 / mired, x, y);
    xyToUV60(x, y, u, v);
}

// The locus's own tangent, rotated a quarter turn and normalised, oriented so +Duv sits above
// the locus (the locus runs from warm/low mired to cool/high mired as drawn here).
void
locusNormal60(double mired,
              double& nx,
              double& ny)
{
    double uPlus, vPlus, uMinus, vMinus;

    locusUV60(std::min(mired + kAxisStepMired, kMiredMax), uPlus, vPlus);
    locusUV60(std::max(mired - kAxisStepMired, kMiredMin), uMinus, vMinus);

    double tx = uPlus - uMinus;
    double ty = vPlus - vMinus;
    double n = std::hypot(tx, ty);

    if (!(n > 0.0)) {
        nx = ny = 0.0;

        return;
    }
    nx = -ty / n;
    ny = tx / n;
    if (ny < 0.0) {
        nx = -nx;
        ny = -ny;
    }
}

void
uvFromMiredDuv(double mired,
               double duv,
               double& u,
               double& v)
{
    double u0, v0, nx, ny;

    locusUV60(mired, u0, v0);
    locusNormal60(mired, nx, ny);
    u = u0 + duv * nx;
    v = v0 + duv * ny;
}

bool
solve2x2(double a00,
         double a01,
         double a10,
         double a11,
         double b0,
         double b1,
         double& x0,
         double& x1)
{
    double det = a00 * a11 - a01 * a10;

    if (det == 0.0 || !std::isfinite(det)) {
        return false;
    }
    x0 = (a11 * b0 - a01 * b1) / det;
    x1 = (a00 * b1 - a10 * b0) / det;

    return std::isfinite(x0) && std::isfinite(x1);
}

// Newton inversion of the locus's (mired, Duv) parametrisation at the target uv, seeded near
// the middle of the domain. The Duv column of the Jacobian is exact (uv is linear in Duv at
// fixed mired); the mired column is a central difference.
bool
miredDuvFromUV(double targetU,
               double targetV,
               double& miredOut,
               double& duvOut)
{
    double m = kNewtonSeedMired;
    double d = 0.0;

    for (int iter = 0; iter < kNewtonIterations; ++iter) {
        double u, v;

        uvFromMiredDuv(m, d, u, v);
        double fx = u - targetU;
        double fy = v - targetV;

        if (std::hypot(fx, fy) < kNewtonConvergeTol) {
            break;
        }

        double uPlus, vPlus, uMinus, vMinus;
        double h = kNewtonStepMired;

        uvFromMiredDuv(std::min(m + h, kMiredMax), d, uPlus, vPlus);
        uvFromMiredDuv(std::max(m - h, kMiredMin), d, uMinus, vMinus);
        double jmU = (uPlus - uMinus) / (2.0 * h);
        double jmV = (vPlus - vMinus) / (2.0 * h);
        double jdU, jdV;

        locusNormal60(m, jdU, jdV);

        double stepM, stepD;

        if (!solve2x2(jmU, jdU, jmV, jdV, fx, fy, stepM, stepD)) {
            return false;
        }
        m = std::min(std::max(m - stepM, kMiredMin), kMiredMax);
        d -= stepD;
        if (!std::isfinite(m) || !std::isfinite(d)) {
            return false;
        }
    }

    double u, v;

    uvFromMiredDuv(m, d, u, v);
    if (std::hypot(u - targetU, v - targetV) > kNewtonResidualTol) {
        return false;
    }
    miredOut = m;
    duvOut = d;

    return true;
}

// The working white's own position on the locus. Independent of Y: XYZ of (1,1,1) is Mprime's
// own row sums, and xy (hence uv) does not depend on their common scale.
bool
solveWorkingWhiteMiredDuv(const LumaBasis& basis,
                          double& miredOut,
                          double& duvOut)
{
    if (!basis.hasMatrix) {
        return false;
    }

    double X = basis.Mprime[0][0] + basis.Mprime[0][1] + basis.Mprime[0][2];
    double Y = basis.Mprime[1][0] + basis.Mprime[1][1] + basis.Mprime[1][2];
    double Z = basis.Mprime[2][0] + basis.Mprime[2][1] + basis.Mprime[2][2];
    double s = X + Y + Z;

    if (!(s > 0.0)) {
        return false;
    }

    double u, v;

    xyToUV60(X / s, Y / s, u, v);
    if (!std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    return miredDuvFromUV(u, v, miredOut, duvOut);
}

bool
workingWhiteMiredDuv(const LumaBasis& basis,
                     double& miredOut,
                     double& duvOut)
{
    if (!basis.hasLocusWhite) {
        return false;
    }
    miredOut = basis.whiteMired;
    duvOut = basis.whiteDuv;

    return true;
}

bool
solve3x3(const double m[3][3],
         const double rhs[3],
         double out[3])
{
    double cof00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    double cof01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    double cof02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    double det = m[0][0] * cof00 + m[0][1] * cof01 + m[0][2] * cof02;

    if (det == 0.0 || !std::isfinite(det)) {
        return false;
    }

    double cof10 = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    double cof11 = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    double cof12 = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    double cof20 = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    double cof21 = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    double cof22 = m[0][0] * m[1][1] - m[0][1] * m[1][0];

    out[0] = (rhs[0] * cof00 + rhs[1] * cof10 + rhs[2] * cof20) / det;
    out[1] = (rhs[0] * cof01 + rhs[1] * cof11 + rhs[2] * cof21) / det;
    out[2] = (rhs[0] * cof02 + rhs[1] * cof12 + rhs[2] * cof22) / det;

    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Walks the locus to mired(white) + temp, steps off it by Duv(white) - tint/1000 along its own
// normal, rebuilds XYZ at Y, and solves Mprime*rgb = XYZ. The residual shift afterwards only
// absorbs the solve's rounding: Mprime's Y row is w, so the result is already luma-exact.
bool
planckianRgbFromTempTint(const LumaBasis& basis,
                         double temp,
                         double tint,
                         double Y,
                         double rgb[3])
{
    double whiteMired, whiteDuv;

    if (!workingWhiteMiredDuv(basis, whiteMired, whiteDuv)) {
        return false;
    }

    double mired = whiteMired + temp;

    if (!(mired > kMiredMin) || !(mired < kMiredMax)) {
        return false;
    }

    double duv = whiteDuv - tint / 1000.0;

    if (std::abs(duv) > kDuvDomain) {
        return false;
    }

    double u, v;

    uvFromMiredDuv(mired, duv, u, v);

    double x, y;

    uv60ToXY(u, v, x, y);
    if (!std::isfinite(x) || !std::isfinite(y) || y == 0.0) {
        return false;
    }

    double xyz[3] = { x / y * Y, Y, (1.0 - x - y) / y * Y };

    if (!solve3x3(basis.Mprime, xyz, rgb)) {
        return false;
    }

    double residual = Y - luma(basis, rgb);

    for (int i = 0; i < 3; ++i) {
        rgb[i] += residual;
    }

    return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
}

// Independent of role, since it only looks at the chromaticity rgb implies, not at what kind
// of knob it came from.
bool
planckianTempTintFromRgb(const LumaBasis& basis,
                         const double rgb[3],
                         double& temp,
                         double& tint)
{
    if (!basis.hasMatrix) {
        return false;
    }
    if (!(rgb[0] > 0.0) || !(rgb[1] > 0.0) || !(rgb[2] > 0.0)) {
        return false;
    }

    double X = dot3(basis.Mprime[0], rgb);
    double Y = dot3(basis.Mprime[1], rgb);
    double Z = dot3(basis.Mprime[2], rgb);
    double s = X + Y + Z;

    if (!(s > 0.0)) {
        return false;
    }

    double u, v;

    xyToUV60(X / s, Y / s, u, v);
    if (!std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    double mired, duv;

    if (!miredDuvFromUV(u, v, mired, duv)) {
        return false;
    }
    if (std::abs(duv) > kDuvDomain || !(mired > kMiredMin) || !(mired < kMiredMax)) {
        return false;
    }

    double whiteMired, whiteDuv;

    if (!workingWhiteMiredDuv(basis, whiteMired, whiteDuv)) {
        return false;
    }
    temp = mired - whiteMired;
    tint = -1000.0 * (duv - whiteDuv);

    return true;
}
TempTintAxes
solveLinearTempTintAxes(const LumaBasis& basis)
{
    TempTintAxes axes;

    if (!basis.hasMatrix) {
        return axes;
    }

    double tempPlus[3], tempMinus[3];
    const double h = kAxisStepMired;

    if (!planckianRgbFromTempTint(basis, h, 0.0, 1.0, tempPlus)
        || !planckianRgbFromTempTint(basis, -h, 0.0, 1.0, tempMinus)) {
        return axes;
    }

    double tintPlus[3], tintMinus[3];

    if (!planckianRgbFromTempTint(basis, 0.0, h, 1.0, tintPlus)
        || !planckianRgbFromTempTint(basis, 0.0, -h, 1.0, tintMinus)) {
        return axes;
    }

    double dTemp[3], dTint[3];

    for (int i = 0; i < 3; ++i) {
        dTemp[i] = tempPlus[i] - tempMinus[i];
        dTint[i] = tintPlus[i] - tintMinus[i];
    }

    double aTemp = dot3(basis.F[0], dTemp);
    double bTemp = dot3(basis.F[1], dTemp);
    double nTemp = std::hypot(aTemp, bTemp);
    double aTint = dot3(basis.F[0], dTint);
    double bTint = dot3(basis.F[1], dTint);
    double nTint = std::hypot(aTint, bTint);

    if (!(nTemp > 0.0) || !(nTint > 0.0) || !std::isfinite(nTemp) || !std::isfinite(nTint)) {
        return axes;
    }
    axes.temp[0] = aTemp / nTemp;
    axes.temp[1] = bTemp / nTemp;
    axes.tint[0] = aTint / nTint;
    axes.tint[1] = bTint / nTint;
    axes.valid = true;

    return axes;
}

void
initTempTintCache(LumaBasis& basis)
{
    basis.hasLocusWhite = solveWorkingWhiteMiredDuv(basis, basis.whiteMired, basis.whiteDuv);

    TempTintAxes axes = solveLinearTempTintAxes(basis);

    basis.hasTempTintAxes = axes.valid;
    for (int i = 0; i < 2; ++i) {
        basis.tempAxis[i] = axes.temp[i];
        basis.tintAxis[i] = axes.tint[i];
    }
}
} // namespace

TempTintAxes
linearTempTintAxes(const LumaBasis& basis)
{
    TempTintAxes axes;

    if (!basis.hasTempTintAxes) {
        return axes;
    }
    for (int i = 0; i < 2; ++i) {
        axes.temp[i] = basis.tempAxis[i];
        axes.tint[i] = basis.tintAxis[i];
    }
    axes.valid = true;

    return axes;
}

TempTintResult
toTempTint(ColorKnobRoleEnum role,
           const LumaBasis& basis,
           const KnobColorValue& v)
{
    TempTintResult result;

    if (role == eColorKnobRolePower) {
        KnobColorValue effect;

        if (!toEffectValue(v, effect)) {
            return result;
        }

        return toTempTint(eColorKnobRoleGain, basis, effect);
    }

    if (role == eColorKnobRoleOffset) {
        TempTintAxes axes = linearTempTintAxes(basis);

        if (!axes.valid) {
            return result;
        }

        double det = axes.temp[0] * axes.tint[1] - axes.temp[1] * axes.tint[0];

        if (det == 0.0 || !std::isfinite(det)) {
            return result;
        }

        LumaChroma lc = split(basis, v.rgb);

        result.temp = (axes.tint[1] * lc.a - axes.tint[0] * lc.b) / det;
        result.tint = (-axes.temp[1] * lc.a + axes.temp[0] * lc.b) / det;
        result.valid = true;

        return result;
    }

    double Y0 = luma(basis, v.rgb);

    if (isFrozen(role, Y0)) {
        return result;
    }
    if (planckianTempTintFromRgb(basis, v.rgb, result.temp, result.tint)) {
        result.valid = true;
    }

    return result;
}

ChromaEditResult
fromTempTint(ColorKnobRoleEnum role,
             const LumaBasis& basis,
             const KnobColorValue& v,
             double temp,
             double tint)
{
    if (role == eColorKnobRolePower) {
        KnobColorValue effect;

        if (!toEffectValue(v, effect)) {
            return refused(v);
        }

        ChromaEditResult effectResult = fromTempTint(eColorKnobRoleGain, basis, effect, temp, tint);

        if (!effectResult.valid) {
            return refused(v);
        }

        ChromaEditResult result;

        if (!fromEffectValue(effectResult.value, result.value)) {
            return refused(v);
        }
        result.valid = true;

        return result;
    }

    if (role == eColorKnobRoleOffset) {
        TempTintAxes axes = linearTempTintAxes(basis);

        if (!axes.valid) {
            return refused(v);
        }

        ChromaEditResult result;

        result.value = v;
        result.valid = true;
        join(basis, luma(basis, v.rgb),
             temp * axes.temp[0] + tint * axes.tint[0],
             temp * axes.temp[1] + tint * axes.tint[1],
             result.value.rgb);

        return result;
    }

    double Y0 = luma(basis, v.rgb);

    if (isFrozen(role, Y0)) {
        return refused(v);
    }

    ChromaEditResult result;

    result.value = v;
    if (!planckianRgbFromTempTint(basis, temp, tint, Y0, result.value.rgb)) {
        return refused(v);
    }
    result.valid = true;

    return result;
}

std::vector<PlanePoint>
drawLocus(const LumaBasis& basis,
          const PlanePoint& anchor)
{
    std::vector<PlanePoint> points;

    double whiteMired, whiteDuv;

    if (!workingWhiteMiredDuv(basis, whiteMired, whiteDuv)) {
        return points;
    }

    const double step = 4.0;

    for (double mired = kMiredMin + step; mired < kMiredMax - 0.5 * step; mired += step) {
        double rgb[3];

        if (!planckianRgbFromTempTint(basis, mired - whiteMired, 0.0, 1.0, rgb)) {
            continue;
        }

        KnobColorValue sample;

        sample.rgb[0] = rgb[0];
        sample.rgb[1] = rgb[1];
        sample.rgb[2] = rgb[2];
        sample.a = 0.0;
        sample.hasAlpha = false;

        PlanePoint q = toPlane(eColorKnobRoleGain, basis, sample);

        if (!q.valid) {
            continue;
        }

        PlanePoint p;

        p.x = q.x - anchor.x;
        p.y = q.y - anchor.y;
        p.valid = true;
        p.linear = q.linear;
        points.push_back(p);
    }

    return points;
}

double
gear(bool ctrlHeld,
     bool shiftHeld)
{
    if (ctrlHeld && shiftHeld) {
        return 0.01;
    }
    if (ctrlHeld) {
        return 0.1;
    }
    if (shiftHeld) {
        return 4.0;
    }

    return 1.0;
}

double
valueRatePerPixel(const TrackballRates& rates,
                  ColorKnobRoleEnum role)
{
    return isRatioRole(role) ? rates.valueStopPerPixelRatio : rates.valuePerPixelOffset;
}

double
tempRatePerPixel(const TrackballRates& rates,
                 ColorKnobRoleEnum role)
{
    return isRatioRole(role) ? rates.tempMiredPerPixelRatio : rates.tempTintPerPixelOffset;
}

double
tintRatePerPixel(const TrackballRates& rates,
                 ColorKnobRoleEnum role)
{
    return isRatioRole(role) ? rates.tintPerPixelRatio : rates.tempTintPerPixelOffset;
}

namespace {
// The out-of-domain and Power-wrapping fallback that applyEdit's relative TempTint edit needs
// and fromTempTint, by design, does not provide on its own (see its header comment): walking
// the locus by (current + delta) can fail where the locus itself is undefined (an out-of-gamut
// ratio-role chromaticity, or a frozen one), and the caller is the one who knows a relative
// nudge should then fall back to moving along the same linear axes directly in (a, b) space
// rather than refusing outright. For Offset, toTempTint's decomposition is exact over the
// whole plane, so this fallback is never actually reached for that role.
ChromaEditResult
nudgeTempTint(ColorKnobRoleEnum role,
              const LumaBasis& basis,
              const KnobColorValue& v,
              double dTemp,
              double dTint)
{
    if (role == eColorKnobRolePower) {
        KnobColorValue effect;

        if (!toEffectValue(v, effect)) {
            return refused(v);
        }

        ChromaEditResult effectResult = nudgeTempTint(eColorKnobRoleGain, basis, effect, dTemp, dTint);

        if (!effectResult.valid) {
            return refused(v);
        }

        ChromaEditResult result;

        if (!fromEffectValue(effectResult.value, result.value)) {
            return refused(v);
        }
        result.valid = true;

        return result;
    }

    TempTintResult current = toTempTint(role, basis, v);

    if (current.valid) {
        ChromaEditResult viaLocus = fromTempTint(role, basis, v, current.temp + dTemp, current.tint + dTint);

        if (viaLocus.valid) {
            return viaLocus;
        }
    }

    double Y0 = luma(basis, v.rgb);
    TempTintAxes axes = linearTempTintAxes(basis);

    if (!axes.valid || isFrozen(role, Y0)) {
        return refused(v);
    }

    LumaChroma lc = split(basis, v.rgb);
    ChromaEditResult result;

    result.value = v;
    result.valid = true;
    join(basis, Y0,
         lc.a + dTemp * axes.temp[0] + dTint * axes.tint[0],
         lc.b + dTemp * axes.temp[1] + dTint * axes.tint[1],
         result.value.rgb);

    return result;
}
} // namespace

ChromaEditResult
applyEdit(ColorKnobRoleEnum role,
          const LumaBasis& basis,
          const KnobColorValue& v,
          const ColorEdit& edit,
          double rememberedHueDegrees)
{
    bool ratio = isRatioRole(role);

    switch (edit.axis) {
    case eColorAxisTrackball:
        return translate(role, basis, v, edit.d1, edit.d2);

    case eColorAxisHue:
        return edit.absolute ? setHue(role, basis, v, edit.d1) : rotateHueAboutWhite(role, basis, v, edit.d1);

    case eColorAxisSat:
        return edit.absolute ? setSaturation(role, basis, v, edit.d1, rememberedHueDegrees)
                             : moveSaturation(role, basis, v, edit.d1, rememberedHueDegrees);

    case eColorAxisValue: {
        double factor = ratio ? std::pow(2.0, edit.d1) : 1.0;
        ChromaEditResult result;

        if (edit.absolute) {
            result.value = setValue(role, basis, v, ratio ? valueStopPivot(role) * factor : edit.d1);
        } else {
            result.value = ratio ? scaleValue(role, basis, v, factor) : addValue(role, basis, v, edit.d1);
        }
        result.valid = true;

        return result;
    }

    case eColorAxisTempTint:
        return edit.absolute ? fromTempTint(role, basis, v, edit.d1, edit.d2) : nudgeTempTint(role, basis, v, edit.d1, edit.d2);
    }

    return refused(v);
}

ChromaEditResult
globalModeEdit(ColorGlobalModeEnum mode,
               ColorKnobRoleEnum role,
               const LumaBasis& basis,
               const KnobColorValue& v,
               double dx,
               double dy,
               double gearFactor,
               const TrackballRates& rates,
               double rememberedHueDegrees)
{
    switch (mode) {
    case eColorGlobalModeC: {
        double rate = rates.globalTrackballPlanePerPixel * gearFactor;

        return translate(role, basis, v, dx * rate, -dy * rate);
    }

    case eColorGlobalModeS: {
        ColorEdit hueEdit;

        hueEdit.axis = eColorAxisHue;
        hueEdit.d1 = dx * rates.hueDegreesPerPixel * gearFactor;
        ChromaEditResult rotated = applyEdit(role, basis, v, hueEdit, rememberedHueDegrees);
        KnobColorValue afterHue = rotated.valid ? rotated.value : v;

        ColorEdit satEdit;

        satEdit.axis = eColorAxisSat;
        satEdit.d1 = -dy * rates.satPlanePerPixel * gearFactor;
        ChromaEditResult moved = applyEdit(role, basis, afterHue, satEdit, rememberedHueDegrees);

        return moved.valid ? moved : rotated;
    }

    case eColorGlobalModeT: {
        ColorEdit ttEdit;

        ttEdit.axis = eColorAxisTempTint;
        ttEdit.d1 = dx * tempRatePerPixel(rates, role) * gearFactor;
        ttEdit.d2 = -dy * tintRatePerPixel(rates, role) * gearFactor;

        return applyEdit(role, basis, v, ttEdit, rememberedHueDegrees);
    }

    case eColorGlobalModeV: {
        ColorEdit valueEdit;

        valueEdit.axis = eColorAxisValue;
        valueEdit.d1 = (dx - dy) * valueRatePerPixel(rates, role) * gearFactor;

        return applyEdit(role, basis, v, valueEdit, rememberedHueDegrees);
    }
    }

    return refused(v);
}

SliderRange
sliderMapping(ColorSliderKindEnum kind,
              ColorKnobRoleEnum role,
              const TrackballRates& rates)
{
    bool ratio = isRatioRole(role);
    SliderRange range;

    switch (kind) {
    case eColorSliderKindHue:
        range.rangeMin = 0.0;
        range.rangeMax = 360.0;
        range.unit = "deg";
        break;

    case eColorSliderKindSat:
        range.rangeMin = 0.0;
        range.rangeMax = rates.wheelRim;
        range.unit = "xy";
        break;

    case eColorSliderKindValue:
        if (role == eColorKnobRolePower) {
            // Gamma 0.55 to 5.2: the cap at kPowerGammaMax sits about 2.2 stops above 0.18, so
            // a wider track would end in a stretch that does nothing.
            range.rangeMin = -2.0;
            range.rangeMax = 2.0;
            range.unit = "stop";
        } else if (ratio) {
            range.rangeMin = -3.0;
            range.rangeMax = 3.0;
            range.unit = "stop";
        } else {
            range.rangeMin = -0.25;
            range.rangeMax = 0.25;
            range.unit = "Y";
        }
        break;

    case eColorSliderKindTemp:
        if (ratio) {
            range.rangeMin = -100.0;
            range.rangeMax = 100.0;
            range.unit = "mired";
        } else {
            range.rangeMin = -0.1;
            range.rangeMax = 0.1;
            range.unit = "axis";
        }
        break;

    case eColorSliderKindTint:
        if (ratio) {
            range.rangeMin = -50.0;
            range.rangeMax = 50.0;
            range.unit = "tint";
        } else {
            range.rangeMin = -0.1;
            range.rangeMax = 0.1;
            range.unit = "axis";
        }
        break;
    }

    return range;
}

double
markerToValue(const SliderRange& range,
              double marker)
{
    double t = std::min(1.0, std::max(0.0, marker));

    return range.rangeMin + t * (range.rangeMax - range.rangeMin);
}

PolarValue
applyPolarEdit(const PolarValue& v, const ColorEdit& edit, double maxRadius)
{
    PolarValue out = v;
    double x = v.radius * std::cos(v.hueDegrees / kDegreesPerRadian);
    double y = v.radius * std::sin(v.hueDegrees / kDegreesPerRadian);
    if (edit.axis == eColorAxisTrackball) {
        x += edit.d1;
        y += edit.d2;
        out.radius = std::hypot(x, y);
        if (out.radius > 0.0) {
            out.hueDegrees = std::atan2(y, x) * kDegreesPerRadian;
        }
    } else if (edit.axis == eColorAxisHue && edit.absolute) {
        out.hueDegrees = edit.d1;
    } else if (edit.axis == eColorAxisSat && edit.absolute) {
        out.radius = edit.d1;
    } else {
        return out;
    }
    out.hueDegrees = std::fmod(out.hueDegrees, 360.0);
    if (out.hueDegrees < 0.0) {
        out.hueDegrees += 360.0;
    }
    if (out.hueDegrees >= 360.0) {
        out.hueDegrees = 0.0;
    }
    out.radius = std::max(0.0, std::min(maxRadius, out.radius));
    return out;
}

NATRON_NAMESPACE_EXIT
