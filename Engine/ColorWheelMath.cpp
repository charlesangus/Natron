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
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {
// Pure R, G, B sit at hue 0/120/240 degrees here whatever the working space, because F does
// not depend on w; only E (built below from the basis's own w) carries the working space.
const double kF[2][3] = {
    { 1.0, -0.5, -0.5 },
    { 0.0, 0.8660254037844386, -0.8660254037844386 }
};

const double kFreezeThreshold = 1e-6;

const double kPivotGrey = 0.18;

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
} // namespace

LumaBasis::LumaBasis(const double weights[3])
    : hasMatrix(false)
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
    return fromPlane(role, basis, v, q, luma(basis, v.rgb));
}
} // namespace

PlanePoint
toPlane(ColorKnobRoleEnum role,
        const LumaBasis& basis,
        const KnobColorValue& v)
{
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

NATRON_NAMESPACE_EXIT
