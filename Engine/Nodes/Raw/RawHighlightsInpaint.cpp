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
 * src/iop/hlreconstruct/inpaint.c, driven as src/iop/highlights.c drives it.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawKernels.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    // The scan of one row or column, with the offsets of the source (whose photosites may sit
    // several floats apart) and of the compact result kept apart.
    struct Scan {
        const float* in;
        float* out;
        std::ptrdiff_t inStep;
        std::ptrdiff_t outStep;
        // The source offsets of the photosite across the scan, to either side.
        std::ptrdiff_t inSide;
    };

    // Index into the ratios of a colour transition; negative when the ratio is inverted.
    const int kRatioIndex[3][3] = { { 0, -1, -2 },
                                    { 1, 0, -3 },
                                    { 2, 3, 0 } };

    inline float
    interpPixXTrans(int ratioNext,
                    std::ptrdiff_t offsetNext,
                    float clip0,
                    float clipNext,
                    const float* in,
                    const float* ratios)
    {
        // A neighbour of another colour may exceed this colour's clip: that is how the highlight
        // colour gets interpolated.
        const float clipVal = std::fmax(clip0, clipNext);

        if (in[offsetNext] >= clipNext - 1e-5f) {
            return clipVal;
        }
        if (ratioNext > 0) {
            return std::fmin(in[offsetNext] / ratios[ratioNext], clipVal);
        }

        return std::fmin(in[offsetNext] * ratios[-ratioNext], clipVal);
    }

    void
    scanBayer(const Scan& scan,
              int width,
              int height,
              int dim,
              int dir,
              int other,
              const float* clip,
              std::uint32_t filters,
              int pass)
    {
        float ratio = 1.0f;
        const int length = dim ? height : width;
        const int beg = (dir == 1) ? 0 : length - 1;
        const int end = (dir == 1) ? length : -1;
        const std::ptrdiff_t offs = scan.inStep * dir;
        const std::ptrdiff_t outOffs = scan.outStep * dir;
        const float* in = scan.in + beg * scan.inStep;
        float* out = scan.out + beg * scan.outStep;

        for (int k = beg; k != end; k += dir, in += offs, out += outOffs) {
            const int i = dim ? other : k;
            const int j = dim ? k : other;
            const float clip0 = clip[fcBayer((std::size_t)j, (std::size_t)i, filters)];
            const float clip1 = clip[fcBayer((std::size_t)(dim ? (j + 1) : j), (std::size_t)(dim ? i : (i + 1)), filters)];

            if ((i == 0) || (i == width - 1) || (j == 0) || (j == height - 1)) {
                if (pass == 3) {
                    out[0] = in[0];
                }
                continue;
            }
            if ((in[0] < clip0) && (in[0] > 1e-5f) && (in[offs] < clip1) && (in[offs] > 1e-5f)) {
                // Exponential decay of ratio = in[odd] / in[even].
                if (k & 1) {
                    ratio = (3.0f * ratio + in[0] / in[offs]) / 4.0f;
                } else {
                    ratio = (3.0f * ratio + in[offs] / in[0]) / 4.0f;
                }
            }
            if (in[0] >= clip0 - 1e-5f) {
                float add = 0.0f;
                if (in[offs] >= clip1 - 1e-5f) {
                    add = std::fmax(clip0, clip1);
                } else if (k & 1) {
                    add = in[offs] * ratio;
                } else {
                    add = in[offs] / ratio;
                }
                if (pass == 0) {
                    out[0] = add;
                } else if (pass == 3) {
                    out[0] = (out[0] + add) / 4.0f;
                } else {
                    out[0] += add;
                }
            } else if (pass == 3) {
                out[0] = in[0];
            }
        }
    }

    void
    scanXTrans(const Scan& scan,
               int width,
               int height,
               int dim,
               int dir,
               int other,
               const float* clip,
               const std::uint8_t (*xtrans)[6],
               int pass)
    {
        // Colour transitions in a row or column are not all the same pair as in Bayer, and a 2x2
        // green block carries none, so a ratio is kept per pair: 1 red-green, 2 red-blue, 3 green-blue.
        float ratios[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const int length = dim ? height : width;
        const int beg = (dir == 1) ? 0 : length - 1;
        const int end = (dir == 1) ? length : -1;
        const std::ptrdiff_t offs = scan.inStep * dir;
        const std::ptrdiff_t offl = offs - scan.inSide;
        const std::ptrdiff_t offr = offs + scan.inSide;
        const std::ptrdiff_t outOffs = scan.outStep * dir;
        const float* in = scan.in + beg * scan.inStep;
        float* out = scan.out + beg * scan.outStep;
        const float clipMax = std::max({ clip[0], clip[1], clip[2] });

        for (int k = beg; k != end; k += dir, in += offs, out += outOffs) {
            const int i = dim ? other : k;
            const int j = dim ? k : other;
            const int f0 = fcXTrans(j, i, xtrans);
            const int f1 = fcXTrans(dim ? (j + dir) : j, dim ? i : (i + dir), xtrans);
            const int fl = fcXTrans(dim ? (j + dir) : (j - 1), dim ? (i - 1) : (i + dir), xtrans);
            const int fr = fcXTrans(dim ? (j + dir) : (j + 1), dim ? (i + 1) : (i + dir), xtrans);
            const float clip0 = clip[f0];
            const float clip1 = clip[f1];
            const float clipl = clip[fl];
            const float clipr = clip[fr];

            if ((i == 0) || (i == width - 1) || (j == 0) || (j == height - 1)) {
                if (pass == 3) {
                    out[0] = std::fmin(clipMax, in[0]);
                }
                continue;
            }
            if ((f0 != f1) && (in[0] < clip0) && (in[0] > 1e-5f) && (in[offs] < clip1) && (in[offs] > 1e-5f)) {
                const int r = kRatioIndex[f0][f1];
                if (r > 0) {
                    ratios[r] = (3.f * ratios[r] + (in[offs] / in[0])) / 4.f;
                } else {
                    ratios[-r] = (3.f * ratios[-r] + (in[0] / in[offs])) / 4.f;
                }
            }
            if (in[0] >= clip0 - 1e-5f) {
                float add;
                if (f0 != f1) {
                    add = interpPixXTrans(kRatioIndex[f0][f1], offs, clip0, clip1, in, ratios);
                } else if (fl != f0) {
                    // At the start of a 2x2 green block the neighbour in line is green too: look diagonally.
                    add = interpPixXTrans(kRatioIndex[f0][fl], offl, clip0, clipl, in, ratios);
                } else {
                    add = interpPixXTrans(kRatioIndex[f0][fr], offr, clip0, clipr, in, ratios);
                }
                if (pass == 0) {
                    out[0] = add;
                } else if (pass == 3) {
                    out[0] = std::fmin(clipMax, (out[0] + add) / 4.0f);
                } else {
                    out[0] += add;
                }
            } else if (pass == 3) {
                out[0] = in[0];
            }
        }
    }
} // namespace

void
inpaintColor(const float* in,
             std::ptrdiff_t inRowStride,
             std::ptrdiff_t inPixelStride,
             float* out,
             int width,
             int height,
             std::uint32_t filters,
             const std::uint8_t (*xtrans)[6],
             const float clip[3],
             const ParallelFor& parallel)
{
    const bool isXTrans = (filters == kFiltersXTrans);

    // Rows are independent of each other, and so are columns once every row is done.
    parallel(height, [&](int j) {
        Scan scan = { in + (std::ptrdiff_t)j * inRowStride, out + (std::ptrdiff_t)j * width, inPixelStride, 1, inRowStride };
        for (int pass = 0; pass < 2; ++pass) {
            const int dir = pass ? -1 : 1;
            if (isXTrans) {
                scanXTrans(scan, width, height, 0, dir, j, clip, xtrans, pass);
            } else {
                scanBayer(scan, width, height, 0, dir, j, clip, filters, pass);
            }
        }
    });
    parallel(width, [&](int i) {
        Scan scan = { in + (std::ptrdiff_t)i * inPixelStride, out + i, inRowStride, width, inPixelStride };
        for (int pass = 2; pass < 4; ++pass) {
            const int dir = (pass == 2) ? 1 : -1;
            if (isXTrans) {
                scanXTrans(scan, width, height, 1, dir, i, clip, xtrans, pass);
            } else {
                scanBayer(scan, width, height, 1, dir, i, clip, filters, pass);
            }
        }
    });
}
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
