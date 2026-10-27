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

#ifndef Engine_Nodes_Filter_EdgeAware_Eigf_h
#define Engine_Nodes_Filter_EdgeAware_Eigf_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"
#include "Engine/Nodes/Filter/EdgeAware/GuidedFilter.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's exposure-independent guided filter (src/common/eigf.h) and the recursive
 * gaussian it averages with (src/common/gaussian.c).
 *
 * EIGF fits a and b like the guided filter, but with the variance and covariance normalised by
 * the product of the local average and the pixel value, so scaling the image by k scales the
 * output by k: the fit is the same at every exposure. It works on linear values, not on their
 * logarithm. The local averages are gaussian, not box, and a and b are not averaged again.
 *
 * Borders: the gaussian's recursions start from the edge sample, as if the image went on with
 * its edge value; the subsampling clamps to the last row and column.
 **/
namespace EdgeAware {
/// The coefficients of the order-0 Deriche recursion recursiveGaussianBlur() runs.
struct DericheCoefficients {
    float a0, a1, a2, a3, b1, b2, coefp, coefn;
};

/// The order-0 coefficients for sigma.
DericheCoefficients dericheCoefficients(float sigma);

/**
 * @brief sigma capped at 16 times the image's larger side. A gaussian that wide is flat across the
 * image well below float precision, while the coefficients of a much larger one lose all precision
 * and turn NaN.
 **/
inline float
clampGaussianSigma(float sigma,
                   int width,
                   int height)
{
    return std::fmin(sigma, 16.f * (float)std::max(width, height));
}

/**
 * @brief The order-0 Deriche recursive gaussian of sigma, in out, over a
 * width x height image of channels interleaved floats (1 to 4). Every input sample is clamped
 * per channel to [min[c], max[c]] first. in and out must not overlap.
 **/
void recursiveGaussianBlur(const float* in,
                           float* out,
                           int width,
                           int height,
                           int channels,
                           float sigma,
                           const float* min,
                           const float* max,
                           const ParallelFor& parallel);

/**
 * @brief In place, on a single-channel positive image.
 *
 * The statistics are taken on the image subsampled by clamp(sigma, 1, 4) with a gaussian of
 * max(sigma / subsampling, 1), then upsampled. Each of iterations blends linearly, the last one
 * with blending, outputs floored at 2^-16. quantization 0 guides the image by itself; otherwise
 * the guide is the image quantized by quantize() with quantizeMin and quantizeMax. feathering is
 * the normalised-variance regulariser. An image smaller than the subsampling on either side is
 * filtered at full resolution instead. Non-finite samples count as 0 and come out unchanged.
 **/
void eigfSurfaceBlur(float* image,
                     int width,
                     int height,
                     float sigma,
                     float feathering,
                     int iterations,
                     GuidedBlendingEnum blending,
                     float quantization,
                     float quantizeMin,
                     float quantizeMax,
                     const ParallelFor& parallel);
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_Eigf_h
