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

#ifndef Engine_Nodes_Filter_EdgeAware_EdgeAwareGpu_h
#define Engine_Nodes_Filter_EdgeAware_EdgeAwareGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <span>

#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Filter/EdgeAware/BoxFilter.h"
#include "Engine/Nodes/Filter/EdgeAware/GuidedFilter.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The edge-aware library on the GPU: each function records into ctx the dispatches of the
 * CPU function of the same name, over device buffers of tightly packed, top-down, interleaved
 * floats with the CPU function's sizes, parameters and results.
 *
 * Inputs must be visible to compute reads when recording starts; the outputs are visible to
 * later dispatches when a function returns. Intermediates are scratch buffers ctx holds, so ctx
 * needs a buffer pool. A buffer of more than 2^32 floats is refused. The results match the CPU's
 * to rounding: GPU division and transcendental functions are not correctly rounded, and the error
 * sums of the heal and the colour guided filter's tiles run in another order.
 **/
namespace EdgeAware {
/// boxMean(), in place.
gpu::GpuStatus recordBoxMean(gpu::GpuRecordContext& ctx,
                             const gpu::GpuBuffer& buf,
                             int width,
                             int height,
                             int channels,
                             int radius,
                             int iterations,
                             BoxSummationEnum summation);

/// boxMeanVariance().
gpu::GpuStatus recordBoxMeanVariance(gpu::GpuRecordContext& ctx,
                                     const gpu::GpuBuffer& in,
                                     int width,
                                     int height,
                                     int radius,
                                     const gpu::GpuBuffer& mean,
                                     const gpu::GpuBuffer& variance);

/// guidedFilter(), in one tile.
gpu::GpuStatus recordGuidedFilter(gpu::GpuRecordContext& ctx,
                                  const gpu::GpuBuffer& guide,
                                  int guideChannels,
                                  const gpu::GpuBuffer& in,
                                  const gpu::GpuBuffer& out,
                                  int width,
                                  int height,
                                  int radius,
                                  float sqrtEps,
                                  float guideWeight,
                                  float min,
                                  float max);

/// guidedFilterGrey().
gpu::GpuStatus recordGuidedFilterGrey(gpu::GpuRecordContext& ctx,
                                      const gpu::GpuBuffer& guide,
                                      const gpu::GpuBuffer& target,
                                      const gpu::GpuBuffer& out,
                                      int width,
                                      int height,
                                      int radius,
                                      float eps);

/// fastGuidedFilterGrey().
gpu::GpuStatus recordFastGuidedFilterGrey(gpu::GpuRecordContext& ctx,
                                          const gpu::GpuBuffer& guide,
                                          const gpu::GpuBuffer& target,
                                          const gpu::GpuBuffer& out,
                                          int width,
                                          int height,
                                          int radius,
                                          float eps,
                                          int subsampling);

/// fastSurfaceBlur(), in place.
gpu::GpuStatus recordFastSurfaceBlur(gpu::GpuRecordContext& ctx,
                                     const gpu::GpuBuffer& image,
                                     int width,
                                     int height,
                                     int radius,
                                     float feathering,
                                     int iterations,
                                     GuidedBlendingEnum blending,
                                     float quantization,
                                     float quantizeMin,
                                     float quantizeMax);

/// interpolateBilinear(). The sample positions and weights are computed on the host, as the CPU does.
gpu::GpuStatus recordInterpolateBilinear(gpu::GpuRecordContext& ctx,
                                         const gpu::GpuBuffer& in,
                                         int widthIn,
                                         int heightIn,
                                         const gpu::GpuBuffer& out,
                                         int widthOut,
                                         int heightOut,
                                         int channels);

/**
 * @brief quantize() over width x height samples. With sampling 1 the step is exact; with other
 * samplings a sample within a few ulps of a step boundary may land on the other side.
 **/
gpu::GpuStatus recordQuantize(gpu::GpuRecordContext& ctx,
                              const gpu::GpuBuffer& image,
                              const gpu::GpuBuffer& out,
                              int width,
                              int height,
                              float sampling,
                              float clipMin,
                              float clipMax);

/// recursiveGaussianBlur(), min and max holding channels floats each.
gpu::GpuStatus recordRecursiveGaussianBlur(gpu::GpuRecordContext& ctx,
                                           const gpu::GpuBuffer& in,
                                           const gpu::GpuBuffer& out,
                                           int width,
                                           int height,
                                           int channels,
                                           float sigma,
                                           const float* min,
                                           const float* max);

/// eigfSurfaceBlur(), in place.
gpu::GpuStatus recordEigfSurfaceBlur(gpu::GpuRecordContext& ctx,
                                     const gpu::GpuBuffer& image,
                                     int width,
                                     int height,
                                     float sigma,
                                     float feathering,
                                     int iterations,
                                     GuidedBlendingEnum blending,
                                     float quantization,
                                     float quantizeMin,
                                     float quantizeMax);

/**
 * @brief dwtDecomposeScales(): bands is set to the number of detail bands, which go to the first
 * entries of details, finest first; details must hold at least min(scales, dwtMaxScale()) buffers.
 **/
gpu::GpuStatus recordDwtDecomposeScales(gpu::GpuRecordContext& ctx,
                                        const gpu::GpuBuffer& in,
                                        int width,
                                        int height,
                                        int scales,
                                        std::span<const gpu::GpuBuffer* const> details,
                                        const gpu::GpuBuffer& residual,
                                        int& bands);

/// bsplineDecomposeScales(), details holding at least scales buffers.
gpu::GpuStatus recordBsplineDecomposeScales(gpu::GpuRecordContext& ctx,
                                            const gpu::GpuBuffer& in,
                                            int width,
                                            int height,
                                            int scales,
                                            std::span<const gpu::GpuBuffer* const> details,
                                            const gpu::GpuBuffer& residual);

/// waveletRecompose() of the detail bands, finest first, and the residual.
gpu::GpuStatus recordWaveletRecompose(gpu::GpuRecordContext& ctx,
                                      std::span<const gpu::GpuBuffer* const> details,
                                      const gpu::GpuBuffer& residual,
                                      int width,
                                      int height,
                                      const gpu::GpuBuffer& out);

/// blur2DBspline().
gpu::GpuStatus recordBlur2DBspline(gpu::GpuRecordContext& ctx,
                                   const gpu::GpuBuffer& in,
                                   const gpu::GpuBuffer& out,
                                   int width,
                                   int height,
                                   int mult,
                                   bool clipNegatives);

/// decompose2DBspline().
gpu::GpuStatus recordDecompose2DBspline(gpu::GpuRecordContext& ctx,
                                        const gpu::GpuBuffer& in,
                                        const gpu::GpuBuffer& HF,
                                        const gpu::GpuBuffer& LF,
                                        int width,
                                        int height,
                                        int mult);

/**
 * @brief poissonHeal(), in place on dest. Records three dispatches an iteration, all maxIter of
 * them: the ones after convergence return at once. A device without push descriptors refuses more
 * than kPoissonHealGpuMaxIterationsWithoutPushDescriptors iterations, which would exhaust a
 * kernel's descriptor pool.
 **/
gpu::GpuStatus recordPoissonHeal(gpu::GpuRecordContext& ctx,
                                 const gpu::GpuBuffer& src,
                                 const gpu::GpuBuffer& dest,
                                 const gpu::GpuBuffer& mask,
                                 int width,
                                 int height,
                                 int maxIter);

const int kPoissonHealGpuMaxIterationsWithoutPushDescriptors = 500;
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Filter_EdgeAware_EdgeAwareGpu_h
