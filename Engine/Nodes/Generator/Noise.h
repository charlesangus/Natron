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

#ifndef Engine_Nodes_Generator_Noise_h
#define Engine_Nodes_Generator_Noise_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <type_traits>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Generator/NoiseCore.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_NOISE "Noise"
#define PLUGIN_MAJOR_NATRON_NOISE 1

#define kNoiseParamType "type"
#define kNoiseParamSize "noiseSize"
#define kNoiseParamZOffset "zOffset"
#define kNoiseParamEvolution "evolution"
#define kNoiseParamLoopPeriod "loopPeriod"
#define kNoiseParamSeed "seed"
#define kNoiseParamReseedPerFrame "reseedPerFrame"
#define kNoiseParamFractal "fractal"
#define kNoiseParamOctaves "octaves"
#define kNoiseParamLacunarity "lacunarity"
#define kNoiseParamGain "gain"
#define kNoiseParamWeightedStrength "weightedStrength"
#define kNoiseParamMetric "metric"
#define kNoiseParamOutput "output"
#define kNoiseParamJitter "jitter"
#define kNoiseParamSmoothness "smoothness"
#define kNoiseParamWarpType "warpType"
#define kNoiseParamWarpAmount "warpAmount"
#define kNoiseParamWarpFrequency "warpFrequency"
#define kNoiseParamTranslate "translate"
#define kNoiseParamRotate "rotate"
#define kNoiseParamScale "scale"
#define kNoiseParamCenter "center"
#define kNoiseParamColor0 "color0"
#define kNoiseParamColor1 "color1"
#define kNoiseParamAmplitude "amplitude"
#define kNoiseParamOffset "offset"
#define kNoiseParamDecorrelate "decorrelate"

NATRON_NAMESPACE_ENTER

/**
 * @brief Everything a Noise pixel needs at one render scale, in plain 32-bit words: the block the
 * GPU kernel reads (NoisePixel in Engine/Gpu/Kernels/noise.slang mirrors it field for field) and
 * the CPU row evaluates.
 *
 * A pixel (x, y) at the render scale samples noise space at
 *   nx = mxx (x + 1/2) + mxy (y + 1/2) + bx,  ny = myx (x + 1/2) + myy (y + 1/2) + by,
 * with each product formed as a split cell and fraction (noise4d::axisScale), so the pattern keeps
 * its precision far from the origin; bx, by, z and w come split from the host's doubles.
 **/
struct NoisePixelParams {
    NoiseCore::NoiseParams noise;
    // 1 when each channel takes its own seed, 0 when one value fills every channel.
    std::uint32_t decorrelate;
    std::uint32_t reserved;
    float color0[4];
    float color1[4];
    float amplitude;
    float offset;
    float mxx;
    float mxy;
    float myx;
    float myy;
    std::uint32_t bxCell;
    float bxFraction;
    std::uint32_t byCell;
    float byFraction;
    std::uint32_t zCell;
    float zFraction;
    std::uint32_t wCell;
    float wFraction;
};
static_assert(std::is_trivially_copyable<NoisePixelParams>::value, "NoisePixelParams is copied to the GPU");
static_assert(sizeof(NoisePixelParams) == 152, "NoisePixelParams must match noise.slang");

// What channel c (in channel-bit order: R, G, B, A) adds to the seed when the channels are
// decorrelated.
constexpr std::uint32_t kNoiseChannelSeed = 0x632BE5ABu;

// The seed white noise adds per frame when it is reseeded every frame.
constexpr std::uint32_t kNoiseFrameSeed = 0x85EBCA6Bu;

/**
 * @brief The noise value, in [-1, 1], at pixel (x, y) for the seed of channel c.
 **/
float noisePixelValue(const NoisePixelParams& p,
                      int x,
                      int y,
                      int channel);

/**
 * @brief The four channels of pixel (x, y), in channel-bit order; only those whose bit is set in
 * channelMask are computed, the others are zero.
 **/
void noisePixel(const NoisePixelParams& p,
                int x,
                int y,
                unsigned channelMask,
                float rgba[4]);

/**
 * @brief Noise: a 4D noise generator. The pattern lives in canonical space, so it is the same
 * whatever the tile split, the region of definition or the render scale. Each pixel centre goes
 * through the inverse of the transform (translate, rotate and scale about the centre) and is
 * divided by the size to reach noise space, whose third and fourth axes are zOffset and
 * evolution, or a circle through them for a seamless loop; the noise value then maps colour 0 to
 * colour 1 through amplitude and offset.
 **/
struct NoiseOp {
    // Knob values; the geometry at a render scale is derived from them.
    struct Params {
        NoiseCore::NoiseParams noise;
        double sizeX;
        double sizeY;
        double zOffset;
        double evolution;
        double loopPeriod;
        double translateX;
        double translateY;
        double rotate; // degrees
        double scaleX;
        double scaleY;
        double centerX;
        double centerY;
        double par;
        float color0[4];
        float color1[4];
        float amplitude;
        float offset;
        bool decorrelate;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_NOISE,
        .label = "Noise",
        .description = "Generate 4D noise: Perlin, simplex, value, cellular (Voronoi) or white noise, "
                       "optionally summed over octaves (fBm, turbulence, ridged, hybrid multifractal) and "
                       "domain-warped.\n"
                       "The pattern is computed in canonical coordinates, so it does not depend on the "
                       "render scale or on how the image is tiled. Evolution moves along the fourth axis "
                       "and animates the pattern continuously; a loop period makes that animation seamless.\n"
                       "Each channel maps the noise from colour 0 to colour 1; decorrelated channels each "
                       "get their own noise.",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_NOISE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Noise.png",
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::targetSelect(),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    void row(const Params& params,
             const PassContext& context,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params,
                 const PassContext& context) const;

    /**
     * @brief What params give a pixel at the render scale renderScaleX x renderScaleY.
     **/
    static NoisePixelParams pixelParams(const Params& params,
                                        double renderScaleX,
                                        double renderScaleY);

    const NativeGenerator* generator = nullptr;
    KnobChoiceWPtr type;
    KnobDoubleWPtr size;
    KnobDoubleWPtr zOffset;
    KnobDoubleWPtr evolution;
    KnobDoubleWPtr loopPeriod;
    KnobIntWPtr seed;
    KnobBoolWPtr reseedPerFrame;
    KnobChoiceWPtr fractal;
    KnobIntWPtr octaves;
    KnobDoubleWPtr lacunarity;
    KnobDoubleWPtr gain;
    KnobDoubleWPtr weightedStrength;
    KnobChoiceWPtr metric;
    KnobChoiceWPtr output;
    KnobDoubleWPtr jitter;
    KnobDoubleWPtr smoothness;
    KnobChoiceWPtr warpType;
    KnobDoubleWPtr warpAmount;
    KnobDoubleWPtr warpFrequency;
    KnobDoubleWPtr translate;
    KnobDoubleWPtr rotate;
    KnobDoubleWPtr scale;
    KnobDoubleWPtr center;
    KnobColorWPtr color0;
    KnobColorWPtr color1;
    KnobDoubleWPtr amplitude;
    KnobDoubleWPtr offset;
    KnobBoolWPtr decorrelate;
};

static_assert(ScaledPointOp<NoiseOp>);

using Noise = GeneratorNode<NoiseOp>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_Noise_h
