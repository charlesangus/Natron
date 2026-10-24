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
 * src/iop/demosaicing/capture.c, with the parts of src/iop/demosaic.c (the iteration count of
 * commit_params(), the coefficient table of init_global()) and src/common/gaussian.c
 * (dt_gaussian_fast_blur() for one channel) it uses, C paths only. darktable is distributed
 * under the GNU GPL, version 3 or (at your option) any later version; this file is therefore
 * GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2025-2026 darktable developers.
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
 * Credits go to Ingo Weyrich (heckflosse67@gmx.de), who implemented the original algorithm for
 * rawtherapee.
 *
 * Where darktable takes a border from the buffer it runs over (a tile, or the whole image when
 * it does not tile), this port takes it from the frame, so that any buffer reaching
 * captureHalo() past the pixels it outputs gives them exactly as the whole frame does.
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
#include <cstdlib>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    const int kCaptureKernels = 256;
    const int kCaptureKernelSize = 25;
    const float kCaptureGaussFraction = 0.01f;
    const float kCaptureYMin = 0.001f;
    const float kCaptureCfaClip = 0.9f;
    const float kCaptureSmall = 0.66f;
    const float kNormMin = 1.52587890625e-05f;

    // darktable's src/iop/demosaicing/capture.c (_calcRadius*())
    const float kRawEps = 0.005f;
    const float kLowerLimit = 0.01f;
    const float kUpperLimit = 0.9f;

    inline float
    sqrf(float a)
    {
        return a * a;
    }

    // darktable's CLIP(): NaN goes to 0.
    inline float
    clip01(float x)
    {
        return (x >= 0) ? ((x <= 1) ? x : 1) : 0;
    }

    inline float
    clampF(float a,
           float mn,
           float mx)
    {
        return (a >= mn) ? ((a <= mx) ? a : mx) : mn;
    }

    inline float
    maxF(float a,
         float b)
    {
        return (a > b) ? a : b;
    }

    inline float
    minF(float a,
         float b)
    {
        return (a < b) ? a : b;
    }

    inline int
    clampI(int a,
           int mn,
           int mx)
    {
        return (a > mn) ? ((a < mx) ? a : mx) : mn;
    }

    // The 5x5 quadrant of a 9x9 Gaussian, normalised over the whole kernel: capture.c's
    // _calc_9x9_gauss_coeffs() with range 2.5^2 or 4.5^2, and gaussian.c's with 4.5^2 always.
    void
    gaussCoeffs(float* coeffs,
                float sigma,
                float range)
    {
        float kernel[9][9];
        const float temp = -2.0f * sigma * sigma;
        float sum = 0.0f;

        for (int k = -4; k < 5; k++) {
            for (int j = -4; j < 5; j++) {
                const float rad = (float)(k * k + j * j);
                if (rad <= range) {
                    kernel[k + 4][j + 4] = expf(rad / temp);
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

    // darktable's gd->gauss_coeffs: kernel i has sigma max(1e-7, i * 0.01).
    const float*
    captureKernels()
    {
        static const std::vector<float> table = [] {
            std::vector<float> t((std::size_t)kCaptureKernels * kCaptureKernelSize);
            for (int i = 0; i < kCaptureKernels; i++) {
                const float sigma = std::max(1e-7f, (float)i * kCaptureGaussFraction);
                gaussCoeffs(&t[(std::size_t)i * kCaptureKernelSize], sigma, (sigma < kCaptureSmall) ? sqrf(2.5f) : sqrf(4.5f));
            }

            return t;
        }();

        return table.data();
    }

    inline unsigned char
    sigmaToIndex(float sigma)
    {
        return (unsigned char)clampI((int)(sigma / kCaptureGaussFraction), 0, 255);
    }

    // The full 9x9 sum of darktable's fast paths, in their order. d points at the centre pixel.
    inline float
    convolveLarge(const float* d,
                  const float* kern,
                  std::ptrdiff_t w1)
    {
        const std::ptrdiff_t w2 = 2 * w1;
        const std::ptrdiff_t w3 = 3 * w1;
        const std::ptrdiff_t w4 = 4 * w1;

        return kern[10 + 4] * (d[-w4 - 2] + d[-w4 + 2] + d[-w2 - 4] + d[-w2 + 4] + d[w2 - 4] + d[w2 + 4] + d[w4 - 2] + d[w4 + 2]) + kern[5 + 4] * (d[-w4 - 1] + d[-w4 + 1] + d[-w1 - 4] + d[-w1 + 4] + d[w1 - 4] + d[w1 + 4] + d[w4 - 1] + d[w4 + 1]) + kern[4] * (d[-w4] + d[-4] + d[4] + d[w4]) + kern[15 + 3] * (d[-w3 - 3] + d[-w3 + 3] + d[w3 - 3] + d[w3 + 3]) + kern[10 + 3] * (d[-w3 - 2] + d[-w3 + 2] + d[-w2 - 3] + d[-w2 + 3] + d[w2 - 3] + d[w2 + 3] + d[w3 - 2] + d[w3 + 2]) + kern[5 + 3] * (d[-w3 - 1] + d[-w3 + 1] + d[-w1 - 3] + d[-w1 + 3] + d[w1 - 3] + d[w1 + 3] + d[w3 - 1] + d[w3 + 1]) + kern[3] * (d[-w3] + d[-3] + d[3] + d[w3]) + kern[10 + 2] * (d[-w2 - 2] + d[-w2 + 2] + d[w2 - 2] + d[w2 + 2]) + kern[5 + 2] * (d[-w2 - 1] + d[-w2 + 1] + d[-w1 - 2] + d[-w1 + 2] + d[w1 - 2] + d[w1 + 2] + d[w2 - 1] + d[w2 + 1]) + kern[2] * (d[-w2] + d[-2] + d[2] + d[w2]) + kern[5 + 1] * (d[-w1 - 1] + d[-w1 + 1] + d[w1 - 1] + d[w1 + 1]) + kern[1] * (d[-w1] + d[-1] + d[1] + d[w1]) + kern[0] * (d[0]);
    }

    // The 5x5 sum of the capture blurs' fast path for a sigma under CAPTURE_SMALL.
    inline float
    convolveSmall(const float* d,
                  const float* kern,
                  std::ptrdiff_t w1)
    {
        const std::ptrdiff_t w2 = 2 * w1;

        return kern[5 + 2] * (d[-w2 - 1] + d[-w2 + 1] + d[-w1 - 2] + d[-w1 + 2] + d[w1 - 2] + d[w1 + 2] + d[w2 - 1] + d[w2 + 1]) + kern[2] * (d[-w2] + d[-2] + d[2] + d[w2]) + kern[5 + 1] * (d[-w1 - 1] + d[-w1 + 1] + d[w1 - 1] + d[w1 + 1]) + kern[1] * (d[-w1] + d[-1] + d[1] + d[w1]) + kern[0] * (d[0]);
    }

    // Where a buffer sits in its frame.
    struct Placement {
        int width;
        int height;
        int frameRow;
        int frameCol;
        int frameWidth;
        int frameHeight;
    };

    // darktable's slow path at the image's border: every in-frame neighbour up to bd away, row by
    // row. Neighbours outside the buffer are skipped too; only pixels whose result is outside what
    // the buffer outputs read them.
    inline float
    convolveBorder(const float* in,
                   const float* kern,
                   int row,
                   int col,
                   int bd,
                   const Placement& p)
    {
        float val = 0.0f;

        for (int ir = -bd; ir <= bd; ir++) {
            const int irow = row + ir;
            const int frow = p.frameRow + irow;
            if ((frow >= 0) && (frow < p.frameHeight) && (irow >= 0) && (irow < p.height)) {
                for (int ic = -bd; ic <= bd; ic++) {
                    const int icol = col + ic;
                    const int fcol = p.frameCol + icol;
                    if ((fcol >= 0) && (fcol < p.frameWidth) && (icol >= 0) && (icol < p.width)) {
                        val += kern[5 * std::abs(ir) + std::abs(ic)] * in[(std::size_t)irow * p.width + icol];
                    }
                }
            }
        }

        return val;
    }

    // The convolution of in at (row, col) by darktable's rule: the fast path when the pixel is bd or
    // more from the frame's edges, the border loop otherwise.
    inline float
    convolveAt(const float* in,
               const float* kern,
               int row,
               int col,
               int bd,
               bool small,
               const Placement& p)
    {
        const int frow = p.frameRow + row;
        const int fcol = p.frameCol + col;
        const bool frameInterior = (fcol >= bd) && (frow >= bd) && (fcol < p.frameWidth - bd) && (frow < p.frameHeight - bd);
        const bool bufferInterior = (col >= bd) && (row >= bd) && (col < p.width - bd) && (row < p.height - bd);

        if (frameInterior && bufferInterior) {
            const float* d = in + (std::size_t)row * p.width + col;

            return small ? convolveSmall(d, kern, p.width) : convolveLarge(d, kern, p.width);
        }

        return convolveBorder(in, kern, row, col, bd, p);
    }

    // darktable's _blur_div(): out = luminance / the blur of in, where blend is positive.
    void
    blurDiv(const float* in,
            float* out,
            const float* luminance,
            const float* blend,
            const unsigned char* table,
            const Placement& p,
            const ParallelFor& parallel)
    {
        const float* kernels = captureKernels();
        const unsigned char idxSmall = sigmaToIndex(kCaptureSmall);

        parallel(p.height, [&](int row) {
            for (int col = 0; col < p.width; col++) {
                const std::size_t i = (std::size_t)row * p.width + col;
                if (blend[i] > 0.0f) {
                    const float* kern = kernels + (std::size_t)kCaptureKernelSize * table[i];
                    const bool small = table[i] < idxSmall;
                    const float val = convolveAt(in, kern, row, col, small ? 2 : 4, small, p);
                    out[i] = luminance[i] / maxF(val, kCaptureYMin);
                }
            }
        });
    }

    // darktable's _blur_mul(): out *= the blur of in, where blend is positive.
    void
    blurMul(const float* in,
            float* out,
            const float* blend,
            const unsigned char* table,
            const Placement& p,
            const ParallelFor& parallel)
    {
        const float* kernels = captureKernels();
        const unsigned char idxSmall = sigmaToIndex(kCaptureSmall);

        parallel(p.height, [&](int row) {
            for (int col = 0; col < p.width; col++) {
                const std::size_t i = (std::size_t)row * p.width + col;
                if (blend[i] > 0.0f) {
                    const float* kern = kernels + (std::size_t)kCaptureKernelSize * table[i];
                    const bool small = table[i] < idxSmall;
                    out[i] *= convolveAt(in, kern, row, col, small ? 2 : 4, small, p);
                }
            }
        });
    }

    // darktable's _prepare_blend(): Yold the BT.709 luminance of rgb, and mask 1 but for the
    // frame's 2 outermost photosites and a 5x5 cross (corners left out) around every photosite
    // inside them that is clipped in cfa or darker than CAPTURE_YMIN. darktable scatters the zeros;
    // gathering them over the same symmetric pattern gives the same mask without racing writes.
    void
    prepareBlend(const float* cfa,
                 const float* rgb,
                 std::uint32_t filters,
                 const std::uint8_t (*xtrans)[6],
                 float* mask,
                 float* Yold,
                 const float whites[4],
                 const Placement& p,
                 const ParallelFor& parallel)
    {
        std::vector<unsigned char> hot((std::size_t)p.width * p.height, 0);

        parallel(p.height, [&](int row) {
            const int frow = p.frameRow + row;
            for (int col = 0; col < p.width; col++) {
                const int fcolumn = p.frameCol + col;
                const std::size_t k = (std::size_t)row * p.width + col;
                const float yw0 = 0.212671f * rgb[k * 4 + 0];
                const float yw1 = 0.715160f * rgb[k * 4 + 1];
                const float yw2 = 0.072169f * rgb[k * 4 + 2];
                Yold[k] = maxF(0.0f, yw0 + yw1 + yw2);
                if ((frow > 1) && (fcolumn > 1) && (frow < p.frameHeight - 2) && (fcolumn < p.frameWidth - 2)) {
                    bool heat;
                    if (filters) {
                        const int color = fcol(row, col, filters, xtrans);
                        heat = cfa[k] > whites[color & 3];
                    } else {
                        heat = cfa[k] > kCaptureCfaClip;
                    }
                    hot[k] = (heat || (Yold[k] < kCaptureYMin)) ? 1 : 0;
                }
            }
        });
        // The cross: rows -2 and +2 over columns -1..+1, rows -1..+1 over columns -2..+2.
        parallel(p.height, [&](int row) {
            const int frow = p.frameRow + row;
            for (int col = 0; col < p.width; col++) {
                const int fcolumn = p.frameCol + col;
                const std::size_t k = (std::size_t)row * p.width + col;
                if (!((frow > 1) && (fcolumn > 1) && (frow < p.frameHeight - 2) && (fcolumn < p.frameWidth - 2))) {
                    mask[k] = 0.0f;
                    continue;
                }
                bool zero = false;
                for (int dy = -2; (dy <= 2) && !zero; dy++) {
                    const int r = row + dy;
                    if ((r < 0) || (r >= p.height)) {
                        continue;
                    }
                    const int span = (std::abs(dy) == 2) ? 1 : 2;
                    for (int dx = -span; dx <= span; dx++) {
                        const int c = col + dx;
                        if ((c >= 0) && (c < p.width) && hot[(std::size_t)r * p.width + c]) {
                            zero = true;
                            break;
                        }
                    }
                }
                mask[k] = zero ? 0.0f : 1.0f;
            }
        });
    } // prepareBlend

    // darktable's _modify_blend(): blend scaled by a sigmoid of the modified coefficient of variation
    // of Yold over 21 photosites around each pixel, the window clamped inside the frame's 2
    // outermost photosites; luminance a copy of Yold.
    void
    modifyBlend(float* blend,
                const float* Yold,
                float* luminance,
                float dthresh,
                const Placement& p,
                const ParallelFor& parallel)
    {
        const float threshold = 0.6f * sqrf(dthresh);
        const float tscale = 200.0f;
        const float offset = -2.5f + tscale * threshold / 2.0f;
        const std::size_t w = (std::size_t)p.width;

        parallel(p.height, [&](int irow) {
            const int row = clampI(p.frameRow + irow, 2, p.frameHeight - 3) - p.frameRow;
            for (int icol = 0; icol < p.width; icol++) {
                const std::size_t k = (std::size_t)irow * w + icol;
                const int col = clampI(p.frameCol + icol, 2, p.frameWidth - 3) - p.frameCol;
                luminance[k] = Yold[k];
                if ((row < 2) || (row > p.height - 3) || (col < 2) || (col > p.width - 3)) {
                    // The window leaves the buffer: only past what the buffer outputs.
                    blend[k] = 0.0f;
                    continue;
                }
                float sum = 0.0f;
                float sum_sq = 0.0f;
                for (int y = row - 1; y < row + 2; y++) {
                    for (int x = col - 2; x < col + 3; x++) {
                        sum += Yold[(std::size_t)y * w + x];
                        sum_sq += sqrf(Yold[(std::size_t)y * w + x]);
                    }
                }
                for (int x = col - 1; x < col + 2; x++) {
                    sum += Yold[(std::size_t)(row - 2) * w + x];
                    sum_sq += sqrf(Yold[(std::size_t)(row - 2) * w + x]);
                    sum += Yold[(std::size_t)(row + 2) * w + x];
                    sum_sq += sqrf(Yold[(std::size_t)(row + 2) * w + x]);
                }
                const float sum_of_squares = maxF(0.0f, sum_sq - sqrf(sum) / 21.0f);
                const float std_deviation = sqrtf(sum_of_squares / 21.0f);
                const float mean = maxF(kNormMin, sum / 21.0f);
                const float modified_coef_variation = std_deviation / sqrtf(mean);
                const float t = logf(1.0f + modified_coef_variation);
                const float weight = 1.0f / (1.0f + expf(offset - tscale * t));
                blend[k] = clip01(blend[k] * 1.01011f * (weight - 0.01f));
            }
        });
    }

    // gaussian.c's _fast_9x9_kernel_1() at sigma 2, clamped to [0, 1], the border loop at the
    // frame's 4 outermost photosites.
    void
    gaussianBlend(const float* in,
                  float* out,
                  const Placement& p,
                  const ParallelFor& parallel)
    {
        float kern[kCaptureKernelSize];

        gaussCoeffs(kern, 2.0f, 4.5f * 4.5f);
        parallel(p.height, [&](int row) {
            for (int col = 0; col < p.width; col++) {
                const std::size_t i = (std::size_t)row * p.width + col;
                out[i] = clampF(convolveAt(in, kern, row, col, 4, false, p), 0.0f, 1.0f);
            }
        });
    }

    // darktable's _cs_precalc_gauss_idx(): each pixel's kernel, the radius grown towards the
    // corners by boost beyond the sharp centre and faded to 0 over the frame's 8 outermost
    // photosites.
    void
    gaussIndex(unsigned char* table,
               float isigma,
               float boost,
               float centre,
               const Placement& p,
               const ParallelFor& parallel)
    {
        const int rwidth = p.frameWidth / 2;
        const int rheight = p.frameHeight / 2;
        const float mdim = (float)std::min(rwidth, rheight);
        const float cboost = 1.0f + 8.0f * sqrf(centre);

        parallel(p.height, [&](int r) {
            const int row = p.frameRow + r;
            const float frow = (float)(row - rheight);
            for (int c = 0; c < p.width; c++) {
                const int col = p.frameCol + c;
                const float fcolumn = (float)(col - rwidth);
                const float sc = hypotf(frow, fcolumn) / mdim;
                const float corr = cboost * boost * sqrf(maxF(0.0f, sc - 0.5f - centre));
                const int edge = std::min(8, std::min(p.frameHeight - row - 1, std::min(p.frameWidth - col - 1, std::min(col, row))));
                const float sigma = (isigma + corr) * 0.125f * (float)edge;
                table[(std::size_t)r * p.width + c] = sigmaToIndex(sigma);
            }
        });
    }

    // The largest ratio of photosites in a row, by _calcRadiusBayer()'s and _calcRadiusMono()'s
    // rules; mono walks every second column from 5, a Bayer row from its first green.
    float
    radiusRowBayer(const float* in,
                   int width,
                   int row,
                   int firstCol)
    {
        float maxRatio = 1.0f;

        for (int col = firstCol; col < width - 4; col += 2) {
            const float* cfa = in + (std::size_t)row * width + col;
            const float val00 = cfa[0];
            if (val00 > kRawEps) {
                const float val1m1 = cfa[width - 1];
                const float val1p1 = cfa[width + 1];
                const float maxVal0 = maxF(val00, val1m1);
                if ((val1m1 > kRawEps) && (maxVal0 > kLowerLimit)) {
                    const float minVal = minF(val00, val1m1);
                    if (maxVal0 > maxRatio * minVal) {
                        bool clipped = false;
                        if (maxVal0 == val00) {
                            // check for influence by clipped green in neighborhood
                            if (maxF(maxF(cfa[-width - 1], cfa[-width + 1]), val1p1) >= kUpperLimit) {
                                clipped = true;
                            }
                        } else {
                            if (maxF(maxF(maxF(cfa[-2], val00), cfa[2 * width - 2]), cfa[2 * width]) >= kUpperLimit) {
                                clipped = true;
                            }
                        }
                        if (!clipped) {
                            maxRatio = maxVal0 / minVal;
                        }
                    }
                }

                const float maxVal1 = maxF(val00, val1p1);
                if ((val1p1 > kRawEps) && (maxVal1 > kLowerLimit)) {
                    const float minVal = minF(val00, val1p1);
                    if (maxVal1 > maxRatio * minVal) {
                        if (maxVal1 == val00) {
                            if (maxF(maxF(cfa[-width - 1], cfa[-width + 1]), val1p1) >= kUpperLimit) {
                                continue;
                            }
                        } else {
                            if (maxF(maxF(maxF(val00, cfa[2]), cfa[2 * width]), cfa[2 * width + 2]) >= kUpperLimit) {
                                continue;
                            }
                        }
                        maxRatio = maxVal1 / minVal;
                    }
                }
            }
        }

        return maxRatio;
    } // radiusRowBayer

    inline void
    ratio(float a,
          float b,
          float* maxRatio)
    {
        const float maxVal = maxF(a, b);

        if (maxVal > kLowerLimit) {
            const float minVal = minF(a, b);
            if (maxVal > *maxRatio * minVal) {
                *maxRatio = maxVal / minVal;
            }
        }
    }

    // The largest ratio of photosites in a row by _calcRadiusXtrans()'s rules.
    float
    radiusRowXTrans(const float* in,
                    int width,
                    int row,
                    int startx)
    {
        float maxRatio = 1.0f;

        for (int col = startx + 2; col < width - 4; col += 3) {
            const float* cfa = in + (std::size_t)row * width + col;
            const float valp1p1 = cfa[width + 1];
            const bool squareClipped = maxF(maxF(maxF(valp1p1, cfa[width + 2]), cfa[2 * width + 1]), cfa[2 * width + 2]) >= kUpperLimit;
            const float greenSolitary = cfa[0];
            if ((greenSolitary > kRawEps) && (maxF(cfa[-width - 1], cfa[-width + 1]) < kUpperLimit)) {
                if (greenSolitary < kUpperLimit) {
                    const float valp1m1 = cfa[width - 1];
                    if ((valp1m1 > kRawEps) && (maxF(maxF(maxF(cfa[width - 2], valp1m1), cfa[2 * width - 2]), cfa[width - 1]) < kUpperLimit)) {
                        ratio(greenSolitary, valp1m1, &maxRatio);
                    }
                    if ((valp1p1 > kRawEps) && !squareClipped) {
                        ratio(greenSolitary, valp1p1, &maxRatio);
                    }
                }
            }

            if (!squareClipped) {
                const float valp2p2 = cfa[2 * width + 2];
                if (valp2p2 > kRawEps) {
                    if (valp1p1 > kRawEps) {
                        ratio(valp1p1, valp2p2, &maxRatio);
                    }
                    const float greenSolitaryRight = cfa[3 * width + 3];
                    if (maxF(maxF(greenSolitaryRight, cfa[4 * width + 2]), cfa[4 * width + 4]) < kUpperLimit) {
                        if (greenSolitaryRight > kRawEps) {
                            ratio(greenSolitaryRight, valp2p2, &maxRatio);
                        }
                    }
                }
                const float valp1p2 = cfa[width + 2];
                const float valp2p1 = cfa[2 * width + 1];
                if (valp2p1 > kRawEps) {
                    if (valp1p2 > kRawEps) {
                        ratio(valp1p2, valp2p1, &maxRatio);
                    }
                    const float greenSolitaryLeft = cfa[3 * width];
                    if (maxF(maxF(greenSolitaryLeft, cfa[4 * width - 1]), cfa[4 * width + 1]) < kUpperLimit) {
                        if (greenSolitaryLeft > kRawEps) {
                            ratio(greenSolitaryLeft, valp2p1, &maxRatio);
                        }
                    }
                }
            }
        }

        return maxRatio;
    } // radiusRowXTrans

    // The maximum over rows of rowMax(row) for row = first, first + step, ... < end, and 1. darktable
    // reduces with OpenMP's max; a ratio's admission does not depend on the order, so per-row maxima
    // give the same result on any number of threads.
    template <typename RowMax>
    float
    maxRatioOverRows(int first,
                     int end,
                     int step,
                     const RowMax& rowMax,
                     const ParallelFor& parallel)
    {
        if (first >= end) {
            return 1.0f;
        }
        const int count = (end - first + step - 1) / step;
        std::vector<float> rows((std::size_t)count, 1.0f);
        parallel(count, [&](int i) {
            rows[(std::size_t)i] = rowMax(first + i * step);
        });
        float maxRatio = 1.0f;
        for (float r : rows) {
            maxRatio = maxF(maxRatio, r);
        }

        return maxRatio;
    }
} // namespace

int
captureIterations(int slider)
{
    return slider + (int)(0.000065f * powf((float)slider, 4.0f));
}

float
captureVarianceThreshold(double sensorWhiteLevel,
                         double iso)
{
    float threshold = 0.4f;

    // >12bit sensors always provide more room
    if (sensorWhiteLevel > 4096) {
        threshold -= 0.07f;
    }
    const float isoF = (float)iso;
    const float clamped = (isoF > 100.f) ? ((isoF < 1000.f) ? isoF : 1000.f) : 100.f;
    const float iso_factor = (float)(600 - clamped) / 100.0f;
    threshold -= 0.012f * iso_factor;

    return threshold;
}

int
captureHalo(int iterations)
{
    return 8 * std::max(0, iterations) + 4 + 4 + 2;
}

CaptureRadiusArea
captureRadiusArea(int frameWidth,
                  int frameHeight)
{
    // _calc_auto_radius() for a roi covering the whole frame: x = y = 0, width = p_width.
    CaptureRadiusArea area;
    const int pwidth = frameWidth;
    const int pheight = frameHeight;
    const int dx = (0 < pwidth / 5) ? (int)(0.2f * (float)pwidth) : 0;
    const int dy = (0 < pheight / 5) ? (int)(0.2f * (float)pheight) : 0;
    const float widthLeft = (float)(pwidth - dx);
    const float widthCentre = 0.8f * (float)pwidth - 0.0f - (float)dx;
    const float heightLeft = (float)(pheight - dy);
    const float heightCentre = 0.8f * (float)pheight - 0.0f - (float)dy;

    area.x = dx;
    area.y = dy;
    area.width = (int)((widthLeft < widthCentre) ? widthLeft : widthCentre);
    area.height = (int)((heightLeft < heightCentre) ? heightLeft : heightCentre);
    area.reliable = (pwidth > 0) && (pheight > 0) && !((((float)area.width / (float)pwidth) < 0.2f) || (((float)area.height / (float)pheight) < 0.2f));
    if (!area.reliable) {
        area.width = std::max(0, area.width);
        area.height = std::max(0, area.height);
    }

    return area;
}

float
captureAutoRadius(const float* area,
                  int width,
                  int height,
                  std::uint32_t filters,
                  const std::uint8_t (*xtrans)[6],
                  const float wbApplied[3],
                  const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return 0.5f;
    }
    const float coeff[4] = {
        1.0f / maxF(1.0f, wbApplied[0]),
        1.0f / maxF(1.0f, wbApplied[1]),
        1.0f / maxF(1.0f, wbApplied[2]),
        1.0f
    };
    std::vector<float> input((std::size_t)width * height);

    parallel(height, [&](int row) {
        for (int col = 0; col < width; col++) {
            const std::size_t k = (std::size_t)row * width + col;
            if (!filters) {
                input[k] = area[k];
            } else if (filters == kFiltersXTrans) {
                input[k] = area[k] * coeff[fcXTrans(row, col, xtrans)];
            } else {
                input[k] = area[k] * coeff[fcBayer((std::size_t)row, (std::size_t)col, filters)];
            }
        }
    });
    const float* in = input.data();
    float radius;
    if (!filters) {
        const float maxRatio = maxRatioOverRows(4, height - 4, 1, [&](int row) { return radiusRowBayer(in, width, row, 5); }, parallel);
        radius = sqrtf(1.0f / logf(maxRatio));
    } else if (filters != kFiltersXTrans) {
        const int fc[2] = { fcBayer(0, 0, filters), fcBayer(1, 0, filters) };
        const float maxRatio = maxRatioOverRows(4, height - 4, 1, [&](int row) { return radiusRowBayer(in, width, row, 5 + (fc[row & 1] & 1)); }, parallel);
        radius = sqrtf(1.0f / logf(maxRatio));
    } else {
        // darktable's search, which leaves starty one past the row it finds: the break leaves
        // the inner loop and the outer one still increments.
        int startx = 6, starty = 6;
        bool found = false;
        for (starty = 6; starty < 12 && !found; starty++) {
            for (startx = 6; startx < 12 && !found; startx++) {
                if (fcXTrans(starty, startx, xtrans) == 1) {
                    if (fcXTrans(starty, startx - 1, xtrans) != fcXTrans(starty, startx + 1, xtrans)) {
                        if (fcXTrans(starty - 1, startx, xtrans) != 1) {
                            if (fcXTrans(starty, startx - 1, xtrans) != 1) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        const int sx = startx;
        const float maxRatio = maxRatioOverRows(starty + 2, height - 4, 3, [&](int row) { return radiusRowXTrans(in, width, row, sx); }, parallel);
        radius = 0.2f + sqrtf(1.0f / logf(maxRatio));
    }

    return clampF(radius, 0.0f, 1.5f);
} // captureAutoRadius

void
captureSharpen(float* rgb,
               const float* cfa,
               int width,
               int height,
               int frameRow,
               int frameCol,
               int frameWidth,
               int frameHeight,
               std::uint32_t filters,
               const std::uint8_t (*xtrans)[6],
               const float wbApplied[3],
               const CaptureSettings& settings,
               const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0) || (frameWidth < 5) || (frameHeight < 5)) {
        return;
    }
    const Placement p = { width, height, frameRow, frameCol, frameWidth, frameHeight };
    const std::size_t pixels = (std::size_t)width * height;
    // darktable's icoeffs: a fourth colour, which no pattern here has, would count as clipped.
    const float whites[4] = {
        kCaptureCfaClip * wbApplied[0],
        kCaptureCfaClip * wbApplied[1],
        kCaptureCfaClip * wbApplied[2],
        0.0f
    };
    std::vector<float> luminance(pixels), tmp2(pixels), tmp1(pixels), blendmask(pixels);

    // tmp2 will hold the temporary clipmask, tmp1 holds Y data
    prepareBlend(cfa, rgb, filters, xtrans, tmp2.data(), tmp1.data(), whites, p, parallel);
    // modify clipmask in tmp2 according to Y variance, also write L to luminance
    modifyBlend(tmp2.data(), tmp1.data(), luminance.data(), settings.threshold, p, parallel);
    gaussianBlend(tmp2.data(), blendmask.data(), p, parallel);
    // after the blur, very tiny edges will not get enough strength of sharpening
    // use the maximum of (unblurred,blurred) values.
    parallel(height, [&](int row) {
        for (std::size_t k = (std::size_t)row * width; k < (std::size_t)(row + 1) * width; k++) {
            const float diff = tmp2[k] - blendmask[k];
            const float w_tmp2 = 1.0f / (1.0f + expf(5.0f - 10.0f * diff));
            blendmask[k] = clip01(w_tmp2 * tmp2[k] + (1.0f - w_tmp2) * blendmask[k]);
        }
    });

    std::vector<unsigned char> gaussIdx(pixels);
    gaussIndex(gaussIdx.data(), settings.radius, settings.boost, settings.center, p, parallel);

    for (int iter = 0; iter < settings.iterations; iter++) {
        blurDiv(tmp1.data(), tmp2.data(), luminance.data(), blendmask.data(), gaussIdx.data(), p, parallel);
        blurMul(tmp2.data(), tmp1.data(), blendmask.data(), gaussIdx.data(), p, parallel);
    }

    parallel(height, [&](int row) {
        for (std::size_t k = (std::size_t)row * width; k < (std::size_t)(row + 1) * width; k++) {
            if (blendmask[k] > 0.0f) {
                const float a = clip01(blendmask[k]);
                const float luminance_new = a * (tmp1[k] - luminance[k]) + luminance[k];
                const float factor = luminance_new / maxF(luminance[k], kCaptureYMin);
                for (int c = 0; c < 4; c++) {
                    rgb[k * 4 + c] *= factor;
                }
            }
        }
    });
} // captureSharpen
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
