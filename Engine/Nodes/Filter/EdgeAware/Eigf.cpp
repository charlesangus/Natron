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
 * src/common/eigf.h and dt_gaussian_blur() of src/common/gaussian.c, C paths only.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2012-2026 darktable developers.
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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Eigf.h"

#include <algorithm>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace EdgeAware {
namespace {
    const int kLineBlock = 16;
    const int kMaxGaussianChannels = 4;
    const float kMinFloat = 1.52587890625e-05f; // exp2f(-16.0f)

    inline float
    clampf(float a,
           float mn,
           float mx)
    {
        return (a >= mn) ? ((a <= mx) ? a : mx) : mn;
    }

    // One line of the separable recursion: count samples step floats apart, the causal pass
    // stored in out and the anticausal one added to it.
    void
    gaussianLine(const float* in,
                 float* out,
                 int count,
                 std::size_t step,
                 int ch,
                 const DericheCoefficients& g,
                 const float* min,
                 const float* max)
    {
        float xp[kMaxGaussianChannels], yb[kMaxGaussianChannels], yp[kMaxGaussianChannels];
        float xn[kMaxGaussianChannels], xa[kMaxGaussianChannels], yn[kMaxGaussianChannels], ya[kMaxGaussianChannels];

        for (int k = 0; k < ch; ++k) {
            xp[k] = clampf(in[k], min[k], max[k]);
            yb[k] = xp[k] * g.coefp;
            yp[k] = yb[k];
        }
        for (int j = 0; j < count; ++j) {
            const std::size_t offset = (std::size_t)j * step;
            for (int k = 0; k < ch; ++k) {
                const float xc = clampf(in[offset + k], min[k], max[k]);
                const float yc = (g.a0 * xc) + (g.a1 * xp[k]) - (g.b1 * yp[k]) - (g.b2 * yb[k]);
                out[offset + k] = yc;
                xp[k] = xc;
                yb[k] = yp[k];
                yp[k] = yc;
            }
        }
        const std::size_t last = (std::size_t)(count - 1) * step;
        for (int k = 0; k < ch; ++k) {
            xn[k] = clampf(in[last + k], min[k], max[k]);
            xa[k] = xn[k];
            yn[k] = xn[k] * g.coefn;
            ya[k] = yn[k];
        }
        for (int j = count - 1; j > -1; --j) {
            const std::size_t offset = (std::size_t)j * step;
            for (int k = 0; k < ch; ++k) {
                const float xc = clampf(in[offset + k], min[k], max[k]);
                const float yc = (g.a2 * xn[k]) + (g.a3 * xa[k]) - (g.b1 * yn[k]) - (g.b2 * ya[k]);
                xa[k] = xn[k];
                xn[k] = xc;
                ya[k] = yn[k];
                yn[k] = yc;
                out[offset + k] += yc;
            }
        }
    }

    // The per-channel data range of size pixels of ch channels, darktable's reductions starting
    // from min 1e7 and max 0. Min and max are exact, so the block split cannot change them.
    void
    channelRange(const float* in,
                 std::size_t size,
                 int ch,
                 float* min,
                 float* max,
                 const ParallelFor& parallel)
    {
        const std::size_t blockSize = 65536;
        const int blocks = (int)((size + blockSize - 1) / blockSize);
        std::vector<float> partials((std::size_t)blocks * 2 * ch);

        parallel(blocks, [&](int b) {
            float* pmin = &partials[(std::size_t)b * 2 * ch];
            float* pmax = pmin + ch;
            for (int k = 0; k < ch; ++k) {
                pmin[k] = 10000000.0f;
                pmax[k] = 0.0f;
            }
            const std::size_t end = std::min(size, (std::size_t)(b + 1) * blockSize);
            for (std::size_t i = (std::size_t)b * blockSize; i < end; ++i) {
                for (int k = 0; k < ch; ++k) {
                    const float v = in[i * ch + k];
                    pmin[k] = std::min(pmin[k], v);
                    pmax[k] = std::max(pmax[k], v);
                }
            }
        });
        for (int k = 0; k < ch; ++k) {
            min[k] = 10000000.0f;
            max[k] = 0.0f;
        }
        for (int b = 0; b < blocks; ++b) {
            for (int k = 0; k < ch; ++k) {
                min[k] = std::min(min[k], partials[(std::size_t)b * 2 * ch + k]);
                max[k] = std::max(max[k], partials[(std::size_t)b * 2 * ch + ch + k]);
            }
        }
    }

    // eigf_variance_analysis(): out holds the averages of guide, its variance, the average of
    // mask and the covariance.
    void
    varianceAnalysis(const float* guide,
                     const float* mask,
                     float* out,
                     int width,
                     int height,
                     float sigma,
                     const ParallelFor& parallel)
    {
        const std::size_t size = (std::size_t)width * (std::size_t)height;
        std::vector<float> in(size * 4);

        forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                const float pixelg = guide[k];
                const float pixelm = mask[k];
                in[k * 4] = pixelg;
                in[k * 4 + 1] = pixelg * pixelg;
                in[k * 4 + 2] = pixelm;
                in[k * 4 + 3] = pixelm * pixelg;
            }
        });
        float min[4], max[4];
        channelRange(in.data(), size, 4, min, max, parallel);
        recursiveGaussianBlur(in.data(), out, width, height, 4, sigma, min, max, parallel);
        forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                out[4 * k + 1] -= out[4 * k] * out[4 * k];
                out[4 * k + 3] -= out[4 * k] * out[4 * k + 2];
            }
        });
    }

    // eigf_variance_analysis_no_mask()
    void
    varianceAnalysisNoMask(const float* guide,
                           float* out,
                           int width,
                           int height,
                           float sigma,
                           const ParallelFor& parallel)
    {
        const std::size_t size = (std::size_t)width * (std::size_t)height;
        std::vector<float> in(size * 2);

        forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                in[2 * k] = guide[k];
                in[2 * k + 1] = guide[k] * guide[k];
            }
        });
        float min[2], max[2];
        channelRange(in.data(), size, 2, min, max, parallel);
        recursiveGaussianBlur(in.data(), out, width, height, 2, sigma, min, max, parallel);
        forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                const float avg = out[2 * k];
                out[2 * k + 1] -= avg * avg;
            }
        });
    }

    inline float
    blend(float pixel,
          float a,
          float b,
          GuidedBlendingEnum blending)
    {
        if (blending == eGuidedBlendingLinear) {
            return std::fmax(pixel * a + b, kMinFloat);
        }

        return std::sqrt(pixel * std::fmax(pixel * a + b, kMinFloat));
    }

    // eigf_blending()
    void
    eigfBlending(float* image,
                 const float* mask,
                 const float* av,
                 int width,
                 int height,
                 GuidedBlendingEnum blending,
                 float feathering,
                 const ParallelFor& parallel)
    {
        forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                const float avg_g = av[k * 4];
                const float avg_m = av[k * 4 + 2];
                const float var_g = av[k * 4 + 1];
                const float covar_mg = av[k * 4 + 3];
                const float norm_g = std::fmax(avg_g * image[k], 1E-6f);
                const float norm_m = std::fmax(avg_m * mask[k], 1E-6f);
                const float normalized_var_guide = var_g / norm_g;
                const float normalized_covar = covar_mg / std::sqrt(norm_g * norm_m);
                const float a = normalized_covar / (normalized_var_guide + feathering);
                const float b = avg_m - a * avg_g;
                image[k] = blend(image[k], a, b, blending);
            }
        });
    }

    // eigf_blending_no_mask()
    void
    eigfBlendingNoMask(float* image,
                       const float* av,
                       int width,
                       int height,
                       GuidedBlendingEnum blending,
                       float feathering,
                       const ParallelFor& parallel)
    {
        forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                const float avg_g = av[k * 2];
                const float var_g = av[k * 2 + 1];
                const float norm_g = std::fmax(avg_g * image[k], 1E-6f);
                const float normalized_var_guide = var_g / norm_g;
                const float a = normalized_var_guide / (normalized_var_guide + feathering);
                const float b = avg_g - a * avg_g;
                image[k] = blend(image[k], a, b, blending);
            }
        });
    }
} // namespace

DericheCoefficients
dericheCoefficients(float sigma)
{
    DericheCoefficients g;
    const float alpha = 1.695f / sigma;
    const float ema = std::exp(-alpha);
    const float ema2 = std::exp(-2.0f * alpha);

    g.b1 = -2.0f * ema;
    g.b2 = ema2;
    const float k = (1.0f - ema) * (1.0f - ema) / (1.0f + (2.0f * alpha * ema) - ema2);
    g.a0 = k;
    g.a1 = k * (alpha - 1.0f) * ema;
    g.a2 = k * (alpha + 1.0f) * ema;
    g.a3 = -k * ema2;
    g.coefp = (g.a0 + g.a1) / (1.0f + g.b1 + g.b2);
    g.coefn = (g.a2 + g.a3) / (1.0f + g.b1 + g.b2);

    return g;
}

void
recursiveGaussianBlur(const float* in,
                      float* out,
                      int width,
                      int height,
                      int channels,
                      float sigma,
                      const float* min,
                      const float* max,
                      const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0) || (channels <= 0) || (channels > kMaxGaussianChannels)) {
        return;
    }
    const DericheCoefficients g = dericheCoefficients(sigma);
    const std::size_t ch = (std::size_t)channels;
    const std::size_t rowStride = (std::size_t)width * ch;
    std::vector<float> temp(rowStride * (std::size_t)height);

    forBlocks(parallel, width, kLineBlock, [&](int begin, int end) {
        for (int i = begin; i < end; ++i) {
            gaussianLine(in + (std::size_t)i * ch, temp.data() + (std::size_t)i * ch, height, rowStride, channels, g, min, max);
        }
    });
    forBlocks(parallel, height, kLineBlock, [&](int begin, int end) {
        for (int j = begin; j < end; ++j) {
            gaussianLine(temp.data() + (std::size_t)j * rowStride, out + (std::size_t)j * rowStride, width, ch, channels, g, min, max);
        }
    });
}

void
eigfSurfaceBlur(float* image,
                int width,
                int height,
                float sigma,
                float feathering,
                int iterations,
                GuidedBlendingEnum blending,
                float quantization,
                float quantizeMin,
                float quantizeMax,
                const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t size = (std::size_t)width * (std::size_t)height;
    NonFiniteSamples nonFinite;
    nonFinite.take(image, size);

    float scaling = std::fmax(std::fmin(sigma, 4.0f), 1.0f);
    int dsWidth = (int)((float)width / scaling);
    int dsHeight = (int)((float)height / scaling);
    if ((dsWidth < 1) || (dsHeight < 1)) {
        scaling = 1.0f;
        dsWidth = width;
        dsHeight = height;
    }
    const float dsSigma = std::fmax(sigma / scaling, 1.0f);
    const std::size_t dsSize = (std::size_t)dsWidth * (std::size_t)dsHeight;
    std::vector<float> mask;
    std::vector<float> dsImage(dsSize);
    std::vector<float> dsMask;
    std::vector<float> dsAv(dsSize * 4);
    std::vector<float> av(size * 4);
    if (quantization != 0.0f) {
        mask.resize(size);
        dsMask.resize(dsSize);
    }

    for (int i = 0; i < iterations; ++i) {
        const GuidedBlendingEnum blend = (i == iterations - 1) ? blending : eGuidedBlendingLinear;
        interpolateBilinear(image, width, height, dsImage.data(), dsWidth, dsHeight, 1, parallel);
        if (quantization != 0.0f) {
            quantize(image, mask.data(), size, quantization, quantizeMin, quantizeMax, parallel);
            interpolateBilinear(mask.data(), width, height, dsMask.data(), dsWidth, dsHeight, 1, parallel);
            varianceAnalysis(dsMask.data(), dsImage.data(), dsAv.data(), dsWidth, dsHeight, dsSigma, parallel);
            interpolateBilinear(dsAv.data(), dsWidth, dsHeight, av.data(), width, height, 4, parallel);
            eigfBlending(image, mask.data(), av.data(), width, height, blend, feathering, parallel);
        } else {
            varianceAnalysisNoMask(dsImage.data(), dsAv.data(), dsWidth, dsHeight, dsSigma, parallel);
            interpolateBilinear(dsAv.data(), dsWidth, dsHeight, av.data(), width, height, 2, parallel);
            eigfBlendingNoMask(image, av.data(), width, height, blend, feathering, parallel);
        }
    }
    nonFinite.restore(image);
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT
