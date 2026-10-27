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
 * src/iop/denoiseprofile.c, with what it calls from src/common/nlmeans_core.c,
 * src/common/eaw.c and src/common/math.h. darktable is distributed under the GNU GPL, version 3
 * or (at your option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2011-2026 darktable developers.
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

#include "DenoiseProfiledKernels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace DenoiseProfiledKernels {
namespace {
    const float kFilter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

    /**
     * @brief eaw_dn_decompose(): one edge-aware a-trous level of in, at a spacing of 2^scale with
     * the edges clamped, into coarse and detail, adding each row's squared details to rowSumSq.
     **/
    void
    decompose(const float* in,
              float* coarse,
              float* detail,
              std::vector<double>* rowSumSq,
              int scale,
              float invSigma2,
              int width,
              int height,
              const ParallelFor& parallel)
    {
        const int mult = 1 << scale;
        float filter[25];

        for (int jj = 0; jj < 5; ++jj) {
            for (int ii = 0; ii < 5; ++ii) {
                filter[jj * 5 + ii] = kFilter[jj] * kFilter[ii];
            }
        }
        parallel(height, [&](int j) {
            double sumSq[3] = { 0., 0., 0. };
            int rows[5];
            for (int jj = 0; jj < 5; ++jj) {
                rows[jj] = std::clamp(j + mult * (jj - 2), 0, height - 1);
            }
            for (int i = 0; i < width; ++i) {
                const std::size_t k = ((std::size_t)j * width + i) * 3;
                const float* px = in + k;
                float sum[3] = { 0.f, 0.f, 0.f };
                float wgt = 0.f;
                for (int jj = 0; jj < 5; ++jj) {
                    const float* line = in + (std::size_t)rows[jj] * width * 3;
                    for (int ii = 0; ii < 5; ++ii) {
                        const int x = std::clamp(i + mult * (ii - 2), 0, width - 1);
                        const float* px2 = line + (std::size_t)x * 3;
                        const float d0 = px[0] - px2[0];
                        const float d1 = px[1] - px2[1];
                        const float d2 = px[2] - px2[2];
                        const float dot = (d0 * d0 + d1 * d1 + d2 * d2) * invSigma2;
                        const float w = filter[jj * 5 + ii] * fastMexp2fFloat(std::max(0.0f, dot * 0.02f - 9.0f));
                        wgt += w;
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += w * px2[c];
                        }
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    const float s = sum[c] / wgt;
                    const float det = px[c] - s;
                    coarse[k + c] = s;
                    detail[k + c] = det;
                    sumSq[c] += det * det;
                }
            }
            for (int c = 0; c < 3; ++c) {
                (*rowSumSq)[(std::size_t)j * 3 + c] = sumSq[c];
            }
        });
    }

    /**
     * @brief variance_stabilizing_xform(): BayesShrink thresholds for one level from the squared
     * details and the curves.
     **/
    Vec3
    thresholds(int scale,
               int maxScale,
               std::size_t npixels,
               const double sumY2[3],
               const Settings& settings)
    {
        const float varf = std::sqrt(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f;
        const float sigmaBand = std::pow(varf, (float)scale);
        const float sb2 = sigmaBand * sigmaBand;
        const Vec3 adjt = waveletAdjust(scale, maxScale, settings);
        Vec3 thrs;
        for (int c = 0; c < 3; ++c) {
            const float varY = (float)sumY2[c] / (npixels - 1.0f);
            const float stdX = std::sqrt(std::max(1e-6f, varY - sb2));
            thrs[c] = adjt[c] * sb2 / stdX;
        }

        return thrs;
    }
} // namespace

Vec3
waveletAdjust(int scale,
              int maxScale,
              const Settings& settings)
{
    Vec3 adjt = { 8.0f, 8.0f, 8.0f };
    const int bandIndex = kBands - (scale + (kBands - maxScale) + 1);
    const auto force2 = [&](int curve) {
        const float f = settings.force[curve][bandIndex];

        return f * f * 4;
    };

    if (settings.colorMode == eColorModeRgb) {
        const float all = force2(eCurveAll);
        for (int c = 0; c < 3; ++c) {
            adjt[c] *= all;
        }
        adjt[0] *= force2(eCurveRed);
        adjt[1] *= force2(eCurveGreen);
        adjt[2] *= force2(eCurveBlue);
    } else {
        adjt[0] *= force2(eCurveY0);
        const float u0v0 = force2(eCurveU0V0);
        adjt[1] *= u0v0;
        adjt[2] *= u0v0;
    }

    return adjt;
}

int
waveletScales(double renderScale,
              int fullSize)
{
    const float inScale = std::min((float)renderScale, 1.0f);
    const float supp0 = std::min((float)(2 * (2u << (kBands - 1)) + 1), fullSize * 0.2f);
    const float i0 = std::log2((supp0 - 1.0f) * .5f);
    int maxScale = 0;

    for (; maxScale < kBands; ++maxScale) {
        const float supp = 2 * (2u << maxScale) + 1;
        const float suppIn = supp * (1.0f / inScale);
        const float iIn = std::log2((suppIn - 1) * .5f) - 1.0f;
        const float t = 1.0f - (iIn + .5f) / i0;
        if (t < 0.0f) {
            break;
        }
    }

    return maxScale;
}

void
denoiseWavelets(const float* in,
                int width,
                int height,
                int fullSize,
                const Settings& settings,
                double renderScale,
                float* out,
                const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t npixels = (std::size_t)width * height;
    const int maxScale = waveletScales(renderScale, fullSize);
    const int maxMult = (maxScale > 0) ? (1 << (maxScale - 1)) : 0;
    if ((maxScale == 0) || (width < 2 * maxMult) || (height < 2 * maxMult)) {
        std::memcpy(out, in, sizeof(float) * 3 * npixels);

        return;
    }
    const float inScale = std::min((float)renderScale, 1.0f);
    Vec3 wb = settings.wb;
    Vec3 p;
    for (int c = 0; c < 3; ++c) {
        p[c] = (float)std::max(settings.shadows + 0.1 * std::log(inScale / wb[c]), 0.0);
    }
    const float compensateP = 0.05f / std::pow(0.05f, settings.shadows);
    Matrix3 toY0U0V0;
    Matrix3 toRgb;
    conversionMatrices(wb, &toY0U0V0, &toRgb);
    const float compensateStrength = (settings.colorMode == eColorModeRgb) ? 1.0f : 2.5f;
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) {
            toY0U0V0[k][c] /= (settings.strength * compensateStrength * inScale);
            toRgb[k][c] *= (settings.strength * compensateStrength * inScale);
        }
    }
    for (int c = 0; c < 3; ++c) {
        wb[c] *= settings.strength * compensateStrength * inScale;
    }
    const float a = settings.profileA * compensateP;
    const float b = settings.profileB;

    std::vector<float> precond(npixels * 3);
    std::vector<float> tmp(npixels * 3);
    std::vector<float> detail(npixels * 3);
    parallel(height, [&](int y) {
        const std::size_t first = (std::size_t)y * width * 3;
        if (settings.colorMode == eColorModeRgb) {
            preconditionV2(in + first, precond.data() + first, (std::size_t)width, a, p, b, wb);
        } else {
            preconditionY0U0V0(in + first, precond.data() + first, (std::size_t)width, a, p, b, toY0U0V0);
        }
    });

    float* buf1 = precond.data();
    float* buf2 = tmp.data();
    std::fill(out, out + npixels * 3, 0.f);
    std::vector<double> rowSumSq((std::size_t)height * 3);
    for (int scale = 0; scale < maxScale; ++scale) {
        const float sigmaBand = std::pow(std::sqrt(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f, (float)scale);
        decompose(buf1, buf2, detail.data(), &rowSumSq, scale, 1.0f / (sigmaBand * sigmaBand), width, height, parallel);
        double sumY2[3] = { 0., 0., 0. };
        for (int y = 0; y < height; ++y) {
            for (int c = 0; c < 3; ++c) {
                sumY2[c] += rowSumSq[(std::size_t)y * 3 + c];
            }
        }
        const Vec3 thrs = thresholds(scale, maxScale, npixels, sumY2, settings);
        parallel(height, [&](int y) {
            const std::size_t first = (std::size_t)y * width * 3;
            for (std::size_t k = first; k < first + (std::size_t)width * 3; k += 3) {
                for (int c = 0; c < 3; ++c) {
                    const float d = detail[k + c];
                    out[k + c] += std::max(d - thrs[c], 0.0f) + std::min(d + thrs[c], 0.0f);
                }
            }
        });
        std::swap(buf1, buf2);
    }

    const float bias = (float)(settings.bias - 0.5 * std::log(inScale));
    parallel(height, [&](int y) {
        const std::size_t first = (std::size_t)y * width * 3;
        for (std::size_t k = first; k < first + (std::size_t)width * 3; ++k) {
            out[k] += buf1[k];
        }
        if (settings.colorMode == eColorModeRgb) {
            backtransformV2(out + first, (std::size_t)width, a, p, b, bias, wb);
        } else {
            backtransformY0U0V0(out + first, (std::size_t)width, a, p, b, bias, wb, toRgb);
        }
    });
} // denoiseWavelets
} // namespace DenoiseProfiledKernels

NATRON_NAMESPACE_EXIT
