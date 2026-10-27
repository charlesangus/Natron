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
 * Ported from darktable, DARKTABLE_MASTER_REF (commit fee19b421fdb), src/iop/contrastntexture.c
 * and the RGB euclidean norm of src/common/luminance_mask.h, C paths only.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2026 darktable developers.
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

#include "ContrastTextureKernels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"

NATRON_NAMESPACE_ENTER

namespace ContrastTextureKernels {
namespace {
    const float kPiF = 3.14159265358979323846f;
    const int kRowBlock = 64;

    // darktable's MAX(), which takes b when a is NaN.
    inline float
    maxOrSecond(float a,
                float b)
    {
        return (a > b) ? a : b;
    }

    void
    forRows(const EdgeAware::ParallelFor& parallel,
            int width,
            int height,
            const std::function<void(std::size_t)>& body)
    {
        const int blocks = (height + kRowBlock - 1) / kRowBlock;
        parallel(blocks, [&](int b) {
            const int y1 = b * kRowBlock;
            const int y2 = std::min(height, y1 + kRowBlock);
            for (std::size_t k = (std::size_t)y1 * width; k < (std::size_t)y2 * width; ++k) {
                body(k);
            }
        });
    }
} // namespace

Plan
makePlan(const Params& p,
         float fullSize,
         float renderScale)
{
    Plan d;

    d.slopeShadows = std::pow(2.0f, -p.gainShadows);
    d.slopeHighlights = std::pow(2.0f, p.gainHighlights);
    d.polynomial[0] = -d.slopeShadows * kMidtonesWidth;
    d.polynomial[1] = d.slopeShadows;
    d.polynomial[2] = (d.slopeHighlights - d.slopeShadows) / (4.0f * kMidtonesWidth);
    d.maxDetailLevel = std::log2(fullSize) - p.detailLevel - 1.0f;
    d.feathering = kDefaultFeathering * std::pow(2.0f, -p.haloControl);
    d.display = p.display;

    const float baseDiameter = fullSize * renderScale;
    for (int level = 0; level < kMaxLevels; ++level) {
        const float scale = std::pow(2.0f, -p.detailLevel - (float)level);
        d.noiseBias[level] = p.noiseBias * std::pow(2.0f, std::fmin((float)level - d.maxDetailLevel, 0.0f));
        d.radius[level] = 0.5f * std::fmin(scale, 0.5f) * baseDiameter;
    }
    // The smallest level with a radius under 2 pixels, past which the levels would be unused.
    d.maxUsedLevel = kMaxLevels - 1;
    for (int level = kMaxLevels - 1; level >= 0; --level) {
        if (d.radius[level] < 2.0f) {
            d.maxUsedLevel = level;
        }
    }

    std::fill(d.gainPerLevel, d.gainPerLevel + kMaxLevels, 0.f);
    const float gains[eBandCount] = { p.gainCoarse, p.gainMedium, p.gainFine };
    if ((p.display == eDisplayCoarse) || (p.display == eDisplayMedium) || (p.display == eDisplayFine)) {
        const int band = (int)p.display - (int)eDisplayCoarse;
        for (int level = 0; level <= d.maxUsedLevel; ++level) {
            d.gainPerLevel[level] = bandWeight(band, level, d.maxDetailLevel);
        }
    } else if (p.display == eDisplayOff) {
        for (int level = 0; level <= d.maxUsedLevel; ++level) {
            for (int band = 0; band < eBandCount; ++band) {
                d.gainPerLevel[level] += gains[band] * bandWeight(band, level, d.maxDetailLevel);
            }
        }
    }

    return d;
} // makePlan

float
bandWeight(int band,
           int level,
           float maxLevel)
{
    float t = (float)level / maxLevel - 0.5f;

    t = std::fmax(std::fmin(t, 0.5f), -0.5f);
    float weight = 0.0f;
    switch (band) {
    case eBandCoarse:
        weight = (t < 0.0f) ? std::sin(kPiF * t) : 0.0f;
        break;
    case eBandMedium:
        weight = std::cos(kPiF * t);
        break;
    case eBandFine:
        weight = (t >= 0.0f) ? std::sin(kPiF * t) : 0.0f;
        break;
    default:
        break;
    }

    return weight * weight;
}

float
extractDetails(float highpass,
               float lowpass,
               float noiseBias)
{
    const float logPixel = std::log2(std::fmax(highpass, kNormMin));
    const float logSmoothed = std::log2(std::fmax(lowpass, kNormMin));
    const float noisePower = noiseBias * noiseBias;
    const float combinedPower = lowpass * lowpass;
    const float wienerGain = std::fmax(combinedPower - noisePower, 0.0f) / std::fmax(combinedPower, kNormMin);

    return wienerGain * std::fmax(std::fmin(logPixel - logSmoothed, 5.0f), -5.0f);
}

float
shadowsHighlights(float lowpass,
                  const Plan& d)
{
    const float pivotOffset = std::log2(d.noiseBias[0] + kPivot);
    const float normalizedEv = std::log2(std::fmax(lowpass, kNormMin)) - pivotOffset;
    float correctionEv = 0.0f;

    if (normalizedEv <= -kMidtonesWidth) {
        correctionEv += d.slopeShadows * normalizedEv;
    } else if (normalizedEv >= kMidtonesWidth) {
        correctionEv += d.slopeHighlights * normalizedEv;
    } else {
        const float shiftedEv = normalizedEv + kMidtonesWidth;
        correctionEv += (d.polynomial[0] + d.polynomial[1] * shiftedEv + d.polynomial[2] * shiftedEv * shiftedEv);
    }

    return correctionEv - normalizedEv;
}

float
pixelLuminance(const float* pixel,
               int nComps)
{
    const int n = std::min(nComps, 3);
    float sum = 0.0f;

    for (int c = 0; c < n; ++c) {
        sum += pixel[c] * pixel[c];
    }
    if (n < 3) {
        sum *= 3.0f / (float)n;
    }
    const float norm = maxOrSecond(std::sqrt(sum), kNormMin);

    return (1.0f / std::sqrt(3.0f)) * norm;
}

bool
isIdentity(const Params& p)
{
    return (p.display == eDisplayOff) && (p.gainCoarse == 0.f) && (p.gainMedium == 0.f) && (p.gainFine == 0.f) && (p.gainShadows == 0.f) && (p.gainHighlights == 0.f);
}

void
computeMap(const float* image,
           int width,
           int height,
           int nComps,
           const Plan& d,
           float* map,
           const EdgeAware::ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t npixels = (std::size_t)width * height;
    std::vector<float> highpass(npixels);
    std::vector<float> lowpass(npixels);
    std::vector<float> corrections(npixels, 0.f);

    const float maxNoiseBias = d.noiseBias[d.maxUsedLevel];
    forRows(parallel, width, height, [&](std::size_t k) {
        lowpass[k] = pixelLuminance(&image[k * nComps], nComps) + maxNoiseBias;
    });

    for (int level = d.maxUsedLevel; level >= 0; --level) {
        std::memcpy(highpass.data(), lowpass.data(), npixels * sizeof(float));
        EdgeAware::eigfSurfaceBlur(lowpass.data(), width, height, d.radius[level], d.feathering, 1, EdgeAware::eGuidedBlendingLinear, 0.0f, kNormMin, 4.0f, parallel);
        const float gain = d.gainPerLevel[level];
        const float noiseBias = d.noiseBias[level];
        const float reduction = (level > 0) ? (d.noiseBias[level] - d.noiseBias[level - 1]) : 0.f;
        forRows(parallel, width, height, [&](std::size_t k) {
            corrections[k] += gain * extractDetails(highpass[k], lowpass[k], noiseBias);
            if (level > 0) {
                lowpass[k] -= reduction;
            }
        });
    }

    forRows(parallel, width, height, [&](std::size_t k) {
        switch (d.display) {
        case eDisplayOff:
            map[k] = std::exp2(corrections[k] + shadowsHighlights(lowpass[k], d));
            break;
        case eDisplayLowpass:
            map[k] = lowpass[k];
            break;
        default: {
            const float localEv = corrections[k];
            map[k] = localEv / std::sqrt(localEv * localEv + 1.0f) * 0.5f + 0.5f;
            break;
        }
        }
    });
} // computeMap

void
writePixel(const float* in,
           float mapValue,
           const Plan& d,
           int nComps,
           float* out)
{
    const int colours = (nComps == 4) ? 3 : nComps;

    switch (d.display) {
    case eDisplayOff:
        for (int c = 0; c < colours; ++c) {
            out[c] = in[c] * mapValue;
        }
        if (nComps == 4) {
            out[3] = in[3];
        }
        break;
    case eDisplayLowpass: {
        const float intensity = mapValue / ((d.noiseBias[0] + kPivot) + mapValue);
        float saturation = (intensity - 0.5f) * 2.0f;
        saturation *= saturation;
        // Blue below the pivot, yellow above it; a plane of fewer than three channels gets grey.
        const float blue[3] = { 0.0f, 0.0f, 1.0f };
        const float yellow[3] = { 1.0f, 1.0f, 0.0f };
        const float* tint = (intensity < 0.5f) ? blue : yellow;
        for (int c = 0; c < colours; ++c) {
            out[c] = (colours >= 3) ? intensity * ((1.0f - saturation) + saturation * tint[c]) : intensity;
        }
        if (nComps == 4) {
            out[3] = 1.0f;
        }
        break;
    }
    default:
        for (int c = 0; c < colours; ++c) {
            out[c] = mapValue;
        }
        if (nComps == 4) {
            out[3] = 1.0f;
        }
        break;
    }
} // writePixel
} // namespace ContrastTextureKernels

NATRON_NAMESPACE_EXIT
