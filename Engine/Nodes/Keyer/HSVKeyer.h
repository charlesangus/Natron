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

#ifndef Engine_Nodes_Keyer_HSVKeyer_h
#define Engine_Nodes_Keyer_HSVKeyer_h

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

#define PLUGINID_NATRON_HSVKEYER "HSVKeyer"
#define PLUGIN_MAJOR_NATRON_HSVKEYER 1

#define kHSVKeyerParamSourceColor "sourceColor"
#define kHSVKeyerParamColorModel "colorModel"
#define kHSVKeyerParamRecentre "recentre"
#define kHSVKeyerParamHueRange "hueRange"
#define kHSVKeyerParamSaturationRange "saturationRange"
#define kHSVKeyerParamLumaRange "lumaRange"
#define kHSVKeyerParamUseHue "useHue"
#define kHSVKeyerParamUseSaturation "useSaturation"
#define kHSVKeyerParamUseLuma "useLuma"
#define kHSVKeyerParamHueSmooth "hueSmooth"
#define kHSVKeyerParamSaturationSmooth "saturationSmooth"
#define kHSVKeyerParamLumaSmooth "lumaSmooth"
#define kHSVKeyerParamInvert "invert"
#define kHSVKeyerParamOutputMode "outputMode"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kHSVKeyerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Keys by hue, saturation and luma in HSV, HSL or OkLCh: the key is the product of a
 * trapezoid over each enabled component, 1 when none is, inverted on request. Each range knob
 * holds a centre and four offsets (KnobDouble::RangeWidgetDimensionEnum), the outer ones measured
 * from the inner edges; a range's smooth toggle eases its falloffs with HueCurves::smoothStep().
 * The hue trapezoid is in turns and wraps around its centre, and its value is weighted by
 * ColorMath::keyHueFade() of the chroma, so a grey never matches a hue range. Alpha is the key;
 * RGB passes, is multiplied by the key or is replaced by it, as the output mode says. The output
 * is RGBA whatever the source.
 **/
struct HSVKeyerOp {
    enum OutputModeEnum {
        eOutputModeAlpha = 0,
        eOutputModePremultiplied,
        eOutputModeMatte
    };

    /**
     * @brief The ranges resolved from their centres and offsets. The hue trapezoid starts at
     * hueLow and reaches 1 hueRise later, stays 1 until hueFull and falls to 0 at hueFall, all
     * going up the hue circle from hueLow. The saturation and luma trapezoids' points are the
     * outer low, inner low, inner high and outer high points, in order.
     **/
    struct Params {
        ColorMath::KeyModelEnum model = ColorMath::eKeyModelHSV;
        OutputModeEnum output = eOutputModeAlpha;
        bool useHue = true;
        bool useSat = true;
        bool useLuma = true;
        bool invert = false;
        bool smoothHue = false;
        bool smoothSat = false;
        bool smoothLuma = false;
        float hueLow = 0.f;
        float hueRise = 0.f;
        float hueFull = 0.f;
        float hueFall = 0.f;
        float sat[4] = { 0.f, 0.f, 0.f, 0.f };
        float luma[4] = { 0.f, 0.f, 0.f, 0.f };
        ColorMath::OkLabBasis basis = {};
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_HSVKEYER,
        .label = "HSVKeyer",
        .description = "Compute a key from the hue, saturation and luma of the source.\n"
                       "\n"
                       "Each component has a range: a centre and four offsets. The key is 1 from the centre minus "
                       "the inner low offset to the centre plus the inner high offset, and falls to 0 over the outer "
                       "low offset below that plateau and over the outer high offset above it, linearly or, with the "
                       "range's smooth toggle on, as a smoothstep. An outer offset of 0 is a hard edge. The key is "
                       "the product of the trapezoids of the enabled components. The hue is in "
                       "turns from red at 0 and its trapezoid wraps around its centre; hue counts for less as the "
                       "chroma nears 0, so greys never match a hue range.\n"
                       "\n"
                       "The components are those of the colour model: HSV (saturation (max - min) / max, value max), "
                       "HSL (lightness (max + min) / 2) or OkLCh (OkLab's lightness, chroma and hue, from the "
                       "working space adapted to D65). Setting the source colour, picked from the viewer or typed, "
                       "or changing the model, sets each range's centre to the source colour's component, keeping "
                       "its offsets.",
        .grouping = PLUGIN_GROUP_KEYER,
        .majorVersion = PLUGIN_MAJOR_NATRON_HSVKEYER,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/HSVKeyer.png",
        .inputs = kHSVKeyerInputs,
    };
    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
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

    // The key of colour rgb.
    static float key(const Params& params,
                     const float* rgb);

    KnobHolder* holder = nullptr;
    KnobColorWPtr sourceColor;
    KnobChoiceWPtr colorModel;
    KnobButtonWPtr recentre;
    KnobDoubleWPtr hueRange;
    KnobDoubleWPtr satRange;
    KnobDoubleWPtr lumaRange;
    KnobBoolWPtr useHue;
    KnobBoolWPtr useSat;
    KnobBoolWPtr useLuma;
    KnobBoolWPtr hueSmooth;
    KnobBoolWPtr satSmooth;
    KnobBoolWPtr lumaSmooth;
    KnobBoolWPtr invert;
    KnobChoiceWPtr outputMode;
};

static_assert(PointOp<HSVKeyerOp>);

class HSVKeyer
    : public ImageNode<HSVKeyerOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new HSVKeyer(node);
    }

    explicit HSVKeyer(NodePtr node);

    virtual ~HSVKeyer();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    HSVKeyerOp::Params bakeHSVKeyer(double time,
                                    ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void hsvKeyerRow(const HSVKeyerOp::Params& params,
                     const RowIO& io) const;

    /**
     * @brief Sets the three ranges' centres to the source colour's components in the current
     * model at time, keeping their offsets, as setting the source colour, changing the model or
     * the Recentre button does. The hue centre stays where it is for a source with no chroma,
     * which has no hue.
     **/
    void recentreOnSource(double time);

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    // Recentres on the source colour once its change bracket closes.
    virtual void evaluate(bool isSignificant, bool refreshMetadatas) OVERRIDE FINAL;

    // The output is RGBA whatever the source: the key is its alpha.
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Set by knobChanged() for a source, model or Recentre edit, consumed by evaluate().
    bool _recentrePending;
    double _recentreTime;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_HSVKeyer_h
