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

#include "DisplayLegendColour.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {
const double kPi = 3.14159265358979323846;
const double kHueLightnessTilt = 0.06;
const double kYellowHue = 110.0 * kPi / 180.0;
const int kGamutMapSteps = 18;
const double kGamutSlack = 1e-6;

double
clamp01(double v)
{
    return std::min(1.0, std::max(0.0, v));
}

bool
inGamut(const double rgb[3])
{
    for (int i = 0; i < 3; ++i) {
        if (!(rgb[i] >= -kGamutSlack) || !(rgb[i] <= 1.0 + kGamutSlack)) {
            return false;
        }
    }

    return true;
}

void
okLch(double L,
      double C,
      double h,
      double rgb[3])
{
    OkLab lab;

    lab.L = L;
    lab.a = C * std::cos(h);
    lab.b = C * std::sin(h);
    okLabToLinearSrgb(lab, rgb);
}
} // anonymous namespace

OkLab
linearSrgbToOkLab(const double rgb[3])
{
    const double l = 0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2];
    const double m = 0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2];
    const double s = 0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2];
    const double l_ = std::cbrt(l);
    const double m_ = std::cbrt(m);
    const double s_ = std::cbrt(s);
    OkLab lab;

    lab.L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    lab.a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    lab.b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;

    return lab;
}

void
okLabToLinearSrgb(const OkLab& lab,
                  double rgb[3])
{
    const double l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s_ = lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;

    rgb[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    rgb[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    rgb[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

double
srgbEncode(double linear)
{
    const double x = clamp01(linear);

    return (x <= 0.0031308) ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

double
srgbDecode(double encoded)
{
    const double x = clamp01(encoded);

    return (x <= 0.04045) ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}

void
okLchToGamutLinearSrgb(double L,
                       double C,
                       double hRadians,
                       double rgb[3])
{
    okLch(L, C, hRadians, rgb);
    if (!inGamut(rgb)) {
        double lo = 0.0;
        double hi = std::max(0.0, C);

        for (int k = 0; k < kGamutMapSteps; ++k) {
            const double mid = 0.5 * (lo + hi);

            okLch(L, mid, hRadians, rgb);
            if (inGamut(rgb)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        okLch(L, lo, hRadians, rgb);
    }
    for (int i = 0; i < 3; ++i) {
        rgb[i] = clamp01(rgb[i]);
    }
}

double
displayLegendHueLightness(double hRadians)
{
    return kDisplayLegendNeutralLightness + kHueLightnessTilt * std::cos(hRadians - kYellowHue);
}

std::vector<double>
displayLegendHues(std::vector<float> workingRgb,
                  const DisplayLegendConverter& converter)
{
    const int count = (int)(workingRgb.size() / 3);
    std::vector<double> hues(count, 0.0);

    if (count <= 0) {
        return hues;
    }
    if (converter) {
        converter(workingRgb.data(), count);
    }
    for (int k = 0; k < count; ++k) {
        double rgb[3];

        for (int i = 0; i < 3; ++i) {
            const double v = workingRgb[3 * k + i];

            rgb[i] = converter ? srgbDecode(v) : std::max(0.0, v);
        }

        const OkLab lab = linearSrgbToOkLab(rgb);

        hues[k] = std::atan2(lab.b, lab.a);
    }

    return hues;
}

NATRON_NAMESPACE_EXIT
