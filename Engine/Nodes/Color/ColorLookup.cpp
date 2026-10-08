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

#include "ColorLookup.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/Image.h"
#include "Engine/Interpolation.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/CurveSnapshot.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kHistogramBins = 256;
const int kRampSliceWidthPixels = 8;
const int kRampMaxSlices = 1024;

// The three colours share one Rec. 709 luminance, so the blue stays visible and their sum is white;
// halved so the sum is 50% white.
const float kHistogramColors[3][3] = {
    { 0.711519527404004f / 2, 0.164533420851110f / 2, 0.164533420851110f / 2 },
    { 0.f, 0.546986106552894f / 2, 0.f },
    { 0.288480472595996f / 2, 0.288480472595996f / 2, 0.835466579148890f / 2 }
};

// Intervals of the lookup table over `range`.
const int kLutIntervals = 1023;

int
curveOfChannel(int channelBit)
{
    return ColorLookup::eCurveRed + channelBit;
}

struct KernelParams {
    ColorLookup::MasterCurveModeEnum mode;
    ColorMath::LuminanceMathEnum luminanceMath;
    double rangeMin;
    double rangeMax;
    bool clampBlack;
    bool clampWhite;

    KernelParams()
        : mode(ColorLookup::eMasterCurveModeStandard)
        , luminanceMath(ColorMath::eLuminanceMathRec709)
        , rangeMin(0.)
        , rangeMax(1.)
        , clampBlack(false)
        , clampWhite(false)
    {
    }
};

class ColorLookupKernel
    : public PixelKernel {
public:
    ColorLookupKernel(const KernelParams& params,
                      const CurveSnapshot curves[ColorLookup::eCurveCount])
        : _mode(params.mode)
        , _luminanceMath(params.luminanceMath)
        , _rangeMin((std::min)(params.rangeMin, params.rangeMax))
        , _rangeMax((std::max)(params.rangeMin, params.rangeMax))
        , _clampBlack(params.clampBlack)
        , _clampWhite(params.clampWhite)
    {
        for (int i = 0; i < ColorLookup::eCurveCount; ++i) {
            _curves[i] = curves[i];
        }
        if (_rangeMin == _rangeMax) {
            _rangeMax = _rangeMin + 1.;
        }
        const bool standard = isStandard();
        for (int curve = 0; curve < ColorLookup::eCurveCount; ++curve) {
            if ((curve == ColorLookup::eCurveMaster) && standard) {
                continue;
            }
            _lut[curve].resize(kLutIntervals + 1);
            for (int position = 0; position <= kLutIntervals; ++position) {
                const double parametricPos = _rangeMin + (_rangeMax - _rangeMin) * double(position) / kLutIntervals;
                _lut[curve][position] = (float)clamp(curveValue(curve, parametricPos));
            }
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;

        if (nComps == 1) {
            const bool process = io.channels[3];
            for (int x = 0; x < io.width; ++x, ++src, ++dst) {
                if (process) {
                    *dst = interpolate(ColorLookup::eCurveAlpha, *src);
                }
            }

            return;
        }
        if (nComps == 2) {
            for (int x = 0; x < io.width; ++x, src += 2, dst += 2) {
                for (int c = 0; c < 2; ++c) {
                    if (io.channels[c]) {
                        dst[c] = channelOnly(curveOfChannel(c), src[c]);
                    }
                }
            }

            return;
        }
        bool process[4];
        for (int c = 0; c < 4; ++c) {
            process[c] = (c < nComps) && io.channels[c];
        }
        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            float out[3];
            if (process[0] || process[1] || process[2]) {
                rgb(src[0], src[1], src[2], out);
                for (int c = 0; c < 3; ++c) {
                    if (process[c]) {
                        dst[c] = out[c];
                    }
                }
            }
            if ((nComps == 4) && process[3]) {
                dst[3] = interpolate(ColorLookup::eCurveAlpha, src[3]);
            }
        }
    }

private:
    bool isStandard() const
    {
        return (_mode == ColorLookup::eMasterCurveModeStandard) || (_mode == ColorLookup::eMasterCurveModeWeightedStandard);
    }

    double clamp(double value) const
    {
        if (_clampBlack && (value < 0.)) {
            return 0.;
        } else if (_clampWhite && (value > 1.)) {
            return 1.;
        }

        return value;
    }

    float clamp(float value) const
    {
        if (_clampBlack && (value < 0.f)) {
            return 0.f;
        } else if (_clampWhite && (value > 1.f)) {
            return 1.f;
        }

        return value;
    }

    // A channel curve at pos, with the master curve folded in for the modes that do so.
    double curveValue(int curve,
                      double pos) const
    {
        double value = _curves[curve].valueAt(pos);

        if (isStandard() && (curve >= ColorLookup::eCurveRed) && (curve <= ColorLookup::eCurveBlue)) {
            value += _curves[ColorLookup::eCurveMaster].valueAt(pos) - pos;
        }

        return value;
    }

    // Interpolated in the table inside `range`, evaluated directly outside it. The float steps
    // are the OpenFX plug-in's.
    float interpolate(int curve,
                      float value) const
    {
        if ((value < _rangeMin) || (_rangeMax < value)) {
            return static_cast<float>(clamp(curveValue(curve, value)));
        }
        const std::vector<float>& lut = _lut[curve];
        const double x = (value - _rangeMin) / (_rangeMax - _rangeMin);
        if (x <= 0.) {
            return lut[0];
        } else if (x >= 1.) {
            return lut[kLutIntervals];
        }
        int i = (int)(x * kLutIntervals);
        i = (std::max)(0, (std::min)(i, kLutIntervals - 1));
        const double alpha = (std::max)(0., (std::min)(x * kLutIntervals - i, 1.));
        const float a = lut[i];
        const float b = lut[i + 1];

        return static_cast<float>(a * (1. - alpha) + b * alpha);
    }

    // One channel of a pixel with fewer than three, which the colour modes cannot be applied to:
    // its own curve, plus the master curve's offset when that is not already in the table.
    float channelOnly(int curve,
                      float value) const
    {
        float result = interpolate(curve, value);

        if (!isStandard()) {
            result = (float)clamp(result + (interpolate(ColorLookup::eCurveMaster, value) - value));
        }

        return result;
    }

    static float triangle(float a,
                          float a1,
                          float b)
    {
        if (a != b) {
            float b1;
            const float a2 = a1 - a;

            if (b < a) {
                b1 = b + a2 * b / a;
            } else {
                b1 = b + a2 * (1.f - b) / (1.f - a);
            }

            return b1;
        }

        return a1;
    }

    void rgbTone(float& r,
                 float& g,
                 float& b) const
    {
        const float rold = r;
        const float gold = g;
        const float bold = b;

        r = interpolate(ColorLookup::eCurveMaster, rold);
        b = interpolate(ColorLookup::eCurveMaster, bold);
        g = b + ((r - b) * (gold - bold) / (rold - bold));
    }

    void rgb(float r,
             float g,
             float b,
             float out[3]) const
    {
        switch (_mode) {
        case ColorLookup::eMasterCurveModeStandard:
            out[0] = interpolate(ColorLookup::eCurveRed, r);
            out[1] = interpolate(ColorLookup::eCurveGreen, g);
            out[2] = interpolate(ColorLookup::eCurveBlue, b);
            break;
        case ColorLookup::eMasterCurveModeWeightedStandard: {
            const float r1 = interpolate(ColorLookup::eCurveRed, r);
            const float g1 = triangle(r, r1, g);
            const float b1 = triangle(r, r1, b);

            const float g2 = interpolate(ColorLookup::eCurveGreen, g);
            const float r2 = triangle(g, g2, r);
            const float b2 = triangle(g, g2, b);

            const float b3 = interpolate(ColorLookup::eCurveBlue, b);
            const float r3 = triangle(b, b3, r);
            const float g3 = triangle(b, b3, g);

            out[0] = clamp(r1 * 0.50f + r2 * 0.25f + r3 * 0.25f);
            out[1] = clamp(g1 * 0.25f + g2 * 0.50f + g3 * 0.25f);
            out[2] = clamp(b1 * 0.25f + b2 * 0.25f + b3 * 0.50f);
            break;
        }
        case ColorLookup::eMasterCurveModeFilmLike: {
            const double rcoef = r < 1e-8 ? 1. : (interpolate(ColorLookup::eCurveRed, r) / r);
            const double gcoef = g < 1e-8 ? 1. : (interpolate(ColorLookup::eCurveGreen, g) / g);
            const double bcoef = b < 1e-8 ? 1. : (interpolate(ColorLookup::eCurveBlue, b) / b);
            if (r >= g) {
                if (g > b) {
                    rgbTone(r, g, b);
                } else if (b > r) {
                    rgbTone(b, r, g);
                } else if (b > g) {
                    rgbTone(r, b, g);
                } else {
                    r = interpolate(ColorLookup::eCurveMaster, r);
                    g = interpolate(ColorLookup::eCurveMaster, g);
                    b = g;
                }
            } else {
                if (r >= b) {
                    rgbTone(g, r, b);
                } else if (b > g) {
                    rgbTone(b, g, r);
                } else {
                    rgbTone(g, b, r);
                }
            }
            out[0] = static_cast<float>(clamp(rcoef * r));
            out[1] = static_cast<float>(clamp(gcoef * g));
            out[2] = static_cast<float>(clamp(bcoef * b));
            break;
        }
        case ColorLookup::eMasterCurveModeLuminance: {
            const float luma = static_cast<float>(ColorMath::luminance(_luminanceMath, r, g, b));
            const double l = (std::max)(luma, 1.e-8f);
            const double coef = interpolate(ColorLookup::eCurveMaster, (float)l) / l;
            out[0] = static_cast<float>(clamp(coef * interpolate(ColorLookup::eCurveRed, r)));
            out[1] = static_cast<float>(clamp(coef * interpolate(ColorLookup::eCurveGreen, g)));
            out[2] = static_cast<float>(clamp(coef * interpolate(ColorLookup::eCurveBlue, b)));
            break;
        }
        }
    }

    ColorLookup::MasterCurveModeEnum _mode;
    ColorMath::LuminanceMathEnum _luminanceMath;
    double _rangeMin;
    double _rangeMax;
    bool _clampBlack;
    bool _clampWhite;
    CurveSnapshot _curves[ColorLookup::eCurveCount];
    std::vector<float> _lut[ColorLookup::eCurveCount];
};

NativeImageTraits
colorLookupTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;

    return traits;
}

template <typename T>
bool
isKnob(KnobI* k,
       const std::weak_ptr<T>& weak)
{
    std::shared_ptr<T> knob = weak.lock();

    return knob && (k == knob.get());
}

void
readColor(const KnobColorWPtr& weak,
          double time,
          ViewIdx view,
          double out[4])
{
    KnobColorPtr knob = weak.lock();

    for (int i = 0; i < 4; ++i) {
        out[i] = knob ? knob->getValueAtTime(time, i, view) : 0.;
    }
}

bool
readBool(const KnobBoolWPtr& weak,
         double time,
         ViewIdx view)
{
    KnobBoolPtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : false;
}

int
readChoice(const KnobChoiceWPtr& weak,
           double time,
           ViewIdx view)
{
    KnobChoicePtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : 0;
}

KnobColorPtr
addColorKnob(KnobHolder* holder,
             const KnobPagePtr& page,
             const std::string& name,
             const std::string& label,
             const std::string& hint)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 4; ++i) {
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(4., i);
    }
    knob->setEvaluateOnChange(false);
    knob->setIsPersistent(false);
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

struct ColorLookup::BackgroundState {
    std::mutex mutex;
    std::vector<unsigned long long> counts;
    double rangeMin;
    double rangeMax;

    BackgroundState()
        : mutex()
        , counts()
        , rangeMin(0.)
        , rangeMax(1.)
    {
    }
};

ColorLookup::ColorLookup(NodePtr node)
    : NativeImageEffect(node, colorLookupTraits())
    , _background(std::make_shared<BackgroundState>())
{
}

ColorLookup::~ColorLookup()
{
}

NativePluginDescription
ColorLookup::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_COLORLOOKUP;
    desc.label = "ColorLookup";
    desc.description = tr("Apply a parametric lookup curve with the possibility to adjust each channel separately.\n"
                          "The master curve is combined with the red, green and blue curves, but not with the alpha curve.\n"
                          "Different algorithms are available when applying the master curve, which are selectable using the \"Master Curve Mode\" parameter.\n"
                          "Computation is faster for values that are within the given range, so it is recommended to set the Range parameter if the input range goes beyond [0,1].\n"
                          "\n"
                          "Note that you can easily do color remapping by setting Source and Target colors and clicking \"Set RGB\" or \"Set RGBA\" below.\n"
                          "This will add control points on the curve to match the target from the source. You can add as many point as you like.\n"
                          "This is very useful for matching color of one shot to another, or adding custom colors to a black and white ramp.\n"
                          "\n"
                          "Optionally, the RGB histogram or a color ramp can be displayed in the background of the lookup curves.\n"
                          "\n"
                          "See also: https://web.archive.org/web/20221208230906/http://opticalenquiry.com/nuke/index.php?title=ColorLookup")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_COLOR;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_COLORLOOKUP;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
ColorLookup::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    {
        KnobBoolPtr knob = createKnob<KnobBool>(std::string("Has Background Interact"));
        knob->setName(kColorLookupParamHasBackgroundInteract);
        knob->setDefaultValue(false);
        knob->setSecretByDefault(true);
        knob->setAnimationEnabled(false);
        knob->setEvaluateOnChange(false);
        page->addKnob(knob);
        _hasBackgroundInteract = knob;
    }
    {
        KnobDoublePtr range = createKnob<KnobDouble>(tr("Range"), 2);
        range->setName(kColorLookupParamRange);
        range->setHintToolTip(tr("Expected range for input values. Within this range, a lookup table is used for faster computation."));
        range->setDimensionName(0, "min");
        range->setDimensionName(1, "max");
        range->setDefaultValue(0., 0);
        range->setDefaultValue(1., 1);
        for (int i = 0; i < 2; ++i) {
            range->setMinimum(-DBL_MAX, i);
            range->setMaximum(DBL_MAX, i);
            range->setDisplayMinimum(0., i);
            range->setDisplayMaximum(1., i);
        }
        page->addKnob(range);
        _range = range;
    }
    {
        KnobParametricPtr table = createKnob<KnobParametric>(tr("Lookup Table"), eCurveCount);
        table->setName(kColorLookupParamLookupTable);
        table->setHintToolTip(tr("Colour lookup table. The master curve is combined with the red, green and blue curves, but not with the alpha curve."));
        table->setDimensionName(eCurveMaster, "master");
        table->setDimensionName(eCurveRed, "red");
        table->setDimensionName(eCurveGreen, "green");
        table->setDimensionName(eCurveBlue, "blue");
        table->setDimensionName(eCurveAlpha, "alpha");
        // The red, green and blue colours share one Rec. 709 luminance.
        table->setCurveColor(eCurveMaster, 0.9, 0.9, 0.9);
        table->setCurveColor(eCurveRed, 0.711519527404004, 0.164533420851110, 0.164533420851110);
        table->setCurveColor(eCurveGreen, 0., 0.546986106552894, 0.);
        table->setCurveColor(eCurveBlue, 0.288480472595996, 0.288480472595996, 0.835466579148890);
        table->setCurveColor(eCurveAlpha, 0.398979, 0.398979, 0.398979);
        table->setParametricRange(0., 1.);
        table->setDisplayMinimumsAndMaximums(std::vector<double>(eCurveCount, 0.), std::vector<double>(eCurveCount, 1.));
        for (int curve = 0; curve < eCurveCount; ++curve) {
            StatusEnum first = table->addControlPoint(eValueChangedReasonPluginEdited, curve, 0., 0., eKeyframeTypeCubic);
            StatusEnum second = table->addControlPoint(eValueChangedReasonPluginEdited, curve, 1., 1., eKeyframeTypeCubic);
            Q_UNUSED(first);
            Q_UNUSED(second);
        }
        table->setDefaultCurvesFromCurves();
        page->addKnob(table);
        _lookupTable = table;
    }
    {
        KnobBoolPtr knob = createKnob<KnobBool>(std::string("Show Ramp"));
        knob->setName(kColorLookupParamShowRamp);
        knob->setDefaultValue(false);
        knob->setSecretByDefault(true);
        knob->setAnimationEnabled(false);
        knob->setEvaluateOnChange(false);
        page->addKnob(knob);
    }
    {
        KnobChoicePtr display = createKnob<KnobChoice>(tr("Display"));
        display->setName(kColorLookupParamDisplay);
        display->setHintToolTip(tr("Display a color ramp or a histogram behind the curves."));
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("none", tr("None").toStdString(), tr("No background display.").toStdString()));
        choices.push_back(ChoiceOption("colorramp", tr("Color Ramp").toStdString(), tr("Display a color ramp.").toStdString()));
        choices.push_back(ChoiceOption("histogram", tr("RGB Histogram").toStdString(), tr("Display the input histogram. Press \"Refresh Histogram\" to recompute the histogram.").toStdString()));
        display->populateChoices(choices);
        display->setDefaultValue((int)eDisplayColorRamp);
        display->setAnimationEnabled(false);
        display->setEvaluateOnChange(false);
        display->setSecretByDefault(true);
        display->setAddNewLine(false);
        display->setSpacingBetweenItems(1);
        page->addKnob(display);
        _display = display;
    }
    {
        KnobButtonPtr button = createKnob<KnobButton>(tr("Update Histogram"));
        button->setName(kColorLookupParamUpdateHistogram);
        button->setHintToolTip(tr("Update the histogram from the input at current time."));
        button->setEvaluateOnChange(false);
        button->setSecretByDefault(true);
        page->addKnob(button);
        _updateHistogram = button;
    }
    _source = addColorKnob(this, page, kColorLookupParamSource, tr("Source").toStdString(), tr("Source color for newly added points (x coordinate on the curve).").toStdString());
    _target = addColorKnob(this, page, kColorLookupParamTarget, tr("Target").toStdString(), tr("Target color for newly added points (y coordinate on the curve).").toStdString());

    struct ButtonSpec {
        const char* name;
        const char* label;
        const char* hint;
        bool newLine;
        KnobButtonWPtr* member;
    };
    const ButtonSpec buttons[] = {
        { kColorLookupParamSetMaster, "Set Master", "Add a new control point mapping source to target to the master curve (the relative luminance is computed using the 'Luminance Math' parameter).", false, &_setMaster },
        { kColorLookupParamSetRGB, "Set RGB", "Add a new control point mapping source to target to the red, green, and blue curves.", false, &_setRGB },
        { kColorLookupParamSetRGBA, "Set RGBA", "Add a new control point mapping source to target to the red, green, blue and alpha curves.", false, &_setRGBA },
        { kColorLookupParamSetA, "Set A", "Add a new control point mapping source to target to the alpha curve", true, &_setA }
    };
    for (std::size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        KnobButtonPtr button = createKnob<KnobButton>(tr(buttons[i].label));
        button->setName(buttons[i].name);
        button->setHintToolTip(tr(buttons[i].hint));
        button->setEvaluateOnChange(false);
        button->setAddNewLine(buttons[i].newLine);
        if (!buttons[i].newLine) {
            button->setSpacingBetweenItems(1);
        }
        page->addKnob(button);
        *buttons[i].member = button;
    }
    {
        KnobChoicePtr mode = createKnob<KnobChoice>(tr("Master Curve Mode"));
        mode->setName(kColorLookupParamMasterCurveMode);
        mode->setHintToolTip(tr("Algorithm that will be used for the master curve. The curve mode will have a strong effect on the appearance of colors, especially if you use a contrast-enhancing curve (S-curve). This can be used for creative effect, but can for some purposes or styles cause undesired color changes depending which mode you choose. Choose a mode that suits your specific taste and needs for the photo at hand. More information can be found at http://rawpedia.rawtherapee.com/Exposure"));
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("standard", tr("Standard").toStdString(), tr("The marster curve is applied independently to R, G and B channels. The drawback of this mode is that e.g. considering an S-curve shape to get more contrast, an orange color with a high value of red and green and a low value of blue will tend to shift toward yellow, because the red and green channel will be raised, while the blue one will be lowered.").toStdString()));
        choices.push_back(ChoiceOption("weightedstandard", tr("Weighted Standard").toStdString(), tr("You can use this method to limit the color shift of the standard curve, even if it won't suppress it entirely.").toStdString()));
        choices.push_back(ChoiceOption("filmlike", tr("Film-Like").toStdString(), tr("The film-like curve provides a result highly similar to the standard type (that is strong saturation increase with increased contrast), but the RGB-HSV hue is kept constant - that is, there are less color-shift problems. This curve type was designed by Adobe as a part of DNG and is thus the one used by Adobe Camera Raw and Lightroom.").toStdString()));
        choices.push_back(ChoiceOption("luminance", tr("Luminance").toStdString(), tr("Each component of the pixel is boosted by the same factor so color and saturation is kept stable, that is the result is very true to the original color. However contrast-increasing curves can still lead to a slightly desaturated look. First the relative luminance value of a pixel is obtained, then the curve is applied to that value, the multiplication factor between before and after luminance is calculated, and then this factor is applied to each R, G and B component. The formula used to compute the luminance can be selected using the \"luminanceMath\" parameter.").toStdString()));
        mode->populateChoices(choices);
        mode->setDefaultValue((int)eMasterCurveModeStandard);
        mode->setAnimationEnabled(false);
        page->addKnob(mode);
        _masterCurveMode = mode;
    }

    _luminanceMath = ColorMath::addLuminanceMathKnob(this, page);
    if (KnobChoicePtr luminanceMath = _luminanceMath.lock()) {
        luminanceMath->setHintToolTip(tr("Formula used to compute luminance from RGB values (only used by 'Set Master' and by the luminance master curve mode)."));
    }

    KnobBoolPtr clampBlack = createKnob<KnobBool>(tr("Clamp Black"));
    clampBlack->setName(kColorLookupParamClampBlack);
    clampBlack->setHintToolTip(tr("All colors below 0 on output are set to 0."));
    clampBlack->setDefaultValue(false);
    clampBlack->setAddNewLine(false);
    page->addKnob(clampBlack);
    _clampBlack = clampBlack;

    KnobBoolPtr clampWhite = createKnob<KnobBool>(tr("Clamp White"));
    clampWhite->setName(kColorLookupParamClampWhite);
    clampWhite->setHintToolTip(tr("All colors above 1 on output are set to 1."));
    clampWhite->setDefaultValue(false);
    page->addKnob(clampWhite);
    _clampWhite = clampWhite;

    addMaskMixKnobs(page);

    installBackgroundPainter();
    refreshBackgroundControls();
} // ColorLookup::initializeKnobs

PixelKernelPtr
ColorLookup::makeKernel(const KernelContext& context)
{
    KernelParams params;

    params.mode = (MasterCurveModeEnum)readChoice(_masterCurveMode, context.time, context.view);
    params.luminanceMath = (ColorMath::LuminanceMathEnum)readChoice(_luminanceMath, context.time, context.view);
    params.clampBlack = readBool(_clampBlack, context.time, context.view);
    params.clampWhite = readBool(_clampWhite, context.time, context.view);
    if (KnobDoublePtr range = _range.lock()) {
        params.rangeMin = range->getValueAtTime(context.time, 0, context.view);
        params.rangeMax = range->getValueAtTime(context.time, 1, context.view);
    }

    CurveSnapshot curves[eCurveCount];
    if (KnobParametricPtr table = _lookupTable.lock()) {
        for (int curve = 0; curve < eCurveCount; ++curve) {
            curves[curve] = CurveSnapshot(table->getParametricCurve(curve));
        }
    }

    return std::make_shared<ColorLookupKernel>(params, curves);
}

void
ColorLookup::refreshBackgroundControls()
{
    KnobBoolPtr hasBackground = _hasBackgroundInteract.lock();
    KnobChoicePtr display = _display.lock();
    KnobButtonPtr update = _updateHistogram.lock();

    if (!hasBackground || !display || !update) {
        return;
    }
    const bool visible = hasBackground->getValue();
    display->setSecret(!visible);
    display->setAllDimensionsEnabled(visible);
    update->setSecret(!visible);
    update->setAllDimensionsEnabled(visible && (display->getValue() == (int)eDisplayHistogram));
}

void
ColorLookup::onKnobsLoaded()
{
    refreshBackgroundControls();
}

void
ColorLookup::installBackgroundPainter()
{
    KnobParametricPtr table = _lookupTable.lock();
    KnobBoolPtr hasBackground = _hasBackgroundInteract.lock();

    if (!table || !hasBackground) {
        return;
    }
    const KnobParametricWPtr weakTable = table;
    const KnobDoubleWPtr weakRange = _range;
    const KnobChoiceWPtr weakDisplay = _display;
    const std::shared_ptr<BackgroundState> state = _background;

    table->setBackgroundPainter([weakTable, weakRange, weakDisplay, state](const ParametricBackgroundContext& context) {
        ParametricBackground background;
        KnobParametricPtr lookup = weakTable.lock();
        KnobDoublePtr range = weakRange.lock();
        KnobChoicePtr display = weakDisplay.lock();

        if (!lookup || !range || !display) {
            return background;
        }
        const int mode = display->getValue();

        if (mode == (int)eDisplayColorRamp) {
            const double rangeMin = range->getValue(0);
            const double rangeMax = range->getValue(1);
            const double x0 = (std::max)(rangeMin, context.xMin);
            const double x1 = (std::min)(rangeMax, context.xMax);
            if (!(rangeMax > rangeMin) || !(x1 > x0)) {
                return background;
            }
            int slices = 1;
            if (context.pixelScaleX > 0.) {
                slices = (int)std::ceil((x1 - x0) / (kRampSliceWidthPixels * context.pixelScaleX));
            }
            slices = (std::max)(1, (std::min)(slices, kRampMaxSlices));

            std::vector<ParametricBackgroundVertex> bottom(slices + 1);
            std::vector<ParametricBackgroundVertex> top(slices + 1);
            for (int i = 0; i <= slices; ++i) {
                const double position = x0 + (x1 - x0) * double(i) / slices;
                double master = position;
                StatusEnum status = lookup->getValue(eCurveMaster, position, &master);
                Q_UNUSED(status);
                float rgb[3];
                for (int c = 0; c < 3; ++c) {
                    double value = position;
                    status = lookup->getValue(curveOfChannel(c), position, &value);
                    rgb[c] = (float)(value + master - position);
                }
                bottom[i] = ParametricBackgroundVertex(position, rangeMin, rgb[0], rgb[1], rgb[2]);
                top[i] = ParametricBackgroundVertex(position, rangeMax, rgb[0], rgb[1], rgb[2]);
            }
            background.quads.resize(slices);
            for (int i = 0; i < slices; ++i) {
                ParametricBackgroundQuad& quad = background.quads[i];
                quad.v[0] = bottom[i];
                quad.v[1] = bottom[i + 1];
                quad.v[2] = top[i + 1];
                quad.v[3] = top[i];
            }
        } else if (mode == (int)eDisplayHistogram) {
            std::vector<unsigned long long> counts;
            double rangeMin;
            double rangeMax;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                counts = state->counts;
                rangeMin = state->rangeMin;
                rangeMax = state->rangeMax;
            }
            if (counts.size() != 3 * (std::size_t)kHistogramBins || !(rangeMax > rangeMin)) {
                return background;
            }
            // The first and last bins collect everything outside the range and would flatten the rest.
            unsigned long long peak = 0;
            for (int c = 0; c < 3; ++c) {
                for (int i = 1; i < kHistogramBins - 1; ++i) {
                    peak = (std::max)(peak, counts[c * kHistogramBins + i]);
                }
            }
            if (peak == 0) {
                return background;
            }
            const double binSize = (rangeMax - rangeMin) / kHistogramBins;
            background.additiveQuads.reserve(counts.size());
            for (int c = 0; c < 3; ++c) {
                for (int i = 0; i < kHistogramBins; ++i) {
                    const unsigned long long count = counts[c * kHistogramBins + i];
                    if (count == 0) {
                        continue;
                    }
                    const double minX = rangeMin + i * binSize;
                    const double maxX = minX + binSize;
                    const double height = (double)count / (double)peak;
                    const float* color = kHistogramColors[c];
                    ParametricBackgroundQuad quad;
                    quad.v[0] = ParametricBackgroundVertex(minX, 0., color[0], color[1], color[2]);
                    quad.v[1] = ParametricBackgroundVertex(minX, height, color[0], color[1], color[2]);
                    quad.v[2] = ParametricBackgroundVertex(maxX, height, color[0], color[1], color[2]);
                    quad.v[3] = ParametricBackgroundVertex(maxX, 0., color[0], color[1], color[2]);
                    background.additiveQuads.push_back(quad);
                }
            }
        }

        return background;
    });
    hasBackground->setDefaultValue(true);
}

void
ColorLookup::dropHistogram()
{
    {
        std::lock_guard<std::mutex> lock(_background->mutex);
        std::vector<unsigned long long>().swap(_background->counts);
    }
    if (KnobParametricPtr table = _lookupTable.lock()) {
        table->notifyBackgroundChanged();
    }
}

std::vector<unsigned long long>
ColorLookup::getHistogramCounts() const
{
    std::lock_guard<std::mutex> lock(_background->mutex);

    return _background->counts;
}

void
ColorLookup::updateHistogram(double time,
                             ViewIdx view)
{
    KnobParametricPtr table = _lookupTable.lock();
    KnobDoublePtr range = _range.lock();

    if (!table || !range || !getInput(0)) {
        return;
    }
    RectI roi;
    ImagePtr src = getImage(0, time, getOverlayInteractRenderScale(), view, NULL, NULL, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return;
    }
    const int nComps = (int)src->getComponentsCount();
    if ((nComps < 1) || (nComps > 4)) {
        return;
    }
    const int nHistComps = (std::min)(nComps, 3);
    const double rangeMin = range->getValueAtTime(time, 0, view);
    const double rangeMax = range->getValueAtTime(time, 1, view);
    if (!(rangeMax > rangeMin)) {
        return;
    }

    std::vector<unsigned long long> counts(3 * (std::size_t)kHistogramBins, 0);
    const RectI& bounds = src->getBounds();
    {
        Image::ReadAccess access(src.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const float* pix = (const float*)access.pixelAt(bounds.x1, y);
            if (!pix) {
                continue;
            }
            for (int x = bounds.x1; x < bounds.x2; ++x, pix += nComps) {
                for (int c = 0; c < nHistComps; ++c) {
                    const double v = pix[c];
                    int bin = 0;
                    if (v >= rangeMax) {
                        bin = kHistogramBins - 1;
                    } else if (v >= rangeMin) {
                        bin = (int)std::floor(kHistogramBins * (v - rangeMin) / (rangeMax - rangeMin));
                    }
                    ++counts[c * kHistogramBins + bin];
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(_background->mutex);
        _background->counts.swap(counts);
        _background->rangeMin = rangeMin;
        _background->rangeMax = rangeMax;
    }
    table->notifyBackgroundChanged();
}

void
ColorLookup::addControlPointFromButton(KnobI* button,
                                       double time,
                                       ViewIdx view)
{
    KnobParametricPtr table = _lookupTable.lock();

    if (!table) {
        return;
    }
    double source[4];
    double target[4];
    readColor(_source, time, view, source);
    readColor(_target, time, view, target);

    if (isKnob(button, _setMaster)) {
        const ColorMath::LuminanceMathEnum math = (ColorMath::LuminanceMathEnum)readChoice(_luminanceMath, time, view);
        StatusEnum stat = table->addControlPoint(eValueChangedReasonPluginEdited, eCurveMaster, ColorMath::luminance(math, source[0], source[1], source[2]), ColorMath::luminance(math, target[0], target[1], target[2]), eKeyframeTypeCubic);
        Q_UNUSED(stat);

        return;
    }
    const int cbegin = isKnob(button, _setA) ? 3 : 0;
    const int cend = isKnob(button, _setRGB) ? 3 : 4;
    for (int c = cbegin; c < cend; ++c) {
        StatusEnum stat = table->addControlPoint(eValueChangedReasonPluginEdited, eCurveRed + c, source[c], target[c], eKeyframeTypeCubic);
        Q_UNUSED(stat);
    }
}

bool
ColorLookup::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec view,
                         double time,
                         bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);

    if (isKnob(k, _updateHistogram)) {
        updateHistogram(time, viewIdx);

        return true;
    }
    if (isKnob(k, _hasBackgroundInteract) || isKnob(k, _display)) {
        refreshBackgroundControls();
        KnobChoicePtr display = _display.lock();
        if (display && isKnob(k, _display)) {
            if (display->getValue() != (int)eDisplayHistogram) {
                dropHistogram();
            } else if (reason == eValueChangedReasonUserEdited) {
                updateHistogram(time, viewIdx);
            } else if (KnobParametricPtr table = _lookupTable.lock()) {
                table->notifyBackgroundChanged();
            }
        } else if (KnobParametricPtr table = _lookupTable.lock()) {
            table->notifyBackgroundChanged();
        }

        return true;
    }
    if ((isKnob(k, _setMaster) || isKnob(k, _setRGB) || isKnob(k, _setRGBA) || isKnob(k, _setA))) {
        if (reason == eValueChangedReasonUserEdited) {
            addControlPointFromButton(k, time, viewIdx);
        }

        return true;
    }
    KnobDoublePtr range = _range.lock();
    if (range && (k == range.get()) && (reason == eValueChangedReasonUserEdited)) {
        const double rangeMin = range->getValueAtTime(time, 0, viewIdx);
        const double rangeMax = range->getValueAtTime(time, 1, viewIdx);
        if (rangeMax < rangeMin) {
            range->setValues(rangeMax, rangeMin, ViewSpec::all(), eValueChangedReasonPluginEdited);
        }

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
