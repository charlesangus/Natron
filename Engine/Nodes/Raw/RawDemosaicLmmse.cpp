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
 * src/iop/demosaicing/lmmse.c, C path only. darktable is distributed under the GNU GPL,
 * version 3 or (at your option) any later version; this file is therefore GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2021-2026 darktable developers.
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
 * The lmmse code base used for the darktable port has been taken from rawtherapee derived
 * librtprocess. Adapt for dt and tiling - hanno schwalm 06/2021
 *
 * LSMME demosaicing algorithm
 * L. Zhang and X. Wu,
 * Color demozaicing via directional Linear Minimum Mean Square-error Estimation,
 * IEEE Trans. on Image Processing, vol. 14, pp. 2167-2178, Dec. 2005.
 *
 * Adapted to RawTherapee by Jacques Desmis 3/2013
 * Improved speed and reduced memory consumption by Ingo Weyrich 2/2015
 *
 * Refinement based on EECI demosaicing algorithm by L. Chang and Y.P. Tan
 * Paul Lee
 * Adapted for RawTherapee - Jacques Desmis 04/2013
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
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    // darktable's DT_LMMSE_TILESIZE, LMMSE_OVERLAP and BORDER_AROUND.
    const int kTile = 136;
    const int kOverlap = 8;
    const int kBorder = 4;
    const int kTileInt = kTile - 2 * kBorder;
    const int kTileValid = kTileInt - 2 * kOverlap;
    const std::ptrdiff_t w1 = kTile;
    const std::ptrdiff_t w2 = kTile * 2;
    const std::ptrdiff_t w3 = kTile * 3;
    const std::ptrdiff_t w4 = kTile * 4;

    struct LmmseGamma {
        std::vector<float> in;
        std::vector<float> out;
    };

    // darktable's _init_lmmse_gamma(), built once on first use instead of into module globals.
    const LmmseGamma&
    lmmseGamma()
    {
        static const LmmseGamma tables = [] {
            LmmseGamma t;
            t.in.resize(65536);
            t.out.resize(65536);
            for (int j = 0; j < 65536; ++j) {
                const double x = (double)j / 65535.0;
                t.in[j] = (float)((x <= 0.001867) ? x * 17.0 : 1.044445 * std::exp(std::log(x) / 2.4) - 0.044445);
                t.out[j] = (float)((x <= 0.031746) ? x / 17.0 : std::exp(std::log((x + 0.044445) / 1.044445) * 2.4));
            }

            return t;
        }();

        return tables;
    }

    inline float
    median3f(float x0,
             float x1,
             float x2)
    {
        return std::fmax(std::fmin(x0, x1), std::fmin(x2, std::fmax(x0, x1)));
    }

    inline float
    calcGamma(float val,
              const float* table)
    {
        const float index = val * 65535.0f;

        // darktable's test is index < 0, which lets a NaN index the table.
        if (!(index >= 0.0f)) {
            return 0.0f;
        }
        if (index > 65534.99f) {
            return 1.0f;
        }
        const int idx = (int)index;
        const float diff = index - (float)idx;
        const float p1 = table[idx];
        const float p2 = table[idx + 1] - p1;

        return p1 + p2 * diff;
    }

    inline float
    sqrf(float a)
    {
        return a * a;
    }

    inline float
    clampf(float a,
           float mn,
           float mx)
    {
        return (a >= mn) ? ((a <= mx) ? a : mx) : mn;
    }

    // glib's MIN and MAX, which darktable's median9f() uses.
    inline float
    dtMin(float a,
          float b)
    {
        return (a < b) ? a : b;
    }

    inline float
    dtMax(float a,
          float b)
    {
        return (a > b) ? a : b;
    }

    float
    median9f(const float* p)
    {
        float p1 = dtMin(p[1], p[2]);
        float p2 = dtMax(p[1], p[2]);
        float p4 = dtMin(p[4], p[5]);
        float p5 = dtMax(p[4], p[5]);
        float p7 = dtMin(p[7], p[8]);
        float p8 = dtMax(p[7], p[8]);
        const float p0 = dtMin(p[0], p1);
        const float p1a = dtMax(p[0], p1);
        float p3 = dtMin(p[3], p4);
        float p4a = dtMax(p[3], p4);
        float p6 = dtMin(p[6], p7);
        float p7a = dtMax(p[6], p7);
        p1 = dtMin(p1a, p2);
        p2 = dtMax(p1a, p2);
        p4 = dtMin(p4a, p5);
        p5 = dtMax(p4a, p5);
        p7 = dtMin(p7a, p8);
        p8 = dtMax(p7a, p8);
        p3 = dtMax(p0, p3);
        p5 = dtMin(p5, p8);
        p7a = dtMax(p4, p7);
        p4 = dtMin(p4, p7);
        p6 = dtMax(p3, p6);
        p4 = dtMax(p1, p4);
        p2 = dtMin(p2, p5);
        p4a = dtMin(p4, p7a);
        p4 = dtMin(p4a, p2);
        p2 = dtMax(p4a, p2);
        p4 = dtMax(p6, p4);

        return dtMin(p2, p4);
    }

    inline int
    FC(int row,
       int col,
       std::uint32_t filters)
    {
        return fcBayer((std::size_t)row, (std::size_t)col, filters);
    }

    // The tiles darktable lays over an axis of n photosites, 1 + (n - 17) / 112 truncated toward 0.
    int
    tileCount(int n)
    {
        return 1 + (n - 2 * kOverlap - 1) / kTileValid;
    }
} // namespace

void
lmmseSpan(int start,
          int end,
          int n,
          int* b0,
          int* b1)
{
    if ((n < 2 * kBorder) || (end <= start)) {
        *b0 = 0;
        *b1 = n;

        return;
    }
    const int tiles = tileCount(n);
    // Tile t writes [t * 112 + 8, t * 112 + 120), the first from photosite 4 and the last to n - 4;
    // the 4 outermost photosites come from PPG.
    const auto tileOf = [&](int p) {
        return std::min(tiles - 1, (p < kOverlap) ? 0 : (p - kOverlap) / kTileValid);
    };
    const int first = tileOf(start);
    const int last = tileOf(end - 1);
    // A tile behaves differently as the first or last of its buffer, so the buffer starts one
    // tile early and ends where the next tile exists and every photosite the last tile's border
    // reaches is still inside, unless the image edge is there anyway.
    *b0 = (first == 0) ? 0 : (first - 1) * kTileValid;
    *b1 = (last == tiles - 1) ? n : std::min(n, last * kTileValid + kTile - kBorder);
}

void
lmmse(float* out,
      const float* in,
      int width,
      int height,
      std::uint32_t filters,
      int mode,
      float scaler,
      const ParallelFor& parallel)
{
    ppg(out, in, width, height, filters, 0.0f, kBorder, parallel);
    if ((width < 2 * kBorder) || (height < 2 * kBorder)) {
        return;
    }

    const LmmseGamma& gamma = lmmseGamma();
    const float* gammaIn = gamma.in.data();
    const float* gammaOut = gamma.out.data();

    float h0 = 1.0f;
    float h1 = std::exp(-1.0f / 8.0f);
    float h2 = std::exp(-4.0f / 8.0f);
    float h3 = std::exp(-9.0f / 8.0f);
    float h4 = std::exp(-16.0f / 8.0f);
    const float hs = h0 + 2.0f * (h1 + h2 + h3 + h4);
    h0 /= hs;
    h1 /= hs;
    h2 /= hs;
    h3 /= hs;
    h4 /= hs;

    const int medians = (mode < 2) ? mode : 3;
    const int refine = (mode > 2) ? mode - 2 : 0;
    const float revscaler = 1.0f / scaler;

    const int numVertical = tileCount(height);
    const int numHorizontal = tileCount(width);
    // darktable clears each thread's scratch once and reuses it, so a tile's reads past what it
    // writes see whichever tile the thread ran before; clearing per tile makes every tile see
    // what darktable's first tile on a thread sees.
    parallel(numVertical * numHorizontal, [&](int tileIndex) {
        const int tileVertical = tileIndex / numHorizontal;
        const int tileHorizontal = tileIndex % numHorizontal;
        std::vector<float> buffer((std::size_t)kTile * kTile * 6, 0.0f);
        float* qix[6];
        for (int i = 0; i < 6; ++i) {
            qix[i] = buffer.data() + (std::size_t)i * kTile * kTile;
        }

        const int rowStart = tileVertical * kTileValid;
        const int rowEnd = std::min(rowStart + kTileInt, height);
        const int colStart = tileHorizontal * kTileValid;
        const int colEnd = std::min(colStart + kTileInt, width);
        const int tileRows = std::min(rowEnd - rowStart, kTileInt);
        const int tileCols = std::min(colEnd - colStart, kTileInt);
        const int lastRr = tileRows + 2 * kBorder;
        const int lastCc = tileCols + 2 * kBorder;

        for (int rrr = kBorder, row = rowStart; rrr < tileRows + kBorder; ++rrr, ++row) {
            float* cfa = qix[5] + rrr * kTile + kBorder;
            std::size_t idx = (std::size_t)row * width + colStart;
            for (int ccc = kBorder; ccc < tileCols + kBorder; ++ccc, ++cfa, ++idx) {
                cfa[0] = calcGamma(revscaler * in[idx], gammaIn);
            }
        }

        // G-R(B)
        for (int rr = 2; rr < lastRr - 2; ++rr) {
            // G-R(B) at R(B) location
            for (int cc = 2 + (FC(rr, 2, filters) & 1); cc < lastCc - 2; cc += 2) {
                const float* cfa = qix[5] + rr * kTile + cc;
                const float v0 = 0.0625f * (cfa[-w1 - 1] + cfa[-w1 + 1] + cfa[w1 - 1] + cfa[w1 + 1]) + 0.25f * cfa[0];
                // horizontal
                float* hdiff = qix[0] + rr * kTile + cc;
                hdiff[0] = -0.25f * (cfa[-2] + cfa[2]) + 0.5f * (cfa[-1] + cfa[0] + cfa[1]);
                const float Y0 = v0 + 0.5f * hdiff[0];
                hdiff[0] = (cfa[0] > 1.75f * Y0) ? median3f(hdiff[0], cfa[-1], cfa[1]) : clampf(hdiff[0], 0.0f, 1.0f);
                hdiff[0] -= cfa[0];

                // vertical
                float* vdiff = qix[1] + rr * kTile + cc;
                vdiff[0] = -0.25f * (cfa[-w2] + cfa[w2]) + 0.5f * (cfa[-w1] + cfa[0] + cfa[w1]);
                const float Y1 = v0 + 0.5f * vdiff[0];
                vdiff[0] = (cfa[0] > 1.75f * Y1) ? median3f(vdiff[0], cfa[-w1], cfa[w1]) : clampf(vdiff[0], 0.0f, 1.0f);
                vdiff[0] -= cfa[0];
            }

            // G-R(B) at G location
            for (int ccc = 2 + (FC(rr, 3, filters) & 1); ccc < lastCc - 2; ccc += 2) {
                const float* cfa = qix[5] + rr * kTile + ccc;
                float* hdiff = qix[0] + rr * kTile + ccc;
                float* vdiff = qix[1] + rr * kTile + ccc;
                hdiff[0] = 0.25f * (cfa[-2] + cfa[2]) - 0.5f * (cfa[-1] + cfa[0] + cfa[1]);
                vdiff[0] = 0.25f * (cfa[-w2] + cfa[w2]) - 0.5f * (cfa[-w1] + cfa[0] + cfa[w1]);
                hdiff[0] = clampf(hdiff[0], -1.0f, 0.0f) + cfa[0];
                vdiff[0] = clampf(vdiff[0], -1.0f, 0.0f) + cfa[0];
            }
        }

        // apply low pass filter on differential colors
        for (int rr = 4; rr < lastRr - 4; ++rr) {
            for (int cc = 4; cc < lastCc - 4; ++cc) {
                const float* hdiff = qix[0] + rr * kTile + cc;
                const float* vdiff = qix[1] + rr * kTile + cc;
                float* hlp = qix[2] + rr * kTile + cc;
                float* vlp = qix[3] + rr * kTile + cc;
                hlp[0] = h0 * hdiff[0] + h1 * (hdiff[-1] + hdiff[1]) + h2 * (hdiff[-2] + hdiff[2]) + h3 * (hdiff[-3] + hdiff[3]) + h4 * (hdiff[-4] + hdiff[4]);
                vlp[0] = h0 * vdiff[0] + h1 * (vdiff[-w1] + vdiff[w1]) + h2 * (vdiff[-w2] + vdiff[w2]) + h3 * (vdiff[-w3] + vdiff[w3]) + h4 * (vdiff[-w4] + vdiff[w4]);
            }
        }

        for (int rr = 4; rr < lastRr - 4; ++rr) {
            for (int cc = 4 + (FC(rr, 4, filters) & 1); cc < lastCc - 4; cc += 2) {
                const float* hdiff = qix[0] + rr * kTile + cc;
                const float* vdiff = qix[1] + rr * kTile + cc;
                const float* hlp = qix[2] + rr * kTile + cc;
                const float* vlp = qix[3] + rr * kTile + cc;
                float* interp = qix[4] + rr * kTile + cc;
                // horizontal
                float p1 = hlp[-4];
                float p2 = hlp[-3];
                float p3 = hlp[-2];
                float p4 = hlp[-1];
                float p5 = hlp[0];
                float p6 = hlp[1];
                float p7 = hlp[2];
                float p8 = hlp[3];
                float p9 = hlp[4];
                float mu = (p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9) / 9.0f;
                float vx = 1e-7f + sqrf(p1 - mu) + sqrf(p2 - mu) + sqrf(p3 - mu) + sqrf(p4 - mu) + sqrf(p5 - mu) + sqrf(p6 - mu) + sqrf(p7 - mu) + sqrf(p8 - mu) + sqrf(p9 - mu);
                p1 -= hdiff[-4];
                p2 -= hdiff[-3];
                p3 -= hdiff[-2];
                p4 -= hdiff[-1];
                p5 -= hdiff[0];
                p6 -= hdiff[1];
                p7 -= hdiff[2];
                p8 -= hdiff[3];
                p9 -= hdiff[4];
                float vn = 1e-7f + sqrf(p1) + sqrf(p2) + sqrf(p3) + sqrf(p4) + sqrf(p5) + sqrf(p6) + sqrf(p7) + sqrf(p8) + sqrf(p9);
                const float xh = (hdiff[0] * vx + hlp[0] * vn) / (vx + vn);
                const float vh = vx * vn / (vx + vn);

                // vertical
                p1 = vlp[-w4];
                p2 = vlp[-w3];
                p3 = vlp[-w2];
                p4 = vlp[-w1];
                p5 = vlp[0];
                p6 = vlp[w1];
                p7 = vlp[w2];
                p8 = vlp[w3];
                p9 = vlp[w4];
                mu = (p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9) / 9.0f;
                vx = 1e-7f + sqrf(p1 - mu) + sqrf(p2 - mu) + sqrf(p3 - mu) + sqrf(p4 - mu) + sqrf(p5 - mu) + sqrf(p6 - mu) + sqrf(p7 - mu) + sqrf(p8 - mu) + sqrf(p9 - mu);
                p1 -= vdiff[-w4];
                p2 -= vdiff[-w3];
                p3 -= vdiff[-w2];
                p4 -= vdiff[-w1];
                p5 -= vdiff[0];
                p6 -= vdiff[w1];
                p7 -= vdiff[w2];
                p8 -= vdiff[w3];
                p9 -= vdiff[w4];
                vn = 1e-7f + sqrf(p1) + sqrf(p2) + sqrf(p3) + sqrf(p4) + sqrf(p5) + sqrf(p6) + sqrf(p7) + sqrf(p8) + sqrf(p9);
                const float xv = (vdiff[0] * vx + vlp[0] * vn) / (vx + vn);
                const float vv = vx * vn / (vx + vn);
                // interpolated G-R(B)
                interp[0] = (xh * vv + xv * vh) / (vh + vv);
            }
        }

        // copy CFA values
        for (int rr = 0, rowIn = rowStart - kBorder; rr < lastRr; ++rr, ++rowIn) {
            for (int cc = 0, colIn = colStart - kBorder; cc < lastCc; ++cc, ++colIn) {
                const int c = FC(rr, cc, filters);
                const bool inside = (rowIn >= 0) && (rowIn < height) && (colIn >= 0) && (colIn < width);
                float* colc = qix[c] + rr * kTile + cc;
                colc[0] = inside ? qix[5][rr * kTile + cc] : 0.0f;
                if (c != 1) {
                    float* col1 = qix[1] + rr * kTile + cc;
                    const float* interp = qix[4] + rr * kTile + cc;
                    col1[0] = inside ? colc[0] + interp[0] : 0.0f;
                }
            }
        }

        // bilinear interpolation for R/B
        // interpolate R/B at G location
        for (int rr = 1; rr < lastRr - 1; ++rr) {
            for (int cc = 1 + (FC(rr, 2, filters) & 1), c = FC(rr, cc + 1, filters); cc < lastCc - 1; cc += 2) {
                float* colc = qix[c] + rr * kTile + cc;
                const float* col1 = qix[1] + rr * kTile + cc;
                colc[0] = col1[0] + 0.5f * (colc[-1] - col1[-1] + colc[1] - col1[1]);
                c = 2 - c;
                colc = qix[c] + rr * kTile + cc;
                colc[0] = col1[0] + 0.5f * (colc[-w1] - col1[-w1] + colc[w1] - col1[w1]);
                c = 2 - c;
            }
        }

        // interpolate R/B at B/R location
        for (int rr = 1; rr < lastRr - 1; ++rr) {
            for (int cc = 1 + (FC(rr, 1, filters) & 1), c = 2 - FC(rr, cc, filters); cc < lastCc - 1; cc += 2) {
                float* colc = qix[c] + rr * kTile + cc;
                const float* col1 = qix[1] + rr * kTile + cc;
                colc[0] = col1[0] + 0.25f * (colc[-w1] - col1[-w1] + colc[-1] - col1[-1] + colc[1] - col1[1] + colc[w1] - col1[w1]);
            }
        }

        // The median and refine passes skip 6 photosites at the image's edges, none at a tile's.
        const int ccmin = (tileHorizontal == 0) ? 6 : 0;
        const int ccmax = lastCc - ((tileHorizontal == numHorizontal - 1) ? 6 : 0);
        const int rrmin = (tileVertical == 0) ? 6 : 0;
        const int rrmax = lastRr - ((tileVertical == numVertical - 1) ? 6 : 0);

        for (int pass = 0; pass < medians; ++pass) {
            // median(R-G) into qix[3], median(B-G) into qix[4]
            for (int rr = 1; rr < lastRr - 1; ++rr) {
                for (int c = 0; c < 3; c += 2) {
                    const int d = c + 3 - ((c == 0) ? 0 : 1);
                    for (int cc = 1; cc < lastCc - 1; ++cc) {
                        float* corr = qix[d] + rr * kTile + cc;
                        const float* colc = qix[c] + rr * kTile + cc;
                        const float* col1 = qix[1] + rr * kTile + cc;
                        const float p[9] = {
                            colc[-w1 - 1] - col1[-w1 - 1],
                            colc[-w1] - col1[-w1],
                            colc[-w1 + 1] - col1[-w1 + 1],
                            colc[-1] - col1[-1],
                            colc[0] - col1[0],
                            colc[1] - col1[1],
                            colc[w1 - 1] - col1[w1 - 1],
                            colc[w1] - col1[w1],
                            colc[w1 + 1] - col1[w1 + 1]
                        };
                        corr[0] = median9f(p);
                    }
                }
            }

            // red/blue at GREEN pixel locations & red/blue and green at BLUE/RED pixel locations
            for (int rr = rrmin; rr < rrmax - 1; ++rr) {
                const std::ptrdiff_t base = (std::ptrdiff_t)rr * kTile;
                int c0 = FC(rr, 0, filters);
                int c1 = FC(rr, 1, filters);
                int cc;
                if (c0 == 1) {
                    c1 = 2 - c1;
                    const int d = c1 + 3 - ((c1 == 0) ? 0 : 1);
                    for (cc = ccmin; cc < ccmax - 1; cc += 2) {
                        const std::ptrdiff_t i = base + cc;
                        qix[0][i] = qix[1][i] + qix[3][i];
                        qix[2][i] = qix[1][i] + qix[4][i];
                        const std::ptrdiff_t j = i + 1;
                        qix[c1][j] = qix[1][j] + qix[d][j];
                        qix[1][j] = 0.5f * (qix[0][j] - qix[3][j] + qix[2][j] - qix[4][j]);
                    }
                    if (cc < ccmax) {
                        const std::ptrdiff_t i = base + cc;
                        qix[0][i] = qix[1][i] + qix[3][i];
                        qix[2][i] = qix[1][i] + qix[4][i];
                    }
                } else {
                    c0 = 2 - c0;
                    const int d = c0 + 3 - ((c0 == 0) ? 0 : 1);
                    for (cc = ccmin; cc < ccmax - 1; cc += 2) {
                        const std::ptrdiff_t i = base + cc;
                        qix[c0][i] = qix[1][i] + qix[d][i];
                        qix[1][i] = 0.5f * (qix[0][i] - qix[3][i] + qix[2][i] - qix[4][i]);
                        const std::ptrdiff_t j = i + 1;
                        qix[0][j] = qix[1][j] + qix[3][j];
                        qix[2][j] = qix[1][j] + qix[4][j];
                    }
                    if (cc < ccmax) {
                        const std::ptrdiff_t i = base + cc;
                        qix[c0][i] = qix[1][i] + qix[d][i];
                        qix[1][i] = 0.5f * (qix[0][i] - qix[3][i] + qix[2][i] - qix[4][i]);
                    }
                }
            }
        }

        // we fill the non-approximated color channels from gamma corrected cfa data
        for (int rrr = 4; rrr < lastRr - 4; ++rrr) {
            for (int ccc = 4; ccc < lastCc - 4; ++ccc) {
                const int idx = rrr * kTile + ccc;
                qix[FC(rrr, ccc, filters)][idx] = qix[5][idx];
            }
        }

        for (int step = 0; step < refine; ++step) {
            // Reinforce interpolated green pixels on RED/BLUE pixel locations
            for (int rr = rrmin + 2; rr < rrmax - 2; ++rr) {
                for (int cc = ccmin + 2 + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters); cc < ccmax - 2; cc += 2) {
                    float* rgb1 = qix[1] + rr * kTile + cc;
                    const float* rgbc = qix[c] + rr * kTile + cc;

                    const float dL = 1.0f / (1.0f + std::fabs(rgbc[-2] - rgbc[0]) + std::fabs(rgb1[1] - rgb1[-1]));
                    const float dR = 1.0f / (1.0f + std::fabs(rgbc[2] - rgbc[0]) + std::fabs(rgb1[1] - rgb1[-1]));
                    const float dU = 1.0f / (1.0f + std::fabs(rgbc[-w2] - rgbc[0]) + std::fabs(rgb1[w1] - rgb1[-w1]));
                    const float dD = 1.0f / (1.0f + std::fabs(rgbc[w2] - rgbc[0]) + std::fabs(rgb1[w1] - rgb1[-w1]));
                    rgb1[0] = (rgbc[0] + ((rgb1[-1] - rgbc[-1]) * dL + (rgb1[1] - rgbc[1]) * dR + (rgb1[-w1] - rgbc[-w1]) * dU + (rgb1[w1] - rgbc[w1]) * dD) / (dL + dR + dU + dD));
                }
            }
            // Reinforce interpolated red/blue pixels on GREEN pixel locations
            for (int rr = rrmin + 2; rr < rrmax - 2; ++rr) {
                for (int cc = ccmin + 2 + (FC(rr, 3, filters) & 1), c = FC(rr, cc + 1, filters); cc < ccmax - 2; cc += 2) {
                    for (int i = 0; i < 2; c = 2 - c, ++i) {
                        const float* rgb1 = qix[1] + rr * kTile + cc;
                        float* rgbc = qix[c] + rr * kTile + cc;

                        const float dL = 1.0f / (1.0f + std::fabs(rgb1[-2] - rgb1[0]) + std::fabs(rgbc[1] - rgbc[-1]));
                        const float dR = 1.0f / (1.0f + std::fabs(rgb1[2] - rgb1[0]) + std::fabs(rgbc[1] - rgbc[-1]));
                        const float dU = 1.0f / (1.0f + std::fabs(rgb1[-w2] - rgb1[0]) + std::fabs(rgbc[w1] - rgbc[-w1]));
                        const float dD = 1.0f / (1.0f + std::fabs(rgb1[w2] - rgb1[0]) + std::fabs(rgbc[w1] - rgbc[-w1]));
                        rgbc[0] = (rgb1[0] - ((rgb1[-1] - rgbc[-1]) * dL + (rgb1[1] - rgbc[1]) * dR + (rgb1[-w1] - rgbc[-w1]) * dU + (rgb1[w1] - rgbc[w1]) * dD) / (dL + dR + dU + dD));
                    }
                }
            }
            // Reinforce integrated red/blue pixels on BLUE/RED pixel locations
            for (int rr = rrmin + 2; rr < rrmax - 2; ++rr) {
                for (int cc = ccmin + 2 + (FC(rr, 2, filters) & 1), c = 2 - FC(rr, cc, filters); cc < ccmax - 2; cc += 2) {
                    const int d = 2 - c;
                    const float* rgb1 = qix[1] + rr * kTile + cc;
                    float* rgbc = qix[c] + rr * kTile + cc;
                    const float* rgbd = qix[d] + rr * kTile + cc;

                    const float dL = 1.0f / (1.0f + std::fabs(rgbd[-2] - rgbd[0]) + std::fabs(rgb1[1] - rgb1[-1]));
                    const float dR = 1.0f / (1.0f + std::fabs(rgbd[2] - rgbd[0]) + std::fabs(rgb1[1] - rgb1[-1]));
                    const float dU = 1.0f / (1.0f + std::fabs(rgbd[-w2] - rgbd[0]) + std::fabs(rgb1[w1] - rgb1[-w1]));
                    const float dD = 1.0f / (1.0f + std::fabs(rgbd[w2] - rgbd[0]) + std::fabs(rgb1[w1] - rgb1[-w1]));
                    rgbc[0] = (rgb1[0] - ((rgb1[-1] - rgbc[-1]) * dL + (rgb1[1] - rgbc[1]) * dR + (rgb1[-w1] - rgbc[-w1]) * dU + (rgb1[w1] - rgbc[w1]) * dD) / (dL + dR + dU + dD));
                }
            }
        }

        // The outermost tiles also write the overlap on the image's side.
        const int firstVertical = rowStart + ((tileVertical == 0) ? kBorder : kOverlap);
        const int lastVertical = rowEnd - ((tileVertical == numVertical - 1) ? kBorder : kOverlap);
        const int firstHorizontal = colStart + ((tileHorizontal == 0) ? kBorder : kOverlap);
        const int lastHorizontal = colEnd - ((tileHorizontal == numHorizontal - 1) ? kBorder : kOverlap);
        for (int row = firstVertical, rr = row - rowStart + kBorder; row < lastVertical; ++row, ++rr) {
            float* dest = out + (std::size_t)4 * ((std::size_t)row * width + firstHorizontal);
            int idx = rr * kTile + firstHorizontal - colStart + kBorder;
            for (int col = firstHorizontal; col < lastHorizontal; ++col, dest += 4, ++idx) {
                dest[0] = std::fmax(0.0f, scaler * calcGamma(qix[0][idx], gammaOut));
                dest[1] = std::fmax(0.0f, scaler * calcGamma(qix[1][idx], gammaOut));
                dest[2] = std::fmax(0.0f, scaler * calcGamma(qix[2][idx], gammaOut));
                dest[3] = 0.0f;
            }
        }
    });
} // lmmse
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
