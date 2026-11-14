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

#ifndef Engine_Nodes_Filter_EdgeAware_BoxFilter_h
#define Engine_Nodes_Filter_EdgeAware_BoxFilter_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's box filters (src/common/box_filters.cc): a moving mean over a
 * (2 radius + 1) square window, separable and O(1) per pixel whatever the radius.
 *
 * Borders: darktable's shrinking window. The mean at a pixel is over the window's samples that
 * lie inside the image, so nothing is mirrored or extended. A radius larger than the image makes
 * every output the mean of the whole row, then column, as in darktable.
 *
 * The running sums accumulate from the buffer's first row and column, so their rounding depends
 * on a pixel's position in the buffer: the same pixels at another offset differ in the last bits.
 * Under the whole-frame contract (EdgeAwareCommon.h) the buffer never moves, so this never
 * reaches a node's output.
 **/
namespace EdgeAware {
/// The most interleaved channels the box filters accept.
const int kBoxMaxChannels = 16;

/// A plain running sum, or a Kahan-compensated one.
enum BoxSummationEnum {
    eBoxSummationPlain = 0,
    eBoxSummationKahan
};

/**
 * @brief In place, iterations times, on a width x height image of channels
 * interleaved floats (1 to kBoxMaxChannels). Non-finite samples count as 0 and come out
 * unchanged.
 **/
void boxMean(float* buf,
             int width,
             int height,
             int channels,
             int radius,
             int iterations,
             BoxSummationEnum summation,
             const ParallelFor& parallel);

/**
 * @brief One horizontal pass in place over a single row of width pixels.
 * scratch holds at least channels * width floats. No non-finite handling.
 **/
void boxMeanRow(float* row,
                int width,
                int channels,
                int radius,
                BoxSummationEnum summation,
                float* scratch);

/**
 * @brief One vertical pass in place over a whole image. No non-finite
 * handling.
 **/
void boxMeanColumns(float* buf,
                    int width,
                    int height,
                    int channels,
                    int radius,
                    BoxSummationEnum summation,
                    const ParallelFor& parallel);

/**
 * @brief The local mean and variance of a single-channel image over the box window, from the box
 * means of the image and its square (compensated sums), the variance clamped at 0. A non-finite
 * input counts as 0, and its pixel gets the input value as mean and 0 as variance.
 **/
void boxMeanVariance(const float* in,
                     int width,
                     int height,
                     int radius,
                     float* mean,
                     float* variance,
                     const ParallelFor& parallel);
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_BoxFilter_h
