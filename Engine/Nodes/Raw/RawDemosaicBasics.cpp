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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7):
 * src/iop/demosaicing/ppg.c, src/iop/demosaicing/rcd.c, src/iop/demosaicing/basics.c and
 * src/iop/demosaicing/passthrough.c.
 *
 *    This file is part of darktable,
 *    Copyright (C) 2010-2026 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 * RATIO CORRECTED DEMOSAICING
 * Luis Sanz Rodriguez (luis.sanz.rodriguez(at)gmail(dot)com)
 * Release 2.3 @ 171125
 * Original code from https://github.com/LuisSR/RCD-Demosaicing
 * Licensed under the GNU GPL version 3
 * The tiling coding was done by Ingo Weyrich (heckflosse67@gmx.de) from rawtherapee.
 *
 * darktable is distributed under the GNU GPL version 3 or later, so this file is too.
 */

#include "RawKernels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {

    inline float
    sqrf(float a)
    {
        return a * a;
    }

    // a * b + (1 - a) * c, in darktable's evaluation order.
    inline float
    interpolatef(float a,
                 float b,
                 float c)
    {
        return a * (b - c) + c;
    }

    inline float
    clip01(float x)
    {
        return (x >= 0.f) ? ((x <= 1.f) ? x : 1.f) : 0.f;
    }

    void
    preMedian(float* out,
              const float* in,
              int width,
              int height,
              std::uint32_t filters,
              int numPasses,
              float threshold,
              const ParallelFor& parallel)
    {
        std::memcpy(out, in, sizeof(float) * (std::size_t)width * height);

        const int lim[5] = { 0, 1, 2, 1, 0 };
        for (int pass = 0; pass < numPasses; ++pass) {
            parallel(std::max(0, height - 6), [&](int r) {
                const int row = r + 3;
                float med[9];
                int col = 3;
                if ((fcBayer(row, col, filters) != 1) && (fcBayer(row, col, filters) != 3)) {
                    ++col;
                }
                float* pixo = out + (std::size_t)width * row + col;
                const float* pixi = in + (std::size_t)width * row + col;
                for (; col < width - 3; col += 2) {
                    int cnt = 0;
                    for (int k = 0, i = 0; i < 5; ++i) {
                        for (int j = -lim[i]; j <= lim[i]; j += 2) {
                            if (std::fabs(pixi[(std::ptrdiff_t)width * (i - 2) + j] - pixi[0]) < threshold) {
                                med[k++] = pixi[(std::ptrdiff_t)width * (i - 2) + j];
                                ++cnt;
                            } else {
                                med[k++] = 64.0f + pixi[(std::ptrdiff_t)width * (i - 2) + j];
                            }
                        }
                    }
                    for (int i = 0; i < 8; ++i) {
                        for (int ii = i + 1; ii < 9; ++ii) {
                            if (med[i] > med[ii]) {
                                std::swap(med[i], med[ii]);
                            }
                        }
                    }
                    pixo[0] = (cnt == 1) ? (med[4] - 64.0f) : med[(cnt - 1) / 2];
                    pixo += 2;
                    pixi += 2;
                }
            });
        }
    }

} // namespace

void
serialFor(int count,
          const std::function<void(int)>& body)
{
    for (int i = 0; i < count; ++i) {
        body(i);
    }
}

std::uint32_t
bayerFilters(const int colours[2][2])
{
    std::uint32_t filters = 0;

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 2; ++col) {
            const int shift = (((row << 1) & 14) + (col & 1)) << 1;
            filters |= ((std::uint32_t)colours[row & 1][col] & 3u) << shift;
        }
    }

    return filters;
}

void
ppg(float* out,
    const float* in,
    int width,
    int height,
    std::uint32_t filters,
    float thrs,
    int margin,
    const ParallelFor& parallel)
{
    parallel(height, [&](int j) {
        float sum[8];
        for (int i = 0; i < width; ++i) {
            // Without the width test, a buffer under 6 wide jumps back to i = 3 forever.
            if ((i == 3) && (j >= 3) && (j < height - 3) && (width - 3 > i)) {
                i = width - 3;
            }
            if (i >= width) {
                break;
            }
            std::memset(sum, 0, sizeof(float) * 8);
            for (int y = j - 1; y != j + 2; ++y) {
                for (int x = i - 1; x != i + 2; ++x) {
                    if ((y >= 0) && (x >= 0) && (y < height) && (x < width)) {
                        const int f = fcBayer(y, x, filters);
                        sum[f] += in[(std::size_t)y * width + x];
                        sum[f + 4]++;
                    }
                }
            }
            const int f = fcBayer(j, i, filters);
            for (int c = 0; c < 3; ++c) {
                if ((c != f) && (sum[c + 4] > 0.0f)) {
                    out[4 * ((std::size_t)j * width + i) + c] = std::fmax(0.0f, sum[c] / sum[c + 4]);
                } else {
                    out[4 * ((std::size_t)j * width + i) + c] = std::fmax(0.0f, in[(std::size_t)j * width + i]);
                }
            }
        }
    });

    const bool median = thrs > 0.0f;
    const float* input = in;
    std::vector<float> medIn;
    if (median) {
        medIn.resize((std::size_t)height * width);
        preMedian(medIn.data(), in, width, height, filters, 1, thrs, parallel);
        input = medIn.data();
    }

    const int border = margin + 3;
    parallel(std::max(0, height - 6), [&](int r) {
        const int j = r + 3;
        float* buf = out + (std::size_t)4 * width * j + 4 * 3;
        const float* bufIn = input + (std::size_t)width * j + 3;
        for (int i = 3; i < width - 3; ++i) {
            // darktable jumps backwards, forever, when the buffer is narrower than twice the
            // border; such a buffer is processed whole instead.
            if ((i == border) && (j >= border) && (j < height - border) && (width - border > i)) {
                i = width - border;
                buf = out + (std::size_t)4 * width * j + 4 * i;
                // darktable reads the unfiltered input past the margin; kept for parity.
                bufIn = in + (std::size_t)width * j + i;
            }
            if (i >= width - 3) {
                break;
            }

            const int c = fcBayer(j, i, filters);
            float color[4] = { 0.f, 0.f, 0.f, 0.f };
            const float pc = bufIn[0];
            if ((c == 0) || (c == 2)) {
                color[c] = pc;
                const std::ptrdiff_t w = width;
                const float pym = bufIn[-w * 1];
                const float pym2 = bufIn[-w * 2];
                const float pym3 = bufIn[-w * 3];
                const float pyM = bufIn[w * 1];
                const float pyM2 = bufIn[w * 2];
                const float pyM3 = bufIn[w * 3];
                const float pxm = bufIn[-1];
                const float pxm2 = bufIn[-2];
                const float pxm3 = bufIn[-3];
                const float pxM = bufIn[+1];
                const float pxM2 = bufIn[+2];
                const float pxM3 = bufIn[+3];

                const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
                const float diffx = (std::fabs(pxm2 - pc) + std::fabs(pxM2 - pc) + std::fabs(pxm - pxM)) * 3.0f
                    + (std::fabs(pxM3 - pxM) + std::fabs(pxm3 - pxm)) * 2.0f;
                const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
                const float diffy = (std::fabs(pym2 - pc) + std::fabs(pyM2 - pc) + std::fabs(pym - pyM)) * 3.0f
                    + (std::fabs(pyM3 - pyM) + std::fabs(pym3 - pym)) * 2.0f;
                if (diffx > diffy) {
                    const float m = std::fmin(pym, pyM);
                    const float M = std::fmax(pym, pyM);
                    color[1] = std::fmax(std::fmin(guessy * .25f, M), m);
                } else {
                    const float m = std::fmin(pxm, pxM);
                    const float M = std::fmax(pxm, pxM);
                    color[1] = std::fmax(std::fmin(guessx * .25f, M), m);
                }
            } else {
                color[1] = pc;
            }

            color[3] = 0.0f;

            for (int k = 0; k < 4; ++k) {
                buf[k] = std::fmax(0.0f, color[k]);
            }

            buf += 4;
            bufIn++;
        }
    });

    // A row only reads the channels its neighbours leave as they were, so rows may run in any
    // order.
    parallel(height, [&](int j) {
        float* buf = out + (std::size_t)4 * width * j;
        for (int i = 0; i < width; ++i) {
            if ((i == margin) && (j >= margin) && (j < height - margin) && (width - margin > i)) {
                i = width - margin;
                buf = out + (std::size_t)4 * ((std::size_t)width * j + i);
            }
            if (i >= width) {
                break;
            }
            float color[4] = { buf[0], buf[1], buf[2], buf[3] };

            if ((j > 0) && (i > 0) && (i < width - 1) && (j < height - 1)) {
                const int c = fcBayer(j, i, filters);
                const std::ptrdiff_t w4 = (std::ptrdiff_t)4 * width;
                if (c & 1) {
                    const float* nt = buf - w4;
                    const float* nb = buf + w4;
                    const float* nl = buf - 4;
                    const float* nr = buf + 4;
                    if (fcBayer(j, i + 1, filters) == 0) {
                        color[2] = (nt[2] + nb[2] + 2.0f * color[1] - nt[1] - nb[1]) * .5f;
                        color[0] = (nl[0] + nr[0] + 2.0f * color[1] - nl[1] - nr[1]) * .5f;
                    } else {
                        color[0] = (nt[0] + nb[0] + 2.0f * color[1] - nt[1] - nb[1]) * .5f;
                        color[2] = (nl[2] + nr[2] + 2.0f * color[1] - nl[1] - nr[1]) * .5f;
                    }
                } else {
                    const float* ntl = buf - 4 - w4;
                    const float* ntr = buf + 4 - w4;
                    const float* nbl = buf - 4 + w4;
                    const float* nbr = buf + 4 + w4;
                    if (c == 0) {
                        const float diff1 = std::fabs(ntl[2] - nbr[2]) + std::fabs(ntl[1] - color[1]) + std::fabs(nbr[1] - color[1]);
                        const float guess1 = ntl[2] + nbr[2] + 2.0f * color[1] - ntl[1] - nbr[1];
                        const float diff2 = std::fabs(ntr[2] - nbl[2]) + std::fabs(ntr[1] - color[1]) + std::fabs(nbl[1] - color[1]);
                        const float guess2 = ntr[2] + nbl[2] + 2.0f * color[1] - ntr[1] - nbl[1];
                        if (diff1 > diff2) {
                            color[2] = guess2 * .5f;
                        } else if (diff1 < diff2) {
                            color[2] = guess1 * .5f;
                        } else {
                            color[2] = (guess1 + guess2) * .25f;
                        }
                    } else {
                        const float diff1 = std::fabs(ntl[0] - nbr[0]) + std::fabs(ntl[1] - color[1]) + std::fabs(nbr[1] - color[1]);
                        const float guess1 = ntl[0] + nbr[0] + 2.0f * color[1] - ntl[1] - nbr[1];
                        const float diff2 = std::fabs(ntr[0] - nbl[0]) + std::fabs(ntr[1] - color[1]) + std::fabs(nbl[1] - color[1]);
                        const float guess2 = ntr[0] + nbl[0] + 2.0f * color[1] - ntr[1] - nbl[1];
                        if (diff1 > diff2) {
                            color[0] = guess2 * .5f;
                        } else if (diff1 < diff2) {
                            color[0] = guess1 * .5f;
                        } else {
                            color[0] = (guess1 + guess2) * .25f;
                        }
                    }
                }
            }
            for (int k = 0; k < 4; ++k) {
                buf[k] = std::fmax(0.0f, color[k]);
            }

            buf += 4;
        }
    });
} // ppg

namespace {
    const int kRcdTileSize = 112;
    // Data within this distance of a tile edge is not stable across tiles.
    const int kRcdBorder = 10;
    // The outermost tiles only need to discard what PPG fills.
    const int kRcdMargin = 9;
    const int kRcdTileValid = kRcdTileSize - 2 * kRcdBorder;
    const float kRcdEps = 1e-5f;
    const float kRcdEpsSq = 1e-10f;

    inline float
    rcdSafeIn(float a,
              float scale)
    {
        return std::fmax(0.0f, a) * scale;
    }
} // namespace

void
rcd(float* out,
    const float* in,
    int width,
    int height,
    std::uint32_t filters,
    float scaler,
    const ParallelFor& parallel)
{
    ppg(out, in, width, height, filters, 0.0f, kRcdBorder, parallel);
    if ((width < 2 * kRcdBorder) || (height < 2 * kRcdBorder)) {
        return;
    }

    const float revscaler = 1.0f / scaler;

    const int numVertical = 1 + (height - 2 * kRcdBorder - 1) / kRcdTileValid;
    const int numHorizontal = 1 + (width - 2 * kRcdBorder - 1) / kRcdTileValid;

    const int w1 = kRcdTileSize;
    const int w2 = 2 * kRcdTileSize;
    const int w3 = 3 * kRcdTileSize;
    const int w4 = 4 * kRcdTileSize;
    const std::size_t tileArea = (std::size_t)kRcdTileSize * kRcdTileSize;

    parallel(numVertical * numHorizontal, [&](int tile) {
        const int tileVertical = tile / numHorizontal;
        const int tileHorizontal = tile % numHorizontal;

        // Every scratch buffer starts zeroed for every tile, so that what a tile reads but never
        // sets does not depend on the tile a thread ran before it.
        std::vector<float> vhDirStore(tileArea, 0.f);
        std::vector<float> pqDirStore(tileArea / 2, 0.f);
        std::vector<float> cfaStore(tileArea, 0.f);
        std::vector<float> pHpfStore(tileArea / 2, 0.f);
        std::vector<float> qHpfStore(tileArea / 2, 0.f);
        std::vector<float> rgbStore(3 * tileArea, 0.f);
        float* const VH_Dir = vhDirStore.data();
        float* const PQ_Dir = pqDirStore.data();
        float* const cfa = cfaStore.data();
        float* const P_CDiff_Hpf = pHpfStore.data();
        float* const Q_CDiff_Hpf = qHpfStore.data();
        float* rgb[3] = { rgbStore.data(), rgbStore.data() + tileArea, rgbStore.data() + 2 * tileArea };
        float* const lpf = PQ_Dir;

        const int rowStart = tileVertical * kRcdTileValid;
        const int rowEnd = std::min(rowStart + kRcdTileSize, height);

        const int colStart = tileHorizontal * kRcdTileValid;
        const int colEnd = std::min(colStart + kRcdTileSize, width);

        const int tileRows = std::min(rowEnd - rowStart, kRcdTileSize);
        const int tileCols = std::min(colEnd - colStart, kRcdTileSize);

        for (int row = rowStart; row < rowEnd; ++row) {
            const int c0 = fcBayer(row, colStart, filters);
            const int c1 = fcBayer(row, colStart + 1, filters);
            std::size_t indx = (std::size_t)(row - rowStart) * kRcdTileSize;
            std::size_t inIndx = (std::size_t)row * width + colStart;
            for (int col = colStart; col < colEnd; ++col, ++indx, ++inIndx) {
                cfa[indx] = rgb[c0][indx] = rgb[c1][indx] = rcdSafeIn(in[inIndx], revscaler);
            }
        }

        float bufferV[3][kRcdTileSize - 8];
        for (int row = 3; row < std::min(tileRows - 3, 5); ++row) {
            for (int col = 4, indx = row * kRcdTileSize + col; col < tileCols - 4; ++col, ++indx) {
                bufferV[row - 3][col - 4] = sqrf((cfa[indx - w3] - cfa[indx - w1] - cfa[indx + w1] + cfa[indx + w3]) - 3.0f * (cfa[indx - w2] + cfa[indx + w2]) + 6.0f * cfa[indx]);
            }
        }

        float bufferH[kRcdTileSize];
        float* V0 = bufferV[0];
        float* V1 = bufferV[1];
        float* V2 = bufferV[2];
        for (int row = 4; row < tileRows - 4; ++row) {
            for (int col = 3, indx = row * kRcdTileSize + col; col < tileCols - 3; ++col, ++indx) {
                bufferH[col - 3] = sqrf((cfa[indx - 3] - cfa[indx - 1] - cfa[indx + 1] + cfa[indx + 3]) - 3.0f * (cfa[indx - 2] + cfa[indx + 2]) + 6.0f * cfa[indx]);
            }
            for (int col = 4, indx = (row + 1) * kRcdTileSize + col; col < tileCols - 4; ++col, ++indx) {
                V2[col - 4] = sqrf((cfa[indx - w3] - cfa[indx - w1] - cfa[indx + w1] + cfa[indx + w3]) - 3.0f * (cfa[indx - w2] + cfa[indx + w2]) + 6.0f * cfa[indx]);
            }
            for (int col = 4, indx = row * kRcdTileSize + col; col < tileCols - 4; ++col, ++indx) {
                const float V_Stat = std::fmax(kRcdEpsSq, V0[col - 4] + V1[col - 4] + V2[col - 4]);
                const float H_Stat = std::fmax(kRcdEpsSq, bufferH[col - 4] + bufferH[col - 3] + bufferH[col - 2]);
                VH_Dir[indx] = V_Stat / (V_Stat + H_Stat);
            }
            float* tmp = V0;
            V0 = V1;
            V1 = V2;
            V2 = tmp;
        }

        for (int row = 2; row < tileRows - 2; ++row) {
            for (int col = 2 + (fcBayer(row, 0, filters) & 1), indx = row * kRcdTileSize + col, lp_indx = indx / 2; col < tileCols - 2; col += 2, indx += 2, ++lp_indx) {
                lpf[lp_indx] = cfa[indx]
                    + 0.5f * (cfa[indx - w1] + cfa[indx + w1] + cfa[indx - 1] + cfa[indx + 1])
                    + 0.25f * (cfa[indx - w1 - 1] + cfa[indx - w1 + 1] + cfa[indx + w1 - 1] + cfa[indx + w1 + 1]);
            }
        }

        for (int row = 4; row < tileRows - 4; ++row) {
            for (int col = 4 + (fcBayer(row, 0, filters) & 1), indx = row * kRcdTileSize + col, lpindx = indx / 2; col < tileCols - 4; col += 2, indx += 2, ++lpindx) {
                const float cfai = cfa[indx];

                const float N_Grad = kRcdEps + std::fabs(cfa[indx - w1] - cfa[indx + w1]) + std::fabs(cfai - cfa[indx - w2]) + std::fabs(cfa[indx - w1] - cfa[indx - w3]) + std::fabs(cfa[indx - w2] - cfa[indx - w4]);
                const float S_Grad = kRcdEps + std::fabs(cfa[indx + w1] - cfa[indx - w1]) + std::fabs(cfai - cfa[indx + w2]) + std::fabs(cfa[indx + w1] - cfa[indx + w3]) + std::fabs(cfa[indx + w2] - cfa[indx + w4]);
                const float W_Grad = kRcdEps + std::fabs(cfa[indx - 1] - cfa[indx + 1]) + std::fabs(cfai - cfa[indx - 2]) + std::fabs(cfa[indx - 1] - cfa[indx - 3]) + std::fabs(cfa[indx - 2] - cfa[indx - 4]);
                const float E_Grad = kRcdEps + std::fabs(cfa[indx + 1] - cfa[indx - 1]) + std::fabs(cfai - cfa[indx + 2]) + std::fabs(cfa[indx + 1] - cfa[indx + 3]) + std::fabs(cfa[indx + 2] - cfa[indx + 4]);

                const float lpfi = lpf[lpindx];
                const float N_Est = cfa[indx - w1] * (lpfi + lpfi) / (kRcdEps + lpfi + lpf[lpindx - w1]);
                const float S_Est = cfa[indx + w1] * (lpfi + lpfi) / (kRcdEps + lpfi + lpf[lpindx + w1]);
                const float W_Est = cfa[indx - 1] * (lpfi + lpfi) / (kRcdEps + lpfi + lpf[lpindx - 1]);
                const float E_Est = cfa[indx + 1] * (lpfi + lpfi) / (kRcdEps + lpfi + lpf[lpindx + 1]);

                const float V_Est = (S_Grad * N_Est + N_Grad * S_Est) / (N_Grad + S_Grad);
                const float H_Est = (W_Grad * E_Est + E_Grad * W_Est) / (E_Grad + W_Grad);

                const float VH_Central_Value = VH_Dir[indx];
                const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] + VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
                const float VH_Disc = (std::fabs(0.5f - VH_Central_Value) < std::fabs(0.5f - VH_Neighbourhood_Value)) ? VH_Neighbourhood_Value : VH_Central_Value;

                rgb[1][indx] = interpolatef(clip01(VH_Disc), H_Est, V_Est);
            }
        }

        for (int row = 3; row < tileRows - 3; ++row) {
            for (int col = 3, indx = row * kRcdTileSize + col, indx2 = indx / 2; col < tileCols - 3; col += 2, indx += 2, ++indx2) {
                P_CDiff_Hpf[indx2] = sqrf((cfa[indx - w3 - 3] - cfa[indx - w1 - 1] - cfa[indx + w1 + 1] + cfa[indx + w3 + 3]) - 3.0f * (cfa[indx - w2 - 2] + cfa[indx + w2 + 2]) + 6.0f * cfa[indx]);
                Q_CDiff_Hpf[indx2] = sqrf((cfa[indx - w3 + 3] - cfa[indx - w1 + 1] - cfa[indx + w1 - 1] + cfa[indx + w3 - 3]) - 3.0f * (cfa[indx - w2 + 2] + cfa[indx + w2 - 2]) + 6.0f * cfa[indx]);
            }
        }
        for (int row = 4; row < tileRows - 4; ++row) {
            for (int col = 4 + (fcBayer(row, 0, filters) & 1), indx = row * kRcdTileSize + col, indx2 = indx / 2, indx3 = (indx - w1 - 1) / 2, indx4 = (indx + w1 - 1) / 2; col < tileCols - 4; col += 2, indx += 2, ++indx2, ++indx3, ++indx4) {
                const float P_Stat = std::fmax(kRcdEpsSq, P_CDiff_Hpf[indx3] + P_CDiff_Hpf[indx2] + P_CDiff_Hpf[indx4 + 1]);
                const float Q_Stat = std::fmax(kRcdEpsSq, Q_CDiff_Hpf[indx3 + 1] + Q_CDiff_Hpf[indx2] + Q_CDiff_Hpf[indx4]);
                PQ_Dir[indx2] = P_Stat / (P_Stat + Q_Stat);
            }
        }

        for (int row = 4; row < tileRows - 4; ++row) {
            for (int col = 4 + (fcBayer(row, 0, filters) & 1), indx = row * kRcdTileSize + col, c = 2 - fcBayer(row, col, filters), pqindx = indx / 2, pqindx2 = (indx - w1 - 1) / 2, pqindx3 = (indx + w1 - 1) / 2; col < tileCols - 4; col += 2, indx += 2, ++pqindx, ++pqindx2, ++pqindx3) {
                const float PQ_Central_Value = PQ_Dir[pqindx];
                const float PQ_Neighbourhood_Value = 0.25f * (PQ_Dir[pqindx2] + PQ_Dir[pqindx2 + 1] + PQ_Dir[pqindx3] + PQ_Dir[pqindx3 + 1]);

                const float PQ_Disc = (std::fabs(0.5f - PQ_Central_Value) < std::fabs(0.5f - PQ_Neighbourhood_Value)) ? PQ_Neighbourhood_Value : PQ_Central_Value;

                const float NW_Grad = kRcdEps + std::fabs(rgb[c][indx - w1 - 1] - rgb[c][indx + w1 + 1]) + std::fabs(rgb[c][indx - w1 - 1] - rgb[c][indx - w3 - 3]) + std::fabs(rgb[1][indx] - rgb[1][indx - w2 - 2]);
                const float NE_Grad = kRcdEps + std::fabs(rgb[c][indx - w1 + 1] - rgb[c][indx + w1 - 1]) + std::fabs(rgb[c][indx - w1 + 1] - rgb[c][indx - w3 + 3]) + std::fabs(rgb[1][indx] - rgb[1][indx - w2 + 2]);
                const float SW_Grad = kRcdEps + std::fabs(rgb[c][indx - w1 + 1] - rgb[c][indx + w1 - 1]) + std::fabs(rgb[c][indx + w1 - 1] - rgb[c][indx + w3 - 3]) + std::fabs(rgb[1][indx] - rgb[1][indx + w2 - 2]);
                const float SE_Grad = kRcdEps + std::fabs(rgb[c][indx - w1 - 1] - rgb[c][indx + w1 + 1]) + std::fabs(rgb[c][indx + w1 + 1] - rgb[c][indx + w3 + 3]) + std::fabs(rgb[1][indx] - rgb[1][indx + w2 + 2]);

                const float NW_Est = rgb[c][indx - w1 - 1] - rgb[1][indx - w1 - 1];
                const float NE_Est = rgb[c][indx - w1 + 1] - rgb[1][indx - w1 + 1];
                const float SW_Est = rgb[c][indx + w1 - 1] - rgb[1][indx + w1 - 1];
                const float SE_Est = rgb[c][indx + w1 + 1] - rgb[1][indx + w1 + 1];

                const float P_Est = (NW_Grad * SE_Est + SE_Grad * NW_Est) / (NW_Grad + SE_Grad);
                const float Q_Est = (NE_Grad * SW_Est + SW_Grad * NE_Est) / (NE_Grad + SW_Grad);

                rgb[c][indx] = rgb[1][indx] + interpolatef(clip01(PQ_Disc), Q_Est, P_Est);
            }
        }

        for (int row = 4; row < tileRows - 4; ++row) {
            for (int col = 4 + (fcBayer(row, 1, filters) & 1), indx = row * kRcdTileSize + col; col < tileCols - 4; col += 2, indx += 2) {
                const float VH_Central_Value = VH_Dir[indx];
                const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] + VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
                const float VH_Disc = (std::fabs(0.5f - VH_Central_Value) < std::fabs(0.5f - VH_Neighbourhood_Value)) ? VH_Neighbourhood_Value : VH_Central_Value;
                const float rgb1 = rgb[1][indx];
                const float N1 = kRcdEps + std::fabs(rgb1 - rgb[1][indx - w2]);
                const float S1 = kRcdEps + std::fabs(rgb1 - rgb[1][indx + w2]);
                const float W1 = kRcdEps + std::fabs(rgb1 - rgb[1][indx - 2]);
                const float E1 = kRcdEps + std::fabs(rgb1 - rgb[1][indx + 2]);

                const float rgb1mw1 = rgb[1][indx - w1];
                const float rgb1pw1 = rgb[1][indx + w1];
                const float rgb1m1 = rgb[1][indx - 1];
                const float rgb1p1 = rgb[1][indx + 1];

                for (int c = 0; c <= 2; c += 2) {
                    const float SNabs = std::fabs(rgb[c][indx - w1] - rgb[c][indx + w1]);
                    const float EWabs = std::fabs(rgb[c][indx - 1] - rgb[c][indx + 1]);

                    const float N_Grad = N1 + SNabs + std::fabs(rgb[c][indx - w1] - rgb[c][indx - w3]);
                    const float S_Grad = S1 + SNabs + std::fabs(rgb[c][indx + w1] - rgb[c][indx + w3]);
                    const float W_Grad = W1 + EWabs + std::fabs(rgb[c][indx - 1] - rgb[c][indx - 3]);
                    const float E_Grad = E1 + EWabs + std::fabs(rgb[c][indx + 1] - rgb[c][indx + 3]);

                    const float N_Est = rgb[c][indx - w1] - rgb1mw1;
                    const float S_Est = rgb[c][indx + w1] - rgb1pw1;
                    const float W_Est = rgb[c][indx - 1] - rgb1m1;
                    const float E_Est = rgb[c][indx + 1] - rgb1p1;

                    const float V_Est = (N_Grad * S_Est + S_Grad * N_Est) / (N_Grad + S_Grad);
                    const float H_Est = (E_Grad * W_Est + W_Grad * E_Est) / (E_Grad + W_Grad);

                    rgb[c][indx] = rgb1 + interpolatef(clip01(VH_Disc), H_Est, V_Est);
                }
            }
        }

        const int firstVertical = rowStart + ((tileVertical == 0) ? kRcdMargin : kRcdBorder);
        const int lastVertical = rowEnd - ((tileVertical == numVertical - 1) ? kRcdMargin : kRcdBorder);
        const int firstHorizontal = colStart + ((tileHorizontal == 0) ? kRcdMargin : kRcdBorder);
        const int lastHorizontal = colEnd - ((tileHorizontal == numHorizontal - 1) ? kRcdMargin : kRcdBorder);
        for (int row = firstVertical; row < lastVertical; ++row) {
            std::size_t idx = (std::size_t)(row - rowStart) * kRcdTileSize + (firstHorizontal - colStart);
            std::size_t oIdx = ((std::size_t)row * width + firstHorizontal) * 4;
            for (int col = firstHorizontal; col < lastHorizontal; ++col, oIdx += 4, ++idx) {
                out[oIdx] = scaler * std::fmax(0.0f, rgb[0][idx]);
                out[oIdx + 1] = scaler * std::fmax(0.0f, rgb[1][idx]);
                out[oIdx + 2] = scaler * std::fmax(0.0f, rgb[2][idx]);
                out[oIdx + 3] = 0.0f;
            }
        }
    });
} // rcd

void
passthroughMonochrome(float* out,
                      const float* in,
                      int width,
                      int height,
                      const ParallelFor& parallel)
{
    parallel(height, [&](int j) {
        for (int i = 0; i < width; ++i) {
            const std::size_t p = (std::size_t)j * width + i;
            for (int c = 0; c < 3; ++c) {
                out[4 * p + c] = in[p];
            }
            out[4 * p + 3] = 0.0f;
        }
    });
}

namespace {
    inline void
    swapMed(float* med,
            int i,
            int j)
    {
        if (med[i] > med[j]) {
            std::swap(med[i], med[j]);
        }
    }
} // namespace

void
colorSmoothing(float* out,
               int width,
               int height,
               int passes,
               const ParallelFor& parallel)
{
    const std::ptrdiff_t width4 = (std::ptrdiff_t)4 * width;

    for (int pass = 0; pass < passes; ++pass) {
        for (int c = 0; c < 3; c += 2) {
            parallel(height, [&](int j) {
                float* outp = out + (std::size_t)4 * j * width;
                for (int i = 0; i < width; ++i, outp += 4) {
                    outp[3] = outp[c];
                }
            });
            parallel(std::max(0, height - 2), [&](int r) {
                const int j = r + 1;
                float* outp = out + (std::size_t)4 * j * width + 4;
                for (int i = 1; i < width - 1; ++i, outp += 4) {
                    float med[9] = { outp[-width4 - 4 + 3] - outp[-width4 - 4 + 1],
                                     outp[-width4 + 0 + 3] - outp[-width4 + 0 + 1],
                                     outp[-width4 + 4 + 3] - outp[-width4 + 4 + 1],
                                     outp[-4 + 3] - outp[-4 + 1],
                                     outp[+0 + 3] - outp[+0 + 1],
                                     outp[+4 + 3] - outp[+4 + 1],
                                     outp[+width4 - 4 + 3] - outp[+width4 - 4 + 1],
                                     outp[+width4 + 0 + 3] - outp[+width4 + 0 + 1],
                                     outp[+width4 + 4 + 3] - outp[+width4 + 4 + 1] };
                    // The optimal 9-element median network.
                    swapMed(med, 1, 2);
                    swapMed(med, 4, 5);
                    swapMed(med, 7, 8);
                    swapMed(med, 0, 1);
                    swapMed(med, 3, 4);
                    swapMed(med, 6, 7);
                    swapMed(med, 1, 2);
                    swapMed(med, 4, 5);
                    swapMed(med, 7, 8);
                    swapMed(med, 0, 3);
                    swapMed(med, 5, 8);
                    swapMed(med, 4, 7);
                    swapMed(med, 3, 6);
                    swapMed(med, 1, 4);
                    swapMed(med, 2, 5);
                    swapMed(med, 4, 7);
                    swapMed(med, 4, 2);
                    swapMed(med, 6, 4);
                    swapMed(med, 4, 2);
                    outp[c] = std::fmax(0.0f, med[4] + outp[1]);
                }
            });
        }
    }
}

void
greenEquilibrationLocal(float* out,
                        const float* in,
                        int width,
                        int height,
                        std::uint32_t filters,
                        float threshold,
                        const ParallelFor& parallel)
{
    const float maximum = 1.0f;

    int oj = 2, oi = 2;
    if (fcBayer(oj, oi, filters) != 1) {
        oj++;
    }
    if (fcBayer(oj, oi, filters) != 1) {
        oi++;
    }
    if (fcBayer(oj, oi, filters) != 1) {
        oj--;
    }

    std::memcpy(out, in, sizeof(float) * (std::size_t)width * height);

    const int nRows = (height - 2 > oj) ? ((height - 2 - oj + 1) / 2) : 0;
    parallel(nRows, [&](int r) {
        const std::size_t j = (std::size_t)oj + 2 * (std::size_t)r;
        const std::size_t w = (std::size_t)width;
        for (std::size_t i = oi; (int)i < width - 2; i += 2) {
            const float o1_1 = in[(j - 1) * w + i - 1];
            const float o1_2 = in[(j - 1) * w + i + 1];
            const float o1_3 = in[(j + 1) * w + i - 1];
            const float o1_4 = in[(j + 1) * w + i + 1];
            const float o2_1 = in[(j - 2) * w + i];
            const float o2_2 = in[(j + 2) * w + i];
            const float o2_3 = in[j * w + i - 2];
            const float o2_4 = in[j * w + i + 2];

            const float m1 = (o1_1 + o1_2 + o1_3 + o1_4) / 4.0f;
            const float m2 = (o2_1 + o2_2 + o2_3 + o2_4) / 4.0f;

            // m1 / m2 large means m2 tiny, which would make a hot pixel.
            if ((m2 > 0.0f) && (m1 > 0.0f) && (m1 / m2 < maximum * 2.0f)) {
                const float c1 = (std::fabs(o1_1 - o1_2) + std::fabs(o1_1 - o1_3) + std::fabs(o1_1 - o1_4) + std::fabs(o1_2 - o1_3)
                                  + std::fabs(o1_3 - o1_4) + std::fabs(o1_2 - o1_4))
                    / 6.0f;
                const float c2 = (std::fabs(o2_1 - o2_2) + std::fabs(o2_1 - o2_3) + std::fabs(o2_1 - o2_4) + std::fabs(o2_2 - o2_3)
                                  + std::fabs(o2_3 - o2_4) + std::fabs(o2_2 - o2_4))
                    / 6.0f;
                if ((in[j * w + i] < maximum * 0.95f) && (c1 < maximum * threshold) && (c2 < maximum * threshold)) {
                    out[j * w + i] = std::fmax(0.0f, in[j * w + i] * m1 / m2);
                }
            }
        }
    });
}

double
greenEquilibrationFullRatio(const float* in,
                            int width,
                            int height,
                            std::uint32_t filters,
                            const ParallelFor& parallel)
{
    const int oj = 0;
    int oi = 0;

    if ((fcBayer(oj, oi, filters) & 1) != 1) {
        oi++;
    }
    const int g2Offset = oi ? -1 : 1;

    const int nRows = (height - 1 > oj) ? ((height - 1 - oj + 1) / 2) : 0;
    std::vector<double> sums1((std::size_t)std::max(0, nRows), 0.);
    std::vector<double> sums2((std::size_t)std::max(0, nRows), 0.);
    parallel(nRows, [&](int r) {
        const std::size_t j = (std::size_t)oj + 2 * (std::size_t)r;
        double s1 = 0.;
        double s2 = 0.;
        for (std::size_t i = oi; (std::ptrdiff_t)i < (std::ptrdiff_t)width - 1 - g2Offset; i += 2) {
            s1 += in[j * width + i];
            s2 += in[(j + 1) * width + i + g2Offset];
        }
        sums1[r] = s1;
        sums2[r] = s2;
    });
    double sum1 = 0.;
    double sum2 = 0.;
    for (int r = 0; r < nRows; ++r) {
        sum1 += sums1[r];
        sum2 += sums2[r];
    }

    if ((sum1 > 0.0) && (sum2 > 0.0)) {
        return sum2 / sum1;
    }

    return 0.;
}

void
greenEquilibrationFullApply(float* buffer,
                            int width,
                            int height,
                            int originRow,
                            int originCol,
                            int imageWidth,
                            int imageHeight,
                            std::uint32_t imageFilters,
                            double ratio,
                            const ParallelFor& parallel)
{
    if (ratio <= 0.) {
        return;
    }
    int oi = 0;
    if ((fcBayer(0, 0, imageFilters) & 1) != 1) {
        oi++;
    }
    const int g2Offset = oi ? -1 : 1;

    parallel(height, [&](int r) {
        const int j = originRow + r;
        if (((j & 1) != 0) || (j >= imageHeight - 1)) {
            return;
        }
        for (int c = 0; c < width; ++c) {
            const int i = originCol + c;
            if ((i < oi) || (((i - oi) & 1) != 0) || (i >= imageWidth - 1 - g2Offset)) {
                continue;
            }
            float* p = buffer + (std::size_t)r * width + c;
            *p = std::fmax(0.0f, (float)(*p * ratio));
        }
    });
}
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
