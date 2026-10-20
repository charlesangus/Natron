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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Interpolation.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderScale.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

struct ColorCorrectOp::Tones {
    CurveSnapshot curves[2];
    std::vector<float> lut[2];
#ifdef NATRON_HAVE_GPU
    // Both curves as curvesnapshot.slang reads them, one after the other, or empty when either
    // has more keys than it holds.
    std::vector<std::uint32_t> gpuCurves;
#endif
};

namespace {
// The tone LUT has this many intervals over [kToneLutMin, kToneLutMax]; a tone curve is evaluated
// directly outside it. The unit interval is where the tone curves vary, so the table keeps its
// resolution there however far the scene-linear values run.
const int kToneLutIntervals = 1023;
const double kToneLutMin = 0.;
const double kToneLutMax = 1.;

#ifdef NATRON_HAVE_GPU
// The parameter block of ColorCorrectOp in colorcorrect.slang, in words.
const int kGpuFlags = 0;
const int kGpuWeights = 1;
const int kGpuTableRange = 4;
const int kGpuKeyCount = 10;
const int kGpuYMin = 12;
const int kGpuYMax = 14;
const int kGpuGroups = 16;
const int kGpuGroupWords = 20;
const int kGpuHeaderWords = kGpuGroups + ColorCorrectOp::eGroupCount * kGpuGroupWords;
const int kGpuLutSize = kToneLutIntervals + 1;
const int kGpuLutBase = kGpuHeaderWords;
const int kGpuCurveBase = kGpuLutBase + 2 * kGpuLutSize;
const int kGpuTotalWords = kGpuCurveBase + 2 * CurveSnapshot::kGpuWords;

const std::uint32_t kGpuClampBlack = 1;
const std::uint32_t kGpuClampWhite = 2;
const std::uint32_t kGpuLumaMax = 4;
const int kGpuActiveShift = 4;

static_assert(kGpuHeaderWords == 96, "must match CC_HEADER_WORDS in colorcorrect.slang");
static_assert(kToneLutIntervals + 1 == 1024, "must match CS_TABLE_SIZE in curvesnapshot.slang");
static_assert(kGpuTableRange + CurveSnapshot::kGpuTableRangeWords <= kGpuKeyCount, "the table range lies before the key counts");

void
putFloat(std::vector<std::uint32_t>& words,
         int index,
         float value)
{
    std::memcpy(&words[index], &value, sizeof(float));
}
#endif // NATRON_HAVE_GPU

struct PixelRGBA {
    double v[4];
};

// The tone weights stay in [0, 1] whatever the output clamps are, or the three groups would
// blend with weights that do not sum to one.
double
toneClamp(double value)
{
    if (value < 0.) {
        return 0.;
    } else if (value > 1.) {
        return 1.;
    }

    return value;
}

// The weight of tone curve `curve` at luminance `value`: interpolated in the LUT inside
// the table's domain, the curve itself outside it.
ALWAYS_INLINE float
toneWeight(const ColorCorrectOp::Tones& tones,
           int curve,
           float value)
{
    if ((value < kToneLutMin) || (kToneLutMax < value)) {
        return static_cast<float>(toneClamp(tones.curves[curve].valueAt(value)));
    }
    const std::vector<float>& lut = tones.lut[curve];
    const double x = (value - kToneLutMin) / (kToneLutMax - kToneLutMin);
    if (x <= 0.) {
        return lut[0];
    } else if (x >= 1.) {
        return lut[kToneLutIntervals];
    }
    int i = (int)(x * kToneLutIntervals);
    i = (std::max)(0, (std::min)(i, kToneLutIntervals - 1));
    const double alpha = (std::max)(0., (std::min)(x * kToneLutIntervals - i, 1.));
    const float a = lut[i];
    const float b = lut[i + 1];

    return static_cast<float>(a * (1.f - alpha) + b * alpha);
}

double
clampOutput(const ColorCorrectOp::Params& params,
            double value)
{
    if (params.clampBlack && (value < 0.)) {
        return 0.;
    } else if (params.clampWhite && (value > 1.)) {
        return 1.;
    }

    return value;
}

void
applySaturation(ColorMath::LuminanceMathEnum luminanceMath,
                const double saturation[4],
                const bool process[4],
                PixelRGBA* p)
{
    bool any = false;

    for (int i = 0; i < 3; ++i) {
        any = any || (process[i] && (saturation[i] != 1.));
    }
    if (!any) {
        return;
    }
    // One luminance for all three channels, taken before any of them changes.
    const double l = ColorMath::luminance(luminanceMath, p->v[0], p->v[1], p->v[2]);
    for (int i = 0; i < 3; ++i) {
        if (process[i] && (saturation[i] != 1.)) {
            p->v[i] = (1.f - saturation[i]) * l + saturation[i] * p->v[i];
        }
    }
}

void
applyGroup(const ColorCorrectOp::Params& params,
           int groupIndex,
           const bool process[4],
           PixelRGBA* p)
{
    const ColorCorrectOp::Group& group = params.groups[groupIndex];
    applySaturation(params.luminanceMath, group.saturation, process, p);
    for (int i = 0; i < 4; ++i) {
        if (!process[i]) {
            continue;
        }
        double v = p->v[i];
        if ((v > 0) && (group.contrast[i] != 1.)) {
            v = std::pow(v / 0.18, group.contrast[i]) * 0.18;
        }
        if ((v > 0) && (group.gamma[i] != 1.)) {
            v = std::pow(v, group.invGamma[i]);
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

bool
isIdentityGroup(const ColorCorrectOp::Group& group)
{
    for (int i = 0; i < 4; ++i) {
        if ((group.saturation[i] != 1.) || (group.contrast[i] != 1.) || (group.gamma[i] != 1.) || (group.gain[i] != 1.) || (group.offset[i] != 0.)) {
            return false;
        }
    }

    return true;
}

KnobColorPtr
addScaleKnob(KnobHolder* holder,
             const KnobGroupPtr& group,
             const std::string& name,
             const std::string& label,
             double defaultValue,
             double displayMin,
             double displayMax,
             ColorKnobRoleEnum role)
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
    knob->setColorRole(role);
    group->addKnob(knob);

    return knob;
}

ColorCorrectOp::GroupKnobs
addGroup(KnobHolder* holder,
         const KnobPagePtr& page,
         ColorCorrectOp::GroupEnum groupIndex,
         const std::string& name,
         bool open)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(holder, name);

    group->setName(name);
    group->setDefaultValue(open);
    page->addKnob(group);

    ColorCorrectOp::GroupKnobs knobs;
    if (groupIndex != ColorCorrectOp::eGroupMaster) {
        KnobBoolPtr enable = AppManager::createKnob<KnobBool>(holder, std::string(kColorCorrectParamEnable));
        enable->setName(name + kColorCorrectParamEnable);
        enable->setHintToolTip(EffectInstance::tr("When checked, %1 correction is enabled.").arg(QString::fromUtf8(name.c_str())));
        enable->setDefaultValue(true);
        group->addKnob(enable);
        knobs.enable = enable;
    }
    knobs.saturation = addScaleKnob(holder, group, name + kColorCorrectParamSaturation, kColorCorrectParamSaturation, 1., 0., 4., eColorKnobRoleGain);
    knobs.contrast = addScaleKnob(holder, group, name + kColorCorrectParamContrast, kColorCorrectParamContrast, 1., 0., 4., eColorKnobRoleGain);
    knobs.gamma = addScaleKnob(holder, group, name + kColorCorrectParamGamma, kColorCorrectParamGamma, 1., 0.2, 5., eColorKnobRolePower);
    knobs.gain = addScaleKnob(holder, group, name + kColorCorrectParamGain, kColorCorrectParamGain, 1., 0., 4., eColorKnobRoleGain);
    knobs.offset = addScaleKnob(holder, group, name + kColorCorrectParamOffset, kColorCorrectParamOffset, 0., -1., 1., eColorKnobRoleOffset);

    return knobs;
}

ColorCorrectOp::Tones
buildTones(const KnobParametricPtr& toneRanges)
{
    ColorCorrectOp::Tones tones;

    for (int curve = 0; curve < 2; ++curve) {
        if (toneRanges) {
            tones.curves[curve] = CurveSnapshot(toneRanges->getParametricCurve(curve));
        }
        tones.lut[curve].resize(kToneLutIntervals + 1);
        for (int position = 0; position <= kToneLutIntervals; ++position) {
            const double parametricPos = kToneLutMin + (kToneLutMax - kToneLutMin) * double(position) / kToneLutIntervals;
            tones.lut[curve][position] = (float)toneClamp(tones.curves[curve].valueAt(parametricPos));
        }
    }
#ifdef NATRON_HAVE_GPU
    tones.gpuCurves.assign(2 * CurveSnapshot::kGpuWords, 0);
    for (int curve = 0; curve < 2; ++curve) {
        if (!tones.curves[curve].packGpu(&tones.gpuCurves[curve * CurveSnapshot::kGpuWords])) {
            tones.gpuCurves.clear();
            break;
        }
    }
#endif

    return tones;
}
} // anonymous namespace

void
ColorCorrectOp::createKnobs(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    groups[eGroupMaster] = addGroup(holder, page, eGroupMaster, kColorCorrectGroupMaster, true);
    groups[eGroupShadows] = addGroup(holder, page, eGroupShadows, kColorCorrectGroupShadows, false);
    groups[eGroupMidtones] = addGroup(holder, page, eGroupMidtones, kColorCorrectGroupMidtones, false);
    groups[eGroupHighlights] = addGroup(holder, page, eGroupHighlights, kColorCorrectGroupHighlights, false);

    KnobPagePtr ranges = AppManager::createKnob<KnobPage>(holder, EffectInstance::tr("Ranges"));
    {
        KnobParametricPtr toneRangesKnob = AppManager::createKnob<KnobParametric>(holder, EffectInstance::tr("Tone Ranges"), 2);
        toneRangesKnob->setName(kColorCorrectParamToneRanges);
        toneRangesKnob->setHintToolTip(EffectInstance::tr("Tone ranges lookup table"));
        toneRangesKnob->setDimensionName(0, "Shadow");
        toneRangesKnob->setDimensionName(1, "Highlight");
        toneRangesKnob->setCurveColor(0, 0.6, 0.4, 0.6);
        toneRangesKnob->setCurveColor(1, 0.8, 0.7, 0.6);
        toneRangesKnob->setParametricRange(0., 1.);
        toneRangesKnob->setDisplayMinimumsAndMaximums(std::vector<double>(2, 0.), std::vector<double>(2, 1.));
        // Horizontal keys are what the OpenFX host gives this plugin's default points, so the
        // default shapes are the same smoothstep ramps.
        const double points[2][2][2] = { { { 0., 1. }, { 0.09, 0. } }, { { 0.5, 0. }, { 1., 1. } } };
        for (int curve = 0; curve < 2; ++curve) {
            for (int p = 0; p < 2; ++p) {
                StatusEnum stat = toneRangesKnob->addControlPoint(eValueChangedReasonPluginEdited, curve, points[curve][p][0], points[curve][p][1], eKeyframeTypeHorizontal);
                Q_UNUSED(stat);
            }
        }
        toneRangesKnob->setDefaultCurvesFromCurves();
        ranges->addKnob(toneRangesKnob);
        toneRanges = toneRangesKnob;
    }

    KnobChoicePtr luminanceMathKnob = ColorMath::addLuminanceMathKnob(holder, page);
    luminanceMathKnob->setHintToolTip(EffectInstance::tr("Formula used to compute luminance from RGB values (used for saturation adjustments)."));
    luminanceMath = luminanceMathKnob;

    KnobBoolPtr clampBlackKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp Black"));
    clampBlackKnob->setName(kColorCorrectParamClampBlack);
    clampBlackKnob->setHintToolTip(EffectInstance::tr("All colors below 0 on output are set to 0."));
    clampBlackKnob->setDefaultValue(false);
    clampBlackKnob->setAddNewLine(false);
    page->addKnob(clampBlackKnob);
    clampBlack = clampBlackKnob;

    KnobBoolPtr clampWhiteKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp White"));
    clampWhiteKnob->setName(kColorCorrectParamClampWhite);
    clampWhiteKnob->setHintToolTip(EffectInstance::tr("All colors above 1 on output are set to 1."));
    clampWhiteKnob->setDefaultValue(false);
    page->addKnob(clampWhiteKnob);
    clampWhite = clampWhiteKnob;
} // ColorCorrectOp::createKnobs

std::shared_ptr<const ColorCorrectOp::Tones>
ColorCorrectOp::bakeTones() const
{
    const KnobParametricPtr knob = toneRanges.lock();
    const auto build = [&knob]() {
        return buildTones(knob);
    };

    if (!knob) {
        return std::make_shared<const Tones>(build());
    }

    return _tones.get(*knob, build);
}

ColorCorrectOp::Params
ColorCorrectOp::bake(const KnobReader& reader) const
{
    Params p;

    for (int g = 0; g < eGroupCount; ++g) {
        Group& group = p.groups[g];
        for (int i = 0; i < 4; ++i) {
            group.saturation[i] = 1.;
            group.contrast[i] = 1.;
            group.gamma[i] = 1.;
            group.gain[i] = 1.;
            group.offset[i] = 0.;
        }
        const GroupKnobs& knobs = groups[g];
        if ((g == eGroupMaster) || reader.value(knobs.enable)) {
            for (int i = 0; i < 4; ++i) {
                group.saturation[i] = reader.value(knobs.saturation, i);
                group.contrast[i] = reader.value(knobs.contrast, i);
                group.gamma[i] = reader.value(knobs.gamma, i);
                group.gain[i] = reader.value(knobs.gain, i);
                group.offset[i] = reader.value(knobs.offset, i);
            }
        }
        for (int i = 0; i < 4; ++i) {
            group.invGamma[i] = 1. / group.gamma[i];
        }
        group.active = !isIdentityGroup(group);
    }
    p.luminanceMath = (ColorMath::LuminanceMathEnum)reader.value(luminanceMath);
    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);
    p.tones = bakeTones();

    return p;
}

bool
ColorCorrectOp::isIdentity(const Params& params) const
{
    if (params.clampBlack || params.clampWhite) {
        return false;
    }
    for (int g = 0; g < eGroupCount; ++g) {
        if (params.groups[g].active) {
            return false;
        }
    }

    return true;
}

void
ColorCorrectOp::row(const Params& params,
                    const RowIO& io) const
{
    const Tones& tones = *params.tones;
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

        const float l = static_cast<float>(ColorMath::luminance(params.luminanceMath, p.v[0], p.v[1], p.v[2]));
        const double sScale = toneWeight(tones, 0, l);
        const double hScale = toneWeight(tones, 1, l);
        const double mScale = 1.f - sScale - hScale;

        // The blend runs even when every tone group is an identity: its weights do not sum to
        // exactly one in floating point.
        PixelRGBA s = p;
        PixelRGBA m = p;
        PixelRGBA h = p;
        if (params.groups[eGroupShadows].active) {
            applyGroup(params, eGroupShadows, process, &s);
        }
        if (params.groups[eGroupMidtones].active) {
            applyGroup(params, eGroupMidtones, process, &m);
        }
        if (params.groups[eGroupHighlights].active) {
            applyGroup(params, eGroupHighlights, process, &h);
        }
        for (int i = 0; i < 4; ++i) {
            if (process[i]) {
                p.v[i] = s.v[i] * sScale + m.v[i] * mScale + h.v[i] * hScale;
            }
        }
        if (params.groups[eGroupMaster].active) {
            applyGroup(params, eGroupMaster, process, &p);
        }

        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (process[bits[c]]) {
                dst[c] = (float)clampOutput(params, p.v[bits[c]]);
            }
        }
    }
} // ColorCorrectOp::row

GpuOpRef
ColorCorrectOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    if (!params.tones || params.tones->gpuCurves.empty()) {
        return GpuOpRef::none();
    }
    const Tones& tones = *params.tones;
    std::vector<std::uint32_t> words(kGpuTotalWords, 0);
    std::uint32_t flags = (params.clampBlack ? kGpuClampBlack : 0) | (params.clampWhite ? kGpuClampWhite : 0) | ((params.luminanceMath == ColorMath::eLuminanceMathMax) ? kGpuLumaMax : 0);
    for (int g = 0; g < eGroupCount; ++g) {
        if (params.groups[g].active) {
            flags |= 1u << (kGpuActiveShift + g);
        }
    }
    words[kGpuFlags] = flags;
    // The luminance of the unit colours, from the function the CPU row calls.
    putFloat(words, kGpuWeights, (float)ColorMath::luminance(params.luminanceMath, 1., 0., 0.));
    putFloat(words, kGpuWeights + 1, (float)ColorMath::luminance(params.luminanceMath, 0., 1., 0.));
    putFloat(words, kGpuWeights + 2, (float)ColorMath::luminance(params.luminanceMath, 0., 0., 1.));
    CurveSnapshot::packGpuTableRange(kToneLutMin, kToneLutMax, kToneLutIntervals, &words[kGpuTableRange]);
    // Gamma travels as the reciprocal the CPU raises to.
    for (int g = 0; g < eGroupCount; ++g) {
        const Group& group = params.groups[g];
        const int base = kGpuGroups + g * kGpuGroupWords;
        for (int i = 0; i < 4; ++i) {
            putFloat(words, base + i, (float)group.saturation[i]);
            putFloat(words, base + 4 + i, (float)group.contrast[i]);
            putFloat(words, base + 8 + i, (float)group.invGamma[i]);
            putFloat(words, base + 12 + i, (float)group.gain[i]);
            putFloat(words, base + 16 + i, (float)group.offset[i]);
        }
    }
    std::copy(tones.gpuCurves.begin(), tones.gpuCurves.end(), words.begin() + kGpuCurveBase);
    for (int curve = 0; curve < 2; ++curve) {
        const CurveSnapshot& snapshot = tones.curves[curve];
        words[kGpuKeyCount + curve] = (std::uint32_t)snapshot.keys().size();
        putFloat(words, kGpuYMin + curve, (float)snapshot.yMin());
        putFloat(words, kGpuYMax + curve, (float)snapshot.yMax());
        for (int i = 0; i < kGpuLutSize; ++i) {
            putFloat(words, kGpuLutBase + curve * kGpuLutSize + i, tones.lut[curve][i]);
        }
    }

    return GpuOpRef::packed((int)gpu::GpuKernelId::colorcorrect_wrapped, std::move(words));
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
} // ColorCorrectOp::gpu

ColorCorrect::ColorCorrect(NodePtr node)
    : ImageNode<ColorCorrectOp>(node)
{
}

ColorCorrect::~ColorCorrect()
{
}

ColorCorrectOp::Params
ColorCorrect::bakeColorCorrect(double time,
                               ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
ColorCorrect::colorCorrectRow(const ColorCorrectOp::Params& params,
                              const RowIO& io) const
{
    op().row(params, io);
}

NATRON_NAMESPACE_EXIT
