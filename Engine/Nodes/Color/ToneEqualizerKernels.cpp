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
 * src/iop/toneequal.c, src/iop/choleski.h and src/common/luminance_mask.h. GPL-3.0-or-later; see
 * ToneEqualizerKernels.h.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ToneEqualizerKernels.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>

#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/GuidedFilter.h"

NATRON_NAMESPACE_ENTER

namespace ToneEqualizerKernels {
namespace {
    const float kCentersOps[kCenters] = { -56.0f / 7.0f, -48.0f / 7.0f, -40.0f / 7.0f, -32.0f / 7.0f, -24.0f / 7.0f, -16.0f / 7.0f, -8.0f / 7.0f, 0.0f / 7.0f };
    const float kCentersParams[kBands] = { -8.0f, -7.0f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f };

    const int kHistogramBins = 512;

    inline float
    contrastFulcrum()
    {
        return std::exp2(-4.0f);
    }

    inline float
    minFloat()
    {
        return std::exp2(-16.0f);
    }

    inline float
    gaussianDenom(float sigma)
    {
        return 2.0f * sigma * sigma;
    }

    inline float
    gaussianFunc(float radius,
                 float denominator)
    {
        return std::exp(-radius * radius / denominator);
    }

    // darktable's fmaxf(fminf(value, top), bottom): NaN comes out as top.
    inline float
    fastClamp(float value,
              float bottom,
              float top)
    {
        return std::fmax(std::fmin(value, top), bottom);
    }

    // glib's MAX and MIN, which keep the second argument when a comparison fails.
    inline float
    glibMax(float a,
            float b)
    {
        return (a > b) ? a : b;
    }

    inline float
    glibMin(float a,
            float b)
    {
        return (a < b) ? a : b;
    }

    inline float
    linearContrast(float pixel,
                   float fulcrum,
                   float contrast)
    {
        return glibMax((pixel - fulcrum) * contrast + fulcrum, minFloat());
    }

    // A float to int conversion as x86's cvttss2si makes it, which darktable's histogram relies on
    // for non-finite exposures: out of range gives INT_MIN.
    inline int
    truncateToInt(float v)
    {
        if (!(v > (float)INT_MIN) || !(v < (float)INT_MAX)) {
            return INT_MIN;
        }

        return (int)v;
    }

    // choleski.h: _choleski_decompose, _triangular_descent, _triangular_ascent on an n x n lower
    // triangle.
    bool
    solveHermitian(const float* a,
                   float* y,
                   int n)
    {
        std::vector<float> l((std::size_t)n * n, 0.f);
        std::vector<float> x(n, 0.f);

        if (a[0] <= 0.0f) {
            return false;
        }
        bool valid = true;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < (i + 1); j++) {
                float sum = 0.0f;
                for (int k = 0; k < j; k++) {
                    sum += l[i * n + k] * l[j * n + k];
                }
                if (i == j) {
                    const float temp = a[i * n + i] - sum;
                    if (temp < 0.0f) {
                        valid = false;
                        l[i * n + j] = NAN;
                    } else {
                        l[i * n + j] = std::sqrt(a[i * n + i] - sum);
                    }
                } else {
                    const float temp = l[j * n + j];
                    if (temp == 0.0f) {
                        valid = false;
                        l[i * n + j] = NAN;
                    } else {
                        l[i * n + j] = (a[i * n + j] - sum) / temp;
                    }
                }
            }
        }
        if (!valid) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            float sum = y[i];
            for (int j = 0; j < i; ++j) {
                sum -= l[i * n + j] * x[j];
            }
            const float temp = l[i * n + i];
            if (temp != 0.0f) {
                x[i] = sum / temp;
            } else {
                x[i] = NAN;
                valid = false;
            }
        }
        if (!valid) {
            return false;
        }
        for (int i = n - 1; i > -1; --i) {
            float sum = x[i];
            for (int j = n - 1; j > i; --j) {
                sum -= l[j * n + i] * y[j];
            }
            const float temp = l[i * n + i];
            if (temp != 0.0f) {
                y[i] = sum / temp;
            } else {
                y[i] = NAN;
                valid = false;
            }
        }

        return valid;
    }

    // choleski.h's pseudo_solve: the least-squares solution of the m x n system A x = y, in y.
    bool
    pseudoSolve(const float* a,
                float* y,
                int m,
                int n)
    {
        std::vector<float> aSquare((std::size_t)n * n, 0.f);
        std::vector<float> ySquare(n, 0.f);

        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < (i + 1); ++j) {
                float sum = 0.0f;
                for (int k = 0; k < m; ++k) {
                    sum += a[k * n + i] * a[k * n + j];
                }
                aSquare[i * n + j] = sum;
            }
        }
        for (int i = 0; i < n; ++i) {
            float sum = 0.0f;
            for (int k = 0; k < m; ++k) {
                sum += a[k * n + i] * y[k];
            }
            ySquare[i] = sum;
        }
        if (!solveHermitian(aSquare.data(), ySquare.data(), n)) {
            return false;
        }
        std::copy(ySquare.begin(), ySquare.end(), y);

        return true;
    }

    Settings
    presetBase()
    {
        Settings s;

        s.gains.fill(0.f);
        s.smoothing = (float)M_SQRT2;

        return s;
    }

    void
    compressGains(Settings* p,
                  float step)
    {
        p->gains = { step, 5.f / 3.f * step, 5.f / 3.f * step, step, 0.0f, -step, -5.f / 3.f * step, -5.f / 3.f * step, -step };
    }

    void
    dilateGains(Settings* p,
                float step)
    {
        p->gains = { -15.f / 9.f * step, -14.f / 9.f * step, -12.f / 9.f * step, -8.f / 9.f * step, 0.f, 8.f / 9.f * step, 12.f / 9.f * step, 14.f / 9.f * step, 15.f / 9.f * step };
    }

    std::vector<Preset>
    makePresets()
    {
        std::vector<Preset> out;
        // darktable's init_presets() edits one params struct from preset to preset; this follows it.
        Settings p = presetBase();

        p.estimator = eEstimatorNormPower;
        p.contrastBoost = 0.0f;
        p.details = eDetailsNone;
        p.exposureBoost = -0.5f;
        p.feathering = 1.0f;
        p.iterations = 1;
        p.quantization = 0.0f;
        // darktable leaves it at 0 from its memset, below the slider's minimum; without a filter it is
        // unused, so the knob's minimum stands in.
        p.blending = 0.01f;
        out.push_back({ "simpleToneCurve", "Simple Tone Curve", p });

        p.details = eDetailsEigf;
        p.estimator = eEstimatorNorm2;
        p.blending = 5.0f;
        p.feathering = 1.0f;
        p.iterations = 1;
        p.quantization = 0.0f;
        p.exposureBoost = 0.0f;
        p.contrastBoost = 0.0f;
        out.push_back({ "maskBlendingAllPurposes", "Mask Blending | All Purposes", p });

        p.blending = 1.0f;
        p.feathering = 10.0f;
        p.iterations = 3;
        out.push_back({ "maskBlendingPeopleWithBacklight", "Mask Blending | People With Backlight", p });

        p.exposureBoost = -1.57f;
        p.contrastBoost = 0.0f;
        p.blending = 2.0f;
        p.iterations = 5;
        p.quantization = 0.0f;
        p.details = eDetailsEigf;
        p.feathering = 20.0f;
        compressGains(&p, 0.65f);
        out.push_back({ "compressShadowsHighlightsEigfStrong", "Compress Shadows/Highlights | EIGF | Strong", p });

        p.details = eDetailsGuided;
        p.feathering = 500.0f;
        out.push_back({ "compressShadowsHighlightsGfStrong", "Compress Shadows/Highlights | GF | Strong", p });

        p.details = eDetailsEigf;
        p.blending = 3.0f;
        p.feathering = 7.0f;
        p.iterations = 3;
        compressGains(&p, 0.45f);
        out.push_back({ "compressShadowsHighlightsEigfMedium", "Compress Shadows/Highlights | EIGF | Medium", p });

        p.details = eDetailsGuided;
        p.feathering = 500.0f;
        out.push_back({ "compressShadowsHighlightsGfMedium", "Compress Shadows/Highlights | GF | Medium", p });

        p.details = eDetailsEigf;
        p.blending = 5.0f;
        p.feathering = 1.0f;
        p.iterations = 1;
        compressGains(&p, 0.25f);
        out.push_back({ "compressShadowsHighlightsEigfSoft", "Compress Shadows/Highlights | EIGF | Soft", p });

        p.details = eDetailsGuided;
        p.feathering = 500.0f;
        out.push_back({ "compressShadowsHighlightsGfSoft", "Compress Shadows/Highlights | GF | Soft", p });

        p.details = eDetailsNone;
        dilateGains(&p, 0.25f);
        out.push_back({ "contrastToneCurveSoft", "Contrast Tone Curve | Soft", p });
        dilateGains(&p, 0.45f);
        out.push_back({ "contrastToneCurveMedium", "Contrast Tone Curve | Medium", p });
        dilateGains(&p, 0.65f);
        out.push_back({ "contrastToneCurveStrong", "Contrast Tone Curve | Strong", p });

        p.details = eDetailsEigf;
        p.blending = 5.0f;
        p.feathering = 1.0f;
        p.iterations = 1;
        p.quantization = 0.0f;
        p.exposureBoost = -0.5f;
        p.contrastBoost = 0.0f;
        p.gains = { 0.0f, 0.15f, 0.6f, 1.15f, 1.33f, 1.15f, 0.6f, 0.15f, 0.0f };
        out.push_back({ "relightFillIn", "Relight: Fill-In", p });

        return out;
    }
} // namespace

bool
sameMask(const Settings& a,
         const Settings& b)
{
    if ((a.estimator != b.estimator) || (a.details != b.details) || (a.exposureBoost != b.exposureBoost)) {
        return false;
    }
    if (a.details == eDetailsNone) {
        return true;
    }
    const bool contrast = (a.details == eDetailsGuided) || (a.details == eDetailsEigf);

    return (a.blending == b.blending) && (a.feathering == b.feathering) && (a.quantization == b.quantization) && (a.iterations == b.iterations)
        && (!contrast || (a.contrastBoost == b.contrastBoost));
}

float
smoothingFromSlider(double slider)
{
    return std::pow((float)M_SQRT2, 1.0f + (float)slider);
}

bool
solveFactors(const std::array<float, kBands>& gainsEv,
             float sigma,
             std::array<float, kCenters>* factors)
{
    float y[kBands];
    for (int c = 0; c < kBands; ++c) {
        y[c] = std::exp2(gainsEv[c]);
    }
    float a[kBands * kCenters];
    const float denom = gaussianDenom(sigma);
    for (int i = 0; i < kBands; ++i) {
        for (int j = 0; j < kCenters; ++j) {
            a[i * kCenters + j] = gaussianFunc(kCentersParams[i] - kCentersOps[j], denom);
        }
    }
    const bool valid = pseudoSolve(a, y, kBands, kCenters);
    for (int i = 0; i < kCenters; ++i) {
        (*factors)[i] = y[i];
    }

    return valid;
}

float
pixelCorrection(float exposure,
                const std::array<float, kCenters>& factors,
                float sigma)
{
    float result = 0.0f;
    const float denom = gaussianDenom(sigma);
    const float expo = fastClamp(exposure, kMinEv, kMaxEv);

    for (int i = 0; i < kCenters; ++i) {
        result += gaussianFunc(expo - kCentersOps[i], denom) * factors[i];
    }

    return fastClamp(result, 0.25f, 4.0f);
}

void
correctionLut(const std::array<float, kCenters>& factors,
              float sigma,
              float* lut)
{
    const float denom = gaussianDenom(sigma);

    for (int j = 0; j < kLutSize; j++) {
        const float exposure = (float)j / (float)kLutResolution + kMinEv;
        float result = 0.0f;
        for (int i = 0; i < kCenters; i++) {
            result += gaussianFunc(exposure - kCentersOps[i], denom) * factors[i];
        }
        lut[j] = fastClamp(result, 0.25f, 4.0f);
    }
}

float
lutCorrection(const float* lut,
              float luminance)
{
    const float exposure = fastClamp(std::log2(luminance), kMinEv, kMaxEv);

    return lut[(unsigned)std::round((exposure - kMinEv) * (float)kLutResolution)];
}

float
estimate(EstimatorEnum estimator,
         float r,
         float g,
         float b,
         float exposureBoost,
         float fulcrum,
         float contrastBoost)
{
    switch (estimator) {
    case eEstimatorMean: {
        float lum = 0.0f;
        lum += r;
        lum += g;
        lum += b;

        return linearContrast(exposureBoost * lum / 3.0f, fulcrum, contrastBoost);
    }
    case eEstimatorLightness: {
        const float maxRgb = glibMax(glibMax(r, g), b);
        const float minRgb = glibMin(glibMin(r, g), b);

        return linearContrast(exposureBoost * (maxRgb + minRgb) / 2.0f, fulcrum, contrastBoost);
    }
    case eEstimatorValue:

        return linearContrast(exposureBoost * glibMax(glibMax(r, g), b), fulcrum, contrastBoost);
    case eEstimatorNorm1: {
        float lum = 0.0f;
        lum += std::fabs(r);
        lum += std::fabs(g);
        lum += std::fabs(b);

        return linearContrast(exposureBoost * lum, fulcrum, contrastBoost);
    }
    case eEstimatorNorm2: {
        float result = 0.0f;
        result += r * r;
        result += g * g;
        result += b * b;

        return linearContrast(exposureBoost * std::sqrt(result), fulcrum, contrastBoost);
    }
    case eEstimatorNormPower: {
        float numerator = 0.0f;
        float denominator = 0.0f;
        for (const float v : { r, g, b }) {
            const float value = std::fabs(v);
            const float square = value * value;
            const float cubic = square * value;
            numerator += cubic;
            denominator += square;
        }

        return linearContrast(exposureBoost * numerator / denominator, fulcrum, contrastBoost);
    }
    case eEstimatorGeomean: {
        float lum = 1.0f;
        lum *= std::fabs(r);
        lum *= std::fabs(g);
        lum *= std::fabs(b);

        return linearContrast(exposureBoost * std::pow(lum, 1.0f / 3.0f), fulcrum, contrastBoost);
    }
    case eEstimatorCount:
        break;
    }

    return 0.f;
}

int
maskRadius(float blending,
           int fullWidth,
           int fullHeight,
           double scale)
{
    const int maxSize = (fullWidth > fullHeight) ? fullWidth : fullHeight;
    const float diameter = blending / 100.0f * (float)maxSize * (float)scale;

    return (int)((diameter - 1.0f) / (2.0f));
}

void
computeMask(const float* rgb,
            int width,
            int height,
            const Settings& s,
            int radius,
            float* mask,
            const EdgeAware::ParallelFor& parallel)
{
    const float exposureBoost = std::exp2(s.exposureBoost);
    const bool contrast = (s.details == eDetailsGuided) || (s.details == eDetailsEigf);
    const float fulcrum = contrast ? contrastFulcrum() : 0.0f;
    const float contrastBoost = contrast ? std::exp2(s.contrastBoost) : 1.0f;

    parallel(height, [&](int y) {
        const float* line = rgb + (std::size_t)y * width * 3;
        float* out = mask + (std::size_t)y * width;
        for (int x = 0; x < width; ++x) {
            out[x] = estimate(s.estimator, line[3 * x], line[3 * x + 1], line[3 * x + 2], exposureBoost, fulcrum, contrastBoost);
        }
    });

    const float feathering = 1.f / s.feathering;
    const float qMin = std::exp2(-14.0f);
    const float qMax = 4.0f;
    switch (s.details) {
    case eDetailsAveragedGuided:
        EdgeAware::fastSurfaceBlur(mask, width, height, radius, feathering, s.iterations, EdgeAware::eGuidedBlendingGeomean, s.quantization, qMin, qMax, parallel);
        break;
    case eDetailsGuided:
        EdgeAware::fastSurfaceBlur(mask, width, height, radius, feathering, s.iterations, EdgeAware::eGuidedBlendingLinear, s.quantization, qMin, qMax, parallel);
        break;
    case eDetailsAveragedEigf:
        EdgeAware::eigfSurfaceBlur(mask, width, height, (float)radius, feathering, s.iterations, EdgeAware::eGuidedBlendingGeomean, s.quantization, qMin, qMax, parallel);
        break;
    case eDetailsEigf:
        EdgeAware::eigfSurfaceBlur(mask, width, height, (float)radius, feathering, s.iterations, EdgeAware::eGuidedBlendingLinear, s.quantization, qMin, qMax, parallel);
        break;
    case eDetailsNone:
    case eDetailsCount:
        break;
    }
}

float
displayMask(float luminance)
{
    return std::sqrt(std::fmin(std::fmax(luminance - 0.00390625f, 0.f) / 0.99609375f, 1.f));
}

int
bandOf(float luminance)
{
    const float exposure = fastClamp(std::log2(luminance), kMinEv, kMaxEv);

    return (int)std::lround(exposure) - (int)kMinEv;
}

float
zoneDisplay(float luminance)
{
    return displayMask(std::exp2((float)(bandOf(luminance) + (int)kMinEv)));
}

float
sampleMask(const float* mask,
           int width,
           int height,
           int x,
           int y)
{
    static const float kKernel[3][3] = { { 0.076555024f, 0.124401914f, 0.076555024f },
                                         { 0.124401914f, 0.196172249f, 0.124401914f },
                                         { 0.076555024f, 0.124401914f, 0.076555024f } };

    if ((x < 0) || (y < 0) || (y >= height) || (x >= width)) {
        return NAN;
    }
    const int rows[3] = { std::max(y, 1) - 1, y, std::min(y + 1, height - 1) };
    const int cols[3] = { std::max(x, 1) - 1, x, std::min(x + 1, width - 1) };
    float luminance = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            luminance += mask[(std::size_t)width * rows[i] + cols[j]] * kKernel[i][j];
        }
    }

    return luminance;
}

Deciles
maskDeciles(const float* mask,
            std::size_t count)
{
    std::vector<int> hist(kHistogramBins, 0);

    for (std::size_t k = 0; k < count; k++) {
        const int index = std::clamp(truncateToInt(((std::log2(mask[k]) + 10.0f) / 16.0f) * (float)kHistogramBins), 0, kHistogramBins - 1);
        hist[index] += 1;
    }

    const int first = (int)((float)count * 0.05f);
    const int last = (int)((float)count * (1.0f - 0.95f));
    std::size_t population = 0;
    int firstPos = 0;
    int lastPos = 0;
    for (int k = 0; k < kHistogramBins; ++k) {
        const std::size_t prev = population;
        population += hist[k];
        if ((prev < (std::size_t)first) && ((std::size_t)first <= population)) {
            firstPos = k;
            break;
        }
    }
    population = 0;
    for (int k = kHistogramBins - 1; k >= 0; --k) {
        const std::size_t prev = population;
        population += hist[k];
        if ((prev < (std::size_t)last) && ((std::size_t)last <= population)) {
            lastPos = k;
            break;
        }
    }

    Deciles d;
    d.first = (float)(16.0 * (float)firstPos / (float)(kHistogramBins - 1) - 10.0);
    d.last = (float)(16.0 * (float)lastPos / (float)(kHistogramBins - 1) - 10.0);

    return d;
}

float
autoExposureBoost(const Deciles& deciles,
                  float exposureBoost,
                  float contrastBoost)
{
    const float fulcrum = contrastFulcrum();
    const float fdNew = std::exp2(deciles.first);
    const float ldNew = std::exp2(deciles.last);
    const float e = std::exp2(exposureBoost);
    const float c = std::exp2(contrastBoost);
    const float fdOld = ((fdNew - fulcrum) / c + fulcrum) / e;
    const float ldOld = ((ldNew - fulcrum) / c + fulcrum) / e;
    const float s1 = fulcrum - std::exp2(-7.0f);
    const float s2 = std::exp2(-1.0f) - fulcrum;
    const float mix = fdOld * s2 + ldOld * s1;

    return std::log2(fulcrum * (s1 + s2) / mix);
}

float
autoContrastBoost(const Deciles& deciles,
                  float exposureBoost,
                  float contrastBoost,
                  DetailsEnum details,
                  float feathering)
{
    const float fulcrum = contrastFulcrum();
    const float fdNew = std::exp2(deciles.first);
    const float ldNew = std::exp2(deciles.last);
    const float e = std::exp2(exposureBoost);
    float c = std::exp2(contrastBoost);
    const float fdOld = ((fdNew - fulcrum) / c + fulcrum) / e;
    const float ldOld = ((ldNew - fulcrum) / c + fulcrum) / e;
    const float s1 = fulcrum - std::exp2(-7.0f);
    const float s2 = std::exp2(-1.0f) - fulcrum;
    const float mix = fdOld * s2 + ldOld * s1;

    c = std::log2(mix / (fulcrum * (ldOld - fdOld)) / c);

    // darktable's regression of how the filters change the histogram when adding contrast.
    if ((details == eDetailsEigf) && (c > 0.0f)) {
        const float correction = (float)(-0.0276f + 0.01823 * feathering + (0.7566f - 1.0f) * c);
        if (feathering < 5.0f) {
            c += correction;
        } else if (feathering < 10.0f) {
            c += correction * (2.0f - feathering / 5.0f);
        }
    } else if ((details == eDetailsGuided) && (c > 0.0f)) {
        c = 0.0235f + 1.1225f * c;
    }

    return contrastBoost + c;
}

const std::vector<Preset>&
presets()
{
    static const std::vector<Preset> all = makePresets();

    return all;
}
} // namespace ToneEqualizerKernels

NATRON_NAMESPACE_EXIT
