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
 * src/iop/cacorrect.c and the C path of dt_gaussian_blur() in src/common/gaussian.c, C paths
 * only. darktable is distributed under the GNU GPL, version 3 or (at your option) any later
 * version; this file is therefore GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2010-2026 darktable developers.
 *     Copyright (C) 2012-2026 darktable developers (src/common/gaussian.c).
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
 * Chromatic Aberration correction on raw bayer cfa data
 *
 *     copyright (c) 2008-2010  Emil Martinec <ejmartin@uchicago.edu>
 *     copyright (c) for improvements (speedups, iterated correction and avoid colour shift)
 *     2018 Ingo Weyrich <heckflosse67@gmx.de>
 *
 *     code dated: September 8, 2018
 *
 *     CA_correct_RT.cc is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * The linear equation solver is by Henry Guennadi Levkin.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawKernels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    const int ts = 128;
    const int tsh = ts / 2;
    const int v1 = ts;
    const int v2 = 2 * ts;
    const int v3 = 3 * ts;
    const int v4 = 4 * ts;
    const int border = 8;
    const int border2 = 2 * border;
    const int borderh = border / 2;
    const float caautostrength = 4.0f;
    const float eps = 1e-5f;
    const float eps2 = 1e-10f;

    inline float
    sqrf(float a)
    {
        return a * a;
    }

    inline float
    interpolatef(float a,
                 float b,
                 float c)
    {
        return a * (b - c) + c;
    }

    inline float
    clampf(float a,
           float mn,
           float mx)
    {
        return (a >= mn) ? ((a <= mx) ? a : mx) : mn;
    }

    inline int
    FC(std::size_t row,
       std::size_t col,
       std::uint32_t filters)
    {
        return fcBayer(row, col, filters);
    }

    float
    median9f(const float* p)
    {
        float p1 = std::min(p[1], p[2]);
        float p2 = std::max(p[1], p[2]);
        float p4 = std::min(p[4], p[5]);
        float p5 = std::max(p[4], p[5]);
        float p7 = std::min(p[7], p[8]);
        float p8 = std::max(p[7], p[8]);
        const float p0 = std::min(p[0], p1);
        const float p1a = std::max(p[0], p1);
        float p3 = std::min(p[3], p4);
        float p4a = std::max(p[3], p4);
        float p6 = std::min(p[6], p7);
        float p7a = std::max(p[6], p7);
        p1 = std::min(p1a, p2);
        p2 = std::max(p1a, p2);
        p4 = std::min(p4a, p5);
        p5 = std::max(p4a, p5);
        p7 = std::min(p7a, p8);
        p8 = std::max(p7a, p8);
        p3 = std::max(p0, p3);
        p5 = std::min(p5, p8);
        p7a = std::max(p4, p7);
        p4 = std::min(p4, p7);
        p6 = std::max(p3, p6);
        p4 = std::max(p1, p4);
        p2 = std::min(p2, p5);
        p4a = std::min(p4, p7a);
        p4 = std::min(p4a, p2);
        p2 = std::max(p4a, p2);
        p4 = std::max(p6, p4);

        return std::min(p2, p4);
    }

    bool
    linEqSolve(int nDim,
               double* pfMatr,
               double* pfVect,
               double* pfSolution)
    {
        double fMaxElem;
        double fAcc;
        int i, j, k, m;

        for (k = 0; k < (nDim - 1); k++) {
            fMaxElem = std::fabs(pfMatr[k * nDim + k]);
            m = k;
            for (i = k + 1; i < nDim; i++) {
                if (fMaxElem < std::fabs(pfMatr[i * nDim + k])) {
                    // The pivot keeps its sign here, as in darktable.
                    fMaxElem = pfMatr[i * nDim + k];
                    m = i;
                }
            }
            if (m != k) {
                for (i = k; i < nDim; i++) {
                    fAcc = pfMatr[k * nDim + i];
                    pfMatr[k * nDim + i] = pfMatr[m * nDim + i];
                    pfMatr[m * nDim + i] = fAcc;
                }
                fAcc = pfVect[k];
                pfVect[k] = pfVect[m];
                pfVect[m] = fAcc;
            }
            if (pfMatr[k * nDim + k] == 0.) {
                return false;
            }
            for (j = (k + 1); j < nDim; j++) {
                fAcc = -pfMatr[j * nDim + k] / pfMatr[k * nDim + k];
                for (i = k; i < nDim; i++) {
                    pfMatr[j * nDim + i] = pfMatr[j * nDim + i] + fAcc * pfMatr[k * nDim + i];
                }
                pfVect[j] = pfVect[j] + fAcc * pfVect[k];
            }
        }
        for (k = (nDim - 1); k >= 0; k--) {
            pfSolution[k] = pfVect[k];
            for (i = (k + 1); i < nDim; i++) {
                pfSolution[k] -= (pfMatr[k * nDim + i] * pfSolution[i]);
            }
            pfSolution[k] = pfSolution[k] / pfMatr[k * nDim + k];
        }

        return true;
    }

    struct Tile {
        int top;
        int left;
        int vblock;
        int hblock;
        int rr1;
        int cc1;
        int rrmin;
        int rrmax;
        int ccmin;
        int ccmax;
    };

    Tile
    makeTile(int top,
             int left,
             int width,
             int height)
    {
        Tile t;

        t.top = top;
        t.left = left;
        t.vblock = ((top + border) / (ts - border2)) + 1;
        t.hblock = ((left + border) / (ts - border2)) + 1;
        const int bottom = std::min(top + ts, height + border);
        const int right = std::min(left + ts, width + border);
        t.rr1 = bottom - top;
        t.cc1 = right - left;
        t.rrmin = (top < 0) ? border : 0;
        t.rrmax = (bottom > height) ? (height - top) : t.rr1;
        t.ccmin = (left < 0) ? border : 0;
        t.ccmax = (right > width) ? (width - left) : t.cc1;

        return t;
    }

    // rgb[] of one tile from the mosaic, the image's borders mirrored; with gtmp, the interpolated
    // green at the red and blue photosites too.
    void
    fillTile(float* const rgb[3],
             const float* in,
             const float* gtmp,
             const Tile& t,
             int width,
             int height,
             std::uint32_t filters)
    {
        const int top = t.top;
        const int left = t.left;
        const int rr1 = t.rr1;
        const int cc1 = t.cc1;
        const int rrmin = t.rrmin;
        const int rrmax = t.rrmax;
        const int ccmin = t.ccmin;
        const int ccmax = t.ccmax;

        for (int rr = rrmin; rr < rrmax; rr++) {
            const int row = rr + top;
            int c = FC(rr, ccmin, filters);
            const int c_diff = c ^ FC(rr, ccmin + 1, filters);
            for (int cc = ccmin; cc < ccmax; cc++) {
                const int col = cc + left;
                const std::size_t indx = (std::size_t)row * width + col;
                const std::size_t indx1 = (std::size_t)rr * ts + cc;
                rgb[c][indx1] = in[indx];
                if (gtmp && ((c & 1) == 0)) {
                    rgb[1][indx1] = gtmp[indx];
                }
                c ^= c_diff;
            }
        }
        if (rrmin > 0) {
            for (int rr = 0; rr < border; rr++) {
                for (int cc = ccmin; cc < ccmax; cc++) {
                    const int c = FC(rr, cc, filters);
                    rgb[c][rr * ts + cc] = rgb[c][(border2 - rr) * ts + cc];
                    if (gtmp) {
                        rgb[1][rr * ts + cc] = rgb[1][(border2 - rr) * ts + cc];
                    }
                }
            }
        }
        if (rrmax < rr1) {
            for (int rr = 0; rr < std::min(border, rr1 - rrmax); rr++) {
                for (int cc = ccmin; cc < ccmax; cc++) {
                    const int c = FC(rr, cc, filters);
                    const std::size_t src = (std::size_t)(height - rr - 2) * width + left + cc;
                    rgb[c][(rrmax + rr) * ts + cc] = in[src];
                    if (gtmp) {
                        rgb[1][(rrmax + rr) * ts + cc] = gtmp[src];
                    }
                }
            }
        }
        if (ccmin > 0) {
            for (int rr = rrmin; rr < rrmax; rr++) {
                for (int cc = 0; cc < border; cc++) {
                    const int c = FC(rr, cc, filters);
                    rgb[c][rr * ts + cc] = rgb[c][rr * ts + border2 - cc];
                    if (gtmp) {
                        rgb[1][rr * ts + cc] = rgb[1][rr * ts + border2 - cc];
                    }
                }
            }
        }
        if (ccmax < cc1) {
            for (int rr = rrmin; rr < rrmax; rr++) {
                for (int cc = 0; cc < std::min(border, cc1 - ccmax); cc++) {
                    const int c = FC(rr, cc, filters);
                    const std::size_t src = (std::size_t)(top + rr) * width + (width - cc - 2);
                    rgb[c][rr * ts + ccmax + cc] = in[src];
                    if (gtmp) {
                        rgb[1][rr * ts + ccmax + cc] = gtmp[src];
                    }
                }
            }
        }
        if ((rrmin > 0) && (ccmin > 0)) {
            for (int rr = 0; rr < border; rr++) {
                for (int cc = 0; cc < border; cc++) {
                    const int c = FC(rr, cc, filters);
                    const std::size_t src = (std::size_t)(border2 - rr) * width + border2 - cc;
                    rgb[c][(rr)*ts + cc] = in[src];
                    if (gtmp) {
                        rgb[1][(rr)*ts + cc] = gtmp[src];
                    }
                }
            }
        }
        if ((rrmax < rr1) && (ccmax < cc1)) {
            for (int rr = 0; rr < std::min(border, rr1 - rrmax); rr++) {
                for (int cc = 0; cc < std::min(border, cc1 - ccmax); cc++) {
                    const int c = FC(rr, cc, filters);
                    const std::size_t src = (std::size_t)(height - rr - 2) * width + (width - cc - 2);
                    rgb[c][(rrmax + rr) * ts + ccmax + cc] = in[src];
                    if (gtmp) {
                        rgb[1][(rrmax + rr) * ts + ccmax + cc] = gtmp[src];
                    }
                }
            }
        }
        if ((rrmin > 0) && (ccmax < cc1)) {
            for (int rr = 0; rr < border; rr++) {
                for (int cc = 0; cc < std::min(border, cc1 - ccmax); cc++) {
                    const int c = FC(rr, cc, filters);
                    const std::size_t src = (std::size_t)(border2 - rr) * width + (width - cc - 2);
                    rgb[c][(rr)*ts + ccmax + cc] = in[src];
                    if (gtmp) {
                        rgb[1][(rr)*ts + ccmax + cc] = gtmp[src];
                    }
                }
            }
        }
        if ((rrmax < rr1) && (ccmin > 0)) {
            for (int rr = 0; rr < std::min(border, rr1 - rrmax); rr++) {
                for (int cc = 0; cc < border; cc++) {
                    const int c = FC(rr, cc, filters);
                    const std::size_t src = (std::size_t)(height - rr - 2) * width + (border2 - cc);
                    rgb[c][(rrmax + rr) * ts + cc] = in[src];
                    if (gtmp) {
                        rgb[1][(rrmax + rr) * ts + cc] = gtmp[src];
                    }
                }
            }
        }
    } // fillTile

    // The scratch of one tile: three colour planes and six half-size planes.
    struct TileBuffers {
        std::vector<float> data;
        float* rgb[3];
        float* rbhpfh;
        float* rbhpfv;
        float* rblpfh;
        float* rblpfv;
        float* grblpfh;
        float* grblpfv;

        TileBuffers()
            : data((std::size_t)3 * ts * ts + (std::size_t)6 * ts * tsh, 0.f)
        {
            const std::size_t full = (std::size_t)ts * ts;
            const std::size_t half = (std::size_t)ts * tsh;
            float* base = data.data();
            rgb[0] = base;
            rgb[1] = base + full;
            rgb[2] = base + 2 * full;
            rbhpfh = base + 3 * full;
            rbhpfv = base + 3 * full + 1 * half;
            rblpfh = base + 3 * full + 2 * half;
            rblpfv = base + 3 * full + 3 * half;
            grblpfh = base + 3 * full + 4 * half;
            grblpfv = base + 3 * full + 5 * half;
        }
    };

    // What the diagnostic pass measures in one tile: its weight, its shifts and its share of the
    // shift statistics, which are then summed over the tiles in a fixed order.
    struct TileShift {
        float weight = 0.f;
        float shift[2][2] = { { 0.f, 0.f }, { 0.f, 0.f } };
        float ave[2][2] = { { 0.f, 0.f }, { 0.f, 0.f } };
        float sqave[2][2] = { { 0.f, 0.f }, { 0.f, 0.f } };
        float denom[2][2] = { { 0.f, 0.f }, { 0.f, 0.f } };
    };

    TileShift
    diagnoseTile(const Tile& t,
                 const float* in,
                 float* gtmp,
                 int width,
                 int height,
                 std::uint32_t filters)
    {
        TileBuffers b;
        float* const* rgb = b.rgb;
        float* rbhpfh = b.rbhpfh;
        float* rbhpfv = b.rbhpfv;
        float* rblpfh = b.rblpfh;
        float* rblpfv = b.rblpfv;
        float* grblpfh = b.grblpfh;
        float* grblpfv = b.grblpfv;
        const int top = t.top;
        const int left = t.left;
        const int rr1 = t.rr1;
        const int cc1 = t.cc1;

        fillTile(b.rgb, in, NULL, t, width, height, filters);

        for (int rr = 3; rr < rr1 - 3; rr++) {
            const int row = rr + top;
            for (int cc = 3 + (FC(rr, 3, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters); cc < cc1 - 3; cc += 2, indx += 2) {
                const float wtu = 1.f / sqrf(eps + std::fabs(rgb[1][indx + v1] - rgb[1][indx - v1]) + std::fabs(rgb[c][indx] - rgb[c][indx - v2]) + std::fabs(rgb[1][indx - v1] - rgb[1][indx - v3]));
                const float wtd = 1.f / sqrf(eps + std::fabs(rgb[1][indx - v1] - rgb[1][indx + v1]) + std::fabs(rgb[c][indx] - rgb[c][indx + v2]) + std::fabs(rgb[1][indx + v1] - rgb[1][indx + v3]));
                const float wtl = 1.f / sqrf(eps + std::fabs(rgb[1][indx + 1] - rgb[1][indx - 1]) + std::fabs(rgb[c][indx] - rgb[c][indx - 2]) + std::fabs(rgb[1][indx - 1] - rgb[1][indx - 3]));
                const float wtr = 1.f / sqrf(eps + std::fabs(rgb[1][indx - 1] - rgb[1][indx + 1]) + std::fabs(rgb[c][indx] - rgb[c][indx + 2]) + std::fabs(rgb[1][indx + 1] - rgb[1][indx + 3]));
                rgb[1][indx] = (wtu * rgb[1][indx - v1] + wtd * rgb[1][indx + v1] + wtl * rgb[1][indx - 1] + wtr * rgb[1][indx + 1]) / (wtu + wtd + wtl + wtr);
            }
            if ((row > -1) && (row < height)) {
                for (int col = std::max(left + 3, 0), indx = rr * ts + 3 - ((left < 0) ? (left + 3) : 0); col < std::min(cc1 + left - 3, width); col++, indx++) {
                    gtmp[(std::size_t)row * width + col] = rgb[1][indx];
                }
            }
        }
        for (int rr = borderh; rr < rr1 - borderh; rr++) {
            for (int cc = borderh + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters); cc < cc1 - borderh; cc += 2, indx += 2) {
                rbhpfv[indx >> 1] = std::fabs(std::fabs((rgb[1][indx] - rgb[c][indx]) - (rgb[1][indx + v4] - rgb[c][indx + v4])) + std::fabs((rgb[1][indx - v4] - rgb[c][indx - v4]) - (rgb[1][indx] - rgb[c][indx])) - std::fabs((rgb[1][indx - v4] - rgb[c][indx - v4]) - (rgb[1][indx + v4] - rgb[c][indx + v4])));
                rbhpfh[indx >> 1] = std::fabs(std::fabs((rgb[1][indx] - rgb[c][indx]) - (rgb[1][indx + 4] - rgb[c][indx + 4])) + std::fabs((rgb[1][indx - 4] - rgb[c][indx - 4]) - (rgb[1][indx] - rgb[c][indx])) - std::fabs((rgb[1][indx - 4] - rgb[c][indx - 4]) - (rgb[1][indx + 4] - rgb[c][indx + 4])));
                const float glpfv = 0.25f * (2.f * rgb[1][indx] + rgb[1][indx + v2] + rgb[1][indx - v2]);
                const float glpfh = 0.25f * (2.f * rgb[1][indx] + rgb[1][indx + 2] + rgb[1][indx - 2]);
                rblpfv[indx >> 1] = eps + std::fabs(glpfv - 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + v2] + rgb[c][indx - v2]));
                rblpfh[indx >> 1] = eps + std::fabs(glpfh - 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + 2] + rgb[c][indx - 2]));
                grblpfv[indx >> 1] = glpfv + 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + v2] + rgb[c][indx - v2]);
                grblpfh[indx >> 1] = glpfh + 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + 2] + rgb[c][indx - 2]);
            }
        }

        float coeff[2][3][2] = {};
        for (int rr = border; rr < rr1 - border; rr++) {
            for (int cc = border + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters); cc < cc1 - border; cc += 2, indx += 2) {
                float gdiff = 0.3125f * (rgb[1][indx + ts] - rgb[1][indx - ts]) + 0.09375f * (rgb[1][indx + ts + 1] - rgb[1][indx - ts + 1] + rgb[1][indx + ts - 1] - rgb[1][indx - ts - 1]);
                const float deltgrb = (rgb[c][indx] - rgb[1][indx]);
                float gradwt = std::fabs(0.25f * rbhpfv[indx >> 1] + 0.125f * (rbhpfv[(indx >> 1) + 1] + rbhpfv[(indx >> 1) - 1]))
                    * (grblpfv[(indx >> 1) - v1] + grblpfv[(indx >> 1) + v1])
                    / (eps + 0.1f * (grblpfv[(indx >> 1) - v1] + grblpfv[(indx >> 1) + v1]) + rblpfv[(indx >> 1) - v1] + rblpfv[(indx >> 1) + v1]);
                coeff[0][0][c >> 1] += gradwt * deltgrb * deltgrb;
                coeff[0][1][c >> 1] += gradwt * gdiff * deltgrb;
                coeff[0][2][c >> 1] += gradwt * gdiff * gdiff;
                gdiff = 0.3125f * (rgb[1][indx + 1] - rgb[1][indx - 1]) + 0.09375f * (rgb[1][indx + 1 + ts] - rgb[1][indx - 1 + ts] + rgb[1][indx + 1 - ts] - rgb[1][indx - 1 - ts]);
                gradwt = std::fabs(0.25f * rbhpfh[indx >> 1] + 0.125f * (rbhpfh[(indx >> 1) + v1] + rbhpfh[(indx >> 1) - v1]))
                    * (grblpfh[(indx >> 1) - 1] + grblpfh[(indx >> 1) + 1])
                    / (eps + 0.1f * (grblpfh[(indx >> 1) - 1] + grblpfh[(indx >> 1) + 1]) + rblpfh[(indx >> 1) - 1] + rblpfh[(indx >> 1) + 1]);
                coeff[1][0][c >> 1] += gradwt * deltgrb * deltgrb;
                coeff[1][1][c >> 1] += gradwt * gdiff * deltgrb;
                coeff[1][2][c >> 1] += gradwt * gdiff * gdiff;
            }
        }

        TileShift s;
        for (int c = 0; c < 2; c++) {
            for (int dir = 0; dir < 2; dir++) {
                float shift;
                if (coeff[dir][2][c] > eps2) {
                    shift = coeff[dir][1][c] / coeff[dir][2][c];
                    s.weight = coeff[dir][2][c] / (eps + coeff[dir][0][c]);
                } else {
                    shift = 17.0f;
                    s.weight = 0;
                }
                if (std::fabs(shift) < 2.0f) {
                    s.ave[dir][c] += shift;
                    s.sqave[dir][c] += sqrf(shift);
                    s.denom[dir][c] += 1;
                }
                s.shift[c][dir] = shift;
            }
        }

        return s;
    } // diagnoseTile

    void
    correctTile(const Tile& t,
                const float* in,
                const float* gtmp,
                float* rawDataTmp,
                int width,
                int height,
                std::uint32_t filters,
                int polyord,
                const double fitparams[2][2][16])
    {
        TileBuffers b;
        float* const* rgb = b.rgb;
        float* grbdiff = b.rbhpfh;
        float* gshift = b.rbhpfv;
        const int top = t.top;
        const int left = t.left;
        const int vblock = t.vblock;
        const int hblock = t.hblock;
        const int rr1 = t.rr1;
        const int cc1 = t.cc1;

        fillTile(b.rgb, in, gtmp, t, width, height, filters);

        float lblockshifts[2][2] = { { 0, 0 }, { 0, 0 } };
        {
            float powVblock = 1.0f;
            for (int i = 0; i < polyord; i++) {
                float powHblock = powVblock;
                for (int j = 0; j < polyord; j++) {
                    lblockshifts[0][0] += powHblock * fitparams[0][0][polyord * i + j];
                    lblockshifts[0][1] += powHblock * fitparams[0][1][polyord * i + j];
                    lblockshifts[1][0] += powHblock * fitparams[1][0][polyord * i + j];
                    lblockshifts[1][1] += powHblock * fitparams[1][1][polyord * i + j];
                    powHblock *= hblock;
                }
                powVblock *= vblock;
            }
            const float bslim = 3.99f;
            lblockshifts[0][0] = clampf(lblockshifts[0][0], -bslim, bslim);
            lblockshifts[0][1] = clampf(lblockshifts[0][1], -bslim, bslim);
            lblockshifts[1][0] = clampf(lblockshifts[1][0], -bslim, bslim);
            lblockshifts[1][1] = clampf(lblockshifts[1][1], -bslim, bslim);
        }

        int GRBdir[2][3] = {};
        int shifthfloor[3] = {};
        int shiftvfloor[3] = {};
        int shifthceil[3] = {};
        int shiftvceil[3] = {};
        float shifthfrac[3] = {};
        float shiftvfrac[3] = {};
        for (int c = 0; c < 3; c += 2) {
            shiftvfloor[c] = (int)std::floor(lblockshifts[c >> 1][0]);
            shiftvceil[c] = (int)std::ceil(lblockshifts[c >> 1][0]);
            if (lblockshifts[c >> 1][0] < 0.f) {
                std::swap(shiftvfloor[c], shiftvceil[c]);
            }
            shiftvfrac[c] = std::fabs(lblockshifts[c >> 1][0] - shiftvfloor[c]);
            shifthfloor[c] = (int)std::floor(lblockshifts[c >> 1][1]);
            shifthceil[c] = (int)std::ceil(lblockshifts[c >> 1][1]);
            if (lblockshifts[c >> 1][1] < 0.f) {
                std::swap(shifthfloor[c], shifthceil[c]);
            }
            shifthfrac[c] = std::fabs(lblockshifts[c >> 1][1] - shifthfloor[c]);
            GRBdir[0][c] = (lblockshifts[c >> 1][0] > 0) ? 2 : -2;
            GRBdir[1][c] = (lblockshifts[c >> 1][1] > 0) ? 2 : -2;
        }

        for (int rr = borderh; rr < rr1 - borderh; rr++) {
            for (int cc = borderh + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters); cc < cc1 - borderh; cc += 2) {
                const float Ginthfloor = interpolatef(shifthfrac[c], rgb[1][(rr + shiftvfloor[c]) * ts + cc + shifthceil[c]], rgb[1][(rr + shiftvfloor[c]) * ts + cc + shifthfloor[c]]);
                const float Ginthceil = interpolatef(shifthfrac[c], rgb[1][(rr + shiftvceil[c]) * ts + cc + shifthceil[c]], rgb[1][(rr + shiftvceil[c]) * ts + cc + shifthfloor[c]]);
                const float Gint = interpolatef(shiftvfrac[c], Ginthceil, Ginthfloor);
                grbdiff[(rr * ts + cc) >> 1] = Gint - rgb[c][rr * ts + cc];
                gshift[(rr * ts + cc) >> 1] = Gint;
            }
        }

        shifthfrac[0] /= 2.f;
        shifthfrac[2] /= 2.f;
        shiftvfrac[0] /= 2.f;
        shiftvfrac[2] /= 2.f;

        for (int rr = border; rr < rr1 - border; rr++) {
            for (int cc = border + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters), indx = rr * ts + cc; cc < cc1 - border; cc += 2, indx += 2) {
                const float grbdiffold = rgb[1][indx] - rgb[c][indx];
                const float grbdiffinthfloor = interpolatef(shifthfrac[c], grbdiff[(indx - GRBdir[1][c]) >> 1], grbdiff[indx >> 1]);
                const float grbdiffinthceil = interpolatef(shifthfrac[c], grbdiff[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1], grbdiff[((rr - GRBdir[0][c]) * ts + cc) >> 1]);
                float grbdiffint = interpolatef(shiftvfrac[c], grbdiffinthceil, grbdiffinthfloor);
                const float RBint = rgb[1][indx] - grbdiffint;
                if (std::fabs(RBint - rgb[c][indx]) < 0.25f * (RBint + rgb[c][indx])) {
                    if (std::fabs(grbdiffold) > std::fabs(grbdiffint)) {
                        rgb[c][indx] = RBint;
                    }
                } else {
                    const float p0 = 1.0f / (eps + std::fabs(rgb[1][indx] - gshift[indx >> 1]));
                    const float p1 = 1.0f / (eps + std::fabs(rgb[1][indx] - gshift[(indx - GRBdir[1][c]) >> 1]));
                    const float p2 = 1.0f / (eps + std::fabs(rgb[1][indx] - gshift[((rr - GRBdir[0][c]) * ts + cc) >> 1]));
                    const float p3 = 1.0f / (eps + std::fabs(rgb[1][indx] - gshift[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1]));
                    grbdiffint = (p0 * grbdiff[indx >> 1] + p1 * grbdiff[(indx - GRBdir[1][c]) >> 1] + p2 * grbdiff[((rr - GRBdir[0][c]) * ts + cc) >> 1] + p3 * grbdiff[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1]) / (p0 + p1 + p2 + p3);
                    if (std::fabs(grbdiffold) > std::fabs(grbdiffint)) {
                        rgb[c][indx] = rgb[1][indx] - grbdiffint;
                    }
                }
                if (grbdiffold * grbdiffint < 0) {
                    rgb[c][indx] = rgb[1][indx] - 0.5f * (grbdiffold + grbdiffint);
                }
            }
        }

        for (int rr = border; rr < rr1 - border; rr++) {
            const int c = FC(rr + top, left + border + (FC(rr + top, 2, filters) & 1), filters);
            const int row = rr + top;
            std::size_t indx = ((std::size_t)row * width + (std::size_t)(border + (FC(rr, 2, filters) & 1) + left)) >> 1;
            for (int cc = border + (FC(rr, 2, filters) & 1); cc < cc1 - border; cc += 2, indx++) {
                rawDataTmp[indx] = rgb[c][(rr)*ts + cc];
            }
        }
    } // correctTile

    // dt_gaussian_blur() of order 0 on one channel, every value clamped to [mn, mx] as it is read.
    void
    gaussianBlur(float* image,
                 int width,
                 int height,
                 float mn,
                 float mx,
                 float sigma,
                 const ParallelFor& parallel)
    {
        const float alpha = 1.695f / sigma;
        const float ema = std::exp(-alpha);
        const float ema2 = std::exp(-2.0f * alpha);
        const float b1 = -2.0f * ema;
        const float b2 = ema2;
        const float k = (1.0f - ema) * (1.0f - ema) / (1.0f + (2.0f * alpha * ema) - ema2);
        const float a0 = k;
        const float a1 = k * (alpha - 1.0f) * ema;
        const float a2 = k * (alpha + 1.0f) * ema;
        const float a3 = -k * ema2;
        const float coefp = (a0 + a1) / (1.0f + b1 + b2);
        const float coefn = (a2 + a3) / (1.0f + b1 + b2);
        std::vector<float> temp((std::size_t)width * height);
        const float* in = image;
        float* out = image;

        parallel(width, [&](int i) {
            float xp = clampf(in[i], mn, mx);
            float yb = xp * coefp;
            float yp = yb;
            for (int j = 0; j < height; j++) {
                const std::size_t offset = (std::size_t)j * width + i;
                const float xc = clampf(in[offset], mn, mx);
                const float yc = (a0 * xc) + (a1 * xp) - (b1 * yp) - (b2 * yb);
                temp[offset] = yc;
                xp = xc;
                yb = yp;
                yp = yc;
            }
            float xn = clampf(in[(std::size_t)(height - 1) * width + i], mn, mx);
            float xa = xn;
            float yn = xn * coefn;
            float ya = yn;
            for (int j = height - 1; j > -1; j--) {
                const std::size_t offset = (std::size_t)j * width + i;
                const float xc = clampf(in[offset], mn, mx);
                const float yc = (a2 * xn) + (a3 * xa) - (b1 * yn) - (b2 * ya);
                xa = xn;
                xn = xc;
                ya = yn;
                yn = yc;
                temp[offset] += yc;
            }
        });

        parallel(height, [&](int j) {
            const std::size_t rowStart = (std::size_t)j * width;
            float xp = clampf(temp[rowStart], mn, mx);
            float yb = xp * coefp;
            float yp = yb;
            for (int i = 0; i < width; i++) {
                const std::size_t offset = rowStart + i;
                const float xc = clampf(temp[offset], mn, mx);
                const float yc = (a0 * xc) + (a1 * xp) - (b1 * yp) - (b2 * yb);
                out[offset] = yc;
                xp = xc;
                yb = yp;
                yp = yc;
            }
            float xn = clampf(temp[rowStart + width - 1], mn, mx);
            float xa = xn;
            float yn = xn * coefn;
            float ya = yn;
            for (int i = width - 1; i > -1; i--) {
                const std::size_t offset = rowStart + i;
                const float xc = clampf(temp[offset], mn, mx);
                const float yc = (a2 * xn) + (a3 * xa) - (b1 * yn) - (b2 * ya);
                xa = xn;
                xn = xc;
                ya = yn;
                yn = yc;
                out[offset] += yc;
            }
        });
    } // gaussianBlur
} // namespace

bool
caCorrect(float* image,
          int width,
          int height,
          std::uint32_t filters,
          int iterations,
          bool avoidShift,
          float scaler,
          const ParallelFor& parallel)
{
    if ((width < kCaCorrectMinimumSize) || (height < kCaCorrectMinimumSize)) {
        return false;
    }
    const std::size_t size = (std::size_t)width * height;
    float* const out = image;
    const float inverse = 1.0f / scaler;
    for (std::size_t k = 0; k < size; ++k) {
        out[k] = inverse * out[k];
    }
    const float* const in = out;

    const int h_width = (width + 1) / 2;
    const int h_height = (height + 1) / 2;
    std::vector<float> redfactor;
    std::vector<float> bluefactor;
    std::vector<float> oldraw;
    if (avoidShift) {
        redfactor.assign((std::size_t)h_width * (h_height + 2), 0.f);
        bluefactor.assign((std::size_t)h_width * (h_height + 2), 0.f);
        oldraw.assign((std::size_t)h_width * (height + 2), 0.f);
        parallel(height, [&](int row) {
            for (int col = (FC(row, 0, filters) & 1); col < width; col += 2) {
                oldraw[(std::size_t)row * h_width + col / 2] = in[(std::size_t)row * width + col];
            }
        });
    }

    std::vector<float> gtmp(size, 0.f);
    std::vector<float> rawDataTmp(size / 2 + (std::size_t)width + 2, 0.f);

    std::vector<Tile> tiles;
    for (int top = -border; top < height; top += ts - border2) {
        for (int left = -border; left < width; left += ts - border2) {
            tiles.push_back(makeTile(top, left, width, height));
        }
    }

    const int vz1 = ((height + border2) % (ts - border2) == 0) ? 1 : 0;
    const int hz1 = ((width + border2) % (ts - border2) == 0) ? 1 : 0;
    const int vert_tiles = (int)std::ceil((float)(height + border2) / (ts - border2) + 2 + vz1);
    const int horiz_tiles = (int)std::ceil((float)(width + border2) / (ts - border2) + 2 + hz1);
    const std::size_t nBlocks = (std::size_t)vert_tiles * horiz_tiles;
    std::vector<float> blockwt(nBlocks, 0.f);
    std::vector<std::array<std::array<float, 2>, 2>> blockshifts(nBlocks, std::array<std::array<float, 2>, 2> { { { 0.f, 0.f }, { 0.f, 0.f } } });

    // darktable keeps the statistics, the fit and its order from one iteration to the next.
    float blockave[2][2] = { { 0, 0 }, { 0, 0 } };
    float blocksqave[2][2] = { { 0, 0 }, { 0, 0 } };
    float blockdenom[2][2] = { { 0, 0 }, { 0, 0 } };
    float blockvar[2][2] = { { 0, 0 }, { 0, 0 } };
    int polyord = 4;
    int numpar = 16;
    double fitparams[2][2][16] = {};
    bool processpasstwo = true;
    bool corrected = false;

    std::vector<TileShift> shifts(tiles.size());
    for (int it = 0; (it < iterations) && processpasstwo; it++) {
        // Overlapping tiles write the same interpolated greens; tiles of one parity class never
        // overlap, so each class runs in parallel and the classes in a fixed order.
        for (int phase = 0; phase < 4; ++phase) {
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                if ((((tiles[i].vblock & 1) << 1) | (tiles[i].hblock & 1)) == phase) {
                    members.push_back(i);
                }
            }
            parallel((int)members.size(), [&](int m) {
                const std::size_t i = members[(std::size_t)m];
                shifts[i] = diagnoseTile(tiles[i], in, gtmp.data(), width, height, filters);
            });
        }
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const std::size_t block = (std::size_t)tiles[i].vblock * horiz_tiles + tiles[i].hblock;
            blockwt[block] = shifts[i].weight;
            for (int c = 0; c < 2; c++) {
                for (int dir = 0; dir < 2; dir++) {
                    blockshifts[block][c][dir] = shifts[i].shift[c][dir];
                }
            }
        }
        for (const TileShift& s : shifts) {
            for (int dir = 0; dir < 2; dir++) {
                for (int c = 0; c < 2; c++) {
                    blockdenom[dir][c] += s.denom[dir][c];
                    blocksqave[dir][c] += s.sqave[dir][c];
                    blockave[dir][c] += s.ave[dir][c];
                }
            }
        }

        for (int dir = 0; dir < 2; dir++) {
            for (int c = 0; c < 2; c++) {
                if (blockdenom[dir][c] != 0.f) {
                    blockvar[dir][c] = blocksqave[dir][c] / blockdenom[dir][c] - sqrf(blockave[dir][c] / blockdenom[dir][c]);
                } else {
                    processpasstwo = false;
                    break;
                }
            }
        }
        if (processpasstwo) {
            for (int vblock = 1; vblock < vert_tiles - 1; vblock++) {
                for (int c = 0; c < 2; c++) {
                    for (int i = 0; i < 2; i++) {
                        blockshifts[(std::size_t)vblock * horiz_tiles][c][i] = blockshifts[(std::size_t)vblock * horiz_tiles + 2][c][i];
                        blockshifts[(std::size_t)vblock * horiz_tiles + horiz_tiles - 1][c][i] = blockshifts[(std::size_t)vblock * horiz_tiles + horiz_tiles - 3][c][i];
                    }
                }
            }
            for (int hblock = 0; hblock < horiz_tiles; hblock++) {
                for (int c = 0; c < 2; c++) {
                    for (int i = 0; i < 2; i++) {
                        blockshifts[hblock][c][i] = blockshifts[(std::size_t)2 * horiz_tiles + hblock][c][i];
                        blockshifts[(std::size_t)(vert_tiles - 1) * horiz_tiles + hblock][c][i] = blockshifts[(std::size_t)(vert_tiles - 3) * horiz_tiles + hblock][c][i];
                    }
                }
            }
            double polymat[2][2][256] = {};
            double shiftmat[2][2][16] = {};
            int numblox[2] = { 0, 0 };

            for (int vblock = 1; vblock < vert_tiles - 1; vblock++) {
                for (int hblock = 1; hblock < horiz_tiles - 1; hblock++) {
                    for (int c = 0; c < 2; c++) {
                        float bstemp[2];
                        for (int dir = 0; dir < 2; dir++) {
                            const float p[9] = {
                                blockshifts[(std::size_t)(vblock - 1) * horiz_tiles + hblock - 1][c][dir],
                                blockshifts[(std::size_t)(vblock - 1) * horiz_tiles + hblock][c][dir],
                                blockshifts[(std::size_t)(vblock - 1) * horiz_tiles + hblock + 1][c][dir],
                                blockshifts[(std::size_t)(vblock)*horiz_tiles + hblock - 1][c][dir],
                                blockshifts[(std::size_t)(vblock)*horiz_tiles + hblock][c][dir],
                                blockshifts[(std::size_t)(vblock)*horiz_tiles + hblock + 1][c][dir],
                                blockshifts[(std::size_t)(vblock + 1) * horiz_tiles + hblock - 1][c][dir],
                                blockshifts[(std::size_t)(vblock + 1) * horiz_tiles + hblock][c][dir],
                                blockshifts[(std::size_t)(vblock + 1) * horiz_tiles + hblock + 1][c][dir],
                            };
                            bstemp[dir] = median9f(p);
                        }
                        if ((sqrf(bstemp[0]) > caautostrength * blockvar[0][c]) || (sqrf(bstemp[1]) > caautostrength * blockvar[1][c])) {
                            continue;
                        }
                        numblox[c]++;
                        const double weight = blockwt[(std::size_t)vblock * horiz_tiles + hblock];
                        double powVblockInit = 1.0;
                        for (int i = 0; i < polyord; i++) {
                            double powHblockInit = 1.0;
                            for (int j = 0; j < polyord; j++) {
                                double powVblock = powVblockInit;
                                for (int m = 0; m < polyord; m++) {
                                    double powHblock = powHblockInit;
                                    for (int n = 0; n < polyord; n++) {
                                        const double inc = powVblock * powHblock * weight;
                                        const std::size_t idx = (std::size_t)numpar * (polyord * i + j) + (polyord * m + n);
                                        polymat[c][0][idx] += inc;
                                        polymat[c][1][idx] += inc;
                                        powHblock *= hblock;
                                    }
                                    powVblock *= vblock;
                                }
                                const double blkinc = powVblockInit * powHblockInit * weight;
                                shiftmat[c][0][(polyord * i + j)] += blkinc * bstemp[0];
                                shiftmat[c][1][(polyord * i + j)] += blkinc * bstemp[1];
                                powHblockInit *= hblock;
                            }
                            powVblockInit *= vblock;
                        }
                    }
                }
            }

            numblox[1] = std::min(numblox[0], numblox[1]);
            if (numblox[1] < 32) {
                polyord = 2;
                numpar = 4;
                if (numblox[1] < 10) {
                    processpasstwo = false;
                }
            }
            if (processpasstwo) {
                for (int c = 0; c < 2; c++) {
                    for (int dir = 0; dir < 2; dir++) {
                        if (!linEqSolve(numpar, polymat[c][dir], shiftmat[c][dir], fitparams[c][dir])) {
                            processpasstwo = false;
                        }
                    }
                }
            }
        }

        if (processpasstwo) {
            parallel((int)tiles.size(), [&](int i) {
                correctTile(tiles[(std::size_t)i], in, gtmp.data(), rawDataTmp.data(), width, height, filters, polyord, fitparams);
            });
            corrected = true;
            parallel(height, [&](int row) {
                std::size_t indx = ((std::size_t)row * width + (std::size_t)(FC(row, 0, filters) & 1)) >> 1;
                for (int col = (FC(row, 0, filters) & 1); col < width; col += 2, indx++) {
                    out[(std::size_t)row * width + col] = rawDataTmp[indx];
                }
            });
        }
    }

    if (avoidShift && processpasstwo) {
        parallel(height, [&](int row) {
            const int firstCol = FC(row, 0, filters) & 1;
            const int color = FC(row, firstCol, filters);
            float* nongreen = (color == 0) ? redfactor.data() : bluefactor.data();
            for (int col = firstCol; col < width; col += 2) {
                const std::size_t index = (std::size_t)row * width + col;
                const std::size_t oindex = (std::size_t)row * h_width + col / 2;
                nongreen[(std::size_t)(row / 2) * h_width + col / 2] = clampf(oldraw[oindex] / in[index], 0.5f, 2.0f);
            }
        });
        if (height % 2) {
            for (int col = 0; col < h_width; col++) {
                redfactor[(std::size_t)(h_height - 1) * h_width + col] = redfactor[(std::size_t)(h_height - 2) * h_width + col];
                bluefactor[(std::size_t)(h_height - 1) * h_width + col] = bluefactor[(std::size_t)(h_height - 2) * h_width + col];
            }
        }
        if (width % 2) {
            const int ngRow = 1 - (FC(0, 0, filters) & 1);
            const int ngCol = FC(ngRow, 0, filters) & 1;
            const int color = FC(ngRow, ngCol, filters);
            std::vector<float>& nongreen = (color == 0) ? redfactor : bluefactor;
            for (int row = 0; row < h_height; row++) {
                nongreen[(std::size_t)row * h_width + h_width - 1] = nongreen[(std::size_t)row * h_width + h_width - 2];
            }
        }
        gaussianBlur(redfactor.data(), h_width, h_height, 0.1f, 10.0f, 30.0f, parallel);
        gaussianBlur(bluefactor.data(), h_width, h_height, 0.1f, 10.0f, 30.0f, parallel);
        parallel(height - 4, [&](int r) {
            const int row = r + 2;
            const int firstCol = FC(row, 0, filters) & 1;
            const int color = FC(row, firstCol, filters);
            const float* nongreen = (color == 0) ? redfactor.data() : bluefactor.data();
            for (int col = firstCol; col < width - 2; col += 2) {
                out[(std::size_t)row * width + col] *= nongreen[(std::size_t)(row / 2) * h_width + col / 2];
            }
        });
    }

    for (std::size_t k = 0; k < size; ++k) {
        out[k] = out[k] * scaler;
    }

    return corrected;
} // caCorrect
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
