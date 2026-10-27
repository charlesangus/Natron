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
 * src/common/box_filters.cc, the moving-mean filters.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2009-2026 darktable developers.
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

#include "BoxFilter.h"

#include <algorithm>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace EdgeAware {
namespace {
    // darktable's MAX_VECT: the vertical pass runs this many float columns at once.
    const int kColumnBlock = 16;
    const int kRowBlock = 16;

    inline void
    loadAdd(float* out,
            float* accum,
            const float* values,
            float* comp,
            int n,
            bool compensated)
    {
        if (compensated) {
            for (int c = 0; c < n; ++c) {
                const float v = values[c];
                out[c] = v;
                const float t1 = v - comp[c];
                const float t2 = accum[c] + t1;
                comp[c] = (t2 - accum[c]) - t1;
                accum[c] = t2;
            }
        } else {
            for (int c = 0; c < n; ++c) {
                const float v = values[c];
                out[c] = v;
                accum[c] += v;
            }
        }
    }

    inline void
    sub(float* accum,
        const float* values,
        float* comp,
        int n,
        bool compensated)
    {
        if (compensated) {
            for (int c = 0; c < n; ++c) {
                const float v = -values[c];
                const float t1 = v - comp[c];
                const float t2 = accum[c] + t1;
                comp[c] = (t2 - accum[c]) - t1;
                accum[c] = t2;
            }
        } else {
            for (int c = 0; c < n; ++c) {
                accum[c] -= values[c];
            }
        }
    }

    inline void
    storeScaled(float* out,
                const float* in,
                float scale,
                int n)
    {
        for (int c = 0; c < n; ++c) {
            out[c] = in[c] / scale;
        }
    }

    // _blur_horizontal(): n interleaved channels, the original values kept in scratch so the
    // window can drop them after buf is overwritten.
    void
    blurHorizontal(float* buf,
                   std::size_t width,
                   std::size_t radius,
                   float* scratch,
                   int n,
                   bool compensated)
    {
        float L[kBoxMaxChannels] = {};
        float comp[kBoxMaxChannels] = {};
        std::size_t hits = 0;

        for (std::size_t x = 0; x < std::min(radius, width); ++x) {
            ++hits;
            loadAdd(scratch + n * x, L, buf + n * x, comp, n, compensated);
        }
        std::size_t x;
        for (x = 0; (x <= radius) && ((x + radius) < width); ++x) {
            const std::size_t np = x + radius;
            ++hits;
            loadAdd(scratch + n * np, L, buf + n * np, comp, n, compensated);
            storeScaled(buf + n * x, L, (float)hits, n);
        }
        for (; (x <= radius) && (x < width); ++x) {
            storeScaled(buf + n * x, L, (float)hits, n);
        }
        for (; x + radius < width; ++x) {
            const std::size_t op = x - radius - 1;
            const std::size_t np = x + radius;
            sub(L, scratch + n * op, comp, n, compensated);
            loadAdd(scratch + n * np, L, buf + n * np, comp, n, compensated);
            storeScaled(buf + n * x, L, (float)hits, n);
        }
        for (; x < width; ++x) {
            const std::size_t op = x - radius - 1;
            --hits;
            sub(L, scratch + n * op, comp, n, compensated);
            storeScaled(buf + n * x, L, (float)hits, n);
        }
    }

    inline std::size_t
    circularMask(std::size_t radius)
    {
        std::size_t mask = 1;

        for (std::size_t r = (2 * radius + 1); r > 1; r >>= 1) {
            mask = (mask << 1) | 1;
        }

        return mask;
    }

    // _blur_vertical(): n adjacent float columns of an image whose rows are stride floats apart,
    // the window's original values kept in a circular scratch of (mask + 1) * n floats.
    void
    blurVertical(float* buf,
                 std::size_t height,
                 std::size_t stride,
                 std::size_t radius,
                 float* scratch,
                 int n,
                 bool compensated)
    {
        const std::size_t mask = circularMask(radius);
        float L[kColumnBlock] = {};
        float comp[kColumnBlock] = {};
        std::size_t hits = 0;

        for (std::size_t y = 0; y < std::min(radius, height); ++y) {
            ++hits;
            loadAdd(scratch + n * (y & mask), L, buf + y * stride, comp, n, compensated);
        }
        std::size_t y;
        for (y = 0; (y <= radius) && (y + radius < height); ++y) {
            const std::size_t np = y + radius;
            ++hits;
            loadAdd(scratch + n * (np & mask), L, buf + np * stride, comp, n, compensated);
            storeScaled(buf + y * stride, L, (float)hits, n);
        }
        for (; (y <= radius) && (y < height); ++y) {
            storeScaled(buf + y * stride, L, (float)hits, n);
        }
        for (; y + radius < height; ++y) {
            const std::size_t np = y + radius;
            const std::size_t op = y - radius - 1;
            sub(L, scratch + n * (op & mask), comp, n, compensated);
            loadAdd(scratch + n * (np & mask), L, buf + np * stride, comp, n, compensated);
            storeScaled(buf + y * stride, L, (float)hits, n);
        }
        for (; y < height; ++y) {
            const std::size_t op = y - radius - 1;
            --hits;
            sub(L, scratch + n * (op & mask), comp, n, compensated);
            storeScaled(buf + y * stride, L, (float)hits, n);
        }
    }
} // namespace

void
boxMeanRow(float* row,
           int width,
           int channels,
           int radius,
           BoxSummationEnum summation,
           float* scratch)
{
    if ((width <= 0) || (channels <= 0) || (channels > kBoxMaxChannels)) {
        return;
    }
    blurHorizontal(row, (std::size_t)width, (std::size_t)std::max(0, radius), scratch, channels, summation == eBoxSummationKahan);
}

void
boxMeanColumns(float* buf,
               int width,
               int height,
               int channels,
               int radius,
               BoxSummationEnum summation,
               const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0) || (channels <= 0)) {
        return;
    }
    const std::size_t stride = (std::size_t)width * (std::size_t)channels;
    const std::size_t r = (std::size_t)std::max(0, radius);
    const std::size_t scratchSize = (circularMask(r) + 1) * kColumnBlock;
    const bool compensated = summation == eBoxSummationKahan;
    const int blocks = (int)((stride + kColumnBlock - 1) / kColumnBlock);

    // Columns are independent, so grouping them changes nothing in the result.
    forBlocks(parallel, blocks, 8, [&](int begin, int end) {
        std::vector<float> scratch(scratchSize);
        for (int b = begin; b < end; ++b) {
            const std::size_t col = (std::size_t)b * kColumnBlock;
            const int n = (int)std::min<std::size_t>(kColumnBlock, stride - col);
            blurVertical(buf + col, (std::size_t)height, stride, r, scratch.data(), n, compensated);
        }
    });
}

void
boxMean(float* buf,
        int width,
        int height,
        int channels,
        int radius,
        int iterations,
        BoxSummationEnum summation,
        const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0) || (channels <= 0) || (channels > kBoxMaxChannels)) {
        return;
    }
    const std::size_t rowSize = (std::size_t)width * (std::size_t)channels;
    NonFiniteSamples nonFinite;
    nonFinite.take(buf, rowSize * (std::size_t)height);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
            std::vector<float> scratch(rowSize);
            for (int row = begin; row < end; ++row) {
                boxMeanRow(buf + (std::size_t)row * rowSize, width, channels, radius, summation, scratch.data());
            }
        });
        boxMeanColumns(buf, width, height, channels, radius, summation, parallel);
    }
    nonFinite.restore(buf);
}

void
boxMeanVariance(const float* in,
                int width,
                int height,
                int radius,
                float* mean,
                float* variance,
                const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t size = (std::size_t)width * (std::size_t)height;
    std::vector<float> storage;
    const float* finite = finiteView(in, size, storage);
    std::vector<float> moments(size * 2);

    forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
        for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
            moments[2 * k] = finite[k];
            moments[2 * k + 1] = finite[k] * finite[k];
        }
    });
    boxMean(moments.data(), width, height, 2, radius, 1, eBoxSummationKahan, parallel);
    forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
        for (std::size_t k = (std::size_t)begin * width; k < (std::size_t)end * width; ++k) {
            const float m = moments[2 * k];
            mean[k] = m;
            variance[k] = std::fmax(moments[2 * k + 1] - m * m, 0.f);
        }
    });
    if (finite != in) {
        for (std::size_t k = 0; k < size; ++k) {
            if (!std::isfinite(in[k])) {
                mean[k] = in[k];
                variance[k] = 0.f;
            }
        }
    }
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT
