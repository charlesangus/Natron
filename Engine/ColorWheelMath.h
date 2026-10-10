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
 **/
struct LumaBasis {
    double w[3];
    double F[2][3];
    double E[3][2];
    double Mprime[3][3];
    bool hasMatrix;

    explicit LumaBasis(const double weights[3]);
    LumaBasis(const double weights[3], const double workingToXYZ[9]);
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

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_COLORWHEELMATH_H
