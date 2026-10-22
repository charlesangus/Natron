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
 * src/iop/demosaicing/xtrans.c: xtrans_markesteijn_interpolate(), Frank Markesteijn's X-Trans
 * algorithm as darktable adapted it from dcraw 9.20 by Dave Coffin. The C path only.
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
 */

#include "RawKernels.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    const int TS = 122;

    const int PAD_G1_G3 = 3;
    const int PAD_G_INTERP = 3;
    const int PAD_G_RECALC = 6;

    inline const short*
    hexmap(const int row,
           const int col,
           short (*const allhex)[3][8])
    {
        // row and col reach -14 (3-pass); 600 is a multiple of 3 keeping the modulo non-negative.
        const int irow = row + 600;
        const int icol = col + 600;

        return allhex[irow % 3][icol % 3];
    }

    inline float
    sqrf(const float a)
    {
        return a * a;
    }

    template <typename T>
    inline T
    clamps(T a,
           T l,
           T h)
    {
        return (a > l) ? ((a < h) ? a : h) : l;
    }

    inline int
    translate(int n,
              int size)
    {
        return (n >= size) ? (2 * size - n - 2) : std::abs(n);
    }
} // namespace

void
markesteijn(float* out,
            const float* in,
            int width,
            int height,
            const std::uint8_t (*xtrans)[6],
            int passes,
            const ParallelFor& parallel)
{
    static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                       patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                       { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } },
                       dir[4] = { 1, TS, TS + 1, TS - 1 };

    short allhex[3][3][8] = {};
    // The offset in the pattern of the solitary green photosites.
    unsigned short sgrow = 0, sgcol = 0;
    const unsigned ndir = 4 << (passes > 1);

    const std::size_t bufferFloats = (std::size_t)TS * TS * (ndir * 4 + 3);

    // Map a green hexagon around each non-green pixel and vice versa.
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            for (int ng = 0, d = 0; d < 10; d += 2) {
                const int g = fcXTrans(row, col, xtrans) == 1;
                if (fcXTrans(row + orth[d], col + orth[d + 2], xtrans) == 1) {
                    ng = 0;
                } else {
                    ng++;
                }
                // Four non-green neighbours in the cardinal directions make the solitary green.
                if (ng == 4) {
                    sgrow = row;
                    sgcol = col;
                }
                if (ng == g + 1) {
                    for (int c = 0; c < 8; c++) {
                        const int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
                        const int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];
                        allhex[row][col][c ^ (g * 2 & d)] = h + v * TS;
                    }
                }
            }
        }
    }

    // Extra passes propagate errors at the edges further, hence the larger padding.
    const int padTile = (passes == 1) ? 12 : 17;
    // The mirrored fill below reads padTile + 1 photosites in from each edge.
    if ((width < padTile + 2) || (height < padTile + 2)) {
        vngLinearInterpolate(out, in, width, height, kFiltersXTrans, xtrans, INT_MAX, parallel);

        return;
    }
    const int step = TS - (padTile * 2);
    const int nTops = (height - padTile + padTile + step - 1) / step;
    const int nLefts = (width - padTile + padTile + step - 1) / step;
    const int nTiles = ((height > 0) && (width > 0)) ? (nTops * nLefts) : 0;

    parallel(nTiles, [&](int tile) {
        const int top = -padTile + (tile / nLefts) * step;
        const int left = -padTile + (tile % nLefts) * step;
        if ((top >= height - padTile) || (left >= width - padTile)) {
            return;
        }
        // Scratch is cleared for every tile so that what a tile reads never depends on which
        // tile its thread ran before.
        std::unique_ptr<float[]> storage(new float[bufferFloats]());
        char* const buffer = reinterpret_cast<char*>(storage.get());
        float (*rgb)[TS][TS][3] = (float (*)[TS][TS][3])buffer;
        float (*const yuv)[TS][TS] = (float (*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
        float (*const drv)[TS][TS] = (float (*)[TS][TS])(buffer + TS * TS * (ndir * 3 + 3) * sizeof(float));
        float (*const gmin)[TS] = (float (*)[TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
        float (*const gmax)[TS] = (float (*)[TS])(buffer + TS * TS * (ndir * 3 + 1) * sizeof(float));
        std::uint8_t (*const homo)[TS][TS] = (std::uint8_t (*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
        std::uint8_t (*const homosum)[TS][TS] = (std::uint8_t (*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float)
                                                                           + TS * TS * ndir * sizeof(std::uint8_t));

        int mrow = std::min(top + TS, height + padTile);
        int mcol = std::min(left + TS, width + padTile);

        // Beyond the image edges the tile is filled with mirrored or interpolated photosites,
        // which avoids discontinuities there.
        for (int row = top; row < mrow; row++) {
            for (int col = left; col < mcol; col++) {
                float* const pix = rgb[0][row - top][col - left];
                if ((col >= 0) && (row >= 0) && (col < width) && (row < height)) {
                    const int f = fcXTrans(row, col, xtrans);
                    for (int c = 0; c < 3; c++) {
                        pix[c] = (c == f) ? (float)fmax(0.0f, in[(std::size_t)width * row + col]) : 0.f;
                    }
                } else {
                    const int c = fcXTrans(row, col, xtrans);
                    for (int cc = 0; cc < 3; cc++) {
                        if (cc != c) {
                            pix[cc] = 0.0f;
                        } else {
                            const int cy = translate(row, height), cx = translate(col, width);
                            if (c == fcXTrans(cy, cx, xtrans)) {
                                pix[c] = fmaxf(0.0f, in[(std::size_t)width * cy + cx]);
                            } else {
                                float sum = 0.0f;
                                std::uint8_t count = 0;
                                for (int y = row - 1; y <= row + 1; y++) {
                                    for (int x = col - 1; x <= col + 1; x++) {
                                        const int yy = translate(y, height), xx = translate(x, width);
                                        const int ff = fcXTrans(yy, xx, xtrans);
                                        if (ff == c) {
                                            sum += fmaxf(0.0f, in[(std::size_t)width * yy + xx]);
                                            count++;
                                        }
                                    }
                                }
                                pix[c] = sum / count;
                            }
                        }
                    }
                }
            }
        }

        for (int c = 1; c <= 3; c++) {
            std::memcpy(rgb[c], rgb[0], sizeof(*rgb));
        }

        // Each pair of red and blue photosites gets as green1 and green3 the minimum and maximum
        // of the greens around it; interpolating green reads them with a 3 pixel border.
        for (int row = top + PAD_G1_G3; row < mrow - PAD_G1_G3; row++) {
            // A max of 0 marks a new pair, whose neighbouring greens are yet to be read.
            float min = FLT_MAX, max = 0.0f;
            for (int col = left + PAD_G1_G3; col < mcol - PAD_G1_G3; col++) {
                if (fcXTrans(row, col, xtrans) == 1) {
                    min = FLT_MAX, max = 0.0f;
                    continue;
                }
                if (max == 0.0f) {
                    float (*const pix)[3] = &rgb[0][row - top][col - left];
                    const short* const hex = hexmap(row, col, allhex);
                    for (int c = 0; c < 6; c++) {
                        const float val = pix[hex[c]][1];
                        if (min > val) {
                            min = val;
                        }
                        if (max < val) {
                            max = val;
                        }
                    }
                }
                gmin[row - top][col - left] = min;
                gmax[row - top][col - left] = max;
                // Vertical pairs: hop down to the second photosite of the pair, then back up and
                // right to the next pair.
                switch ((row - sgrow) % 3) {
                case 1:
                    if (row < mrow - 4) {
                        row++, col--;
                    }
                    break;
                case 2:
                    min = FLT_MAX, max = 0.0f;
                    if (((col += 2) < mcol - 4) && (row > top + 3)) {
                        row--;
                    }
                }
            }
        }

        // Green horizontally, vertically and along both diagonals; 3 * hex[] reaches 3 pixels.
        for (int row = top + PAD_G_INTERP; row < mrow - PAD_G_INTERP; row++) {
            for (int col = left + PAD_G_INTERP; col < mcol - PAD_G_INTERP; col++) {
                float color[8];
                const int f = fcXTrans(row, col, xtrans);
                if (f == 1) {
                    continue;
                }
                float (*const pix)[3] = &rgb[0][row - top][col - left];
                const short* const hex = hexmap(row, col, allhex);
                // The constants are dcraw's integer maths as fractions.
                color[0] = 0.6796875f * (pix[hex[1]][1] + pix[hex[0]][1])
                    - 0.1796875f * (pix[2 * hex[1]][1] + pix[2 * hex[0]][1]);
                color[1] = 0.87109375f * pix[hex[3]][1] + pix[hex[2]][1] * 0.13f
                    + 0.359375f * (pix[0][f] - pix[-hex[2]][f]);
                for (int c = 0; c < 2; c++) {
                    color[2 + c] = 0.640625f * pix[hex[4 + c]][1] + 0.359375f * pix[-2 * hex[4 + c]][1]
                        + 0.12890625f * (2 * pix[0][f] - pix[3 * hex[4 + c]][f] - pix[-3 * hex[4 + c]][f]);
                }
                for (int c = 0; c < 4; c++) {
                    rgb[c ^ !((row - sgrow) % 3)][row - top][col - left][1]
                        = clamps(color[c], gmin[row - top][col - left], gmax[row - top][col - left]);
                }
            }
        }

        for (int pass = 0; pass < passes; pass++) {
            if (pass == 1) {
                // The second pass works on a copy of the first four directions.
                std::memcpy(rgb + 4, rgb, sizeof(*rgb) * 4);
                rgb += 4;
            }

            // Recalculate green from the interpolated values of closer pixels.
            if (pass) {
                for (int row = top + PAD_G_RECALC; row < mrow - PAD_G_RECALC; row++) {
                    for (int col = left + PAD_G_RECALC; col < mcol - PAD_G_RECALC; col++) {
                        const int f = fcXTrans(row, col, xtrans);
                        if (f == 1) {
                            continue;
                        }
                        const short* const hex = hexmap(row, col, allhex);
                        for (int d = 3; d < 6; d++) {
                            float (*rfx)[3] = &rgb[(d - 2) ^ !((row - sgrow) % 3)][row - top][col - left];
                            const float val = rfx[-2 * hex[d]][1]
                                + 2 * rfx[hex[d]][1] - rfx[-2 * hex[d]][f]
                                - 2 * rfx[hex[d]][f] + 3 * rfx[0][f];
                            rfx[0][1] = clamps(val / 3.0f, gmin[row - top][col - left], gmax[row - top][col - left]);
                        }
                    }
                }
            }

            // Red and blue for the solitary green photosites.
            const int padRbG = (passes == 1) ? 6 : 5;
            for (int row = (top - sgrow + padRbG + 2) / 3 * 3 + sgrow; row < mrow - padRbG; row += 3) {
                for (int col = (left - sgcol + padRbG + 2) / 3 * 3 + sgcol; col < mcol - padRbG; col += 3) {
                    float (*rfx)[3] = &rgb[0][row - top][col - left];
                    int h = fcXTrans(row, col + 1, xtrans);
                    float diff[6] = { 0.0f };
                    // Twice the interpolated colour: first index red or blue, second the pass.
                    float color[2][6];
                    // Six passes alternating horizontal and vertical (i), starting with red or
                    // blue (h), whichever is closest. Passes 0 and 1 go to rgb[0] and rgb[1],
                    // passes 3 and 5 to rgb[2] and rgb[3] with the better of the two directions.
                    for (int i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2) {
                        for (int c = 0; c < 2; c++, h ^= 2) {
                            const float g = 2 * rfx[0][1] - rfx[i << c][1] - rfx[-(i << c)][1];
                            color[h != 0][d] = g + rfx[i << c][h] + rfx[-(i << c)][h];
                            if (d > 1) {
                                diff[d] += sqrf(rfx[i << c][1] - rfx[-(i << c)][1] - rfx[i << c][h] + rfx[-(i << c)][h])
                                    + sqrf(g);
                            }
                        }
                        if ((d < 2) || (d & 1)) {
                            const int dOut = d - ((d > 1) && (diff[d - 1] < diff[d]));
                            rfx[0][0] = color[0][dOut] / 2.f;
                            rfx[0][2] = color[1][dOut] / 2.f;
                            rfx += TS * TS;
                        }
                    }
                }
            }

            // Red for blue photosites and vice versa.
            const int padRbBr = (passes == 1) ? 6 : 5;
            for (int row = top + padRbBr; row < mrow - padRbBr; row++) {
                for (int col = left + padRbBr; col < mcol - padRbBr; col++) {
                    const int f = 2 - fcXTrans(row, col, xtrans);
                    if (f == 1) {
                        continue;
                    }
                    float (*rfx)[3] = &rgb[0][row - top][col - left];
                    const int c = (row - sgrow) % 3 ? TS : 1;
                    const int h = 3 * (c ^ TS ^ 1);
                    for (int d = 0; d < 4; d++, rfx += TS * TS) {
                        const int i = ((d > 1) || ((d ^ c) & 1) || ((fabsf(rfx[0][1] - rfx[c][1]) + fabsf(rfx[0][1] - rfx[-c][1])) < 2.f * (fabsf(rfx[0][1] - rfx[h][1]) + fabsf(rfx[0][1] - rfx[-h][1])))) ? c : h;
                        rfx[0][f] = (rfx[i][f] + rfx[-i][f] + 2.f * rfx[0][1] - rfx[i][1] - rfx[-i][1]) / 2.f;
                    }
                }
            }

            // Red and blue for 2x2 blocks of green.
            const int padG22 = (passes == 1) ? 8 : 4;
            for (int row = top + padG22; row < mrow - padG22; row++) {
                if ((row - sgrow) % 3) {
                    for (int col = left + padG22; col < mcol - padG22; col++) {
                        if ((col - sgcol) % 3) {
                            float (*rfx)[3] = &rgb[0][row - top][col - left];
                            const short* const hex = hexmap(row, col, allhex);
                            for (unsigned d = 0; d < ndir; d += 2, rfx += TS * TS) {
                                if (hex[d] + hex[d + 1]) {
                                    const float g = 3.f * rfx[0][1] - 2.f * rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                                    for (int c = 0; c < 4; c += 2) {
                                        rfx[0][c] = (g + 2.f * rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 3.f;
                                    }
                                } else {
                                    const float g = 2.f * rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                                    for (int c = 0; c < 4; c += 2) {
                                        rfx[0][c] = (g + rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 2.f;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        rgb = (float (*)[TS][TS][3])buffer;
        mrow -= top;
        mcol -= left;

        // YPbPr (ITU-R BT.2020) stands in for dcraw's CIELab as the perceptual space: nearly
        // indistinguishable, for camera RGB that is roughly linear.
        for (unsigned d = 0; d < ndir; ++d) {
            const int padYuv = (passes == 1) ? 8 : 13;
            for (int row = padYuv; row < mrow - padYuv; row++) {
                for (int col = padYuv; col < mcol - padYuv; col++) {
                    const float* rx = rgb[d][row][col];
                    const float y = 0.2627f * rx[0] + 0.6780f * rx[1] + 0.0593f * rx[2];
                    yuv[0][row][col] = y;
                    yuv[1][row][col] = (rx[2] - y) * 0.56433f;
                    yuv[2][row][col] = (rx[0] - y) * 0.67815f;
                }
            }
            // f steps a column or a row (TS) either way inside the padded yuv tiles.
            const int f = dir[d & 3];
            const int padDrv = (passes == 1) ? 9 : 14;
            for (int row = padDrv; row < mrow - padDrv; row++) {
                for (int col = padDrv; col < mcol - padDrv; col++) {
                    const float* y0 = &yuv[0][row][col];
                    const float* y1 = &yuv[1][row][col];
                    const float* y2 = &yuv[2][row][col];
                    drv[d][row][col] = sqrf(2 * y0[0] - y0[f] - y0[-f])
                        + sqrf(2 * y1[0] - y1[f] - y1[-f])
                        + sqrf(2 * y2[0] - y2[f] - y2[-f]);
                }
            }
        }

        std::memset(homo, 0, sizeof(std::uint8_t) * ndir * TS * TS);
        const int padHomo = (passes == 1) ? 10 : 15;
        for (int row = padHomo; row < mrow - padHomo; row++) {
            for (int col = padHomo; col < mcol - padHomo; col++) {
                float tr = FLT_MAX;
                for (unsigned d = 0; d < ndir; ++d) {
                    if (tr > drv[d][row][col]) {
                        tr = drv[d][row][col];
                    }
                }
                tr *= 8;
                for (unsigned d = 0; d < ndir; ++d) {
                    for (int v = -1; v <= 1; v++) {
                        for (int h = -1; h <= 1; h++) {
                            homo[d][row][col] += ((drv[d][row + v][col + h] <= tr) ? 1 : 0);
                        }
                    }
                }
            }
        }

        // 5x5 sums of the homogeneity maps, by rolling column sums.
        for (unsigned d = 0; d < ndir; ++d) {
            for (int row = padTile; row < mrow - padTile; row++) {
                // Starting before the first column where homo[d][row][col + 2] can be non-zero,
                // the rolling sums start at zero.
                int col = padTile - 5;
                std::uint8_t v5sum[5] = { 0 };
                homosum[d][row][col] = 0;
                for (col++; col < mcol - padTile; col++) {
                    std::uint8_t colsum = 0;
                    for (int v = -2; v <= 2; v++) {
                        colsum += homo[d][row + v][col + 2];
                    }
                    homosum[d][row][col] = homosum[d][row][col - 1] - v5sum[col % 5] + colsum;
                    v5sum[col % 5] = colsum;
                }
            }
        }

        // The result is the average of the most homogeneous directions.
        for (int row = padTile; row < mrow - padTile; row++) {
            for (int col = padTile; col < mcol - padTile; col++) {
                std::uint8_t hm[8] = { 0 };
                std::uint8_t maxval = 0;
                for (unsigned d = 0; d < ndir; ++d) {
                    hm[d] = homosum[d][row][col];
                    maxval = (maxval < hm[d] ? hm[d] : maxval);
                }
                maxval -= maxval >> 3;
                for (unsigned d = 0; d < ndir - 4; ++d) {
                    if (hm[d] < hm[d + 4]) {
                        hm[d] = 0;
                    } else if (hm[d] > hm[d + 4]) {
                        hm[d + 4] = 0;
                    }
                }
                float avg[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (unsigned d = 0; d < ndir; ++d) {
                    if (hm[d] >= maxval) {
                        for (int c = 0; c < 3; c++) {
                            avg[c] += rgb[d][row][col][c];
                        }
                        avg[3]++;
                    }
                }
                for (int c = 0; c < 3; c++) {
                    out[4 * ((std::size_t)width * (row + top) + col + left) + c] = fmaxf(0.0f, avg[c] / avg[3]);
                }
            }
        }
    });

    vngLinearInterpolate(out, in, width, height, kFiltersXTrans, xtrans, padTile, parallel);
} // markesteijn
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
