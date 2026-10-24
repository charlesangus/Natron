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
 * src/iop/hlreconstruct/laplacian.c, driven as src/iop/highlights.c drives it, with the helpers it
 * calls from src/common/bspline.h, src/common/dwt.h, src/common/box_filters.cc,
 * src/common/fast_guided_filter.h and src/develop/noise_generator.h. The algorithm is Aurélien
 * Pierre's.
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
#include <cfloat>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    const int kRed = 0;
    const int kGreen = 1;
    const int kBlue = 2;
    const int kAlpha = 3;
    const int kMaxNumScales = 12;
    const int kMaskRadius = 2;
    const float kBSplineSigma = 1.0553651328015339f;
    const float kBSplineToLaplacian = 3.182727439285017f;
    const float k2PiF = 6.28318530717958647693f;

    const unsigned int kAnyScale = 1u << 0;
    const unsigned int kFirstScale = 1u << 1;
    const unsigned int kLastScale = 1u << 2;

    enum DiffuseVariantEnum {
        eDiffuseRgb = 0,
        eDiffuseChroma
    };

    /**
     * @brief Photosite (row, col) of the image at base[(row - row0) * rowStride + (col - col0) *
     * pixelStride].
     **/
    struct Photosites {
        const float* base;
        std::ptrdiff_t rowStride;
        std::ptrdiff_t pixelStride;
        int row0;
        int col0;

        float at(int row,
                 int col) const
        {
            return base[(std::ptrdiff_t)(row - row0) * rowStride + (std::ptrdiff_t)(col - col0) * pixelStride];
        }
    };

    inline std::uint32_t
    splitmix32(std::uint64_t seed)
    {
        std::uint64_t result = (seed ^ (seed >> 33)) * 0x62a9d9ed799705f5ull;

        result = (result ^ (result >> 28)) * 0xcb24d0a5c88c35b3ull;

        return (std::uint32_t)(result >> 32);
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
        state[3] = (state[3] << 11) | (state[3] >> 21);

        // The top 24 bits make the mantissa.
        return (float)(result >> 8) * 0x1.0p-24f;
    }

    inline float
    sqf(float a)
    {
        return a * a;
    }

    // darktable's CLIP(), which sends NaN to 0.
    inline float
    clip01(float x)
    {
        return (x >= 0.f) ? ((x <= 1.f) ? x : 1.f) : 0.f;
    }

    inline unsigned int
    scaleType(int s,
              int scales)
    {
        unsigned int scale = kAnyScale;

        if (s == 0) {
            scale |= kFirstScale;
        }
        if (s == scales - 1) {
            scale |= kLastScale;
        }

        return scale;
    }

    float
    equivalentSigmaAtStep(float sigma,
                          unsigned int s)
    {
        float radius = sigma;

        for (unsigned int k = 1; k <= s; ++k) {
            radius = std::sqrt(sqf(radius) + sqf(std::exp2((float)k) * sigma));
        }

        return radius;
    }

    // _interpolate_and_mask() for photosite (i, j): the bilinear RGB and norm divided by the white
    // balance, and which of them read a clipped photosite.
    void
    interpolateAndMask(const Photosites& input,
                       int i,
                       int j,
                       int width,
                       int height,
                       std::uint32_t filters,
                       const LaplacianParams& p,
                       float interpolated[4],
                       float clipped[4])
    {
        const float* const clips = p.clips;
        const int c = fcBayer((std::size_t)i, (std::size_t)j, filters);
        const float center = input.at(i, j);
        float R = 0.f;
        float G = 0.f;
        float B = 0.f;
        int R_clipped = 0;
        int G_clipped = 0;
        int B_clipped = 0;

        if ((i == 0) || (j == 0) || (i == height - 1) || (j == width - 1)) {
            R = G = B = center;
            R_clipped = G_clipped = B_clipped = (center > clips[c]);
        } else {
            const float north = input.at(i - 1, j);
            const float south = input.at(i + 1, j);
            const float west = input.at(i, j - 1);
            const float east = input.at(i, j + 1);

            const float north_east = input.at(i - 1, j + 1);
            const float north_west = input.at(i - 1, j - 1);
            const float south_east = input.at(i + 1, j + 1);
            const float south_west = input.at(i + 1, j - 1);

            const auto fc = [filters](int row, int col) {
                return fcBayer((std::size_t)row, (std::size_t)col, filters);
            };

            if (c == kGreen) {
                G = center;
                G_clipped = (center > clips[kGreen]);
            } else {
                G = (north + south + east + west) / 4.f;
                G_clipped = (north > clips[kGreen] || south > clips[kGreen] || east > clips[kGreen] || west > clips[kGreen]);
            }

            if (c == kRed) {
                R = center;
                R_clipped = (center > clips[kRed]);
            } else if ((fc(i - 1, j) == kRed) && (fc(i + 1, j) == kRed)) {
                R = (north + south) / 2.f;
                R_clipped = (north > clips[kRed] || south > clips[kRed]);
            } else if ((fc(i, j - 1) == kRed) && (fc(i, j + 1) == kRed)) {
                R = (west + east) / 2.f;
                R_clipped = (west > clips[kRed] || east > clips[kRed]);
            } else {
                R = (north_west + north_east + south_east + south_west) / 4.f;
                R_clipped = (north_west > clips[kRed] || north_east > clips[kRed] || south_west > clips[kRed] || south_east > clips[kRed]);
            }

            if (c == kBlue) {
                B = center;
                B_clipped = (center > clips[kBlue]);
            } else if ((fc(i - 1, j) == kBlue) && (fc(i + 1, j) == kBlue)) {
                B = (north + south) / 2.f;
                B_clipped = (north > clips[kBlue] || south > clips[kBlue]);
            } else if ((fc(i, j - 1) == kBlue) && (fc(i, j + 1) == kBlue)) {
                B = (west + east) / 2.f;
                B_clipped = (west > clips[kBlue] || east > clips[kBlue]);
            } else {
                B = (north_west + north_east + south_east + south_west) / 4.f;
                B_clipped = (north_west > clips[kBlue] || north_east > clips[kBlue] || south_west > clips[kBlue] || south_east > clips[kBlue]);
            }
        }

        const float RGB[4] = { R, G, B, std::sqrt(sqf(R) + sqf(G) + sqf(B)) };
        const float flags[4] = { (float)R_clipped, (float)G_clipped, (float)B_clipped, (R_clipped || G_clipped || B_clipped) ? 1.0f : 0.0f };
        const float wb[4] = { p.wb[0], p.wb[1], p.wb[2], 1.f };
        for (int k = 0; k < 4; ++k) {
            interpolated[k] = std::fmax(RGB[k] / wb[k], 0.f);
            clipped[k] = flags[k];
        }
    } // interpolateAndMask

    /**
     * @brief The clipping mask after darktable's dt_box_mean(radius 2, one iteration) for rows
     * [row0, row0 + rows) and columns [col0, col0 + cols) of the image, four floats per photosite.
     * The horizontal means of 0 and 1 are exact; the vertical pass sums its window directly where
     * darktable keeps a running sum down the whole column, so it can differ from darktable by
     * rounding, but no longer depends on the rows above the region.
     **/
    void
    boxMeanMask(const Photosites& input,
                int width,
                int height,
                std::uint32_t filters,
                const LaplacianParams& p,
                int row0,
                int col0,
                int rows,
                int cols,
                float* out)
    {
        const int fr0 = std::max(0, row0 - kMaskRadius);
        const int fr1 = std::min(height, row0 + rows + kMaskRadius);
        const int fc0 = std::max(0, col0 - kMaskRadius);
        const int fc1 = std::min(width, col0 + cols + kMaskRadius);
        const int fw = fc1 - fc0;
        std::vector<float> flags((std::size_t)(fr1 - fr0) * (std::size_t)fw * 4);
        float interpolated[4];

        for (int r = fr0; r < fr1; ++r) {
            for (int c = fc0; c < fc1; ++c) {
                interpolateAndMask(input, r, c, width, height, filters, p, interpolated, &flags[((std::size_t)(r - fr0) * (std::size_t)fw + (std::size_t)(c - fc0)) * 4]);
            }
        }
        std::vector<float> horizontal((std::size_t)(fr1 - fr0) * (std::size_t)cols * 4);
        for (int r = fr0; r < fr1; ++r) {
            for (int c = col0; c < col0 + cols; ++c) {
                const int x0 = std::max(0, c - kMaskRadius);
                const int x1 = std::min(width - 1, c + kMaskRadius);
                float sum[4] = { 0.f, 0.f, 0.f, 0.f };
                for (int x = x0; x <= x1; ++x) {
                    const float* f = &flags[((std::size_t)(r - fr0) * (std::size_t)fw + (std::size_t)(x - fc0)) * 4];
                    for (int k = 0; k < 4; ++k) {
                        sum[k] += f[k];
                    }
                }
                const float hits = (float)(x1 - x0 + 1);
                float* h = &horizontal[((std::size_t)(r - fr0) * (std::size_t)cols + (std::size_t)(c - col0)) * 4];
                for (int k = 0; k < 4; ++k) {
                    h[k] = sum[k] / hits;
                }
            }
        }
        for (int r = row0; r < row0 + rows; ++r) {
            const int y0 = std::max(0, r - kMaskRadius);
            const int y1 = std::min(height - 1, r + kMaskRadius);
            const float hits = (float)(y1 - y0 + 1);
            for (int c = col0; c < col0 + cols; ++c) {
                float sum[4] = { 0.f, 0.f, 0.f, 0.f };
                for (int y = y0; y <= y1; ++y) {
                    const float* h = &horizontal[((std::size_t)(y - fr0) * (std::size_t)cols + (std::size_t)(c - col0)) * 4];
                    for (int k = 0; k < 4; ++k) {
                        sum[k] += h[k];
                    }
                }
                float* o = out + ((std::size_t)(r - row0) * (std::size_t)cols + (std::size_t)(c - col0)) * 4;
                for (int k = 0; k < 4; ++k) {
                    o[k] = sum[k] / hits;
                }
            }
        }
    } // boxMeanMask

    // The neighbours and weights of interpolate_bilinear() for output position i of size out
    // sampled from size in.
    struct BilinearAxis {
        std::size_t prev;
        std::size_t next;
        float dPrev;
        float dNext;

        BilinearAxis(std::size_t i,
                     std::size_t sizeOut,
                     std::size_t sizeIn)
        {
            const float rel = (float)i / (float)sizeOut;
            const float abs = rel * (float)sizeIn;

            prev = (std::size_t)std::floor(abs);
            next = prev + 1;
            prev = (prev < sizeIn) ? prev : sizeIn - 1;
            next = (next < sizeIn) ? next : sizeIn - 1;
            dNext = (float)next - abs;
            dPrev = 1.f - dNext;
        }
    };

    inline float
    bilinear(const BilinearAxis& y,
             const BilinearAxis& x,
             float nw,
             float ne,
             float se,
             float sw)
    {
        return y.dPrev * (sw * x.dNext + se * x.dPrev) + y.dNext * (nw * x.dNext + ne * x.dPrev);
    }

    // _interpolate_and_mask(), dt_box_mean() and the two interpolate_bilinear() downsamplings of
    // process_laplacian_bayer(), evaluated at the photosites the downsampling reads.
    void
    downsample(const Photosites& input,
               int width,
               int height,
               std::uint32_t filters,
               const LaplacianParams& p,
               int dsWidth,
               int dsHeight,
               float* dsInterpolated,
               float* dsMask,
               const ParallelFor& parallel)
    {
        parallel(dsHeight, [&](int i) {
            const BilinearAxis y((std::size_t)i, (std::size_t)dsHeight, (std::size_t)height);
            const int rows = (int)(y.next - y.prev) + 1;
            std::vector<float> mask((std::size_t)rows * (std::size_t)width * 4);
            boxMeanMask(input, width, height, filters, p, (int)y.prev, 0, rows, width, mask.data());
            const float* maskPrev = mask.data();
            const float* maskNext = mask.data() + (std::size_t)(rows - 1) * (std::size_t)width * 4;

            for (int j = 0; j < dsWidth; ++j) {
                const BilinearAxis x((std::size_t)j, (std::size_t)dsWidth, (std::size_t)width);
                float nw[4], ne[4], se[4], sw[4], unused[4];
                interpolateAndMask(input, (int)y.prev, (int)x.prev, width, height, filters, p, nw, unused);
                interpolateAndMask(input, (int)y.prev, (int)x.next, width, height, filters, p, ne, unused);
                interpolateAndMask(input, (int)y.next, (int)x.next, width, height, filters, p, se, unused);
                interpolateAndMask(input, (int)y.next, (int)x.prev, width, height, filters, p, sw, unused);
                float* outInterpolated = dsInterpolated + ((std::size_t)i * (std::size_t)dsWidth + (std::size_t)j) * 4;
                float* outMask = dsMask + ((std::size_t)i * (std::size_t)dsWidth + (std::size_t)j) * 4;
                for (int c = 0; c < 4; ++c) {
                    outInterpolated[c] = bilinear(y, x, nw[c], ne[c], se[c], sw[c]);
                    outMask[c] = bilinear(y, x, maskPrev[x.prev * 4 + c], maskPrev[x.next * 4 + c], maskNext[x.next * 4 + c], maskNext[x.prev * 4 + c]);
                }
            }
        });
    }

    inline void
    sparseScalarProduct(const float* buf,
                        const std::size_t indices[5],
                        float* result)
    {
        static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

        for (int c = 0; c < 4; ++c) {
            const float v = filter[0] * buf[indices[0] + c] + filter[1] * buf[indices[1] + c] + filter[2] * buf[indices[2] + c] + filter[3] * buf[indices[3] + c] + filter[4] * buf[indices[4] + c];
            result[c] = (0.0f > v) ? 0.0f : v;
        }
    }

    // decompose_2D_Bspline(), negatives clipped.
    void
    decompose2DBspline(const float* in,
                       float* HF,
                       float* LF,
                       std::size_t width,
                       std::size_t height,
                       int mult,
                       const ParallelFor& parallel)
    {
        parallel((int)height, [&](int r) {
            const std::size_t i = (std::size_t)r;
            std::vector<float> temp(4 * width);
            const std::size_t rows[5] = { 4 * width * (std::size_t)std::max((int)i - 2 * mult, 0),
                                          4 * width * (std::size_t)std::max((int)i - mult, 0),
                                          4 * width * i,
                                          4 * width * std::min(i + (std::size_t)mult, height - 1),
                                          4 * width * std::min(i + 2 * (std::size_t)mult, height - 1) };
            for (std::size_t j = 0; j < width; ++j) {
                sparseScalarProduct(in + j * 4, rows, &temp[j * 4]);
            }
            for (std::size_t j = 0; j < width; ++j) {
                const std::size_t cols[5] = { 4 * (std::size_t)std::max((int)j - 2 * mult, 0),
                                              4 * (std::size_t)std::max((int)j - mult, 0),
                                              4 * j,
                                              4 * std::min(j + (std::size_t)mult, width - 1),
                                              4 * std::min(j + 2 * (std::size_t)mult, width - 1) };
                const std::size_t index = 4 * (i * width + j);
                sparseScalarProduct(temp.data(), cols, LF + index);
                for (int c = 0; c < 4; ++c) {
                    HF[index + c] = in[index + c] - LF[index + c];
                }
            }
        });
    }

    // The 3x3 neighbours of (i, j) at distance mult, clamped to the image, four floats each.
    inline void
    fetchNeighbours(const float* HF,
                    std::size_t i,
                    std::size_t j,
                    std::size_t width,
                    std::size_t height,
                    int mult,
                    float neighbours[9][4])
    {
        const std::size_t i_neighbours[3] = { (std::size_t)std::max((int)i - mult, 0) * width,
                                              i * width,
                                              (std::size_t)std::min((int)i + mult, (int)height - 1) * width };
        const std::size_t j_neighbours[3] = { (std::size_t)std::max((int)j - mult, 0),
                                              j,
                                              (std::size_t)std::min((int)j + mult, (int)width - 1) };

        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                for (int c = 0; c < 4; ++c) {
                    neighbours[3 * a + b][c] = HF[4 * (i_neighbours[a] + j_neighbours[b]) + c];
                }
            }
        }
    }

    // poisson_noise_simd() for the three colour channels: darktable's fourth channel draws no
    // numbers and is overwritten by the norm right after.
    inline void
    poissonNoise(const float mu[4],
                 const float sigma[4],
                 std::uint32_t state[4],
                 float out[3])
    {
        const int flip[3] = { 1, 0, 1 };
        float u1[3];
        float u2[3];

        for (int c = 0; c < 3; ++c) {
            u1[c] = std::fmax(xoshiro128plus(state), FLT_MIN);
            u2[c] = xoshiro128plus(state);
        }
        for (int c = 0; c < 3; ++c) {
            const float noise = flip[c] ? std::sqrt(-2.0f * std::log(u1[c])) * std::cos(k2PiF * u2[c])
                                        : std::sqrt(-2.0f * std::log(u1[c])) * std::sin(k2PiF * u2[c]);
            const float r = noise * sigma[c] + 2.0f * std::sqrt(std::fmax(mu[c] + 3.f / 8.f, 0.0f));
            out[c] = (r * r - sigma[c] * sigma[c]) / 4.f - 3.f / 8.f;
        }
    }

    void
    guideLaplacians(const float* HF,
                    const float* LF,
                    const float* clipping_mask,
                    float* out,
                    std::size_t width,
                    std::size_t height,
                    int mult,
                    float noise_level,
                    bool salt,
                    unsigned int scale,
                    float radius_sq,
                    const ParallelFor& parallel)
    {
        parallel((int)height, [&](int i) {
            for (std::size_t j = 0; j < width; ++j) {
                const std::size_t idx = (std::size_t)i * width + j;
                const std::size_t index = idx * 4;
                const float alpha = clipping_mask[index + kAlpha];
                const float alpha_comp = 1.f - clipping_mask[index + kAlpha];
                float high_frequency[4] = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

                if (alpha > 0.f) {
                    float neighbour_pixel_HF[9][4];
                    fetchNeighbours(HF, (std::size_t)i, j, width, height, mult, neighbour_pixel_HF);

                    float means_HF[4] = { 0.f, 0.f, 0.f, 0.f };
                    for (int k = 0; k < 9; ++k) {
                        for (int c = 0; c < 4; ++c) {
                            means_HF[c] += neighbour_pixel_HF[k][c] / 9.f;
                        }
                    }
                    float variance_HF[4] = { 0.f, 0.f, 0.f, 0.f };
                    for (int k = 0; k < 9; ++k) {
                        for (int c = 0; c < 4; ++c) {
                            variance_HF[c] += sqf(neighbour_pixel_HF[k][c] - means_HF[c]) / 9.f;
                        }
                    }
                    // The channel most likely to hold details guides the others.
                    int guide = kAlpha;
                    float guiding_value_HF = 0.f;
                    for (int c = 0; c < 3; ++c) {
                        if (variance_HF[c] > guiding_value_HF) {
                            guiding_value_HF = variance_HF[c];
                            guide = c;
                        }
                    }
                    float covariance_HF[4] = { 0.f, 0.f, 0.f, 0.f };
                    for (int k = 0; k < 9; ++k) {
                        for (int c = 0; c < 4; ++c) {
                            covariance_HF[c] += (neighbour_pixel_HF[k][c] - means_HF[c]) * (neighbour_pixel_HF[k][guide] - means_HF[guide]) / 9.f;
                        }
                    }
                    const float scale_multiplier = 1.f / radius_sq;
                    const float alpha_ch[4] = { clipping_mask[index + kRed], clipping_mask[index + kGreen], clipping_mask[index + kBlue], clipping_mask[index + kAlpha] };
                    // As in darktable, channels after the guide read its updated value.
                    for (int c = 0; c < 4; ++c) {
                        const float a_HF = std::fmax(covariance_HF[c] / variance_HF[guide], 0.f);
                        const float b_HF = means_HF[c] - a_HF * means_HF[guide];
                        high_frequency[c] = alpha_ch[c] * scale_multiplier * (a_HF * high_frequency[guide] + b_HF) + (1.f - alpha_ch[c] * scale_multiplier) * high_frequency[c];
                    }
                }

                float* o = out + index;
                if (scale & kFirstScale) {
                    for (int c = 0; c < 4; ++c) {
                        o[c] = high_frequency[c];
                    }
                } else {
                    for (int c = 0; c < 4; ++c) {
                        o[c] += high_frequency[c];
                    }
                }
                if (scale & kLastScale) {
                    for (int c = 0; c < 4; ++c) {
                        o[c] = std::fmax(o[c] + LF[index + c], 0.f);
                    }
                }
                if ((scale & kLastScale) && salt && (alpha > 0.f)) {
                    std::uint32_t state[4] = { splitmix32((std::uint64_t)(j + 1)), splitmix32((std::uint64_t)((std::int64_t)(j + 1) * (std::int64_t)(i + 3))), splitmix32(1337), splitmix32(666) };
                    for (int k = 0; k < 4; ++k) {
                        (void)xoshiro128plus(state);
                    }
                    float sigma[4];
                    for (int c = 0; c < 4; ++c) {
                        sigma[c] = o[c] * noise_level;
                    }
                    float noise[3];
                    poissonNoise(o, sigma, state, noise);
                    // The noise only brightens, the photosite being clipped.
                    for (int c = 0; c < 3; ++c) {
                        noise[c] = o[c] + std::fabs(noise[c] - o[c]);
                        o[c] = std::fmax(alpha * noise[c] + alpha_comp * o[c], 0.f);
                    }
                }
                if (scale & kLastScale) {
                    const float norm = std::fmax(std::sqrt(sqf(o[kRed]) + sqf(o[kGreen]) + sqf(o[kBlue])), 1e-6f);
                    for (int c = 0; c < 3; ++c) {
                        o[c] /= norm;
                    }
                    o[kAlpha] = norm;
                }
            }
        });
    } // guideLaplacians

    void
    heatPdeDiffusion(const float* HF,
                     const float* LF,
                     const float* clipping_mask,
                     float* out,
                     std::size_t width,
                     std::size_t height,
                     int mult,
                     unsigned int scale,
                     float first_order_factor,
                     const ParallelFor& parallel)
    {
        static const float anisotropic_kernel_isophote[9] = { 0.25f, 0.5f, 0.25f, 0.5f, -3.f, 0.5f, 0.25f, 0.5f, 0.25f };

        parallel((int)height, [&](int i) {
            for (std::size_t j = 0; j < width; ++j) {
                const std::size_t idx = (std::size_t)i * width + j;
                const std::size_t index = idx * 4;
                const float alpha[4] = { clipping_mask[index + kRed], clipping_mask[index + kGreen], clipping_mask[index + kBlue], clipping_mask[index + kAlpha] };
                // The norm is not diffused.
                float high_frequency[4] = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

                if (alpha[kAlpha] > 0.f) {
                    float neighbour_pixel_HF[9][4];
                    fetchNeighbours(HF, (std::size_t)i, j, width, height, mult, neighbour_pixel_HF);

                    float laplacian_HF[3] = { 0.f, 0.f, 0.f };
                    for (int k = 0; k < 9; ++k) {
                        for (int c = 0; c < 3; ++c) {
                            laplacian_HF[c] += neighbour_pixel_HF[k][c] * anisotropic_kernel_isophote[k];
                        }
                    }
                    for (int c = 0; c < 3; ++c) {
                        high_frequency[c] += alpha[c] * (1.f / kBSplineToLaplacian) * (laplacian_HF[c] - first_order_factor * high_frequency[c]);
                    }
                }

                float* o = out + index;
                if (scale & kFirstScale) {
                    for (int c = 0; c < 4; ++c) {
                        o[c] = high_frequency[c];
                    }
                } else {
                    for (int c = 0; c < 4; ++c) {
                        o[c] += high_frequency[c];
                    }
                }
                if (scale & kLastScale) {
                    for (int c = 0; c < 4; ++c) {
                        o[c] = std::fmax(o[c] + LF[index + c], 0.f);
                    }
                    if (alpha[kAlpha] > 0.f) {
                        const float norm = std::sqrt(sqf(o[kRed]) + sqf(o[kGreen]) + sqf(o[kBlue]));
                        if (norm > 1e-4f) {
                            for (int c = 0; c < 3; ++c) {
                                o[c] /= norm;
                            }
                        }
                    }
                    // Back to RGB from ratios and norm; the norm stays in the fourth channel.
                    for (int c = 0; c < 3; ++c) {
                        o[c] = o[c] * o[kAlpha];
                    }
                }
            }
        });
    } // heatPdeDiffusion

    struct WaveletBuffers {
        float* HF;
        float* LF_odd;
        float* LF_even;
    };

    void
    waveletsProcess(const float* in,
                    float* reconstructed,
                    const float* clipping_mask,
                    std::size_t width,
                    std::size_t height,
                    int scales,
                    const WaveletBuffers& buffers,
                    DiffuseVariantEnum variant,
                    float noise_level,
                    bool salt,
                    float first_order_factor,
                    const ParallelFor& parallel)
    {
        for (int s = 0; s < scales; ++s) {
            const int mult = 1 << s;
            const float* buffer_in;
            float* buffer_out;

            if (s == 0) {
                buffer_in = in;
                buffer_out = buffers.LF_odd;
            } else if (s % 2 != 0) {
                buffer_in = buffers.LF_odd;
                buffer_out = buffers.LF_even;
            } else {
                buffer_in = buffers.LF_even;
                buffer_out = buffers.LF_odd;
            }
            decompose2DBspline(buffer_in, buffers.HF, buffer_out, width, height, mult, parallel);

            const unsigned int current_scale_type = scaleType(s, scales);
            const float radius = sqf(equivalentSigmaAtStep(kBSplineSigma, (unsigned int)(s * kLaplacianDownsampling)));
            if (variant == eDiffuseRgb) {
                guideLaplacians(buffers.HF, buffer_out, clipping_mask, reconstructed, width, height, mult, noise_level, salt, current_scale_type, radius, parallel);
            } else {
                heatPdeDiffusion(buffers.HF, buffer_out, clipping_mask, reconstructed, width, height, mult, current_scale_type, first_order_factor, parallel);
            }
        }
    }
} // namespace

int
laplacianDownsampledWidth(int width)
{
    return width / kLaplacianDownsampling;
}

int
laplacianDownsampledHeight(int height)
{
    return height / kLaplacianDownsampling;
}

void
laplacianReconstruct(const float* in,
                     std::ptrdiff_t inRowStride,
                     std::ptrdiff_t inPixelStride,
                     int width,
                     int height,
                     std::uint32_t filters,
                     const LaplacianParams& params,
                     float* reconstructed,
                     const ParallelFor& parallel)
{
    const int dsWidth = laplacianDownsampledWidth(width);
    const int dsHeight = laplacianDownsampledHeight(height);

    if ((dsWidth < 1) || (dsHeight < 1)) {
        return;
    }
    const Photosites input = { in, inRowStride, inPixelStride, 0, 0 };
    const std::size_t size = (std::size_t)dsWidth * (std::size_t)dsHeight * 4;
    std::vector<float> dsMask(size);
    std::vector<float> temp(size);
    std::vector<float> HF(size);
    std::vector<float> LF_odd(size);
    std::vector<float> LF_even(size);
    const WaveletBuffers buffers = { HF.data(), LF_odd.data(), LF_even.data() };

    downsample(input, width, height, filters, params, dsWidth, dsHeight, reconstructed, dsMask.data(), parallel);

    // darktable's scale at full resolution, where piece->iscale and roi_in->scale are both 1.
    const float scale = std::fmax((float)kLaplacianDownsampling, 1.f);
    const float final_radius = (float)((int)(1 << params.scales)) / scale;
    const int scales = std::clamp((int)std::ceil(std::log2(final_radius)), 1, kMaxNumScales);
    const float noise_level = params.noiseLevel / scale;

    for (int i = 0; i < params.iterations; ++i) {
        // Noise goes in on the last iteration only.
        const bool salt = (i == params.iterations - 1);
        waveletsProcess(reconstructed, temp.data(), dsMask.data(), (std::size_t)dsWidth, (std::size_t)dsHeight, scales, buffers, eDiffuseRgb, noise_level, salt, params.solidColor, parallel);
        waveletsProcess(temp.data(), reconstructed, dsMask.data(), (std::size_t)dsWidth, (std::size_t)dsHeight, scales, buffers, eDiffuseChroma, noise_level, salt, params.solidColor, parallel);
    }
}

void
laplacianRemosaic(const float* in,
                  std::ptrdiff_t inRowStride,
                  std::ptrdiff_t inPixelStride,
                  int inRow0,
                  int inCol0,
                  int width,
                  int height,
                  std::uint32_t filters,
                  const LaplacianParams& params,
                  const float* reconstructed,
                  int row0,
                  int col0,
                  int rows,
                  int cols,
                  float* out,
                  const ParallelFor& parallel)
{
    const Photosites input = { in, inRowStride, inPixelStride, inRow0, inCol0 };
    const int dsWidth = laplacianDownsampledWidth(width);
    const int dsHeight = laplacianDownsampledHeight(height);

    if ((dsWidth < 1) || (dsHeight < 1)) {
        parallel(rows, [&](int r) {
            for (int c = 0; c < cols; ++c) {
                out[(std::size_t)r * (std::size_t)cols + (std::size_t)c] = input.at(row0 + r, col0 + c);
            }
        });

        return;
    }
    parallel(rows, [&](int r) {
        const int row = row0 + r;
        std::vector<float> mask((std::size_t)cols * 4);
        boxMeanMask(input, width, height, filters, params, row, col0, 1, cols, mask.data());
        const BilinearAxis y((std::size_t)row, (std::size_t)height, (std::size_t)dsHeight);
        const float* dsPrev = reconstructed + y.prev * (std::size_t)dsWidth * 4;
        const float* dsNext = reconstructed + y.next * (std::size_t)dsWidth * 4;
        for (int cc = 0; cc < cols; ++cc) {
            const int col = col0 + cc;
            const int c = fcBayer((std::size_t)row, (std::size_t)col, filters);
            const BilinearAxis x((std::size_t)col, (std::size_t)width, (std::size_t)dsWidth);
            const float interpolated = bilinear(y, x, dsPrev[x.prev * 4 + c], dsPrev[x.next * 4 + c], dsNext[x.next * 4 + c], dsNext[x.prev * 4 + c]);
            const float opacity = clip01(mask[(std::size_t)cc * 4 + kAlpha]);
            out[(std::size_t)r * (std::size_t)cols + (std::size_t)cc] = opacity * std::fmax(interpolated * params.wb[c], 0.f) + (1.f - opacity) * input.at(row, col);
        }
    });
}
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
