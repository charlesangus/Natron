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

#ifndef Engine_Nodes_Time_TimeBlend_h
#define Engine_Nodes_Time_TimeBlend_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <span>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The weighted mean of several images of one input, read at several times, as a running
 * mean: the result starts as the first image and the k-th folds in as
 * result = (image - result) * blend[k] + result, blend[k] being its weight over the sum of the
 * weights up to it. The CPU and the GPU (the timeblend kernel) fold in the same order with the
 * same float arithmetic, so they match exactly. Pixels outside an image read zero.
 **/
namespace TimeBlend {
/**
 * @brief The fold blends of weights, one per image after the first; empty for fewer than two
 * weights. A zero running sum gives a blend of 0.
 **/
std::vector<float> stepBlends(std::span<const double> weights);

/**
 * @brief dst over roi, of dst's component count, the weighted mean of images, which must be
 * float; an image may be null, reading zero. False when dst is not float or an image is not.
 **/
bool renderCpu(const ImagePtr& dst, const RectI& roi, std::span<const ImagePtr> images, std::span<const double> weights);

/**
 * @brief args.output over args.outputRect, the weighted mean of views, which have the output's
 * component count, with no mask and every channel written. Needs NATRON_HAVE_GPU; fails
 * without it.
 **/
StatusEnum recordGpu(const GpuRenderArgs& args, std::span<const GpuImageView* const> views, std::span<const double> weights);
} // namespace TimeBlend

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_TimeBlend_h
