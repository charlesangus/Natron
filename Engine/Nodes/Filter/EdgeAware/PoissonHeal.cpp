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

namespace {
    // Active cells per parallel block of a multigrid level.
    const std::size_t kCellBlock = 4096;
    // The residual reduction, against the boundary terms, at which the multigrid solve stops.
    const double kMultigridTolerance = 1e-5;
    // Gauss-Seidel sweep pairs before and after each coarse-grid correction.
    const int kSmoothPairs = 2;
    // Piecewise-constant prolongation undershoots the smooth error by about half; this
    // over-correction, which keeps the cycle symmetric, roughly halves the cycles needed.
    const float kCoarseWeight = 1.8f;
    const int kCoarsestSweeps = 16;
    const int kCoarsestSize = 4;

    // A run of active cells in row y from column x0, starting at padded index start.
    struct Span {
        std::size_t start;
        int x0;
        int y;
        int count;
    };

    /*
     * One grid of the hierarchy, with a one-cell zero border so that stencils need no bounds
     * checks. The operator is (A v)_k = diag_k v_k - the sum of c v over the four neighbours,
     * with east[k] coupling k to k + 1 and south[k] coupling k to k + stride; couplings to
     * inactive cells are 0. Coarse levels aggregate 2x2 cells with piecewise-constant transfers
     * and the Galerkin operator, which stays a five-point stencil.
     */
    struct Level {
        int nx = 0;
        int ny = 0;
        std::size_t stride = 0;
        std::vector<unsigned char> active;
        std::vector<float> diag;
        std::vector<float> invDiag;
        std::vector<float> east;
        std::vector<float> south;
        std::vector<Span> spans;
        // Span ranges [first, second) of about kCellBlock active cells each.
        std::vector<std::pair<std::size_t, std::size_t>> blocks;
        std::size_t activeCount = 0;
        std::vector<float> b;
        std::vector<float> e;
        std::vector<float> t;

        std::size_t index(int x,
                          int y) const
        {
            return (std::size_t)(y + 1) * stride + (std::size_t)(x + 1);
        }

        std::size_t cells() const
        {
            return stride * (std::size_t)(ny + 2);
        }

        void allocate(int w,
                      int h)
        {
            nx = w;
            ny = h;
            stride = (std::size_t)w + 2;
            active.assign(cells(), 0);
            diag.assign(cells(), 0.f);
            invDiag.assign(cells(), 0.f);
            east.assign(cells(), 0.f);
            south.assign(cells(), 0.f);
        }

        void buildSpans()
        {
            spans.clear();
            blocks.clear();
            activeCount = 0;
            for (int y = 0; y < ny; ++y) {
                const unsigned char* row = &active[index(0, y)];
                int x = 0;
                while (x < nx) {
                    if (!row[x]) {
                        ++x;
                        continue;
                    }
                    const int x0 = x;
                    while ((x < nx) && row[x]) {
                        ++x;
                    }
                    spans.push_back({ index(x0, y), x0, y, x - x0 });
                    activeCount += (std::size_t)(x - x0);
                }
            }
            std::size_t first = 0;
            std::size_t inBlock = 0;
            for (std::size_t i = 0; i < spans.size(); ++i) {
                inBlock += (std::size_t)spans[i].count;
                if (inBlock >= kCellBlock) {
                    blocks.push_back({ first, i + 1 });
                    first = i + 1;
                    inBlock = 0;
                }
            }
            if (first < spans.size()) {
                blocks.push_back({ first, spans.size() });
            }
            for (const Span& span : spans) {
                for (int j = 0; j < span.count; ++j) {
                    const std::size_t k = span.start + (std::size_t)j;
                    invDiag[k] = (diag[k] > 0.f) ? 1.f / diag[k] : 0.f;
                }
            }
        }
    };

    // Calls body(span) on every span, the blocks in parallel; one block runs on this thread.
    template <typename Body>
    void
    forSpans(const Level& level,
             const ParallelFor& parallel,
             const Body& body)
    {
        const auto block = [&](int bi) {
            const std::pair<std::size_t, std::size_t>& range = level.blocks[(std::size_t)bi];
            for (std::size_t i = range.first; i < range.second; ++i) {
                body(level.spans[i]);
            }
        };

        if (level.blocks.size() == 1) {
            block(0);
        } else if (level.blocks.size() > 1) {
            parallel((int)level.blocks.size(), block);
        }
    }

    // Per channel sums of body(span, sums) over the spans, summed per block and then over the
    // blocks in order, so that they do not depend on the threads.
    template <typename Body>
    void
    sumSpans(const Level& level,
             const ParallelFor& parallel,
             double sums[4],
             const Body& body)
    {
        std::vector<double> partials(level.blocks.size() * 4, 0.);
        const auto block = [&](int bi) {
            const std::pair<std::size_t, std::size_t>& range = level.blocks[(std::size_t)bi];
            double* part = &partials[(std::size_t)bi * 4];
            for (std::size_t i = range.first; i < range.second; ++i) {
                body(level.spans[i], part);
            }
        };

        if (level.blocks.size() == 1) {
            block(0);
        } else if (level.blocks.size() > 1) {
            parallel((int)level.blocks.size(), block);
        }
        for (int c = 0; c < 4; ++c) {
            sums[c] = 0.;
        }
        for (std::size_t bi = 0; bi < level.blocks.size(); ++bi) {
            for (int c = 0; c < 4; ++c) {
                sums[c] += partials[bi * 4 + c];
            }
        }
    }

    // The cells of span of one checkerboard colour start at this offset in it.
    inline int
    colourStart(const Span& span,
                int colour)
    {
        return ((span.x0 + span.y) ^ colour) & 1;
    }

    // One Gauss-Seidel sweep over the cells of one checkerboard colour of A e = b.
    void
    relax(const Level& level,
          const float* b,
          float* e,
          int colour,
          const ParallelFor& parallel)
    {
        const std::size_t s = level.stride;
        const float* const east = level.east.data();
        const float* const south = level.south.data();
        const float* const invDiag = level.invDiag.data();

        forSpans(level, parallel, [&](const Span& span) {
            for (int j = colourStart(span, colour); j < span.count; j += 2) {
                const std::size_t k = span.start + (std::size_t)j;
                const float cw = east[k - 1];
                const float ce = east[k];
                const float cn = south[k - s];
                const float cs = south[k];
                const float inv = invDiag[k];
                const float* const left = e + 4 * (k - 1);
                const float* const right = e + 4 * (k + 1);
                const float* const up = e + 4 * (k - s);
                const float* const down = e + 4 * (k + s);
                const float* const bk = b + 4 * k;
                float v[4];
                for (int c = 0; c < 4; ++c) {
                    v[c] = (bk[c] + cw * left[c] + ce * right[c] + cn * up[c] + cs * down[c]) * inv;
                }
                std::copy(v, v + 4, e + 4 * k);
            }
        });
    }

    // out = (A v)_k.
    inline void
    applyAt(const float* diag,
            const float* east,
            const float* south,
            std::size_t s,
            const float* v,
            std::size_t k,
            float out[4])
    {
        const float cw = east[k - 1];
        const float ce = east[k];
        const float cn = south[k - s];
        const float cs = south[k];
        const float d = diag[k];
        const float* const left = v + 4 * (k - 1);
        const float* const right = v + 4 * (k + 1);
        const float* const up = v + 4 * (k - s);
        const float* const down = v + 4 * (k + s);
        const float* const centre = v + 4 * k;

        for (int c = 0; c < 4; ++c) {
            out[c] = d * centre[c] - (cw * left[c] + ce * right[c] + cn * up[c] + cs * down[c]);
        }
    }

    // The aggregation of fine's 2x2 cells into coarse, with the Galerkin operator.
    void
    coarsen(const Level& fine,
            Level& coarse)
    {
        coarse.allocate((fine.nx + 1) / 2, (fine.ny + 1) / 2);
        for (const Span& span : fine.spans) {
            for (int j = 0; j < span.count; ++j) {
                const int x = span.x0 + j;
                const std::size_t k = span.start + (std::size_t)j;
                const std::size_t kc = coarse.index(x / 2, span.y / 2);
                coarse.active[kc] = 1;
                coarse.diag[kc] += fine.diag[k];
                if ((x & 1) == 0) {
                    coarse.diag[kc] -= 2.f * fine.east[k];
                } else {
                    coarse.east[kc] += fine.east[k];
                }
                if ((span.y & 1) == 0) {
                    coarse.diag[kc] -= 2.f * fine.south[k];
                } else {
                    coarse.south[kc] += fine.south[k];
                }
            }
        }
        coarse.buildSpans();
        coarse.b.assign(coarse.cells() * 4, 0.f);
        coarse.e.assign(coarse.cells() * 4, 0.f);
        coarse.t.assign(coarse.cells() * 4, 0.f);
    }

    class Multigrid {
    public:
        explicit Multigrid(const ParallelFor& parallel)
            : _parallel(parallel)
        {
        }

        Level& fine()
        {
            return _levels.front();
        }

        void build(Level&& fine)
        {
            _levels.clear();
            _levels.push_back(std::move(fine));
            while (true) {
                const Level& last = _levels.back();
                if (((last.nx <= kCoarsestSize) && (last.ny <= kCoarsestSize)) || (last.activeCount <= 1)) {
                    break;
                }
                Level coarse;
                coarsen(last, coarse);
                _levels.push_back(std::move(coarse));
            }
        }

        // e = M b: one symmetric V-cycle from e = 0. t is scratch on level l.
        void cycle(std::size_t l,
                   const float* b,
                   float* e,
                   float* t)
        {
            const Level& level = _levels[l];
            forSpans(level, _parallel, [&](const Span& span) {
                std::fill(e + 4 * span.start, e + 4 * (span.start + (std::size_t)span.count), 0.f);
            });
            if (l + 1 == _levels.size()) {
                // An odd palindrome of sweeps, so that the cycle stays symmetric.
                for (int i = 0; i < kCoarsestSweeps; ++i) {
                    relax(level, b, e, 0, _parallel);
                    relax(level, b, e, 1, _parallel);
                }
                relax(level, b, e, 0, _parallel);

                return;
            }
            for (int i = 0; i < kSmoothPairs; ++i) {
                relax(level, b, e, 0, _parallel);
                relax(level, b, e, 1, _parallel);
            }
            // The sweep of colour 1 has just zeroed its cells' residual.
            const std::size_t s = level.stride;
            const float* const diag = level.diag.data();
            const float* const east = level.east.data();
            const float* const south = level.south.data();
            forSpans(level, _parallel, [&](const Span& span) {
                const int first = colourStart(span, 0);
                for (int j = 0; j < span.count; ++j) {
                    const std::size_t k = span.start + (std::size_t)j;
                    float* const tk = t + 4 * k;
                    if (((j - first) & 1) != 0) {
                        std::fill(tk, tk + 4, 0.f);
                        continue;
                    }
                    float ae[4];
                    applyAt(diag, east, south, s, e, k, ae);
                    for (int c = 0; c < 4; ++c) {
                        tk[c] = b[4 * k + c] - ae[c];
                    }
                }
            });
            Level& coarse = _levels[l + 1];
            float* const cb = coarse.b.data();
            forSpans(coarse, _parallel, [&](const Span& span) {
                for (int j = 0; j < span.count; ++j) {
                    const std::size_t kc = span.start + (std::size_t)j;
                    const std::size_t k00 = level.index(2 * (span.x0 + j), 2 * span.y);
                    const std::size_t k10 = k00 + s;
                    for (int c = 0; c < 4; ++c) {
                        cb[4 * kc + c] = t[4 * k00 + c] + t[4 * (k00 + 1) + c] + t[4 * k10 + c] + t[4 * (k10 + 1) + c];
                    }
                }
            });
            cycle(l + 1, coarse.b.data(), coarse.e.data(), coarse.t.data());
            const float* const ce = coarse.e.data();
            forSpans(level, _parallel, [&](const Span& span) {
                const std::size_t rowc = coarse.index(0, span.y / 2);
                for (int j = 0; j < span.count; ++j) {
                    const std::size_t k = span.start + (std::size_t)j;
                    const std::size_t kc = rowc + (std::size_t)((span.x0 + j) / 2);
                    for (int c = 0; c < 4; ++c) {
                        e[4 * k + c] += kCoarseWeight * ce[4 * kc + c];
                    }
                }
            });
            for (int i = 0; i < kSmoothPairs; ++i) {
                relax(level, b, e, 1, _parallel);
                relax(level, b, e, 0, _parallel);
            }
        }

    private:
        const ParallelFor& _parallel;
        std::vector<Level> _levels;
    };

    // What poissonHeal() and poissonHealMultigrid() share about non-finite samples: those of dest
    // and src are set aside and their dest set to the finite src, so that their difference is 0.
    class HealInput {
    public:
        HealInput(const float* src,
                  float* dest,
                  const float* mask,
                  std::size_t pixels)
            : _dest(dest)
        {
            const std::size_t samples = 4 * pixels;
            inMask.resize(pixels);
            for (std::size_t k = 0; k < pixels; ++k) {
                inMask[k] = std::isfinite(mask[k]) && (mask[k] != 0.f);
            }
            finiteSrc = finiteView(src, samples, _srcStorage);
            for (std::size_t k = 0; k < samples; ++k) {
                if (!std::isfinite(dest[k]) || !std::isfinite(src[k])) {
                    _keptIndex.push_back(k);
                    _keptValue.push_back(dest[k]);
                    dest[k] = finiteSrc[k];
                }
            }
        }

        void restore()
        {
            for (std::size_t i = 0; i < _keptIndex.size(); ++i) {
                _dest[_keptIndex[i]] = _keptValue[i];
            }
        }

        std::vector<unsigned char> inMask;
        const float* finiteSrc = nullptr;

    private:
        float* _dest;
        std::vector<float> _srcStorage;
        std::vector<std::size_t> _keptIndex;
        std::vector<float> _keptValue;
    };
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
    HealInput input(src, dest, mask, w * h);

    const std::size_t subwidth = 4 * ((w + 1) / 2);
    std::vector<float> red(subwidth * (h + 2));
    std::vector<float> black(subwidth * (h + 2));

    healSub(dest, input.finiteSrc, red.data(), black.data(), w, h, parallel);
    healLaplaceLoop(red.data(), black.data(), w, h, input.inMask.data(), maxIter, parallel);
    healAdd(red.data(), black.data(), input.finiteSrc, dest, w, h, parallel);
    input.restore();
}

int
poissonHealMultigrid(const float* src,
                     float* dest,
                     const float* mask,
                     int width,
                     int height,
                     int maxCycles,
                     const ParallelFor& parallel,
                     const float* guess)
{
    if ((width <= 0) || (height <= 0)) {
        return 0;
    }
    const std::size_t pixels = (std::size_t)width * (std::size_t)height;
    HealInput input(src, dest, mask, pixels);

    Level fine;
    fine.allocate(width, height);
    const std::size_t s = fine.stride;
    // dest - src over the region and 0 in the border cells: the unknowns inside the mask, the
    // fixed boundary values outside it.
    std::vector<float> u(fine.cells() * 4, 0.f);
    forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t p = (std::size_t)y * width + x;
                const std::size_t k = fine.index(x, y);
                const bool masked = input.inMask[p];
                const float* g = (masked && guess) ? guess + 4 * p : nullptr;
                for (int c = 0; c < 4; ++c) {
                    const float d = dest[4 * p + c] - input.finiteSrc[4 * p + c];
                    u[4 * k + c] = (g && std::isfinite(g[c])) ? g[c] : d;
                }
                fine.active[k] = masked;
                fine.diag[k] = (float)((x > 0) + (x + 1 < width) + (y > 0) + (y + 1 < height));
            }
        }
    });
    forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t k = fine.index(x, y);
                if (fine.active[k]) {
                    fine.east[k] = fine.active[k + 1] ? 1.f : 0.f;
                    fine.south[k] = fine.active[k + s] ? 1.f : 0.f;
                }
            }
        }
    });
    fine.buildSpans();

    int cycles = 0;
    Multigrid mg(parallel);
    const bool any = fine.activeCount > 0;
    mg.build(std::move(fine));
    const Level& f = mg.fine();
    if (any) {
        const std::size_t n = f.cells() * 4;
        std::vector<float> r(n, 0.f);
        std::vector<float> z(n, 0.f);
        std::vector<float> p(n, 0.f);
        std::vector<float> q(n, 0.f);
        const unsigned char* const active = f.active.data();
        const float* const diag = f.diag.data();
        const float* const east = f.east.data();
        const float* const south = f.south.data();

        // r = the boundary terms - A u: u's plain Laplacian, as its border cells are 0. The
        // boundary terms alone measure the tolerance.
        double scale2[4];
        sumSpans(f, parallel, scale2, [&](const Span& span, double* sums) {
            for (int j = 0; j < span.count; ++j) {
                const std::size_t k = span.start + (std::size_t)j;
                const std::size_t nbr[4] = { k - 1, k + 1, k - s, k + s };
                for (int c = 0; c < 4; ++c) {
                    float boundary = 0.f;
                    float sum = 0.f;
                    for (std::size_t m : nbr) {
                        sum += u[4 * m + c];
                        boundary += active[m] ? 0.f : u[4 * m + c];
                    }
                    r[4 * k + c] = sum - diag[k] * u[4 * k + c];
                    sums[c] += (double)boundary * boundary;
                }
            }
        });
        double r2[4];
        sumSpans(f, parallel, r2, [&](const Span& span, double* sums) {
            for (std::size_t i = 4 * span.start; i < 4 * (span.start + (std::size_t)span.count); ++i) {
                sums[i & 3] += (double)r[i] * r[i];
            }
        });
        double target2[4];
        bool done[4];
        bool allDone = true;
        for (int c = 0; c < 4; ++c) {
            target2[c] = kMultigridTolerance * kMultigridTolerance * std::max(scale2[c], r2[c]);
            done[c] = r2[c] <= target2[c];
            allDone = allDone && done[c];
        }

        double rz[4] = { 0., 0., 0., 0. };
        if (!allDone && (maxCycles > 0)) {
            mg.cycle(0, r.data(), z.data(), q.data());
            sumSpans(f, parallel, rz, [&](const Span& span, double* sums) {
                for (std::size_t i = 4 * span.start; i < 4 * (span.start + (std::size_t)span.count); ++i) {
                    p[i] = z[i];
                    sums[i & 3] += (double)r[i] * z[i];
                }
            });
        }
        while (!allDone && (cycles < maxCycles)) {
            double pq[4];
            sumSpans(f, parallel, pq, [&](const Span& span, double* sums) {
                for (int j = 0; j < span.count; ++j) {
                    const std::size_t k = span.start + (std::size_t)j;
                    float ap[4];
                    applyAt(diag, east, south, s, p.data(), k, ap);
                    for (int c = 0; c < 4; ++c) {
                        q[4 * k + c] = ap[c];
                        sums[c] += (double)p[4 * k + c] * ap[c];
                    }
                }
            });
            float alpha[4];
            for (int c = 0; c < 4; ++c) {
                const bool usable = !done[c] && (pq[c] > 0.) && (rz[c] > 0.);
                alpha[c] = usable ? (float)(rz[c] / pq[c]) : 0.f;
                done[c] = done[c] || !usable;
            }
            sumSpans(f, parallel, r2, [&](const Span& span, double* sums) {
                for (std::size_t i = 4 * span.start; i < 4 * (span.start + (std::size_t)span.count); ++i) {
                    u[i] += alpha[i & 3] * p[i];
                    r[i] -= alpha[i & 3] * q[i];
                    sums[i & 3] += (double)r[i] * r[i];
                }
            });
            ++cycles;
            allDone = true;
            for (int c = 0; c < 4; ++c) {
                done[c] = done[c] || (r2[c] <= target2[c]);
                allDone = allDone && done[c];
            }
            if (allDone || (cycles >= maxCycles)) {
                break;
            }
            mg.cycle(0, r.data(), z.data(), q.data());
            double rzNew[4];
            sumSpans(f, parallel, rzNew, [&](const Span& span, double* sums) {
                for (std::size_t i = 4 * span.start; i < 4 * (span.start + (std::size_t)span.count); ++i) {
                    sums[i & 3] += (double)r[i] * z[i];
                }
            });
            float beta[4];
            for (int c = 0; c < 4; ++c) {
                beta[c] = (!done[c] && (rz[c] > 0.)) ? (float)(rzNew[c] / rz[c]) : 0.f;
                rz[c] = rzNew[c];
            }
            forSpans(f, parallel, [&](const Span& span) {
                for (std::size_t i = 4 * span.start; i < 4 * (span.start + (std::size_t)span.count); ++i) {
                    p[i] = z[i] + beta[i & 3] * p[i];
                }
            });
        }
    }

    forSpans(f, parallel, [&](const Span& span) {
        const std::size_t p = (std::size_t)span.y * width + (std::size_t)span.x0;
        for (int j = 0; j < span.count; ++j) {
            for (int c = 0; c < 4; ++c) {
                dest[4 * (p + j) + c] = u[4 * (span.start + (std::size_t)j) + c] + input.finiteSrc[4 * (p + j) + c];
            }
        }
    });
    input.restore();

    return cycles;
} // poissonHealMultigrid

double
poissonHealResidual(const float* src,
                    const float* dest,
                    const float* mask,
                    int width,
                    int height)
{
    double sum = 0.;
    const auto diff = [&](int x, int y, int c) -> double {
        const std::size_t i = 4 * ((std::size_t)y * width + x) + c;
        const double d = (double)dest[i] - (double)src[i];

        return std::isfinite(d) ? d : 0.;
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float m = mask[(std::size_t)y * width + x];
            if (!std::isfinite(m) || (m == 0.f)) {
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                double lap = 0.;
                const double centre = diff(x, y, c);
                if (x > 0) {
                    lap += diff(x - 1, y, c) - centre;
                }
                if (x + 1 < width) {
                    lap += diff(x + 1, y, c) - centre;
                }
                if (y > 0) {
                    lap += diff(x, y - 1, c) - centre;
                }
                if (y + 1 < height) {
                    lap += diff(x, y + 1, c) - centre;
                }
                sum += lap * lap;
            }
        }
    }

    return std::sqrt(sum);
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT
