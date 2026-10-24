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
 * src/iop/demosaicing/dual.c, with the parts of src/develop/masks/detail.c
 * (dt_masks_calc_scharr_mask(), dt_masks_calc_detail_blend()), src/common/gaussian.c
 * (dt_gaussian_fast_blur() for one channel) and src/common/math.h (scharr_gradient(),
 * dt_fast_expf()) it calls, C paths only. darktable is distributed under the GNU GPL, version 3
 * or (at your option) any later version; this file is therefore GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2010-2026 darktable developers.
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
 *
 * Dual demosaicing has been implemented by Ingo Weyrich <heckflosse67@gmx.de> for
 * rawtherapee under GNU General Public License Version 3
 * and has been modified to work for darktable by Hanno Schwalm (hanno@schwalm-bremen.de).
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawKernels.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    inline float
    clip01(float x)
    {
        return std::fmin(1.0f, std::fmax(0.0f, x));
    }

    // darktable's dt_fast_expf(). darktable converts the float to int unchecked; x86 turns an
    // out-of-range one into INT_MIN, which the k0 > 0 test then maps to 0, as the guard here does.
    inline float
    fastExpf(float x)
    {
        const int i1 = 0x3f800000;
        const int i2 = 0x402DF854;
        const float f = (float)i1 + x * (float)(i2 - i1);
        const int k0 = (f > 0.0f) ? (int)f : 0;
        std::uint32_t k = (std::uint32_t)k0;
        float r;
        std::memcpy(&r, &k, sizeof(r));

        return r;
    }

    // darktable's dt_fast_hypotf() as its -ffast-math Release builds define it.
    inline float
    scharrGradient(const float* p,
                   int w)
    {
        const float gx = 47.0f / 255.0f * (p[-w - 1] - p[-w + 1] + p[w - 1] - p[w + 1])
            + 162.0f / 255.0f * (p[-1] - p[1]);
        const float gy = 47.0f / 255.0f * (p[-w - 1] - p[w - 1] + p[-w + 1] - p[w + 1])
            + 162.0f / 255.0f * (p[-w] - p[w]);

        return std::sqrt(gx * gx + gy * gy);
    }

    void
    scharrMask(float* mask,
               const float* src,
               int width,
               int height,
               const float wb[3],
               const ParallelFor& parallel)
    {
        std::vector<float> tmp((std::size_t)width * height);
        parallel(height, [&](int row) {
            for (int col = 0; col < width; ++col) {
                const std::size_t idx = (std::size_t)row * width + col;
                const float val = std::fmax(0.0f, src[4 * idx] * wb[0])
                    + std::fmax(0.0f, src[4 * idx + 1] * wb[1])
                    + std::fmax(0.0f, src[4 * idx + 2] * wb[2]);
                tmp[idx] = std::sqrt(val / 3.0f);
            }
        });
        parallel(height, [&](int row) {
            const int irow = std::min(std::max(row, 1), height - 2);
            for (int col = 0; col < width; ++col) {
                const int icol = std::min(std::max(col, 1), width - 2);
                const std::size_t idx = (std::size_t)irow * width + icol;
                mask[(std::size_t)row * width + col] = clip01(scharrGradient(&tmp[idx], width) / 16.0f);
            }
        });
    }

    void
    detailBlend(const float* src,
                float* out,
                std::size_t msize,
                float threshold,
                const ParallelFor& parallel)
    {
        // darktable's MAX(1e-7, threshold) is a double, so is the quotient before it is stored.
        const float ithreshold = (float)(16.0f / std::max(1e-7, (double)threshold));
        const int rows = (int)((msize + 4095) / 4096);
        parallel(rows, [&](int r) {
            const std::size_t end = std::min(msize, (std::size_t)(r + 1) * 4096);
            for (std::size_t idx = (std::size_t)r * 4096; idx < end; ++idx) {
                out[idx] = clip01(1.0f / (1.0f + fastExpf(16.0f - ithreshold * src[idx])));
            }
        });
    }

    void
    gauss9x9Coeffs(float* coeffs,
                   float sigma)
    {
        float kernel[9][9];
        const float range = 4.5f * 4.5f;
        const float temp = -2.0f * sigma * sigma;
        float sum = 0.0f;

        for (int k = -4; k < 5; k++) {
            for (int j = -4; j < 5; j++) {
                const float rad = (float)(k * k + j * j);
                if (rad <= range) {
                    kernel[k + 4][j + 4] = std::exp(rad / temp);
                    sum += kernel[k + 4][j + 4];
                } else {
                    kernel[k + 4][j + 4] = 0.0f;
                }
            }
        }
        for (int k = 0; k < 5; k++) {
            for (int j = 0; j < 5; j++) {
                coeffs[5 * k + j] = kernel[k + 4][j + 4] / sum;
            }
        }
    }

    // darktable's _fast_9x9_kernel_1(): the inner pixels sum the non-zero taps in its grouped
    // order, the outer 4 rows and columns every tap inside the image in raster order.
    void
    fastGaussianBlur(const float* in,
                     float* out,
                     int width,
                     int height,
                     float sigma,
                     float min,
                     float max,
                     const ParallelFor& parallel)
    {
        float kern[25];
        gauss9x9Coeffs(kern, sigma);
        const int w1 = width;
        const int w2 = 2 * width;
        const int w3 = 3 * width;
        const int w4 = 4 * width;
        const int h1 = 1, h2 = 2, h3 = 3, h4 = 4;

        parallel(height, [&](int row) {
            for (int col = 0; col < width; col++) {
                const std::size_t i = (std::size_t)row * width + col;
                float val = 0.0f;
                if ((col >= 4) && (row >= 4) && (col < width - 4) && (row < height - 4)) {
                    val = kern[10 + 4] * (in[i - w4 - h2] + in[i - w4 + h2] + in[i - w2 - h4] + in[i - w2 + h4] + in[i + w2 - h4] + in[i + w2 + h4] + in[i + w4 - h2] + in[i + w4 + h2])
                        + kern[5 + 4] * (in[i - w4 - h1] + in[i - w4 + h1] + in[i - w1 - h4] + in[i - w1 + h4] + in[i + w1 - h4] + in[i + w1 + h4] + in[i + w4 - h1] + in[i + w4 + h1])
                        + kern[4] * (in[i - w4] + in[i - h4] + in[i + h4] + in[i + w4])
                        + kern[15 + 3] * (in[i - w3 - h3] + in[i - w3 + h3] + in[i + w3 - h3] + in[i + w3 + h3])
                        + kern[10 + 3] * (in[i - w3 - h2] + in[i - w3 + h2] + in[i - w2 - h3] + in[i - w2 + h3] + in[i + w2 - h3] + in[i + w2 + h3] + in[i + w3 - h2] + in[i + w3 + h2])
                        + kern[5 + 3] * (in[i - w3 - h1] + in[i - w3 + h1] + in[i - w1 - h3] + in[i - w1 + h3] + in[i + w1 - h3] + in[i + w1 + h3] + in[i + w3 - h1] + in[i + w3 + h1])
                        + kern[3] * (in[i - w3] + in[i - h3] + in[i + h3] + in[i + w3])
                        + kern[10 + 2] * (in[i - w2 - h2] + in[i - w2 + h2] + in[i + w2 - h2] + in[i + w2 + h2])
                        + kern[5 + 2] * (in[i - w2 - h1] + in[i - w2 + h1] + in[i - w1 - h2] + in[i - w1 + h2] + in[i + w1 - h2] + in[i + w1 + h2] + in[i + w2 - h1] + in[i + w2 + h1])
                        + kern[2] * (in[i - w2] + in[i - h2] + in[i + h2] + in[i + w2])
                        + kern[5 + 1] * (in[i - w1 - h1] + in[i - w1 + h1] + in[i + w1 - h1] + in[i + w1 + h1])
                        + kern[1] * (in[i - w1] + in[i - h1] + in[i + h1] + in[i + w1])
                        + kern[0] * (in[i]);
                } else {
                    for (int ir = -4; ir <= 4; ir++) {
                        const int irow = row + ir;
                        if ((irow >= 0) && (irow < height)) {
                            for (int ic = -4; ic <= 4; ic++) {
                                const int icol = col + ic;
                                if ((icol >= 0) && (icol < width)) {
                                    val += kern[5 * std::abs(ir) + std::abs(ic)] * in[(std::size_t)irow * width + icol];
                                }
                            }
                        }
                    }
                }
                out[i] = std::fmin(max, std::fmax(min, val));
            }
        });
    }
} // namespace

float
dualContrast(float slider)
{
    return 0.005f * std::pow(slider, 1.1f);
}

void
dualDetailMask(float* mask,
               const float* high,
               int width,
               int height,
               float threshold,
               const float wb[3],
               const ParallelFor& parallel)
{
    const std::size_t msize = (std::size_t)width * height;
    std::vector<float> tmp(msize);

    scharrMask(mask, high, width, height, wb, parallel);
    detailBlend(mask, tmp.data(), msize, dualContrast(threshold), parallel);
    fastGaussianBlur(tmp.data(), mask, width, height, 2.0f, 0.0f, 1.0f, parallel);
}

void
dualDemosaic(float* high,
             const float* raw,
             int width,
             int height,
             std::uint32_t filters,
             const std::uint8_t (*xtrans)[6],
             float threshold,
             const float wb[3],
             const ParallelFor& parallel)
{
    if ((width < 16) || (height < 16) || (threshold <= 0.0f)) {
        return;
    }
    const std::size_t msize = (std::size_t)width * height;
    std::vector<float> mask(msize);
    dualDetailMask(mask.data(), high, width, height, threshold, wb, parallel);

    std::vector<float> low(msize * 4);
    vng(low.data(), raw, width, height, filters, xtrans, true, parallel);
    colorSmoothing(low.data(), width, height, 2, parallel);

    parallel(height, [&](int row) {
        for (int col = 0; col < width; ++col) {
            const std::size_t idx = (std::size_t)row * width + col;
            const std::size_t oidx = idx * 4;
            for (int c = 0; c < 3; c++) {
                high[oidx + c] = mask[idx] * (high[oidx + c] - low[oidx + c]) + low[oidx + c];
            }
            high[oidx + 3] = 0.0f;
        }
    });
}
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
