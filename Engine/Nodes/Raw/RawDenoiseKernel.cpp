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
 * src/iop/rawdenoise.c and the C path of dwt_denoise() in src/common/dwt.c, C paths only.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2011-2026 darktable developers.
 *     Copyright (C) 2017-2026 darktable developers (src/common/dwt.c).
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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    // darktable's channel indices into the strengths: all channels, then red, green and blue.
    const int kAll = 0;
    const int kRed = 1;
    const int kGreen = 2;
    const int kBlue = 3;

    void
    dwtDenoiseVert(float* out,
                   const float* in,
                   int height,
                   int width,
                   int lev,
                   const ParallelFor& parallel)
    {
        // Capped at height - 1: the top reflection reads |row - vscale|, one row past the buffer at
        // row 0 once vscale reaches height.
        const int vscale = std::min(1 << lev, height - 1);

        parallel(height, [&](int row) {
            const std::size_t rowstart = (std::size_t)row * width;
            const std::size_t below_row = (row + vscale < height) ? (std::size_t)(row + vscale) : (std::size_t)(2 * (height - 1) - (row + vscale));
            const float* center = in + rowstart;
            const float* above = in + (std::size_t)std::abs(row - vscale) * width;
            const float* below = in + below_row * width;
            float* outrow = out + rowstart;
            for (int col = 0; col < width; col++) {
                outrow[col] = 2.f * center[col] + above[col] + below[col];
            }
        });
    }

    void
    dwtDenoiseHoriz(float* out,
                    float* in,
                    float* accum,
                    int height,
                    int width,
                    int lev,
                    float thold,
                    bool last,
                    const ParallelFor& parallel)
    {
        const int hscale = std::min(1 << lev, width - 1);

        parallel(height, [&](int row) {
            const std::size_t rowindex = (std::size_t)row * width;
            float* details = in + rowindex;
            const float* coarse = out + rowindex;
            float* accum_row = accum + rowindex;
            // Clamped so that the left edge, the middle and the right edge stay a partition of the
            // row when 2 * hscale exceeds the width.
            const int left_end = std::min(hscale, width);
            const int right_start = std::max(width - hscale, left_end);
            for (int col = 0; col < left_end; col++) {
                const int rightpos = (col + hscale < width) ? (col + hscale) : 2 * (width - 1) - (col + hscale);
                const float hat = (2.f * coarse[col] + coarse[hscale - col] + coarse[rightpos]) / 16.f;
                const float diff = details[col] - hat;
                details[col] = hat;
                accum_row[col] += std::max(diff - thold, 0.0f) + std::min(diff + thold, 0.0f);
            }
            for (int col = left_end; col < right_start; col++) {
                const float hat = (2.f * coarse[col] + coarse[col - hscale] + coarse[col + hscale]) / 16.f;
                const float diff = details[col] - hat;
                details[col] = hat;
                accum_row[col] += std::max(diff - thold, 0.0f) + std::min(diff + thold, 0.0f);
            }
            for (int col = right_start; col < width; col++) {
                const float right = coarse[2 * (width - 1) - (col + hscale)];
                const float hat = (2.f * coarse[col] + coarse[col - hscale] + right) / 16.f;
                const float diff = details[col] - hat;
                details[col] = hat;
                accum_row[col] += std::max(diff - thold, 0.0f) + std::min(diff + thold, 0.0f);
            }
            if (last) {
                for (int col = 0; col < width; col++) {
                    details[col] += accum_row[col];
                }
            }
        });
    }

    void
    dwtDenoise(float* img,
               int width,
               int height,
               const float* noise,
               const ParallelFor& parallel)
    {
        if ((width <= 0) || (height <= 0)) {
            return;
        }
        const std::size_t size = (std::size_t)width * height;
        std::vector<float> details(size, 0.f);
        std::vector<float> interm(size, 0.f);

        for (int lev = 0; lev < kRawDenoiseBands; lev++) {
            const bool last = (lev + 1) == kRawDenoiseBands;
            dwtDenoiseVert(interm.data(), img, height, width, lev, parallel);
            dwtDenoiseHoriz(interm.data(), img, details.data(), height, width, lev, noise[lev], last, parallel);
        }
    }

    inline float
    vstransform(float value)
    {
        return std::sqrt(std::fmax(0.0f, value));
    }
} // namespace

void
rawDenoiseChannelNoise(float noise[kRawDenoiseBands],
                       int color,
                       float threshold,
                       const float force[4][kRawDenoiseBands])
{
    // Proportional to the image detail in each band, so the same for Bayer and X-Trans.
    static const float noise_all[] = { 0.8002f, 0.2735f, 0.1202f, 0.0585f, 0.0291f, 0.0152f, 0.0080f, 0.0044f };

    for (int i = 0; i < kRawDenoiseBands; i++) {
        float chan_threshold_exp_4;
        switch (color) {
        case 0:
            chan_threshold_exp_4 = force[kRed][kRawDenoiseBands - i - 1];
            break;
        case 2:
            chan_threshold_exp_4 = force[kBlue][kRawDenoiseBands - i - 1];
            break;
        default:
            chan_threshold_exp_4 = force[kGreen][kRawDenoiseBands - i - 1];
            break;
        }
        chan_threshold_exp_4 *= chan_threshold_exp_4;
        chan_threshold_exp_4 *= chan_threshold_exp_4;
        float all_threshold_exp_4 = force[kAll][kRawDenoiseBands - i - 1];
        all_threshold_exp_4 *= all_threshold_exp_4;
        all_threshold_exp_4 *= all_threshold_exp_4;
        noise[i] = noise_all[i] * all_threshold_exp_4 * chan_threshold_exp_4 * 16.0f * 16.0f;
        // Kept apart from the line above: darktable's integration test depends on the rounding.
        noise[i] *= threshold;
    }
}

void
rawDenoiseBayer(const float* in,
                float* out,
                int width,
                int height,
                std::uint32_t filters,
                float threshold,
                const float force[4][kRawDenoiseBands],
                const ParallelFor& parallel)
{
    std::vector<float> fimg((std::size_t)(width / 2 + 1) * (std::size_t)(height / 2 + 1), 0.f);

    for (int c = 0; c < 4; c++) {
        const int color = fcBayer((std::size_t)(c % 2), (std::size_t)(c / 2), filters);
        float noise[kRawDenoiseBands];
        rawDenoiseChannelNoise(noise, color, threshold, force);

        const int halfwidth = width / 2 + (width & (~(c >> 1)) & 1);
        const int halfheight = height / 2 + (height & (~c) & 1);
        const int offset = (c & 2) >> 1;
        const int senselwidth = (width - offset + 1) / 2;
        const int firstRow = c & 1;
        const int rows = (height - firstRow + 1) / 2;

        parallel(rows, [&](int r) {
            const int row = firstRow + 2 * r;
            float* fimgp = fimg.data() + (std::size_t)(row / 2) * halfwidth;
            const float* inp = in + (std::size_t)row * width + offset;
            for (int col = 0; col < senselwidth; col++) {
                fimgp[col] = std::sqrt(std::max(0.0f, inp[2 * col]));
            }
        });

        dwtDenoise(fimg.data(), halfwidth, halfheight, noise, parallel);

        parallel(rows, [&](int r) {
            const int row = firstRow + 2 * r;
            const float* fimgp = fimg.data() + (std::size_t)(row / 2) * halfwidth;
            float* outp = out + (std::size_t)row * width + offset;
            for (int col = 0; col < senselwidth; col++) {
                const float d = fimgp[col];
                outp[2 * col] = d * d;
            }
        });
    }
}

void
rawDenoiseXTrans(const float* in,
                 float* out,
                 int width,
                 int height,
                 const std::uint8_t (*xtrans)[6],
                 float threshold,
                 const float force[4][kRawDenoiseBands],
                 const ParallelFor& parallel)
{
    // Two rows more than the image: the extraction below writes one row above the first and one
    // row below the last without a special case.
    std::vector<float> img((std::size_t)width * (std::size_t)(height + 2), 0.f);
    float* const fimg = img.data() + width;
    const auto fc = [xtrans](int row, int col) {
        return fcXTrans(row, col, xtrans);
    };

    for (int c = 0; c < 3; c++) {
        float noise[kRawDenoiseBands];
        rawDenoiseChannelNoise(noise, c, threshold, force);

        for (int col = 0; col < width; col++) {
            fimg[col] = 0.5f;
            fimg[(std::size_t)(height - 1) * width + col] = 0.5f;
        }
        // darktable splits these rows into one chunk per thread and repairs the rows the chunks
        // share; a single chunk is the result that repair reproduces.
        for (int row = 0; row < height; row++) {
            const float* inp = in + (std::size_t)row * width;
            float* fimgp = fimg + (std::size_t)row * width;
            if ((c != 1) && (fc(row, 0) == c)) {
                const float d = vstransform(inp[0]);
                fimgp[0] = fimgp[-width] = fimgp[-width + 1] = d;
            }
            for (int col = (c != 1) ? 1 : 0; col < width - 1; col++) {
                if (fc(row, col) == c) {
                    const float d = vstransform(inp[col]);
                    fimgp[col] = d;
                    if (c == 1) {
                        fimgp[col + 1] = fimgp[col + width] = d;
                    } else {
                        fimgp[col - width - 1] = fimgp[col - width] = fimgp[col - width + 1] = d;
                        fimgp[col - 1] = fimgp[col + 1] = d;
                        if (row < height - 1) {
                            fimgp[col + width - 1] = fimgp[col + width] = fimgp[col + width + 1] = d;
                        }
                    }
                }
            }
            if (fc(row, 0) != c) {
                int src = 0;
                if ((row > 1) && (fc(row - 1, 0) == c)) {
                    src = -width;
                } else if (fc(row, 1) == c) {
                    src = 1;
                } else if ((row > 1) && (fc(row - 1, 1) == c)) {
                    src = -width + 1;
                }
                fimgp[0] = vstransform(inp[src]);
            }
            if ((c != 1) && (fc(row, width - 1) == c)) {
                const float d = vstransform(inp[width - 1]);
                fimgp[width - 2] = fimgp[width - 1] = fimgp[-1] = d;
            } else if (fc(row, width - 1) != c) {
                int src = width - 1;
                if (fc(row, width - 2) == c) {
                    src = width - 2;
                } else if ((row > 1) && (fc(row - 1, width - 1) == c)) {
                    src = -1;
                } else if ((row > 1) && (fc(row - 1, width - 2) == c)) {
                    src = -2;
                }
                fimgp[width - 1] = vstransform(inp[src]);
            }
        }

        dwtDenoise(fimg, width, height, noise, parallel);

        parallel(height, [&](int row) {
            const float* fimgp = fimg + (std::size_t)row * width;
            float* outp = out + (std::size_t)row * width;
            for (int col = 0; col < width; col++) {
                if (fc(row, col) == c) {
                    const float d = fimgp[col];
                    outp[col] = d * d;
                }
            }
        });
    }
}
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
