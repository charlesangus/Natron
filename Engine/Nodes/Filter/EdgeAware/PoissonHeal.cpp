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
 * src/common/heal.c, C path only.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2017-2026 darktable developers.
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

#include "PoissonHeal.h"

#include <algorithm>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace EdgeAware {
namespace {
    const int kRowBlock = 16;
    const int kRunBlock = 256;

    // A run of same-coloured masked pixels: its first pixel's index in the padded colour buffer
    // and its length.
    struct Run {
        std::size_t index;
        std::size_t count;
    };

    // dest - src split into checkerboard halves, each with a zero padding row above and below. In
    // an even image row, even columns go to black.
    void
    healSub(const float* top,
            const float* bottom,
            float* red,
            float* black,
            std::size_t width,
            std::size_t height,
            const ParallelFor& parallel)
    {
        const std::size_t resStride = 4 * ((width + 1) / 2);

        forBlocks(parallel, (int)height, kRowBlock, [&](int begin, int end) {
            for (std::size_t row = (std::size_t)begin; row < (std::size_t)end; ++row) {
                const bool parity = row & 1;
                const std::size_t rowStart = (row + 1) * resStride;
                float* buf1 = parity ? red + rowStart : black + rowStart;
                float* buf2 = parity ? black + rowStart : red + rowStart;
                for (std::size_t col = 0; col < width / 2; ++col) {
                    const std::size_t idx = 4 * (row * width + 2 * col);
                    for (int c = 0; c < 4; ++c) {
                        buf1[4 * col + c] = top[idx + c] - bottom[idx + c];
                        buf2[4 * col + c] = top[idx + 4 + c] - bottom[idx + 4 + c];
                    }
                }
                if (width & 1) {
                    const std::size_t resIdx = (width - 1) / 2;
                    const std::size_t idx = 4 * (row * width + (width - 1));
                    for (int c = 0; c < 4; ++c) {
                        buf1[4 * resIdx + c] = top[idx + c] - bottom[idx + c];
                        buf2[4 * resIdx + c] = 0.0f;
                    }
                }
            }
        });
        std::fill(red, red + resStride, 0.f);
        std::fill(red + (height + 1) * resStride, red + (height + 2) * resStride, 0.f);
        std::fill(black, black + resStride, 0.f);
        std::fill(black + (height + 1) * resStride, black + (height + 2) * resStride, 0.f);
    }

    void
    healAdd(const float* red,
            const float* black,
            const float* second,
            float* result,
            std::size_t width,
            std::size_t height,
            const ParallelFor& parallel)
    {
        const std::size_t resStride = 4 * ((width + 1) / 2);

        forBlocks(parallel, (int)height, kRowBlock, [&](int begin, int end) {
            for (std::size_t row = (std::size_t)begin; row < (std::size_t)end; ++row) {
                const bool parity = row & 1;
                const std::size_t rowStart = (row + 1) * resStride;
                const float* buf1 = parity ? red + rowStart : black + rowStart;
                const float* buf2 = parity ? black + rowStart : red + rowStart;
                for (std::size_t col = 0; col < width / 2; ++col) {
                    const std::size_t idx = 4 * (row * width + 2 * col);
                    for (int c = 0; c < 4; ++c) {
                        result[idx + c] = buf1[4 * col + c] + second[idx + c];
                        result[idx + 4 + c] = buf2[4 * col + c] + second[idx + 4 + c];
                    }
                }
                if (width & 1) {
                    const std::size_t resIdx = (width - 1) / 2;
                    const std::size_t idx = 4 * (row * width + (width - 1));
                    for (int c = 0; c < 4; ++c) {
                        result[idx + c] = buf1[4 * resIdx + c] + second[idx + c];
                    }
                }
            }
        });
    }

    // One over-relaxed sweep of one colour, width being the colour buffer's width in pixels. Runs
    // never share a pixel, so their order cannot change the result; the error is summed per block
    // of runs, then over blocks in order.
    float
    healLaplaceIteration(float* active,
                         const float* neighbor,
                         std::size_t height,
                         std::size_t width,
                         const std::vector<Run>& runs,
                         std::size_t startParity,
                         float w,
                         const ParallelFor& parallel)
    {
        const int blocks = (int)((runs.size() + kRunBlock - 1) / kRunBlock);
        std::vector<float> partials((std::size_t)blocks * 4, 0.f);

        parallel(blocks, [&](int b) {
            float err[4] = { 0.f, 0.f, 0.f, 0.f };
            const std::size_t end = std::min(runs.size(), (std::size_t)(b + 1) * kRunBlock);
            for (std::size_t i = (std::size_t)b * kRunBlock; i < end; ++i) {
                const std::size_t idx = runs[i].index;
                const std::size_t count = runs[i].count;
                const std::size_t index = 4 * idx;
                const std::size_t row = idx / width;
                float a = 4.0f;
                if (row == 1) {
                    a -= 1.0f;
                }
                if (row == height) {
                    a -= 1.0f;
                }
                const std::size_t vertOffset = 4 * width;
                const std::size_t lroffset = 4 * (startParity ^ (row & 1));
                if (count == 1) {
                    const std::size_t col = idx % width;
                    float aa = a;
                    float left[4] = { 0.f, 0.f, 0.f, 0.f };
                    float right[4] = { 0.f, 0.f, 0.f, 0.f };
                    if ((col > 0) || lroffset) {
                        for (int c = 0; c < 4; ++c) {
                            left[c] = neighbor[index - 4 + lroffset + c];
                        }
                    } else {
                        aa -= 1.0f;
                    }
                    if ((col + 1 < width) || (lroffset == 0)) {
                        for (int c = 0; c < 4; ++c) {
                            right[c] = neighbor[index + lroffset + c];
                        }
                    } else {
                        aa -= 1.0f;
                    }
                    for (int c = 0; c < 4; ++c) {
                        const float diff = w * ((aa * active[index + c]) - (neighbor[index - vertOffset + c] + neighbor[index + vertOffset + c] + left[c] + right[c]));
                        active[index + c] -= diff;
                        err[c] += diff * diff;
                    }
                    continue;
                }
                float left[4];
                for (int c = 0; c < 4; ++c) {
                    left[c] = neighbor[index - 4 + lroffset + c];
                }
                for (std::size_t j = 0; j < count; ++j) {
                    const std::size_t pixidx = index + 4 * j;
                    for (int c = 0; c < 4; ++c) {
                        const float right = neighbor[pixidx + lroffset + c];
                        const float diff = w * (a * active[pixidx + c] - (neighbor[pixidx - vertOffset + c] + neighbor[pixidx + vertOffset + c] + left[c] + right));
                        active[pixidx + c] -= diff;
                        err[c] += diff * diff;
                        left[c] = right;
                    }
                }
            }
            std::copy(err, err + 4, &partials[(std::size_t)b * 4]);
        });

        float err[4] = { 0.f, 0.f, 0.f, 0.f };
        for (int b = 0; b < blocks; ++b) {
            for (int c = 0; c < 4; ++c) {
                err[c] += partials[(std::size_t)b * 4 + c];
            }
        }

        return err[0] + err[1] + err[2];
    }

    // The runs of one row's pixels of one colour, those at columns start, start + 2, ... The first
    // pixel of the row and the last of a run ending on the row's last pixel get runs of their own,
    // so the sweep's left and right neighbours stay in the row.
    void
    collectColorRuns(const unsigned char* mask,
                     std::size_t startIndex,
                     std::size_t start,
                     std::size_t width,
                     std::vector<Run>& runs,
                     std::size_t& nmask)
    {
        std::size_t masked = 0;

        if ((start == 0) && mask[start]) {
            runs.push_back({ startIndex, 1 });
            ++masked;
            start += 2;
        }
        bool inRun = false;
        std::size_t runStart = 0;
        std::size_t col;
        for (col = start; col < width; col += 2) {
            if (mask[col]) {
                ++masked;
                if (!inRun) {
                    runStart = col;
                    inRun = true;
                }
            } else if (inRun) {
                runs.push_back({ startIndex + runStart / 2, (col - runStart) / 2 });
                inRun = false;
            }
        }
        if (inRun) {
            const Run run = { startIndex + runStart / 2, (col - runStart) / 2 };
            if ((run.count > 1) && (col > width)) {
                runs.push_back({ run.index, run.count - 1 });
                runs.push_back({ run.index + run.count - 1, 1 });
            } else {
                runs.push_back(run);
            }
        }
        nmask += masked;
    }

    void
    collectRuns(std::size_t start,
                const unsigned char* mask,
                std::size_t width,
                std::size_t height,
                std::size_t subwidth,
                std::vector<Run>& runs,
                std::size_t& nmask)
    {
        for (std::size_t row = 0; row < height; ++row) {
            const std::size_t parity = start ^ (row & 1);
            collectColorRuns(mask + row * width, (row + 1) * subwidth, parity, width, runs, nmask);
        }
    }

    void
    healLaplaceLoop(float* red,
                    float* black,
                    std::size_t width,
                    std::size_t height,
                    const unsigned char* mask,
                    int maxIter,
                    const ParallelFor& parallel)
    {
        const std::size_t subwidth = (width + 1) / 2;
        std::vector<Run> redRuns;
        std::vector<Run> blackRuns;
        std::size_t nmaskRed = 0;
        std::size_t nmaskBlack = 0;

        collectRuns(1, mask, width, height, subwidth, redRuns, nmaskRed);
        collectRuns(0, mask, width, height, subwidth, blackRuns, nmaskBlack);
        const std::size_t nmask = nmaskRed + nmaskBlack;

        // darktable's empirical over-relaxation factor, benchmarked on round brushes.
        const float w = ((2.0f - 1.0f / (0.1575f * std::sqrt((float)nmask) + 0.8f)) * .25f);
        const float epsilon = (0.1 / 255);
        const float errExit = epsilon * epsilon * w * w;

        for (int iter = 0; iter < maxIter; ++iter) {
            float err = healLaplaceIteration(black, red, height, subwidth, blackRuns, 1, w, parallel);
            err += healLaplaceIteration(red, black, height, subwidth, redRuns, 0, w, parallel);
            if (err < errExit) {
                break;
            }
        }
    }
} // namespace

void
poissonHeal(const float* src,
            float* dest,
            const float* mask,
            int width,
            int height,
            int maxIter,
            const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t w = (std::size_t)width;
    const std::size_t h = (std::size_t)height;
    const std::size_t pixels = w * h;
    const std::size_t samples = 4 * pixels;

    std::vector<unsigned char> inMask(pixels);
    for (std::size_t k = 0; k < pixels; ++k) {
        inMask[k] = std::isfinite(mask[k]) && (mask[k] != 0.f);
    }
    std::vector<float> srcStorage;
    const float* finiteSrc = finiteView(src, samples, srcStorage);
    std::vector<std::size_t> keptIndex;
    std::vector<float> keptValue;
    for (std::size_t k = 0; k < samples; ++k) {
        if (!std::isfinite(dest[k]) || !std::isfinite(src[k])) {
            keptIndex.push_back(k);
            keptValue.push_back(dest[k]);
            dest[k] = finiteSrc[k];
        }
    }

    const std::size_t subwidth = 4 * ((w + 1) / 2);
    std::vector<float> red(subwidth * (h + 2));
    std::vector<float> black(subwidth * (h + 2));

    healSub(dest, finiteSrc, red.data(), black.data(), w, h, parallel);
    healLaplaceLoop(red.data(), black.data(), w, h, inMask.data(), maxIter, parallel);
    healAdd(red.data(), black.data(), finiteSrc, dest, w, h, parallel);

    for (std::size_t i = 0; i < keptIndex.size(); ++i) {
        dest[keptIndex[i]] = keptValue[i];
    }
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT
