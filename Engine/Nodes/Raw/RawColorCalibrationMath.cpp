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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): src/common/illuminants.h
 * (Copyright (C) 2020-2024 darktable developers), src/common/chromatic_adaptation.h (Copyright (C)
 * 2020-2025 darktable developers), the xyY, u'v'Y, Lab and LCh conversions of
 * src/common/colorspaces_inline_conversions.h and _check_if_close_to_daylight() of
 * src/iop/channelmixerrgb.c. darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2020-2025 darktable developers.
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

#include "RawColorCalibrationMath.h"

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawColorCalibrationMath {
namespace {
    const float kNormMin = 1.52587890625e-05f;
    const float k2PiF = 6.28318530717958647693f;

    // darktable's D50xyY is a double cmsCIExyY.
    const double kD50x = 0.34567;
    const double kD50y = 0.35850;

    const float kFluorescent[static_cast<int>(Fluorescent::Last)][2] = {
        { 0.31310f, 0.33727f },
        { 0.37208f, 0.37529f },
        { 0.40910f, 0.39430f },
        { 0.44018f, 0.40329f },
        { 0.31379f, 0.34531f },
        { 0.37790f, 0.38835f },
        { 0.31292f, 0.32933f },
        { 0.34588f, 0.35875f },
        { 0.37417f, 0.37281f },
        { 0.34609f, 0.35986f },
        { 0.38052f, 0.37713f },
        { 0.43695f, 0.40441f },
    };

    const float kLed[static_cast<int>(Led::Last)][2] = {
        { 0.4560f, 0.4078f },
        { 0.4357f, 0.4012f },
        { 0.3756f, 0.3723f },
        { 0.3422f, 0.3502f },
        { 0.3118f, 0.3236f },
        { 0.4474f, 0.4066f },
        { 0.4557f, 0.4211f },
        { 0.4560f, 0.4548f },
        { 0.3781f, 0.3775f },
    };

    const Mat3 kXyzToBradfordLms = { 0.8951f, 0.2664f, -0.1614f,
                                     -0.7502f, 1.7135f, 0.0367f,
                                     0.0389f, -0.0685f, 1.0296f };

    const Mat3 kBradfordLmsToXyz = { 0.9870f, -0.1471f, 0.1600f,
                                     0.4323f, 0.5184f, 0.0493f,
                                     -0.0085f, 0.0400f, 0.9685f };

    const Mat3 kXyzToCat16Lms = { 0.401288f, 0.650173f, -0.051461f,
                                  -0.250268f, 1.204414f, 0.045854f,
                                  -0.002079f, 0.048952f, 0.953127f };

    const Mat3 kCat16LmsToXyz = { 1.862068f, -1.011255f, 0.149187f,
                                  0.38752f, 0.621447f, -0.008974f,
                                  -0.015841f, -0.034123f, 1.049964f };

    const Vec3 kD50BradfordLms = { 0.996078f, 1.020646f, 0.818155f };
    const Vec3 kD50Cat16Lms = { 0.994535f, 1.000997f, 0.833036f };
    const Vec3 kD50Xyz = { 0.9642119944211994f, 1.0f, 0.8251882845188288f };

    const Vec3 kLabD50 = { 0.9642f, 1.0f, 0.8249f };
    const Vec3 kLabD50Inv = { 1.0f / 0.9642f, 1.0f, 1.0f / 0.8249f };

    const float kLuvD50[2] = { 0.20915914598542354f, 0.488075320769787f };

    Vec3
    applyMatrix(const Mat3& m,
                const Vec3& in)
    {
        Vec3 out;
        for (int r = 0; r < 3; ++r) {
            out[r] = m[r * 3 + 0] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
        }
        return out;
    }

    Mat3
    multiplyMatrices(const Mat3& m1,
                     const Mat3& m2)
    {
        Mat3 dst;
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < 3; ++i) {
                float sum = 0.0f;
                for (int j = 0; j < 3; ++j) {
                    sum += m1[k * 3 + j] * m2[j * 3 + i];
                }
                dst[k * 3 + i] = sum;
            }
        }
        return dst;
    }

    void
    downscaleVector(Vec3& vector,
                    float scaling)
    {
        const bool valid = scaling > kNormMin;
        for (int c = 0; c < 3; ++c) {
            vector[c] = valid ? vector[c] / (scaling + kNormMin) : vector[c] / kNormMin;
        }
    }

    void
    upscaleVector(Vec3& vector,
                  float scaling)
    {
        const bool valid = scaling > kNormMin;
        for (int c = 0; c < 3; ++c) {
            vector[c] = valid ? vector[c] * (scaling + kNormMin) : vector[c] * kNormMin;
        }
    }

    // darktable builds with -ffast-math, where dt_fast_hypotf() is this rather than hypotf().
    float
    fastHypot(float x,
              float y)
    {
        return std::sqrt(x * x + y * y);
    }

    float
    cube(float x)
    {
        return x * x * x;
    }

    float
    labFInv(float x)
    {
        const float epsilon = 0.20689655172413796f;
        const float kappa = 24389.0f / 27.0f;
        return (x > epsilon) ? x * x * x : (116.0f * x - 16.0f) / kappa;
    }

    struct PlanckianLut {
        std::vector<float> temperature;
        std::vector<float> x;
        std::vector<float> y;
    };

    const PlanckianLut&
    planckianLut()
    {
        static const PlanckianLut lut = [] {
            const float tMin = 1667.f;
            const float tMax = 25000.f;
            const float tRange = tMax - tMin;
            const std::size_t samples = 1 << 16;
            PlanckianLut out;
            out.temperature.resize(samples);
            out.x.resize(samples);
            out.y.resize(samples);
            for (std::size_t i = 0; i < samples; ++i) {
                const float step = std::pow(static_cast<float>(i) / static_cast<float>(samples - 1), 4.0f);
                const float t = tMin + step * tRange;
                const Xy xy = (t >= 4000.f) ? cctToXyDaylight(t) : cctToXyBlackbody(t);
                out.temperature[i] = t;
                out.x[i] = xy.x;
                out.y[i] = xy.y;
            }
            return out;
        }();
        return lut;
    }
} // namespace

const char*
illuminantLabel(Illuminant illuminant)
{
    switch (illuminant) {
    case Illuminant::Pipe:
        return "same as pipeline (D50)";
    case Illuminant::A:
        return "A (incandescent)";
    case Illuminant::D:
        return "D (daylight)";
    case Illuminant::E:
        return "E (equi-energy)";
    case Illuminant::F:
        return "F (fluorescent)";
    case Illuminant::Led:
        return "LED (LED light)";
    case Illuminant::BlackBody:
        return "Planckian (black body)";
    case Illuminant::Custom:
        return "custom";
    case Illuminant::Camera:
        return "as shot in camera";
    case Illuminant::Last:
    case Illuminant::DetectSurfaces:
    case Illuminant::DetectEdges:
        break;
    }
    return nullptr;
}

const char*
fluorescentLabel(Fluorescent fluorescent)
{
    static const char* const labels[] = {
        "F1 (Daylight 6430 K) – medium CRI",
        "F2 (Cool White 4230 K) – medium CRI",
        "F3 (White 3450 K) – medium CRI",
        "F4 (Warm White 2940 K) – medium CRI",
        "F5 (Daylight 6350 K) – medium CRI",
        "F6 (Lite White 4150 K) – medium CRI",
        "F7 (D65 simulator 6500 K) – high CRI",
        "F8 (D50 simulator 5000 K) – high CRI",
        "F9 (Cool White Deluxe 4150 K) – high CRI",
        "F10 (Tuned RGB 5000 K) – low CRI",
        "F11 (Tuned RGB 4000 K) – low CRI",
        "F12 (Tuned RGB 3000 K) – low CRI",
    };
    const int i = static_cast<int>(fluorescent);
    return (i >= 0 && i < static_cast<int>(Fluorescent::Last)) ? labels[i] : nullptr;
}

const char*
ledLabel(Led led)
{
    static const char* const labels[] = {
        "B1 (Blue 2733 K)",
        "B2 (Blue 2998 K)",
        "B3 (Blue 4103 K)",
        "B4 (Blue 5109 K)",
        "B5 (Blue 6598 K)",
        "BH1 (Blue-Red hybrid 2851 K)",
        "RGB1 (RGB 2840 K)",
        "V1 (Violet 2724 K)",
        "V2 (Violet 4070 K)",
    };
    const int i = static_cast<int>(led);
    return (i >= 0 && i < static_cast<int>(Led::Last)) ? labels[i] : nullptr;
}

const char*
adaptationLabel(Adaptation adaptation)
{
    switch (adaptation) {
    case Adaptation::LinearBradford:
        return "linear Bradford (ICC v4)";
    case Adaptation::Cat16:
        return "CAT16 (CIECAM16)";
    case Adaptation::FullBradford:
        return "non-linear Bradford";
    case Adaptation::Xyz:
        return "XYZ";
    case Adaptation::Rgb:
        return "none (bypass)";
    case Adaptation::Last:
        break;
    }
    return nullptr;
}

Xy
fluorescentXy(Fluorescent fluorescent)
{
    const int i = static_cast<int>(fluorescent);
    if (i < 0 || i >= static_cast<int>(Fluorescent::Last)) {
        return { 0.f, 0.f };
    }
    return { kFluorescent[i][0], kFluorescent[i][1] };
}

Xy
ledXy(Led led)
{
    const int i = static_cast<int>(led);
    if (i < 0 || i >= static_cast<int>(Led::Last)) {
        return { 0.f, 0.f };
    }
    return { kLed[i][0], kLed[i][1] };
}

float
xyToCct(float x,
        float y)
{
    if (x < FLT_MAX) {
        const float n = (x - 0.3366f) / (y - 0.1735f);
        return (-949.86315f + 6253.80338f * std::exp(-n / 0.92159f)
                + 28.70599f * std::exp(-n / 0.20039f)
                + 0.00004f * std::exp(-n / 0.07125f));
    }
    return 0.0f;
}

Xy
cctToXyDaylight(float t)
{
    float x = 0.f;
    if (t >= 4000.f && t <= 7000.0f) {
        x = ((-4.6070e9f / t + 2.9678e6f) / t + 0.09911e3f) / t + 0.244063f;
    } else if (t > 7000.f && t <= 25000.f) {
        x = ((-2.0064e9f / t + 1.9018e6f) / t + 0.24748e3f) / t + 0.237040f;
    }
    const float y = (-3.f * x + 2.87f) * x - 0.275f;
    return { x, y };
}

Xy
cctToXyBlackbody(float t)
{
    float x = 0.f;
    float y = 0.f;
    if (t >= 1667.f && t <= 4000.f) {
        x = ((-0.2661239e9f / t - 0.2343589e6f) / t + 0.8776956e3f) / t + 0.179910f;
    } else if (t > 4000.f && t <= 25000.f) {
        x = ((-3.0258469e9f / t + 2.1070379e6f) / t + 0.2226347e3f) / t + 0.240390f;
    }

    if (t >= 1667.f && t <= 2222.f) {
        y = ((-1.1063814f * x - 1.34811020f) * x + 2.18555832f) * x - 0.20219683f;
    } else if (t > 2222.f && t <= 4000.f) {
        y = ((-0.9549476f * x - 1.37418593f) * x + 2.09137015f) * x - 0.16748867f;
    } else if (t > 4000.f && t <= 25000.f) {
        y = ((3.0817580f * x - 5.87338670f) * x + 3.75112997f) * x - 0.37001483f;
    }
    return { x, y };
}

Vec3
illuminantXyToXyz(float x,
                  float y)
{
    return { x / y, 1.f, (1.f - x - y) / y };
}

std::array<float, 2>
xyToUv(const Xy& xy)
{
    const float denom = 12.f * xy.y - 1.882f * xy.x + 2.9088f;
    // darktable's 1.9116 is a double literal, so the sum is formed in double.
    float u = static_cast<float>(5.5932f * xy.x + 1.9116 * xy.y);
    float v = 7.8972f * xy.y;
    u /= denom;
    v /= denom;
    return { u, v };
}

float
cctReverseLookup(float x,
                 float y)
{
    const PlanckianLut& lut = planckianLut();
    float minRadius = FLT_MAX;
    float temperature = 0.0f;
    const std::size_t n = lut.temperature.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float radius = fastHypot(lut.x[i] - x, lut.y[i] - y);
        if (radius < minRadius) {
            minRadius = radius;
            temperature = lut.temperature[i];
        }
    }
    return temperature;
}

void
matricePseudoinverse(const float (*in)[3],
                     float (*out)[3],
                     int size)
{
    float work[3][6];

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 6; ++j) {
            work[i][j] = (j == i + 3) ? 1.f : 0.f;
        }
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < size; ++k) {
                work[i][j] += in[k][i] * in[k][j];
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        float num = work[i][i];
        for (int j = 0; j < 6; ++j) {
            work[i][j] /= num;
        }
        for (int k = 0; k < 3; ++k) {
            if (k == i) {
                continue;
            }
            num = work[k][i];
            for (int j = 0; j < 6; ++j) {
                work[k][j] -= work[i][j] * num;
            }
        }
    }
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i][j] = 0.0f;
            for (int k = 0; k < 3; ++k) {
                out[i][j] += work[j][k + 3] * in[i][k];
            }
        }
    }
}

Xy
wbCoeffsToIlluminantXy(const Mat3& camToXyzT,
                       const Vec3& wb)
{
    Vec3 xyz;
    for (int c = 0; c < 3; ++c) {
        xyz[c] = camToXyzT[0 * 3 + c] / wb[0] + camToXyzT[1 * 3 + c] / wb[1] + camToXyzT[2 * 3 + c] / wb[2];
    }

    const Vec3 d65 = { 0.941238f, 1.040633f, 1.088932f };
    const float p = std::pow(1.088932f / 0.818155f, 0.0834f);

    Vec3 lms = convertXyzToBradfordLms(xyz);
    lms = bradfordAdaptD50(lms, d65, p, false);
    xyz = convertBradfordLmsToXyz(lms);

    xyz[0] /= xyz[1];
    xyz[2] /= xyz[1];
    xyz[1] /= xyz[1];

    return { xyz[0] / (xyz[0] + xyz[1] + xyz[2]), xyz[1] / (xyz[0] + xyz[1] + xyz[2]) };
}

std::optional<Xy>
findTemperatureFromRawCoeffs(const RawCameraWhite& camera,
                             const Vec4* customWb)
{
    if (!camera.matrixCorrectionSupported) {
        return std::nullopt;
    }

    const int numCoeffs = camera.fourColors ? 4 : 3;
    for (int k = 0; k < numCoeffs; ++k) {
        if (!std::isnormal(camera.wbCoeffs[k]) || camera.wbCoeffs[k] == 0.0f) {
            return std::nullopt;
        }
    }

    Vec4 wb = camera.wbCoeffs;
    if (customWb) {
        for (int k = 0; k < 4; ++k) {
            wb[k] *= (*customWb)[k];
        }
    }

    const Mat3* matrix = nullptr;
    if (camera.d65ColorMatrix && std::isfinite((*camera.d65ColorMatrix)[0])) {
        matrix = &*camera.d65ColorMatrix;
    } else if (camera.adobeXyzToCam) {
        matrix = &*camera.adobeXyzToCam;
    }
    if (!matrix || !std::isfinite((*matrix)[0])) {
        return std::nullopt;
    }

    float xyzToCam[4][3] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            xyzToCam[i][j] = (*matrix)[i * 3 + j];
        }
    }
    float camToXyz[4][3] = {};
    matricePseudoinverse(xyzToCam, camToXyz, 3);
    if (!std::isfinite(camToXyz[0][0])) {
        return std::nullopt;
    }

    Mat3 camToXyzT;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            camToXyzT[i * 3 + j] = camToXyz[i][j];
        }
    }
    return wbCoeffsToIlluminantXy(camToXyzT, { wb[0], wb[1], wb[2] });
}

std::optional<Xy>
illuminantToXy(Illuminant illuminant,
               const RawCameraWhite* camera,
               const Vec4* customWb,
               float t,
               Fluorescent fluorescent,
               Led led)
{
    float x = 0.f;
    float y = 0.f;

    switch (illuminant) {
    case Illuminant::Pipe:
        x = static_cast<float>(kD50x);
        y = static_cast<float>(kD50y);
        break;
    case Illuminant::E:
        x = y = 1.f / 3.f;
        break;
    case Illuminant::A:
        x = 0.44757f;
        y = 0.40745f;
        break;
    case Illuminant::F: {
        const Xy xy = fluorescentXy(fluorescent);
        x = xy.x;
        y = xy.y;
        break;
    }
    case Illuminant::Led: {
        const Xy xy = ledXy(led);
        x = xy.x;
        y = xy.y;
        break;
    }
    case Illuminant::D: {
        const Xy xy = cctToXyDaylight(t);
        x = xy.x;
        y = xy.y;
        if (y != 0.f && x != 0.f) {
            break;
        }
    }
        [[fallthrough]];
    case Illuminant::BlackBody: {
        const Xy xy = cctToXyBlackbody(t);
        x = xy.x;
        y = xy.y;
        if (y != 0.f && x != 0.f) {
            break;
        }
    }
        [[fallthrough]];
    case Illuminant::Camera:
        if (camera) {
            if (const std::optional<Xy> xy = findTemperatureFromRawCoeffs(*camera, customWb)) {
                x = xy->x;
                y = xy->y;
                break;
            }
        }
        [[fallthrough]];
    case Illuminant::Custom:
    case Illuminant::DetectEdges:
    case Illuminant::DetectSurfaces:
    case Illuminant::Last:
        return std::nullopt;
    }

    if (x != 0.f && y != 0.f) {
        return Xy { x, y };
    }
    return std::nullopt;
}

DaylightCheck
checkIfCloseToDaylight(float x,
                       float y)
{
    float t = xyToCct(x, y);

    // xyToCct() is only valid from 3000 K.
    if (t < 3000.f && t > 1667.f) {
        t = cctReverseLookup(x, y);
    }

    const std::array<float, 2> uvRef = xyToUv({ x, y });

    // A failed lookup leaves the previous test chromaticity in place, as in darktable.
    Xy xyTest = { 0.f, 0.f };
    if (const std::optional<Xy> xy = illuminantToXy(Illuminant::D, nullptr, nullptr, t, Fluorescent::Last, Led::Last)) {
        xyTest = *xy;
    }
    std::array<float, 2> uvTest = xyToUv(xyTest);
    const float deltaDaylight = fastHypot(uvTest[0] - uvRef[0], uvTest[1] - uvRef[1]);

    if (const std::optional<Xy> xy = illuminantToXy(Illuminant::BlackBody, nullptr, nullptr, t, Fluorescent::Last, Led::Last)) {
        xyTest = *xy;
    }
    uvTest = xyToUv(xyTest);
    const float deltaBb = fastHypot(uvTest[0] - uvRef[0], uvTest[1] - uvRef[1]);

    DaylightCheck out;
    out.temperature = t;
    if (deltaBb < 0.005f || deltaDaylight < 0.005f) {
        out.illuminant = (deltaBb < deltaDaylight) ? Illuminant::BlackBody : Illuminant::D;
    } else {
        out.illuminant = Illuminant::Custom;
    }
    out.adaptation = Adaptation::Cat16;
    return out;
}

Vec3
convertXyzToBradfordLms(const Vec3& xyz)
{
    return applyMatrix(kXyzToBradfordLms, xyz);
}

Vec3
convertBradfordLmsToXyz(const Vec3& lms)
{
    return applyMatrix(kBradfordLmsToXyz, lms);
}

Vec3
convertXyzToCat16Lms(const Vec3& xyz)
{
    return applyMatrix(kXyzToCat16Lms, xyz);
}

Vec3
convertCat16LmsToXyz(const Vec3& lms)
{
    return applyMatrix(kCat16LmsToXyz, lms);
}

Vec3
convertAnyXyzToLms(const Vec3& xyz,
                   Adaptation kind)
{
    switch (kind) {
    case Adaptation::FullBradford:
    case Adaptation::LinearBradford:
        return convertXyzToBradfordLms(xyz);
    case Adaptation::Cat16:
        return convertXyzToCat16Lms(xyz);
    case Adaptation::Xyz:
    case Adaptation::Rgb:
    case Adaptation::Last:
        break;
    }
    return xyz;
}

Vec3
convertAnyLmsToXyz(const Vec3& lms,
                   Adaptation kind)
{
    switch (kind) {
    case Adaptation::FullBradford:
    case Adaptation::LinearBradford:
        return convertBradfordLmsToXyz(lms);
    case Adaptation::Cat16:
        return convertCat16LmsToXyz(lms);
    case Adaptation::Xyz:
    case Adaptation::Rgb:
    case Adaptation::Last:
        break;
    }
    return lms;
}

Mat3
makeRgbToBradfordLms(const Mat3& rgbToXyz)
{
    return multiplyMatrices(kXyzToBradfordLms, rgbToXyz);
}

Mat3
makeBradfordLmsToXyz(const Mat3& lms)
{
    return multiplyMatrices(kBradfordLmsToXyz, lms);
}

Mat3
makeRgbToCat16Lms(const Mat3& rgbToXyz)
{
    return multiplyMatrices(kXyzToCat16Lms, rgbToXyz);
}

Mat3
makeCat16LmsToXyz(const Mat3& lms)
{
    return multiplyMatrices(kCat16LmsToXyz, lms);
}

Vec3
bradfordAdaptD50(const Vec3& lmsIn,
                 const Vec3& originIlluminant,
                 float p,
                 bool full)
{
    Vec3 temp = { lmsIn[0] / originIlluminant[0],
                  lmsIn[1] / originIlluminant[1],
                  lmsIn[2] / originIlluminant[2] };

    // The exponent is skipped for negative blue, which falls back to linear Bradford.
    if (full) {
        temp[2] = (temp[2] > 0.f) ? std::pow(temp[2], p) : temp[2];
    }

    return { kD50BradfordLms[0] * temp[0], kD50BradfordLms[1] * temp[1], kD50BradfordLms[2] * temp[2] };
}

Vec3
cat16AdaptD50(const Vec3& lmsIn,
              const Vec3& originIlluminant,
              float D,
              bool full)
{
    const Vec3& d50 = kD50Cat16Lms;
    if (full) {
        return { lmsIn[0] * d50[0] / originIlluminant[0],
                 lmsIn[1] * d50[1] / originIlluminant[1],
                 lmsIn[2] * d50[2] / originIlluminant[2] };
    }
    return { lmsIn[0] * (D * d50[0] / originIlluminant[0] + 1.f - D),
             lmsIn[1] * (D * d50[1] / originIlluminant[1] + 1.f - D),
             lmsIn[2] * (D * d50[2] / originIlluminant[2] + 1.f - D) };
}

Vec3
xyzAdaptD50(const Vec3& xyzIn,
            const Vec3& originIlluminant)
{
    const Vec3& d50 = kD50Xyz;
    return { xyzIn[0] * d50[0] / originIlluminant[0],
             xyzIn[1] * d50[1] / originIlluminant[1],
             xyzIn[2] * d50[2] / originIlluminant[2] };
}

Vec3
convertD50ToLms(Adaptation adaptation)
{
    switch (adaptation) {
    case Adaptation::FullBradford:
    case Adaptation::LinearBradford:
        return kD50BradfordLms;
    case Adaptation::Cat16:
        return kD50Cat16Lms;
    case Adaptation::Xyz:
        return kD50Xyz;
    case Adaptation::Rgb:
    case Adaptation::Last:
        break;
    }
    return { 1.f, 1.f, 1.f };
}

Vec3
chromaAdaptPixel(const Vec3& in,
                 const Vec3& illuminant,
                 Adaptation adaptation,
                 float p)
{
    const float Y = in[1];

    switch (adaptation) {
    case Adaptation::FullBradford:
    case Adaptation::LinearBradford: {
        Vec3 lms = convertXyzToBradfordLms(in);
        downscaleVector(lms, Y);
        lms = bradfordAdaptD50(lms, illuminant, p, adaptation == Adaptation::FullBradford);
        upscaleVector(lms, Y);
        return convertBradfordLmsToXyz(lms);
    }
    case Adaptation::Cat16: {
        Vec3 lms = convertXyzToCat16Lms(in);
        downscaleVector(lms, Y);
        lms = cat16AdaptD50(lms, illuminant, 1.0f, true);
        upscaleVector(lms, Y);
        return convertCat16LmsToXyz(lms);
    }
    case Adaptation::Xyz: {
        Vec3 xyz = in;
        downscaleVector(xyz, Y);
        xyz = xyzAdaptD50(xyz, illuminant);
        upscaleVector(xyz, Y);
        return xyz;
    }
    case Adaptation::Rgb:
    case Adaptation::Last:
        break;
    }
    return in;
}

Vec3
xyzToLab(const Vec3& xyz)
{
    const float epsilon = 216.0f / 24389.0f;
    const float kappa = 24389.0f / 27.0f;
    Vec3 f;
    for (int i = 0; i < 3; ++i) {
        const float x = xyz[i] * kLabD50Inv[i];
        f[i] = x > epsilon ? std::cbrt(x) : (kappa * x + 16.0f) / 116.0f;
    }
    return { 116.0f * f[1] - 16.0f, 500.0f * (f[0] - f[1]), -200.0f * (f[2] - f[1]) };
}

Vec3
labToXyz(const Vec3& lab)
{
    const float scaled0 = lab[1] * (1.0f / 500.0f);
    const float scaled1 = (lab[0] + 16.0f) * (1.0f / 116.0f);
    const float scaled2 = lab[2] * (-1.0f / 200.0f);
    return { kLabD50[0] * labFInv(scaled0 + scaled1),
             kLabD50[1] * labFInv(scaled1),
             kLabD50[2] * labFInv(scaled2 + scaled1) };
}

Vec3
labToLch(const Vec3& lab)
{
    float h = std::atan2(lab[2], lab[1]);
    if (h > 0.0f) {
        h = h / k2PiF;
    } else {
        h = 1.0f - std::fabs(h) / k2PiF;
    }
    return { lab[0], std::hypot(lab[1], lab[2]), h };
}

Vec3
lchToLab(const Vec3& lch)
{
    return { lch[0], std::cos(k2PiF * lch[2]) * lch[1], std::sin(k2PiF * lch[2]) * lch[1] };
}

Vec3
d50XyzToXyY(const Vec3& sXyz)
{
    Vec3 xyz;
    for (int c = 0; c < 3; ++c) {
        // _mm_max_ps(v, 0) also turns NaN into 0.
        xyz[c] = sXyz[c] > 0.0f ? sXyz[c] : 0.0f;
    }
    const float sum = xyz[0] + xyz[1] + xyz[2];
    return { (sum > 0.0f) ? xyz[0] / sum : static_cast<float>(kD50x),
             (sum > 0.0f) ? xyz[1] / sum : static_cast<float>(kD50y),
             xyz[1] };
}

Vec3
xyYToXyz(const Vec3& xyY)
{
    if (xyY[1] == 0.0f) {
        return { 0.0f, 0.0f, 0.0f };
    }
    return { xyY[2] * xyY[0] / xyY[1], xyY[2], xyY[2] * (1.f - xyY[0] - xyY[1]) / xyY[1] };
}

Vec3
xyYToUvY(const Vec3& xyY)
{
    const float denominator = -2.f * xyY[0] + 12.f * xyY[1] + 3.f;
    return { 4.f * xyY[0] / denominator, 9.f * xyY[1] / denominator, xyY[2] };
}

Vec3
uvYToXyY(const Vec3& uvY)
{
    const float denominator = 6.0f * uvY[0] - 16.f * uvY[1] + 12.0f;
    return { 9.f * uvY[0] / denominator, 4.f * uvY[1] / denominator, uvY[2] };
}

Vec3
xyYToLuv(const Vec3& xyY)
{
    const Vec3 uvY = xyYToUvY(xyY);
    const float threshold = cube(6.0f / 29.0f);
    const float L = (uvY[2] <= threshold) ? cube(29.0f / 3.0f) * uvY[2] : 116.0f * std::cbrt(uvY[2]) - 16.f;
    return { L, 13.f * L * (uvY[0] - kLuvD50[0]), 13.f * L * (uvY[1] - kLuvD50[1]) };
}

Vec3
luvToLch(const Vec3& luv)
{
    float h = std::atan2(luv[2], luv[1]);
    h = (h < 0.f) ? k2PiF + h : h;
    return { luv[0], std::hypot(luv[2], luv[1]), h };
}

Vec3
xyYToLch(const Vec3& xyY)
{
    return luvToLch(xyYToLuv(xyY));
}

Vec3
luvToXyY(const Vec3& luv)
{
    const float threshold = 8.0f;
    Vec3 uvY;
    uvY[2] = (luv[0] <= threshold) ? luv[0] * cube(3.f / 29.f) : cube((luv[0] + 16.f) / 116.f);
    uvY[0] = luv[1] / (luv[0] * 13.f) + kLuvD50[0];
    uvY[1] = luv[2] / (luv[0] * 13.f) + kLuvD50[1];
    return uvYToXyY(uvY);
}

Vec3
lchToLuv(const Vec3& lch)
{
    return { lch[0], lch[1] * std::cos(lch[2]), lch[1] * std::sin(lch[2]) };
}

Vec3
lchToXyY(const Vec3& lch)
{
    return luvToXyY(lchToLuv(lch));
}

namespace {
    float
    scalarProduct(const Vec3& v1,
                  const Vec3& v2)
    {
        float acc = 0.f;
        for (int c = 0; c < 3; ++c) {
            acc += v1[c] * v2[c];
        }
        return acc;
    }

    float
    euclideanNorm(const Vec3& v)
    {
        return std::fmax(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), kNormMin);
    }

    // fmaxf() returns the other operand for a NaN, which is how darktable's clips drop NaNs.
    void
    clipNegNan(Vec3& v)
    {
        for (int c = 0; c < 3; ++c) {
            v[c] = std::fmax(v[c], 0.f);
        }
    }

    // _gamut_mapping().
    Vec3
    gamutMapping(const Vec3& input,
                 float compression,
                 bool clip)
    {
        const float sum = input[0] + input[1] + input[2];
        const float Y = input[1];
        Vec3 xyY = { (sum > 0.0f) ? input[0] / sum : static_cast<float>(kD50x),
                     (sum > 0.0f) ? input[1] / sum : static_cast<float>(kD50y),
                     Y };
        Vec3 uvY = xyYToUvY(xyY);
        const float delta[2] = { kLuvD50[0] - uvY[0], kLuvD50[1] - uvY[1] };
        const float Delta = Y * (delta[0] * delta[0] + delta[1] * delta[1]);
        const float correction = (compression == 0.0f) ? 0.f : std::pow(Delta, compression);
        for (int c = 0; c < 2; ++c) {
            // Stopping at D50 keeps the correction from flipping the hue to its opposite.
            const float tmp = correction * delta[c] + uvY[c];
            uvY[c] = (uvY[c] > kLuvD50[c]) ? std::fmax(tmp, kLuvD50[c]) : std::fmin(tmp, kLuvD50[c]);
        }
        xyY = uvYToXyY(uvY);
        if (clip) {
            for (int c = 0; c < 2; ++c) {
                xyY[c] = std::fmax(xyY[c], 0.0f);
            }
        }
        xyY[1] = std::fmax(xyY[1], kNormMin);
        // Z = Y (1 - x - y) / y would be negative.
        const float scale = xyY[0] + xyY[1];
        if (scale >= 1.f) {
            xyY[0] /= scale;
            xyY[1] /= scale;
        }
        return xyYToXyz(xyY);
    }

    // _luma_chroma().
    Vec3
    lumaChroma(const Vec3& input,
               const Vec3& saturation,
               const Vec3& lightness,
               SaturationVersion version)
    {
        float norm = euclideanNorm(input);
        const float avg = std::fmax((input[0] + input[1] + input[2]) / 3.0f, kNormMin);
        if (!(norm > 0.f && avg > 0.f)) {
            return input;
        }
        const float invSqrt3 = 0.5773502691896258f;
        const float mix = scalarProduct(input, lightness);
        if (version == SaturationVersion::V3) {
            norm *= invSqrt3;
        }
        Vec3 output;
        for (int c = 0; c < 3; ++c) {
            output[c] = input[c] / norm;
        }
        float coeffRatio = 0.f;
        if (version == SaturationVersion::V1) {
            for (int c = 0; c < 3; ++c) {
                coeffRatio += (1.0f - output[c]) * (1.0f - output[c]) * saturation[c];
            }
        } else {
            coeffRatio = scalarProduct(output, saturation) / 3.f;
        }
        for (int c = 0; c < 3; ++c) {
            // A ratio already negative may stay so, or bright saturated blues would turn black.
            const float minRatio = (output[c] < 0.0f) ? output[c] : 0.0f;
            output[c] = std::fmax((1.0f - output[c]) * coeffRatio + output[c], minRatio);
        }
        if (version == SaturationVersion::V3) {
            norm /= euclideanNorm(output) * invSqrt3;
        }
        norm *= std::fmax(1.f + mix / avg, 0.f);
        for (int c = 0; c < 3; ++c) {
            output[c] *= norm;
        }
        return output;
    }
} // namespace

Kernel
makeKernel(const Settings& s,
           const Mat3& rgbToXyz,
           const Mat3& xyzToRgb,
           const RawCameraWhite* camera,
           const Vec4* customWb)
{
    Kernel k;
    k.version = s.version;

    const float normR = s.normalizeR ? (s.red[0] + s.red[1] + s.red[2]) : 1.0f;
    const float normG = s.normalizeG ? (s.green[0] + s.green[1] + s.green[2]) : 1.0f;
    const float normB = s.normalizeB ? (s.blue[0] + s.blue[1] + s.blue[2]) : 1.0f;
    const float normSat = s.normalizeSaturation ? (s.saturation[0] + s.saturation[1] + s.saturation[2]) / 3.f : 0.0f;
    const float normLight = s.normalizeLightness ? (s.lightness[0] + s.lightness[1] + s.lightness[2]) / 3.f : 0.0f;
    float normGrey = s.grey[0] + s.grey[1] + s.grey[2];
    k.applyGrey = (s.grey[0] != 0.f) || (s.grey[1] != 0.f) || (s.grey[2] != 0.f);
    if (!s.normalizeGrey || normGrey == 0.f) {
        normGrey = 1.f;
    }

    Mat3 mix;
    for (int i = 0; i < 3; ++i) {
        mix[0 * 3 + i] = s.red[i] / normR;
        mix[1 * 3 + i] = s.green[i] / normG;
        mix[2 * 3 + i] = s.blue[i] / normB;
        k.saturation[i] = -s.saturation[i] + normSat;
        k.lightness[i] = s.lightness[i] - normLight;
        k.grey[i] = s.grey[i] / normGrey;
    }
    // The first algorithm weighs red and blue the other way round.
    if (s.version == SaturationVersion::V1) {
        k.saturation[0] = -s.saturation[2] + normSat;
        k.saturation[2] = -s.saturation[0] + normSat;
    }

    k.adaptation = s.adaptation;
    k.clip = s.clip;
    k.gamut = (s.gamut == 0.f) ? s.gamut : 1.f / s.gamut;

    Xy xy = { s.x, s.y };
    if (const std::optional<Xy> found = illuminantToXy(s.illuminant, camera, customWb, s.temperature, s.fluorescent, s.led)) {
        xy = *found;
    }
    // The camera illuminant is found at commit time, and so is the adaptation that suits it.
    if (s.illuminant == Illuminant::Camera) {
        k.adaptation = checkIfCloseToDaylight(xy.x, xy.y).adaptation;
    }
    k.illuminant = convertAnyXyzToLms(illuminantXyToXyz(xy.x, xy.y), k.adaptation);
    k.p = std::pow(0.818155f / k.illuminant[2], 0.0834f);

    k.rgbToXyz = rgbToXyz;
    k.xyzToRgb = xyzToRgb;
    switch (k.adaptation) {
    case Adaptation::FullBradford:
    case Adaptation::LinearBradford:
        k.rgbToLms = makeRgbToBradfordLms(rgbToXyz);
        k.mixToXyz = makeBradfordLmsToXyz(mix);
        break;
    case Adaptation::Cat16:
        k.rgbToLms = makeRgbToCat16Lms(rgbToXyz);
        k.mixToXyz = makeCat16LmsToXyz(mix);
        break;
    case Adaptation::Xyz:
        k.rgbToLms = rgbToXyz;
        k.mixToXyz = mix;
        break;
    case Adaptation::Rgb:
    case Adaptation::Last:
        k.rgbToLms = Mat3 { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
        k.mixToXyz = multiplyMatrices(rgbToXyz, mix);
        break;
    }
    return k;
}

void
processPixel(const Kernel& k,
             const float* in,
             float* out)
{
    const float minval = k.clip ? 0.0f : -FLT_MAX;
    Vec3 tempTwo = { std::fmax(in[0], minval), std::fmax(in[1], minval), std::fmax(in[2], minval) };
    Vec3 tempOne;

    switch (k.adaptation) {
    case Adaptation::FullBradford: {
        tempOne = applyMatrix(k.rgbToXyz, tempTwo);
        const float Y = tempOne[1];
        tempTwo = convertXyzToBradfordLms(tempOne);
        downscaleVector(tempTwo, Y);
        tempOne = bradfordAdaptD50(tempTwo, k.illuminant, k.p, true);
        upscaleVector(tempOne, Y);
        tempTwo = tempOne;
        break;
    }
    case Adaptation::LinearBradford:
        tempOne = applyMatrix(k.rgbToLms, tempTwo);
        tempTwo = bradfordAdaptD50(tempOne, k.illuminant, k.p, false);
        break;
    case Adaptation::Cat16:
        tempOne = applyMatrix(k.rgbToLms, tempTwo);
        tempTwo = cat16AdaptD50(tempOne, k.illuminant, 1.0f, true);
        break;
    case Adaptation::Xyz:
        tempOne = applyMatrix(k.rgbToXyz, tempTwo);
        tempTwo = xyzAdaptD50(tempOne, k.illuminant);
        break;
    case Adaptation::Rgb:
    case Adaptation::Last:
        break;
    }

    tempOne = applyMatrix(k.mixToXyz, tempTwo);
    if (k.clip) {
        clipNegNan(tempOne);
    }
    tempTwo = gamutMapping(tempOne, k.gamut, k.clip);

    switch (k.adaptation) {
    case Adaptation::FullBradford:
    case Adaptation::LinearBradford:
    case Adaptation::Cat16:
    case Adaptation::Xyz:
        tempOne = convertAnyXyzToLms(tempTwo, k.adaptation);
        break;
    case Adaptation::Rgb:
    case Adaptation::Last:
        tempOne = applyMatrix(k.xyzToRgb, tempTwo);
        break;
    }
    if (k.clip) {
        clipNegNan(tempOne);
    }

    tempTwo = lumaChroma(tempOne, k.saturation, k.lightness, k.version);
    if (k.clip) {
        clipNegNan(tempTwo);
    }

    if (k.applyGrey) {
        // The grey stays in the adaptation's space, as darktable leaves it.
        const float grey = std::fmax(scalarProduct(tempTwo, k.grey), 0.0f);
        tempTwo = { grey, grey, grey };
    } else {
        switch (k.adaptation) {
        case Adaptation::FullBradford:
        case Adaptation::LinearBradford:
        case Adaptation::Cat16:
        case Adaptation::Xyz:
            tempOne = convertAnyLmsToXyz(tempTwo, k.adaptation);
            break;
        case Adaptation::Rgb:
        case Adaptation::Last:
            tempOne = applyMatrix(k.rgbToXyz, tempTwo);
            break;
        }
        if (k.clip) {
            clipNegNan(tempOne);
        }
        tempTwo = applyMatrix(k.xyzToRgb, tempOne);
        if (k.clip) {
            clipNegNan(tempTwo);
        }
    }
    out[0] = tempTwo[0];
    out[1] = tempTwo[1];
    out[2] = tempTwo[2];
}
} // namespace RawColorCalibrationMath

NATRON_NAMESPACE_EXIT
