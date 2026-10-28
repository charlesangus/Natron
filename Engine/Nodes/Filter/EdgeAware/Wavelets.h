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

#ifndef Engine_Nodes_Filter_EdgeAware_Wavelets_h
#define Engine_Nodes_Filter_EdgeAware_Wavelets_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <climits>
#include <functional>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief À-trous wavelet decompositions on 4-channel interleaved images, ported from darktable's
 * src/common/dwt.c (the [1 2 1] hat transform) and src/common/bspline.h (the [1 4 6 4 1] B-spline).
 *
 * Scale s (from 0) filters with taps 2^s pixels apart. The detail band of a scale is its input
 * minus its low-pass, so the details plus the last low-pass give the input back.
 *
 * Borders follow darktable and differ between the two: the hat transform mirrors without
 * repeating the edge sample (its tap spacing capped at the image size minus 1), and the B-spline
 * clamps to the edge sample. Both borders are the buffer's edges and the number of scales is
 * capped by its size, so pass the whole frame (EdgeAwareCommon.h).
 **/
namespace EdgeAware {
/// The standard deviation of the gaussian the B-spline approximates.
const float kBSplineSigma = 1.0553651328015339f;
/// Scales a B-spline detail band to approximate a laplacian.
const float kBSplineToLaplacian = 3.182727439285017f;

/**
 * @brief returnLayer 0 asks for the
 * recomposed image, 1 to scales for one detail band, scales + 1 for the residual. Detail bands
 * from mergeFromScale on (when above 0) are summed into one. previewScale is the zoom, which
 * caps the number of scales on small previews.
 **/
struct DwtParams {
    int width = 0;
    int height = 0;
    int scales = 0;
    int returnLayer = 0;
    int mergeFromScale = 0;
    float previewScale = 1.f;
};

/**
 * @brief Called in place on the image at scale 0 before the
 * decomposition, on each detail band (or merged band) at 1 to scales, on the residual at
 * scales + 1 and, when returnLayer is 0, on the recomposed image at scales + 2.
 **/
using DwtLayerFn = std::function<void(float* layer, const DwtParams& params, int scale)>;

/// The most scales the image and zoom allow.
int dwtMaxScale(const DwtParams& params);

/// The first detail scale coarser than a pixel at the zoom.
int dwtFirstScaleVisible(const DwtParams& params);

/**
 * @brief Decomposes image (width x height x 4) in place with the hat transform,
 * calling layerFn on each layer, and leaves in image the layer returnLayer asks for. params.scales
 * and params.returnLayer are first capped as darktable does. Non-finite samples are 0 in every
 * layer and come out unchanged in the returned image.
 **/
void dwtDecompose(float* image,
                  DwtParams& params,
                  const DwtLayerFn& layerFn,
                  const ParallelFor& parallel);

/// The detail bands of a decomposition, finest first, and its residual low-pass.
struct WaveletScales {
    std::vector<std::vector<float>> details;
    std::vector<float> residual;
};

/**
 * @brief The hat-transform detail bands of in (width x height x 4) and its residual, at most
 * scales of them, capped by dwtMaxScale() at zoom 1. Returns the number of detail bands. A
 * non-finite sample is 0 in every band and keeps its value in the residual.
 **/
int dwtDecomposeScales(const float* in,
                       int width,
                       int height,
                       int scales,
                       WaveletScales& out,
                       const ParallelFor& parallel);

/**
 * @brief The B-spline detail bands of in (width x height x 4) and its residual: scales
 * successive decompose2DBspline() steps, step s with taps 2^s apart, each low-pass clipped at 0.
 * A non-finite sample is 0 in every band and keeps its value in the residual.
 **/
void bsplineDecomposeScales(const float* in,
                            int width,
                            int height,
                            int scales,
                            WaveletScales& out,
                            const ParallelFor& parallel);

/**
 * @brief The image the bands came from: the detail bands summed finest first, then the residual,
 * into out (width x height x 4).
 **/
void waveletRecompose(const WaveletScales& scales,
                      int width,
                      int height,
                      float* out,
                      const ParallelFor& parallel);

/**
 * @brief The B-spline low-pass of in into out (width x height x 4), taps mult
 * pixels apart, optionally clipping negatives after each pass. No non-finite handling.
 **/
void blur2DBspline(const float* in,
                   float* out,
                   int width,
                   int height,
                   int mult,
                   bool clipNegatives,
                   const ParallelFor& parallel);

/**
 * @brief The B-spline low-pass of in, negatives clipped, into LF and
 * in - LF into HF (width x height x 4 each). No non-finite handling.
 **/
void decompose2DBspline(const float* in,
                        float* HF,
                        float* LF,
                        int width,
                        int height,
                        int mult,
                        const ParallelFor& parallel);

/// The tap spacing of B-spline step s, 2^s, saturating at the largest int instead of overflowing.
inline int
bsplineStepMult(int s)
{
    return (s < 0) ? 1 : ((s >= 30) ? INT_MAX : (1 << s));
}

/// The standard deviation of steps 0 to s of B-spline blurs of sigma.
float equivalentSigmaAtStep(float sigma,
                            unsigned int s);

/// The inverse of equivalentSigmaAtStep(), plus one; 1 when sigmaFilter is not positive.
unsigned int numStepsToReachEquivalentSigma(float sigmaFilter,
                                            float sigmaFinal);
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_Wavelets_h
