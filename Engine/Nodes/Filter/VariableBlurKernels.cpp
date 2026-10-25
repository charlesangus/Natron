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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "VariableBlurKernels.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace VariableBlurKernels {
namespace {
    /// The table's integral of the image over [0, u) x [0, v), bilinear between the pixel corners;
    /// u and v are clamped to the image, outside which it is zero. width and height are at least 1.
    inline double
    integral(const double* sat,
             std::size_t stride,
             int width,
             int height,
             double u,
             double v)
    {
        u = std::min(std::max(u, 0.), (double)width);
        v = std::min(std::max(v, 0.), (double)height);
        const int i = std::min((int)u, width - 1);
        const int j = std::min((int)v, height - 1);
        const double fu = u - i;
        const double fv = v - j;
        const double* r0 = sat + (std::size_t)j * stride + i;
        const double* r1 = r0 + stride;

        return r0[0] + fu * (r0[1] - r0[0]) + fv * (r1[0] - r0[0]) + fu * fv * (r1[1] - r1[0] - r0[1] + r0[0]);
    }
} // anonymous namespace

double
boxVariance(double r)
{
    if (!(r > 0.)) {
        return 0.;
    }
    // Pixels -n..n weigh 1/w each and the two at n + 1 weigh f/w, w = 2r + 1.
    const double n = std::floor(r);
    const double f = r - n;

    return (n * (n + 1.) * (2. * n + 1.) / 3. + 2. * f * (n + 1.) * (n + 1.)) / (2. * r + 1.);
}

double
boxRadius(double sigma)
{
    if (!(sigma > 0.)) {
        return 0.;
    }
    const double target = sigma * sigma / 3.;
    // At a whole radius n the variance is n (n + 1) / 3; n is the largest one not above target.
    double n = std::floor((std::sqrt(1. + 12. * target) - 1.) / 2.);
    while ((n > 0.) && (n * (n + 1.) / 3. > target)) {
        n -= 1.;
    }
    while ((n + 1.) * (n + 2.) / 3. <= target) {
        n += 1.;
    }
    // Between n and n + 1 the variance is a ratio of linear functions of f, solved for target.
    const double f = (2. * n + 1.) * (target - n * (n + 1.) / 3.) / (2. * ((n + 1.) * (n + 1.) - target));

    return n + std::min(std::max(f, 0.), 1.);
}

void
prefixRow(const float* src,
          int width,
          double* satRow)
{
    double sum = 0.;

    satRow[0] = 0.;
    for (int x = 0; x < width; ++x) {
        sum += src[x];
        satRow[x + 1] = sum;
    }
}

void
accumulateColumn(double* sat,
                 std::size_t stride,
                 int height,
                 int column)
{
    double* entry = sat + column;
    double sum = entry[0];

    for (int y = 1; y <= height; ++y) {
        entry += stride;
        sum += *entry;
        *entry = sum;
    }
}

void
boxRow(const double* sat,
       int width,
       int height,
       int y,
       const float* rx,
       const float* ry,
       const float* src,
       float* dst)
{
    const std::size_t stride = (std::size_t)width + 1;

    for (int x = 0; x < width; ++x) {
        const double a = rx[x];
        const double b = ry[x];
        // No filter at all, so that a radius of 0 leaves the pixel exactly as it was.
        if ((a <= 0.) && (b <= 0.)) {
            dst[x] = src[x];
            continue;
        }
        const double u0 = x - a;
        const double u1 = x + 1. + a;
        const double v0 = y - b;
        const double v1 = y + 1. + b;
        const double sum = integral(sat, stride, width, height, u1, v1) - integral(sat, stride, width, height, u0, v1)
            - integral(sat, stride, width, height, u1, v0) + integral(sat, stride, width, height, u0, v0);
        dst[x] = (float)(sum / ((2. * a + 1.) * (2. * b + 1.)));
    }
}

void
iteratedBox(const float* src,
            int width,
            int height,
            const float* rx,
            const float* ry,
            int iterations,
            float* dst)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t stride = (std::size_t)width + 1;
    const std::size_t planeSize = (std::size_t)width * height;
    std::vector<double> sat(stride * ((std::size_t)height + 1), 0.);
    std::vector<float> from(src, src + planeSize);
    std::vector<float> to(planeSize);

    for (int k = 0; k < iterations; ++k) {
        for (int y = 0; y < height; ++y) {
            prefixRow(&from[(std::size_t)y * width], width, &sat[(std::size_t)(y + 1) * stride]);
        }
        for (int c = 1; c <= width; ++c) {
            accumulateColumn(sat.data(), stride, height, c);
        }
        for (int y = 0; y < height; ++y) {
            const std::size_t row = (std::size_t)y * width;
            boxRow(sat.data(), width, height, y, rx + row, ry + row, &from[row], &to[row]);
        }
        from.swap(to);
    }
    std::copy(from.begin(), from.end(), dst);
}
} // namespace VariableBlurKernels

NATRON_NAMESPACE_EXIT
