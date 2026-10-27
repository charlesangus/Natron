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

#ifndef Engine_Nodes_Color_HueCorrect_h
#define Engine_Nodes_Color_HueCorrect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/CurveSnapshot.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_HUECORRECT "HueCorrect"
#define PLUGIN_MAJOR_NATRON_HUECORRECT 1

#define kHueCorrectParamHue "hue"
#define kHueCorrectParamHueVsHue "huevshue"
#define kHueCorrectParamClampBlack "clampBlack"
#define kHueCorrectParamClampWhite "clampWhite"
#define kHueCorrectParamMixLuminanceEnable "mixLuminanceEnable"
#define kHueCorrectParamMixLuminance "mixLuminance"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kHueCorrectInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Hue-dependent adjustments from ten periodic curves over the hue, red at curve position
 * 1: a hue shift, the red/green/blue suppressions, the luminance and red/green/blue gains above
 * a saturation threshold, then a saturation gain towards the luminance, and optionally the
 * source luminance mixed back in. Each curve is read from a table sampled from it (HueCurves).
 * Saturation is chroma over the largest of R, G and B, and nothing is clamped unless asked, so
 * HDR values are scaled rather than clipped.
 **/
struct HueCorrectOp {
    enum CurveEnum {
        eCurveHue = 0,
        eCurveSat,
        eCurveLum,
        eCurveRed,
        eCurveGreen,
        eCurveBlue,
        eCurveRSup,
        eCurveGSup,
        eCurveBSup,
        eCurveSatThrsh,
        eCurveCount
    };

    enum LumaKindEnum {
        eLumaKindWeighted = 0,
        eLumaKindAverage,
        eLumaKindMax
    };

    // The tables sampled from the curves, shared by every Params baked from the same curves.
    struct Tables {
        // eCurveCount tables of HueCurves::kLutSize values, one after the other, each at least 0.
        std::vector<float> lut;
        // Whether every table holds its curve's default (1, or 0 for sat_thrsh) throughout.
        bool atDefaults = true;
    };

    struct Params {
        std::shared_ptr<const Tables> tables;
        // How luminanceMath computes luminance: a weighted sum, the average or the largest of
        // R, G and B.
        LumaKindEnum lumaKind = eLumaKindWeighted;
        // The weights of a weighted sum.
        float lumaWeights[3] = { 0.f, 0.f, 0.f };
        // The luminance mix, 0 when mixLuminanceEnable is off.
        float mixLuminance = 0.f;
        bool clampBlack = false;
        bool clampWhite = false;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_HUECORRECT,
        .label = "HueCorrect",
        .description = "Apply hue-dependent color adjustments using lookup curves.\n"
                       "Hue and saturation are computed from the the source RGB values. Depending on the hue value, the various adjustment values are computed, and then applied:\n"
                       "hue: hue shift.\n"
                       "sat: saturation gain. This modification is applied last.\n"
                       "lum: luminance gain\n"
                       "red: red gain\n"
                       "green: green gain\n"
                       "blue: blue gain\n"
                       "r_sup: red suppression. If r > min(g,b),  r = min(g,b) + r_sup * (r-min(g,b))\n"
                       "g_sup: green suppression\n"
                       "b_sup: blue suppression\n"
                       "sat_thrsh: if source saturation is below this value, do not apply the lum, red, green, blue gains. Above this value, apply gain progressively.\n"
                       "\n"
                       "The 'Luminance Mix' parameter may be used to restore partially or fully the original luminance (luminance is computed using the 'Luminance Math' parameter).\n"
                       "\n"
                       "Saturation is chroma divided by the largest of R, G and B, and no value is clamped unless "
                       "Clamp Black or Clamp White is on, so HDR colours are scaled rather than clipped.\n"
                       "\n"
                       "See also: https://web.archive.org/web/20220627022150/http://www.opticalenquiry.com/nuke/index.php?title=HueCorrect",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_HUECORRECT,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/HueCorrect.png",
        .inputs = kHueCorrectInputs,
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

    // The colour (rgb[0..2]) of one pixel, in place.
    static void pixel(const Params& params,
                      float* rgb);

    // The luminance of rgb as Params compute it, in float.
    static float luminance(const Params& params,
                           const float* rgb);

    KnobParametricWPtr hue;
    KnobBoolWPtr hueVsHue;
    KnobChoiceWPtr luminanceMath;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;
    KnobBoolWPtr mixLuminanceEnable;
    KnobDoubleWPtr mixLuminance;

private:
    CurveTablesCache<Tables> _tables;
};

static_assert(PointOp<HueCorrectOp>);

class HueCorrect
    : public ImageNode<HueCorrectOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new HueCorrect(node);
    }

    explicit HueCorrect(NodePtr node);

    virtual ~HueCorrect();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    HueCorrectOp::Params bakeHueCorrect(double time,
                                        ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void hueCorrectRow(const HueCorrectOp::Params& params,
                       const RowIO& io) const;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Enables mixLuminance while mixLuminanceEnable is on.
    void refreshMixLuminance();

    // Paints the hue under the curves, or the hue-vs-hue guide while huevshue is on.
    void installBackgroundPainter();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_HueCorrect_h
