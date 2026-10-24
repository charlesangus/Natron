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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): src/common/illuminants.h,
 * src/common/chromatic_adaptation.h, the xyY, u'v'Y, Lab and LCh conversions of
 * src/common/colorspaces_inline_conversions.h and _check_if_close_to_daylight() of
 * src/iop/channelmixerrgb.c. darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2020-2025 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Engine_Nodes_Raw_RawColorCalibrationMath_h
#define Engine_Nodes_Raw_RawColorCalibrationMath_h

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
 * @brief The illuminant and chromatic adaptation maths of darktable's color calibration module
 * (channelmixerrgb). Everything is single precision, as in darktable, so that the node
 * reproduces its results. Matrices are row-major 3x3, multiplying column vectors.
 **/
namespace RawColorCalibrationMath {
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Mat3 = std::array<float, 9>;

struct Xy {
    float x;
    float y;
};

/**
 * @brief dt_illuminant_t. The values are darktable's and are stored in its parameters; the two
 * detection modes come after Last as they do there.
 **/
enum class Illuminant : int {
    Pipe = 0,
    A = 1,
    D = 2,
    E = 3,
    F = 4,
    Led = 5,
    BlackBody = 6,
    Custom = 7,
    Camera = 10,
    Last = 11,
    DetectSurfaces = 8,
    DetectEdges = 9,
};

/**
 * @brief dt_illuminant_fluo_t: the CIE fluorescent illuminants F1 to F12.
 **/
enum class Fluorescent : int {
    F1 = 0,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Last
};

/**
 * @brief dt_illuminant_led_t: the CIE LED illuminants.
 **/
enum class Led : int {
    B1 = 0,
    B2,
    B3,
    B4,
    B5,
    BH1,
    RGB1,
    V1,
    V2,
    Last
};

/**
 * @brief dt_adaptation_t.
 **/
enum class Adaptation : int {
    LinearBradford = 0,
    Cat16 = 1,
    FullBradford = 2,
    Xyz = 3,
    Rgb = 4,
    Last = 5
};

/**
 * @brief darktable's $DESCRIPTION labels (untranslated), or nullptr for a value that has none
 * (the detection modes and the Last values).
 **/
const char* illuminantLabel(Illuminant illuminant);
const char* fluorescentLabel(Fluorescent fluorescent);
const char* ledLabel(Led led);
const char* adaptationLabel(Adaptation adaptation);

/**
 * @brief The xy chromaticity of the fluorescent and LED tables of illuminants.h.
 **/
Xy fluorescentXy(Fluorescent fluorescent);
Xy ledXy(Led led);

/**
 * @brief McCamy-style correlated colour temperature of an xy chromaticity (Lee's fit, meant for
 * 3000 to 50000 K), 0 when x is FLT_MAX or above. The result is returned however far off it is.
 **/
float xyToCct(float x,
              float y);

/**
 * @brief The CIE daylight locus at t, valid in 4000 to 25000 K. Outside, x is 0 and y is -0.275,
 * which illuminantToXy() takes as "out of range".
 **/
Xy cctToXyDaylight(float t);

/**
 * @brief The Planckian locus at t (Kim et al. cubic splines), valid in 1667 to 25000 K; zeros
 * outside.
 **/
Xy cctToXyBlackbody(float t);

/**
 * @brief The XYZ of an illuminant of chromaticity xy at Y = 1.
 **/
Vec3 illuminantXyToXyz(float x,
                       float y);

/**
 * @brief CIE 1960 uv of an xy chromaticity.
 **/
std::array<float, 2> xyToUv(const Xy& xy);

/**
 * @brief The temperature of the point of the daylight (from 4000 K) or Planckian (below) locus
 * nearest xy, by brute-force search over 65536 temperatures from 1667 to 25000 K.
 **/
float cctReverseLookup(float x,
                       float y);

/**
 * @brief What darktable reads from a raw image to find its as-shot illuminant.
 **/
struct RawCameraWhite {
    /** dt_image_is_matrix_correction_supported(): raw and not monochrome. */
    bool matrixCorrectionSupported = true;
    /** DT_IMAGE_4BAYER: four coefficients are checked rather than three. */
    bool fourColors = false;
    Vec4 wbCoeffs = { 0.f, 0.f, 0.f, 0.f };
    /** d65_color_matrix, a DNG's embedded XYZ to camera matrix; preferred when present. */
    std::optional<Mat3> d65ColorMatrix;
    /** adobe_XYZ_to_CAM, the standard XYZ (D65) to camera matrix. */
    std::optional<Mat3> adobeXyzToCam;
};

/**
 * @brief dcraw's pseudoinverse as darktable has it: out (size x 3) is the transpose of
 * in's (size x 3) pseudo-inverse, so a camera's XYZ to camera matrix gives the transposed
 * camera to XYZ matrix.
 **/
void matricePseudoinverse(const float (*in)[3],
                          float (*out)[3],
                          int size);

/**
 * @brief The xy, adapted to D50 by linear Bradford, of camera RGB (1/wb) through camToXyzT, the
 * transposed camera to XYZ (D65) matrix matricePseudoinverse() returns.
 **/
Xy wbCoeffsToIlluminantXy(const Mat3& camToXyzT,
                          const Vec3& wb);

/**
 * @brief The as-shot illuminant chromaticity of a raw image: its coefficients, each multiplied by
 * customWb when given, through its camera matrix. nullopt where darktable returns FALSE.
 **/
std::optional<Xy> findTemperatureFromRawCoeffs(const RawCameraWhite& camera,
                                               const Vec4* customWb);

/**
 * @brief The chromaticity of an illuminant: t is the temperature for D and BlackBody, fluorescent
 * and led select F and Led. D falls back to BlackBody out of its range and BlackBody to Camera,
 * which needs camera. nullopt for Custom, the detection modes, and every failure.
 **/
std::optional<Xy> illuminantToXy(Illuminant illuminant,
                                 const RawCameraWhite* camera,
                                 const Vec4* customWb,
                                 float t,
                                 Fluorescent fluorescent,
                                 Led led);

struct DaylightCheck {
    float temperature;
    /** BlackBody or D when the locus at temperature lies within 0.005 in uv of xy, else Custom. */
    Illuminant illuminant;
    /** Always Cat16. */
    Adaptation adaptation;
};

/**
 * @brief _check_if_close_to_daylight(): the temperature of xy and the illuminant model that fits it.
 **/
DaylightCheck checkIfCloseToDaylight(float x,
                                     float y);

Vec3 convertXyzToBradfordLms(const Vec3& xyz);
Vec3 convertBradfordLmsToXyz(const Vec3& lms);
Vec3 convertXyzToCat16Lms(const Vec3& xyz);
Vec3 convertCat16LmsToXyz(const Vec3& lms);
Vec3 convertAnyXyzToLms(const Vec3& xyz,
                        Adaptation kind);
Vec3 convertAnyLmsToXyz(const Vec3& lms,
                        Adaptation kind);

/**
 * @brief XYZ to LMS times rgbToXyz, and LMS to XYZ times lms.
 **/
Mat3 makeRgbToBradfordLms(const Mat3& rgbToXyz);
Mat3 makeBradfordLmsToXyz(const Mat3& lms);
Mat3 makeRgbToCat16Lms(const Mat3& rgbToXyz);
Mat3 makeCat16LmsToXyz(const Mat3& lms);

/**
 * @brief Bradford adaptation of lmsIn from originIlluminant (Bradford LMS) to D50; full applies
 * the non-linear blue exponent p = (origin[2] / D50[2])^0.0834 to positive values.
 **/
Vec3 bradfordAdaptD50(const Vec3& lmsIn,
                      const Vec3& originIlluminant,
                      float p,
                      bool full);

/**
 * @brief CAT16 adaptation of lmsIn from originIlluminant (CAT16 LMS) to D50 with the degree of
 * adaptation D, or completely when full.
 **/
Vec3 cat16AdaptD50(const Vec3& lmsIn,
                   const Vec3& originIlluminant,
                   float D,
                   bool full);

/**
 * @brief Von Kries scaling in XYZ from originIlluminant to D50.
 **/
Vec3 xyzAdaptD50(const Vec3& xyzIn,
                 const Vec3& originIlluminant);

/**
 * @brief D50 in the adaptation's LMS space (ones for Rgb and Last).
 **/
Vec3 convertD50ToLms(Adaptation adaptation);

/**
 * @brief Adapts an XYZ pixel from illuminant (in the adaptation's LMS space, Y = 1) to D50; p is
 * the full Bradford exponent. CAT16 always adapts completely.
 **/
Vec3 chromaAdaptPixel(const Vec3& in,
                      const Vec3& illuminant,
                      Adaptation adaptation,
                      float p);

/**
 * @brief CIE Lab with darktable's D50 white (0.9642, 1, 0.8249).
 **/
Vec3 xyzToLab(const Vec3& xyz);
Vec3 labToXyz(const Vec3& lab);

/**
 * @brief Lab to LCh with the hue in [0, 1] turns, and back (dt_Lab_2_LCH(), dt_LCH_2_Lab()).
 **/
Vec3 labToLch(const Vec3& lab);
Vec3 lchToLab(const Vec3& lch);

/**
 * @brief XYZ to xyY, negative components clipped to 0, with D50's chromaticity for black.
 **/
Vec3 d50XyzToXyY(const Vec3& xyz);

/**
 * @brief xyY to XYZ, zeros when y is 0.
 **/
Vec3 xyYToXyz(const Vec3& xyY);

/**
 * @brief CIE 1976 u'v'Y.
 **/
Vec3 xyYToUvY(const Vec3& xyY);
Vec3 uvYToXyY(const Vec3& uvY);

/**
 * @brief CIE L*u*v* relative to D50 with Yn = 1, and its polar form with the hue in radians in
 * [0, 2 pi).
 **/
Vec3 xyYToLuv(const Vec3& xyY);
Vec3 luvToLch(const Vec3& luv);
Vec3 xyYToLch(const Vec3& xyY);
Vec3 luvToXyY(const Vec3& luv);
Vec3 lchToLuv(const Vec3& lch);
Vec3 lchToXyY(const Vec3& lch);

/**
 * @brief dt_iop_channelmixer_rgb_version_t: the colorfulness algorithm.
 **/
enum class SaturationVersion : int {
    V1 = 0,
    V2 = 1,
    V3 = 2
};

/**
 * @brief dt_iop_channelmixer_rgb_params_t, at darktable's static defaults (init() makes the mix
 * the identity).
 **/
struct Settings {
    Vec3 red = { 1.f, 0.f, 0.f };
    Vec3 green = { 0.f, 1.f, 0.f };
    Vec3 blue = { 0.f, 0.f, 1.f };
    Vec3 saturation = { 0.f, 0.f, 0.f };
    Vec3 lightness = { 0.f, 0.f, 0.f };
    Vec3 grey = { 0.f, 0.f, 0.f };
    bool normalizeR = false;
    bool normalizeG = false;
    bool normalizeB = false;
    bool normalizeSaturation = false;
    bool normalizeLightness = false;
    bool normalizeGrey = false;
    Illuminant illuminant = Illuminant::D;
    Fluorescent fluorescent = Fluorescent::F3;
    Led led = Led::B5;
    Adaptation adaptation = Adaptation::Cat16;
    float x = 0.333f;
    float y = 0.333f;
    float temperature = 5003.f;
    float gamut = 1.f;
    bool clip = true;
    SaturationVersion version = SaturationVersion::V3;
};

/**
 * @brief What the pixel loop reads: commit_params()'s dt_iop_channelmixer_rbg_data_t and the
 * matrices _loop_switch() derives from the working profile. Plain data, so that a GPU kernel can
 * take it as it is.
 **/
struct Kernel {
    Mat3 rgbToXyz;
    Mat3 xyzToRgb;
    Mat3 rgbToLms;
    Mat3 mixToXyz;
    Vec3 illuminant;
    Vec3 saturation;
    Vec3 lightness;
    Vec3 grey;
    float p;
    float gamut;
    Adaptation adaptation;
    SaturationVersion version;
    bool clip;
    bool applyGrey;
};

/**
 * @brief commit_params() for working RGB whose matrix to XYZ (D50) is rgbToXyz, and xyzToRgb its
 * inverse. camera and customWb are what illuminantToXy() needs for the camera illuminant.
 **/
Kernel makeKernel(const Settings& settings,
                  const Mat3& rgbToXyz,
                  const Mat3& xyzToRgb,
                  const RawCameraWhite* camera,
                  const Vec4* customWb);

/**
 * @brief One pixel of _loop_switch(): the three colour channels of in to out.
 **/
void processPixel(const Kernel& kernel,
                  const float* in,
                  float* out);
} // namespace RawColorCalibrationMath

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorCalibrationMath_h
