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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7):
 * src/iop/colorequal.c, src/iop/choleski.h and data/kernels/colorequal.cl. darktable is
 * distributed under the GNU GPL, version 3 or (at your option) any later version, and so is
 * this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2024-2025 darktable developers.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ColorEqualizer.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/GuidedFilter.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/WholeFrame.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawColorProfiles.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Filter/BloomKernels.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.h"
#endif

NATRON_NAMESPACE_ENTER

using namespace DtColorSpaces;
using namespace PassExec;

namespace {

constexpr InputSpec kColorEqualizerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kColorEqualizerSpec = {
    .id = PLUGINID_NATRON_COLOREQUALIZER,
    .label = "ColorEqualizer",
    .description = "darktable's color equalizer: hue-keyed hue, saturation and brightness edits.\n"
                   "\n"
                   "Eight nodes around the hue circle (red, orange, yellow, green, cyan, blue, lavender and "
                   "magenta) each shift the hue and scale the saturation and brightness of the colours near them; "
                   "a smooth periodic curve through the nodes sets every hue in between, which the Curves graph "
                   "shows. Near-neutral colours are left alone, fading in with saturation above Saturation "
                   "Threshold. With Use Filter on, a guided filter on chromaticity smooths the selection so "
                   "that noisy chroma does not break the edit up.\n"
                   "\n"
                   "The node works in linear Rec.2020, darktable's pipeline space, converting from and back to the "
                   "project's working space, in darktable UCS 22. Output shows the weights instead of the result. "
                   "Alpha passes through.",
    .grouping = PLUGIN_GROUP_COLOR,
    .majorVersion = PLUGIN_MAJOR_NATRON_COLOREQUALIZER,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/ColorEqualizer.png",
    .inputs = kColorEqualizerInputs,
};

constexpr ImagePipelineSpec kColorEqualizerPipeline = {
    .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Passes,
};

constexpr int kNodes = ColorEqualizerParams::kNodes;
constexpr int kSatSize = 4096;
constexpr float kAngleShift = 20.f;
constexpr float kSatEffect = 2.f;
constexpr float kBrightEffect = 8.f;
constexpr float kNormMin = 1.52587890625e-05f;
constexpr float kBlurRange = 1.0e9f;

const char* const kXyzD65Role = "cie_xyz_d65_interchange";
const int kChunk = 128;

const char* const kNodeNames[kNodes] = { "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Lavender", "Magenta" };

struct PipelineMatrices {
    Mat3 input; // Rec.2020 to XYZ D65 through CAT16
    Mat3 output; // XYZ D65 to Rec.2020
    double xyzToRec2020[9];
    double rec2020ToXyzD65[9];
    std::array<float, kGamutLutSize> gamut;
};

Mat3
toFloat(const RawColorMath::Mat3& m)
{
    Mat3 o = {};

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r][c] = (float)m[r * 3 + c];
        }
    }

    return o;
}

Mat3
toFloat(const double m[9])
{
    Mat3 o = {};

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r][c] = (float)m[r * 3 + c];
        }
    }

    return o;
}

// darktable's pipeline matrices for its default working profile, linear Rec.2020 as Little CMS
// stores it (colorants adapted to D50), and the UCS gamut boundary of that space.
const PipelineMatrices&
pipelineMatrices()
{
    static const PipelineMatrices matrices = [] {
        PipelineMatrices p = {};
        const RawColorMath::Mat3 toXyz = RawColorProfiles::toXyz(RawColorProfiles::Space::LinearRec2020);
        const RawColorMath::Mat3 fromXyz = RawColorProfiles::fromXyz(RawColorProfiles::Space::LinearRec2020);
        const std::optional<RawColorMath::Mat3> toD50 = RawColorMath::bradford(RawColorMath::d65White(), RawColorMath::lcmsD50White());
        const RawColorMath::Mat3 matrixIn = RawColorMath::multiply(*toD50, toXyz);
        const std::optional<RawColorMath::Mat3> matrixOut = RawColorMath::invert(matrixIn);

        p.input = multiply(kXyzD50ToD65Cat16, toFloat(matrixIn));
        p.output = multiply(toFloat(*matrixOut), kXyzD65ToD50Cat16);
        for (int i = 0; i < 9; ++i) {
            p.xyzToRec2020[i] = fromXyz[i];
            p.rec2020ToXyzD65[i] = toXyz[i];
        }
        buildUcsGamutLut(p.input, p.gamut.data());

        return p;
    }();

    return matrices;
}

void
multiply3(const double a[9],
          const double b[9],
          double o[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
    }
}

/**
 * @brief The conversions between the project's working space and linear Rec.2020: one matrix each
 * way when the working space has a matrix to XYZ (ACEScg when there is no project), otherwise OCIO
 * processors to and from XYZ D65 followed by the Rec.2020 matrices.
 **/
struct WorkingSpace {
    bool matrix = true;
    Mat3 toPipe = {};
    Mat3 fromPipe = {};
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr toXyz;
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr fromXyz;
};

WorkingSpace
resolveWorkingSpace(const KnobHolder* holder)
{
    WorkingSpace ws;
    const PipelineMatrices& pm = pipelineMatrices();
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    double w[9];

    if (cm && !cm->getWorkingToXYZ(w)) {
        const std::string working = project->getWorkingColorSpace();
        std::string error;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr to;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr from;
        if (IoColorSpace::processor(*project, working, kXyzD65Role, {}, &to, &error) && IoColorSpace::processor(*project, kXyzD65Role, working, {}, &from, &error)) {
            ws.matrix = false;
            ws.toXyz = to;
            ws.fromXyz = from;
            ws.toPipe = toFloat(pm.xyzToRec2020);
            ws.fromPipe = toFloat(pm.rec2020ToXyzD65);

            return ws;
        }
    }
    ColorMath::workingToXYZ(holder, w);
    const std::optional<RawColorMath::Mat3> xyzToWorking = RawColorMath::invert(RawColorMath::Mat3 { w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8] });
    double toPipe[9];
    double fromPipe[9];
    multiply3(pm.xyzToRec2020, w, toPipe);
    if (xyzToWorking) {
        multiply3(xyzToWorking->data(), pm.rec2020ToXyzD65, fromPipe);
    } else {
        std::copy(pm.rec2020ToXyzD65, pm.rec2020ToXyzD65 + 9, fromPipe);
    }
    ws.matrix = true;
    ws.toPipe = toFloat(toPipe);
    ws.fromPipe = toFloat(fromPipe);

    return ws;
}

struct Preset {
    const char* id;
    const char* label;
    ColorEqualizerParams params;
};

ColorEqualizerParams
makePreset(float threshold,
           float smoothingHue,
           float contrast,
           float paramSize,
           float hueShift,
           std::array<float, kNodes> saturation,
           std::array<float, kNodes> hue,
           std::array<float, kNodes> brightness)
{
    ColorEqualizerParams p;

    p.threshold = threshold;
    p.smoothingHue = smoothingHue;
    p.contrast = contrast;
    p.whiteLevel = 1.f;
    p.chromaSize = 1.5f;
    p.paramSize = paramSize;
    p.useFilter = true;
    p.hueShift = hueShift;
    for (int i = 0; i < kNodes; ++i) {
        p.saturation[i] = saturation[i];
        p.hue[i] = hue[i];
        p.brightness[i] = brightness[i];
    }

    return p;
}

const std::vector<Preset>&
presets()
{
    static const std::vector<Preset> list = {
        { "bleachBypass", "Bleach Bypass",
          makePreset(0.f, 1.f, 0.f, 1.f, 0.f,
                     { 1.f - 0.2215f, 1.f - 0.1772f, 1.f - 0.3861f, 1.f - 0.3924f, 1.f - 0.4557f, 1.f - 0.4177f, 1.f - 0.2468f, 1.f - 0.2532f },
                     { 15.46f, 0.f, -2.21f, 28.72f, 16.57f, 0.f, 0.f, 0.f },
                     { 1.f - 0.25f, 1.f - 0.25f, 1.f - 0.25f, 1.f - 0.35f, 1.f - 0.35f, 1.f - 0.25f, 1.f - 0.25f, 1.f - 0.25f }) },
        { "kodachrome64Like", "Kodachrome 64 Like",
          makePreset(0.193f, 1.f, -0.345f, 19.f, 0.f,
                     { 1.f + 0.2390f, 1.f + 0.0377f, 1.f - 0.1761f, 1.f - 0.1635f, 1.f - 0.0126f, 1.f + 0.0126f, 1.f - 0.f, 1.f + 0.1384f },
                     { -2.20f, -17.56f, -3.29f, 32.93f, 14.27f, 6.59f, -7.68f, 0.f },
                     { 1.f - 0.0063f, 1.f + 0.1824f, 1.f - 0.1950f, 1.f - 0.2390f, 1.f - 0.2453f, 1.f + 0.0377f, 1.f - 0.1572f, 1.f - 0.1384f }) },
        { "kodakPortra400Like", "Kodak Portra 400 Like",
          makePreset(0.199f, 1.f, -0.375f, 1.f, -23.f,
                     { 1.f + 0.0692f, 1.f + 0.0503f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f },
                     { 9.88f, -4.39f, 15.37f, 8.78f, 2.20f, -19.76f, -3.29f, 0.f },
                     { 1.f + 0.0881f, 1.f + 0.0629f, 1.f + 0.0629f, 1.f - 0.1069f, 1.f - 0.1069f, 1.f - 0.1006f, 1.f - 0.0189f, 1.f - 0.f }) },
        { "tealAndOrange", "Teal & Orange",
          makePreset(0.184f, 0.52f, -0.3f, 1.f, 0.f,
                     { 1.f + 0.1572f, 1.f - 0.0063f, 1.f - 0.3270f, 1.f - 0.0377f, 1.f, 1.f + 0.0126f, 1.f, 1.f },
                     { 15.37f, -24.15f, 75.74f, 42.81f, 2.20f, -36.22f, 2.20f, 42.81f },
                     { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f }) },
    };

    return list;
}

inline float
deg2rad(float d)
{
    return d * kPi / 180.f;
}

inline float
hueNode(int k,
        float hueShift)
{
    return ColorEqualizer::conventionalHueToUcs((float)k * 360.f / (float)kNodes + hueShift);
}

// darktable's choleski.h: the least-squares solution of A x = y through the normal equations,
// written into y only when every step is well defined.
bool
pseudoSolve(const float* a,
            float* y,
            int n)
{
    std::vector<float> aSq((std::size_t)n * n, 0.f);
    std::vector<float> ySq(n, 0.f);
    std::vector<float> l((std::size_t)n * n, 0.f);
    std::vector<float> x(n, 0.f);

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            float sum = 0.f;
            for (int k = 0; k < n; ++k) {
                sum += a[k * n + i] * a[k * n + j];
            }
            aSq[i * n + j] = sum;
        }
        float sum = 0.f;
        for (int k = 0; k < n; ++k) {
            sum += a[k * n + i] * y[k];
        }
        ySq[i] = sum;
    }

    if (aSq[0] <= 0.f) {
        return false;
    }
    bool valid = true;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            float sum = 0.f;
            for (int k = 0; k < j; ++k) {
                sum += l[i * n + k] * l[j * n + k];
            }
            if (i == j) {
                const float temp = aSq[i * n + i] - sum;
                if (temp < 0.f) {
                    valid = false;
                    l[i * n + j] = NAN;
                } else {
                    l[i * n + j] = std::sqrt(aSq[i * n + i] - sum);
                }
            } else {
                const float temp = l[j * n + j];
                if (temp == 0.f) {
                    valid = false;
                    l[i * n + j] = NAN;
                } else {
                    l[i * n + j] = (aSq[i * n + j] - sum) / temp;
                }
            }
        }
    }
    if (!valid) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        float sum = ySq[i];
        for (int j = 0; j < i; ++j) {
            sum -= l[i * n + j] * x[j];
        }
        if (l[i * n + i] == 0.f) {
            return false;
        }
        x[i] = sum / l[i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        float sum = x[i];
        for (int j = n - 1; j > i; --j) {
            sum -= l[j * n + i] * ySq[j];
        }
        if (l[i * n + i] == 0.f) {
            return false;
        }
        ySq[i] = sum / l[i * n + i];
    }
    std::copy(ySq.begin(), ySq.end(), y);

    return true;
}

inline float
scaling(float sigma)
{
    return std::max(1.f, std::min(4.f, std::floor(sigma - 1.5f)));
}

// dt_gaussian_mean_blur(), in place.
void
meanBlur(float* buf,
         int width,
         int height,
         int channels,
         float sigma,
         const EdgeAware::ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const float mins[4] = { -kBlurRange, -kBlurRange, -kBlurRange, -kBlurRange };
    const float maxs[4] = { kBlurRange, kBlurRange, kBlurRange, kBlurRange };
    std::vector<float> tmp(buf, buf + (std::size_t)width * height * channels);

    EdgeAware::recursiveGaussianBlur(tmp.data(), buf, width, height, channels, sigma, mins, maxs, parallel);
}

void
forPixels(std::size_t count,
          const EdgeAware::ParallelFor& parallel,
          const std::function<void(std::size_t)>& body)
{
    const int blockSize = 16384;
    const int blocks = (int)((count + blockSize - 1) / blockSize);

    parallel(blocks, [&](int b) {
        const std::size_t begin = (std::size_t)b * blockSize;
        const std::size_t end = std::min(count, begin + blockSize);
        for (std::size_t k = begin; k < end; ++k) {
            body(k);
        }
    });
}

std::vector<float>
initCovariance(const float* uv,
               std::size_t pixels,
               const EdgeAware::ParallelFor& parallel)
{
    std::vector<float> cov(pixels * 4);

    forPixels(pixels, parallel, [&](std::size_t k) {
        cov[4 * k + 0] = uv[2 * k + 0] * uv[2 * k + 0];
        cov[4 * k + 1] = cov[4 * k + 2] = uv[2 * k] * uv[2 * k + 1];
        cov[4 * k + 3] = uv[2 * k + 1] * uv[2 * k + 1];
    });

    return cov;
}

void
finishCovariance(const float* uv,
                 float* cov,
                 std::size_t pixels,
                 const EdgeAware::ParallelFor& parallel)
{
    forPixels(pixels, parallel, [&](std::size_t k) {
        cov[4 * k + 0] -= uv[2 * k + 0] * uv[2 * k + 0];
        cov[4 * k + 1] -= uv[2 * k + 0] * uv[2 * k + 1];
        cov[4 * k + 2] -= uv[2 * k + 0] * uv[2 * k + 1];
        cov[4 * k + 3] -= uv[2 * k + 1] * uv[2 * k + 1];
    });
}

// a = c Σ⁻¹ with Σ the covariance plus eps on the diagonal, 0 where Σ is singular.
inline void
solveA(const float* cov,
       const float* c,
       float eps,
       float* a)
{
    const float s0 = cov[0] + eps;
    const float s1 = cov[1];
    const float s2 = cov[2];
    const float s3 = cov[3] + eps;
    const float det = s0 * s3 - s1 * s2;

    if (std::fabs(det) > 4.f * FLT_EPSILON) {
        const float i0 = s3 / det;
        const float i1 = -s1 / det;
        const float i2 = -s2 / det;
        const float i3 = s0 / det;
        a[0] = c[0] * i0 + c[1] * i1;
        a[1] = c[0] * i2 + c[1] * i3;
        a[2] = c[2] * i0 + c[3] * i1;
        a[3] = c[2] * i2 + c[3] * i3;
    } else {
        a[0] = a[1] = a[2] = a[3] = 0.f;
    }
}

// darktable's _prefilter_chromaticity(): UV smoothed by a guided filter guided by itself.
void
prefilterChromaticity(const ColorEqualizerData& data,
                      float* uv,
                      const float* saturation,
                      int width,
                      int height,
                      float sigma,
                      float eps,
                      float satShift,
                      const EdgeAware::ParallelFor& parallel)
{
    const std::size_t pixels = (std::size_t)width * height;
    const float scale = scaling(sigma);
    const float gsigma = std::max(0.2f, sigma / scale);
    const int dsHeight = (int)(height / scale);
    const int dsWidth = (int)(width / scale);
    if ((dsWidth < 1) || (dsHeight < 1)) {
        return;
    }
    const std::size_t dsPixels = (std::size_t)dsWidth * dsHeight;
    const bool resized = (width != dsWidth) || (height != dsHeight);

    std::vector<float> dsStorage;
    float* dsUV = uv;
    if (resized) {
        dsStorage.resize(dsPixels * 2);
        dsUV = dsStorage.data();
        EdgeAware::interpolateBilinear(uv, width, height, dsUV, dsWidth, dsHeight, 2, parallel);
    }
    std::vector<float> cov = initCovariance(dsUV, dsPixels, parallel);
    meanBlur(dsUV, dsWidth, dsHeight, 2, gsigma, parallel);
    meanBlur(cov.data(), dsWidth, dsHeight, 4, gsigma, parallel);
    finishCovariance(dsUV, cov.data(), dsPixels, parallel);

    std::vector<float> a(dsPixels * 4);
    std::vector<float> b(dsPixels * 2);
    forPixels(dsPixels, parallel, [&](std::size_t k) {
        solveA(&cov[4 * k], &cov[4 * k], eps, &a[4 * k]);
        b[2 * k + 0] = dsUV[2 * k + 0] - a[4 * k + 0] * dsUV[2 * k + 0] - a[4 * k + 1] * dsUV[2 * k + 1];
        b[2 * k + 1] = dsUV[2 * k + 1] - a[4 * k + 2] * dsUV[2 * k + 0] - a[4 * k + 3] * dsUV[2 * k + 1];
    });
    cov = std::vector<float>();
    dsStorage = std::vector<float>();
    meanBlur(a.data(), dsWidth, dsHeight, 4, gsigma, parallel);
    meanBlur(b.data(), dsWidth, dsHeight, 2, gsigma, parallel);
    if (resized) {
        std::vector<float> fa(pixels * 4);
        std::vector<float> fb(pixels * 2);
        EdgeAware::interpolateBilinear(a.data(), dsWidth, dsHeight, fa.data(), width, height, 4, parallel);
        EdgeAware::interpolateBilinear(b.data(), dsWidth, dsHeight, fb.data(), width, height, 2, parallel);
        a.swap(fa);
        b.swap(fb);
    }

    forPixels(pixels, parallel, [&](std::size_t k) {
        const float u = uv[2 * k + 0];
        const float v = uv[2 * k + 1];
        const float cv0 = a[4 * k + 0] * u + a[4 * k + 1] * v + b[2 * k + 0];
        const float cv1 = a[4 * k + 2] * u + a[4 * k + 3] * v + b[2 * k + 1];
        const float w = ColorEqualizer::satWeight(data, saturation[k] - satShift);
        uv[2 * k + 0] = w * (cv0 - u) + u;
        uv[2 * k + 1] = w * (cv1 - v) + v;
    });
}

// darktable's _guide_with_chromaticity(): the saturation and brightness corrections smoothed by a
// guided filter guided by UV.
void
guideWithChromaticity(const ColorEqualizerData& data,
                      float* uv,
                      float* corrections,
                      const float* saturation,
                      float* bCorrections,
                      const float* gradients,
                      int width,
                      int height,
                      float sigma,
                      float eps,
                      float brightShift,
                      float satShift,
                      const EdgeAware::ParallelFor& parallel)
{
    const std::size_t pixels = (std::size_t)width * height;
    const float scale = scaling(sigma);
    const float gsigma = std::max(0.2f, sigma / scale);
    const int dsHeight = (int)(height / scale);
    const int dsWidth = (int)(width / scale);
    if ((dsWidth < 1) || (dsHeight < 1)) {
        return;
    }
    const std::size_t dsPixels = (std::size_t)dsWidth * dsHeight;
    const bool resized = (width != dsWidth) || (height != dsHeight);

    std::vector<float> dsUVStorage;
    std::vector<float> dsCorrStorage;
    std::vector<float> dsBStorage;
    float* dsUV = uv;
    float* dsCorr = corrections;
    float* dsB = bCorrections;
    if (resized) {
        dsUVStorage.resize(dsPixels * 2);
        dsCorrStorage.resize(dsPixels * 2);
        dsBStorage.resize(dsPixels);
        dsUV = dsUVStorage.data();
        dsCorr = dsCorrStorage.data();
        dsB = dsBStorage.data();
        EdgeAware::interpolateBilinear(uv, width, height, dsUV, dsWidth, dsHeight, 2, parallel);
        EdgeAware::interpolateBilinear(corrections, width, height, dsCorr, dsWidth, dsHeight, 2, parallel);
        EdgeAware::interpolateBilinear(bCorrections, width, height, dsB, dsWidth, dsHeight, 1, parallel);
    }
    std::vector<float> cov = initCovariance(dsUV, dsPixels, parallel);
    std::vector<float> corr(dsPixels * 4);
    forPixels(dsPixels, parallel, [&](std::size_t k) {
        corr[4 * k + 0] = dsUV[2 * k + 0] * dsCorr[2 * k + 1];
        corr[4 * k + 1] = dsUV[2 * k + 1] * dsCorr[2 * k + 1];
        corr[4 * k + 2] = dsUV[2 * k + 0] * dsB[k];
        corr[4 * k + 3] = dsUV[2 * k + 1] * dsB[k];
    });
    meanBlur(dsUV, dsWidth, dsHeight, 2, gsigma, parallel);
    meanBlur(cov.data(), dsWidth, dsHeight, 4, gsigma, parallel);
    meanBlur(dsCorr, dsWidth, dsHeight, 2, gsigma, parallel);
    meanBlur(dsB, dsWidth, dsHeight, 1, 0.1f * gsigma, parallel);
    meanBlur(corr.data(), dsWidth, dsHeight, 4, gsigma, parallel);
    finishCovariance(dsUV, cov.data(), dsPixels, parallel);

    std::vector<float> a(dsPixels * 4);
    std::vector<float> b(dsPixels * 2);
    forPixels(dsPixels, parallel, [&](std::size_t k) {
        float* c = &corr[4 * k];
        c[0] -= dsUV[2 * k + 0] * dsCorr[2 * k + 1];
        c[1] -= dsUV[2 * k + 1] * dsCorr[2 * k + 1];
        c[2] -= dsUV[2 * k + 0] * dsB[k];
        c[3] -= dsUV[2 * k + 1] * dsB[k];
        solveA(&cov[4 * k], c, eps, &a[4 * k]);
        b[2 * k + 0] = dsCorr[2 * k + 1] - a[4 * k + 0] * dsUV[2 * k + 0] - a[4 * k + 1] * dsUV[2 * k + 1];
        b[2 * k + 1] = dsB[k] - a[4 * k + 2] * dsUV[2 * k + 0] - a[4 * k + 3] * dsUV[2 * k + 1];
    });
    cov = std::vector<float>();
    corr = std::vector<float>();
    dsUVStorage = std::vector<float>();
    dsCorrStorage = std::vector<float>();
    dsBStorage = std::vector<float>();
    meanBlur(a.data(), dsWidth, dsHeight, 4, gsigma, parallel);
    meanBlur(b.data(), dsWidth, dsHeight, 2, gsigma, parallel);
    if (resized) {
        std::vector<float> fa(pixels * 4);
        std::vector<float> fb(pixels * 2);
        EdgeAware::interpolateBilinear(a.data(), dsWidth, dsHeight, fa.data(), width, height, 4, parallel);
        EdgeAware::interpolateBilinear(b.data(), dsWidth, dsHeight, fb.data(), width, height, 2, parallel);
        a.swap(fa);
        b.swap(fb);
    }

    forPixels(pixels, parallel, [&](std::size_t k) {
        const float u = uv[2 * k + 0];
        const float v = uv[2 * k + 1];
        const float cv0 = a[4 * k + 0] * u + a[4 * k + 1] * v + b[2 * k + 0];
        const float cv1 = a[4 * k + 2] * u + a[4 * k + 3] * v + b[2 * k + 1];
        corrections[2 * k + 1] = 1.f + (cv0 - 1.f) * ColorEqualizer::satWeight(data, saturation[k] - satShift);
        const float gradientWeight = 1.f - std::min(1.f, std::max(0.f, gradients[k]));
        bCorrections[k] = cv1 * gradientWeight * ColorEqualizer::satWeight(data, saturation[k] - brightShift);
    });
}

inline float
scharrGradient(const float* p,
               int w)
{
    const float gx = 47.f / 255.f * (p[-w - 1] - p[-w + 1] + p[w - 1] - p[w + 1]) + 162.f / 255.f * (p[-1] - p[1]);
    const float gy = 47.f / 255.f * (p[-w - 1] - p[w - 1] + p[-w + 1] - p[w + 1]) + 162.f / 255.f * (p[-w] - p[w]);

    return fastHypot(gx, gy);
}

// darktable's gamut_map_HSB().
inline void
gamutMapHSB(float hsb[3],
            const float* gamut,
            float white)
{
    float jch[3];

    ucsHSBToJCH(hsb, jch);
    const float maxColorfulness = lookupGamut(gamut, jch[2]);
    const float maxChroma = ucsChroma(jch[0] * white, white, maxColorfulness);
    const float boundaryJch[3] = { jch[0], maxChroma, jch[2] };
    float boundary[3];
    ucsJCHToHSB(boundaryJch, boundary);
    hsb[1] = softClip(hsb[1], 0.8f * boundary[1], boundary[1]);
}

KnobGroupPtr
addTab(KnobHolder* holder,
       const KnobPagePtr& page,
       const char* name,
       const char* label)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(holder, EffectInstance::tr(label).toStdString());

    group->setName(name);
    group->setAsTab();
    page->addKnob(group);

    return group;
}

KnobDoublePtr
addDouble(KnobHolder* holder,
          const KnobGroupPtr& tab,
          const std::string& name,
          const std::string& label,
          const std::string& hint,
          double defaultValue,
          double minimum,
          double maximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(minimum);
    knob->setDisplayMaximum(maximum);
    knob->setDecimals(4);
    tab->addKnob(knob);

    return knob;
}

// The curve graph's x is the hue in degrees as the sliders read it, its y the gain, a hue shift
// shown as 1 + shift / 180°.
constexpr double kCurveHueMax = 360.;
constexpr double kCurveYMax = 2.;
const int kCurveSamples = 360;
const int kRampSlices = 180;
const float kRampSaturation = 0.6f;
const float kRampBrightness = 0.65f;

inline float
encodeSrgb(float v)
{
    v = std::min(1.f, std::max(0.f, v));

    return (v <= 0.0031308f) ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

// The display colour of a hue in degrees, as darktable's graph draws it: HSB at the ramp's
// saturation and brightness, gamut-mapped, to sRGB.
void
rampColour(float degrees,
           float rgb[3])
{
    static const Mat3 kXyzD65ToLinearSrgb = { { { 3.2404542f, -1.5371385f, -0.4985314f },
                                                { -0.9692660f, 1.8760108f, 0.0415560f },
                                                { 0.0556434f, -0.2040259f, 1.0572252f } } };
    float h = ColorEqualizer::conventionalHueToUcs(degrees);
    while (h >= kPi) {
        h -= kTwoPi;
    }
    while (h < -kPi) {
        h += kTwoPi;
    }
    float hsb[3] = { h, kRampSaturation, kRampBrightness };
    gamutMapHSB(hsb, pipelineMatrices().gamut.data(), 1.f);
    float jch[3];
    float xyY[3];
    float xyz[3];
    float lin[3];
    ucsHSBToJCH(hsb, jch);
    ucsJCHToXyY(jch, 1.f, xyY);
    xyYToXyz(xyY, xyz);
    apply(kXyzD65ToLinearSrgb, xyz, lin);
    for (int c = 0; c < 3; ++c) {
        rgb[c] = encodeSrgb(lin[c]);
    }
}

inline float
lutAtDegrees(const std::array<float, kGamutLutSize>& lut,
             float degrees)
{
    float h = ColorEqualizer::conventionalHueToUcs(degrees);
    while (h >= kPi) {
        h -= kTwoPi;
    }
    while (h < -kPi) {
        h += kTwoPi;
    }

    return lookupGamut(lut.data(), h);
}

} // anonymous namespace

ColorEqualizer::ColorEqualizer(NodePtr node)
    : StagedImageEffect(node)
{
}

ColorEqualizer::~ColorEqualizer()
{
}

const NodeSpec*
ColorEqualizer::nodeSpec() const
{
    return staticNodeSpec<kColorEqualizerSpec>();
}

const ImagePipelineSpec&
ColorEqualizer::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kColorEqualizerPipeline, kColorEqualizerSpec>();
}

std::string
ColorEqualizer::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kColorEqualizerInputSource:
        return tr("The image to equalize.").toStdString();
    case kColorEqualizerInputMask:
        return tr("The mask the result is mixed with the source through.").toStdString();
    }

    return std::string();
}

void
ColorEqualizer::addAcceptedComponents(int inputNb,
                                      std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

const char*
ColorEqualizer::nodeName(int node)
{
    return ((node >= 0) && (node < kNodes)) ? kNodeNames[node] : "";
}

float
ColorEqualizer::conventionalHueToUcs(float degrees)
{
    return deg2rad(degrees + kAngleShift);
}

void
ColorEqualizer::periodicRbfInterpolate(float nodes[ColorEqualizerParams::kNodes],
                                       float smoothing,
                                       float* lut,
                                       float hueShift,
                                       bool clip)
{
    const int m = (int)std::ceil(3.f * std::sqrt(smoothing));
    float a[kNodes][kNodes] = {};

    for (int i = 0; i < kNodes; ++i) {
        for (int j = 0; j < kNodes; ++j) {
            for (int l = 0; l < m; ++l) {
                a[i][j] += std::exp(-(float)l * (float)l / smoothing) * std::cos((float)l * std::fabs(hueNode(i, hueShift) - hueNode(j, hueShift)));
            }
            a[i][j] = std::exp(a[i][j]);
        }
    }
    pseudoSolve(&a[0][0], nodes, kNodes);

    for (int i = 0; i < kGamutLutSize; ++i) {
        const float hue = deg2rad((float)i * 360.f / (float)kGamutLutSize) - kPi;
        lut[i] = 0.f;
        for (int k = 0; k < kNodes; ++k) {
            float result = 0.f;
            for (int l = 0; l < m; ++l) {
                result += std::exp(-(float)l * (float)l / smoothing) * std::cos((float)l * std::fabs(hue - hueNode(k, hueShift)));
            }
            lut[i] += nodes[k] * std::exp(result);
        }
        if (clip) {
            lut[i] = std::max(0.f, lut[i]);
        }
    }
}

ColorEqualizerData
ColorEqualizer::commit(const ColorEqualizerParams& p)
{
    ColorEqualizerData d;

    d.whiteLevel = std::exp2(p.whiteLevel);
    d.chromaSize = p.chromaSize;
    d.chromaFeathering = std::pow(10.f, -5.f);
    d.paramSize = p.paramSize;
    d.paramFeathering = std::pow(10.f, -6.f);
    d.useFilter = p.useFilter;
    d.threshold = -0.015f + 0.3f * square(5.f * p.threshold);
    d.output = p.output;

    float values[kNodes];
    std::copy(p.saturation, p.saturation + kNodes, values);
    periodicRbfInterpolate(values, kPi, d.lutSaturation.data(), p.hueShift, true);
    for (int i = 0; i < kNodes; ++i) {
        values[i] = deg2rad(p.hue[i]);
    }
    periodicRbfInterpolate(values, 1.f / p.smoothingHue * kPi, d.lutHue.data(), p.hueShift, false);
    std::copy(p.brightness, p.brightness + kNodes, values);
    d.maxBrightness = 1.f;
    for (int i = 0; i < kNodes; ++i) {
        d.maxBrightness = std::max(d.maxBrightness, values[i]);
    }
    periodicRbfInterpolate(values, kPi, d.lutBrightness.data(), p.hueShift, true);

    d.satWeights.resize(ColorEqualizerData::kSatWeights);
    const double factor = -60.0 - 40.0 * (double)p.contrast;
    for (int i = -kSatSize; i < kSatSize + 1; ++i) {
        const double val = 0.5 / (double)kSatSize * (double)i;
        d.satWeights[i + kSatSize] = (float)(1.0 / (1.0 + std::exp(factor * val)));
    }

    return d;
}

ColorEqualizerScaled
ColorEqualizer::scaled(const ColorEqualizerData& d,
                       float scale)
{
    ColorEqualizerScaled s;

    s.white = yToUcsLStar(d.whiteLevel);
    s.satShift = d.threshold;
    s.brightShift = s.satShift + 0.01f * d.maxBrightness * std::min(5.f, std::sqrt(d.paramSize));
    s.gradientAmp = 4.f * std::sqrt(d.maxBrightness) * square(scale);
    s.hueSigma = 0.5f * d.chromaSize * scale;
    s.paramSigma = 0.5f * d.paramSize * scale;
    s.satSigma = std::max(0.5f, scale);
    s.scharrSigma = std::max(0.5f, scale);

    return s;
}

float
ColorEqualizer::satWeight(const ColorEqualizerData& data,
                          float sat)
{
    const float isat = (float)kSatSize * (1.f + std::min(1.f - 1.f / kSatSize, std::max(-1.f, sat)));
    const float base = std::floor(isat);
    const int i = (int)base;
    const float* w = data.satWeights.data();

    return w[i] + (isat - base) * (w[i + 1] - w[i]);
}

int
ColorEqualizer::halo(const ColorEqualizerParams& params,
                     float scale)
{
    const ColorEqualizerScaled s = scaled(commit(params), scale);

    if (!params.useFilter) {
        return (int)std::ceil(3.f * s.satSigma) + 1;
    }
    const float reach = 3.f * (2.f * s.hueSigma + 2.f * s.paramSigma + s.satSigma + s.scharrSigma);

    return std::min(1 << 16, (int)std::ceil(reach) + 10);
}

void
ColorEqualizer::process(const ColorEqualizerData& data,
                        float scale,
                        const float* rgba,
                        float* out,
                        int width,
                        int height,
                        const ColorEqualizerFrame& frame,
                        const EdgeAware::ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const PipelineMatrices& pm = pipelineMatrices();
    const ColorEqualizerScaled s = scaled(data, scale);
    const std::size_t pixels = (std::size_t)width * height;

    // Every plane is top-down as darktable's, the buffers bottom-up.
    std::vector<float> uv(pixels * 2);
    std::vector<float> saturation(pixels);
    std::vector<float> lScharr(pixels);
    std::vector<float> hsb(pixels * 3);
    std::vector<float> corrections(pixels * 2);
    std::vector<float> bCorrections(pixels);
    const auto bufferIndex = [width, height](std::size_t k) {
        const std::size_t row = k / width;
        const std::size_t col = k % width;

        return (std::size_t)(height - 1 - row) * width + col;
    };

    forPixels(pixels, parallel, [&](std::size_t k) {
        const float* pix = rgba + 4 * bufferIndex(k);
        float xyz[3];
        float xyY[3];
        apply(pm.input, pix, xyz);
        d65XyzToXyY(xyz, xyY);
        const float dmin = std::min(pix[0], std::min(pix[1], pix[2]));
        const float dmax = std::max(pix[0], std::max(pix[1], pix[2]));
        const float delta = dmax - dmin;
        saturation[k] = ((dmax > kNormMin) && (delta > kNormMin)) ? delta / dmax : 0.f;
        xyYToUcsUV(xyY, &uv[2 * k]);
        lScharr[k] = yToUcsLStar(xyY[2]);
    });
    meanBlur(saturation.data(), width, height, 1, s.satSigma, parallel);

    if (data.useFilter) {
        prefilterChromaticity(data, uv.data(), saturation.data(), width, height, s.hueSigma, data.chromaFeathering, s.satShift, parallel);
    }

    const bool gradients = data.useFilter && (width >= 3) && (height >= 3);
    forPixels(pixels, parallel, [&](std::size_t k) {
        const int row = (int)(k / width);
        const int col = (int)(k % width);
        float jch[3];
        ucsLUVToJCH(lScharr[k], s.white, &uv[2 * k], jch);
        ucsJCHToHSB(jch, &hsb[3 * k]);
        if (data.useFilter) {
            float g = 0.f;
            if (gradients) {
                const int vrow = std::min(height - 2, std::max(1, row));
                const int vcol = std::min(width - 2, std::max(1, col));
                g = s.gradientAmp * square(std::max(0.f, scharrGradient(&saturation[(std::size_t)vrow * width + vcol], width) - 0.02f));
            }
            lScharr[k] = g;
        }
        if (jch[1] > kNormMin) {
            const float hue = hsb[3 * k + 0];
            const float sat = hsb[3 * k + 1];
            corrections[2 * k + 0] = lookupGamut(data.lutHue.data(), hue);
            corrections[2 * k + 1] = lookupGamut(data.lutSaturation.data(), hue);
            bCorrections[k] = sat * (lookupGamut(data.lutBrightness.data(), hue) - 1.f);
        } else {
            corrections[2 * k + 0] = 0.f;
            corrections[2 * k + 1] = 1.f;
            bCorrections[k] = 0.f;
        }
    });

    if (data.useFilter) {
        meanBlur(lScharr.data(), width, height, 1, s.scharrSigma, parallel);
        guideWithChromaticity(data, uv.data(), corrections.data(), saturation.data(), bCorrections.data(), lScharr.data(), width, height,
                              s.paramSigma, data.paramFeathering, s.brightShift, s.satShift, parallel);
    }

    if (data.output == ColorEqualizerParams::eOutputResult) {
        forPixels(pixels, parallel, [&](std::size_t k) {
            const std::size_t o = 4 * bufferIndex(k);
            float h[3] = { hsb[3 * k + 0], hsb[3 * k + 1], hsb[3 * k + 2] };
            h[0] += corrections[2 * k + 0];
            h[1] = std::max(0.f, h[1] * (1.f + kSatEffect * (corrections[2 * k + 1] - 1.f)));
            h[2] = std::max(0.f, h[2] * (1.f + kBrightEffect * bCorrections[k]));
            gamutMapHSB(h, pm.gamut.data(), s.white);
            float jch[3];
            float xyY[3];
            float xyz[3];
            ucsHSBToJCH(h, jch);
            ucsJCHToXyY(jch, s.white, xyY);
            xyYToXyz(xyY, xyz);
            apply(pm.output, xyz, out + o);
            out[o + 3] = rgba[o + 3];
        });

        return;
    }

    const int mode = data.output;
    const bool gradMode = (mode == ColorEqualizerParams::eOutputSaturationThreshold) || (mode == ColorEqualizerParams::eOutputBrightnessThreshold);
    const float shift = (mode == ColorEqualizerParams::eOutputSaturationThreshold) ? s.satShift : s.brightShift;
    forPixels(pixels, parallel, [&](std::size_t k) {
        const std::size_t o = 4 * bufferIndex(k);
        const float val = std::sqrt(hsb[3 * k + 2] * s.white);
        float corr = 0.f;
        switch (mode) {
        case ColorEqualizerParams::eOutputBrightness:
            corr = kBrightEffect * bCorrections[k];
            break;
        case ColorEqualizerParams::eOutputSaturation:
            corr = kSatEffect * (corrections[2 * k + 1] - 1.f);
            break;
        case ColorEqualizerParams::eOutputBrightnessThreshold:
            corr = satWeight(data, saturation[k] - s.brightShift) - 0.5f;
            break;
        case ColorEqualizerParams::eOutputSaturationThreshold:
            corr = satWeight(data, saturation[k] - s.satShift) - 0.5f;
            break;
        default:
            corr = 0.2f * corrections[2 * k + 0];
            break;
        }
        const bool neg = corr < 0.f;
        corr = std::fabs(corr);
        corr = (corr < 2e-3f) ? 0.f : corr;
        out[o + 0] = std::max(0.f, neg ? val - corr : val);
        out[o + 1] = std::max(0.f, val - corr);
        out[o + 2] = std::max(0.f, neg ? val : val - corr);
        out[o + 3] = rgba[o + 3];
        // The halo-suppression gradient, which holds L* when the filter is off.
        if ((mode == ColorEqualizerParams::eOutputBrightness) && data.useFilter && (lScharr[k] > 0.1f)) {
            out[o + 0] = out[o + 2] = 0.f;
            out[o + 1] = lScharr[k];
        }
        if (gradMode && (frame.width > 0) && (frame.height > 0)) {
            // The weighting curve over the region of definition, as darktable draws it over the image.
            const int row = (int)(k / width);
            const int col = (int)(k % width);
            const int ax = frame.x + col;
            const int dtRow = frame.height - 1 - (frame.y + (height - 1 - row));
            const float scaler = (float)frame.width * 16.f;
            const float eps = 0.5f / (float)frame.height;
            for (int sub = 0; sub < 16; ++sub) {
                const float weight = satWeight(data, (float)(16 * ax + sub) / scaler - shift);
                if ((weight > eps) && (weight < 1.f - eps) && ((int)((1.f - weight) * (float)(frame.height - 1)) == dtRow)) {
                    out[o + 0] = out[o + 2] = 0.f;
                    out[o + 1] = 1.f;
                    break;
                }
            }
        }
    });
}

bool
ColorEqualizer::pipeMatrices(const KnobHolder* holder,
                             Mat3* toPipe,
                             Mat3* fromPipe)
{
    const WorkingSpace ws = resolveWorkingSpace(holder);

    if (!ws.matrix) {
        return false;
    }
    *toPipe = ws.toPipe;
    *fromPipe = ws.fromPipe;

    return true;
}

int
ColorEqualizer::presetCount()
{
    return (int)presets().size();
}

const char*
ColorEqualizer::presetLabel(int index)
{
    return ((index >= 0) && (index < presetCount())) ? presets()[index].label : "";
}

ColorEqualizerParams
ColorEqualizer::presetParams(int index)
{
    return ((index >= 0) && (index < presetCount())) ? presets()[index].params : ColorEqualizerParams();
}

void
ColorEqualizer::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();
    const ColorEqualizerParams defaults;

    {
        std::vector<ChoiceOption> options;
        for (const Preset& preset : presets()) {
            options.push_back(ChoiceOption(preset.id, tr(preset.label).toStdString(), std::string()));
        }
        KnobChoicePtr choice = AppManager::createKnob<KnobChoice>(this, tr("Preset").toStdString());
        choice->setName(kColorEqualizerParamPreset);
        choice->setHintToolTip(tr("One of darktable's built-in presets. Apply Preset writes it into every control but Output.").toStdString());
        choice->populateChoices(options);
        choice->setDefaultValue(0);
        choice->setAnimationEnabled(false);
        choice->setEvaluateOnChange(false);
        choice->setAddNewLine(false);
        page->addKnob(choice);
        _preset = choice;

        KnobButtonPtr apply = AppManager::createKnob<KnobButton>(this, tr("Apply Preset").toStdString());
        apply->setName(kColorEqualizerParamApplyPreset);
        apply->setHintToolTip(tr("Sets every control but Output to the preset's values.").toStdString());
        apply->setEvaluateOnChange(false);
        page->addKnob(apply);
        _applyPreset = apply;
    }
    {
        KnobParametricPtr knob = AppManager::createKnob<KnobParametric>(this, tr("Curves").toStdString(), 3);
        knob->setName(kColorEqualizerParamCurves);
        knob->setHintToolTip(tr("The hue, saturation and brightness curves the node values interpolate to, against the hue in degrees. "
                                "Saturation and brightness are gains; the hue curve shows 1 + shift / 180°. Read-only: "
                                "the tabs below set the node values.")
                                 .toStdString());
        knob->setPeriodic(true);
        knob->setParametricRange(0., kCurveHueMax);
        knob->setMinimumsAndMaximums(std::vector<double>(3, 0.), std::vector<double>(3, kCurveYMax));
        knob->setDisplayMinimumsAndMaximums(std::vector<double>(3, 0.), std::vector<double>(3, kCurveYMax));
        const char* names[3] = { "hue", "saturation", "brightness" };
        const double colours[3][3] = { { 0.9, 0.4, 0.9 }, { 0.4, 0.8, 0.9 }, { 0.9, 0.9, 0.9 } };
        for (int c = 0; c < 3; ++c) {
            knob->setDimensionName(c, names[c]);
            knob->setCurveColor(c, colours[c][0], colours[c][1], colours[c][2]);
        }
        knob->setDefaultCurvesFromCurves();
        knob->setIsPersistent(false);
        knob->setEvaluateOnChange(false);
        knob->setAnimationEnabled(false);
        knob->setAllDimensionsEnabled(false);
        page->addKnob(knob);
        _curves = knob;
    }

    const struct {
        const char* tabName;
        const char* tabLabel;
        const char* prefix;
        const char* labelPrefix;
        KnobDoubleWPtr* knobs;
        const float* defaultsOf;
        double minimum;
        double maximum;
        const char* hint;
    } groups[3] = {
        { kColorEqualizerTabHue, "Hue", kColorEqualizerParamHuePrefix, "Hue", _hue, defaults.hue, -180., 180.,
          "The hue shift, in degrees, of the colours of this node's hue." },
        { kColorEqualizerTabSaturation, "Saturation", kColorEqualizerParamSaturationPrefix, "Saturation", _saturation, defaults.saturation, 0., 2.,
          "The saturation gain of the colours of this node's hue: 1 leaves them, 0 removes it; darktable's 0 % is 1." },
        { kColorEqualizerTabBrightness, "Brightness", kColorEqualizerParamBrightnessPrefix, "Brightness", _brightness, defaults.brightness, 0., 2.,
          "The brightness gain of the colours of this node's hue: 1 leaves them; darktable's 0 % is 1." },
    };
    for (const auto& g : groups) {
        const KnobGroupPtr tab = addTab(this, page, g.tabName, g.tabLabel);
        for (int i = 0; i < kNodes; ++i) {
            const std::string name = std::string(g.prefix) + kNodeNames[i];
            const std::string label = tr(g.labelPrefix).toStdString() + " " + tr(kNodeNames[i]).toStdString();
            g.knobs[i] = addDouble(this, tab, name, label, tr(g.hint).toStdString(), g.defaultsOf[i], g.minimum, g.maximum);
        }
    }

    const KnobGroupPtr options = addTab(this, page, kColorEqualizerTabOptions, "Options");
    _nodePlacement = addDouble(this, options, kColorEqualizerParamNodePlacement, tr("Node Placement").toStdString(),
                               tr("Rotates the eight nodes around the hue circle, in degrees.").toStdString(), defaults.hueShift, -23., 23.);
    _hueCurveSmoothing = addDouble(this, options, kColorEqualizerParamHueCurveSmoothing, tr("Hue Curve Smoothing").toStdString(),
                                   tr("How smoothly the hue curve runs between the nodes: lower follows the node values more closely, higher is smoother.").toStdString(),
                                   defaults.smoothingHue, 0.05, 2.);
    _saturationThreshold = addDouble(this, options, kColorEqualizerParamSaturationThreshold, tr("Saturation Threshold").toStdString(),
                                     tr("The saturation below which colours are treated as neutral and left alone. Raise it to keep low-saturation colours out of the edits.").toStdString(),
                                     defaults.threshold, 0., 0.3);
    _contrast = addDouble(this, options, kColorEqualizerParamContrast, tr("Contrast").toStdString(),
                          tr("How sharply the edits fade in above Saturation Threshold: higher is a softer transition, lower a sharper one.").toStdString(),
                          defaults.contrast, -1., 1.);
    _whiteLevel = addDouble(this, options, kColorEqualizerParamWhiteLevel, tr("White Level").toStdString(),
                            tr("The scene's white, in EV, which the brightness is measured against. The default suits most images.").toStdString(),
                            defaults.whiteLevel, -2., 16.);
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(this, tr("Use Filter").toStdString());
        knob->setName(kColorEqualizerParamUseFilter);
        knob->setHintToolTip(tr("Smooths the selection with a guided filter on chromaticity, so that noisy chroma does not break the edits up, "
                                "and suppresses the halos of brightness edits at sharp saturation edges.")
                                 .toStdString());
        knob->setDefaultValue(defaults.useFilter);
        knob->setAnimationEnabled(false);
        options->addKnob(knob);
        _useFilter = knob;
    }
    _chromaSize = addDouble(this, options, kColorEqualizerParamChromaSize, tr("Chroma Size").toStdString(),
                            tr("The radius, in pixels at full scale, of the chromaticity smoothing before the hue is read.").toStdString(),
                            defaults.chromaSize, 1., 10.);
    _paramSize = addDouble(this, options, kColorEqualizerParamParamSize, tr("Param Size").toStdString(),
                           tr("The radius, in pixels at full scale, of the smoothing of the saturation and brightness edits.").toStdString(),
                           defaults.paramSize, 1., 128.);
    {
        std::vector<ChoiceOption> outputs = {
            ChoiceOption("result", "Result", ""),
            ChoiceOption("hue", "Hue", ""),
            ChoiceOption("saturation", "Saturation", ""),
            ChoiceOption("brightness", "Brightness", ""),
            ChoiceOption("saturationThreshold", "Saturation Threshold", ""),
            ChoiceOption("brightnessThreshold", "Brightness Threshold", ""),
        };
        KnobChoicePtr choice = AppManager::createKnob<KnobChoice>(this, tr("Output").toStdString());
        choice->setName(kColorEqualizerParamOutput);
        choice->setHintToolTip(tr("Result, or the strength of an edit over the grey image: Hue, Saturation and Brightness show the edit "
                                  "(magenta where it raises, green where it lowers; the Brightness view marks the suppressed edges green), "
                                  "Saturation Threshold and Brightness Threshold the weight of the edits, with their weighting curve in green.")
                                   .toStdString());
        choice->populateChoices(outputs);
        choice->setDefaultValue(defaults.output);
        choice->setAnimationEnabled(false);
        options->addKnob(choice);
        _output = choice;
    }

    addStageKnobs(page);
    installCurvePainter();
} // ColorEqualizer::initializeKnobs

ColorEqualizerParams
ColorEqualizer::getParams(double time,
                          ViewIdx view) const
{
    const KnobReader reader(time, view);
    ColorEqualizerParams p;

    for (int i = 0; i < kNodes; ++i) {
        p.hue[i] = (float)reader.value(_hue[i]);
        p.saturation[i] = (float)reader.value(_saturation[i]);
        p.brightness[i] = (float)reader.value(_brightness[i]);
    }
    p.hueShift = (float)reader.value(_nodePlacement);
    p.smoothingHue = std::max(0.05f, (float)reader.value(_hueCurveSmoothing));
    p.threshold = (float)reader.value(_saturationThreshold);
    p.contrast = (float)reader.value(_contrast);
    p.whiteLevel = (float)reader.value(_whiteLevel);
    p.useFilter = reader.value(_useFilter);
    p.chromaSize = std::max(1.f, (float)reader.value(_chromaSize));
    p.paramSize = std::max(1.f, (float)reader.value(_paramSize));
    p.output = std::min((int)ColorEqualizerParams::eOutputBrightnessThreshold, std::max(0, (int)reader.value(_output)));

    return p;
}

void
ColorEqualizer::installCurvePainter()
{
    KnobParametricPtr curves = _curves.lock();

    if (!curves) {
        return;
    }
    const ColorEqualizer* self = this;
    curves->setBackgroundPainter([self](const ParametricBackgroundContext& context) {
        ParametricBackground background;
        const double x0 = context.xMin;
        const double x1 = context.xMax;
        if (!(x1 > x0) || !(context.yMax > context.yMin)) {
            return background;
        }
        const ColorEqualizerParams p = self->getParams(self->getCurrentTime(), ViewIdx(0));
        const ColorEqualizerData d = commit(p);

        background.quads.resize(kRampSlices);
        for (int i = 0; i < kRampSlices; ++i) {
            const double a = x0 + (x1 - x0) * double(i) / kRampSlices;
            const double b = x0 + (x1 - x0) * double(i + 1) / kRampSlices;
            float ca[3];
            float cb[3];
            rampColour((float)a, ca);
            rampColour((float)b, cb);
            const float dim = 0.45f;
            ParametricBackgroundQuad& quad = background.quads[i];
            quad.v[0] = ParametricBackgroundVertex(a, context.yMin, dim * ca[0], dim * ca[1], dim * ca[2]);
            quad.v[1] = ParametricBackgroundVertex(b, context.yMin, dim * cb[0], dim * cb[1], dim * cb[2]);
            quad.v[2] = ParametricBackgroundVertex(b, context.yMax, dim * cb[0], dim * cb[1], dim * cb[2]);
            quad.v[3] = ParametricBackgroundVertex(a, context.yMax, dim * ca[0], dim * ca[1], dim * ca[2]);
        }

        const std::array<float, kGamutLutSize>* luts[3] = { &d.lutHue, &d.lutSaturation, &d.lutBrightness };
        const float lineColours[3][3] = { { 0.95f, 0.45f, 0.95f }, { 0.45f, 0.85f, 0.95f }, { 0.95f, 0.95f, 0.95f } };
        for (int c = 0; c < 3; ++c) {
            ParametricBackgroundPolyline line;
            line.lineWidth = 2.;
            for (int i = 0; i <= kCurveSamples; ++i) {
                const double x = x0 + (x1 - x0) * double(i) / kCurveSamples;
                float y = lutAtDegrees(*luts[c], (float)x);
                if (c == 0) {
                    y = 1.f + y / kPi;
                }
                line.points.push_back(ParametricBackgroundVertex(x, y, lineColours[c][0], lineColours[c][1], lineColours[c][2]));
            }
            background.polylines.push_back(line);
        }
        // The nodes: a short vertical tick at each node's hue.
        for (int k = 0; k < kNodes; ++k) {
            double x = (double)k * 360. / kNodes + p.hueShift;
            while (x < x0) {
                x += kCurveHueMax;
            }
            while (x > x1) {
                x -= kCurveHueMax;
            }
            float rgb[3];
            rampColour((float)x, rgb);
            ParametricBackgroundPolyline tick;
            tick.lineWidth = 3.;
            const double h = 0.06 * (context.yMax - context.yMin);
            tick.points.push_back(ParametricBackgroundVertex(x, context.yMin, rgb[0], rgb[1], rgb[2]));
            tick.points.push_back(ParametricBackgroundVertex(x, context.yMin + h, rgb[0], rgb[1], rgb[2]));
            background.polylines.push_back(tick);
        }

        return background;
    });
}

void
ColorEqualizer::applyPresetAt(int index)
{
    if ((index < 0) || (index >= presetCount())) {
        return;
    }
    const ColorEqualizerParams p = presets()[index].params;
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();

    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const auto set = [](const KnobDoubleWPtr& weak,
                        double v) {
        KnobDoublePtr knob = weak.lock();
        if (knob && (knob->getValue() != v)) {
            knob->setValue(v);
        }
    };
    for (int i = 0; i < kNodes; ++i) {
        set(_hue[i], p.hue[i]);
        set(_saturation[i], p.saturation[i]);
        set(_brightness[i], p.brightness[i]);
    }
    set(_nodePlacement, p.hueShift);
    set(_hueCurveSmoothing, p.smoothingHue);
    set(_saturationThreshold, p.threshold);
    set(_contrast, p.contrast);
    set(_whiteLevel, p.whiteLevel);
    set(_chromaSize, p.chromaSize);
    set(_paramSize, p.paramSize);
    if (KnobBoolPtr useFilter = _useFilter.lock()) {
        if (useFilter->getValue() != p.useFilter) {
            useFilter->setValue(p.useFilter);
        }
    }
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
}

bool
ColorEqualizer::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason,
                            ViewSpec view,
                            double time,
                            bool originatedFromMainThread)
{
    const KnobButtonPtr applyButton = _applyPreset.lock();

    if (applyButton && (k == applyButton.get())) {
        const KnobChoicePtr choice = _preset.lock();
        if (choice) {
            applyPresetAt(choice->getValue());
        }

        return true;
    }
    if (KnobParametricPtr curves = _curves.lock()) {
        if (k != curves.get()) {
            curves->notifyBackgroundChanged();
        }
    }

    return StagedImageEffect::knobChanged(k, reason, view, time, originatedFromMainThread);
}

bool
ColorEqualizer::isOpIdentity(double /*time*/,
                             const RenderScale& /*scale*/,
                             const RectI& /*roi*/,
                             ViewIdx /*view*/)
{
    // Even at its defaults the result is gamut-mapped into Rec.2020.
    return false;
}

PassList
ColorEqualizer::opPasses(double time,
                         ViewIdx view,
                         const RenderScale& scale) const
{
    const ColorEqualizerParams params = getParams(time, view);
    const float s = (float)scale.toOfxPointD().x;
    int h = halo(params, s);
    // The frame is the source's, which is not asked of this effect here: its own region action
    // builds these passes.
    RectI frame;
    const bool hasFrame = framePixels(time, view, scale, &frame);
    if (hasFrame) {
        h = std::max(h, std::max(frame.width(), frame.height()));
    }
    PassList list;
    Pass carrier = Pass::gather(h, h, true, std::function<PassRowFn(const PassScratch&)>());

    carrier.growX = 0;
    carrier.growY = 0;
    list.boundary = Boundary::Zero;
    list.passes.push_back(carrier);

#ifdef NATRON_HAVE_GPU
    if (resolveWorkingSpace(this).matrix) {
        const ColorEqualizer* self = this;
        list.gpu = [self, params, s, frame, hasFrame](const PassGpuIO& io) -> StatusEnum {
            return self->recordGpu(io, params, s, hasFrame ? frame : RectI());
        };
        // The largest buffer the recorder allocates holds four floats a frame pixel: the source
        // and the guided filter's coefficients.
        list.gpuAccepts = [frame, hasFrame](const GpuQuery& query) {
            return !hasFrame || wholeFrameFitsGpu(frame, std::max(4, query.nComps), query.maxScratchBytes);
        };
    }
#endif

    return list;
}

void
ColorEqualizer::getRegionsOfInterest(double time,
                                     const RenderScale& scale,
                                     const RectD& outputRoD,
                                     const RectD& renderWindow,
                                     ViewIdx view,
                                     RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    const int sourceInput = getSourceInput();
    EffectInstancePtr input = (sourceInput >= 0) ? getInput(sourceInput) : EffectInstancePtr();
    RectI frame;
    if (!input || (ret->find(input) == ret->end()) || !framePixels(time, view, scale, &frame)) {
        return;
    }
    const RectD frameCanonical = toCanonical(frame, scale, getRenderMetadata(time, view)->getPixelAspectRatio(sourceInput));
    RectD need = renderWindow;
    if (need.intersects(frameCanonical)) {
        need.merge(frameCanonical);
    }
    (*ret)[input] = need;
}

bool
ColorEqualizer::framePixels(double time,
                            ViewIdx view,
                            const RenderScale& scale,
                            RectI* frame) const
{
    return wholeFramePixels(*this, getSourceInput(), time, view, scale, frame);
}

StatusEnum
ColorEqualizer::renderOp(const RenderActionArgs& args,
                         const StagePlan& plan)
{
    const PassList list = opPasses(args.time, args.view, args.mappedScale);
    const ColorEqualizerParams params = getParams(args.time, args.view);
    const ColorEqualizerData data = commit(params);
    const WorkingSpace ws = resolveWorkingSpace(this);
    const int sourceInput = plan.sourceInput;

    ImagePtr mask;
    if (plan.applyMask && (plan.maskChannel >= 0)) {
        RectI maskRoI;
        mask = getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        if (mask && (plan.maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }

    std::vector<PlaneJob> jobs;
    std::vector<std::string> layerIds;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            continue;
        }
        job.nComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, job.nComps, args.processChannels);
        ImageLayerDesc sourceLayer;
        if ((sourceInput >= 0) && getInput(sourceInput) && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            job.src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (plan.divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &plan.divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
            if (job.divisor && (plan.divisorChannel < (int)job.divisor->getComponentsCount())) {
                job.divisorChannel = plan.divisorChannel;
                job.skipChannel = Node::getUnPremultSkipChannel(it->first.isColorLayer() ? job.dst->getComponents() : it->first, plan.divisorLayer, plan.divisorChannel);
            } else {
                job.divisor.reset();
            }
        }
        if (!isFloatImage(job.dst) || !isFloatImage(job.src) || !isFloatImage(job.divisor)) {
            return eStatusFailed;
        }
        jobs.push_back(job);
        layerIds.push_back(it->first.getLayerID());
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }

    const RectI& roi = args.roi;
    if ((roi.width() <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }
    RectI rod;
    if (!framePixels(args.time, args.view, args.mappedScale, &rod)) {
        return eStatusFailed;
    }

    MaskInput maskSource;
    maskSource.applied = plan.applyMask;
    maskSource.invert = plan.maskInvert;
    maskSource.image = mask;
    maskSource.channel = plan.maskChannel;
    maskSource.lock();
    for (PlaneJob& job : jobs) {
        job.lock();
    }

    // Outside the frame the source passes through.
    RectI processWindow(0, 0, 0, 0);
    if ((plan.mix == 0.) || !intersectRects(roi, rod, &processWindow)) {
        processWindow = RectI(0, 0, 0, 0);
    }
    const bool processing = !isEmptyRect(processWindow);
    // The passes' halo reaches the whole frame from any window.
    const RectI bufferRect = processing ? rod : RectI(0, 0, 0, 0);
    const int width = std::max(0, bufferRect.width());
    const int height = std::max(0, bufferRect.height());
    const std::size_t planeSize = (std::size_t)width * height;

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    const EdgeAware::ParallelFor parallel = [&cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, cancel, body);
    };

    ColorEqualizerFrame frame;
    frame.width = rod.width();
    frame.height = rod.height();

    // What the whole-frame result depends on besides the layer and channels: the node's controls and
    // its source through the render hash, the working space, and where and how the frame is rendered.
    const EffectInstancePtr input = (sourceInput >= 0) ? getInput(sourceInput) : EffectInstancePtr();
    const ProjectPtr project = getApp() ? getApp()->getProject() : ProjectPtr();
    const std::string frameKey = std::to_string(getRenderHash()) + '|' + std::to_string(input ? input->getRenderHash() : 0) + '|' + std::to_string(args.time) + '|'
        + std::to_string((int)args.view) + '|' + std::to_string(args.mappedScale.toOfxPointD().x) + '|' + std::to_string(args.mappedScale.toOfxPointD().y) + '|'
        + std::to_string(rod.x1) + ',' + std::to_string(rod.y1) + ',' + std::to_string(rod.x2) + ',' + std::to_string(rod.y2) + '|'
        + (project ? project->getWorkingColorSpace() : std::string()) + '|' + plan.divisorLayer.getLayerID() + '|' + std::to_string(plan.divisorChannel);

    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const PlaneJob& job = jobs[j];
        const ProcessedPlanes planes(job);
        std::shared_ptr<const std::vector<float>> values;
        std::unique_lock<std::mutex> frameLock(_frameMutex, std::defer_lock);
        const std::string key = frameKey + '|' + layerIds[j] + '|' + std::to_string(job.nComps) + '|' + std::to_string(job.channels.to_ulong());
        if (processing && planeSize && (planes.count() > 0)) {
            // Concurrent windows wait for the one computing the frame, then share it.
            frameLock.lock();
            if (_frame && (_frameKey == key)) {
                values = _frame;
            }
        }
        if (frameLock.owns_lock() && !values) {
            std::shared_ptr<std::vector<float>> computed = std::make_shared<std::vector<float>>();
            std::vector<float>& result = *computed;
            const int nComps = job.nComps;
            PlaneJob full = job;
            full.channels.reset();
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                full.channels.set(pixelKernelChannelBit(nComps, c));
            }
            const ProcessedPlanes fullPlanes(full);
            std::vector<float> source(planeSize * fullPlanes.count());
            fillBuffer(full, fullPlanes, bufferRect, Boundary::Zero, nThreads, cancel, source.data());
            if (cancel.check()) {
                return eStatusOK;
            }
            result.resize(planeSize * planes.count());
            if (nComps < 3) {
                for (int p = 0; p < planes.count(); ++p) {
                    const float* from = source.data() + planeSize * fullPlanes.planeOfChannel[planes.channel[p]];
                    std::copy(from, from + planeSize, result.data() + planeSize * p);
                }
            } else {
                std::vector<float> rgba(planeSize * 4);
                std::vector<float> out(planeSize * 4);
                const auto plane = [&](int c) {
                    return source.data() + planeSize * fullPlanes.planeOfChannel[c];
                };
                parallel(height, [&](int y) {
                    float row[kChunk * 3];
                    for (int x0 = 0; x0 < width; x0 += kChunk) {
                        const int n = std::min(kChunk, width - x0);
                        for (int i = 0; i < n; ++i) {
                            const std::size_t k = (std::size_t)y * width + x0 + i;
                            for (int c = 0; c < 3; ++c) {
                                row[3 * i + c] = plane(c)[k];
                            }
                        }
                        if (!ws.matrix) {
                            std::string error;
                            IoColorSpace::convertRow(*ws.toXyz, row, n, 3, &error);
                        }
                        for (int i = 0; i < n; ++i) {
                            const std::size_t k = (std::size_t)y * width + x0 + i;
                            apply(ws.toPipe, &row[3 * i], &rgba[4 * k]);
                            rgba[4 * k + 3] = (nComps > 3) ? plane(3)[k] : 1.f;
                        }
                    }
                });
                if (cancel.check()) {
                    return eStatusOK;
                }
                process(data, (float)args.mappedScale.toOfxPointD().x, rgba.data(), out.data(), width, height, frame, parallel);
                if (cancel.check()) {
                    return eStatusOK;
                }
                const bool convertBack = (params.output == ColorEqualizerParams::eOutputResult);
                parallel(height, [&](int y) {
                    float row[kChunk * 3];
                    for (int x0 = 0; x0 < width; x0 += kChunk) {
                        const int n = std::min(kChunk, width - x0);
                        for (int i = 0; i < n; ++i) {
                            const std::size_t k = (std::size_t)y * width + x0 + i;
                            if (convertBack) {
                                apply(ws.fromPipe, &out[4 * k], &row[3 * i]);
                            } else {
                                std::copy(&out[4 * k], &out[4 * k] + 3, &row[3 * i]);
                            }
                        }
                        if (convertBack && !ws.matrix) {
                            std::string error;
                            IoColorSpace::convertRow(*ws.fromXyz, row, n, 3, &error);
                        }
                        for (int i = 0; i < n; ++i) {
                            const std::size_t k = (std::size_t)y * width + x0 + i;
                            for (int p = 0; p < planes.count(); ++p) {
                                const int c = planes.channel[p];
                                result[planeSize * p + k] = (c < 3) ? row[3 * i + c] : out[4 * k + 3];
                            }
                        }
                    }
                });
                if (cancel.check()) {
                    return eStatusOK;
                }
            }
            _frame = computed;
            _frameKey = key;
            values = computed;
        }
        if (frameLock.owns_lock()) {
            frameLock.unlock();
        }

        WindowLayout layout;
        layout.roi = roi;
        layout.processWindow = processWindow;
        layout.processing = processing;
        layout.bufferRect = bufferRect;
        layout.buffer = (values && !values->empty()) ? values->data() : NULL;
        layout.mix = (float)plan.mix;
        writeWindow(job, planes, maskSource, layout, nThreads, cancel);
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // ColorEqualizer::renderOp

#ifdef NATRON_HAVE_GPU
namespace {

// Word offsets of the tables in the parameter buffer colorequal.slang reads.
constexpr uint32_t kGpuToPipe = 0;
constexpr uint32_t kGpuFromPipe = 9;
constexpr uint32_t kGpuInputMatrix = 18;
constexpr uint32_t kGpuOutputMatrix = 27;
constexpr uint32_t kGpuLutHue = 36;
constexpr uint32_t kGpuLutSaturation = kGpuLutHue + kGamutLutSize;
constexpr uint32_t kGpuLutBrightness = kGpuLutSaturation + kGamutLutSize;
constexpr uint32_t kGpuGamut = kGpuLutBrightness + kGamutLutSize;
constexpr uint32_t kGpuSatWeights = kGpuGamut + kGamutLutSize;
constexpr uint32_t kGpuWords = kGpuSatWeights + ColorEqualizerData::kSatWeights;

// colorequal.slang's flags.
constexpr uint32_t kGpuUseFilter = 1;
constexpr uint32_t kGpuGuided = 2;
constexpr uint32_t kGpuPassThrough = 4;

constexpr int kGpuRoles = 10;

// Mirrors colorequal.slang's CEParams.
struct ColorEqualizerGpuParams {
    uint32_t width;
    uint32_t height;
    uint32_t dsWidth;
    uint32_t dsHeight;
    uint32_t nComps;
    uint32_t flags;
    uint32_t output;
    uint32_t outOffset;
    uint32_t outStride;
    int32_t outX0;
    int32_t outY0;
    uint32_t outWidth;
    uint32_t outHeight;
    int32_t frameX;
    int32_t frameY;
    int32_t frameWidth;
    int32_t frameHeight;
    float white;
    float satShift;
    float brightShift;
    float gradientAmp;
    float eps;
};
static_assert(sizeof(ColorEqualizerGpuParams) <= 128, "push constants");

struct GpuCtx {
    gpu::GpuRecordContext& ctx;
    const gpu::GpuBuffer* prm;
    const gpu::GpuBuffer* src;
    const gpu::GpuBuffer* dst;
    ColorEqualizerGpuParams p;
};

bool
gpuBuffer(gpu::GpuRecordContext& ctx,
          std::size_t floats,
          gpu::GpuBuffer** out)
{
    return (bool)ctx.scratch((VkDeviceSize)std::max<std::size_t>(1, floats) * sizeof(float), *out);
}

// Dispatches kernel over threadsX x threadsY with roles in b0.., the rest bound to the parameters.
bool
dispatchCe(GpuCtx& g,
           gpu::GpuKernelId kernel,
           std::initializer_list<const gpu::GpuBuffer*> roles,
           uint32_t threadsX,
           uint32_t threadsY)
{
    if (!threadsX || !threadsY) {
        return true;
    }
    std::array<const gpu::GpuBuffer*, 3 + kGpuRoles> buffers;
    buffers.fill(g.prm);
    buffers[0] = g.prm;
    buffers[1] = g.src;
    int i = 2;
    for (const gpu::GpuBuffer* b : roles) {
        buffers[i++] = b ? b : g.prm;
    }
    buffers[2 + kGpuRoles] = g.dst;
    if (!g.ctx.dispatch(kernel, std::span<const gpu::GpuBuffer* const>(buffers.data(), buffers.size()), std::as_bytes(std::span(&g.p, 1)), { threadsX, threadsY, 1 })) {
        return false;
    }
    g.ctx.computeBarrier();

    return true;
}

bool
gpuBlur(gpu::GpuRecordContext& ctx,
        const gpu::GpuBuffer* in,
        int width,
        int height,
        int channels,
        float sigma,
        gpu::GpuBuffer** out)
{
    const float mins[4] = { -kBlurRange, -kBlurRange, -kBlurRange, -kBlurRange };
    const float maxs[4] = { kBlurRange, kBlurRange, kBlurRange, kBlurRange };

    if (!gpuBuffer(ctx, (std::size_t)width * height * channels, out)) {
        return false;
    }

    return (bool)EdgeAware::recordRecursiveGaussianBlur(ctx, *in, **out, width, height, channels, sigma, mins, maxs);
}

bool
gpuResample(gpu::GpuRecordContext& ctx,
            const gpu::GpuBuffer* in,
            int wIn,
            int hIn,
            int wOut,
            int hOut,
            int channels,
            gpu::GpuBuffer** out)
{
    if (!gpuBuffer(ctx, (std::size_t)wOut * hOut * channels, out)) {
        return false;
    }

    return (bool)EdgeAware::recordInterpolateBilinear(ctx, *in, wIn, hIn, **out, wOut, hOut, channels);
}

} // anonymous namespace

StatusEnum
ColorEqualizer::recordGpu(const PassGpuIO& io,
                          const ColorEqualizerParams& params,
                          float scale,
                          const RectI& frame) const
{
    // Over a finite source the region of definition is the frame; over an infinite one the
    // domain is clamped to it and the output outside it is the source.
    if (frame.isNull() || (io.rod == frame)) {
        return recordGpuFrame(io, params, scale, false);
    }
    RectI inside(0, 0, 0, 0);
    const bool overlaps = PassExec::intersectRects(io.output, frame, &inside) && !PassExec::isEmptyRect(inside);
    if (!overlaps || (inside != io.output)) {
        if (recordGpuFrame(io, params, scale, true) != eStatusOK) {
            return eStatusFailed;
        }
        if (!overlaps) {
            return eStatusOK;
        }
        io.context.computeBarrier();
    }
    PassGpuIO clamped(io);
    clamped.rod = frame;
    clamped.output = inside;
    if (!PassExec::intersectRects(io.domain, frame, &clamped.domain)) {
        return eStatusFailed;
    }

    return recordGpuFrame(clamped, params, scale, false);
}

StatusEnum
ColorEqualizer::recordGpuFrame(const PassGpuIO& io,
                               const ColorEqualizerParams& params,
                               float scale,
                               bool passThrough) const
{
    gpu::GpuRecordContext& ctx = io.context;
    const int nComps = io.nComps;
    const RectI& domain = io.domain;
    const RectI& output = io.output;

    if ((nComps < 1) || (nComps > 4)) {
        return eStatusFailed;
    }
    const WorkingSpace ws = resolveWorkingSpace(this);
    if (!ws.matrix) {
        return eStatusFailed;
    }
    const ColorEqualizerData data = commit(params);
    const ColorEqualizerScaled s = scaled(data, scale);
    const PipelineMatrices& pm = pipelineMatrices();

    gpu::GpuBuffer* fill = nullptr;
    if (BloomKernels::recordFill(io, false, &fill) != eStatusOK) {
        return eStatusFailed;
    }
    if (PassExec::isEmptyRect(output) || PassExec::isEmptyRect(domain)) {
        return eStatusOK;
    }

    std::vector<uint32_t> words(kGpuWords, 0u);
    const auto put = [&words](uint32_t at, float v) {
        std::memcpy(&words[at], &v, sizeof(float));
    };
    const auto putMat = [&put](uint32_t at, const Mat3& m) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                put(at + 3 * r + c, m.m[r][c]);
            }
        }
    };
    putMat(kGpuToPipe, ws.toPipe);
    putMat(kGpuFromPipe, ws.fromPipe);
    putMat(kGpuInputMatrix, pm.input);
    putMat(kGpuOutputMatrix, pm.output);
    for (int i = 0; i < kGamutLutSize; ++i) {
        put(kGpuLutHue + i, data.lutHue[i]);
        put(kGpuLutSaturation + i, data.lutSaturation[i]);
        put(kGpuLutBrightness + i, data.lutBrightness[i]);
        put(kGpuGamut + i, pm.gamut[i]);
    }
    for (int i = 0; i < ColorEqualizerData::kSatWeights; ++i) {
        put(kGpuSatWeights + i, data.satWeights[i]);
    }
    gpu::GpuBuffer* prm = nullptr;
    if (!gpuBuffer(ctx, kGpuWords, &prm)) {
        return eStatusFailed;
    }
    if (!ctx.updateBuffer(*prm, 0, std::as_bytes(std::span<const uint32_t>(words.data(), words.size())))) {
        return eStatusFailed;
    }
    ctx.computeBarrier();

    const gpu::GpuBlurImage dst = PassExec::toGpuBlurImage(io.filtered);
    const int width = domain.width();
    const int height = domain.height();
    const std::size_t pixels = (std::size_t)width * height;

    GpuCtx g { ctx, prm, fill, dst.buffer, {} };
    ColorEqualizerGpuParams& p = g.p;
    p.width = (uint32_t)width;
    p.height = (uint32_t)height;
    p.nComps = (uint32_t)nComps;
    p.output = (uint32_t)params.output;
    p.outStride = dst.strideFloats;
    p.outX0 = output.x1 - domain.x1;
    p.outY0 = output.y1 - domain.y1;
    p.outWidth = (uint32_t)output.width();
    p.outHeight = (uint32_t)output.height();
    p.frameX = domain.x1 - io.rod.x1;
    p.frameY = domain.y1 - io.rod.y1;
    p.frameWidth = io.rod.width();
    p.frameHeight = io.rod.height();
    p.white = s.white;
    p.satShift = s.satShift;
    p.brightShift = s.brightShift;
    p.gradientAmp = s.gradientAmp;
    if (!BloomKernels::gpuFloatIndex(dst, output.x1, output.y1, nComps, &p.outOffset)) {
        return eStatusFailed;
    }

    if ((nComps < 3) || passThrough) {
        p.flags = kGpuPassThrough;

        return dispatchCe(g, gpu::GpuKernelId::ce_final, {}, p.outWidth, p.outHeight) ? eStatusOK : eStatusFailed;
    }

    gpu::GpuBuffer* uv = nullptr;
    gpu::GpuBuffer* satRaw = nullptr;
    gpu::GpuBuffer* lStar = nullptr;
    if (!gpuBuffer(ctx, pixels * 2, &uv) || !gpuBuffer(ctx, pixels, &satRaw) || !gpuBuffer(ctx, pixels, &lStar)) {
        return eStatusFailed;
    }
    if (!dispatchCe(g, gpu::GpuKernelId::ce_prepare, { uv, satRaw, lStar }, p.width, p.height)) {
        return eStatusFailed;
    }
    gpu::GpuBuffer* sat = nullptr;
    if (!gpuBlur(ctx, satRaw, width, height, 1, s.satSigma, &sat)) {
        return eStatusFailed;
    }

    // The guided filters' downsampling, as darktable's.
    const auto grid = [width, height](float sigma, int* dsW, int* dsH, float* gsigma) {
        const float sc = scaling(sigma);
        *gsigma = std::max(0.2f, sigma / sc);
        *dsW = (int)(width / sc);
        *dsH = (int)(height / sc);
    };

    if (data.useFilter) {
        int dsW, dsH;
        float gsigma;
        grid(s.hueSigma, &dsW, &dsH, &gsigma);
        if ((dsW >= 1) && (dsH >= 1)) {
            const bool resized = (dsW != width) || (dsH != height);
            const std::size_t dsPixels = (std::size_t)dsW * dsH;
            gpu::GpuBuffer* dsUV = uv;
            if (resized && !gpuResample(ctx, uv, width, height, dsW, dsH, 2, &dsUV)) {
                return eStatusFailed;
            }
            p.dsWidth = (uint32_t)dsW;
            p.dsHeight = (uint32_t)dsH;
            p.eps = data.chromaFeathering;
            gpu::GpuBuffer* cov = nullptr;
            gpu::GpuBuffer* a = nullptr;
            gpu::GpuBuffer* b = nullptr;
            if (!gpuBuffer(ctx, dsPixels * 4, &cov) || !gpuBuffer(ctx, dsPixels * 4, &a) || !gpuBuffer(ctx, dsPixels * 2, &b)) {
                return eStatusFailed;
            }
            if (!dispatchCe(g, gpu::GpuKernelId::ce_covariance, { dsUV, cov }, p.dsWidth, p.dsHeight)) {
                return eStatusFailed;
            }
            gpu::GpuBuffer* uvMean = nullptr;
            gpu::GpuBuffer* covMean = nullptr;
            if (!gpuBlur(ctx, dsUV, dsW, dsH, 2, gsigma, &uvMean) || !gpuBlur(ctx, cov, dsW, dsH, 4, gsigma, &covMean)) {
                return eStatusFailed;
            }
            if (!dispatchCe(g, gpu::GpuKernelId::ce_prefilter_ab, { uvMean, covMean, a, b }, p.dsWidth, p.dsHeight)) {
                return eStatusFailed;
            }
            gpu::GpuBuffer* aMean = nullptr;
            gpu::GpuBuffer* bMean = nullptr;
            if (!gpuBlur(ctx, a, dsW, dsH, 4, gsigma, &aMean) || !gpuBlur(ctx, b, dsW, dsH, 2, gsigma, &bMean)) {
                return eStatusFailed;
            }
            if (resized && (!gpuResample(ctx, aMean, dsW, dsH, width, height, 4, &aMean) || !gpuResample(ctx, bMean, dsW, dsH, width, height, 2, &bMean))) {
                return eStatusFailed;
            }
            // Unresized, darktable blurs UV in place, and the filter reads the blurred UV.
            gpu::GpuBuffer* uvIn = resized ? uv : uvMean;
            if (!dispatchCe(g, gpu::GpuKernelId::ce_prefilter_apply, { uv, sat, aMean, bMean, uvIn }, p.width, p.height)) {
                return eStatusFailed;
            }
        }
    }

    gpu::GpuBuffer* hsb = nullptr;
    gpu::GpuBuffer* corr = nullptr;
    gpu::GpuBuffer* bCorr = nullptr;
    gpu::GpuBuffer* grad = nullptr;
    if (!gpuBuffer(ctx, pixels * 4, &hsb) || !gpuBuffer(ctx, pixels * 2, &corr) || !gpuBuffer(ctx, pixels, &bCorr) || !gpuBuffer(ctx, pixels, &grad)) {
        return eStatusFailed;
    }
    p.flags = data.useFilter ? kGpuUseFilter : 0u;
    if (!dispatchCe(g, gpu::GpuKernelId::ce_corrections, { uv, sat, lStar, hsb, corr, bCorr, grad }, p.width, p.height)) {
        return eStatusFailed;
    }

    // Final roles: hsb, uv, the hue corrections, corrections, b corrections, a, b, saturation, gradient.
    const gpu::GpuBuffer* finalUV = uv;
    const gpu::GpuBuffer* hueCorr = corr;
    const gpu::GpuBuffer* finalA = nullptr;
    const gpu::GpuBuffer* finalB = nullptr;
    const gpu::GpuBuffer* gradMean = grad;
    if (data.useFilter) {
        gpu::GpuBuffer* gradBlurred = nullptr;
        if (!gpuBlur(ctx, grad, width, height, 1, s.scharrSigma, &gradBlurred)) {
            return eStatusFailed;
        }
        gradMean = gradBlurred;
        int dsW, dsH;
        float gsigma;
        grid(s.paramSigma, &dsW, &dsH, &gsigma);
        if ((dsW >= 1) && (dsH >= 1)) {
            const bool resized = (dsW != width) || (dsH != height);
            const std::size_t dsPixels = (std::size_t)dsW * dsH;
            gpu::GpuBuffer* dsUV = uv;
            gpu::GpuBuffer* dsCorr = corr;
            gpu::GpuBuffer* dsB = bCorr;
            if (resized && (!gpuResample(ctx, uv, width, height, dsW, dsH, 2, &dsUV) || !gpuResample(ctx, corr, width, height, dsW, dsH, 2, &dsCorr) || !gpuResample(ctx, bCorr, width, height, dsW, dsH, 1, &dsB))) {
                return eStatusFailed;
            }
            p.dsWidth = (uint32_t)dsW;
            p.dsHeight = (uint32_t)dsH;
            p.eps = data.paramFeathering;
            gpu::GpuBuffer* cov = nullptr;
            gpu::GpuBuffer* correl = nullptr;
            gpu::GpuBuffer* a = nullptr;
            gpu::GpuBuffer* b = nullptr;
            if (!gpuBuffer(ctx, dsPixels * 4, &cov) || !gpuBuffer(ctx, dsPixels * 4, &correl) || !gpuBuffer(ctx, dsPixels * 4, &a) || !gpuBuffer(ctx, dsPixels * 2, &b)) {
                return eStatusFailed;
            }
            if (!dispatchCe(g, gpu::GpuKernelId::ce_guide_init, { dsUV, dsCorr, dsB, cov, correl }, p.dsWidth, p.dsHeight)) {
                return eStatusFailed;
            }
            gpu::GpuBuffer* uvMean = nullptr;
            gpu::GpuBuffer* covMean = nullptr;
            gpu::GpuBuffer* corrMean = nullptr;
            gpu::GpuBuffer* bMeanIn = nullptr;
            gpu::GpuBuffer* correlMean = nullptr;
            if (!gpuBlur(ctx, dsUV, dsW, dsH, 2, gsigma, &uvMean) || !gpuBlur(ctx, cov, dsW, dsH, 4, gsigma, &covMean)
                || !gpuBlur(ctx, dsCorr, dsW, dsH, 2, gsigma, &corrMean) || !gpuBlur(ctx, dsB, dsW, dsH, 1, 0.1f * gsigma, &bMeanIn)
                || !gpuBlur(ctx, correl, dsW, dsH, 4, gsigma, &correlMean)) {
                return eStatusFailed;
            }
            if (!dispatchCe(g, gpu::GpuKernelId::ce_guide_ab, { uvMean, covMean, corrMean, bMeanIn, correlMean, a, b }, p.dsWidth, p.dsHeight)) {
                return eStatusFailed;
            }
            gpu::GpuBuffer* aMean = nullptr;
            gpu::GpuBuffer* bMean = nullptr;
            if (!gpuBlur(ctx, a, dsW, dsH, 4, gsigma, &aMean) || !gpuBlur(ctx, b, dsW, dsH, 2, gsigma, &bMean)) {
                return eStatusFailed;
            }
            if (resized && (!gpuResample(ctx, aMean, dsW, dsH, width, height, 4, &aMean) || !gpuResample(ctx, bMean, dsW, dsH, width, height, 2, &bMean))) {
                return eStatusFailed;
            }
            // Unresized, darktable blurs UV and the corrections in place: the hue correction stays blurred.
            finalUV = resized ? uv : uvMean;
            hueCorr = resized ? corr : corrMean;
            finalA = aMean;
            finalB = bMean;
            p.flags |= kGpuGuided;
        }
    }

    return dispatchCe(g, gpu::GpuKernelId::ce_final, { hsb, finalUV, hueCorr, corr, bCorr, finalA, finalB, sat, gradMean }, p.outWidth, p.outHeight)
        ? eStatusOK
        : eStatusFailed;
} // ColorEqualizer::recordGpu
#else
StatusEnum
ColorEqualizer::recordGpu(const PassGpuIO& /*io*/,
                          const ColorEqualizerParams& /*params*/,
                          float /*scale*/,
                          const RectI& /*frame*/) const
{
    return eStatusFailed;
}

StatusEnum
ColorEqualizer::recordGpuFrame(const PassGpuIO& /*io*/,
                               const ColorEqualizerParams& /*params*/,
                               float /*scale*/,
                               bool /*passThrough*/) const
{
    return eStatusFailed;
}
#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT
