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

#include "ColorCorrect.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Curve.h"
#include "Engine/Interpolation.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace {
// The tone LUT has this many intervals over `range`, as the OpenFX plugin's float path.
const int kToneLutIntervals = 1023;

struct GroupValues {
    double saturation[4];
    double contrast[4];
    double gamma[4];
    double gain[4];
    double offset[4];

    GroupValues()
    {
        setIdentity();
    }

    void setIdentity()
    {
        for (int i = 0; i < 4; ++i) {
            saturation[i] = 1.;
            contrast[i] = 1.;
            gamma[i] = 1.;
            gain[i] = 1.;
            offset[i] = 0.;
        }
    }

    bool isIdentity() const
    {
        for (int i = 0; i < 4; ++i) {
            if ((saturation[i] != 1.) || (contrast[i] != 1.) || (gamma[i] != 1.) || (gain[i] != 1.) || (offset[i] != 0.)) {
                return false;
            }
        }

        return true;
    }
};

struct PixelRGBA {
    double v[4];
};

// An immutable copy of a tone curve, evaluated without the Curve's mutex: render threads share
// one kernel, and pixels outside `range` evaluate the curve directly, once per pixel. The
// evaluation is Curve::getValueAt()'s for a non-periodic curve: the same segment selection and
// extrapolation, Interpolation::interpolate() and the clamp to the curve's Y range.
class ToneCurve {
public:
    ToneCurve()
        : _yMin(-std::numeric_limits<double>::infinity())
        , _yMax(std::numeric_limits<double>::infinity())
    {
    }

    explicit ToneCurve(const CurvePtr& curve)
        : _yMin(-std::numeric_limits<double>::infinity())
        , _yMax(std::numeric_limits<double>::infinity())
    {
        if (!curve) {
            return;
        }
        const KeyFrameSet keys = curve->getKeyFrames_mt_safe();
        _keys.assign(keys.begin(), keys.end());
        const Curve::YRange yRange = curve->getCurveYRange();
        _yMin = yRange.min;
        _yMax = yRange.max;
    }

    double valueAt(double t) const
    {
        if (_keys.empty()) {
            return 0.;
        }
        std::vector<KeyFrame>::const_iterator next = std::upper_bound(_keys.begin(), _keys.end(), t, isBefore);
        double tcur, vcur, vcurDerivRight, tnext, vnext, vnextDerivLeft;
        KeyframeTypeEnum interp, interpNext;
        if (next == _keys.begin()) {
            tnext = next->getTime();
            vnext = next->getValue();
            vnextDerivLeft = next->getLeftDerivative();
            interpNext = next->getInterpolation();
            tcur = tnext - 1.;
            vcur = vnext;
            vcurDerivRight = 0.;
            interp = eKeyframeTypeNone;
        } else if (next == _keys.end()) {
            const KeyFrame& last = _keys.back();
            tcur = last.getTime();
            vcur = last.getValue();
            vcurDerivRight = last.getRightDerivative();
            interp = last.getInterpolation();
            tnext = tcur + 1.;
            vnext = vcur;
            vnextDerivLeft = 0.;
            interpNext = eKeyframeTypeNone;
        } else {
            const KeyFrame& cur = *(next - 1);
            tcur = cur.getTime();
            vcur = cur.getValue();
            vcurDerivRight = cur.getRightDerivative();
            interp = cur.getInterpolation();
            tnext = next->getTime();
            vnext = next->getValue();
            vnextDerivLeft = next->getLeftDerivative();
            interpNext = next->getInterpolation();
        }
        const double v = Interpolation::interpolate(tcur, vcur, vcurDerivRight, vnextDerivLeft, tnext, vnext, t, interp, interpNext);
        if (v > _yMax) {
            return _yMax;
        } else if (v < _yMin) {
            return _yMin;
        }

        return v;
    }

private:
    static bool isBefore(double t,
                         const KeyFrame& key)
    {
        return t < key.getTime();
    }

    std::vector<KeyFrame> _keys;
    double _yMin;
    double _yMax;
};

class ColorCorrectKernel
    : public PixelKernel {
public:
    ColorCorrectKernel(const GroupValues groups[ColorCorrect::eGroupCount],
                       ColorMath::LuminanceMathEnum luminanceMath,
                       double rangeMin,
                       double rangeMax,
                       bool clampBlack,
                       bool clampWhite,
                       const ToneCurve curves[2])
        : _luminanceMath(luminanceMath)
        , _rangeMin((std::min)(rangeMin, rangeMax))
        , _rangeMax((std::max)(rangeMin, rangeMax))
        , _clampBlack(clampBlack)
        , _clampWhite(clampWhite)
    {
        for (int g = 0; g < ColorCorrect::eGroupCount; ++g) {
            _groups[g] = groups[g];
        }
        if (_rangeMin == _rangeMax) {
            _rangeMax = _rangeMin + 1.;
        }
        for (int curve = 0; curve < 2; ++curve) {
            _curves[curve] = curves[curve];
            _lut[curve].resize(kToneLutIntervals + 1);
            for (int position = 0; position <= kToneLutIntervals; ++position) {
                const double parametricPos = _rangeMin + (_rangeMax - _rangeMin) * double(position) / kToneLutIntervals;
                _lut[curve][position] = (float)clamp(curveValue(curve, parametricPos));
            }
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const int nComps = io.nComps;
        int bits[4] = { -1, -1, -1, -1 };
        bool process[4] = { false, false, false, false };

        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            bits[c] = pixelKernelChannelBit(nComps, c);
            process[bits[c]] = io.channels[bits[c]];
        }
        const float* src = io.src[0];
        float* dst = io.dst;
        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            // A pixel without alpha is opaque and one without colour black, as the OpenFX
            // plugin reads them, so the luminance of an alpha-only pixel is zero.
            PixelRGBA p;
            p.v[0] = p.v[1] = p.v[2] = 0.;
            p.v[3] = 1.;
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                p.v[bits[c]] = src[c];
            }

            const float l = static_cast<float>(ColorMath::luminance(_luminanceMath, p.v[0], p.v[1], p.v[2]));
            const double sScale = interpolate(0, l);
            const double hScale = interpolate(1, l);
            const double mScale = 1.f - sScale - hScale;

            PixelRGBA s = p;
            PixelRGBA m = p;
            PixelRGBA h = p;
            applyGroup(_groups[ColorCorrect::eGroupShadows], process, &s);
            applyGroup(_groups[ColorCorrect::eGroupMidtones], process, &m);
            applyGroup(_groups[ColorCorrect::eGroupHighlights], process, &h);
            for (int i = 0; i < 4; ++i) {
                if (process[i]) {
                    p.v[i] = s.v[i] * sScale + m.v[i] * mScale + h.v[i] * hScale;
                }
            }
            applyGroup(_groups[ColorCorrect::eGroupMaster], process, &p);

            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (process[bits[c]]) {
                    dst[c] = (float)clamp(p.v[bits[c]]);
                }
            }
        }
    }

private:
    double clamp(double value) const
    {
        if (_clampBlack && (value < 0.)) {
            return 0.;
        } else if (_clampWhite && (value > 1.)) {
            return 1.;
        }

        return value;
    }

    double curveValue(int curve,
                      double parametricPos) const
    {
        return _curves[curve].valueAt(parametricPos);
    }

    // The weight of tone curve `curve` at luminance `value`: interpolated in the LUT inside
    // `range`, the curve itself outside it. The float steps are the OpenFX plugin's.
    float interpolate(int curve,
                      float value) const
    {
        if ((value < _rangeMin) || (_rangeMax < value)) {
            return static_cast<float>(clamp(curveValue(curve, value)));
        }
        const double x = (value - _rangeMin) / (_rangeMax - _rangeMin);
        if (x <= 0.) {
            return _lut[curve][0];
        } else if (x >= 1.) {
            return _lut[curve][kToneLutIntervals];
        }
        int i = (int)(x * kToneLutIntervals);
        i = (std::max)(0, (std::min)(i, kToneLutIntervals - 1));
        const double alpha = (std::max)(0., (std::min)(x * kToneLutIntervals - i, 1.));
        const float a = _lut[curve][i];
        const float b = _lut[curve][i + 1];

        return static_cast<float>(a * (1.f - alpha) + b * alpha);
    }

    void applyGroup(const GroupValues& group,
                    const bool process[4],
                    PixelRGBA* p) const
    {
        applySaturation(group.saturation, process, p);
        for (int i = 0; i < 4; ++i) {
            if (!process[i]) {
                continue;
            }
            double v = p->v[i];
            if ((v > 0) && (group.contrast[i] != 1.)) {
                v = std::pow(v / 0.18, group.contrast[i]) * 0.18;
            }
            if ((v > 0) && (group.gamma[i] != 1.)) {
                v = std::pow(v, 1. / group.gamma[i]);
            }
            if (group.gain[i] != 1.) {
                v = v * group.gain[i];
            }
            if (group.offset[i] != 0.) {
                v = v + group.offset[i];
            }
            p->v[i] = v;
        }
    }

    void applySaturation(const double saturation[4],
                         const bool process[4],
                         PixelRGBA* p) const
    {
        bool any = false;

        for (int i = 0; i < 3; ++i) {
            any = any || (process[i] && (saturation[i] != 1.));
        }
        if (!any) {
            return;
        }
        // One luminance for all three channels, taken before any of them changes.
        const double l = ColorMath::luminance(_luminanceMath, p->v[0], p->v[1], p->v[2]);
        for (int i = 0; i < 3; ++i) {
            if (process[i] && (saturation[i] != 1.)) {
                p->v[i] = (1.f - saturation[i]) * l + saturation[i] * p->v[i];
            }
        }
    }

    GroupValues _groups[ColorCorrect::eGroupCount];
    ColorMath::LuminanceMathEnum _luminanceMath;
    double _rangeMin;
    double _rangeMax;
    bool _clampBlack;
    bool _clampWhite;
    ToneCurve _curves[2];
    std::vector<float> _lut[2];
};

NativeImageTraits
colorCorrectTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;
    traits.defaultChannels[3] = false;

    return traits;
}

KnobColorPtr
addScaleKnob(KnobHolder* holder,
             const KnobGroupPtr& group,
             const std::string& name,
             const std::string& label,
             double defaultValue,
             double displayMin,
             double displayMax)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);

    knob->setName(name);
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(defaultValue, i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(displayMin, i);
        knob->setDisplayMaximum(displayMax, i);
    }
    group->addKnob(knob);

    return knob;
}

void
readColor(const KnobColorWPtr& weak,
          double time,
          ViewIdx view,
          double defaultValue,
          double out[4])
{
    KnobColorPtr knob = weak.lock();

    for (int i = 0; i < 4; ++i) {
        out[i] = knob ? knob->getValueAtTime(time, i, view) : defaultValue;
    }
}

bool
readBool(const KnobBoolWPtr& weak,
         double time,
         ViewIdx view,
         bool defaultValue)
{
    KnobBoolPtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : defaultValue;
}
} // anonymous namespace

ColorCorrect::ColorCorrect(NodePtr node)
    : NativeImageEffect(node, colorCorrectTraits())
{
}

ColorCorrect::~ColorCorrect()
{
}

NativePluginDescription
ColorCorrect::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_COLORCORRECT;
    desc.label = "ColorCorrect";
    desc.description = tr("Adjusts the saturation, contrast, gamma, gain and offset of an image.\n"
                          "The ranges of the shadows, midtones and highlights are controlled by the curves "
                          "in the \"Ranges\" tab.\n"
                          "The Contrast adjustment works using the formula: Output = (Input/0.18)^Contrast*0.18.\n"
                          "\n"
                          "See also:\n"
                          "- http://opticalenquiry.com/nuke/index.php?title=ColorCorrect\n"
                          "- https://compositormathematic.wordpress.com/2013/07/06/gamma-contrast/")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_COLOR;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_COLORCORRECT;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
ColorCorrect::addGroup(const KnobPagePtr& page,
                       GroupEnum groupIndex,
                       const std::string& name,
                       bool open)
{
    KnobGroupPtr group = createKnob<KnobGroup>(name);

    group->setName(name);
    group->setDefaultValue(open);
    page->addKnob(group);

    GroupKnobs& knobs = _groups[groupIndex];
    if (groupIndex != eGroupMaster) {
        KnobBoolPtr enable = createKnob<KnobBool>(std::string(kColorCorrectParamEnable));
        enable->setName(name + kColorCorrectParamEnable);
        enable->setHintToolTip(tr("When checked, %1 correction is enabled.").arg(QString::fromUtf8(name.c_str())));
        enable->setDefaultValue(true);
        group->addKnob(enable);
        knobs.enable = enable;
    }
    knobs.saturation = addScaleKnob(this, group, name + kColorCorrectParamSaturation, kColorCorrectParamSaturation, 1., 0., 4.);
    knobs.contrast = addScaleKnob(this, group, name + kColorCorrectParamContrast, kColorCorrectParamContrast, 1., 0., 4.);
    knobs.gamma = addScaleKnob(this, group, name + kColorCorrectParamGamma, kColorCorrectParamGamma, 1., 0.2, 5.);
    knobs.gain = addScaleKnob(this, group, name + kColorCorrectParamGain, kColorCorrectParamGain, 1., 0., 4.);
    knobs.offset = addScaleKnob(this, group, name + kColorCorrectParamOffset, kColorCorrectParamOffset, 0., -1., 1.);
}

void
ColorCorrect::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    addGroup(page, eGroupMaster, kColorCorrectGroupMaster, true);
    addGroup(page, eGroupShadows, kColorCorrectGroupShadows, false);
    addGroup(page, eGroupMidtones, kColorCorrectGroupMidtones, false);
    addGroup(page, eGroupHighlights, kColorCorrectGroupHighlights, false);

    KnobPagePtr ranges = createKnob<KnobPage>(tr("Ranges"));
    {
        KnobDoublePtr range = createKnob<KnobDouble>(tr("Range"), 2);
        range->setName(kColorCorrectParamRange);
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
        ranges->addKnob(range);
        _range = range;
    }
    {
        KnobParametricPtr toneRanges = createKnob<KnobParametric>(tr("Tone Ranges"), 2);
        toneRanges->setName(kColorCorrectParamToneRanges);
        toneRanges->setHintToolTip(tr("Tone ranges lookup table"));
        toneRanges->setDimensionName(0, "Shadow");
        toneRanges->setDimensionName(1, "Highlight");
        toneRanges->setCurveColor(0, 0.6, 0.4, 0.6);
        toneRanges->setCurveColor(1, 0.8, 0.7, 0.6);
        toneRanges->setParametricRange(0., 1.);
        toneRanges->setDisplayMinimumsAndMaximums(std::vector<double>(2, 0.), std::vector<double>(2, 1.));
        // Horizontal keys are what the OpenFX host gives this plugin's default points, so the
        // default shapes are the same smoothstep ramps.
        const double points[2][2][2] = { { { 0., 1. }, { 0.09, 0. } }, { { 0.5, 0. }, { 1., 1. } } };
        for (int curve = 0; curve < 2; ++curve) {
            for (int p = 0; p < 2; ++p) {
                StatusEnum stat = toneRanges->addControlPoint(eValueChangedReasonPluginEdited, curve, points[curve][p][0], points[curve][p][1], eKeyframeTypeHorizontal);
                Q_UNUSED(stat);
            }
        }
        toneRanges->setDefaultCurvesFromCurves();
        ranges->addKnob(toneRanges);
        _toneRanges = toneRanges;
    }

    _luminanceMath = ColorMath::addLuminanceMathKnob(this, page);
    if (KnobChoicePtr luminanceMath = _luminanceMath.lock()) {
        luminanceMath->setHintToolTip(tr("Formula used to compute luminance from RGB values (used for saturation adjustments)."));
    }

    KnobBoolPtr clampBlack = createKnob<KnobBool>(tr("Clamp Black"));
    clampBlack->setName(kColorCorrectParamClampBlack);
    clampBlack->setHintToolTip(tr("All colors below 0 on output are set to 0."));
    clampBlack->setDefaultValue(true);
    clampBlack->setAddNewLine(false);
    page->addKnob(clampBlack);
    _clampBlack = clampBlack;

    KnobBoolPtr clampWhite = createKnob<KnobBool>(tr("Clamp White"));
    clampWhite->setName(kColorCorrectParamClampWhite);
    clampWhite->setHintToolTip(tr("All colors above 1 on output are set to 1."));
    clampWhite->setDefaultValue(false);
    page->addKnob(clampWhite);
    _clampWhite = clampWhite;

    addMaskMixKnobs(page);
} // ColorCorrect::initializeKnobs

PixelKernelPtr
ColorCorrect::makeKernel(const KernelContext& context)
{
    GroupValues groups[eGroupCount];

    for (int g = 0; g < eGroupCount; ++g) {
        const GroupKnobs& knobs = _groups[g];
        if ((g != eGroupMaster) && !readBool(knobs.enable, context.time, context.view, true)) {
            continue;
        }
        readColor(knobs.saturation, context.time, context.view, 1., groups[g].saturation);
        readColor(knobs.contrast, context.time, context.view, 1., groups[g].contrast);
        readColor(knobs.gamma, context.time, context.view, 1., groups[g].gamma);
        readColor(knobs.gain, context.time, context.view, 1., groups[g].gain);
        readColor(knobs.offset, context.time, context.view, 0., groups[g].offset);
    }

    double rangeMin = 0.;
    double rangeMax = 1.;
    if (KnobDoublePtr range = _range.lock()) {
        rangeMin = range->getValueAtTime(context.time, 0, context.view);
        rangeMax = range->getValueAtTime(context.time, 1, context.view);
    }

    ColorMath::LuminanceMathEnum luminanceMath = ColorMath::eLuminanceMathRec709;
    if (KnobChoicePtr choice = _luminanceMath.lock()) {
        luminanceMath = (ColorMath::LuminanceMathEnum)choice->getValueAtTime(context.time, 0, context.view);
    }

    ToneCurve curves[2];
    if (KnobParametricPtr toneRanges = _toneRanges.lock()) {
        for (int curve = 0; curve < 2; ++curve) {
            curves[curve] = ToneCurve(toneRanges->getParametricCurve(curve));
        }
    }

    return std::make_shared<ColorCorrectKernel>(groups,
                                                luminanceMath,
                                                rangeMin,
                                                rangeMax,
                                                readBool(_clampBlack, context.time, context.view, true),
                                                readBool(_clampWhite, context.time, context.view, false),
                                                curves);
}

bool
ColorCorrect::isIdentityOp(double time,
                           const RenderScale& /*scale*/,
                           const RectI& /*roi*/,
                           ViewIdx view)
{
    if (readBool(_clampBlack, time, view, true) || readBool(_clampWhite, time, view, false)) {
        return false;
    }
    for (int g = 0; g < eGroupCount; ++g) {
        const GroupKnobs& knobs = _groups[g];
        if ((g != eGroupMaster) && !readBool(knobs.enable, time, view, true)) {
            continue;
        }
        GroupValues values;
        readColor(knobs.saturation, time, view, 1., values.saturation);
        readColor(knobs.contrast, time, view, 1., values.contrast);
        readColor(knobs.gamma, time, view, 1., values.gamma);
        readColor(knobs.gain, time, view, 1., values.gain);
        readColor(knobs.offset, time, view, 0., values.offset);
        if (!values.isIdentity()) {
            return false;
        }
    }

    return true;
}

bool
ColorCorrect::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec view,
                          double time,
                          bool /*originatedFromMainThread*/)
{
    KnobDoublePtr range = _range.lock();

    if (!range || (k != range.get()) || (reason != eValueChangedReasonUserEdited)) {
        return false;
    }
    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);
    const double rangeMin = range->getValueAtTime(time, 0, viewIdx);
    const double rangeMax = range->getValueAtTime(time, 1, viewIdx);
    if (rangeMax < rangeMin) {
        range->setValues(rangeMax, rangeMin, ViewSpec::all(), eValueChangedReasonPluginEdited);
    }

    return true;
}

NATRON_NAMESPACE_EXIT
