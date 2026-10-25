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

#ifndef Engine_Nodes_Generator_NoiseCore_h
#define Engine_Nodes_Generator_NoiseCore_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <type_traits>

#include "libs/noise/Noise4D.h"

NATRON_NAMESPACE_ENTER

// The noise node's sampling core: one 4D noise value per point, from a basis generator
// (libs/noise/Noise4D.h), an optional fractal sum of octaves and an optional domain warp.
// Engine/Gpu/Kernels/noisecore.slang mirrors it line for line; keep the two in step, including the
// order of every floating-point operation. No fused multiply-add is used, so the core gives the
// same result on every x86-64 machine and matches the GPU kernel compiled with -fp-mode precise.
namespace NoiseCore {

using noise4d::NoiseAxis;
using noise4d::NoiseCoord;
using noise4d::uint;

// The basis generators.
constexpr int kNoiseTypePerlin = 0;
constexpr int kNoiseTypeSimplex = 1; // SuperSimplex (OpenSimplex2S)
constexpr int kNoiseTypeValue = 2;
constexpr int kNoiseTypeCellular = 3;
constexpr int kNoiseTypeWhite = 4;

// The cellular metrics and outputs.
constexpr int kNoiseMetricEuclidean = noise4d::kNoiseMetricEuclidean;
constexpr int kNoiseMetricManhattan = noise4d::kNoiseMetricManhattan;
constexpr int kNoiseMetricChebyshev = noise4d::kNoiseMetricChebyshev;
constexpr int kNoiseCellularF1 = noise4d::kNoiseCellularF1;
constexpr int kNoiseCellularF2 = noise4d::kNoiseCellularF2;
constexpr int kNoiseCellularF2MinusF1 = noise4d::kNoiseCellularF2MinusF1;
constexpr int kNoiseCellularCellValue = noise4d::kNoiseCellularCellValue;
constexpr int kNoiseCellularDistanceToEdge = noise4d::kNoiseCellularDistanceToEdge;
constexpr int kNoiseCellularSmoothF1 = noise4d::kNoiseCellularSmoothF1;

// The fractal combiners.
constexpr int kNoiseFractalNone = 0;
constexpr int kNoiseFractalFBm = 1;
constexpr int kNoiseFractalBillow = 2; // also called turbulence
constexpr int kNoiseFractalRidged = 3;
constexpr int kNoiseFractalHybrid = 4; // hybrid multifractal

// The domain warps.
constexpr int kNoiseWarpNone = 0;
constexpr int kNoiseWarpPerlin = 1;
constexpr int kNoiseWarpSimplex = 2;

constexpr int kNoiseMaxOctaves = 16;

constexpr uint kNoiseWarpSeed = 0x9E3779B9u;

// Everything that shapes the noise but the sample point. Plain scalars, so the struct can be
// pushed to the GPU unchanged (see noisecore.slang's NoiseParams).
struct NoiseParams {
    int type;
    uint seed;
    int fractal;
    int octaves; // clamped to [1, kNoiseMaxOctaves]
    float lacunarity; // frequency ratio between octaves
    float gain; // amplitude ratio between octaves
    float weightedStrength; // how much a low octave damps the next one's amplitude, in [0, 1]
    int cellularMetric;
    int cellularOutput;
    float jitter; // cellular feature point jitter, clamped to [0, 1]
    float smoothness; // smooth F1 blend width
    int warpType;
    float warpAmount; // displacement in noise-space units
    float warpFrequency; // warp field frequency relative to the noise
};
static_assert(std::is_trivially_copyable<NoiseParams>::value, "NoiseParams is pushed to the GPU");
static_assert(sizeof(NoiseParams) == 56, "NoiseParams must match noisecore.slang");

// Defaults: single-octave Perlin, a classic fBm setup once a fractal is chosen.
inline NoiseParams
defaultNoiseParams()
{
    NoiseParams p;
    p.type = kNoiseTypePerlin;
    p.seed = 0u;
    p.fractal = kNoiseFractalNone;
    p.octaves = 4;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.weightedStrength = 0.0f;
    p.cellularMetric = kNoiseMetricEuclidean;
    p.cellularOutput = kNoiseCellularF1;
    p.jitter = 1.0f;
    p.smoothness = 0.1f;
    p.warpType = kNoiseWarpNone;
    p.warpAmount = 0.0f;
    p.warpFrequency = 1.0f;
    return p;
}

// The basis generator of type at p, in [-1, 1].
inline float
noiseBasis(NoiseParams prm,
           int type,
           uint seed,
           NoiseCoord p)
{
    if (type == kNoiseTypeSimplex) {
        return noise4d::superSimplex4(seed, p);
    }
    if (type == kNoiseTypeValue) {
        return noise4d::value4(seed, p);
    }
    if (type == kNoiseTypeCellular) {
        return noise4d::cellular4(seed, p, prm.cellularMetric, prm.cellularOutput, prm.jitter, prm.smoothness);
    }
    if (type == kNoiseTypeWhite) {
        return noise4d::white4(seed, p);
    }
    return noise4d::perlin4(seed, p);
}

// p displaced by a 4D field of Perlin or SuperSimplex noise at warpFrequency times p, each
// component scaled by warpAmount.
inline NoiseCoord
noiseWarp(NoiseParams prm,
          NoiseCoord p)
{
    if ((prm.warpType != kNoiseWarpPerlin) && (prm.warpType != kNoiseWarpSimplex)) {
        return p;
    }
    NoiseCoord q = noise4d::coordScale(p, prm.warpFrequency);
    float d[4];
    for (int i = 0; i < 4; i++) {
        uint seed = prm.seed + kNoiseWarpSeed * (uint)(i + 1);
        float n;
        if (prm.warpType == kNoiseWarpSimplex) {
            n = noise4d::superSimplex4(seed, q);
        } else {
            n = noise4d::perlin4(seed, q);
        }
        d[i] = n * prm.warpAmount;
    }

    return noise4d::coordOffset(p, d[0], d[1], d[2], d[3]);
}

// The noise at p, in [-1, 1]. p is in noise space: one lattice cell per unit at the base
// frequency.
//
// The fractals sum octaves k = 0 .. octaves - 1, octave k sampling the basis at
// p * lacunarity^k (scaled from the split coordinate, so the octaves stay exact far from the
// origin) with seed + k:
// - fBm: sum of n_k * a_k, with a_0 = 1 and a_k+1 = a_k * gain * lerp(1, (n_k + 1) / 2,
//   weightedStrength) as in FastNoise2's FractalFBm.
// - Billow: fBm over |n_k| * 2 - 1.
// - Ridged: sum of (1 - 2 |n_k|) * a_k, with a_k+1 = a_k * gain * lerp(1, 1 - |n_k|,
//   weightedStrength) as in FastNoise2's FractalRidged.
// - Hybrid multifractal: sum of a_k * w_k * s_k over s_k = (n_k + 1) / 2, with a_k = gain^k,
//   w_0 = 1 and w_k+1 = min(1, 2 w_k s_k), so octaves only add detail where the lower ones are
//   high; mapped back from [0, 1].
// Each sum is divided by the sum of |gain|^k, the most it can reach for a weighted strength in
// [0, 1], and clamped to [-1, 1].
inline float
noise4(NoiseParams prm,
       NoiseCoord p)
{
    p = noiseWarp(prm, p);
    if ((prm.fractal != kNoiseFractalFBm) && (prm.fractal != kNoiseFractalBillow) && (prm.fractal != kNoiseFractalRidged) && (prm.fractal != kNoiseFractalHybrid)) {
        return noiseBasis(prm, prm.type, prm.seed, p);
    }

    int octaves = prm.octaves < 1 ? 1 : (prm.octaves > kNoiseMaxOctaves ? kNoiseMaxOctaves : prm.octaves);
    float gainBound = noise4d::vabs(prm.gain);
    uint seed = prm.seed;
    float frequency = 1.0f;
    float amp = 1.0f;
    float bound = 0.0f;
    float boundAmp = 1.0f;
    float weight = 1.0f;
    float sum = 0.0f;
    for (int k = 0; k < octaves; k++) {
        NoiseCoord q = p;
        if (k > 0) {
            q = noise4d::coordScale(p, frequency);
        }
        float n = noiseBasis(prm, prm.type, seed, q);
        if (prm.fractal == kNoiseFractalRidged) {
            float r = noise4d::vabs(n);
            sum += (1.0f - 2.0f * r) * amp;
            amp *= noise4d::lerpV(1.0f, 1.0f - r, prm.weightedStrength) * prm.gain;
        } else if (prm.fractal == kNoiseFractalHybrid) {
            float s = (n + 1.0f) * 0.5f;
            sum += amp * weight * s;
            weight = noise4d::vmin(1.0f, weight * s * 2.0f);
            amp *= prm.gain;
        } else {
            if (prm.fractal == kNoiseFractalBillow) {
                n = noise4d::vabs(n) * 2.0f - 1.0f;
            }
            sum += n * amp;
            amp *= noise4d::lerpV(1.0f, (n + 1.0f) * 0.5f, prm.weightedStrength) * prm.gain;
        }
        bound += boundAmp;
        boundAmp *= gainBound;
        seed += 1u;
        frequency *= prm.lacunarity;
    }

    float v = sum / bound;
    if (prm.fractal == kNoiseFractalHybrid) {
        v = v * 2.0f - 1.0f;
    }

    // A negative gain or a weighted strength outside [0, 1] can leave the bound.
    return noise4d::vmax(-1.0f, noise4d::vmin(1.0f, v));
}
} // namespace NoiseCore

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_NoiseCore_h
