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
#include <cstdint>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace DenoiseProfiledKernels {
namespace {
    float
    bitsToFloat(std::int32_t bits)
    {
        float f;

        std::memcpy(&f, &bits, sizeof(f));

        return f;
    }
} // namespace

float
fastMexp2fInt(float x)
{
    // Past 126 the result is 0 anyway; this keeps the conversion below in range.
    if (!(x < 200.f)) {
        return 0.f;
    }
    const std::int32_t i1 = 0x3f800000;
    const std::int32_t i2 = 0x3f000000;
    const std::int32_t k0 = i1 + (std::int32_t)(x * (float)(i2 - i1));

    return bitsToFloat((k0 >= 0x800000) ? k0 : 0);
}

float
fastMexp2fFloat(float x)
{
    if (!(x < 200.f)) {
        return 0.f;
    }
    const float i1 = (float)0x3f800000u;
    const float i2 = (float)0x3f000000u;
    const float k0 = i1 + x * (i2 - i1);

    return bitsToFloat((k0 >= (float)0x800000u) ? (std::int32_t)k0 : 0);
}

int
inferRadius(float a)
{
    const float v = 1.0f + a * 15000.0f + a * a * 300000.0f;

    return (int)std::min((unsigned)std::max(v, 0.f), 8u);
}

float
inferScattering(float a)
{
    return std::min(3000.0f * a, 1.0f);
}

float
inferShadows(float a)
{
    return (float)std::min(std::max(0.1f - 0.1 * std::log(a), (double)0.7f), (double)1.8f);
}

float
inferBias(float a)
{
    return (float)-std::max(5 + 0.5 * std::log(a), 0.0);
}

Vec3
wbFactors(const std::optional<std::array<double, 3>>& applied,
          bool adaptive)
{
    Vec3 coeffs = { 0.f, 0.f, 0.f };

    if (applied) {
        for (int c = 0; c < 3; ++c) {
            coeffs[c] = (float)(*applied)[c];
        }
    }
    const float mean = (coeffs[0] + coeffs[1] + coeffs[2]) / 3.0f;
    if ((mean != 0.0f) && adaptive) {
        return coeffs;
    }
    if (mean == 0.0f) {
        // No white balance has been applied, as on a non-raw image.
        return { 1.f, 1.f, 1.f };
    }

    return { mean, mean, mean };
}

float
quantizeForce(double value)
{
    const int v = (int)(value * 65535.0f + 0.5f);

    return (float)std::clamp(v, 0, 65535) * (1.0f / 65536.0f);
}

void
preconditionV2(const float* in,
               float* out,
               std::size_t n,
               float a,
               const Vec3& p,
               float b,
               const Vec3& wb)
{
    Vec3 expon;
    Vec3 denom;

    for (int c = 0; c < 3; ++c) {
        expon[c] = -p[c] / 2 + 1;
        denom[c] = (-p[c] + 2) * std::sqrt(a);
    }
    for (std::size_t j = 0; j < 3 * n; j += 3) {
        for (int c = 0; c < 3; ++c) {
            const float scaled = std::max(in[j + c] / wb[c] + b, 0.0f);
            out[j + c] = 2.0f * std::pow(scaled, expon[c]) / denom[c];
        }
    }
}

void
backtransformV2(float* buf,
                std::size_t n,
                float a,
                const Vec3& p,
                float b,
                float bias,
                const Vec3& wb)
{
    Vec3 expon;
    Vec3 denom;

    for (int c = 0; c < 3; ++c) {
        expon[c] = 1.0f / (1.0f - p[c] / 2.0f);
        denom[c] = 4.0f / (std::sqrt(a) * (2.0f - p[c]));
    }
    for (std::size_t j = 0; j < 3 * n; j += 3) {
        for (int c = 0; c < 3; ++c) {
            const float x = std::max(buf[j + c], 0.0f);
            const float delta = x * x + bias;
            const float z1 = (x + std::sqrt(std::max(delta, 0.0f))) / denom[c];
            buf[j + c] = wb[c] * (std::pow(z1, expon[c]) - b);
        }
    }
}

bool
invertMatrix(const Matrix3& in,
             Matrix3* out)
{
    const float biga = in[1][1] * in[2][2] - in[1][2] * in[2][1];
    const float bigb = -in[1][0] * in[2][2] + in[1][2] * in[2][0];
    const float bigc = in[1][0] * in[2][1] - in[1][1] * in[2][0];
    const float bigd = -in[0][1] * in[2][2] + in[0][2] * in[2][1];
    const float bige = in[0][0] * in[2][2] - in[0][2] * in[2][0];
    const float bigf = -in[0][0] * in[2][1] + in[0][1] * in[2][0];
    const float bigg = in[0][1] * in[1][2] - in[0][2] * in[1][1];
    const float bigh = -in[0][0] * in[1][2] + in[0][2] * in[1][0];
    const float bigi = in[0][0] * in[1][1] - in[0][1] * in[1][0];
    const float det = in[0][0] * biga + in[0][1] * bigb + in[0][2] * bigc;

    if (det == 0.0f) {
        return false;
    }
    (*out)[0] = { 1.0f / det * biga, 1.0f / det * bigd, 1.0f / det * bigg };
    (*out)[1] = { 1.0f / det * bigb, 1.0f / det * bige, 1.0f / det * bigh };
    (*out)[2] = { 1.0f / det * bigc, 1.0f / det * bigf, 1.0f / det * bigi };

    return true;
}

void
conversionMatrices(const Vec3& wb,
                   Matrix3* toY0U0V0,
                   Matrix3* toRgb)
{
    // "Secrets of image denoising cuisine", with Y0 weighted by the white balance and every row
    // normalised to a variance of 1.
    Matrix3 m = { Vec3 { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
                  Vec3 { 0.5f, 0.0f, -0.5f },
                  Vec3 { 0.25f, -0.5f, 0.25f } };
    float sumInvWb = 1.0f / wb[0] + 1.0f / wb[1] + 1.0f / wb[2];

    sumInvWb *= std::sqrt(3.f);
    for (int c = 0; c < 3; ++c) {
        m[0][c] = sumInvWb / wb[c];
    }
    const float stddevU0 = std::sqrt(0.5f * 0.5f * wb[0] * wb[0] + 0.5f * 0.5f * wb[2] * wb[2]);
    const float stddevV0 = std::sqrt(0.25f * 0.25f * wb[0] * wb[0] + 0.5f * 0.5f * wb[1] * wb[1] + 0.25f * 0.25f * wb[2] * wb[2]);
    for (int c = 0; c < 3; ++c) {
        m[1][c] /= stddevU0;
        m[2][c] /= stddevV0;
    }
    Matrix3 inverse = {};
    if (!invertMatrix(m, &inverse)) {
        const float stddevY0 = std::sqrt(1.0f / 9.0f * (wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2]));
        for (int c = 0; c < 3; ++c) {
            m[0][c] = 1.0f / (3.0f * stddevY0);
        }
        (void)invertMatrix(m, &inverse);
    }
    *toY0U0V0 = m;
    *toRgb = inverse;
}

void
preconditionY0U0V0(const float* in,
                   float* out,
                   std::size_t n,
                   float a,
                   const Vec3& p,
                   float b,
                   const Matrix3& toY0U0V0)
{
    Vec3 expon;
    Vec3 scale;

    for (int c = 0; c < 3; ++c) {
        expon[c] = -p[c] / 2 + 1;
        scale[c] = 2.0f / ((-p[c] + 2) * std::sqrt(a));
    }
    for (std::size_t j = 0; j < 3 * n; j += 3) {
        float tmp[3];
        for (int c = 0; c < 3; ++c) {
            tmp[c] = std::pow(std::max(in[j + c] + b, 0.0f), expon[c]) * scale[c];
        }
        for (int r = 0; r < 3; ++r) {
            out[j + r] = toY0U0V0[r][0] * tmp[0] + toY0U0V0[r][1] * tmp[1] + toY0U0V0[r][2] * tmp[2];
        }
    }
}

void
backtransformY0U0V0(float* buf,
                    std::size_t n,
                    float a,
                    const Vec3& p,
                    float b,
                    float bias,
                    const Vec3& wb,
                    const Matrix3& toRgb)
{
    Vec3 biasWb;
    Vec3 expon;
    Vec3 scale;

    for (int c = 0; c < 3; ++c) {
        biasWb[c] = bias * wb[c];
        expon[c] = 1.0f / (1.0f - p[c] / 2.0f);
        scale[c] = (std::sqrt(a) * (2.0f - p[c])) / 4.0f;
    }
    for (std::size_t j = 0; j < 3 * n; j += 3) {
        float rgb[3];
        for (int r = 0; r < 3; ++r) {
            rgb[r] = toRgb[r][0] * buf[j] + toRgb[r][1] * buf[j + 1] + toRgb[r][2] * buf[j + 2];
        }
        for (int c = 0; c < 3; ++c) {
            const float x = std::max(rgb[c], 0.0f);
            const float delta = x * x + biasWb[c];
            const float z1 = (x + std::sqrt(std::max(delta, 0.0f))) * scale[c];
            buf[j + c] = std::pow(z1, expon[c]) - b;
        }
    }
}
} // namespace DenoiseProfiledKernels

NATRON_NAMESPACE_EXIT
