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

#ifndef Engine_Nodes_Color_Posterize_h
#define Engine_Nodes_Color_Posterize_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_POSTERIZE "Posterize"
#define PLUGIN_MAJOR_NATRON_POSTERIZE 1

#define kPosterizeParamLevels "levels"
#define kPosterizeParamRangeMin "rangeMin"
#define kPosterizeParamRangeMax "rangeMax"
#define kPosterizeParamDither "dither"
#define kPosterizeParamSeed "seed"
#define kPosterizeParamStaticSeed "staticSeed"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kPosterizeInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Quantizes each processed channel to `levels` evenly spaced values from rangeMin to
 * rangeMax, optionally dithered. Values outside the range step on at the same spacing.
 **/
struct PosterizeOp {
    enum DitherEnum {
        eDitherNone = 0,
        eDitherBayer2,
        eDitherBayer4,
        eDitherBayer8,
        eDitherBlueNoise14,
        eDitherBlueNoise25,
        eDitherRandom,
        eDitherCount
    };

    // How a value picks between its two neighbouring levels.
    enum KindEnum {
        eKindNone = 0,
        eKindOrdered,
        eKindRandom
    };

    /**
     * @brief For a value v, d = v - rangeMin. With no dither the level is d·binScale rounded
     * as Quantize did; dithered, it is floor(d·levelScale) or one above, by whether the
     * fraction is above the threshold: the tiled matrix's entry at the pixel, or a hash of the
     * pixel, the channel and seed. The output is level·outScale + rangeMin.
     **/
    struct Params {
        bool identity = true;
        int dither = eDitherNone;
        float rangeMin = 0.f;
        float binScale = 1.f;
        float levelScale = 1.f;
        float outScale = 1.f;
        std::uint32_t seed = 0;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_POSTERIZE,
        .label = "Posterize",
        .description = "Reduce each channel to a number of evenly spaced levels.\n"
                       "\n"
                       "Levels counts the values from Range Min to Range Max, both included, and values outside the range "
                       "step on at the same spacing. Dither trades the bands for noise: an ordered Bayer matrix, a "
                       "void-and-cluster blue-noise matrix, or random noise from a hash of the pixel position, the "
                       "channel, the seed and, unless Static Seed is on, the frame. The same settings give the same "
                       "output on the CPU and the GPU. Fewer than 2 levels leave the image as it is.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_POSTERIZE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Posterize.png",
        .inputs = kPosterizeInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);
    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;

    /**
     * @brief The posterized value v at pixel (x, y) in the channel whose RGBA index is bit.
     **/
    static float quantize(const Params& params,
                          float v,
                          int x,
                          int y,
                          int bit);

    /**
     * @brief The side of an ordered dither's matrix, or 0.
     **/
    static int matrixSize(int dither);

    /**
     * @brief The thresholds of an ordered dither's matrix, side·side of them, entry
     * [x mod side][y mod side] at index (x mod side)·side + (y mod side); null for the others.
     **/
    static const float* thresholds(int dither);

    // The integer hash of the random dither, shared with posterize.slang.
    static std::uint32_t hash(std::uint32_t a);

    KnobDoubleWPtr levels;
    KnobDoubleWPtr rangeMin;
    KnobDoubleWPtr rangeMax;
    KnobChoiceWPtr dither;
    KnobIntWPtr seed;
    KnobBoolWPtr staticSeed;
};

static_assert(PointOp<PosterizeOp>);

class Posterize
    : public ImageNode<PosterizeOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Posterize(node);
    }

    explicit Posterize(NodePtr node);

    virtual ~Posterize();

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    PosterizeOp::Params bakePosterize(double time,
                                      ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void posterizeRow(const PosterizeOp::Params& params,
                      const RowIO& io) const;

private:
    // Random dither without a static seed changes every frame.
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Posterize_h
