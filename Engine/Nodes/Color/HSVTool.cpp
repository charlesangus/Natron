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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "HSVTool.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/HueCurves.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Project.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

// Pixels darker or greyer than this carry too noisy a hue for Set Src from Rectangle.
const double kMinSaturation = 0.1;
const double kMinValue = 0.1;
// The rolloff Set Src from Rectangle sets, as a fraction of the range it measured.
const double kRectangleRolloff = 0.5;

#ifdef NATRON_HAVE_GPU
// The parameter block of HsvToolOp in hue.slang, which has no padding between members.
struct HSVToolGpuParams {
    uint32_t flags;
    uint32_t outputAlpha;
    float hueLow, hueHigh, hueLowRolloff, hueHighRolloff, hueMean, hueRotation, hueGain;
    float satLow, satHigh, satLowRolloff, satHighRolloff, satRolloff, satAdjust, satGain, satMid;
    float valLow, valHigh, valLowRolloff, valHighRolloff, valRolloff, valAdjust, valGain, valMid;
};

static_assert(sizeof(HSVToolGpuParams) == 25 * 4, "must match HsvToolOp in hue.slang");

const uint32_t kGpuFlagAllHues = 1u;
const uint32_t kGpuFlagChangesColour = 2u;
const uint32_t kGpuFlagClampBlack = 4u;
const uint32_t kGpuFlagClampWhite = 8u;
#endif

// a in degrees wrapped into 0 to 360.
double
normalizeAngle(double a)
{
    const double c = std::floor(a / 360.);

    return a - c * 360.;
}

// a in degrees wrapped into -180 to 180.
double
normalizeAngleSigned(double a)
{
    return normalizeAngle(a + 180.) - 180.;
}

double
ipow10(int decimals)
{
    double p = 1.;

    for (int i = 0; i < decimals; ++i) {
        p *= 10.;
    }

    return p;
}

double
ffloor(double v,
       int decimals)
{
    const double p = ipow10(decimals);

    return std::floor(v * p) / p;
}

double
fround(double v,
       int decimals)
{
    const double p = ipow10(decimals);

    return std::floor(v * p + 0.5) / p;
}

double
fceil(double v,
      int decimals)
{
    const double p = ipow10(decimals);

    return std::ceil(v * p) / p;
}

// The hue (in degrees), saturation and brightness of a colour, as the node selects them.
struct HSVColor {
    double h = 0.;
    double s = 0.;
    double v = 0.;
};

HSVColor
hsvOf(float r,
      float g,
      float b)
{
    const float maxv = (std::max)(r, (std::max)(g, b));
    const float minv = (std::min)(r, (std::min)(g, b));
    const float delta = maxv - minv;
    HSVColor c;

    c.h = (double)ColorMath::colorspaceHexHue(r, g, b, maxv, delta) * 360.;
    c.s = (maxv > 0.f) ? (double)(delta / maxv) : 0.;
    c.v = maxv;

    return c;
}

// The selection coefficient of x for the range lo to hi with linear ramps of width r out to
// loR and hiR.
inline float
rangeCoeff(float x,
           float lo,
           float hi,
           float loR,
           float hiR,
           float r)
{
    if ((lo <= x) && (x <= hi)) {
        return 1.f;
    }
    if ((loR <= x) && (x <= lo)) {
        const float n = x - loR;

        return n / r;
    }
    if ((hi <= x) && (x <= hiR)) {
        const float n = hiR - x;

        return n / r;
    }

    return 0.f;
}

KnobDoublePtr
addDouble(KnobHolder* holder,
          const KnobGroupPtr& group,
          const char* name,
          const QString& label,
          const QString& hint,
          int dimensions,
          double defaultValue,
          double minimum,
          double maximum,
          double displayMinimum,
          double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString(), dimensions);

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    for (int d = 0; d < dimensions; ++d) {
        knob->setMinimum(minimum, d);
        knob->setMaximum(maximum, d);
        knob->setDisplayMinimum(displayMinimum, d);
        knob->setDisplayMaximum(displayMaximum, d);
        knob->setDefaultValue(defaultValue, d);
    }
    group->addKnob(knob);

    return knob;
}

KnobGroupPtr
addGroup(KnobHolder* holder,
         const KnobPagePtr& page,
         const char* name,
         const QString& label,
         const QString& hint)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(holder, label.toStdString());

    group->setName(name);
    group->setHintToolTip(hint.toStdString());
    group->setDefaultValue(true);
    page->addKnob(group);

    return group;
}

// Whether the node's Channels knob processes the alpha of an RGBA colour plane.
bool
processesAlpha(KnobHolder* holder)
{
    KnobChannelSetPtr channels = holder ? std::dynamic_pointer_cast<KnobChannelSet>(holder->getKnobByName(kNodeParamChannelSet)) : KnobChannelSetPtr();

    if (!channels) {
        return false;
    }
    const std::vector<ResolvedLayer> resolved = channels->resolve(std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()));
    for (const ResolvedLayer& layer : resolved) {
        if (layer.desc.isColorLayer() && layer.channels[3]) {
            return true;
        }
    }

    return false;
}

} // anonymous namespace

void
HSVToolOp::createKnobs(KnobHolder* knobHolder,
                       const KnobPagePtr& page)
{
    holder = knobHolder;

    KnobGroupPtr replacement = addGroup(knobHolder, page, "colorReplacement", EffectInstance::tr("Color Replacement"),
                                        EffectInstance::tr("Easily replace a given color by another color by setting srcColor and dstColor. Set Src Color first, then Dst Color."));
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(knobHolder, EffectInstance::tr("Src Analysis Rectangle").toStdString());
        knob->setName(kHSVToolParamEnableRectangle);
        knob->setHintToolTip(EffectInstance::tr("Enable the rectangle interact for analysis of Src and Dst colors and ranges.").toStdString());
        knob->setDefaultValue(false);
        knob->setAnimationEnabled(false);
        knob->setEvaluateOnChange(false);
        replacement->addKnob(knob);
        enableRectangle = knob;
    }
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(knobHolder, EffectInstance::tr("Bottom Left").toStdString(), 2);
        knob->setName(kHSVToolParamBottomLeft);
        knob->setHintToolTip(EffectInstance::tr("Coordinates of the bottom left corner of the analysis rectangle.").toStdString());
        knob->setSpatial(true);
        knob->disableSlider();
        knob->setDefaultValuesAreNormalized(true);
        for (int d = 0; d < 2; ++d) {
            knob->setMinimum(-DBL_MAX, d);
            knob->setMaximum(DBL_MAX, d);
            knob->setDisplayMinimum(0., d);
            knob->setDisplayMaximum(10000., d);
            knob->setIncrement(1., d);
            knob->setDecimals(0, d);
            knob->setDefaultValue(0.25, d);
        }
        knob->setEvaluateOnChange(false);
        knob->setSecretByDefault(true);
        knob->setAddNewLine(false);
        replacement->addKnob(knob);
        bottomLeft = knob;
    }
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(knobHolder, EffectInstance::tr("Size").toStdString(), 2);
        knob->setName(kHSVToolParamSize);
        knob->setHintToolTip(EffectInstance::tr("Width and height of the analysis rectangle.").toStdString());
        knob->setSpatial(true);
        knob->disableSlider();
        knob->setDefaultValuesAreNormalized(true);
        knob->setDimensionName(0, "w");
        knob->setDimensionName(1, "h");
        for (int d = 0; d < 2; ++d) {
            knob->setMinimum(0., d);
            knob->setMaximum(DBL_MAX, d);
            knob->setDisplayMinimum(0., d);
            knob->setDisplayMaximum(10000., d);
            knob->setIncrement(1., d);
            knob->setDecimals(0, d);
            knob->setDefaultValue(0.5, d);
        }
        knob->setEvaluateOnChange(false);
        knob->setSecretByDefault(true);
        replacement->addKnob(knob);
        size = knob;
    }
    {
        KnobButtonPtr knob = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Set Src from Rectangle").toStdString());
        knob->setName(kHSVToolParamSetSrcFromRectangle);
        knob->setHintToolTip(EffectInstance::tr("Set the Src color and ranges and the adjustments from the colors of the source image within the selection rectangle and the Dst Color.").toStdString());
        knob->setEvaluateOnChange(false);
        knob->setSecretByDefault(true);
        replacement->addKnob(knob);
        setSrcFromRectangle = knob;
    }
    for (int i = 0; i < 2; ++i) {
        const bool src = (i == 0);
        KnobColorPtr knob = AppManager::createKnob<KnobColor>(knobHolder, (src ? EffectInstance::tr("Src Color") : EffectInstance::tr("Dst Color")).toStdString(), 3);
        knob->setName(src ? kHSVToolParamSrcColor : kHSVToolParamDstColor);
        knob->setHintToolTip(src ? EffectInstance::tr("Source color for replacement. Changing this parameter sets the hue, saturation and brightness ranges for this color, and sets the fallofs to default values.").toStdString()
                                 : EffectInstance::tr("Destination color for replacement. Changing this parameter sets the hue rotation, and saturation and brightness adjustments. Should be set after Src Color.").toStdString());
        for (int d = 0; d < 3; ++d) {
            knob->setDefaultValue(0., d);
            knob->setMinimum(-DBL_MAX, d);
            knob->setMaximum(DBL_MAX, d);
            knob->setDisplayMinimum(0., d);
            knob->setDisplayMaximum(1., d);
        }
        knob->setEvaluateOnChange(false);
        replacement->addKnob(knob);
        if (src) {
            srcColor = knob;
        } else {
            dstColor = knob;
        }
    }

    KnobGroupPtr hue = addGroup(knobHolder, page, "hue", EffectInstance::tr("Hue"), EffectInstance::tr("Hue modification settings."));
    {
        KnobDoublePtr knob = addDouble(knobHolder, hue, kHSVToolParamHueRange, EffectInstance::tr("Hue Range"),
                                       EffectInstance::tr("Range of color hues that are modified (in degrees). Red is 0, green is 120, blue is 240. The affected hue range is the smallest interval. For example, if the range is (12, 348), then the selected range is red plus or minus 12 degrees. Exception: if the range width is exactly 360, then all hues are modified."),
                                       2, 0., -DBL_MAX, DBL_MAX, 0., 360.);
        knob->setDefaultValue(360., 1);
        knob->setDimensionName(0, "min");
        knob->setDimensionName(1, "max");
        hueRange = knob;
    }
    hueRotation = addDouble(knobHolder, hue, kHSVToolParamHueRotation, EffectInstance::tr("Hue Rotation"),
                            EffectInstance::tr("Rotation of color hues (in degrees) within the range."),
                            1, 0., -DBL_MAX, DBL_MAX, -180., 180.);
    hueRotationGain = addDouble(knobHolder, hue, kHSVToolParamHueRotationGain, EffectInstance::tr("Hue Rotation Gain"),
                                EffectInstance::tr("Factor to be applied to the rotation of color hues (in degrees) within the range. A value of 0 will set all values within range to a constant (computed at the center of the range), and a value of 1 will add hueRotation to all values within range."),
                                1, 1., -DBL_MAX, DBL_MAX, 0., 2.);
    hueRangeRolloff = addDouble(knobHolder, hue, kHSVToolParamHueRangeRolloff, EffectInstance::tr("Hue Range Rolloff"),
                                EffectInstance::tr("Interval (in degrees) around Hue Range, where hue rotation decreases progressively to zero."),
                                1, 0., 0., 180., 0., 180.);

    KnobGroupPtr saturation = addGroup(knobHolder, page, "saturation", EffectInstance::tr("Saturation"), EffectInstance::tr("Saturation modification settings."));
    {
        KnobDoublePtr knob = addDouble(knobHolder, saturation, kHSVToolParamSaturationRange, EffectInstance::tr("Saturation Range"),
                                       EffectInstance::tr("Range of color saturations that are modified. Saturation is the chroma over the largest of R, G and B, so a colour with a negative channel can be above 1."),
                                       2, 0., 0., DBL_MAX, 0., 1.);
        knob->setDefaultValue(1., 1);
        knob->setDimensionName(0, "min");
        knob->setDimensionName(1, "max");
        saturationRange = knob;
    }
    saturationAdjustment = addDouble(knobHolder, saturation, kHSVToolParamSaturationAdjustment, EffectInstance::tr("Saturation Adjustment"),
                                     EffectInstance::tr("Adjustment of color saturations within the range. Saturation is clamped to zero to avoid color inversions."),
                                     1, 0., -1., 1., 0., 1.);
    saturationAdjustmentGain = addDouble(knobHolder, saturation, kHSVToolParamSaturationAdjustmentGain, EffectInstance::tr("Saturation Adjustment Gain"),
                                         EffectInstance::tr("Factor to be applied to the saturation adjustment within the range. A value of 0 will set all values within range to a constant (computed at the center of the range), and a value of 1 will add saturationAdjustment to all values within range."),
                                         1, 1., -DBL_MAX, DBL_MAX, 0., 2.);
    saturationRangeRolloff = addDouble(knobHolder, saturation, kHSVToolParamSaturationRangeRolloff, EffectInstance::tr("Saturation Range Rolloff"),
                                       EffectInstance::tr("Interval around Saturation Range, where the saturation adjustment decreases progressively to zero."),
                                       1, 0., 0., 1., 0., 1.);

    KnobGroupPtr brightness = addGroup(knobHolder, page, "brightness", EffectInstance::tr("Brightness"), EffectInstance::tr("Brightness modification settings."));
    {
        KnobDoublePtr knob = addDouble(knobHolder, brightness, kHSVToolParamBrightnessRange, EffectInstance::tr("Brightness Range"),
                                       EffectInstance::tr("Range of color brightness that are modified. Brightness is the largest of R, G and B. With No Upper Limit, every brightness from the lower end up is selected, and the upper end only sets the center of the range for the gain."),
                                       2, 0., 0., DBL_MAX, 0., 1.);
        knob->setDefaultValue(1., 1);
        knob->setDimensionName(0, "min");
        knob->setDimensionName(1, "max");
        knob->setAddNewLine(false);
        brightnessRange = knob;
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(knobHolder, EffectInstance::tr("No Upper Limit").toStdString());
        knob->setName(kHSVToolParamBrightnessRangeOpen);
        knob->setHintToolTip(EffectInstance::tr("Select every brightness above the lower end of Brightness Range, so HDR values are selected. Setting Src Color or Set Src from Rectangle turns this off.").toStdString());
        knob->setDefaultValue(true);
        knob->setAnimationEnabled(false);
        brightness->addKnob(knob);
        brightnessRangeOpen = knob;
    }
    brightnessAdjustment = addDouble(knobHolder, brightness, kHSVToolParamBrightnessAdjustment, EffectInstance::tr("Brightness Adjustment"),
                                     EffectInstance::tr("Adjustment of color brightness within the range."),
                                     1, 0., -DBL_MAX, DBL_MAX, 0., 1.);
    brightnessAdjustmentGain = addDouble(knobHolder, brightness, kHSVToolParamBrightnessAdjustmentGain, EffectInstance::tr("Brightness Adjustment Gain"),
                                         EffectInstance::tr("Factor to be applied to the brightness adjustment within the range. A value of 0 will set all values within range to a constant (computed at the center of the range), and a value of 1 will add brightnessAdjustment to all values within range."),
                                         1, 1., -DBL_MAX, DBL_MAX, 0., 2.);
    brightnessRangeRolloff = addDouble(knobHolder, brightness, kHSVToolParamBrightnessRangeRolloff, EffectInstance::tr("Brightness Range Rolloff"),
                                       EffectInstance::tr("Interval around Brightness Range, where the brightness adjustment decreases progressively to zero."),
                                       1, 0., 0., DBL_MAX, 0., 1.);

    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(knobHolder, EffectInstance::tr("Clamp Black").toStdString());
        knob->setName(kHSVToolParamClampBlack);
        knob->setHintToolTip(EffectInstance::tr("All colors below 0 on output are set to 0.").toStdString());
        knob->setDefaultValue(false);
        knob->setAddNewLine(false);
        page->addKnob(knob);
        clampBlack = knob;
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(knobHolder, EffectInstance::tr("Clamp White").toStdString());
        knob->setName(kHSVToolParamClampWhite);
        knob->setHintToolTip(EffectInstance::tr("All colors above 1 on output are set to 1.").toStdString());
        knob->setDefaultValue(false);
        page->addKnob(knob);
        clampWhite = knob;
    }
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Output Alpha").toStdString());
        knob->setName(kHSVToolParamOutputAlpha);
        knob->setHintToolTip(EffectInstance::tr("Output alpha channel, when the Channels knob processes A. This can either be the source alpha, one of the coefficients for hue, saturation, brightness, or a combination of those.").toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("source", "Source", EffectInstance::tr("Alpha channel is kept unmodified.").toStdString()));
        options.push_back(ChoiceOption("hue", "Hue", EffectInstance::tr("Set Alpha to the Hue modification mask.").toStdString()));
        options.push_back(ChoiceOption("saturation", "Saturation", EffectInstance::tr("Set Alpha to the Saturation modification mask.").toStdString()));
        options.push_back(ChoiceOption("brightness", "Brightness", EffectInstance::tr("Alpha is set to the Brightness mask.").toStdString()));
        options.push_back(ChoiceOption("minhuesaturation", "min(Hue,Saturation)", EffectInstance::tr("Alpha is set to min(Hue mask,Saturation mask)").toStdString()));
        options.push_back(ChoiceOption("minhuebrightness", "min(Hue,Brightness)", EffectInstance::tr("Alpha is set to min(Hue mask,Brightness mask)").toStdString()));
        options.push_back(ChoiceOption("minsaturationbrightness", "min(Saturation,Brightness)", EffectInstance::tr("Alpha is set to min(Saturation mask,Brightness mask)").toStdString()));
        options.push_back(ChoiceOption("min", "min(all)", EffectInstance::tr("Alpha is set to min(Hue mask,Saturation mask,Brightness mask)").toStdString()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eOutputAlphaHue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        outputAlpha = knob;
    }
} // HSVToolOp::createKnobs

HSVToolOp::Params
HSVToolOp::bake(const KnobReader& reader) const
{
    Params p;

    // The hue range in degrees, as OpenFX's HSVTool normalises it.
    double h0 = reader.value(hueRange, 0);
    double h1 = reader.value(hueRange, 1);
    double hueRolloff = reader.value(hueRangeRolloff);
    double lo = 0., hi = 360., loR = 0., hiR = 360., mean = 0.;
    if (h1 == h0 + 360.) {
        p.allHues = true;
    } else {
        p.allHues = false;
        h0 = normalizeAngle(h0);
        h1 = normalizeAngle(h1);
        if (h1 < h0) {
            std::swap(h0, h1);
        }
        if (h1 - h0 > 180.) {
            std::swap(h0, h1);
        }
        hueRolloff = (std::max)(0., (std::min)(180., hueRolloff));
        lo = h0;
        hi = h1;
        loR = normalizeAngle(h0 - hueRolloff);
        hiR = normalizeAngle(h1 + hueRolloff);
        mean = normalizeAngle(h0 + normalizeAngleSigned(h1 - h0) / 2.);
    }
    p.hueLow = (float)(lo / 360.);
    p.hueHigh = (float)(hi / 360.);
    p.hueLowRolloff = (float)(loR / 360.);
    p.hueHighRolloff = (float)(hiR / 360.);
    p.hueMean = (float)(mean / 360.);
    p.hueRotation = (float)(reader.value(hueRotation) / 360.);
    p.hueGain = (float)(reader.value(hueRotationGain) - 1.);

    double s0 = reader.value(saturationRange, 0);
    double s1 = reader.value(saturationRange, 1);
    if (s1 < s0) {
        std::swap(s0, s1);
    }
    const double satRolloff = (std::max)(0., (double)reader.value(saturationRangeRolloff));
    p.satLow = (float)s0;
    p.satHigh = (float)s1;
    p.satLowRolloff = (float)(s0 - satRolloff);
    p.satHighRolloff = (float)(s1 + satRolloff);
    p.satRolloff = (float)satRolloff;
    p.satAdjust = (float)reader.value(saturationAdjustment);
    p.satGain = (float)(reader.value(saturationAdjustmentGain) - 1.);
    p.satMid = (float)((s0 + s1) / 2.);

    double v0 = reader.value(brightnessRange, 0);
    double v1 = reader.value(brightnessRange, 1);
    if (v1 < v0) {
        std::swap(v0, v1);
    }
    const double valRolloff = (std::max)(0., (double)reader.value(brightnessRangeRolloff));
    p.valLow = (float)v0;
    p.valLowRolloff = (float)(v0 - valRolloff);
    if (reader.value(brightnessRangeOpen)) {
        p.valHigh = FLT_MAX;
        p.valHighRolloff = FLT_MAX;
    } else {
        p.valHigh = (float)v1;
        p.valHighRolloff = (float)(v1 + valRolloff);
    }
    p.valRolloff = (float)valRolloff;
    p.valAdjust = (float)reader.value(brightnessAdjustment);
    p.valGain = (float)(reader.value(brightnessAdjustmentGain) - 1.);
    p.valMid = (float)((v0 + v1) / 2.);

    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);
    const int alpha = reader.value(outputAlpha);
    p.outputAlpha = ((alpha >= 0) && (alpha < (int)eOutputAlphaCount)) ? alpha : (int)eOutputAlphaSource;
    p.changesColour = (p.hueRotation != 0.f) || (p.hueGain != 0.f) || (p.satAdjust != 0.f) || (p.satGain != 0.f) || (p.valAdjust != 0.f) || (p.valGain != 0.f);
    p.writesAlpha = (p.outputAlpha != (int)eOutputAlphaSource) && processesAlpha(holder);

    return p;
} // HSVToolOp::bake

bool
HSVToolOp::isIdentity(const Params& params) const
{
    return !params.changesColour && !params.clampBlack && !params.clampWhite && !params.writesAlpha;
}

void
HSVToolOp::pixel(const Params& params,
                 const float* rgba,
                 float* out)
{
    const float r = rgba[0];
    const float g = rgba[1];
    const float b = rgba[2];
    const float maxv = (std::max)(r, (std::max)(g, b));
    const float minv = (std::min)(r, (std::min)(g, b));
    const float delta = maxv - minv;
    float h = ColorMath::colorspaceHexHue(r, g, b, maxv, delta);

    if (h >= 1.f) {
        h = h - 1.f;
    }
    const float s = (maxv > 0.f) ? delta / maxv : 0.f;
    const float v = maxv;

    float hc = 1.f;
    if (!params.allHues && !HueCurves::withinArc(h, params.hueLow, params.hueHigh)) {
        const float c0 = HueCurves::arcRise(h, params.hueLowRolloff, params.hueLow);
        const float c1 = HueCurves::arcFall(h, params.hueHigh, params.hueHighRolloff);
        hc = (std::max)(c0, c1);
    }
    const float sc = rangeCoeff(s, params.satLow, params.satHigh, params.satLowRolloff, params.satHighRolloff, params.satRolloff);
    const float vc = rangeCoeff(v, params.valLow, params.valHigh, params.valLowRolloff, params.valHighRolloff, params.valRolloff);
    const float coeff = (std::min)((std::min)(hc, sc), vc);

    out[0] = r;
    out[1] = g;
    out[2] = b;
    if (params.changesColour && (coeff > 0.f)) {
        const float dh = h - params.hueMean;
        const float sh = HueCurves::signedTurn(dh);
        const float gh = params.hueGain * sh;
        const float ah = params.hueRotation + gh;
        const float ch = coeff * ah;
        const float hn = h + ch;
        const float ds = s - params.satMid;
        const float gs = params.satGain * ds;
        const float as = params.satAdjust + gs;
        const float cs = coeff * as;
        float sn = s + cs;
        if (sn < 0.f) {
            sn = 0.f;
        }
        const float dv = v - params.valMid;
        const float gv = params.valGain * dv;
        const float av = params.valAdjust + gv;
        const float cv = coeff * av;
        const float vn = v + cv;
        // A colour whose largest channel is not positive has no saturation to scale its chroma
        // by, so it keeps its chroma.
        const float chroma = (maxv > 0.f) ? vn * sn : delta;
        ColorMath::colorspaceHexToRgb(HueCurves::wrapTurn(hn), vn, chroma, out);
    }
    if (params.clampBlack) {
        out[0] = (std::max)(0.f, out[0]);
        out[1] = (std::max)(0.f, out[1]);
        out[2] = (std::max)(0.f, out[2]);
    }
    if (params.clampWhite) {
        out[0] = (std::min)(1.f, out[0]);
        out[1] = (std::min)(1.f, out[1]);
        out[2] = (std::min)(1.f, out[2]);
    }
    switch (params.outputAlpha) {
    case eOutputAlphaHue:
        out[3] = hc;
        break;
    case eOutputAlphaSaturation:
        out[3] = sc;
        break;
    case eOutputAlphaBrightness:
        out[3] = vc;
        break;
    case eOutputAlphaHueSaturation:
        out[3] = (std::min)(hc, sc);
        break;
    case eOutputAlphaHueBrightness:
        out[3] = (std::min)(hc, vc);
        break;
    case eOutputAlphaSaturationBrightness:
        out[3] = (std::min)(sc, vc);
        break;
    case eOutputAlphaAll:
        out[3] = coeff;
        break;
    default:
        out[3] = rgba[3];
        break;
    }
} // HSVToolOp::pixel

void
HSVToolOp::row(const Params& params,
               const RowIO& io) const
{
    const int nComps = io.nComps;
    // addAcceptedComponents() admits RGB and RGBA only; a narrower pixel has no colour to select.
    if (nComps < 3) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[4] = { false, false, false, false };

    for (int c = 0; c < nComps; ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        const float rgba[4] = { src[0], src[1], src[2], (nComps == 4) ? src[3] : 1.f };
        float out[4];
        pixel(params, rgba, out);
        for (int c = 0; c < nComps; ++c) {
            if (process[c]) {
                dst[c] = out[c];
            }
        }
    }
}

GpuOpRef
HSVToolOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    HSVToolGpuParams p = {};
    p.flags = (params.allHues ? kGpuFlagAllHues : 0u) | (params.changesColour ? kGpuFlagChangesColour : 0u) | (params.clampBlack ? kGpuFlagClampBlack : 0u) | (params.clampWhite ? kGpuFlagClampWhite : 0u);
    p.outputAlpha = (uint32_t)params.outputAlpha;
    p.hueLow = params.hueLow;
    p.hueHigh = params.hueHigh;
    p.hueLowRolloff = params.hueLowRolloff;
    p.hueHighRolloff = params.hueHighRolloff;
    p.hueMean = params.hueMean;
    p.hueRotation = params.hueRotation;
    p.hueGain = params.hueGain;
    p.satLow = params.satLow;
    p.satHigh = params.satHigh;
    p.satLowRolloff = params.satLowRolloff;
    p.satHighRolloff = params.satHighRolloff;
    p.satRolloff = params.satRolloff;
    p.satAdjust = params.satAdjust;
    p.satGain = params.satGain;
    p.satMid = params.satMid;
    p.valLow = params.valLow;
    p.valHigh = params.valHigh;
    p.valLowRolloff = params.valLowRolloff;
    p.valHighRolloff = params.valHighRolloff;
    p.valRolloff = params.valRolloff;
    p.valAdjust = params.valAdjust;
    p.valGain = params.valGain;
    p.valMid = params.valMid;

    GpuOpRef ref = GpuOpRef::compiled((int)gpu::GpuKernelId::hsvtool_wrapped, p);
    ref.acceptedComponents = GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

HSVTool::HSVTool(NodePtr node)
    : ImageNode<HSVToolOp>(node)
    , _syncing(false)
{
}

HSVTool::~HSVTool()
{
}

HSVToolOp::Params
HSVTool::bakeHSVTool(double time,
                     ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
HSVTool::hsvToolRow(const HSVToolOp::Params& params,
                    const RowIO& io) const
{
    op().row(params, io);
}

void
HSVTool::addAcceptedComponents(int inputNb,
                               std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

void
HSVTool::initializeKnobs()
{
    ImageNode<HSVToolOp>::initializeKnobs();

    NodePtr node = getNode();
    if (node) {
        node->addRectangleInteract(op().bottomLeft.lock(), op().size.lock(), KnobBoolPtr(), op().enableRectangle.lock());
    }
}

void
HSVTool::onKnobsLoaded()
{
    ImageNode<HSVToolOp>::onKnobsLoaded();
    refreshRectangleVisibility();
}

void
HSVTool::refreshRectangleVisibility()
{
    const HSVToolOp& o = op();
    KnobBoolPtr enable = o.enableRectangle.lock();

    if (!enable) {
        return;
    }
    const bool on = enable->getValue();
    if (KnobDoublePtr knob = o.bottomLeft.lock()) {
        knob->setSecret(!on);
    }
    if (KnobDoublePtr knob = o.size.lock()) {
        knob->setSecret(!on);
    }
    if (KnobButtonPtr knob = o.setSrcFromRectangle.lock()) {
        knob->setSecret(!on);
    }
    if (KnobColorPtr knob = o.srcColor.lock()) {
        knob->setAllDimensionsEnabled(!on);
    }
}

void
HSVTool::setSrcFromColor(double time)
{
    const HSVToolOp& o = op();
    KnobColorPtr src = o.srcColor.lock();
    KnobColorPtr dst = o.dstColor.lock();

    if (!src || !dst) {
        return;
    }
    const HSVColor from = hsvOf((float)src->getValueAtTime(time, 0), (float)src->getValueAtTime(time, 1), (float)src->getValueAtTime(time, 2));
    const HSVColor to = hsvOf((float)dst->getValueAtTime(time, 0), (float)dst->getValueAtTime(time, 1), (float)dst->getValueAtTime(time, 2));
    const double dh = normalizeAngleSigned(to.h - from.h);

    o.hueRange.lock()->setValue(from.h, ViewSpec::all(), 0);
    o.hueRange.lock()->setValue(from.h, ViewSpec::all(), 1);
    o.hueRangeRolloff.lock()->setValue(50.);
    // No rotation or saturation change towards black.
    if (to.v != 0.) {
        o.hueRotation.lock()->setValue(dh);
    }
    o.saturationRange.lock()->setValue(from.s, ViewSpec::all(), 0);
    o.saturationRange.lock()->setValue(from.s, ViewSpec::all(), 1);
    o.saturationRangeRolloff.lock()->setValue(0.3);
    if (to.v != 0.) {
        o.saturationAdjustment.lock()->setValue(to.s - from.s);
    }
    o.brightnessRange.lock()->setValue(from.v, ViewSpec::all(), 0);
    o.brightnessRange.lock()->setValue(from.v, ViewSpec::all(), 1);
    o.brightnessRangeOpen.lock()->setValue(false);
    o.brightnessRangeRolloff.lock()->setValue(0.3);
    o.brightnessAdjustment.lock()->setValue(to.v - from.v);
}

void
HSVTool::setDstFromColor(double time)
{
    const HSVToolOp& o = op();
    KnobColorPtr src = o.srcColor.lock();
    KnobColorPtr dst = o.dstColor.lock();

    if (!src || !dst) {
        return;
    }
    const HSVColor from = hsvOf((float)src->getValueAtTime(time, 0), (float)src->getValueAtTime(time, 1), (float)src->getValueAtTime(time, 2));
    const HSVColor to = hsvOf((float)dst->getValueAtTime(time, 0), (float)dst->getValueAtTime(time, 1), (float)dst->getValueAtTime(time, 2));
    const double dh = normalizeAngleSigned(to.h - from.h);

    if (to.v != 0.) {
        o.hueRotation.lock()->setValue(dh);
        o.saturationAdjustment.lock()->setValue(to.s - from.s);
    }
    o.brightnessAdjustment.lock()->setValue(to.v - from.v);
}

bool
HSVTool::setSrcFromRectangleAt(double time,
                               ViewIdx view)
{
    const HSVToolOp& o = op();
    KnobDoublePtr bottomLeftKnob = o.bottomLeft.lock();
    KnobDoublePtr sizeKnob = o.size.lock();
    KnobColorPtr dst = o.dstColor.lock();

    if (!bottomLeftKnob || !sizeKnob || !dst || !getInput(0)) {
        return false;
    }
    const double x = bottomLeftKnob->getValueAtTime(time, 0);
    const double y = bottomLeftKnob->getValueAtTime(time, 1);
    RectI box;
    box.x1 = (int)std::floor(x);
    box.y1 = (int)std::floor(y);
    box.x2 = (int)std::ceil(x + sizeKnob->getValueAtTime(time, 0));
    box.y2 = (int)std::ceil(y + sizeKnob->getValueAtTime(time, 1));
    if (box.isNull()) {
        return false;
    }
    const RectD bounds(box.x1, box.y1, box.x2, box.y2);
    RectI roi;
    ImagePtr src = getImage(0, time, RenderScale::identity, view, &bounds, NULL, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return false;
    }
    const RectI area = box.intersect(src->getBounds());
    const int nComps = (int)src->getComponentsCount();
    if (area.isNull() || (nComps < 3)) {
        return false;
    }

    // The circular mean of the hues of the pixels bright and saturated enough to have one,
    // then each range around it.
    double sumSin = 0.;
    double sumCos = 0.;
    unsigned long count = 0;
    double dhMin = 0.;
    double dhMax = 0.;
    HSVColor lo;
    HSVColor hi;
    lo.s = lo.v = DBL_MAX;
    hi.s = hi.v = -DBL_MAX;
    double hueMean = 0.;
    {
        Image::ReadAccess access(src.get());
        for (int pass = 0; pass < 2; ++pass) {
            for (int py = area.y1; py < area.y2; ++py) {
                const float* pix = (const float*)access.pixelAt(area.x1, py);
                if (!pix) {
                    continue;
                }
                for (int px = area.x1; px < area.x2; ++px, pix += nComps) {
                    const HSVColor c = hsvOf(pix[0], pix[1], pix[2]);
                    const bool hasHue = (c.s > kMinSaturation) && (c.v > kMinValue);
                    if (pass == 0) {
                        if (hasHue) {
                            sumSin += std::sin(c.h * M_PI / 180.);
                            sumCos += std::cos(c.h * M_PI / 180.);
                            ++count;
                        }
                        continue;
                    }
                    if (hasHue) {
                        const double dh = normalizeAngleSigned(c.h - hueMean);
                        dhMin = (std::min)(dhMin, dh);
                        dhMax = (std::max)(dhMax, dh);
                    }
                    lo.s = (std::min)(lo.s, c.s);
                    hi.s = (std::max)(hi.s, c.s);
                    lo.v = (std::min)(lo.v, c.v);
                    hi.v = (std::max)(hi.v, c.v);
                }
            }
            if (pass == 0) {
                hueMean = count ? normalizeAngle(std::atan2(sumSin / (double)count, sumCos / (double)count) * 180. / M_PI) : 0.;
            }
        }
    }
    if (dhMax - dhMin > 179.9) {
        lo.h = 0.;
        hi.h = 360.;
    } else {
        lo.h = normalizeAngle(hueMean + dhMin);
        hi.h = normalizeAngle(hueMean + dhMax);
    }

    const double meanS = (lo.s + hi.s) / 2.;
    const double meanV = (lo.v + hi.v) / 2.;
    float rgb[3];
    const float meanHue = (float)(normalizeAngle(hueMean) / 360.);
    ColorMath::colorspaceHexToRgb(meanHue, (float)meanV, (float)(meanV * meanS), rgb);
    const HSVColor to = hsvOf((float)dst->getValueAtTime(time, 0), (float)dst->getValueAtTime(time, 1), (float)dst->getValueAtTime(time, 2));
    const double dh = normalizeAngleSigned(to.h - normalizeAngle(hueMean));
    double hueSpan = hi.h - lo.h;
    if (hueSpan < 0.) {
        hueSpan += 360.;
    }
    const double hueRolloff = (std::min)(hueSpan * kRectangleRolloff, (360. - hueSpan) / 2.);

    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const bool wasSyncing = _syncing;
    _syncing = true;
    if (KnobColorPtr srcKnob = o.srcColor.lock()) {
        for (int c = 0; c < 3; ++c) {
            srcKnob->setValue(fround(rgb[c], 4), ViewSpec::all(), c);
        }
    }
    o.hueRange.lock()->setValue(ffloor(lo.h, 2), ViewSpec::all(), 0);
    o.hueRange.lock()->setValue(fceil(hi.h, 2), ViewSpec::all(), 1);
    o.hueRangeRolloff.lock()->setValue(ffloor(hueRolloff, 2));
    if (to.v != 0.) {
        o.hueRotation.lock()->setValue(fround(dh, 2));
    }
    o.saturationRange.lock()->setValue(ffloor(lo.s, 4), ViewSpec::all(), 0);
    o.saturationRange.lock()->setValue(fceil(hi.s, 4), ViewSpec::all(), 1);
    o.saturationRangeRolloff.lock()->setValue(ffloor((hi.s - lo.s) * kRectangleRolloff, 4));
    if (to.v != 0.) {
        o.saturationAdjustment.lock()->setValue(fround(to.s - meanS, 4));
    }
    o.brightnessRange.lock()->setValue(ffloor(lo.v, 4), ViewSpec::all(), 0);
    o.brightnessRange.lock()->setValue(fceil(hi.v, 4), ViewSpec::all(), 1);
    o.brightnessRangeOpen.lock()->setValue(false);
    o.brightnessRangeRolloff.lock()->setValue(ffloor((hi.v - lo.v) * kRectangleRolloff, 4));
    o.brightnessAdjustment.lock()->setValue(fround(to.v - meanV, 4));
    _syncing = wasSyncing;
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }

    return true;
} // HSVTool::setSrcFromRectangleAt

bool
HSVTool::knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     ViewSpec view,
                     double time,
                     bool originatedFromMainThread)
{
    const HSVToolOp& o = op();
    const KnobBoolPtr enable = o.enableRectangle.lock();
    const KnobButtonPtr fromRectangle = o.setSrcFromRectangle.lock();
    const KnobColorPtr src = o.srcColor.lock();
    const KnobColorPtr dst = o.dstColor.lock();
    const bool isEnable = enable && (k == enable.get());
    const bool isFromRectangle = fromRectangle && (k == fromRectangle.get());
    const bool isSrc = src && (k == src.get());
    const bool isDst = dst && (k == dst.get());

    if (!isEnable && !isFromRectangle && !isSrc && !isDst) {
        return ImageNode<HSVToolOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
    }
    if (isEnable) {
        refreshRectangleVisibility();

        return true;
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    // A project load or a time change sets every knob to what was saved; only an edit moves the
    // others.
    if (_syncing || loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
        return true;
    }
    if (isFromRectangle) {
        const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);
        (void)setSrcFromRectangleAt(time, viewIdx);

        return true;
    }
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    _syncing = true;
    if (isSrc) {
        setSrcFromColor(time);
    } else {
        setDstFromColor(time);
    }
    _syncing = false;
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }

    return true;
} // HSVTool::knobChanged

NATRON_NAMESPACE_EXIT
