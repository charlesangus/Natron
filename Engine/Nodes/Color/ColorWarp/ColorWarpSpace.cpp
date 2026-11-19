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

#include "ColorWarpSpace.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {
// OKLab's XYZ (D65) to LMS and LMS' to Lab matrices, row-major.
const double kOklabXyzToLms[9] = {
    0.8189330101, 0.3618667424, -0.1288597137,
    0.0329845436, 0.9293118715, 0.0361456387,
    0.0482003018, 0.2643662691, 0.6338517070
};
const double kOklabLmsToLab[9] = {
    0.2104542553, 0.7936177850, -0.0040720468,
    1.9779984951, -2.4285922050, 0.4505937099,
    0.0259040371, 0.7827717662, -0.8086757660
};

void
mul3(const double m[9], const double v[3], double out[3])
{
    const double x = v[0], y = v[1], z = v[2];

    out[0] = m[0] * x + m[1] * y + m[2] * z;
    out[1] = m[3] * x + m[4] * y + m[5] * z;
    out[2] = m[6] * x + m[7] * y + m[8] * z;
}

bool
invert3(const double m[9], double out[9])
{
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    double scale = 0.;

    for (int i = 0; i < 9; ++i) {
        scale = std::max(scale, std::fabs(m[i]));
    }
    if (!std::isfinite(det) || (std::fabs(det) <= 1e-12 * scale * scale * scale)) {
        return false;
    }
    const double inv = 1. / det;
    out[0] = c00 * inv;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
    out[3] = c01 * inv;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
    out[6] = c02 * inv;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;

    return true;
}

struct OklabInverses {
    double lmsToXyz[9];
    double labToLms[9];

    OklabInverses()
    {
        invert3(kOklabXyzToLms, lmsToXyz);
        invert3(kOklabLmsToLab, labToLms);
    }
};

const OklabInverses&
oklabInverses()
{
    static const OklabInverses inverses;

    return inverses;
}

double
wrapHueDegrees(double h)
{
    h = std::fmod(h, 360.);
    if (h < 0.) {
        h += 360.;
    }

    return (h >= 360.) ? 0. : h;
}

double
wrapHueOffsetDegrees(double dh)
{
    dh = std::fmod(dh, 360.);
    if (dh > 180.) {
        dh -= 360.;
    } else if (dh <= -180.) {
        dh += 360.;
    }

    return dh;
}
} // anon namespace

const char*
colorWarpSpaceName(ColorWarpSpaceEnum space)
{
    switch (space) {
    case eColorWarpSpaceOklabLog:
        return "oklablog";
    case eColorWarpSpaceXyz:
        return "xyz";
    case eColorWarpSpaceLogRgb:
        return "logrgb";
    case eColorWarpSpaceLinearRgb:
        return "linearrgb";
    }

    return "unknown";
}

ColorWarpSpaceBasis::ColorWarpSpaceBasis(const double workingToXYZ[9])
{
    std::copy(workingToXYZ, workingToXYZ + 9, rgbToXyz);
    valid = invert3(rgbToXyz, xyzToRgb);
    if (!valid) {
        const double identity[9] = { 1., 0., 0., 0., 1., 0., 0., 0., 1. };
        std::copy(identity, identity + 9, rgbToXyz);
        std::copy(identity, identity + 9, xyzToRgb);
    }
}

double
colorWarpLogEncode(double x)
{
    if (x >= kColorWarpLogToe) {
        return std::log2(x);
    }

    return std::log2(kColorWarpLogToe) + (x - kColorWarpLogToe) / (kColorWarpLogToe * M_LN2);
}

double
colorWarpLogDecode(double y)
{
    const double yToe = std::log2(kColorWarpLogToe);

    if (y >= yToe) {
        return std::exp2(y);
    }

    return kColorWarpLogToe + (y - yToe) * (kColorWarpLogToe * M_LN2);
}

void
toWarpSpace(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, const double rgb[3], double out[3])
{
    switch (space) {
    case eColorWarpSpaceOklabLog: {
        double xyz[3], lms[3];
        mul3(basis.rgbToXyz, rgb, xyz);
        mul3(kOklabXyzToLms, xyz, lms);
        for (int c = 0; c < 3; ++c) {
            lms[c] = colorWarpLogEncode(lms[c]);
        }
        mul3(kOklabLmsToLab, lms, out);
        return;
    }
    case eColorWarpSpaceXyz:
        mul3(basis.rgbToXyz, rgb, out);
        return;
    case eColorWarpSpaceLogRgb:
        for (int c = 0; c < 3; ++c) {
            out[c] = colorWarpLogEncode(rgb[c]);
        }
        return;
    case eColorWarpSpaceLinearRgb:
        break;
    }
    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
}

void
fromWarpSpace(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, const double warp[3], double rgb[3])
{
    switch (space) {
    case eColorWarpSpaceOklabLog: {
        const OklabInverses& inv = oklabInverses();
        double lms[3], xyz[3];
        mul3(inv.labToLms, warp, lms);
        for (int c = 0; c < 3; ++c) {
            lms[c] = colorWarpLogDecode(lms[c]);
        }
        mul3(inv.lmsToXyz, lms, xyz);
        mul3(basis.xyzToRgb, xyz, rgb);
        return;
    }
    case eColorWarpSpaceXyz:
        mul3(basis.xyzToRgb, warp, rgb);
        return;
    case eColorWarpSpaceLogRgb:
        for (int c = 0; c < 3; ++c) {
            rgb[c] = colorWarpLogDecode(warp[c]);
        }
        return;
    case eColorWarpSpaceLinearRgb:
        break;
    }
    rgb[0] = warp[0];
    rgb[1] = warp[1];
    rgb[2] = warp[2];
}

void
colorWarpRgbToHsv(const double rgb[3], double hsv[3])
{
    const double r = rgb[0], g = rgb[1], b = rgb[2];
    const double maxC = std::max(r, std::max(g, b));
    const double minC = std::min(r, std::min(g, b));
    const double chroma = maxC - minC;

    hsv[2] = maxC;
    if ((maxC <= 0.) || (chroma <= 0.)) {
        hsv[0] = 0.;
        hsv[1] = 0.;
        return;
    }
    hsv[1] = chroma / maxC;
    double h;
    if (maxC == r) {
        h = (g - b) / chroma;
    } else if (maxC == g) {
        h = 2. + (b - r) / chroma;
    } else {
        h = 4. + (r - g) / chroma;
    }
    hsv[0] = wrapHueDegrees(h * 60.);
}

void
colorWarpHsvToRgb(const double hsv[3], double rgb[3])
{
    const double v = hsv[2];
    const double chroma = hsv[1] * v;
    const double minC = v - chroma;
    const double hp = wrapHueDegrees(hsv[0]) / 60.;
    const int sector = std::min(5, static_cast<int>(std::floor(hp)));
    const double frac = hp - sector;
    const double rising = minC + chroma * frac;
    const double falling = minC + chroma * (1. - frac);

    switch (sector) {
    case 0:
        rgb[0] = v;
        rgb[1] = rising;
        rgb[2] = minC;
        break;
    case 1:
        rgb[0] = falling;
        rgb[1] = v;
        rgb[2] = minC;
        break;
    case 2:
        rgb[0] = minC;
        rgb[1] = v;
        rgb[2] = rising;
        break;
    case 3:
        rgb[0] = minC;
        rgb[1] = falling;
        rgb[2] = v;
        break;
    case 4:
        rgb[0] = rising;
        rgb[1] = minC;
        rgb[2] = v;
        break;
    default:
        rgb[0] = v;
        rgb[1] = minC;
        rgb[2] = falling;
        break;
    }
}

void
shiftFromHsvRgbOffset(const ColorWarpSpaceBasis& basis,
                      ColorWarpOffsetKindEnum kind,
                      const double sourceRgb[3],
                      const double offset[3],
                      double xyzShift[3])
{
    double targetRgb[3];

    if (kind == eColorWarpOffsetHsv) {
        double hsv[3];
        colorWarpRgbToHsv(sourceRgb, hsv);
        hsv[0] += offset[0];
        hsv[1] += offset[1];
        hsv[2] += offset[2];
        colorWarpHsvToRgb(hsv, targetRgb);
    } else {
        for (int c = 0; c < 3; ++c) {
            targetRgb[c] = sourceRgb[c] + offset[c];
        }
    }
    double sourceXyz[3], targetXyz[3];
    mul3(basis.rgbToXyz, sourceRgb, sourceXyz);
    mul3(basis.rgbToXyz, targetRgb, targetXyz);
    for (int c = 0; c < 3; ++c) {
        xyzShift[c] = targetXyz[c] - sourceXyz[c];
    }
}

void
hsvRgbOffsetFromShift(const ColorWarpSpaceBasis& basis,
                      ColorWarpOffsetKindEnum kind,
                      const double sourceRgb[3],
                      const double xyzShift[3],
                      double offset[3])
{
    double sourceXyz[3], targetXyz[3], targetRgb[3];

    mul3(basis.rgbToXyz, sourceRgb, sourceXyz);
    for (int c = 0; c < 3; ++c) {
        targetXyz[c] = sourceXyz[c] + xyzShift[c];
    }
    mul3(basis.xyzToRgb, targetXyz, targetRgb);
    if (kind == eColorWarpOffsetHsv) {
        double sourceHsv[3], targetHsv[3];
        colorWarpRgbToHsv(sourceRgb, sourceHsv);
        colorWarpRgbToHsv(targetRgb, targetHsv);
        offset[0] = wrapHueOffsetDegrees(targetHsv[0] - sourceHsv[0]);
        offset[1] = targetHsv[1] - sourceHsv[1];
        offset[2] = targetHsv[2] - sourceHsv[2];
    } else {
        for (int c = 0; c < 3; ++c) {
            offset[c] = targetRgb[c] - sourceRgb[c];
        }
    }
}

NATRON_NAMESPACE_EXIT
