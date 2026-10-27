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

#ifndef Engine_Nodes_Color_HSVTool_h
#define Engine_Nodes_Color_HSVTool_h

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

#define PLUGINID_NATRON_HSVTOOL "HSVTool"
#define PLUGIN_MAJOR_NATRON_HSVTOOL 1

#define kHSVToolParamEnableRectangle "enableRectangle"
#define kHSVToolParamBottomLeft "bottomLeft"
#define kHSVToolParamSize "size"
#define kHSVToolParamSetSrcFromRectangle "setSrcFromRectangle"
#define kHSVToolParamSrcColor "srcColor"
#define kHSVToolParamDstColor "dstColor"
#define kHSVToolParamHueRange "hueRange"
#define kHSVToolParamHueRotation "hueRotation"
#define kHSVToolParamHueRotationGain "hueRotationGain"
#define kHSVToolParamHueRangeRolloff "hueRangeRolloff"
#define kHSVToolParamSaturationRange "saturationRange"
#define kHSVToolParamSaturationAdjustment "saturationAdjustment"
#define kHSVToolParamSaturationAdjustmentGain "saturationAdjustmentGain"
#define kHSVToolParamSaturationRangeRolloff "saturationRangeRolloff"
#define kHSVToolParamBrightnessRange "brightnessRange"
#define kHSVToolParamBrightnessRangeOpen "brightnessRangeOpen"
#define kHSVToolParamBrightnessAdjustment "brightnessAdjustment"
#define kHSVToolParamBrightnessAdjustmentGain "brightnessAdjustmentGain"
#define kHSVToolParamBrightnessRangeRolloff "brightnessRangeRolloff"
#define kHSVToolParamClampBlack "clampBlack"
#define kHSVToolParamClampWhite "clampWhite"
#define kHSVToolParamOutputAlpha "outputAlpha"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kHSVToolInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Selects colours by ranges of hue, saturation and brightness (HSV value) with linear
 * rolloffs, and rotates the hue and adjusts the saturation and brightness of the selection.
 * Saturation is chroma over the largest of R, G and B, 0 when that is not positive, and neither
 * it nor brightness is clamped, so HDR and negative values are selected and adjusted too.
 * With A processed, alpha can be set to the selection coefficients instead (outputAlpha).
 **/
struct HSVToolOp {
    enum OutputAlphaEnum {
        eOutputAlphaSource = 0,
        eOutputAlphaHue,
        eOutputAlphaSaturation,
        eOutputAlphaBrightness,
        eOutputAlphaHueSaturation,
        eOutputAlphaHueBrightness,
        eOutputAlphaSaturationBrightness,
        eOutputAlphaAll,
        eOutputAlphaCount
    };

    /**
     * @brief The ranges in turns and HSV units. A range's low/high rolloff ends are where its
     * ramps reach 0; the gains are stored minus 1.
     **/
    struct Params {
        bool allHues = true;
        float hueLow = 0.f;
        float hueHigh = 1.f;
        float hueLowRolloff = 0.f;
        float hueHighRolloff = 1.f;
        float hueMean = 0.f;
        float hueRotation = 0.f;
        float hueGain = 0.f;
        float satLow = 0.f;
        float satHigh = 1.f;
        float satLowRolloff = 0.f;
        float satHighRolloff = 1.f;
        float satRolloff = 0.f;
        float satAdjust = 0.f;
        float satGain = 0.f;
        float satMid = 0.5f;
        float valLow = 0.f;
        float valHigh = 1.f;
        float valLowRolloff = 0.f;
        float valHighRolloff = 1.f;
        float valRolloff = 0.f;
        float valAdjust = 0.f;
        float valGain = 0.f;
        float valMid = 0.5f;
        bool clampBlack = false;
        bool clampWhite = false;
        int outputAlpha = eOutputAlphaHue;
        // Whether any rotation, adjustment or gain moves a selected colour.
        bool changesColour = false;
        // Whether alpha is processed and set from the coefficients.
        bool writesAlpha = false;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_HSVTOOL,
        .label = "HSVTool",
        .description = "Adjust hue, saturation and brightness, or perform color replacement.\n"
                       "\n"
                       "Color replacement:\n"
                       "Set the srcColor and dstColor parameters. The range of the replacement is determined by the "
                       "three groups of parameters: Hue, Saturation and Brightness.\n"
                       "\n"
                       "Color adjust:\n"
                       "Use the Rotation of the Hue parameter and the Adjustment of the Saturation and Lightness. "
                       "The ranges and falloff parameters allow for more complex adjustments.\n"
                       "\n"
                       "Hue keyer:\n"
                       "Process the A channel, set the outputAlpha parameter (the last one) to min(all), and use a "
                       "viewer to display the Alpha channel. First, set the Range parameter of the Hue parameter set "
                       "and then work down the other Ranges parameters, tuning with the range Falloff and "
                       "Adjustment parameters.\n"
                       "\n"
                       "Saturation is chroma divided by the largest of R, G and B, and brightness is that largest "
                       "value; neither is clamped, so HDR colours are selected and adjusted as well.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_HSVTOOL,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/HSVTool.png",
        .inputs = kHSVToolInputs,
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
     * @brief The colour (rgb) and alpha (rgba[3]) of one pixel; rgba[3] is the source alpha
     * when outputAlpha is Source.
     **/
    static void pixel(const Params& params,
                      const float* rgba,
                      float* out);

    KnobBoolWPtr enableRectangle;
    KnobDoubleWPtr bottomLeft;
    KnobDoubleWPtr size;
    KnobButtonWPtr setSrcFromRectangle;
    KnobColorWPtr srcColor;
    KnobColorWPtr dstColor;
    KnobDoubleWPtr hueRange;
    KnobDoubleWPtr hueRotation;
    KnobDoubleWPtr hueRotationGain;
    KnobDoubleWPtr hueRangeRolloff;
    KnobDoubleWPtr saturationRange;
    KnobDoubleWPtr saturationAdjustment;
    KnobDoubleWPtr saturationAdjustmentGain;
    KnobDoubleWPtr saturationRangeRolloff;
    KnobDoubleWPtr brightnessRange;
    KnobBoolWPtr brightnessRangeOpen;
    KnobDoubleWPtr brightnessAdjustment;
    KnobDoubleWPtr brightnessAdjustmentGain;
    KnobDoubleWPtr brightnessRangeRolloff;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;
    KnobChoiceWPtr outputAlpha;
    // The node, whose Channels knob bake() reads to tell whether alpha is processed.
    KnobHolder* holder = nullptr;
};

static_assert(PointOp<HSVToolOp>);

class HSVTool
    : public ImageNode<HSVToolOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new HSVTool(node);
    }

    explicit HSVTool(NodePtr node);

    virtual ~HSVTool();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    HSVToolOp::Params bakeHSVTool(double time,
                                  ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void hsvToolRow(const HSVToolOp::Params& params,
                    const RowIO& io) const;

    /**
     * @brief Sets srcColor, the ranges, the rolloffs and the adjustments from the source pixels
     * under the analysis rectangle, as the Set Src from Rectangle button does. False when there
     * is no float source image there.
     **/
    bool setSrcFromRectangleAt(double time,
                               ViewIdx view);

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    void refreshRectangleVisibility();

    void setSrcFromColor(double time);

    void setDstFromColor(double time);

    bool _syncing;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_HSVTool_h
