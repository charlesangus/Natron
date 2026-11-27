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

#ifndef NATRON_ENGINE_COLORWARPSPACE_H
#define NATRON_ENGINE_COLORWARPSPACE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>

NATRON_NAMESPACE_ENTER

/**
 * @brief The colour space a colour warp's pins live in and its displacements are blended in.
 * Every space is a pair of conversions from and to the scene-linear working space, defined for
 * negative and above-1 values and never clamped.
 *
 * - OklabLog: working RGB -> XYZ -> OKLab's LMS, then the log encoding below in place of OKLab's
 *   cube root, then OKLab's LMS-to-Lab matrix. Its a and b rows sum to zero to within 4e-8, so
 *   scaling a colour by k (above the toe) moves L by log2(k) and a and b by under 1e-7 per stop:
 *   a chroma shift is exposure-invariant to about 1e-7.
 * - Oklab: OKLab itself, with a signed cube root so negative LMS values stay invertible. Scaling
 *   a colour by k scales L, a and b by cbrt(k), so a chroma shift grows with exposure.
 * - Xyz: CIE XYZ, linear. Shifts are authored as HSV or RGB offsets relative to the pin's
 *   source (see shiftFromHsvRgbOffset) and stored as XYZ displacements.
 * - LogRgb: the log encoding below, per working-space channel.
 * - LinearRgb: the working space itself.
 **/
enum ColorWarpSpaceEnum {
    eColorWarpSpaceOklabLog,
    eColorWarpSpaceOklab,
    eColorWarpSpaceXyz,
    eColorWarpSpaceLogRgb,
    eColorWarpSpaceLinearRgb
};

inline constexpr int kColorWarpSpaceCount = 5;

const char* colorWarpSpaceName(ColorWarpSpaceEnum space);

/**
 * @brief The working space's RGB-to-XYZ matrix (row-major, rows X, Y, Z) and its inverse.
 * valid is false when the matrix is singular; the conversions then fall back to treating
 * working RGB as XYZ, so that a broken colour config degrades instead of producing NaN.
 **/
struct ColorWarpSpaceBasis {
    double rgbToXyz[9];
    double xyzToRgb[9];
    bool valid;

    explicit ColorWarpSpaceBasis(const double workingToXYZ[9]);
};

/**
 * @brief OKLab's XYZ (D65) to LMS and LMS' to Lab matrices and their inverses, row-major, as the
 * Oklab and OklabLog conversions use them. The inverses are the double-precision inverses of the
 * forward matrices, so a kernel that uploads these reproduces the CPU conversions.
 **/
extern const double kColorWarpOklabXyzToLms[9];
extern const double kColorWarpOklabLmsToLab[9];
const double* colorWarpOklabLmsToXyz();
const double* colorWarpOklabLabToLms();

/**
 * @brief log2 above kColorWarpLogToe, continued below it by the tangent line, so that zero and
 * negative values have a finite, invertible encoding with a continuous first derivative.
 **/
inline constexpr double kColorWarpLogToe = 0.0078125;
inline constexpr double kColorWarpLogToeEncoded = -7.;
inline constexpr double kColorWarpLogToeSlope = 1. / (kColorWarpLogToe * M_LN2);

/**
 * @brief The warp-space reach that a pin falloff of 1 stands for in space: the falloff at which a
 * lone saturated-red pin under the Gaussian kernel changes the same share (15%) of a chart that
 * spans HDR ramps, a colour checker and a neutral ramp. Multiplying a user falloff by it makes one
 * falloff value reach about as far in every space; in OklabLog it is close to one stop along L.
 **/
double colorWarpFalloffScale(ColorWarpSpaceEnum space);

/**
 * @brief The unit vector along space's neutral axis at the working space's white, which a
 * Lightness Falloff stretches a pin's reach along: L in Oklab and OklabLog, the white point's
 * direction in Xyz and R = G = B in LogRgb and LinearRgb.
 **/
void colorWarpLightnessAxis(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, double axis[3]);

double colorWarpLogEncode(double x);
double colorWarpLogDecode(double y);

void toWarpSpace(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, const double rgb[3], double out[3]);
void fromWarpSpace(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, const double warp[3], double rgb[3]);

/**
 * @brief Scene-linear HSV: V is the largest channel, S the chroma (max - min) over V, H in
 * degrees [0, 360). V and S are not limited to [0, 1]. A colour whose largest channel is not
 * positive has no defined hue or saturation and reads as S = 0, H = 0; converting it back gives
 * the neutral at V, so the pair round-trips only for colours with a positive largest channel.
 **/
void colorWarpRgbToHsv(const double rgb[3], double hsv[3]);
void colorWarpHsvToRgb(const double hsv[3], double rgb[3]);

enum ColorWarpOffsetKindEnum {
    eColorWarpOffsetRgb,
    eColorWarpOffsetHsv
};

/**
 * @brief The XYZ displacement that moves sourceRgb to the colour the offset describes: for Rgb,
 * sourceRgb + offset; for Hsv, the source's HSV plus offset (hue in degrees, wrapped). This is
 * the Xyz space's translation layer: the user edits offsets relative to the picked colour, the
 * field stores and blends XYZ displacements.
 **/
void shiftFromHsvRgbOffset(const ColorWarpSpaceBasis& basis,
                           ColorWarpOffsetKindEnum kind,
                           const double sourceRgb[3],
                           const double offset[3],
                           double xyzShift[3]);

/**
 * @brief The inverse of shiftFromHsvRgbOffset: the offset that, applied to sourceRgb, lands on
 * XYZ(sourceRgb) + xyzShift. Hue offsets come back in (-180, 180].
 **/
void hsvRgbOffsetFromShift(const ColorWarpSpaceBasis& basis,
                           ColorWarpOffsetKindEnum kind,
                           const double sourceRgb[3],
                           const double xyzShift[3],
                           double offset[3]);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_COLORWARPSPACE_H
