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

#include "Noise.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/PixelKernel.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
const double kPi = 3.14159265358979323846;

// A coordinate beyond this is clamped before it is split: its cell would no longer be exact in a
// double, and the noise wraps every 2^32 cells anyway.
const double kNoiseSplitLimit = 1e15;

std::string
trStd(const char* text)
{
    return EffectInstance::tr(text).toStdString();
}

double
finiteOr(double v,
         double fallback)
{
    return std::isfinite(v) ? v : fallback;
}

// v as a cell (modulo 2^32, two's complement, as the core wraps its cells) and a fraction in
// [0, 1).
void
splitAxis(double v,
          std::uint32_t* cell,
          float* fraction)
{
    v = finiteOr(v, 0.);
    v = (std::max)(-kNoiseSplitLimit, (std::min)(kNoiseSplitLimit, v));
    const double k = std::floor(v);
    std::int64_t c = (std::int64_t)k;
    float f = (float)(v - k);
    // A fraction just under 1 rounds up to it in float.
    if (f >= 1.f) {
        f = 0.f;
        ++c;
    }
    *cell = (std::uint32_t)(std::uint64_t)c;
    *fraction = f;
}

// The noise-space point of pixel (x, y): see NoisePixelParams.
noise4d::NoiseCoord
noiseCoordAt(const NoisePixelParams& p,
             int x,
             int y)
{
    const noise4d::uint ux = (noise4d::uint)x;
    const noise4d::uint uy = (noise4d::uint)y;
    noise4d::NoiseAxis bx;
    bx.c = p.bxCell;
    bx.f = p.bxFraction;
    noise4d::NoiseAxis by;
    by.c = p.byCell;
    by.f = p.byFraction;
    noise4d::NoiseAxis z;
    z.c = p.zCell;
    z.f = p.zFraction;
    noise4d::NoiseAxis w;
    w.c = p.wCell;
    w.f = p.wFraction;
    const noise4d::NoiseAxis nx = noise4d::axisAdd(noise4d::axisAdd(noise4d::axisScale(ux, 0.5f, p.mxx), noise4d::axisScale(uy, 0.5f, p.mxy)), bx);
    const noise4d::NoiseAxis ny = noise4d::axisAdd(noise4d::axisAdd(noise4d::axisScale(ux, 0.5f, p.myx), noise4d::axisScale(uy, 0.5f, p.myy)), by);

    return noise4d::coordFromAxes(nx, ny, z, w);
}

NoiseCore::NoiseParams
channelNoiseParams(const NoisePixelParams& p,
                   int channel)
{
    NoiseCore::NoiseParams prm = p.noise;
    if (p.decorrelate) {
        prm.seed = prm.seed + kNoiseChannelSeed * (std::uint32_t)channel;
    }

    return prm;
}

// Colour 0 at t = 0 and colour 1 at t = 1, where t spans [0, 1] over the noise's [-1, 1] at
// amplitude 1 and offset 0.
float
noiseColor(const NoisePixelParams& p,
           float n,
           int channel)
{
    const float t = 0.5f * (1.0f + p.amplitude * n) + p.offset;

    return p.color0[channel] + (p.color1[channel] - p.color0[channel]) * t;
}

KnobDoublePtr
addDoubleKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const char* label,
              const char* hint,
              int dimension,
              double defaultValue,
              double minimum,
              double maximum,
              double displayMinimum,
              double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, trStd(label), dimension);

    knob->setName(name);
    knob->setHintToolTip(trStd(hint));
    for (int d = 0; d < dimension; ++d) {
        knob->setMinimum(minimum, d);
        knob->setMaximum(maximum, d);
        knob->setDisplayMinimum(displayMinimum, d);
        knob->setDisplayMaximum(displayMaximum, d);
        knob->setDefaultValue(defaultValue, d);
    }
    page->addKnob(knob);

    return knob;
}

struct NoiseChoice {
    const char* id;
    const char* label;
    const char* hint;
};

template <std::size_t N>
KnobChoicePtr
addChoiceKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const char* label,
              const char* hint,
              const NoiseChoice (&choices)[N],
              int defaultValue)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, trStd(label));

    knob->setName(name);
    knob->setHintToolTip(trStd(hint));
    std::vector<ChoiceOption> options;
    for (std::size_t i = 0; i < N; ++i) {
        options.push_back(ChoiceOption(choices[i].id, trStd(choices[i].label), trStd(choices[i].hint)));
    }
    knob->populateChoices(options);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

KnobColorPtr
addColorKnob(KnobHolder* holder,
             const KnobPagePtr& page,
             const char* name,
             const char* label,
             const char* hint,
             double value,
             double alpha)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, trStd(label), 4);

    knob->setName(name);
    knob->setHintToolTip(trStd(hint));
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(i < 3 ? value : alpha, i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);

    return knob;
}

int
clampChoice(int v,
            int last)
{
    return (std::max)(0, (std::min)(last, v));
}

void
readColor(const KnobReader& reader,
          const KnobColorWPtr& knob,
          double fallbackValue,
          float out[4])
{
    for (int i = 0; i < 4; ++i) {
        out[i] = (float)reader.valueOr(knob, i < 3 ? fallbackValue : 1., i);
    }
}
} // anonymous namespace

float
noisePixelValue(const NoisePixelParams& p,
                int x,
                int y,
                int channel)
{
    return NoiseCore::noise4(channelNoiseParams(p, channel), noiseCoordAt(p, x, y));
}

void
noisePixel(const NoisePixelParams& p,
           int x,
           int y,
           unsigned channelMask,
           float rgba[4])
{
    for (int c = 0; c < 4; ++c) {
        rgba[c] = 0.f;
    }
    if ((channelMask & 15u) == 0) {
        return;
    }
    const noise4d::NoiseCoord q = noiseCoordAt(p, x, y);
    if (p.decorrelate) {
        for (int c = 0; c < 4; ++c) {
            if (channelMask & (1u << c)) {
                rgba[c] = noiseColor(p, NoiseCore::noise4(channelNoiseParams(p, c), q), c);
            }
        }

        return;
    }
    const float n = NoiseCore::noise4(p.noise, q);
    for (int c = 0; c < 4; ++c) {
        if (channelMask & (1u << c)) {
            rgba[c] = noiseColor(p, n, c);
        }
    }
}

void
NoiseOp::createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
{
    static const NoiseChoice kTypes[] = {
        { "perlin", "Perlin", "Gradient noise on a square lattice." },
        { "simplex", "Simplex", "Gradient noise on a simplex lattice (OpenSimplex2S): fewer axis-aligned artefacts than Perlin, at about twice the cost." },
        { "value", "Value", "Random values at the lattice points, smoothly interpolated." },
        { "cellular", "Cellular", "Voronoi noise: distances to jittered feature points, or the value of the nearest one. See Metric and Output." },
        { "white", "White", "A random value per cell, without interpolation. At a size of 1 every pixel is its own cell." },
    };
    static const NoiseChoice kFractals[] = {
        { "none", "None", "A single octave." },
        { "fbm", "fBm", "Fractional Brownian motion: octaves of rising frequency and falling amplitude, summed." },
        { "turbulence", "Turbulence", "fBm over the absolute value of each octave (billow)." },
        { "ridged", "Ridged", "Ridged multifractal: sharp ridges where each octave crosses zero." },
        { "hybrid", "Hybrid", "Hybrid multifractal: higher octaves add detail only where the lower ones are high." },
    };
    static const NoiseChoice kMetrics[] = {
        { "euclidean", "Euclidean", "Straight-line distance." },
        { "manhattan", "Manhattan", "Sum of the distances along each axis." },
        { "chebyshev", "Chebyshev", "Largest distance along any axis." },
    };
    static const NoiseChoice kOutputs[] = {
        { "f1", "F1", "Distance to the nearest feature point." },
        { "f2", "F2", "Distance to the second nearest feature point." },
        { "f2MinusF1", "F2 - F1", "Difference of the two: dark along the cell edges." },
        { "cellValue", "Cell Value", "A random value per cell: flat Voronoi cells." },
        { "distanceToEdge", "Distance to Edge", "Distance to the nearest cell edge: exact for Euclidean, (F2 - F1) / 2 for Manhattan and Chebyshev." },
        { "smoothF1", "Smooth F1", "F1 with a smooth minimum over the feature points, of width Smoothness." },
    };
    static const NoiseChoice kWarps[] = {
        { "none", "None", "No domain warp." },
        { "perlin", "Perlin", "Displace the sample point by Perlin noise." },
        { "simplex", "Simplex", "Displace the sample point by simplex noise." },
    };

    type = addChoiceKnob(holder, page, kNoiseParamType, "Type", "The basis noise.", kTypes, NoiseCore::kNoiseTypePerlin);

    KnobDoublePtr sizeKnob = addDoubleKnob(holder, page, kNoiseParamSize, "Size",
                                           "Size of a noise cell in pixels, along x and y, before the transform.", 2, 100., 1e-3, DBL_MAX, 1., 500.);
    sizeKnob->setCanAutoFoldDimensions(true);
    size = sizeKnob;
    zOffset = addDoubleKnob(holder, page, kNoiseParamZOffset, "Z Offset",
                            "Position along the third noise axis, in noise cells.", 1, 0., -DBL_MAX, DBL_MAX, -10., 10.);
    evolution = addDoubleKnob(holder, page, kNoiseParamEvolution, "Evolution",
                              "Position along the fourth noise axis, in noise cells. Animate it to evolve the pattern continuously.", 1, 0., -DBL_MAX, DBL_MAX, 0., 10.);
    loopPeriod = addDoubleKnob(holder, page, kNoiseParamLoopPeriod, "Loop Period",
                               "When positive, Evolution walks a circle of this circumference through the third and fourth noise axes "
                               "instead of a line, so the pattern repeats seamlessly every Loop Period of Evolution. 0 does not loop.",
                               1, 0., 0., DBL_MAX, 0., 10.);

    KnobIntPtr seedKnob = AppManager::createKnob<KnobInt>(holder, trStd("Seed"), 1);
    seedKnob->setName(kNoiseParamSeed);
    seedKnob->setHintToolTip(trStd("Random seed: each value gives a different pattern."));
    seedKnob->setMinimum(INT_MIN);
    seedKnob->setMaximum(INT_MAX);
    seedKnob->setDisplayMinimum(0);
    seedKnob->setDisplayMaximum(1000);
    seedKnob->setDefaultValue(0);
    page->addKnob(seedKnob);
    seed = seedKnob;

    KnobBoolPtr reseedKnob = AppManager::createKnob<KnobBool>(holder, trStd("Reseed Per Frame"));
    reseedKnob->setName(kNoiseParamReseedPerFrame);
    reseedKnob->setHintToolTip(trStd("White noise only: draw a new pattern on every frame, like film grain. Off, white noise only changes with the seed and the noise-space position."));
    reseedKnob->setDefaultValue(false);
    page->addKnob(reseedKnob);
    reseedPerFrame = reseedKnob;

    fractal = addChoiceKnob(holder, page, kNoiseParamFractal, "Fractal", "How octaves of the basis noise are combined.", kFractals, NoiseCore::kNoiseFractalFBm);

    KnobIntPtr octavesKnob = AppManager::createKnob<KnobInt>(holder, trStd("Octaves"), 1);
    octavesKnob->setName(kNoiseParamOctaves);
    octavesKnob->setHintToolTip(trStd("Number of octaves the fractal sums, at most 16."));
    octavesKnob->setMinimum(1);
    octavesKnob->setMaximum(NoiseCore::kNoiseMaxOctaves);
    octavesKnob->setDisplayMinimum(1);
    octavesKnob->setDisplayMaximum(NoiseCore::kNoiseMaxOctaves);
    octavesKnob->setDefaultValue(4);
    page->addKnob(octavesKnob);
    octaves = octavesKnob;

    lacunarity = addDoubleKnob(holder, page, kNoiseParamLacunarity, "Lacunarity",
                               "Frequency ratio between one octave and the next.", 1, 2., -DBL_MAX, DBL_MAX, 1., 4.);
    gain = addDoubleKnob(holder, page, kNoiseParamGain, "Gain",
                         "Amplitude ratio between one octave and the next.", 1, 0.5, -DBL_MAX, DBL_MAX, 0., 1.);
    weightedStrength = addDoubleKnob(holder, page, kNoiseParamWeightedStrength, "Weighted Strength",
                                     "fBm, Turbulence and Ridged: how much a low value of one octave damps the next.", 1, 0., -DBL_MAX, DBL_MAX, 0., 1.);

    metric = addChoiceKnob(holder, page, kNoiseParamMetric, "Metric", "Cellular only: the distance between a point and a feature point.", kMetrics, NoiseCore::kNoiseMetricEuclidean);
    output = addChoiceKnob(holder, page, kNoiseParamOutput, "Output", "Cellular only: what the noise value is made of.", kOutputs, NoiseCore::kNoiseCellularF1);
    jitter = addDoubleKnob(holder, page, kNoiseParamJitter, "Jitter",
                           "Cellular only: how far feature points stray from their cell centres, from 0 (a regular grid) to 1.", 1, 1., 0., 1., 0., 1.);
    smoothness = addDoubleKnob(holder, page, kNoiseParamSmoothness, "Smoothness",
                               "Cellular Smooth F1 only: width of the smooth minimum, in noise cells.", 1, 0.1, 0., DBL_MAX, 0., 1.);

    warpType = addChoiceKnob(holder, page, kNoiseParamWarpType, "Warp", "Domain warp: displace each sample point by another noise field first.", kWarps, NoiseCore::kNoiseWarpNone);
    warpAmount = addDoubleKnob(holder, page, kNoiseParamWarpAmount, "Warp Amount",
                               "Largest displacement of the warp, in noise cells.", 1, 0.5, -DBL_MAX, DBL_MAX, 0., 2.);
    warpFrequency = addDoubleKnob(holder, page, kNoiseParamWarpFrequency, "Warp Frequency",
                                  "Frequency of the warp field relative to the noise.", 1, 1., -DBL_MAX, DBL_MAX, 0., 4.);

    KnobDoublePtr translateKnob = addDoubleKnob(holder, page, kNoiseParamTranslate, "Translate",
                                                "Translation of the pattern along x and y in pixels.", 2, 0., -DBL_MAX, DBL_MAX, -10000., 10000.);
    translateKnob->setSpatial(true);
    translateKnob->disableSlider();
    translate = translateKnob;
    rotate = addDoubleKnob(holder, page, kNoiseParamRotate, "Rotate",
                           "Rotation of the pattern in degrees around the Center.", 1, 0., -DBL_MAX, DBL_MAX, -180., 180.);
    KnobDoublePtr scaleKnob = addDoubleKnob(holder, page, kNoiseParamScale, "Scale",
                                            "Scale of the pattern along x and y around the Center.", 2, 1., -DBL_MAX, DBL_MAX, 0.1, 10.);
    scaleKnob->setCanAutoFoldDimensions(true);
    scale = scaleKnob;
    KnobDoublePtr centerKnob = addDoubleKnob(holder, page, kNoiseParamCenter, "Center",
                                             "Centre of rotation and scale.", 2, 0.5, -DBL_MAX, DBL_MAX, -10000., 10000.);
    centerKnob->setSpatial(true);
    centerKnob->setDefaultValuesAreNormalized(true);
    // setDefaultValue applies the default immediately, so it has to be set again once the knob
    // knows it is normalized, for the value to become the middle of the project format.
    for (int d = 0; d < 2; ++d) {
        centerKnob->setDefaultValue(0.5, d);
    }
    centerKnob->disableSlider();
    center = centerKnob;

    color0 = addColorKnob(holder, page, kNoiseParamColor0, "Color 0", "The colour where the noise is at its lowest.", 0., 1.);
    color1 = addColorKnob(holder, page, kNoiseParamColor1, "Color 1", "The colour where the noise is at its highest.", 1., 1.);
    amplitude = addDoubleKnob(holder, page, kNoiseParamAmplitude, "Amplitude",
                              "Scales the noise around the middle of Color 0 and Color 1. At 1 the noise spans them.", 1, 1., -DBL_MAX, DBL_MAX, 0., 2.);
    offset = addDoubleKnob(holder, page, kNoiseParamOffset, "Offset",
                           "Added to the noise after Amplitude, as a fraction of the way from Color 0 to Color 1.", 1, 0., -DBL_MAX, DBL_MAX, -1., 1.);

    KnobBoolPtr decorrelateKnob = AppManager::createKnob<KnobBool>(holder, trStd("Decorrelate Channels"));
    decorrelateKnob->setName(kNoiseParamDecorrelate);
    decorrelateKnob->setHintToolTip(trStd("Give each channel its own noise, from a seed offset per channel, instead of one value in every channel."));
    decorrelateKnob->setDefaultValue(false);
    page->addKnob(decorrelateKnob);
    decorrelate = decorrelateKnob;
} // NoiseOp::createKnobs

NoiseOp::Params
NoiseOp::bake(const KnobReader& reader) const
{
    Params params;
    NoiseCore::NoiseParams& noise = params.noise;

    noise = NoiseCore::defaultNoiseParams();
    noise.type = clampChoice(reader.valueOr(type, NoiseCore::kNoiseTypePerlin), NoiseCore::kNoiseTypeWhite);
    noise.seed = (std::uint32_t)reader.valueOr(seed, 0);
    if ((noise.type == NoiseCore::kNoiseTypeWhite) && reader.valueOr(reseedPerFrame, false)) {
        const double frame = std::floor(finiteOr(reader.time(), 0.));
        noise.seed = noise.seed + kNoiseFrameSeed * (std::uint32_t)(std::int64_t)frame;
    }
    noise.fractal = clampChoice(reader.valueOr(fractal, NoiseCore::kNoiseFractalFBm), NoiseCore::kNoiseFractalHybrid);
    noise.octaves = (std::max)(1, (std::min)(NoiseCore::kNoiseMaxOctaves, (int)reader.valueOr(octaves, 4)));
    noise.lacunarity = (float)finiteOr(reader.valueOr(lacunarity, 2.), 2.);
    noise.gain = (float)finiteOr(reader.valueOr(gain, 0.5), 0.5);
    noise.weightedStrength = (float)finiteOr(reader.valueOr(weightedStrength, 0.), 0.);
    noise.cellularMetric = clampChoice(reader.valueOr(metric, NoiseCore::kNoiseMetricEuclidean), NoiseCore::kNoiseMetricChebyshev);
    noise.cellularOutput = clampChoice(reader.valueOr(output, NoiseCore::kNoiseCellularF1), NoiseCore::kNoiseCellularSmoothF1);
    noise.jitter = (float)(std::max)(0., (std::min)(1., finiteOr(reader.valueOr(jitter, 1.), 1.)));
    noise.smoothness = (float)(std::max)(0., finiteOr(reader.valueOr(smoothness, 0.1), 0.1));
    noise.warpType = clampChoice(reader.valueOr(warpType, NoiseCore::kNoiseWarpNone), NoiseCore::kNoiseWarpSimplex);
    noise.warpAmount = (float)finiteOr(reader.valueOr(warpAmount, 0.5), 0.);
    noise.warpFrequency = (float)finiteOr(reader.valueOr(warpFrequency, 1.), 1.);

    params.sizeX = reader.valueOr(size, 100., 0);
    params.sizeY = reader.valueOr(size, 100., 1);
    params.zOffset = reader.valueOr(zOffset, 0.);
    params.evolution = reader.valueOr(evolution, 0.);
    params.loopPeriod = reader.valueOr(loopPeriod, 0.);
    params.translateX = reader.valueOr(translate, 0., 0);
    params.translateY = reader.valueOr(translate, 0., 1);
    params.rotate = reader.valueOr(rotate, 0.);
    params.scaleX = reader.valueOr(scale, 1., 0);
    params.scaleY = reader.valueOr(scale, 1., 1);
    params.centerX = reader.valueOr(center, 0., 0);
    params.centerY = reader.valueOr(center, 0., 1);
    params.par = generator ? generator->getPixelAspectRatio() : 1.;
    readColor(reader, color0, 0., params.color0);
    readColor(reader, color1, 1., params.color1);
    params.amplitude = (float)reader.valueOr(amplitude, 1.);
    params.offset = (float)reader.valueOr(offset, 0.);
    params.decorrelate = reader.valueOr(decorrelate, false);

    return params;
} // NoiseOp::bake

NoisePixelParams
NoiseOp::pixelParams(const Params& params,
                     double renderScaleX,
                     double renderScaleY)
{
    NoisePixelParams p;

    p.noise = params.noise;
    p.decorrelate = params.decorrelate ? 1u : 0u;
    p.reserved = 0u;
    for (int c = 0; c < 4; ++c) {
        p.color0[c] = params.color0[c];
        p.color1[c] = params.color1[c];
    }
    p.amplitude = params.amplitude;
    p.offset = params.offset;

    // Canonical c maps to the pattern's plane through the inverse of c = centre + translate +
    // R(rotate) S(scale) (q - centre), then to noise space as q / size: n = A c + b, with
    // A = size^-1 S^-1 R(-rotate) and b = size^-1 (centre - S^-1 R(-rotate) (translate + centre)).
    // A singular scale or size sends the whole plane to one point.
    const double angle = finiteOr(params.rotate, 0.) * kPi / 180.;
    const double cosA = std::cos(angle);
    const double sinA = std::sin(angle);
    const double invX = finiteOr(1. / (params.sizeX * params.scaleX), 0.);
    const double invY = finiteOr(1. / (params.sizeY * params.scaleY), 0.);
    const double invSizeX = finiteOr(1. / params.sizeX, 0.);
    const double invSizeY = finiteOr(1. / params.sizeY, 0.);
    const double a00 = invX * cosA;
    const double a01 = invX * sinA;
    const double a10 = -invY * sinA;
    const double a11 = invY * cosA;
    const double tcX = finiteOr(params.translateX, 0.) + finiteOr(params.centerX, 0.);
    const double tcY = finiteOr(params.translateY, 0.) + finiteOr(params.centerY, 0.);
    const double bx = invSizeX * finiteOr(params.centerX, 0.) - (a00 * tcX + a01 * tcY);
    const double by = invSizeY * finiteOr(params.centerY, 0.) - (a10 * tcX + a11 * tcY);

    // Pixel x covers canonical [x, x + 1) par / renderScaleX, pixel y [y, y + 1) / renderScaleY.
    const double pixelX = finiteOr(params.par / renderScaleX, 1.);
    const double pixelY = finiteOr(1. / renderScaleY, 1.);
    p.mxx = (float)finiteOr(a00 * pixelX, 0.);
    p.mxy = (float)finiteOr(a01 * pixelY, 0.);
    p.myx = (float)finiteOr(a10 * pixelX, 0.);
    p.myy = (float)finiteOr(a11 * pixelY, 0.);
    splitAxis(bx, &p.bxCell, &p.bxFraction);
    splitAxis(by, &p.byCell, &p.byFraction);

    double z = finiteOr(params.zOffset, 0.);
    double w = finiteOr(params.evolution, 0.);
    const double period = finiteOr(params.loopPeriod, 0.);
    if (period > 0.) {
        // A circle of circumference period, so evolution moves through noise space at the same
        // speed as without the loop.
        const double radius = period / (2. * kPi);
        const double turn = 2. * kPi * (w / period);
        z = z + radius * std::cos(turn);
        w = radius * std::sin(turn);
    }
    splitAxis(z, &p.zCell, &p.zFraction);
    splitAxis(w, &p.wCell, &p.wFraction);

    return p;
} // NoiseOp::pixelParams

bool
NoiseOp::isIdentity(const Params& /*params*/,
                    const PassContext& /*context*/) const
{
    return false;
}

void
NoiseOp::row(const Params& params,
             const PassContext& context,
             const RowIO& io) const
{
    const OfxPointD renderScale = context.scale.toOfxPointD();
    const NoisePixelParams p = pixelParams(params, renderScale.x, renderScale.y);
    const GeneratorRowChannels channels(io);
    unsigned channelMask = 0;

    for (int k = 0; k < channels.count; ++k) {
        channelMask |= 1u << channels.rgbaIndex[k];
    }
    float* dst = io.dst;
    for (int i = 0; i < io.width; ++i, dst += io.nComps) {
        float rgba[4];
        noisePixel(p, io.x0 + i, io.y, channelMask, rgba);
        channels.write(rgba, dst);
    }
}

GpuOpRef
NoiseOp::gpu(const Params& params,
             const PassContext& context) const
{
#ifdef NATRON_HAVE_GPU
    const OfxPointD renderScale = context.scale.toOfxPointD();

    return GpuOpRef::compiled((int)gpu::GpuKernelId::noise, pixelParams(params, renderScale.x, renderScale.y));
#else
    Q_UNUSED(params);
    Q_UNUSED(context);

    return GpuOpRef::none();
#endif
}

NATRON_NAMESPACE_EXIT
