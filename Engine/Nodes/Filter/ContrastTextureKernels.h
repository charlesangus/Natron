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

#ifndef Engine_Nodes_Filter_ContrastTextureKernels_h
#define Engine_Nodes_Filter_ContrastTextureKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's contrast & texture module (src/iop/contrastntexture.c, params version 2):
 * local contrast in log luminance over a pyramid of exposure-invariant guided filter (EIGF)
 * blurs, with shadows and highlights set on the coarsest low-pass.
 *
 * Images are row-major, top-down and tightly packed, channels interleaved, as the edge-aware
 * library takes them: the EIGF subsampling clamps at the last row and column, so the orientation
 * changes the result.
 **/
namespace ContrastTextureKernels {
/// darktable's MAX_ITERATIONS: the pyramid never has more levels.
const int kMaxLevels = 20;
/// darktable's NORM_MIN, 2^-16.
const float kNormMin = 1.52587890625e-05f;
/// The scene grey the shadows and highlights curve pivots on, before the noise bias.
const float kPivot = 0.1845f;
const float kMidtonesWidth = 0.3f;
const float kDefaultFeathering = 0.2f;

/// What the node outputs: the image, or one of darktable's mask previews.
enum DisplayEnum {
    eDisplayOff = 0,
    eDisplayCoarse,
    eDisplayMedium,
    eDisplayFine,
    eDisplayLowpass
};

/// The detail bands the three local contrast gains act on.
enum BandEnum {
    eBandCoarse = 0,
    eBandMedium,
    eBandFine,
    eBandCount
};

/// dt_iop_contrastntexture_params_t, with the GUI's mask preview.
struct Params {
    float gainCoarse = 0.f;
    float gainMedium = 0.f;
    float gainFine = 0.f;
    float detailLevel = 5.f;
    float haloControl = 0.f;
    float noiseBias = 0.001f;
    float gainShadows = 0.f;
    float gainHighlights = 0.f;
    DisplayEnum display = eDisplayOff;
};

/// What commit_params(), modify_roi_in() and the top of process() derive from the params.
struct Plan {
    float radius[kMaxLevels];
    float noiseBias[kMaxLevels];
    float gainPerLevel[kMaxLevels];
    int maxUsedLevel;
    float maxDetailLevel;
    float feathering;
    float slopeShadows;
    float slopeHighlights;
    float polynomial[3];
    DisplayEnum display;
};

/**
 * @brief The plan for an image whose larger side is fullSize pixels at full resolution, rendered
 * at renderScale. The detail levels follow the full-resolution size and the blur radii the
 * rendered one, so a proxy render filters the same features.
 **/
Plan makePlan(const Params& params, float fullSize, float renderScale) WARN_UNUSED_RETURN;

/// The squared sine and cosine crossfade weight of band at level.
float bandWeight(int band, int level, float maxLevel) WARN_UNUSED_RETURN;

/// The Wiener-weighted log2 ratio of a level's high-pass to its low-pass, in [-5, 5] EV.
float extractDetails(float highpass, float lowpass, float noiseBias) WARN_UNUSED_RETURN;

/// The shadows and highlights correction, in EV, of a final low-pass value.
float shadowsHighlights(float lowpass, const Plan& plan) WARN_UNUSED_RETURN;

/**
 * @brief darktable's RGB euclidean norm of a pixel's first three channels (all of them when it
 * has fewer, scaled to match a grey pixel), floored at 2^-16, over sqrt(3).
 **/
float pixelLuminance(const float* pixel, int nComps) WARN_UNUSED_RETURN;

/// Whether params leave every pixel unchanged.
bool isIdentity(const Params& params) WARN_UNUSED_RETURN;

/**
 * @brief The per-pixel value writePixel() takes, for each of width x height pixels of image:
 * the multiplier, the detail preview's grey or the low-pass, as plan.display says.
 **/
void computeMap(const float* image,
                int width,
                int height,
                int nComps,
                const Plan& plan,
                float* map,
                const EdgeAware::ParallelFor& parallel);

/**
 * @brief The output pixel for an input pixel of nComps channels and its computeMap() value. A
 * fourth channel is alpha: kept, or 1 in a preview.
 **/
void writePixel(const float* in, float mapValue, const Plan& plan, int nComps, float* out);
} // namespace ContrastTextureKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_ContrastTextureKernels_h
