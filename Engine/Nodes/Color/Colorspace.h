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

#ifndef Engine_Nodes_Color_Colorspace_h
#define Engine_Nodes_Color_Colorspace_h

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

#define PLUGINID_NATRON_COLORSPACE "Colorspace"
#define PLUGIN_MAJOR_NATRON_COLORSPACE 1

#define kColorspaceParamIn "colorspace_in"
#define kColorspaceParamOut "colorspace_out"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kColorspaceInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Converts the RGB of each pixel from the colorspace_in model to the colorspace_out model
 * (ColorMath::ColorspaceModelEnum), through linear RGB, or through XYZ between the XYZ models.
 * RGB is the project's working space, whose matrix to XYZ and white the XYZ, xyY and Lab models
 * use; alpha is left as it is.
 **/
struct ColorspaceOp {
    struct Params {
        ColorMath::ColorspaceSide in;
        ColorMath::ColorspaceSide out;
        ColorMath::ColorspaceColorimetry colorimetry;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORSPACE,
        .label = "Colorspace",
        .description = "Convert the RGB channels from one colour model to another: linear RGB, HSV, HSL, HSI, "
                       "Y'PbPr, Y'CbCr and YUV (Rec. 601, 709 and 2020 coefficients), CIE XYZ, xyY and L*a*b*.\n"
                       "RGB is the project's working space. XYZ, xyY and L*a*b* use its primaries and white, "
                       "the white being RGB (1, 1, 1). The luma/chroma models are applied to the linear values, "
                       "with no transfer function. Hue is in turns, from 0 to 1, and L* goes from 0 to 100.\n"
                       "To convert between OpenColorIO colourspaces, use OCIOColorSpace.",
        .grouping = PLUGIN_GROUP_COLOR "/Transform",
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORSPACE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Colorspace.png",
        .inputs = kColorspaceInputs,
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

    KnobChoiceWPtr in;
    KnobChoiceWPtr out;
    // The node, whose project's working space bake() reads.
    KnobHolder* holder = nullptr;
};

static_assert(PointOp<ColorspaceOp>);

class Colorspace
    : public ImageNode<ColorspaceOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Colorspace(node);
    }

    explicit Colorspace(NodePtr node);

    virtual ~Colorspace();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ColorspaceOp::Params bakeColorspace(double time,
                                        ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void colorspaceRow(const ColorspaceOp::Params& params,
                       const RowIO& io) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Colorspace_h
