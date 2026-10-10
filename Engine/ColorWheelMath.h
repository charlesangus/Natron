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

#ifndef NATRON_ENGINE_COLORWHEELMATH_H
#define NATRON_ENGINE_COLORWHEELMATH_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <optional>

NATRON_NAMESPACE_ENTER

/**
 * @brief Where a colour knob's value lives on the Gain <-> Offset axis, and what its wheel
 * puck means. Auto is never passed to the functions below: it is resolved to one of the other
 * four roles (by native declaration or by inferColorRole) before any edit is applied.
 **/
enum ColorKnobRoleEnum {
    eColorKnobRoleAuto,
    eColorKnobRoleGain,
    eColorKnobRoleOffset,
    eColorKnobRoleColour,
    eColorKnobRolePower
};

/**
 * @brief True for the roles whose puck is (a, b)/Y (Y changes multiplicatively and the
 * chroma/luma ratio is what a hue or saturation edit preserves), false for Offset, whose
 * puck is (a, b) on its own. Power is a ratio role: it runs the gain model on a mid-grey
 * effect vector and maps the result back to gamma. Auto is treated as a ratio role, the
 * same as Colour, so a knob whose role has not been resolved yet still behaves safely.
 **/
bool isRatioRole(ColorKnobRoleEnum role);

/**
 * @brief A ratio role's chroma/luma ratio is undefined once Y collapses to (near) zero: this
 * reports that case so that value edits can fall back to additive and chroma edits can refuse
 * rather than divide by it. Offset is never frozen.
 **/
bool isFrozen(ColorKnobRoleEnum role, double Y);

/**
 * @brief Which projective chromaticity the wheel's plane is drawn in. Both are projective
 * maps of XYZ, so gamut edges stay straight and the triangle is the chromaticity diagram's
 * own; u'v' is the alternative if xy reads badly, and is a one-constant swap.
 **/
enum ChromaticityKindEnum {
    eChromaticityKindXY,
    eChromaticityKindUPrimeVPrime
};

constexpr ChromaticityKindEnum kChromaticityKind = eChromaticityKindXY;

/**
 * @brief The luma-chroma split (design doc Sec. 3.3), built once per working space and shared
 * by every colour knob. w is the working space's luma weights, renormalised so they sum to 1.
 * F is fixed (pure R, G, B sit at hue 0/120/240 degrees whatever the working space); E depends
 * on w and is the only place the working space enters split/join. By construction F*E is the
 * 2x2 identity and w.E is the zero row, so join(split(basis, c)) == c and any edit that only
 * changes (a, b) leaves Y bit-exact.
 *
 * Mprime is the working-to-XYZ matrix, uniformly rescaled so its Y row is exactly w (the scale
 * factor is 1 / (the raw Y row's own sum), which is how w itself is derived from that row, so
 * the two agree whenever the caller's weights and matrix come from the same probe). hasMatrix
 * is false when there is no matrix (a fallback config such as plain Rec.709 constants), and
 * Mprime is then left zeroed.
 *
 * The chroma plane is chroma(c) = (chromaNum * c) / (chromaDen . c), the chosen chromaticity
 * (xy or u'v') written directly over working rgb, and white is chroma of (1, 1, 1). J is that
 * map's Jacobian at (1, 1, 1) and JEinv is (J*E)^-1, which together give the linear plane used
 * where the projective one is undefined. Without a matrix there is no chromaticity at all, so
 * chromaNum, chromaDen and white are zeroed and J is a fixed multiple of F instead: the plane
 * is then linear everywhere, still about white and still Y-exact, just not xy-shaped.
 **/
struct LumaBasis {
    double w[3];
    double F[2][3];
    double E[3][2];
    double Mprime[3][3];
    bool hasMatrix;
    double chromaNum[2][3];
    double chromaDen[3];
    double white[2];
    double J[2][3];
    double JEinv[2][2];

    explicit LumaBasis(const double weights[3]);
    LumaBasis(const double weights[3], const double workingToXYZ[9], ChromaticityKindEnum kind = kChromaticityKind);
};

/**
 * @brief A colour knob's current value, in whatever dimension the knob declares. hasAlpha is
 * false for a 3-dim knob, which the alpha-tracking rule below then leaves untouched.
 **/
struct KnobColorValue {
    double rgb[3];
    double a;
    bool hasAlpha;
};

/**
 * @brief The luma-chroma coordinates of an RGB triplet under a basis: Y = w.rgb, and (a, b) =
 * F*rgb. a and b do not depend on Y on their own; a ratio role's puck is (a, b)/Y (see
 * isRatioRole), computed by the caller.
 **/
struct LumaChroma {
    double Y;
    double a;
    double b;
};

double luma(const LumaBasis& basis, const double rgb[3]);
LumaChroma split(const LumaBasis& basis, const double rgb[3]);
void join(const LumaBasis& basis, double Y, double a, double b, double rgb[3]);

/**
 * @brief The three value edits, every one of which ends up calling setValue with the target Y
 * it computes: scaleValue(f) targets Y*f (an exposure-style "stop" edit), addValue(d) targets
 * Y+d (an additive "lift" edit), and setValue itself targets an absolute Y.
 *
 * setValue keeps (a, b)/Y, i.e. it scales the whole rgb triplet, when the role is a ratio role
 * and the starting Y is not frozen; otherwise it keeps (a, b) absolute and shifts rgb by the
 * Y delta, which is the "switch to additive" the frozen case needs: scaling a near-zero Y by
 * any factor stays near zero, but adding to it does not, so addValue is what escapes a frozen
 * ratio role, and scaleValue on one is left close to a no-op, as multiplying by zero should be.
 *
 * Every one of the three applies the same alpha rule after the rgb edit: alpha scales by the
 * same Y1/Y0 factor as a ratio role's rgb when neither end is frozen, and otherwise shifts by
 * Y1-Y0, matching the rgb edit itself. hasAlpha false (a 3-dim knob) leaves alpha untouched.
 * Never clamped.
 **/
KnobColorValue scaleValue(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double factor);
KnobColorValue addValue(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double delta);
KnobColorValue setValue(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double Y);

/**
 * @brief The Y a value reset targets: 1 for Gain and Power, 0 for Offset. Colour has no value
 * reset target (its neutral is the knob's own current Y, per the knob roles table), so it
 * returns nullopt, and so does Auto, which is never resolved to a role by the time this is
 * called. The caller feeds the result to setValue when it holds a value.
 **/
std::optional<double> resetValue(ColorKnobRoleEnum role);

/**
 * @brief A point (or a vector) in the chroma plane, relative to the working white, so neutral
 * is (0, 0) and the hue angle and saturation are the polar coordinates about white.
 *
 * valid is false when the value has no plane point: a frozen ratio role, a ratio role whose
 * chromaticity denominator is not positive, or a solve that failed. linear is true when the
 * point came from the linear (Jacobian) plane rather than the projective one: an offset whose
 * 18% grey pivot has Y <= 0 or a non-positive chromaticity denominator, or any role on a
 * matrix-less basis.
 **/
struct PlanePoint {
    double x = 0.0;
    double y = 0.0;
    bool valid = false;
    bool linear = false;
};

/**
 * @brief The outcome of a chroma edit. On valid false, value is the input unchanged, so a
 * caller can apply it either way and only needs valid to decide whether to report a refusal.
 **/
struct ChromaEditResult {
    KnobColorValue value;
    bool valid;
};

/**
 * @brief The plane point of a knob value.
 *
 * Ratio roles (Gain, Colour, Power, Auto) use q = chroma(rgb) - white, which does not depend on
 * Y. Offset uses the chroma of what the offset does to 18% grey, chroma(0.18 + o) - white, so
 * a gain and an offset that do the same thing to mid-grey land on the same point; where that
 * is undefined it uses the linear plane J*o/0.18, which agrees with it to first order near
 * white, so the switch has no visible jump at small chroma.
 **/
PlanePoint toPlane(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v);

/**
 * @brief The knob value whose plane point is q and whose luma w.rgb is Yheld (the knob's own
 * luma, including for Offset: the 18% grey pivot is added internally). Alpha is copied from v.
 *
 * v is the gesture-start value and is only used to pick the plane, projective or linear, that
 * toPlane(v) uses, so toPlane(fromPlane(v, q, Y)) == q within a gesture even across the
 * offset fallback boundary. w.rgb == Yheld to rounding, by construction.
 **/
ChromaEditResult fromPlane(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, const PlanePoint& q, double Yheld);

/**
 * @brief The chroma edits. Each one reads q = toPlane(v), moves it, and rebuilds through
 * fromPlane at v's own luma, so none of them changes w.rgb or alpha.
 *
 * Angles are in degrees, measured counter-clockwise from +x about white (the xy dominant
 * direction, not HSV hue). translate adds (dx, dy) to q. rotateHueAboutWhite turns q by
 * dThetaDegrees, and setHue sets its angle, both at constant saturation; on a neutral value
 * they are no-ops. setSaturation sets |q| (clamped at 0) along q's own hue, or along
 * rememberedHueDegrees when q is neutral, which is how a saturation drag grows back out of
 * white in the direction it came from. moveSaturation is setSaturation(|q| + ds).
 **/
ChromaEditResult translate(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double dx, double dy);
ChromaEditResult rotateHueAboutWhite(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double dThetaDegrees);
ChromaEditResult setHue(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double thetaDegrees);
ChromaEditResult setSaturation(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double s, double rememberedHueDegrees);
ChromaEditResult moveSaturation(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& v, double ds, double rememberedHueDegrees);

/**
 * @brief The polar coordinates of a plane point about white: hue in [0, 360) degrees and
 * saturation as the distance from white in plane units. A neutral point reads hue 0.
 **/
double planeHue(const PlanePoint& q);
double planeSaturation(const PlanePoint& q);

/**
 * @brief The working gamut's primaries in the plane, relative to white, in r, g, b order. Its
 * edges are the straight segments where one channel is 0. valid is false on a matrix-less
 * basis, which has no chromaticity to draw a triangle in.
 **/
struct GamutTriangle {
    PlanePoint vertex[3];
    bool valid;
};

GamutTriangle gamutTriangle(const LumaBasis& basis);

/**
 * @brief Where neutral sits on a wheel re-centred on anchor: the wheel draws every overlay
 * relative to the anchor, so white is at -anchor.
 **/
PlanePoint whitePoint(const PlanePoint& anchor);

/**
 * @brief Clamps a wheel-relative point to a disc of radius rim. point is p itself on the disc
 * and p pulled back onto the rim past it; direction is the unit vector from the centre toward
 * p (zero when p is the centre), which is both where a parked puck points and where the
 * off-wheel white marker's caret points.
 **/
struct DiscClamp {
    PlanePoint point;
    bool offDisc;
    PlanePoint direction;
};

DiscClamp clampToDisc(const PlanePoint& p, double rim);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_COLORWHEELMATH_H
