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

#ifndef Engine_Nodes_Color_ColorMatrix_h
#define Engine_Nodes_Color_ColorMatrix_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_COLORMATRIX "ColorMatrix"
#define PLUGIN_MAJOR_NATRON_COLORMATRIX 1

#define kColorMatrixParamOutputRed "outputRed"
#define kColorMatrixParamOutputGreen "outputGreen"
#define kColorMatrixParamOutputBlue "outputBlue"
#define kColorMatrixParamOutputAlpha "outputAlpha"
#define kColorMatrixParamClampBlack "clampBlack"
#define kColorMatrixParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kColorMatrixInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Each processed output channel is a weighted sum of the input's R, G, B and A: output
 * row c of the matrix (outputRed, outputGreen, outputBlue, outputAlpha) dotted with the input
 * pixel. A pixel without alpha reads alpha as 1, and one without colour reads colour as 0. The
 * sum is in float, in the order r, g, b, a, with no fused multiply-add, as the GPU kernel
 * computes it.
 **/
struct ColorMatrixOp {
    struct Params {
        // Rows R, G, B, A over columns r, g, b, a.
        float m[4][4];
        bool clampBlack;
        bool clampWhite;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORMATRIX,
        .label = "ColorMatrix",
        .description = "Multiply the RGBA channels by an arbitrary 4x4 matrix.\n"
                       "Each processed output channel is the dot product of its row (Output Red, Output Green, "
                       "Output Blue or Output Alpha) with the input's R, G, B and A. An input without alpha reads "
                       "alpha as 1.",
        .grouping = PLUGIN_GROUP_COLOR "/Math",
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORMATRIX,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/ColorMatrix.png",
        .inputs = kColorMatrixInputs,
    };
    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);
    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;

    KnobColorWPtr outputs[4];
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;
};

static_assert(PointOp<ColorMatrixOp>);

class ColorMatrix
    : public ImageNode<ColorMatrixOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorMatrix(node);
    }

    explicit ColorMatrix(NodePtr node);

    virtual ~ColorMatrix();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ColorMatrixOp::Params bakeColorMatrix(double time,
                                          ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void colorMatrixRow(const ColorMatrixOp::Params& params,
                        const RowIO& io) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorMatrix_h
