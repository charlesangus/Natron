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

#ifndef Engine_Nodes_Filter_EdgeAware_GuidedFilter_h
#define Engine_Nodes_Filter_EdgeAware_GuidedFilter_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Guided filters (He, Sun and Tang, "Guided Image Filtering"), ported from darktable's
 * src/common/guided_filter.c and src/common/fast_guided_filter.h.
 *
 * Borders: the box means use darktable's shrinking window (see BoxFilter.h), and the
 * subsampling of the fast variants clamps to the last row and column.
 **/
namespace EdgeAware {
/// darktable's subsampling factor for its fast guided filter.
const int kFastGuidedSubsampling = 4;

/// darktable's dt_iop_guided_filter_blending_t.
enum GuidedBlendingEnum {
    eGuidedBlendingLinear = 0,
    eGuidedBlendingGeomean
};

/**
 * @brief darktable's guided_filter(): filters the single-channel in with a colour guide of
 * guideChannels interleaved floats per pixel (at least 3, the first three used).
 *
 * radius is the box radius w, sqrtEps the square root of the regulariser, guideWeight a
 * factor balancing the guide's amplitude against the input's, and the result is clamped to
 * [min, max]. Processed in tiles of 512 pixels (or 3 radius, whichever is larger) with a
 * 3 radius overlap, as darktable does to bound memory. A pixel whose input or guide is
 * non-finite gets its input value; elsewhere those samples count as 0.
 **/
void guidedFilter(const float* guide,
                  int guideChannels,
                  const float* in,
                  float* out,
                  int width,
                  int height,
                  int radius,
                  float sqrtEps,
                  float guideWeight,
                  float min,
                  float max,
                  const ParallelFor& parallel);

/**
 * @brief The single-channel guided filter at full resolution: out = mean(a) guide + mean(b),
 * where a and b fit target = a guide + b over each box of the given radius with the variance
 * regulariser eps, darktable's variance_analyse() (its feathering is eps). A pixel whose guide or
 * target is non-finite gets its target value; elsewhere those samples count as 0.
 **/
void guidedFilterGrey(const float* guide,
                      const float* target,
                      float* out,
                      int width,
                      int height,
                      int radius,
                      float eps,
                      const ParallelFor& parallel);

/**
 * @brief The fast guided filter (He and Sun, arXiv:1505.00996) as fast_surface_blur() builds it:
 * guide and target are bilinearly subsampled by subsampling, a and b are fitted and box-averaged
 * there with radius max(1, radius / subsampling), then upsampled and applied to the full
 * resolution guide.
 *
 * Its error against guidedFilterGrey() comes from the subsampling alone. It is small where the
 * guide is smooth at the scale of subsampling pixels, and largest across edges sharper than that
 * and near the image border, as darktable's resampling anchors pixels at their top-left corner
 * rather than their centre. An image smaller than subsampling pixels on either side, or subsampling below 2, falls back to
 * guidedFilterGrey().
 **/
void fastGuidedFilterGrey(const float* guide,
                          const float* target,
                          float* out,
                          int width,
                          int height,
                          int radius,
                          float eps,
                          int subsampling,
                          const ParallelFor& parallel);

/**
 * @brief darktable's fast_surface_blur(): in place, the self-guided fast guided filter on a
 * single-channel positive image, subsampled by kFastGuidedSubsampling.
 *
 * Each of iterations re-fits a and b against the image quantized by quantize(), blending
 * linearly between iterations and with blending on the last, outputs floored at 2^-16.
 * feathering is the variance regulariser. An image smaller than kFastGuidedSubsampling pixels on
 * either side is filtered at full resolution instead. Non-finite samples count as 0 and come out
 * unchanged.
 **/
void fastSurfaceBlur(float* image,
                     int width,
                     int height,
                     int radius,
                     float feathering,
                     int iterations,
                     GuidedBlendingEnum blending,
                     float quantization,
                     float quantizeMin,
                     float quantizeMax,
                     const ParallelFor& parallel);

/**
 * @brief darktable's interpolate_bilinear(): resamples a widthIn x heightIn image of channels
 * interleaved floats to widthOut x heightOut, output pixel j reading input position
 * j * widthIn / widthOut, neighbours clamped to the image.
 **/
void interpolateBilinear(const float* in,
                         int widthIn,
                         int heightIn,
                         float* out,
                         int widthOut,
                         int heightOut,
                         int channels,
                         const ParallelFor& parallel);

/**
 * @brief darktable's quantize(): posterizes size samples into exposure steps of sampling EV,
 * clamped to [clipMin, clipMax]. sampling 0 copies.
 **/
void quantize(const float* image,
              float* out,
              std::size_t size,
              float sampling,
              float clipMin,
              float clipMax,
              const ParallelFor& parallel);
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_GuidedFilter_h
