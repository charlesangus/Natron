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
#include <cstdlib>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace DenoiseProfiledKernels {
namespace {
    // Rows of the window one body denoises. Fixed, so that the running sums, and with them the
    // result, do not depend on the number of threads.
    const int kBandRows = 32;

    struct Patch {
        int rows;
        int cols;
    };

    int
    sign(int a)
    {
        return (a > 0) - (a < 0);
    }

    int
    scatter(float scale,
            float scattering,
            int index1,
            int index2)
    {
        const int absI1 = std::abs(index1);
        const int absI2 = std::abs(index2);

        return (int)(scale * ((absI1 * absI1 * absI1 + 7.0 * absI1 * std::sqrt((double)absI2)) * sign(index1) * scattering / 6.0 + index1));
    }

    std::vector<Patch>
    definePatches(const NlmGeometry& g,
                  int* maxShift)
    {
        std::vector<Patch> patches;
        int shift = 0;

        patches.reserve((std::size_t)(2 * g.searchRadius + 1) * (2 * g.searchRadius + 1));
        for (int rowIndex = -g.searchRadius; rowIndex <= g.searchRadius; ++rowIndex) {
            for (int colIndex = -g.searchRadius; colIndex <= g.searchRadius; ++colIndex) {
                const int r = scatter(g.scale, g.scattering, rowIndex, colIndex);
                const int c = scatter(g.scale, g.scattering, colIndex, rowIndex);
                patches.push_back(Patch { r, c });
                shift = std::max(shift, std::max(std::abs(r), std::abs(c)));
            }
        }
        *maxShift = shift;

        return patches;
    }

    inline float
    pixelDifference(const float* p1,
                    const float* p2)
    {
        const float d0 = p1[0] - p2[0];
        const float d1 = p1[1] - p2[1];
        const float d2 = p1[2] - p2[2];

        return d0 * d0 + d1 * d1 + d2 * d2;
    }

    /**
     * @brief nlmeans_denoise() with luma and chroma 1 and a unit norm, over the window rows
     * [bandY0, bandY1) and columns [windowX, windowX + windowWidth) of the width x height image
     * in. Each patch compares the (2P+1)^2 neighbourhoods of a pixel and of the pixel the patch
     * shifts it to, both clipped to the image; a shift leaving the image does not count. acc gets
     * the weighted sums and the weight in a fourth float per pixel.
     **/
    void
    nlmeansBand(const float* in,
                int width,
                int height,
                int windowX,
                int windowWidth,
                int bandY0,
                int bandY1,
                const std::vector<Patch>& patches,
                int radius,
                float centerWeight,
                float sharpness,
                float* acc)
    {
        const int side = 2 * radius + 1;
        const float cpNorm = centerWeight * side * side;
        // Column sums over [windowX - radius - 1, windowX + windowWidth + radius], zero where the
        // column or its shifted column lies outside the image.
        const int sumsX0 = windowX - radius - 1;
        std::vector<float> colSums((std::size_t)windowWidth + 2 * radius + 2, 0.f);
        float* const sums = colSums.data() - sumsX0;

        for (const Patch& patch : patches) {
            const int dr = patch.rows;
            const int dc = patch.cols;
            const int rowMin = std::max(bandY0, std::max(0, -dr));
            const int rowMax = std::min(bandY1, height - std::max(0, dr));
            const int colMin = std::max(windowX, -dc);
            const int colMax = std::min(windowX + windowWidth, width - dc);
            if ((rowMin >= rowMax) || (colMin >= colMax)) {
                continue;
            }
            const int csLo = std::max(colMin - radius, std::max(0, -dc));
            const int csHi = std::min(colMax + radius, width - std::max(0, dc));
            std::fill(colSums.begin(), colSums.end(), 0.f);
            const std::ptrdiff_t offset = ((std::ptrdiff_t)dr * width + dc) * 3;
            const auto rowLo = [&](int row) {
                return std::max(row - radius, std::max(0, -dr));
            };
            const auto rowHi = [&](int row) {
                return std::min(row + radius, std::min(height - 1, height - 1 - dr));
            };
            const auto addRow = [&](int r, float sgn) {
                const float* line = in + (std::size_t)r * width * 3;
                for (int c = csLo; c < csHi; ++c) {
                    const float* px = line + (std::size_t)c * 3;
                    sums[c] += sgn * pixelDifference(px, px + offset);
                }
            };

            for (int row = rowMin; row < rowMax; ++row) {
                if (row == rowMin) {
                    for (int r = rowLo(row); r <= rowHi(row); ++r) {
                        addRow(r, 1.f);
                    }
                } else {
                    for (int r = rowLo(row - 1); r < rowLo(row); ++r) {
                        addRow(r, -1.f);
                    }
                    for (int r = rowHi(row - 1) + 1; r <= rowHi(row); ++r) {
                        addRow(r, 1.f);
                    }
                }
                float distortion = 0.f;
                for (int i = colMin - radius; i < colMin + radius; ++i) {
                    distortion += sums[i];
                }
                const float* line = in + (std::size_t)row * width * 3;
                float* accRow = acc + (std::size_t)(row - bandY0) * windowWidth * 4;
                for (int col = colMin; col < colMax; ++col) {
                    distortion += sums[col + radius] - sums[col - radius - 1];
                    const float* px = line + (std::size_t)col * 3;
                    const float* shifted = px + offset;
                    const float dissimilarity = (distortion + cpNorm * pixelDifference(px, shifted)) / (1.0f + centerWeight);
                    const float wt = fastMexp2fInt(std::max(0.0f, dissimilarity * sharpness - 2.0f));
                    float* o = accRow + (std::size_t)(col - windowX) * 4;
                    o[0] += shifted[0] * wt;
                    o[1] += shifted[1] * wt;
                    o[2] += shifted[2] * wt;
                    o[3] += wt;
                }
            }
        }
    }
} // namespace

std::vector<std::array<int, 2>>
nlmPatches(const NlmGeometry& g)
{
    int maxShift = 0;
    const std::vector<Patch> patches = definePatches(g, &maxShift);
    std::vector<std::array<int, 2>> shifts;
    shifts.reserve(patches.size());
    for (const Patch& p : patches) {
        shifts.push_back({ p.rows, p.cols });
    }

    return shifts;
}

NlmGeometry
nlmGeometry(const Settings& settings,
            double renderScale)
{
    NlmGeometry g;

    g.scale = std::min(std::min((float)renderScale, 2.0f) / 1.0f, 1.0f);
    g.patchRadius = (int)std::ceil(settings.radius * g.scale);
    int K = settings.searchRadius;
    float scattering = settings.scattering;
    const int maxk = (int)((K * K * K + 7.0 * K * std::sqrt((double)K)) * scattering / 6.0 + K);
    // darktable's rule for its canvas pipes, which render below full scale; its export, at full
    // scale, keeps both.
    if (g.scale < 1.0f) {
        K = (int)std::max((float)std::min(kMinCanvasSearchRadius, K), K * g.scale);
        scattering = (float)((maxk - K) * 6.0 / (K * K * K + 7.0 * K * std::sqrt((double)K)));
    }
    g.searchRadius = K;
    g.scattering = scattering;
    (void)definePatches(g, &g.maxShift);

    return g;
}

void
denoiseNlm(const float* in,
           int width,
           int height,
           int windowX,
           int windowY,
           int windowWidth,
           int windowHeight,
           const Settings& settings,
           double renderScale,
           float* out,
           const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0) || (windowWidth <= 0) || (windowHeight <= 0)) {
        return;
    }
    const NlmGeometry g = nlmGeometry(settings, renderScale);
    const float scale = g.scale;
    const int P = g.patchRadius;
    const float norm = 0.045f / ((2 * P + 1) * (2 * P + 1));
    const float centerWeight = settings.centralPixelWeight * scale;

    Vec3 wb = settings.wb;
    Vec3 p;
    for (int c = 0; c < 3; ++c) {
        p[c] = std::max(settings.shadows + 0.1f * std::log(scale / wb[c]), 0.0f);
    }
    for (int c = 0; c < 3; ++c) {
        wb[c] *= settings.strength * scale;
    }
    const float compensateP = 0.05f / std::pow(0.05f, settings.shadows);
    const float a = settings.profileA * compensateP;

    const std::size_t npixels = (std::size_t)width * height;
    std::vector<float> pre(npixels * 3);
    parallel(height, [&](int y) {
        const std::size_t first = (std::size_t)y * width;
        preconditionV2(in + first * 3, pre.data() + first * 3, (std::size_t)width, a, p, settings.profileB, wb);
    });

    int maxShift = 0;
    const std::vector<Patch> patches = definePatches(g, &maxShift);
    const int nBands = (windowHeight + kBandRows - 1) / kBandRows;
    const float bias = (float)(settings.bias - 0.5 * std::log(scale));
    parallel(nBands, [&](int band) {
        const int y0 = windowY + band * kBandRows;
        const int y1 = std::min(y0 + kBandRows, windowY + windowHeight);
        std::vector<float> acc((std::size_t)(y1 - y0) * windowWidth * 4, 0.f);
        nlmeansBand(pre.data(), width, height, windowX, windowWidth, y0, y1, patches, P, centerWeight, norm, acc.data());
        for (int y = y0; y < y1; ++y) {
            const float* a4 = acc.data() + (std::size_t)(y - y0) * windowWidth * 4;
            float* o = out + (std::size_t)(y - windowY) * windowWidth * 3;
            for (int x = 0; x < windowWidth; ++x) {
                const float w = a4[4 * x + 3];
                for (int c = 0; c < 3; ++c) {
                    o[3 * x + c] = a4[4 * x + c] / w;
                }
            }
            backtransformV2(o, (std::size_t)windowWidth, a, p, settings.profileB, bias, wb);
        }
    });
} // denoiseNlm
} // namespace DenoiseProfiledKernels

NATRON_NAMESPACE_EXIT
