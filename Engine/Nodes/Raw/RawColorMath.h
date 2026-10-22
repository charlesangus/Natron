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

#ifndef Engine_Nodes_Raw_RawColorMath_h
#define Engine_Nodes_Raw_RawColorMath_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <optional>

NATRON_NAMESPACE_ENTER

/**
 * @brief The colour maths of the raw white balance and input colour nodes, ported from
 * darktable's temperature module and colour space helpers: correlated colour temperature and
 * tint to CIE XYZ and back, and the camera matrices those go through. Matrices are row-major
 * 3x3, multiplying column vectors.
 **/
namespace RawColorMath {
using Mat3 = std::array<double, 9>;
using Vec3 = std::array<double, 3>;

const double kLowestTemperature = 1901.;
const double kHighestTemperature = 25000.;
const double kLowestTint = 0.135;
const double kHighestTint = 2.326;

/**
 * @brief The inverse of m, or nullopt when it is singular.
 **/
std::optional<Mat3> invert(const Mat3& m);

Mat3 multiply(const Mat3& a,
              const Mat3& b);

Vec3 apply(const Mat3& m,
           const Vec3& v);

/**
 * @brief Linear sRGB (D65) to XYZ, the matrix darktable's temperature module uses for an image
 * that has no camera matrix.
 **/
Mat3 srgbToXyz();

/**
 * @brief darktable's camera matrices, XYZ_to_CAM and its pseudo-inverse CAM_to_XYZ, built from
 * a camera's XYZ (D65) to camera RGB matrix (dt_colorspaces_conversion_matrices_xyz()).
 **/
struct CameraMatrices {
    double xyzToCam[4][3];
    double camToXyz[3][4];
};

CameraMatrices cameraMatrices(const Mat3& xyzToCam);

/**
 * @brief The matrices darktable uses for an image that is not raw: sRGB.
 **/
CameraMatrices srgbCameraMatrices();

/**
 * @brief The white of a black body (below 4000 K) or of CIE daylight at tempK, clamped to the
 * supported range, scaled so its largest component is 1 (darktable's _temperature_to_XYZ()).
 **/
Vec3 temperatureToXyz(double tempK);

/**
 * @brief Camera multipliers that make the white at tempK and tint neutral, normalised so that
 * green is 1 (darktable's _temp2mul() followed by the slider callback's normalisation).
 **/
Vec3 multipliersForTemperature(const CameraMatrices& matrices,
                               double tempK,
                               double tint);

/**
 * @brief The temperature and tint whose white the multipliers coeffs make neutral, by the
 * binary search of darktable's _mul2temp(), clamped to the supported ranges.
 **/
void temperatureForMultipliers(const CameraMatrices& matrices,
                               const Vec3& coeffs,
                               float* tempK,
                               float* tint);

/**
 * @brief darktable's "camera reference" white balance: the multipliers that make D65 white
 * neutral through xyzToCam, normalised so that green is 1 (_calculate_bogus_daylight_wb(), with
 * dt_colorspaces_conversion_matrices_rgb()). nullopt when a row of the matrix sums to zero.
 **/
std::optional<Vec3> cameraReferenceMultipliers(const Mat3& xyzToCam);

/**
 * @brief The camera RGB to XYZ (D65) matrix of the RGB space whose primaries are the
 * chromaticities of camToXyz's columns and whose white, camera RGB (1, 1, 1), is D65 at Y = 1:
 * the matrix of the profile darktable's input colour module builds from a camera matrix
 * (dt_colorspaces_create_xyzmatrix_profile()). nullopt when the primaries are degenerate.
 **/
std::optional<Mat3> whiteNormalisedCameraToXyz(const Mat3& camToXyz);

/**
 * @brief darktable's D65 white, XYZ with Y = 1.
 **/
Vec3 d65White();
} // namespace RawColorMath

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorMath_h
