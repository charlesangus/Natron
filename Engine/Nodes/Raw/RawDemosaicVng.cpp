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
 * src/iop/demosaicing/vng.c: vng_interpolate() and _vng_lininterpolate(), which darktable
 * adapted from dcraw 9.20 by Dave Coffin. The C path only.
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
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    const int kRowsPerChunk = 16;

    int
    chunkCount(int n,
               int perChunk)
    {
        return (n <= 0) ? 0 : ((n + perChunk - 1) / perChunk);
    }

    // _mm_max_ps(v, 0) as darktable's dt_vector_clipneg() uses it: a NaN becomes 0.
    inline void
    clipNeg4(float* v)
    {
        for (int c = 0; c < 4; ++c) {
            v[c] = (v[c] > 0.0f) ? v[c] : 0.0f;
        }
    }

    const int kTerms[]
        = { -2, -2, +0, -1, 1, 0x01, -2, -2, +0, +0, 2, 0x01, -2, -1, -1, +0, 1, 0x01, -2, -1, +0, -1, 1, 0x02,
            -2, -1, +0, +0, 1, 0x03, -2, -1, +0, +1, 2, 0x01, -2, +0, +0, -1, 1, 0x06, -2, +0, +0, +0, 2, 0x02,
            -2, +0, +0, +1, 1, 0x03, -2, +1, -1, +0, 1, 0x04, -2, +1, +0, -1, 2, 0x04, -2, +1, +0, +0, 1, 0x06,
            -2, +1, +0, +1, 1, 0x02, -2, +2, +0, +0, 2, 0x04, -2, +2, +0, +1, 1, 0x04, -1, -2, -1, +0, 1, 0x80,
            -1, -2, +0, -1, 1, 0x01, -1, -2, +1, -1, 1, 0x01, -1, -2, +1, +0, 2, 0x01, -1, -1, -1, +1, 1, 0x88,
            -1, -1, +1, -2, 1, 0x40, -1, -1, +1, -1, 1, 0x22, -1, -1, +1, +0, 1, 0x33, -1, -1, +1, +1, 2, 0x11,
            -1, +0, -1, +2, 1, 0x08, -1, +0, +0, -1, 1, 0x44, -1, +0, +0, +1, 1, 0x11, -1, +0, +1, -2, 2, 0x40,
            -1, +0, +1, -1, 1, 0x66, -1, +0, +1, +0, 2, 0x22, -1, +0, +1, +1, 1, 0x33, -1, +0, +1, +2, 2, 0x10,
            -1, +1, +1, -1, 2, 0x44, -1, +1, +1, +0, 1, 0x66, -1, +1, +1, +1, 1, 0x22, -1, +1, +1, +2, 1, 0x10,
            -1, +2, +0, +1, 1, 0x04, -1, +2, +1, +0, 2, 0x04, -1, +2, +1, +1, 1, 0x04, +0, -2, +0, +0, 2, 0x80,
            +0, -1, +0, +1, 2, 0x88, +0, -1, +1, -2, 1, 0x40, +0, -1, +1, +0, 1, 0x11, +0, -1, +2, -2, 1, 0x40,
            +0, -1, +2, -1, 1, 0x20, +0, -1, +2, +0, 1, 0x30, +0, -1, +2, +1, 2, 0x10, +0, +0, +0, +2, 2, 0x08,
            +0, +0, +2, -2, 2, 0x40, +0, +0, +2, -1, 1, 0x60, +0, +0, +2, +0, 2, 0x20, +0, +0, +2, +1, 1, 0x30,
            +0, +0, +2, +2, 2, 0x10, +0, +1, +1, +0, 1, 0x44, +0, +1, +1, +2, 1, 0x10, +0, +1, +2, -1, 2, 0x40,
            +0, +1, +2, +0, 1, 0x60, +0, +1, +2, +1, 1, 0x20, +0, +1, +2, +2, 1, 0x10, +1, -2, +1, +0, 1, 0x80,
            +1, -1, +1, +1, 1, 0x88, +1, +0, +1, +2, 1, 0x08, +1, +0, +2, -1, 1, 0x40, +1, +0, +2, +1, 1, 0x10 };

    const int kChood[] = { -1, -1, -1, 0, -1, +1, 0, +1, +1, +1, +1, 0, +1, -1, 0, -1 };
} // namespace

void
vngLinearInterpolate(float* out,
                     const float* in,
                     int width,
                     int height,
                     std::uint32_t filters,
                     const std::uint8_t (*xtrans)[6],
                     int border,
                     const ParallelFor& parallel)
{
    const bool isXTrans = filters == kFiltersXTrans;
    const int colors = isXTrans ? 3 : 4;

    parallel(chunkCount(height, kRowsPerChunk), [&](int chunk) {
        const int rowEnd = std::min(height, (chunk + 1) * kRowsPerChunk);
        for (int row = chunk * kRowsPerChunk; row < rowEnd; row++) {
            for (int col = 0; col < width; col++) {
                float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                std::uint8_t count[4] = { 0, 0, 0, 0 };
                if ((col == 1) && (row >= 1) && (row < height - 1)) {
                    col = width - 1;
                }
                for (int y = row - 1; y != row + 2; y++) {
                    for (int x = col - 1; x != col + 2; x++) {
                        if ((y >= 0) && (x >= 0) && (y < height) && (x < width)) {
                            const int f = fcol(y, x, filters, xtrans);
                            sum[f] += fmaxf(0.0f, in[(std::size_t)y * width + x]);
                            count[f]++;
                        }
                    }
                }
                const int f = fcol(row, col, filters, xtrans);
                for (int c = 0; c < colors; c++) {
                    if ((c != f) && (count[c] != 0)) {
                        out[4 * ((std::size_t)row * width + col) + c] = sum[c] / count[c];
                    } else {
                        out[4 * ((std::size_t)row * width + col) + c] = fmaxf(0.0f, in[(std::size_t)row * width + col]);
                    }
                }
            }
        }
    });

    // For each position of the pattern: the number of neighbours of other colours, then for each
    // its offset, weight and colour, then each missing colour with its total weight, then the
    // pixel's own colour.
    std::vector<int> lookupStorage((std::size_t)16 * 16 * 32, 0);
    int (*const lookup)[16][32] = reinterpret_cast<int (*)[16][32]>(lookupStorage.data());

    const int size = (filters == kFiltersXTrans) ? 6 : 16;
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            int* ip = &(lookup[row][col][1]);
            int sum[4] = { 0, 0, 0, 0 };
            const int f = fcol(row, col, filters, xtrans);
            for (int y = -1; y <= 1; y++) {
                for (int x = -1; x <= 1; x++) {
                    const int weight = 1 << ((y == 0) + (x == 0));
                    const int color = fcol(row + y, col + x, filters, xtrans);
                    if (color == f) {
                        continue;
                    }
                    *ip++ = width * y + x;
                    *ip++ = weight;
                    *ip++ = color;
                    sum[color] += weight;
                }
            }
            lookup[row][col][0] = (int)((ip - &(lookup[row][col][0])) / 3);
            for (int c = 0; c < colors; c++) {
                if (c != f) {
                    *ip++ = c;
                    *ip++ = sum[c];
                }
            }
            *ip = f;
        }
    }

    const int innerRows = height - 2;
    parallel(chunkCount(innerRows, kRowsPerChunk), [&](int chunk) {
        const int rowEnd = std::min(height - 1, 1 + (chunk + 1) * kRowsPerChunk);
        for (int row = 1 + chunk * kRowsPerChunk; row < rowEnd; row++) {
            float* buf = out + (std::size_t)4 * width * row + 4;
            const float* bufIn = in + (std::size_t)width * row + 1;
            for (int col = 1; col < width - 1; col++) {
                // darktable jumps backwards, forever, when the buffer is no wider than twice the
                // border, as Markesteijn's narrowest buffers are; such a row is interpolated whole.
                if ((col == border) && (row >= border) && (row < height - border) && (width - border > col)) {
                    col = width - border;
                    buf = out + (std::size_t)4 * width * row + 4 * col;
                    bufIn = in + (std::size_t)width * row + col;
                }
                if (col == width) {
                    break;
                }
                float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const int* ip = &(lookup[row % size][col % size][0]);
                for (int i = *ip++; i--; ip += 3) {
                    sum[ip[2]] += fmaxf(0.0f, bufIn[ip[0]]) * ip[1];
                }
                for (int i = colors; --i; ip += 2) {
                    buf[*ip] = sum[ip[0]] / ip[1];
                }
                buf[*ip] = *bufIn;
                clipNeg4(buf);
                buf += 4;
                bufIn++;
            }
        }
    });
} // vngLinearInterpolate

void
vng(float* out,
    const float* in,
    int width,
    int height,
    std::uint32_t filters,
    const std::uint8_t (*xtrans)[6],
    bool onlyLinear,
    const ParallelFor& parallel)
{
    const bool isXTrans = (filters == kFiltersXTrans);
    const int prow = isXTrans ? 6 : 8;
    const int pcol = isXTrans ? 6 : 2;
    const int colors = isXTrans ? 3 : 4;
    const bool mixGreens = !isXTrans;

    // VNG4 tells the two greens of a Bayer pattern apart: the second becomes colour 3.
    std::uint32_t filters4 = filters;
    if (isXTrans) {
        filters4 = filters;
    } else if ((filters & 3) == 1) {
        filters4 = filters | 0x03030303u;
    } else {
        filters4 = filters | 0x0c0c0c0cu;
    }

    vngLinearInterpolate(out, in, width, height, filters4, xtrans, 1000000, parallel);

    if (!onlyLinear && (width > 4) && (height > 4)) {
        std::vector<int> codeStorage((std::size_t)prow * pcol * 320, 0);
        int* code[16][16];
        int* ip = codeStorage.data();

        for (int row = 0; row < prow; row++) {
            for (int col = 0; col < pcol; col++) {
                code[row][col] = ip;
                const int* cp = kTerms;
                for (int t = 0; t < 64; t++) {
                    const int y1 = *cp++, x1 = *cp++;
                    const int y2 = *cp++, x2 = *cp++;
                    const int weight = *cp++;
                    const int grads = *cp++;
                    const int color = fcol(row + y1, col + x1, filters4, xtrans);
                    if (fcol(row + y2, col + x2, filters4, xtrans) != color) {
                        continue;
                    }
                    const int diag = ((fcol(row, col + 1, filters4, xtrans) == color) && (fcol(row + 1, col, filters4, xtrans) == color)) ? 2 : 1;
                    if ((std::abs(y1 - y2) == diag) && (std::abs(x1 - x2) == diag)) {
                        continue;
                    }
                    *ip++ = (y1 * width + x1) * 4 + color;
                    *ip++ = (y2 * width + x2) * 4 + color;
                    *ip++ = weight;
                    for (int g = 0; g < 8; g++) {
                        if (grads & (1 << g)) {
                            *ip++ = g;
                        }
                    }
                    *ip++ = -1;
                }
                *ip++ = INT_MAX;
                cp = kChood;
                for (int g = 0; g < 8; g++) {
                    const int y = *cp++, x = *cp++;
                    *ip++ = (y * width + x) * 4;
                    const int color = fcol(row, col, filters4, xtrans);
                    if ((fcol(row + y, col + x, filters4, xtrans) != color) && (fcol(row + y * 2, col + x * 2, filters4, xtrans) == color)) {
                        *ip++ = (y * width + x) * 8 + color;
                    } else {
                        *ip++ = 0;
                    }
                }
            }
        }

        // Every pixel reads the rows around it as the linear pass left them, which darktable
        // ensures by writing each row back two rows late through a ring of three. Here bands of
        // rows are computed in parallel and each is written back once the next band, the last
        // one to read it, is done; the values are the same.
        const int bandRows = 32;
        const int firstRow = 2;
        const int endRow = height - 2;
        const std::size_t bandFloats = (std::size_t)4 * width * bandRows;
        std::vector<float> bandStorage(2 * bandFloats, 0.0f);
        int pendingBand = -1;
        const auto writeBack = [&](int band) {
            const int r0 = firstRow + band * bandRows;
            const int r1 = std::min(endRow, r0 + bandRows);
            const float* src = bandStorage.data() + (band & 1) * bandFloats;
            for (int row = r0; row < r1; row++) {
                std::memcpy(out + 4 * ((std::size_t)row * width + 2), src + (std::size_t)4 * ((std::size_t)(row - r0) * width + 2), sizeof(float) * (std::size_t)(width - 4) * 4);
            }
        };
        for (int band = 0; firstRow + band * bandRows < endRow; band++) {
            const int r0 = firstRow + band * bandRows;
            const int r1 = std::min(endRow, r0 + bandRows);
            float* const bandBase = bandStorage.data() + (band & 1) * bandFloats;
            parallel(r1 - r0, [&](int bandRow) {
                const int row = r0 + bandRow;
                float (*const target)[4] = reinterpret_cast<float (*)[4]>(bandBase) + (std::size_t)bandRow * width;
                for (int col = 2; col < width - 2; col++) {
                    int g;
                    float gval[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                    float* pix = out + 4 * ((std::size_t)row * width + col);
                    const int* cip = code[row % prow][col % pcol];
                    while ((g = cip[0]) != INT_MAX) {
                        float diff = fabsf(pix[g] - pix[cip[1]]) * cip[2];
                        gval[cip[3]] += diff;
                        cip += 5;
                        if ((g = cip[-1]) == -1) {
                            continue;
                        }
                        gval[g] += diff;
                        while ((g = *cip++) != -1) {
                            gval[g] += diff;
                        }
                    }
                    cip++;
                    float gmin = gval[0], gmax = gval[0];
                    for (g = 1; g < 8; g++) {
                        if (gmin > gval[g]) {
                            gmin = gval[g];
                        }
                        if (gmax < gval[g]) {
                            gmax = gval[g];
                        }
                    }
                    if (gmax == 0) {
                        std::memcpy(target[col], pix, sizeof(*out) * 4);
                        continue;
                    }
                    const float thold = gmin + (gmax * 0.5f);
                    float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    const int color = fcol(row, col, filters4, xtrans);
                    int num = 0;
                    for (g = 0; g < 8; g++, cip += 2) {
                        if (gval[g] <= thold) {
                            for (int c = 0; c < colors; c++) {
                                if ((c == color) && cip[1]) {
                                    sum[c] += (pix[c] + pix[cip[1]]) * 0.5f;
                                } else {
                                    sum[c] += pix[cip[0] + c];
                                }
                            }
                            num++;
                        }
                    }
                    for (int c = 0; c < colors; c++) {
                        float tot = pix[color];
                        if (c != color) {
                            tot += (sum[c] - sum[color]) / num;
                        }
                        target[col][c] = tot;
                    }
                }
            });
            if (pendingBand >= 0) {
                writeBack(pendingBand);
            }
            pendingBand = band;
        }
        if (pendingBand >= 0) {
            writeBack(pendingBand);
        }
    }

    const std::size_t nPixels = (std::size_t)width * height;
    const int pixelChunk = 4096;
    parallel((int)((nPixels + pixelChunk - 1) / pixelChunk), [&](int chunk) {
        const std::size_t end = std::min(nPixels, (std::size_t)(chunk + 1) * pixelChunk);
        for (std::size_t p = (std::size_t)chunk * pixelChunk; p < end; p++) {
            const std::size_t i = 4 * p;
            if (mixGreens) {
                out[i + 1] = 0.5f * (out[i + 1] + out[i + 3]);
                out[i + 3] = 0.0f;
            }
            clipNeg4(&out[i]);
        }
    });
} // vng
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
