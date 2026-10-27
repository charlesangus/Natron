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
 * src/common/guided_filter.c and src/common/fast_guided_filter.h, C paths only.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2017-2026 darktable developers.
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

#include "GuidedFilter.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/BoxFilter.h"

NATRON_NAMESPACE_ENTER

namespace EdgeAware {
namespace {
    const int kTileSize = 512;
    const int kRowBlock = 16;
    const std::size_t kSampleBlock = 16384;
    const float kMinFloat = 1.52587890625e-05f; // exp2f(-16.0f)

    inline int
    roundUp(int size,
            int alignment)
    {
        return ((size % alignment) == 0) ? size : ((size - 1) / alignment + 1) * alignment;
    }

    struct Tile {
        int left, right, lower, upper;
    };

    enum {
        kInpMean = 0,
        kGuideMeanR = 1,
        kGuideMeanG = 2,
        kGuideMeanB = 3
    };
    enum {
        kCovR = 0,
        kCovG = 1,
        kCovB = 2,
        kVarRR = 3,
        kVarRG = 4,
        kVarRB = 5,
        kVarGG = 6,
        kVarGB = 7,
        kVarBB = 8
    };

    // _guided_filter_tiling()
    void
    guidedFilterTile(const float* guide,
                     int guideChannels,
                     const float* in,
                     float* out,
                     int imageWidth,
                     int imageHeight,
                     const Tile& target,
                     int w,
                     float eps,
                     float guideWeight,
                     float min,
                     float max,
                     const ParallelFor& parallel)
    {
        const int overlap = roundUp(3 * w, 16);
        const Tile source = { std::max(target.left - overlap, 0), std::min(target.right + overlap, imageWidth),
                              std::max(target.lower - overlap, 0), std::min(target.upper + overlap, imageHeight) };
        const int width = source.right - source.left;
        const int height = source.upper - source.lower;
        const std::size_t size = (std::size_t)width * (std::size_t)height;
        std::vector<float> mean(size * 4);
        std::vector<float> variance(size * 9);

        forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
            std::vector<float> scratch(9 * (std::size_t)roundUp(width, 16));
            for (int j = begin; j < end; ++j) {
                const int jImg = j + source.lower;
                float* meanpx = mean.data() + 4 * (std::size_t)j * width;
                float* varpx = variance.data() + 9 * (std::size_t)j * width;
                for (int iImg = source.left; iImg < source.right; ++iImg) {
                    const std::size_t i = (std::size_t)(iImg - source.left);
                    const std::size_t index = (std::size_t)iImg + (std::size_t)jImg * imageWidth;
                    const float* p = guide + index * guideChannels;
                    const float pixel[3] = { p[0] * guideWeight, p[1] * guideWeight, p[2] * guideWeight };
                    const float input = in[index];
                    meanpx[4 * i + kInpMean] = input;
                    meanpx[4 * i + kGuideMeanR] = pixel[0];
                    meanpx[4 * i + kGuideMeanG] = pixel[1];
                    meanpx[4 * i + kGuideMeanB] = pixel[2];
                    varpx[9 * i + kCovR] = pixel[0] * input;
                    varpx[9 * i + kCovG] = pixel[1] * input;
                    varpx[9 * i + kCovB] = pixel[2] * input;
                    varpx[9 * i + kVarRR] = pixel[0] * pixel[0];
                    varpx[9 * i + kVarRG] = pixel[0] * pixel[1];
                    varpx[9 * i + kVarRB] = pixel[0] * pixel[2];
                    varpx[9 * i + kVarGG] = pixel[1] * pixel[1];
                    varpx[9 * i + kVarGB] = pixel[1] * pixel[2];
                    varpx[9 * i + kVarBB] = pixel[2] * pixel[2];
                }
                boxMeanRow(meanpx, width, 4, w, eBoxSummationKahan, scratch.data());
                boxMeanRow(varpx, width, 9, w, eBoxSummationKahan, scratch.data());
            }
        });
        boxMeanColumns(mean.data(), width, height, 4, w, eBoxSummationKahan, parallel);
        boxMeanColumns(variance.data(), width, height, 9, w, eBoxSummationKahan, parallel);

        // a_r, a_g, a_b and b overwrite the means they are solved from.
        std::vector<float>& ab = mean;
        forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
            for (std::size_t i = (std::size_t)begin * width; i < (std::size_t)end * width; ++i) {
                const float* meanpx = &mean[4 * i];
                const float inpMean = meanpx[kInpMean];
                const float guideR = meanpx[kGuideMeanR];
                const float guideG = meanpx[kGuideMeanG];
                const float guideB = meanpx[kGuideMeanB];
                const float* varpx = &variance[9 * i];
                const float Sigma_0_0 = varpx[kVarRR] - (guideR * guideR) + eps;
                const float Sigma_0_1 = varpx[kVarRG] - (guideR * guideG);
                const float Sigma_0_2 = varpx[kVarRB] - (guideR * guideB);
                const float Sigma_1_1 = varpx[kVarGG] - (guideG * guideG) + eps;
                const float Sigma_1_2 = varpx[kVarGB] - (guideG * guideB);
                const float Sigma_2_2 = varpx[kVarBB] - (guideB * guideB) + eps;
                const float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                    - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                    + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
                float a_r, a_g, a_b, b;
                if (std::fabs(det0) > 4.f * FLT_EPSILON) {
                    const float cov_r = varpx[kCovR] - guideR * inpMean;
                    const float cov_g = varpx[kCovG] - guideG * inpMean;
                    const float cov_b = varpx[kCovB] - guideB * inpMean;
                    const float det1 = cov_r * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                        - Sigma_0_1 * (cov_g * Sigma_2_2 - cov_b * Sigma_1_2)
                        + Sigma_0_2 * (cov_g * Sigma_1_2 - cov_b * Sigma_1_1);
                    const float det2 = Sigma_0_0 * (cov_g * Sigma_2_2 - cov_b * Sigma_1_2)
                        - cov_r * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                        + Sigma_0_2 * (Sigma_0_1 * cov_b - Sigma_0_2 * cov_g);
                    const float det3 = Sigma_0_0 * (Sigma_1_1 * cov_b - Sigma_1_2 * cov_g)
                        - Sigma_0_1 * (Sigma_0_1 * cov_b - Sigma_0_2 * cov_g)
                        + cov_r * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
                    a_r = det1 / det0;
                    a_g = det2 / det0;
                    a_b = det3 / det0;
                    b = inpMean - a_r * guideR - a_g * guideG - a_b * guideB;
                } else {
                    a_r = 0.f;
                    a_g = 0.f;
                    a_b = 0.f;
                    b = inpMean;
                }
                ab[4 * i + 0] = a_r;
                ab[4 * i + 1] = a_g;
                ab[4 * i + 2] = a_b;
                ab[4 * i + 3] = b;
            }
        });
        variance.clear();
        variance.shrink_to_fit();
        boxMean(ab.data(), width, height, 4, w, 1, eBoxSummationKahan, parallel);

        forBlocks(parallel, target.upper - target.lower, kRowBlock, [&](int begin, int end) {
            for (int jImg = target.lower + begin; jImg < target.lower + end; ++jImg) {
                std::size_t l = (std::size_t)target.left + (std::size_t)jImg * imageWidth;
                std::size_t k = (std::size_t)(target.left - source.left) + (std::size_t)(jImg - source.lower) * width;
                for (int iImg = target.left; iImg < target.right; ++iImg, ++k, ++l) {
                    const float* pixel = guide + l * guideChannels;
                    const float* pxAb = &ab[4 * k];
                    float res = guideWeight * (pxAb[0] * pixel[0] + pxAb[1] * pixel[1] + pxAb[2] * pixel[2]);
                    res += pxAb[3];
                    out[iImg + (std::size_t)jImg * imageWidth] = (res > max) ? max : ((res < min) ? min : res);
                }
            }
        });
    } // guidedFilterTile

    // variance_analyse(): a and b fitting mask = a guide + b over each box.
    void
    varianceAnalyse(const float* guide,
                    const float* mask,
                    float* ab,
                    int width,
                    int height,
                    int radius,
                    float feathering,
                    const ParallelFor& parallel)
    {
        const std::size_t size = (std::size_t)width * (std::size_t)height;
        std::vector<float> input(size * 4);

        forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
            for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
                input[4 * k] = guide[k];
                input[4 * k + 1] = mask[k];
                input[4 * k + 2] = guide[k] * guide[k];
                input[4 * k + 3] = guide[k] * mask[k];
            }
        });
        boxMean(input.data(), width, height, 4, radius, 1, eBoxSummationPlain, parallel);
        forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
            for (std::size_t idx = (std::size_t)begin * width; idx < (std::size_t)end * width; ++idx) {
                const float d = std::fmax((input[4 * idx + 2] - input[4 * idx + 0] * input[4 * idx + 0]) + feathering, 1e-15f);
                const float a = (input[4 * idx + 3] - input[4 * idx + 0] * input[4 * idx + 1]) / d;
                const float b = input[4 * idx + 1] - a * input[4 * idx + 0];
                ab[2 * idx] = a;
                ab[2 * idx + 1] = b;
            }
        });
    }

    void
    forSamples(const ParallelFor& parallel,
               std::size_t size,
               const std::function<void(std::size_t begin, std::size_t end)>& body)
    {
        const std::size_t blocks = (size + kSampleBlock - 1) / kSampleBlock;

        parallel((int)blocks, [&](int b) {
            const std::size_t begin = (std::size_t)b * kSampleBlock;
            body(begin, std::min(size, begin + kSampleBlock));
        });
    }

    void
    applyLinearBlending(float* image,
                        const float* ab,
                        std::size_t size,
                        const ParallelFor& parallel)
    {
        forSamples(parallel, size, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) {
                image[k] = std::fmax(image[k] * ab[k * 2] + ab[k * 2 + 1], kMinFloat);
            }
        });
    }

    void
    applyLinearBlendingGeomean(float* image,
                               const float* ab,
                               std::size_t size,
                               const ParallelFor& parallel)
    {
        forSamples(parallel, size, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) {
                image[k] = std::sqrt(image[k] * std::fmax(image[k] * ab[k * 2] + ab[k * 2 + 1], kMinFloat));
            }
        });
    }

    void
    resample(const float* in,
             int widthIn,
             int heightIn,
             float* out,
             int widthOut,
             int heightOut,
             int channels,
             const ParallelFor& parallel)
    {
        if ((widthIn == widthOut) && (heightIn == heightOut)) {
            std::copy(in, in + (std::size_t)widthIn * heightIn * channels, out);
        } else {
            interpolateBilinear(in, widthIn, heightIn, out, widthOut, heightOut, channels, parallel);
        }
    }

    void
    passThroughNonFinite(const float* guide,
                         const float* target,
                         float* out,
                         std::size_t size)
    {
        for (std::size_t k = 0; k < size; ++k) {
            if (!std::isfinite(guide[k]) || !std::isfinite(target[k])) {
                out[k] = target[k];
            }
        }
    }
} // namespace

void
interpolateBilinear(const float* in,
                    int widthIn,
                    int heightIn,
                    float* out,
                    int widthOut,
                    int heightOut,
                    int channels,
                    const ParallelFor& parallel)
{
    if ((widthIn <= 0) || (heightIn <= 0) || (widthOut <= 0) || (heightOut <= 0)) {
        return;
    }
    const std::size_t width_in = (std::size_t)widthIn;
    const std::size_t height_in = (std::size_t)heightIn;
    const std::size_t width_out = (std::size_t)widthOut;
    const std::size_t height_out = (std::size_t)heightOut;
    const std::size_t ch = (std::size_t)channels;

    forBlocks(parallel, heightOut, kRowBlock, [&](int begin, int end) {
        for (std::size_t i = (std::size_t)begin; i < (std::size_t)end; ++i) {
            for (std::size_t j = 0; j < width_out; ++j) {
                const float x_out = (float)j / (float)width_out;
                const float y_out = (float)i / (float)height_out;
                const float x_in = x_out * (float)width_in;
                const float y_in = y_out * (float)height_in;
                std::size_t x_prev = (std::size_t)std::floor(x_in);
                std::size_t x_next = x_prev + 1;
                std::size_t y_prev = (std::size_t)std::floor(y_in);
                std::size_t y_next = y_prev + 1;
                x_prev = (x_prev < width_in) ? x_prev : width_in - 1;
                x_next = (x_next < width_in) ? x_next : width_in - 1;
                y_prev = (y_prev < height_in) ? y_prev : height_in - 1;
                y_next = (y_next < height_in) ? y_next : height_in - 1;
                const std::size_t Y_prev = y_prev * width_in;
                const std::size_t Y_next = y_next * width_in;
                const float* Q_NW = in + (Y_prev + x_prev) * ch;
                const float* Q_NE = in + (Y_prev + x_next) * ch;
                const float* Q_SE = in + (Y_next + x_next) * ch;
                const float* Q_SW = in + (Y_next + x_prev) * ch;
                const float Dy_next = (float)y_next - y_in;
                const float Dy_prev = 1.f - Dy_next;
                const float Dx_next = (float)x_next - x_in;
                const float Dx_prev = 1.f - Dx_next;
                float* pixel_out = out + (i * width_out + j) * ch;
                for (std::size_t c = 0; c < ch; ++c) {
                    pixel_out[c] = Dy_prev * (Q_SW[c] * Dx_next + Q_SE[c] * Dx_prev) + Dy_next * (Q_NW[c] * Dx_next + Q_NE[c] * Dx_prev);
                }
            }
        }
    });
}

void
quantize(const float* image,
         float* out,
         std::size_t size,
         float sampling,
         float clipMin,
         float clipMax,
         const ParallelFor& parallel)
{
    const auto clampValue = [clipMin, clipMax](float value) {
        return std::fmax(std::fmin(value, clipMax), clipMin);
    };

    forSamples(parallel, size, [&](std::size_t begin, std::size_t end) {
        if (sampling == 0.0f) {
            std::copy(image + begin, image + end, out + begin);
        } else if (sampling == 1.0f) {
            for (std::size_t k = begin; k < end; ++k) {
                out[k] = clampValue(std::exp2(std::floor(std::log2(image[k]))));
            }
        } else {
            for (std::size_t k = begin; k < end; ++k) {
                out[k] = clampValue(std::exp2(std::floor(std::log2(image[k]) / sampling) * sampling));
            }
        }
    });
}

void
guidedFilter(const float* guide,
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
             const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0) || (guideChannels < 3)) {
        return;
    }
    const int w = std::max(1, radius);
    const std::size_t size = (std::size_t)width * (std::size_t)height;
    std::vector<float> guideStorage;
    std::vector<float> inStorage;
    const float* finiteGuide = finiteView(guide, size * guideChannels, guideStorage);
    const float* finiteIn = finiteView(in, size, inStorage);
    const int tileDim = std::max(roundUp(3 * w, 16), kTileSize);
    const float eps = sqrtEps * sqrtEps;

    for (int j = 0; j < height; j += tileDim) {
        for (int i = 0; i < width; i += tileDim) {
            const Tile target = { i, std::min(i + tileDim, width), j, std::min(j + tileDim, height) };
            guidedFilterTile(finiteGuide, guideChannels, finiteIn, out, width, height, target, w, eps, guideWeight, min, max, parallel);
        }
    }
    if ((finiteGuide != guide) || (finiteIn != in)) {
        for (std::size_t k = 0; k < size; ++k) {
            const float* g = guide + k * guideChannels;
            if (!std::isfinite(in[k]) || !std::isfinite(g[0]) || !std::isfinite(g[1]) || !std::isfinite(g[2])) {
                out[k] = in[k];
            }
        }
    }
}

void
guidedFilterGrey(const float* guide,
                 const float* target,
                 float* out,
                 int width,
                 int height,
                 int radius,
                 float eps,
                 const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t size = (std::size_t)width * (std::size_t)height;
    std::vector<float> guideStorage;
    std::vector<float> targetStorage;
    const float* I = finiteView(guide, size, guideStorage);
    const float* p = finiteView(target, size, targetStorage);
    std::vector<float> ab(size * 2);

    varianceAnalyse(I, p, ab.data(), width, height, radius, eps, parallel);
    boxMean(ab.data(), width, height, 2, radius, 1, eBoxSummationPlain, parallel);
    forSamples(parallel, size, [&](std::size_t begin, std::size_t end) {
        for (std::size_t k = begin; k < end; ++k) {
            out[k] = ab[2 * k] * I[k] + ab[2 * k + 1];
        }
    });
    if ((I != guide) || (p != target)) {
        passThroughNonFinite(guide, target, out, size);
    }
}

void
fastGuidedFilterGrey(const float* guide,
                     const float* target,
                     float* out,
                     int width,
                     int height,
                     int radius,
                     float eps,
                     int subsampling,
                     const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const float scaling = (float)subsampling;
    const int dsWidth = (int)((float)width / scaling);
    const int dsHeight = (int)((float)height / scaling);
    if ((subsampling < 2) || (dsWidth < 1) || (dsHeight < 1)) {
        guidedFilterGrey(guide, target, out, width, height, radius, eps, parallel);

        return;
    }
    const std::size_t size = (std::size_t)width * (std::size_t)height;
    const std::size_t dsSize = (std::size_t)dsWidth * (std::size_t)dsHeight;
    const int dsRadius = (radius < subsampling) ? 1 : (int)((float)radius / scaling);
    std::vector<float> guideStorage;
    std::vector<float> targetStorage;
    const float* I = finiteView(guide, size, guideStorage);
    const float* p = finiteView(target, size, targetStorage);
    std::vector<float> dsGuide(dsSize);
    std::vector<float> dsTarget(dsSize);
    std::vector<float> dsAb(dsSize * 2);
    std::vector<float> ab(size * 2);

    interpolateBilinear(I, width, height, dsGuide.data(), dsWidth, dsHeight, 1, parallel);
    interpolateBilinear(p, width, height, dsTarget.data(), dsWidth, dsHeight, 1, parallel);
    varianceAnalyse(dsGuide.data(), dsTarget.data(), dsAb.data(), dsWidth, dsHeight, dsRadius, eps, parallel);
    boxMean(dsAb.data(), dsWidth, dsHeight, 2, dsRadius, 1, eBoxSummationPlain, parallel);
    interpolateBilinear(dsAb.data(), dsWidth, dsHeight, ab.data(), width, height, 2, parallel);
    forSamples(parallel, size, [&](std::size_t begin, std::size_t end) {
        for (std::size_t k = begin; k < end; ++k) {
            out[k] = ab[2 * k] * I[k] + ab[2 * k + 1];
        }
    });
    if ((I != guide) || (p != target)) {
        passThroughNonFinite(guide, target, out, size);
    }
}

void
fastSurfaceBlur(float* image,
                int width,
                int height,
                int radius,
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

    float scaling = (float)kFastGuidedSubsampling;
    int dsWidth = (int)((float)width / scaling);
    int dsHeight = (int)((float)height / scaling);
    if ((dsWidth < 1) || (dsHeight < 1)) {
        scaling = 1.f;
        dsWidth = width;
        dsHeight = height;
    }
    const int dsRadius = ((float)radius < scaling) ? 1 : (int)((float)radius / scaling);
    const std::size_t dsSize = (std::size_t)dsWidth * (std::size_t)dsHeight;
    std::vector<float> dsImage(dsSize);
    std::vector<float> dsMask(dsSize);
    std::vector<float> dsAb(dsSize * 2);
    std::vector<float> ab(size * 2);

    resample(image, width, height, dsImage.data(), dsWidth, dsHeight, 1, parallel);
    for (int i = 0; i < iterations; ++i) {
        quantize(dsImage.data(), dsMask.data(), dsSize, quantization, quantizeMin, quantizeMax, parallel);
        varianceAnalyse(dsMask.data(), dsImage.data(), dsAb.data(), dsWidth, dsHeight, dsRadius, feathering, parallel);
        boxMean(dsAb.data(), dsWidth, dsHeight, 2, dsRadius, 1, eBoxSummationPlain, parallel);
        if (i != iterations - 1) {
            applyLinearBlending(dsImage.data(), dsAb.data(), dsSize, parallel);
        }
    }
    resample(dsAb.data(), dsWidth, dsHeight, ab.data(), width, height, 2, parallel);
    if (blending == eGuidedBlendingLinear) {
        applyLinearBlending(image, ab.data(), size, parallel);
    } else {
        applyLinearBlendingGeomean(image, ab.data(), size, parallel);
    }
    nonFinite.restore(image);
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT
