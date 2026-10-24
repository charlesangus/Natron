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

/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): the built-in
 * profiles of src/common/colorspaces.c (dt_colorspaces_init() and the profile constructors it
 * calls), their tone curves as src/iop/colorin.c and src/common/iop_profile.c sample and
 * extrapolate them, and dt_Lab_to_XYZ() of src/common/colorspaces_inline_conversions.h.
 * Copyright (C) 2009-2026 darktable developers, distributed under the GNU GPL version 3 or later,
 * and so is this port: GPL-3.0-or-later.
 */

#ifndef Engine_Nodes_Raw_RawColorProfiles_h
#define Engine_Nodes_Raw_RawColorProfiles_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>

#include "Engine/Nodes/Raw/RawColorMath.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's built-in colour profiles, each a tone curve per channel and a matrix, and
 * the way darktable's pipeline evaluates the curves: a 0x10000-entry table below 1 and a power
 * law fitted to its top above, so values over 1 are extrapolated rather than clamped.
 **/
namespace RawColorProfiles {
/**
 * @brief The tone curves of the built-in profiles, each from encoded to linear.
 **/
enum class Trc {
    Linear,
    Srgb, // Little CMS parametric type 4: 2.4, 1/1.055, 0.055/1.055, 1/12.92, 0.04045
    AdobeRgb, // gamma 2 + 51/256
    Rec709, // Little CMS parametric type 4: 1/0.45, 1/1.099, 0.099/1.099, 1/4.5, 0.081
    Pq, // SMPTE ST 2084, tabulated at 4096 points
    Hlg // BT.2100 hybrid log-gamma as darktable writes it, tabulated at 4096 points
};

/**
 * @brief The built-in profiles of dt_colorspaces_init() that are input or working profiles, in
 * its order.
 **/
enum class Space {
    Srgb,
    AdobeRgb,
    LinearRec709,
    Rec709,
    LinearRec2020,
    PqRec2020,
    HlgRec2020,
    PqP3,
    HlgP3,
    DisplayP3,
    LinearProPhoto,
    LinearXyz,
    Lab,
    LinearInfraredBgr,
    Brg
};

Trc trcOf(Space space);

/**
 * @brief The linear RGB to XYZ (D65) matrix of space. The colorants are built as
 * cmsCreateRGBProfile() builds them from the primaries and white, adapted to D50, and read back
 * to D65 as the working profile does (RawColorMath::profileCameraToXyz()); linear Rec709 keeps
 * RawColorMath::srgbToXyz(), the matrix the node has always used for it. For LinearXyz and Lab,
 * whose data is relative to the D50 profile connection space, the Bradford adaptation from D50
 * to D65.
 **/
RawColorMath::Mat3 toXyz(Space space);

/**
 * @brief The inverse of toXyz(): XYZ (D65) to linear RGB of space.
 **/
RawColorMath::Mat3 fromXyz(Space space);

/**
 * @brief The curve's exact value at x in [0, 1], as Little CMS evaluates it
 * (cmsEvalToneCurveFloat()).
 **/
double curve(Trc trc,
             double x);

/**
 * @brief Encoded to linear as colorin's _apply_tone_curves() does: below 1 linear interpolation
 * in the table of curve() (negatives read its first entry), from 1 the power law
 * dt_iop_estimate_exp() fits through the table at 0.7, 0.8, 0.9 and 1. Linear passes v through.
 **/
float decode(Trc trc,
             float v);

/**
 * @brief Linear to encoded as darktable's working profile writes it (iop_profile.c
 * _apply_tonecurves() with its output tables): the reversed curve, tabulated and extrapolated as
 * decode() is. Linear passes v through.
 **/
float encode(Trc trc,
             float v);

/**
 * @brief darktable's dt_Lab_to_XYZ(): Lab (L from 0 to 100) to XYZ relative to D50
 * (0.9642, 1, 0.8249).
 **/
std::array<float, 3> labToXyzD50(float L,
                                 float a,
                                 float b);
} // namespace RawColorProfiles

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorProfiles_h
