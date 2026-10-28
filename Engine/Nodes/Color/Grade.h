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

#ifndef Engine_Nodes_Color_Grade_h
#define Engine_Nodes_Color_Grade_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_GRADE "Grade"
#define PLUGIN_MAJOR_NATRON_GRADE 1

#define kGradeParamBlackPoint "blackPoint"
#define kGradeParamWhitePoint "whitePoint"
#define kGradeParamBlack "black"
#define kGradeParamWhite "white"
#define kGradeParamMultiply "multiply"
#define kGradeParamOffset "offset"
#define kGradeParamGamma "gamma"
#define kGradeParamNormalize "normalize"
#define kGradeParamReverse "reverse"
#define kGradeParamClampBlack "clampBlack"
#define kGradeParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kGradeInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Grade: maps blackPoint/whitePoint to black (Lift)/white (Gain), then applies multiply,
 * offset and gamma, per channel:
 *   A = multiply * (white - black) / (whitePoint - blackPoint), or 0 when the points coincide
 *   B = offset + black - A * blackPoint
 *   output = pow(A * input + B, 1 / gamma)
 * where a gamma below 0.01 counts as 0.01 and values <= 0 are not raised to the power, reverse
 * applies the inverse, and clampBlack/clampWhite clamp the result to [0, 1]. Both clamps default
 * off, so a default Grade is an identity. Knob names are the openfx-misc GradePlugin's; it
 * registers under that plugin's ID one major above.
 **/
struct GradeOp {
    struct Channel {
        double a;
        double b;
        double gamma;
    };

    // Doubles, as the CPU row computes in double; the GPU block takes floats from them.
    struct Params {
        Channel channels[4];
        bool reverse;
        bool clampBlack;
        bool clampWhite;
        // Every knob at its default. Not derived from channels: other settings can reduce to
        // a = 1, b = 0, but only the defaults leave -0 and NaN payloads untouched.
        bool isDefault;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GRADE,
        .label = "Grade",
        .description = "Modify the tonal spread of an image from the white and black points.\n"
                       "This node can also be used to match colors of 2 images: The darkest and lightest points of "
                       "the target image are converted to black and white using the blackpoint and whitepoint values. "
                       "These 2 values are then moved to new values using the black(for dark point) and white(for white point). "
                       "You can also apply multiply/offset/gamma for other color fixing you may need.\n"
                       "Here is the formula used:\n"
                       "A = multiply * (white - black) / (whitepoint - blackpoint)\n"
                       "B = offset + black - A * blackpoint\n"
                       "output = pow(A * input + B, 1 / gamma).\n"
                       "\n"
                       "A special use for Grade is to generate a mask image with soft edges by thresholding an input image. "
                       "Set the \"Black Point\" and \"White Point\" to values just below and just above the threshold, and "
                       "check the \"Clamp Black\" and \"Clamp White\" options. If a binary mask containing only 0 and 1 is "
                       "preferred, the Clamp node can be used instead.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_GRADE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Grade.png",
        .inputs = kGradeInputs,
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

    KnobColorWPtr blackPoint;
    KnobColorWPtr whitePoint;
    KnobColorWPtr black;
    KnobColorWPtr white;
    KnobColorWPtr multiply;
    KnobColorWPtr offset;
    KnobColorWPtr gamma;
    KnobButtonWPtr normalize;
    KnobBoolWPtr reverse;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;
};

static_assert(PointOp<GradeOp>);

class Grade
    : public ImageNode<GradeOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Grade(node);
    }

    explicit Grade(NodePtr node);

    virtual ~Grade();

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    GradeOp::Params bakeGrade(double time,
                              ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void gradeRow(const GradeOp::Params& params,
                  const RowIO& io) const;

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Sets blackPoint and whitePoint to the per-channel minimum and maximum of the source
     * at (time, view), as one undoable edit.
     **/
    void normalize(double time, ViewIdx view);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Grade_h
