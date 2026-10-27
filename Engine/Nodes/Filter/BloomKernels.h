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

#ifndef Engine_Nodes_Filter_BloomKernels_h
#define Engine_Nodes_Filter_BloomKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Image/PassExecutor.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The bloom that Bloom and Glow share: the mean of count blurs of sizes size * ratio^i,
 * i = 0 to count - 1, each capped at kMaxBlurSize and each one of Blur's filters.
 *
 * The Gaussian (FIR) blurs run on a decimated pyramid. Each level halves the previous one along
 * one axis with the [1 3 3 1] / 8 kernel, and a blur of standard deviation sigma along an axis runs
 * on the deepest level k along it where sigma / 2^k is still at least 2.5, so that every FIR
 * kernel reads at most 15 samples on each side. Its standard deviation there is the remainder,
 * sqrt((sigma / 2^k)^2 - (1 - 4^-k) / 4), the variance the downsampling already added taken out.
 * The blurred level is brought back with Catmull-Rom (bicubic) upsampling and added in. Below
 * sigma 5 a blur runs at full resolution, where it is the FIR Gaussian Blur applies.
 *
 * The other filters run every blur at full resolution, as Blur runs them.
 *
 * Level sample i of a level 2^k times smaller along an axis is centred at 2^k i + 2^(k-1) along
 * it, in absolute pixel coordinates, so the pyramid does not depend on the rect it is computed
 * over and a tile matches the full frame. The levels are computed over the samples whose kernel
 * reaches into the previous level; outside the samples a level holds, the reads repeat its edge
 * (nearest) or are zero.
 **/
namespace BloomKernels {
/// The largest count and ratio the Bloom and Glow knobs accept; normalised() caps them too.
constexpr int kMaxCount = 20;
constexpr double kMaxRatio = 8.;
/// Each blur's size, in pixels at the render scale, is capped at this: far wider than any image,
/// so that every halo, pyramid factor and region of interest stays well inside int.
constexpr double kMaxBlurSize = 65536.;

/// The blurs, in pixels at the render scale.
struct Settings {
    double sizeX = 0.;
    double sizeY = 0.;
    double ratio = 2.;
    int count = 5;
    BlurKernels::Filter filter = BlurKernels::eFilterFIRGaussian;
};

/// CImg Bloom's rules: at least one blur, one blur when the ratio is at most 1, and ratio 1 then.
/// The count and the ratio are capped at kMaxCount and kMaxRatio.
Settings normalised(const Settings& settings) WARN_UNUSED_RETURN;

/**
 * @brief Whether the bloom leaves the image unchanged: its largest blur, of size
 * size * ratio^(count - 1) capped at kMaxBlurSize, is the identity by Blur's rule (sigma below
 * 0.1 for the recursive Gaussians, no FIR sample, a box of at most 1 pixel).
 **/
bool isIdentity(const Settings& settings) WARN_UNUSED_RETURN;

struct Plan;
using PlanPtr = std::shared_ptr<const Plan>;

/// The plan of the bloom of settings; nearest repeats the edge outside the image, else it is black.
PlanPtr makePlan(const Settings& settings, bool nearest) WARN_UNUSED_RETURN;

/// How far, in pixels, the bloom reads beyond what it writes.
int haloX(const Plan& plan) WARN_UNUSED_RETURN;
int haloY(const Plan& plan) WARN_UNUSED_RETURN;

/**
 * @brief Appends the passes that write the bloom of scratch slot input into slot acc, both
 * nComps-plane slots of the pass list: an extent-only pass carrying the halo, growX and growY,
 * then point passes. The levels live in memory the passes own, outside the scratch slots.
 **/
void appendPasses(const PlanPtr& plan, int input, int acc, int growX, int growY, std::vector<Pass>* passes);

#ifdef NATRON_HAVE_GPU
/// A tightly packed scratch buffer for nComps floats per pixel of rect, at least one float.
bool gpuScratch(gpu::GpuRecordContext& ctx, const RectI& rect, int nComps, gpu::GpuBuffer** buffer) WARN_UNUSED_RETURN;

/// The image of a tightly packed buffer over rect.
gpu::GpuBlurImage gpuScratchImage(const gpu::GpuBuffer* buffer, const RectI& rect, int nComps) WARN_UNUSED_RETURN;

/// The float index of pixel (x, y) of image, or false when the image exceeds 32-bit indices.
bool gpuFloatIndex(const gpu::GpuBlurImage& image, int x, int y, int nComps, uint32_t* out) WARN_UNUSED_RETURN;

/**
 * @brief Records the source of io into a tightly packed buffer over io.domain, as the CPU's
 * fillBuffer() fills slot 0: with the nearest pixel of the source repeated, else zero outside it.
 * *buffer is the filled buffer; an empty domain gives a one-float buffer and records nothing.
 **/
StatusEnum recordFill(const PassGpuIO& io, bool nearest, gpu::GpuBuffer** buffer) WARN_UNUSED_RETURN;

/**
 * @brief Records the bloom of input, a tightly packed buffer of io.nComps floats per pixel over
 * io.domain, into dst over io.output, zero outside io.domain as the CPU writes it. The
 * pyramid's levels and the blurred buffers come from io.context's scratch.
 **/
StatusEnum recordGpu(const Plan& plan, const PassGpuIO& io, const gpu::GpuBuffer* input, const gpu::GpuBlurImage& dst) WARN_UNUSED_RETURN;
#endif
} // namespace BloomKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_BloomKernels_h
