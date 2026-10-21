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

#ifndef Engine_Nodes_Color_Saturation_h
#define Engine_Nodes_Color_Saturation_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_SATURATION "net.sf.openfx.SaturationPlugin"
#define PLUGIN_MAJOR_NATRON_SATURATION 3

#define kSaturationParamSaturation "saturation"
#define kSaturationParamClampBlack "clampBlack"
#define kSaturationParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kSaturationInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Saturation: moves each processed colour channel away from, or towards, the pixel's
 * luminance l: c' = (1 - saturation) * l + saturation * c, with clampBlack/clampWhite clamping
 * every processed channel, alpha included, to [0, 1]. RGB and RGBA only. Knob names, defaults
 * and maths are the openfx-misc SaturationPlugin's, except that the ACES AP1 luminance uses the
 * AP1 coefficients and clampBlack defaults off so that a default node is an identity; it
 * registers under that plugin's ID one major above.
 **/
struct SaturationOp {
    // The saturation is a double, as the CPU row computes in double; the GPU block takes a float.
    struct Params {
        double saturation;
        ColorMath::LuminanceMathEnum luminanceMath;
        bool clampBlack;
        bool clampWhite;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_SATURATION,
        .label = "Saturation",
        .description = "Modify the color saturation of an image.\n"
                       "Each processed channel is moved away from (saturation above 1) or towards "
                       "(saturation below 1) the pixel's luminance. A saturation of 0 produces grayscale.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_SATURATION,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.SaturationPlugin.png",
        .inputs = kSaturationInputs,
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

    KnobDoubleWPtr saturation;
    KnobChoiceWPtr luminanceMath;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;
};

static_assert(PointOp<SaturationOp>);

class Saturation
    : public ImageNode<SaturationOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Saturation(node);
    }

    explicit Saturation(NodePtr node);

    virtual ~Saturation();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    SaturationOp::Params bakeSaturation(double time,
                                        ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void saturationRow(const SaturationOp::Params& params,
                       const RowIO& io) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Saturation_h
