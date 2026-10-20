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

#ifndef Engine_Nodes_Color_ColorCorrect_h
#define Engine_Nodes_Color_ColorCorrect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include <memory>

#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/CurveSnapshot.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_COLORCORRECT "net.sf.openfx.ColorCorrectPlugin"
#define PLUGIN_MAJOR_NATRON_COLORCORRECT 3

#define kColorCorrectGroupMaster "Master"
#define kColorCorrectGroupShadows "Shadows"
#define kColorCorrectGroupMidtones "Midtones"
#define kColorCorrectGroupHighlights "Highlights"
#define kColorCorrectParamEnable "Enable"
#define kColorCorrectParamSaturation "Saturation"
#define kColorCorrectParamContrast "Contrast"
#define kColorCorrectParamGamma "Gamma"
#define kColorCorrectParamGain "Gain"
#define kColorCorrectParamOffset "Offset"
#define kColorCorrectParamToneRanges "toneRanges"
#define kColorCorrectParamClampBlack "clampBlack"
#define kColorCorrectParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kColorCorrectInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief ColorCorrect: saturation, contrast, gamma, gain and offset, once for the shadows,
 * midtones and highlights blended by the tone-range curves of the luminance, then once for the
 * whole image (Master). Each group applies, per channel:
 *   saturation: c = (1 - s) * luminance + s * c, on R, G and B only
 *   contrast:   c = pow(c / 0.18, contrast) * 0.18, for c > 0
 *   gamma:      c = pow(c, 1 / gamma), for c > 0
 *   gain, then offset
 * and clampBlack/clampWhite clamp the result. Knob names, defaults and maths are the
 * openfx-misc ColorCorrectPlugin's, except that clampBlack defaults off so that a default node is
 * an identity; it registers under that plugin's ID one major above.
 **/
struct ColorCorrectOp {
    enum GroupEnum {
        eGroupMaster = 0,
        eGroupShadows,
        eGroupMidtones,
        eGroupHighlights,
        eGroupCount
    };

    struct Group {
        double saturation[4];
        double contrast[4];
        double gamma[4];
        double invGamma[4];
        double gain[4];
        double offset[4];
        // Whether the group changes anything; an identity group is not run, so that it leaves
        // the pixel bit-for-bit as it is.
        bool active;
    };

    // The tone-range curves and the tables sampled from them, shared by every Params baked from
    // the same curves.
    struct Tones;

    struct Params {
        Group groups[eGroupCount];
        ColorMath::LuminanceMathEnum luminanceMath;
        bool clampBlack;
        bool clampWhite;
        std::shared_ptr<const Tones> tones;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORCORRECT,
        .label = "ColorCorrect",
        .description = "Adjusts the saturation, contrast, gamma, gain and offset of an image.\n"
                       "The ranges of the shadows, midtones and highlights are controlled by the curves "
                       "in the \"Ranges\" tab.\n"
                       "The Contrast adjustment works using the formula: Output = (Input/0.18)^Contrast*0.18.\n"
                       "\n"
                       "See also:\n"
                       "- http://opticalenquiry.com/nuke/index.php?title=ColorCorrect\n"
                       "- https://compositormathematic.wordpress.com/2013/07/06/gamma-contrast/",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORCORRECT,
        .minorVersion = 0,
        .inputs = kColorCorrectInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    struct GroupKnobs {
        KnobBoolWPtr enable;
        KnobColorWPtr saturation;
        KnobColorWPtr contrast;
        KnobColorWPtr gamma;
        KnobColorWPtr gain;
        KnobColorWPtr offset;
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    GroupKnobs groups[eGroupCount];
    KnobParametricWPtr toneRanges;
    KnobChoiceWPtr luminanceMath;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;

private:
    std::shared_ptr<const Tones> bakeTones() const;

    CurveTablesCache<Tones> _tones;
};

static_assert(PointOp<ColorCorrectOp>);

class ColorCorrect
    : public ImageNode<ColorCorrectOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorCorrect(node);
    }

    explicit ColorCorrect(NodePtr node);

    virtual ~ColorCorrect();

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ColorCorrectOp::Params bakeColorCorrect(double time,
                                            ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void colorCorrectRow(const ColorCorrectOp::Params& params,
                         const RowIO& io) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorCorrect_h
