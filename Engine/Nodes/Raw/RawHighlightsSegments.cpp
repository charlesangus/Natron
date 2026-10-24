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
 * src/iop/hlreconstruct/segbased.c, with the helpers it calls from src/common/box_filters.cc,
 * src/common/gaussian.c, src/common/distance_transform.c, src/common/math.h and
 * src/develop/noise_generator.h. The algorithm was developed by Iain and garagecoder of the G'MIC
 * team with Hanno Schwalm. The distance transform follows Pedro F. Felzenszwalb and Daniel P.
 * Huttenlocher, "Distance Transforms of Sampled Functions", (C) 2006 Pedro Felzenszwalb, GPL v2
 * or later.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2022-2026 darktable developers.
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

#include "RawSegmentation.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    const int kRgbPlanes = 3;
    const int kSegmentPlanes = 4;
    const int kFloatPlanes = 8;
    const int kBorder = 8;
    const float kClipMagicSegments = 0.987f;
    const float kDistanceMax = 1e20f;
    const float k2PiF = 6.28318530717958647693f;
    const int kNumRecoveryModes = 7;

    inline float
    sqrf(float a)
    {
        return a * a;
    }

    inline float
    fcube(float a)
    {
        return a * a * a;
    }

    inline std::size_t
    roundSize(std::size_t size,
              std::size_t alignment)
    {
        return ((size % alignment) == 0) ? size : ((size - 1) / alignment + 1) * alignment;
    }

    struct Mosaic {
        const float* base;
        std::ptrdiff_t rowStride;
        std::ptrdiff_t pixelStride;

        float at(int row,
                 int col) const
        {
            return base[(std::ptrdiff_t)row * rowStride + (std::ptrdiff_t)col * pixelStride];
        }
    };

    inline float
    localStdDeviation(const float* p,
                      int w)
    {
        const int w2 = 2 * w;
        const float av = (p[-w2 - 1] + p[-w2] + p[-w2 + 1] + p[-w - 2] + p[-w - 1] + p[-w] + p[-w + 1] + p[-w + 2] + p[-2] + p[-1] + p[0] + p[1] + p[2] + p[w - 2] + p[w - 1] + p[w] + p[w + 1] + p[w + 2] + p[w2 - 1] + p[w2] + p[w2 + 1]) / 21.0f;
        return std::sqrt(
            (sqrf(p[-w2 - 1] - av) + sqrf(p[-w2] - av) + sqrf(p[-w2 + 1] - av) + sqrf(p[-w - 2] - av) + sqrf(p[-w - 1] - av) + sqrf(p[-w] - av) + sqrf(p[-w + 1] - av) + sqrf(p[-w + 2] - av) + sqrf(p[-2] - av) + sqrf(p[-1] - av) + sqrf(p[0] - av) + sqrf(p[1] - av) + sqrf(p[2] - av) + sqrf(p[w - 2] - av) + sqrf(p[w - 1] - av) + sqrf(p[w] - av) + sqrf(p[w + 1] - av) + sqrf(p[w + 2] - av) + sqrf(p[w2 - 1] - av) + sqrf(p[w2] - av) + sqrf(p[w2 + 1] - av)) / 21.0f);
    }

    float
    calcWeight(const float* s,
               std::size_t loc,
               int w,
               float clipval)
    {
        const float smoothness = std::fmax(0.0f, 1.0f - 10.0f * std::sqrt(localStdDeviation(&s[loc], w)));
        float val = 0.0f;
        for (int y = -1; y < 2; ++y) {
            for (int x = -1; x < 2; ++x) {
                val += s[loc + (std::ptrdiff_t)y * w + x] / 9.0f;
            }
        }
        const float sval = std::fmax(1.0f, std::pow(std::fmin(clipval, val) / clipval, 2.0f));

        return sval * smoothness;
    }

    void
    calcPlaneCandidates(const float* plane,
                        const float* refavg,
                        Segmentation* seg,
                        float clipval,
                        float badlevel,
                        const ParallelFor& parallel)
    {
        parallel(seg->nr - 2, [&](int i) {
            const std::uint32_t id = 2 + (std::uint32_t)i;
            seg->val1[id] = 0.0f;
            seg->val2[id] = 0.0f;
            // Very small segments get no candidate.
            if (!((seg->ymax[id] - seg->ymin[id] > 2) && (seg->xmax[id] - seg->xmin[id] > 2))) {
                return;
            }
            std::size_t testref = 0;
            float testweight = 0.0f;
            // The rows and columns kept clear of the border never see duplicated border data.
            for (int row = std::max(seg->border + 2, seg->ymin[id] - 2); row < std::min(seg->height - seg->border - 2, seg->ymax[id] + 3); ++row) {
                for (int col = std::max(seg->border + 2, seg->xmin[id] - 2); col < std::min(seg->width - seg->border - 2, seg->xmax[id] + 3); ++col) {
                    const std::size_t pos = (std::size_t)row * seg->width + col;
                    const std::uint32_t sid = seg->segmentId(pos);
                    if ((sid == id) && (plane[pos] < clipval)) {
                        const float wht = calcWeight(plane, pos, seg->width, clipval) * ((seg->data[pos] & kSegmentIdMask) ? 1.0f : 0.75f);
                        if (wht > testweight) {
                            testweight = wht;
                            testref = pos;
                        }
                    }
                }
            }
            if (!testref || !(testweight > 1.0f - badlevel)) {
                return;
            }
            float sum = 0.0f;
            float pix = 0.0f;
            const float weights[5][5] = { { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f },
                                          { 4.0f, 16.0f, 24.0f, 16.0f, 4.0f },
                                          { 6.0f, 24.0f, 36.0f, 24.0f, 6.0f },
                                          { 4.0f, 16.0f, 24.0f, 16.0f, 4.0f },
                                          { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f } };
            for (int y = -2; y < 3; ++y) {
                for (int x = -2; x < 3; ++x) {
                    const std::size_t pos = testref + (std::ptrdiff_t)y * seg->width + x;
                    const bool unclipped = plane[pos] < clipval;
                    sum += unclipped ? plane[pos] * weights[y + 2][x + 2] : 0.0f;
                    pix += unclipped ? weights[y + 2][x + 2] : 0.0f;
                }
            }
            const float av = sum / std::fmax(1.0f, pix);
            if (av > 0.125f * clipval) {
                seg->val1[id] = std::fmin(clipval, av);
                seg->val2[id] = refavg[testref];
            }
        });
    } // calcPlaneCandidates

    // segbased.c's _calc_refavg() in cube-root space, with no late correction.
    float
    calcRefavgCroot(const Mosaic& in,
                    std::uint32_t filters,
                    const std::uint8_t (*xtrans)[6],
                    int width,
                    int height,
                    int row,
                    int col)
    {
        const int color = fcol(row, col, filters, xtrans);
        float mean[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float cnt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        const int dymin = std::max(0, row - 1);
        const int dxmin = std::max(0, col - 1);
        const int dymax = std::min(height - 1, row + 2);
        const int dxmax = std::min(width - 1, col + 2);

        for (int dy = dymin; dy < dymax; ++dy) {
            for (int dx = dxmin; dx < dxmax; ++dx) {
                const float val = std::fmax(0.0f, in.at(dy, dx));
                const int c = fcol(dy, dx, filters, xtrans);
                mean[c] += val;
                cnt[c] += 1.0f;
            }
        }
        for (int c = 0; c < 3; ++c) {
            mean[c] = (cnt[c] > 0.0f) ? std::cbrt((1.0f * mean[c]) / cnt[c]) : 0.0f;
        }

        const float crootRefavg[3] = { 0.5f * (mean[1] + mean[2]),
                                       0.5f * (mean[0] + mean[2]),
                                       0.5f * (mean[0] + mean[1]) };

        return crootRefavg[color];
    }

    // darktable's scharr_gradient() as its release builds compile it: with -ffast-math,
    // dt_fast_hypotf() is the plain square root of the sum of squares.
    inline float
    scharrGradient(const float* p,
                   int w)
    {
        const float gx = 47.0f / 255.0f * (p[-w - 1] - p[-w + 1] + p[w - 1] - p[w + 1])
            + 162.0f / 255.0f * (p[-1] - p[1]);
        const float gy = 47.0f / 255.0f * (p[-w - 1] - p[w - 1] + p[-w + 1] - p[w + 1])
            + 162.0f / 255.0f * (p[-w] - p[w]);

        return std::sqrt(gx * gx + gy * gy);
    }

    void
    gaussCoeffs9x9(float* coeffs,
                   float sigma)
    {
        float kernel[9][9];
        // Locations further than 4.5 from the centre are left out.
        const float range = 4.5f * 4.5f;
        const float temp = -2.0f * sigma * sigma;
        float sum = 0.0;
        for (int k = -4; k < 5; ++k) {
            for (int j = -4; j < 5; ++j) {
                const float rad = (float)(k * k + j * j);
                if (rad <= range) {
                    kernel[k + 4][j + 4] = std::exp(rad / temp);
                    sum += kernel[k + 4][j + 4];
                } else {
                    kernel[k + 4][j + 4] = 0.0f;
                }
            }
        }
        for (int k = 0; k < 5; ++k) {
            for (int j = 0; j < 5; ++j) {
                coeffs[5 * k + j] = kernel[k + 4][j + 4] / sum;
            }
        }
    }

    inline float
    clampf(float a,
           float mn,
           float mx)
    {
        return (a >= mn) ? ((a <= mx) ? a : mx) : mn;
    }

    // dt_gaussian_fast_blur() on one channel, from in to a different out.
    void
    gaussianFastBlur(const float* in,
                     float* out,
                     int width,
                     int height,
                     float sigma,
                     float min,
                     float max,
                     const ParallelFor& parallel)
    {
        float kern[25];
        gaussCoeffs9x9(kern, sigma);
        const int w1 = width;
        const int w2 = 2 * width;
        const int w3 = 3 * width;
        const int w4 = 4 * width;
        const int h0 = 0;
        const int h1 = 1;
        const int h2 = 2;
        const int h3 = 3;
        const int h4 = 4;

        parallel(height, [&](int row) {
            for (int col = 0; col < width; ++col) {
                const std::size_t i = (std::size_t)row * width + col;
                float val = 0.0f;
                if ((col >= 4) && (row >= 4) && (col < width - 4) && (row < height - 4)) {
                    val = kern[10 + 4] * (in[i - w4 - h2] + in[i - w4 + h2] + in[i - w2 - h4] + in[i - w2 + h4] + in[i + w2 - h4] + in[i + w2 + h4] + in[i + w4 - h2] + in[i + w4 + h2]) + kern[5 + 4] * (in[i - w4 - h1] + in[i - w4 + h1] + in[i - w1 - h4] + in[i - w1 + h4] + in[i + w1 - h4] + in[i + w1 + h4] + in[i + w4 - h1] + in[i + w4 + h1]) + kern[4] * (in[i - w4 + h0] + in[i - h4] + in[i + h4] + in[i + w4 + h0]) + kern[15 + 3] * (in[i - w3 - h3] + in[i - w3 + h3] + in[i + w3 - h3] + in[i + w3 + h3]) + kern[10 + 3] * (in[i - w3 - h2] + in[i - w3 + h2] + in[i - w2 - h3] + in[i - w2 + h3] + in[i + w2 - h3] + in[i + w2 + h3] + in[i + w3 - h2] + in[i + w3 + h2]) + kern[5 + 3] * (in[i - w3 - h1] + in[i - w3 + h1] + in[i - w1 - h3] + in[i - w1 + h3] + in[i + w1 - h3] + in[i + w1 + h3] + in[i + w3 - h1] + in[i + w3 + h1]) + kern[3] * (in[i - w3 + h0] + in[i - h3] + in[i + h3] + in[i + w3 + h0]) + kern[10 + 2] * (in[i - w2 - h2] + in[i - w2 + h2] + in[i + w2 - h2] + in[i + w2 + h2]) + kern[5 + 2] * (in[i - w2 - h1] + in[i - w2 + h1] + in[i - w1 - h2] + in[i - w1 + h2] + in[i + w1 - h2] + in[i + w1 + h2] + in[i + w2 - h1] + in[i + w2 + h1]) + kern[2] * (in[i - w2 + h0] + in[i - h2] + in[i + h2] + in[i + w2 + h0]) + kern[5 + 1] * (in[i - w1 - h1] + in[i - w1 + h1] + in[i + w1 - h1] + in[i + w1 + h1]) + kern[1] * (in[i - w1 + h0] + in[i - h1] + in[i + h1] + in[i + w1 + h0]) + kern[0] * (in[i + h0]);
                } else {
                    for (int ir = -4; ir <= 4; ++ir) {
                        const int irow = row + ir;
                        if ((irow >= 0) && (irow < height)) {
                            for (int ic = -4; ic <= 4; ++ic) {
                                const int icol = col + ic;
                                if ((icol >= 0) && (icol < width)) {
                                    val += kern[5 * std::abs(ir) + std::abs(ic)] * in[(std::size_t)irow * width + icol];
                                }
                            }
                        }
                    }
                }
                out[i] = clampf(val, min, max);
            }
        });
    } // gaussianFastBlur

    // One pass of darktable's running box mean along n values stride apart; scratch keeps the
    // values before they are overwritten.
    void
    boxBlurLine(float* buf,
                std::size_t n,
                std::size_t stride,
                std::size_t radius,
                std::vector<float>* scratchBuffer)
    {
        std::vector<float>& scratch = *scratchBuffer;
        scratch.resize(n);
        float L = 0.0f;
        std::size_t hits = 0;
        for (std::size_t x = 0; x < std::min(radius, n); ++x) {
            ++hits;
            scratch[x] = buf[x * stride];
            L += scratch[x];
        }
        std::size_t x;
        for (x = 0; (x <= radius) && ((x + radius) < n); ++x) {
            const std::size_t np = x + radius;
            ++hits;
            scratch[np] = buf[np * stride];
            L += scratch[np];
            buf[x * stride] = L / (float)hits;
        }
        for (; (x <= radius) && (x < n); ++x) {
            buf[x * stride] = L / (float)hits;
        }
        for (; x + radius < n; ++x) {
            const std::size_t op = x - radius - 1;
            const std::size_t np = x + radius;
            L -= scratch[op];
            scratch[np] = buf[np * stride];
            L += scratch[np];
            buf[x * stride] = L / (float)hits;
        }
        for (; x < n; ++x) {
            const std::size_t op = x - radius - 1;
            --hits;
            L -= scratch[op];
            buf[x * stride] = L / (float)hits;
        }
    }

    // dt_box_mean() on one channel, in place.
    void
    boxMean(float* buf,
            std::size_t height,
            std::size_t width,
            std::size_t radius,
            int iterations,
            const ParallelFor& parallel)
    {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            parallel((int)height, [&](int row) {
                std::vector<float> scratch;
                boxBlurLine(buf + (std::size_t)row * width, width, 1, radius, &scratch);
            });
            parallel((int)width, [&](int col) {
                std::vector<float> scratch;
                boxBlurLine(buf + col, height, width, radius, &scratch);
            });
        }
    }

    void
    distanceTransformLine(const float* f,
                          float* z,
                          float* d,
                          int* v,
                          int n)
    {
        int k = 0;
        v[0] = 0;
        z[0] = -kDistanceMax;
        z[1] = kDistanceMax;
        for (int q = 1; q <= n - 1; ++q) {
            float s = (f[q] + sqrf((float)q)) - (f[v[k]] + sqrf((float)v[k]));
            while (s <= z[k] * (float)(2 * q - 2 * v[k])) {
                --k;
                s = (f[q] + sqrf((float)q)) - (f[v[k]] + sqrf((float)v[k]));
            }
            s /= (float)(2 * q - 2 * v[k]);
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = kDistanceMax;
        }

        k = 0;
        for (int q = 0; q <= n - 1; ++q) {
            while (z[k + 1] < (float)q) {
                ++k;
            }
            d[q] = sqrf((float)(q - v[k])) + f[v[k]];
        }
    }

    // dt_image_distance_transform() on an out prepared with 0 and kDistanceMax; returns the
    // largest distance.
    float
    distanceTransform(float* out,
                      std::size_t width,
                      std::size_t height,
                      const ParallelFor& parallel)
    {
        const std::size_t maxdim = std::max(width, height);

        parallel((int)width, [&](int x) {
            std::vector<float> f(maxdim);
            std::vector<float> z(maxdim + 1);
            std::vector<float> d(maxdim);
            std::vector<int> v(maxdim);
            for (std::size_t y = 0; y < height; ++y) {
                f[y] = out[y * width + x];
            }
            distanceTransformLine(f.data(), z.data(), d.data(), v.data(), (int)height);
            for (std::size_t y = 0; y < height; ++y) {
                out[y * width + x] = d[y];
            }
        });
        std::vector<float> rowMax(height, 0.0f);
        parallel((int)height, [&](int y) {
            std::vector<float> z(maxdim + 1);
            std::vector<float> d(maxdim);
            std::vector<int> v(maxdim);
            float* row = out + (std::size_t)y * width;
            distanceTransformLine(row, z.data(), d.data(), v.data(), (int)width);
            for (std::size_t x = 0; x < width; ++x) {
                const float val = std::sqrt(d[x]);
                row[x] = val;
                rowMax[y] = std::fmax(rowMax[y], val);
            }
        });
        float maxDistance = 0.0f;
        for (float m : rowMax) {
            maxDistance = std::fmax(maxDistance, m);
        }

        return maxDistance;
    } // distanceTransform

    inline std::uint32_t
    splitmix32(std::uint64_t seed)
    {
        std::uint64_t result = (seed ^ (seed >> 33)) * 0x62a9d9ed799705f5ull;
        result = (result ^ (result >> 28)) * 0xcb24d0a5c88c35b3ull;

        return (std::uint32_t)(result >> 32);
    }

    inline std::uint32_t
    rol32(std::uint32_t x,
          int k)
    {
        return (x << k) | (x >> (32 - k));
    }

    inline float
    xoshiro128plus(std::uint32_t state[4])
    {
        const std::uint32_t result = state[0] + state[3];
        const std::uint32_t t = state[1] << 9;

        state[2] ^= state[0];
        state[3] ^= state[1];
        state[1] ^= state[2];
        state[0] ^= state[3];

        state[2] ^= t;
        state[3] = rol32(state[3], 11);

        // The top 24 bits make the mantissa.
        return (float)(result >> 8) * 0x1.0p-24f;
    }

    // Gaussian noise through the Anscombe transform.
    inline float
    poissonNoise(float mu,
                 float sigma,
                 int flip,
                 std::uint32_t state[4])
    {
        const float u1 = std::fmax(xoshiro128plus(state), FLT_MIN);
        const float u2 = xoshiro128plus(state);
        const float noise = flip ? std::sqrt(-2.0f * std::log(u1)) * std::cos(k2PiF * u2)
                                 : std::sqrt(-2.0f * std::log(u1)) * std::sin(k2PiF * u2);
        const float r = noise * sigma + 2.0f * std::sqrt(std::fmax(mu + 3.f / 8.f, 0.0f));

        return (r * r - sigma * sigma) / 4.f - 3.f / 8.f;
    }

    void
    initialGradients(std::size_t w,
                     std::size_t height,
                     const float* luminance,
                     const float* distance,
                     float* gradient,
                     const ParallelFor& parallel)
    {
        parallel((int)height - 2 * (kBorder + 2), [&](int r) {
            const std::size_t row = (std::size_t)(kBorder + 2 + r);
            for (std::size_t col = kBorder + 2; col < w - kBorder - 2; ++col) {
                const std::size_t v = row * w + col;
                float g = 0.0f;
                if ((distance[v] > 0.0f) && (distance[v] < 2.0f)) {
                    g = 4.0f * scharrGradient(&luminance[v], (int)w);
                }
                gradient[v] = g;
            }
        });
    }

    float
    segmentMaxdistance(const float* distance,
                       const Segmentation& seg,
                       std::uint32_t id)
    {
        const int xmin = std::max(seg.xmin[id] - 2, seg.border);
        const int xmax = std::min(seg.xmax[id] + 3, seg.width - seg.border);
        const int ymin = std::max(seg.ymin[id] - 2, seg.border);
        const int ymax = std::min(seg.ymax[id] + 3, seg.height - seg.border);
        float maxDistance = 0.0f;

        for (int row = ymin; row < ymax; ++row) {
            for (int col = xmin; col < xmax; ++col) {
                const std::size_t v = (std::size_t)row * seg.width + col;
                if (id == seg.data[v]) {
                    maxDistance = std::fmax(maxDistance, distance[v]);
                }
            }
        }

        return maxDistance;
    }

    float
    segmentAttenuation(const Segmentation& seg,
                       std::uint32_t id,
                       int mode)
    {
        const float attenuate[kNumRecoveryModes] = { 0.0f, 1.7f, 1.0f, 1.7f, 1.0f, 1.0f, 1.0f };

        if (mode < eSegmentsRecoveryAdapt) {
            return attenuate[mode];
        }
        const float maxdist = std::fmax(1.0f, seg.val1[id]);

        return std::fmin(1.7f, 0.9f + (3.0f / maxdist));
    }

    float
    segmentCorrection(const Segmentation& seg,
                      std::uint32_t id,
                      int mode,
                      int recoveryClose)
    {
        const float correction = segmentAttenuation(seg, id, mode);

        return correction - 0.1f * (float)recoveryClose;
    }

    void
    calcDistanceRing(int xmin,
                     int xmax,
                     int ymin,
                     int ymax,
                     float* gradient,
                     const float* distance,
                     float attenuate,
                     float dist,
                     const Segmentation& seg,
                     std::uint32_t id,
                     const ParallelFor& parallel)
    {
        // A ring reads only the ring inside it, so its locations are independent of each other.
        parallel(ymax - ymin, [&](int r) {
            const int row = ymin + r;
            for (int col = xmin; col < xmax; ++col) {
                const std::size_t v = (std::size_t)row * seg.width + col;
                const float dv = distance[v];
                if ((dv >= dist) && (dv < dist + 1.5f) && (id == seg.data[v])) {
                    float grd = 0.0f;
                    float cnt = 0.0f;
                    for (int y = -2; y < 3; ++y) {
                        for (int x = -2; x < 3; ++x) {
                            const std::size_t p = v + x + (std::ptrdiff_t)seg.width * y;
                            const float dd = distance[p];
                            if ((dd >= dist - 1.5f) && (dd < dist)) {
                                cnt += 1.0f;
                                grd += gradient[p];
                            }
                        }
                    }
                    if (cnt > 0.0f) {
                        gradient[v] = std::fmin(1.5f, (grd / cnt) * (1.0f + 1.0f / std::pow(distance[v], attenuate)));
                    }
                }
            }
        });
    }

    void
    segmentGradients(const float* distance,
                     float* gradient,
                     float* tmp,
                     int mode,
                     const Segmentation& seg,
                     std::uint32_t id,
                     int recoveryClose,
                     const ParallelFor& parallel)
    {
        const int xmin = std::max(seg.xmin[id] - 1, seg.border);
        const int xmax = std::min(seg.xmax[id] + 2, seg.width - seg.border);
        const int ymin = std::max(seg.ymin[id] - 1, seg.border);
        const int ymax = std::min(seg.ymax[id] + 2, seg.height - seg.border);
        const float attenuate = segmentAttenuation(seg, id, mode);
        const float strength = segmentCorrection(seg, id, mode, recoveryClose);
        const std::size_t boxWidth = (std::size_t)(xmax - xmin);

        float maxdist = 1.5f;
        while (maxdist < seg.val1[id]) {
            calcDistanceRing(xmin, xmax, ymin, ymax, gradient, distance, attenuate, maxdist, seg, id, parallel);
            maxdist += 1.5f;
        }

        if (maxdist > 4.0f) {
            parallel(ymax - ymin, [&](int r) {
                const std::size_t row = (std::size_t)(ymin + r);
                const std::size_t s = row * seg.width + xmin;
                const std::size_t d = (std::size_t)r * boxWidth;
                std::copy(gradient + s, gradient + s + boxWidth, tmp + d);
            });
            boxMean(tmp, (std::size_t)(ymax - ymin), boxWidth, (std::size_t)std::min((int)maxdist, 15), 2, parallel);
            parallel(ymax - ymin, [&](int r) {
                const std::size_t row = (std::size_t)(ymin + r);
                for (std::size_t col = xmin, v = row * seg.width + col, s = (std::size_t)r * boxWidth; col < (std::size_t)xmax; ++col, ++v, ++s) {
                    if (id == seg.data[v]) {
                        gradient[v] = tmp[s];
                    }
                }
            });
        }
        parallel(ymax - ymin, [&](int r) {
            const int row = ymin + r;
            for (int col = xmin; col < xmax; ++col) {
                const std::size_t v = (std::size_t)row * seg.width + col;
                if (id == seg.data[v]) {
                    gradient[v] *= strength;
                }
            }
        });
    } // segmentGradients

    // The generator is seeded by the segment's top-left corner, so a segment's noise depends on
    // where it is and never on threads or tiles.
    void
    addPoissonNoise(float* lum,
                    const Segmentation& seg,
                    std::uint32_t id,
                    float noiseLevel)
    {
        const int xmin = std::max(seg.xmin[id], seg.border);
        const int xmax = std::min(seg.xmax[id] + 1, seg.width - seg.border);
        const int ymin = std::max(seg.ymin[id], seg.border);
        const int ymax = std::min(seg.ymax[id] + 1, seg.height - seg.border);
        std::uint32_t state[4] = { splitmix32((std::uint64_t)(std::int64_t)ymin), splitmix32((std::uint64_t)(std::int64_t)xmin), splitmix32(1337), splitmix32(666) };

        for (int warm = 0; warm < 4; ++warm) {
            (void)xoshiro128plus(state);
        }
        for (int row = ymin; row < ymax; ++row) {
            for (int col = xmin; col < xmax; ++col) {
                const std::size_t v = (std::size_t)row * seg.width + col;
                if (seg.data[v] == id) {
                    lum[v] += poissonNoise(lum[v] * noiseLevel, noiseLevel, col & 1, state);
                }
            }
        }
    }

    inline std::size_t
    rawToPlane(std::size_t width,
               int row,
               int col)
    {
        return (std::size_t)(kBorder + (row / 3)) * width + (std::size_t)(col / 3) + kBorder;
    }

    // Repeats the outermost inner rows and columns over a border of the given width.
    void
    masksExtendBorder(float* mask,
                      std::size_t width,
                      std::size_t height,
                      std::size_t border,
                      const ParallelFor& parallel)
    {
        if (border <= 0) {
            return;
        }
        parallel((int)(height - 2 * border), [&](int r) {
            const std::size_t idx = (border + (std::size_t)r) * width;
            for (std::size_t i = 0; i < border; ++i) {
                mask[idx + i] = mask[idx + border];
                mask[idx + width - i - 1] = mask[idx + width - border - 1];
            }
        });
        parallel((int)width, [&](int c) {
            const std::size_t col = (std::size_t)c;
            const float top = mask[border * width + std::min(width - border - 1, std::max(col, border))];
            const float bot = mask[(height - border - 1) * width + std::min(width - border - 1, std::max(col, border))];
            for (std::size_t i = 0; i < border; ++i) {
                mask[col + i * width] = top;
                mask[col + (height - i - 1) * width] = bot;
            }
        });
    }
} // namespace

void
segmentsHighlights(const float* in,
                   std::ptrdiff_t inRowStride,
                   std::ptrdiff_t inPixelStride,
                   float* out,
                   int width,
                   int height,
                   std::uint32_t filters,
                   const std::uint8_t (*xtrans)[6],
                   const SegmentsParams& params,
                   const ParallelFor& parallel)
{
    if ((width < 3) || (height < 3)) {
        return;
    }
    const Mosaic input = { in, inRowStride, inPixelStride };
    const float clipval = std::max(0.1f, kClipMagicSegments * params.clip);
    const float icoeffs[3] = { params.coeffs[0], params.coeffs[1], params.coeffs[2] };
    const float clips[3] = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2] };
    const float cubeCoeffs[3] = { std::cbrt(clips[0]), std::cbrt(clips[1]), std::cbrt(clips[2]) };

    const int recoveryMode = std::max(0, std::min((int)params.recovery, kNumRecoveryModes - 1));
    const float strength = params.strength;
    const int recoveryClosing[kNumRecoveryModes] = { 0, 0, 0, 2, 2, 0, 2 };
    const int recoveryClose = recoveryClosing[recoveryMode];
    // 250 segments per megapixel.
    const int segmentationLimit = (int)((float)((std::int64_t)width * height) / 4000.f);

    const std::size_t pwidth = roundSize((std::size_t)(width / 3), 2) + 2 * kBorder;
    const std::size_t pheight = roundSize((std::size_t)(height / 3), 2) + 2 * kBorder;
    const std::size_t pSize = roundSize(pwidth * pheight, 64);

    // darktable leaves these uninitialised; zeroing them keeps unwritten borders deterministic.
    std::vector<float> fbuffer((std::size_t)kFloatPlanes * pSize, 0.0f);
    float* plane[kFloatPlanes];
    for (int i = 0; i < kFloatPlanes; ++i) {
        plane[i] = fbuffer.data() + (std::size_t)i * pSize;
    }
    // refavg shares its planes with the recovery's gradient, luminance and recout below, and the
    // recovery reads what is left of refavg around recout's border, as darktable does.
    float* refavg[kRgbPlanes];
    for (int i = 0; i < kRgbPlanes; ++i) {
        refavg[i] = plane[kSegmentPlanes + i];
    }

    Segmentation isegments[kSegmentPlanes];
    for (int i = 0; i < kSegmentPlanes; ++i) {
        isegments[i].init((int)pwidth, (int)pheight, kBorder + 1, segmentationLimit);
    }

    const int xshifter = ((filters != kFiltersXTrans) && (fcBayer(0, 0, filters) == 1)) ? 1 : 2;

    std::vector<int> rowAnyclipped(height, 0);
    std::vector<char> rowAllclipped(height, 0);
    parallel(height - 2, [&](int r) {
        const int row = r + 1;
        if (row % 3 != 1) {
            return;
        }
        for (int col = 1; col < width - 1; ++col) {
            // Each 3x3 box is centred on a green photosite of a Bayer mosaic, so it always holds
            // five greens, two reds and two blues and its chroma noise stays stable.
            if (col % 3 != xshifter) {
                continue;
            }
            float mean[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float cnt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int dy = row - 1; dy < row + 2; ++dy) {
                for (int dx = col - 1; dx < col + 2; ++dx) {
                    const float val = out[(std::size_t)dy * width + dx];
                    const int c = fcol(dy, dx, filters, xtrans);
                    mean[c] += val;
                    cnt[c] += 1.0f;
                }
            }
            for (int c = 0; c < 3; ++c) {
                mean[c] = (cnt[c] > 0.0f) ? std::cbrt(1.0f * mean[c] / cnt[c]) : 0.0f;
            }
            const float cubeRefavg[3] = { 0.5f * (mean[1] + mean[2]),
                                          0.5f * (mean[0] + mean[2]),
                                          0.5f * (mean[0] + mean[1]) };

            const std::size_t o = rawToPlane(pwidth, row, col);
            int allclipped = 0;
            for (int c = 0; c < 3; ++c) {
                plane[c][o] = mean[c];
                refavg[c][o] = cubeRefavg[c];
                if (mean[c] > cubeCoeffs[c]) {
                    allclipped += 1;
                    isegments[c].data[o] = 1;
                }
            }
            isegments[3].data[o] = (allclipped == 3) ? 1 : 0;
            rowAllclipped[row] |= (allclipped == 3) ? 1 : 0;
            rowAnyclipped[row] += allclipped;
        }
    });
    int anyclipped = 0;
    bool hasAllclipped = false;
    for (int row = 0; row < height; ++row) {
        anyclipped += rowAnyclipped[row];
        hasAllclipped |= (rowAllclipped[row] != 0);
    }

    if (anyclipped < 20) {
        return;
    }

    for (int i = 0; i < kRgbPlanes; ++i) {
        masksExtendBorder(plane[i], pwidth, pheight, kBorder, parallel);
    }

    std::vector<std::uint32_t> combineTmp(pwidth * pheight, 0);
    for (int p = 0; p < kRgbPlanes; ++p) {
        combineSegments(&isegments[p], params.combine, combineTmp.data(), parallel);
    }

    parallel(kRgbPlanes, [&](int p) {
        segmentizePlane(&isegments[p]);
    });

    for (int p = 0; p < kRgbPlanes; ++p) {
        calcPlaneCandidates(plane[p], refavg[p], &isegments[p], cubeCoeffs[p], params.candidating, parallel);
    }

    // Every photosite of a 3x3 block writes the block's plane location; a block row runs in
    // order so the last of them wins, as in darktable's serial order.
    parallel((height + 2) / 3, [&](int block) {
        const int rowEnd = std::min(height - 1, 3 * block + 3);
        for (int row = std::max(1, 3 * block); row < rowEnd; ++row) {
            for (int col = 1; col < width - 1; ++col) {
                const std::size_t idx = (std::size_t)row * width + col;
                const float inval = std::fmax(0.0f, input.at(row, col));
                const int color = fcol(row, col, filters, xtrans);
                if (!(inval > clips[color])) {
                    continue;
                }
                const std::size_t o = rawToPlane(pwidth, row, col);
                const Segmentation& seg = isegments[color];
                const std::uint32_t pid = seg.segmentId(o);
                if (!((pid > 1) && (pid < (std::uint32_t)seg.nr))) {
                    continue;
                }
                const float candidate = seg.val1[pid];
                if (candidate != 0.0f) {
                    const float candReference = seg.val2[pid];
                    const float refavgHere = calcRefavgCroot(input, filters, xtrans, width, height, row, col);
                    const float oval = fcube(refavgHere + candidate - candReference);
                    out[idx] = plane[color][o] = std::fmax(inval, oval);
                }
            }
        }
    });

    float* distance = plane[kRgbPlanes];
    float* gradient = plane[kRgbPlanes + 1];
    float* luminance = plane[kRgbPlanes + 2];
    float* recout = plane[kRgbPlanes + 3];
    float* tmp = plane[kRgbPlanes + 4];

    Segmentation* segall = &isegments[3];

    const bool doRecovery = (recoveryMode != eSegmentsRecoveryOff) && hasAllclipped && (strength > 0.0f);
    if (!doRecovery) {
        return;
    }

    combineSegments(segall, recoveryClose, combineTmp.data(), parallel);
    std::fill(gradient, gradient + pwidth * pheight, std::fmin(1.0f, 5.0f * strength));
    std::fill(distance, distance + pwidth * pheight, 0.0f);
    const int sborder = segall->border;
    parallel((int)pheight - 2 * sborder, [&](int r) {
        const std::size_t row = (std::size_t)(sborder + r);
        for (std::size_t col = sborder; col < pwidth - sborder; ++col) {
            const std::size_t i = row * pwidth + col;
            tmp[i] = (plane[0][i] * icoeffs[0] + plane[1][i] * icoeffs[1] + plane[2][i] * icoeffs[2]) / 3.0f;
            distance[i] = (segall->data[i] == 1) ? kDistanceMax : 0.0f;
        }
    });
    masksExtendBorder(tmp, pwidth, pheight, sborder, parallel);
    gaussianFastBlur(tmp, luminance, (int)pwidth, (int)pheight, 1.2f, 0.0f, 20.0f, parallel);

    const float maxDistance = distanceTransform(distance, pwidth, pheight, parallel);
    if (!(maxDistance > 3.0f)) {
        return;
    }
    segmentizePlane(segall);
    initialGradients(pwidth, pheight, luminance, distance, recout, parallel);
    masksExtendBorder(recout, pwidth, pheight, sborder, parallel);

    // Segments run in id order: a segment's box mean reads the gradients earlier segments left.
    for (std::uint32_t id = 2; id < (std::uint32_t)segall->nr; ++id) {
        segall->val1[id] = segmentMaxdistance(distance, *segall, id);
        if (segall->val1[id] > 2.0f) {
            segmentGradients(distance, recout, tmp, recoveryMode, *segall, id, recoveryClose, parallel);
        }
    }

    gaussianFastBlur(recout, gradient, (int)pwidth, (int)pheight, 1.2f, 0.0f, 20.0f, parallel);
    const float noiseLevel = params.noiseLevel;
    if (noiseLevel > 0.0f) {
        for (std::uint32_t id = 2; id < (std::uint32_t)segall->nr; ++id) {
            if (segall->val1[id] > 3.0f) {
                addPoissonNoise(gradient, *segall, id, noiseLevel);
            }
        }
    }

    const float dshift = 2.0f + (float)recoveryClosing[recoveryMode];
    parallel(height - 2, [&](int r) {
        const int row = r + 1;
        for (int col = 1; col < width - 1; ++col) {
            const std::size_t idx = (std::size_t)row * width + col;
            const int color = fcol(row, col, filters, xtrans);
            const float ival = std::fmax(0.0f, input.at(row, col));
            if (ival > clips[color]) {
                const std::size_t o = rawToPlane(pwidth, row, col);
                const float effect = strength / (1.0f + std::exp(-(distance[o] - dshift)));
                out[idx] += std::fmax(0.0f, gradient[o] * effect);
            }
        }
    });
} // segmentsHighlights
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
