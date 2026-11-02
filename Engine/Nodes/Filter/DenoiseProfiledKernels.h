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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/denoiseprofile.c, with what it calls from src/common/nlmeans_core.c,
 * src/common/eaw.c and src/common/math.h. darktable is distributed under the GNU GPL, version 3
 * or (at your option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2011-2026 darktable developers.
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

#ifndef Engine_Nodes_Filter_DenoiseProfiledKernels_h
#define Engine_Nodes_Filter_DenoiseProfiledKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Raw/RawKernels.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The maths of DenoiseProfiled on interleaved RGB buffers, three floats per pixel, rows in
 * any order: a filter whose taps are symmetric reads the same either way up.
 **/
namespace DenoiseProfiledKernels {
using Vec3 = std::array<float, 3>;
using Vec3d = std::array<double, 3>;
using Matrix3 = std::array<Vec3, 3>;
using RawKernels::ParallelFor;

// DT_IOP_DENOISE_PROFILE_BANDS: the wavelet scales, and the curve points per channel group.
const int kBands = 7;
// The search radius the canvas rule never goes below.
const int kMinCanvasSearchRadius = 4;

enum ModeEnum {
    eModeNlm = 0,
    eModeNlmAuto,
    eModeWavelets,
    eModeWaveletsAuto,
    eModeVariance,
};

enum ColorModeEnum {
    eColorModeRgb = 0,
    eColorModeY0U0V0,
};

// The curve groups: all, red, green and blue for RGB; Y0 and U0V0 for Y0U0V0.
enum CurveEnum {
    eCurveAll = 0,
    eCurveRed,
    eCurveGreen,
    eCurveBlue,
    eCurveY0,
    eCurveU0V0,
    eCurveCount
};

/**
 * @brief dt_fast_mexp2f(): 2^-x through the bits of a float, the integer version, which the
 * non-local means weights use.
 **/
float fastMexp2fInt(float x) WARN_UNUSED_RETURN;

/**
 * @brief fast_mexp2f(): the same in float arithmetic, with its lower precision, which the wavelet
 * decomposition's weights use.
 **/
float fastMexp2fFloat(float x) WARN_UNUSED_RETURN;

/**
 * @brief The radius, scattering, shadows and bias darktable derives from a profile's green a
 * (times the adjust-autoset factor in the auto modes, alone for the defaults).
 **/
int inferRadius(float a) WARN_UNUSED_RETURN;
float inferScattering(float a) WARN_UNUSED_RETURN;
float inferShadows(float a) WARN_UNUSED_RETURN;
float inferBias(float a) WARN_UNUSED_RETURN;

/**
 * @brief compute_wb_factors() as of fix_anscombe_and_nlmeans_norm: the applied white balance
 * coefficients when adaptive, their mean otherwise, and 1 when there are none (their mean is 0).
 **/
Vec3 wbFactors(const std::optional<std::array<double, 3>>& applied,
               bool adaptive) WARN_UNUSED_RETURN;

/**
 * @brief A wavelet curve's value at a band, sampled as dt_draw_curve_calc_values() stores it:
 * rounded to 16 bits and read back over 65536.
 **/
float quantizeForce(double value) WARN_UNUSED_RETURN;

/**
 * @brief precondition_v2(): the generalised Anscombe transform of n pixels, in and out may alias.
 **/
void preconditionV2(const float* in,
                    float* out,
                    std::size_t n,
                    float a,
                    const Vec3& p,
                    float b,
                    const Vec3& wb);

/**
 * @brief backtransform_v2(): its low-bias inverse in place, bias standing in for the bias term of
 * the second-order expansion.
 **/
void backtransformV2(float* buf,
                     std::size_t n,
                     float a,
                     const Vec3& p,
                     float b,
                     float bias,
                     const Vec3& wb);

/**
 * @brief set_up_conversion_matrices(): the white-balance-adapted RGB to Y0U0V0 matrix and its
 * inverse, falling back to equal Y0 weights when the adapted matrix is singular.
 **/
void conversionMatrices(const Vec3& wb,
                        Matrix3* toY0U0V0,
                        Matrix3* toRgb);

/**
 * @brief invert_matrix(): false, leaving *out alone, for a singular matrix.
 **/
bool invertMatrix(const Matrix3& in,
                  Matrix3* out) WARN_UNUSED_RETURN;

/**
 * @brief precondition_Y0U0V0(): the transform per channel, then toY0U0V0.
 **/
void preconditionY0U0V0(const float* in,
                        float* out,
                        std::size_t n,
                        float a,
                        const Vec3& p,
                        float b,
                        const Matrix3& toY0U0V0);

/**
 * @brief backtransform_Y0U0V0(): toRgb, then the low-bias inverse per channel, in place.
 **/
void backtransformY0U0V0(float* buf,
                         std::size_t n,
                         float a,
                         const Vec3& p,
                         float b,
                         float bias,
                         const Vec3& wb,
                         const Matrix3& toRgb);

/**
 * @brief What one denoise needs, resolved from the knobs, the profile and the render scale.
 * profileA and profileB are the profile's green a and b. force holds the curves' values at the
 * seven bands, coarsest first.
 **/
struct Settings {
    ModeEnum mode = eModeWavelets;
    ColorModeEnum colorMode = eColorModeY0U0V0;
    float profileA = 1e-4f;
    float profileB = 0.f;
    float radius = 1.f;
    int searchRadius = 7;
    float strength = 1.f;
    float shadows = 1.f;
    float bias = 0.f;
    float scattering = 0.f;
    float centralPixelWeight = 0.1f;
    Vec3 wb = { 1.f, 1.f, 1.f };
    float force[eCurveCount][kBands] = {};
};

/**
 * @brief The render scale the non-local means path runs at, min(scale, 1), and its patch and
 * search geometry there: P = ceil(radius x scale), and under 1 darktable's canvas rule for the
 * search radius and scattering.
 **/
struct NlmGeometry {
    float scale = 1.f;
    int patchRadius = 0;
    int searchRadius = 0;
    float scattering = 0.f;
    // How far the farthest scattered patch is.
    int maxShift = 0;

    // The pixels around a window the path reads.
    int halo() const WARN_UNUSED_RETURN
    {
        return maxShift + patchRadius;
    }
};

NlmGeometry nlmGeometry(const Settings& settings,
                        double renderScale) WARN_UNUSED_RETURN;

/**
 * @brief The scattered shifts of g, a row and a column each, in the order denoiseNlm() walks them.
 **/
std::vector<std::array<int, 2>> nlmPatches(const NlmGeometry& g) WARN_UNUSED_RETURN;

/**
 * @brief process_nlmeans() on a width x height buffer: preconditions it, denoises the window of
 * windowWidth x windowHeight pixels at (windowX, windowY) with the buffer's edges as the image's,
 * and back-transforms the result into out (windowWidth x windowHeight pixels).
 **/
void denoiseNlm(const float* in,
                int width,
                int height,
                int windowX,
                int windowY,
                int windowWidth,
                int windowHeight,
                const Settings& settings,
                double renderScale,
                float* out,
                const ParallelFor& parallel);

/**
 * @brief process_variance(): the red, green and blue sample variance of n interleaved RGB pixels
 * after the variance-stabilising transform, which is 1 where the profile fits a uniform patch.
 **/
Vec3d measureVariance(const float* in,
                      std::size_t n,
                      const Settings& settings) WARN_UNUSED_RETURN;

/**
 * @brief The number of wavelet scales process_wavelets() uses at renderScale for an image whose
 * larger side is fullSize pixels at full scale.
 **/
int waveletScales(double renderScale,
                  int fullSize) WARN_UNUSED_RETURN;

/**
 * @brief The factor of the curves on a channel's BayesShrink threshold at wavelet level scale of
 * maxScale.
 **/
Vec3 waveletAdjust(int scale,
                   int maxScale,
                   const Settings& settings) WARN_UNUSED_RETURN;

/**
 * @brief process_wavelets() on the whole width x height image in, into out. fullSize is the
 * image's larger side at full scale.
 **/
void denoiseWavelets(const float* in,
                     int width,
                     int height,
                     int fullSize,
                     const Settings& settings,
                     double renderScale,
                     float* out,
                     const ParallelFor& parallel);
} // namespace DenoiseProfiledKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_DenoiseProfiledKernels_h
