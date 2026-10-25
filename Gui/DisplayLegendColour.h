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

#ifndef NATRON_GUI_DISPLAYLEGENDCOLOUR_H
#define NATRON_GUI_DISPLAYLEGENDCOLOUR_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief Colours for the trackball's legends (the hue ring and the S, T and V discs). A legend
 * shows which way a drag goes rather than the exact colour it makes, so it is drawn
 * display-referred: the hue of each direction is read through the working space and the
 * display conversion, and the lightness and chroma are chosen here in OKLab, so no hue is much
 * lighter or more vivid than its neighbours. Everything is linear or encoded sRGB, as the
 * display is assumed to be.
 **/
struct OkLab {
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

OkLab linearSrgbToOkLab(const double rgb[3]);
void okLabToLinearSrgb(const OkLab& lab, double rgb[3]);

/**
 * @brief The sRGB transfer functions; the argument is clamped to [0, 1].
 **/
double srgbEncode(double linear);
double srgbDecode(double encoded);

/**
 * @brief Linear sRGB in [0, 1] for OKLCh (L, C, h in radians), with C reduced at fixed L and h
 * until the colour is inside the gamut.
 **/
void okLchToGamutLinearSrgb(double L, double C, double hRadians, double rgb[3]);

/**
 * @brief The OKLab lightness of the legends' hues: a little higher for yellows than for blues,
 * so neither reads as khaki nor as near-black.
 **/
double displayLegendHueLightness(double hRadians);

/// The chroma of the hue ring and of the S disc's top edge.
constexpr double kDisplayLegendChroma = 0.14;

/// The OKLab lightness of the T disc's neutral centre.
constexpr double kDisplayLegendNeutralLightness = 0.75;

typedef std::function<void(float* rgb, int count)> DisplayLegendConverter;

/**
 * @brief The OKLab hue, in radians, of each packed working-space RGB triplet as the display
 * shows it: through converter, whose output is display-encoded, then decoded. With no converter
 * the working space is taken to be linear sRGB.
 **/
std::vector<double> displayLegendHues(std::vector<float> workingRgb, const DisplayLegendConverter& converter);

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_DISPLAYLEGENDCOLOUR_H
