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
 * src/iop/toneequal.c, with what it calls from src/iop/choleski.h and
 * src/common/luminance_mask.h. darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2018-2026 darktable developers.
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

#ifndef Engine_Nodes_Color_ToneEqualizerKernels_h
#define Engine_Nodes_Color_ToneEqualizerKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's tone equalizer as functions over float buffers: the luminance mask, its
 * smoothing, the radial-basis gain curve and the auto-adjust statistics. Buffers are row-major and
 * tightly packed, colour as interleaved RGB.
 **/
namespace ToneEqualizerKernels {
/// The nine user bands, at -8, -7, ..., 0 EV.
const int kBands = 9;
/// The eight gaussians the bands are approximated by, evenly spread over [-8, 0] EV.
const int kCenters = 8;
const int kLutResolution = 10000;
/// The correction table, one entry per 1/kLutResolution EV over [-8, 0].
const int kLutSize = kCenters * kLutResolution + 1;
const float kMinEv = -8.f;
const float kMaxEv = 0.f;

/// darktable's dt_iop_luminance_mask_method_t, in its order.
enum EstimatorEnum {
    eEstimatorMean = 0,
    eEstimatorLightness,
    eEstimatorValue,
    eEstimatorNorm1,
    eEstimatorNorm2,
    eEstimatorNormPower,
    eEstimatorGeomean,
    eEstimatorCount
};

/// darktable's dt_iop_toneequalizer_filter_t, in its order.
enum DetailsEnum {
    eDetailsNone = 0,
    eDetailsAveragedGuided,
    eDetailsGuided,
    eDetailsAveragedEigf,
    eDetailsEigf,
    eDetailsCount
};

/// What the node outputs.
enum OutputEnum {
    eOutputResult = 0,
    eOutputMask,
    eOutputZones,
    eOutputCount
};

/**
 * @brief dt_iop_toneequalizer_params_t, version 2: gains, exposure and contrast boosts in EV,
 * blending in per cent of the larger image side, smoothing the gaussians' sigma.
 **/
struct Settings {
    std::array<float, kBands> gains = {};
    float blending = 5.f;
    float smoothing = 1.41421356f;
    float feathering = 1.f;
    float quantization = 0.f;
    float contrastBoost = 0.f;
    float exposureBoost = 0.f;
    DetailsEnum details = eDetailsEigf;
    EstimatorEnum estimator = eEstimatorNorm2;
    int iterations = 1;
};

/// The settings the mask depends on, so that a band edit keeps a computed mask.
bool sameMask(const Settings& a,
              const Settings& b) WARN_UNUSED_RETURN;

/**
 * @brief The sigma darktable's Curve Smoothing slider sets: sqrt(2)^(1 + slider).
 **/
float smoothingFromSlider(double slider) WARN_UNUSED_RETURN;

/**
 * @brief The weights of the eight gaussians, fitted to the bands' linear gains by least squares
 * (darktable's pseudo_solve). On an unstable fit, which darktable reports as "the interpolation
 * is unstable", the weights are the first eight linear gains and false is returned.
 **/
bool solveFactors(const std::array<float, kBands>& gainsEv,
                  float sigma,
                  std::array<float, kCenters>* factors) WARN_UNUSED_RETURN;

/**
 * @brief The linear correction at exposure (EV), the gaussians' sum clamped to [1/4, 4].
 **/
float pixelCorrection(float exposure,
                      const std::array<float, kCenters>& factors,
                      float sigma) WARN_UNUSED_RETURN;

/**
 * @brief Fills lut, of kLutSize entries, with pixelCorrection() over [-8, 0] EV.
 **/
void correctionLut(const std::array<float, kCenters>& factors,
                   float sigma,
                   float* lut);

/**
 * @brief The table entry for a mask value: log2 clamped to [-8, 0] EV, rounded to the table.
 **/
float lutCorrection(const float* lut,
                    float luminance) WARN_UNUSED_RETURN;

/**
 * @brief One pixel's luminance estimate, exposure-boosted (linear factor) and contrast-boosted
 * around fulcrum, floored at 2^-16.
 **/
float estimate(EstimatorEnum estimator,
               float r,
               float g,
               float b,
               float exposureBoost,
               float fulcrum,
               float contrastBoost) WARN_UNUSED_RETURN;

/**
 * @brief The smoothing radius in pixels, as darktable's modify_roi_in sets it: blending (per
 * cent) of the larger full-scale side at scale, the diameter less one, halved.
 **/
int maskRadius(float blending,
               int fullWidth,
               int fullHeight,
               double scale) WARN_UNUSED_RETURN;

/**
 * @brief darktable's compute_luminance_mask on a width x height RGB image: the estimate, then the
 * guided filter or EIGF of the details mode with radius. mask holds width x height floats.
 **/
void computeMask(const float* rgb,
                 int width,
                 int height,
                 const Settings& settings,
                 int radius,
                 float* mask,
                 const EdgeAware::ParallelFor& parallel);

/**
 * @brief The grey darktable's mask display shows for a mask value: [-8, 0] EV on a square root.
 **/
float displayMask(float luminance) WARN_UNUSED_RETURN;

/**
 * @brief The band a mask value falls in: its exposure rounded to the nearest of -8 ... 0 EV, as
 * an index from 0 (-8 EV).
 **/
int bandOf(float luminance) WARN_UNUSED_RETURN;

/**
 * @brief The Zones output: the mask display grey of the band a mask value falls in.
 **/
float zoneDisplay(float luminance) WARN_UNUSED_RETURN;

/**
 * @brief The 3x3 gaussian-weighted mask value around (x, y), rows clamped to the image, as
 * darktable's hover cursor reads it. NaN outside the image.
 **/
float sampleMask(const float* mask,
                 int width,
                 int height,
                 int x,
                 int y) WARN_UNUSED_RETURN;

/**
 * @brief The exposures (EV) of the 5th and 95th percentiles of a mask, from darktable's extended
 * histogram of 512 bins over [-10, 6] EV.
 **/
struct Deciles {
    float first = 0.f;
    float last = 0.f;
};

Deciles maskDeciles(const float* mask,
                    std::size_t count) WARN_UNUSED_RETURN;

/**
 * @brief The Mask Exposure Compensation (EV) that centres the mask's deciles, which were measured
 * with exposureBoost and contrastBoost (EV), on the bands: darktable's auto_adjust_exposure_boost.
 **/
float autoExposureBoost(const Deciles& deciles,
                        float exposureBoost,
                        float contrastBoost) WARN_UNUSED_RETURN;

/**
 * @brief The Mask Contrast Compensation (EV) that spreads the deciles over [-7, -1] EV, with
 * darktable's correction for the guided filter and EIGF: auto_adjust_contrast_boost.
 **/
float autoContrastBoost(const Deciles& deciles,
                        float exposureBoost,
                        float contrastBoost,
                        DetailsEnum details,
                        float feathering) WARN_UNUSED_RETURN;

/**
 * @brief darktable's built-in presets, in its order.
 **/
struct Preset {
    const char* id;
    const char* label;
    Settings settings;
};

const std::vector<Preset>& presets() WARN_UNUSED_RETURN;
} // namespace ToneEqualizerKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ToneEqualizerKernels_h
