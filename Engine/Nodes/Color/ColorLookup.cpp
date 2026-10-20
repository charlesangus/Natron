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
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/Interpolation.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

struct ColorLookupOp::Tables {
    CurveSnapshot curves[eCurveCount];
    // Empty for the master curve in the standard modes, which fold it into the channel tables.
    std::vector<float> lut[eCurveCount];
#ifdef NATRON_HAVE_GPU
    // Every curve as curvesnapshot.slang reads them, one after the other, or empty when any has
    // more keys than it holds.
    std::vector<std::uint32_t> gpuCurves;
#endif
};

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

// The lookup table has this many intervals over [kLutMin, kLutMax]; a curve is evaluated directly
// outside it. The unit interval is the curve editor's domain, so the table keeps its resolution
// where the curves are edited however far the scene-linear values run. The histogram and the
// colour ramp span the same interval.
const int kLutIntervals = 1023;
const double kLutMin = 0.;
const double kLutMax = 1.;

#ifdef NATRON_HAVE_GPU
// The parameter block of ColorLookupOp in colorlookup.slang, in words.
const int kGpuLutSize = kLutIntervals + 1;
const int kGpuCurveWords = CurveSnapshot::kGpuWords;
const int kGpuHeaderWords = 32;
const int kGpuYMin = 16;
const int kGpuYMax = 21;
const int kGpuKeyCount = 8;
const int kGpuTableRange = 26;
const int kGpuLutBase = kGpuHeaderWords;
const int kGpuCurveBase = kGpuLutBase + ColorLookupOp::eCurveCount * kGpuLutSize;
const int kGpuTotalWords = kGpuCurveBase + ColorLookupOp::eCurveCount * kGpuCurveWords;

static_assert(kLutIntervals + 1 == 1024, "must match CS_TABLE_SIZE in curvesnapshot.slang");
static_assert(kGpuTableRange + CurveSnapshot::kGpuTableRangeWords <= kGpuHeaderWords, "the table range lies in the header");

void
putFloat(std::vector<std::uint32_t>& words,
         int index,
         float value)
{
    std::memcpy(&words[index], &value, sizeof(float));
}
#endif // NATRON_HAVE_GPU

int
curveOfChannel(int channelBit)
{
    return ColorLookupOp::eCurveRed + channelBit;
}

// The op's maths over one set of Params and their tables.
struct LookupMaths {
    const ColorLookupOp::Tables& tables;
    ColorLookupOp::MasterCurveModeEnum mode;
    ColorMath::LuminanceMathEnum luminanceMath;
    bool clampBlack;
    bool clampWhite;

    void processRow(const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;

        if (nComps == 1) {
            const bool process = io.channels[3];
            for (int x = 0; x < io.width; ++x, ++src, ++dst) {
                if (process) {
                    *dst = interpolate(ColorLookupOp::eCurveAlpha, *src);
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
                dst[3] = interpolate(ColorLookupOp::eCurveAlpha, src[3]);
            }
        }
    }

    bool isStandard() const
    {
        return (mode == ColorLookupOp::eMasterCurveModeStandard) || (mode == ColorLookupOp::eMasterCurveModeWeightedStandard);
    }

    double clamp(double value) const
    {
        if (clampBlack && (value < 0.)) {
            return 0.;
        } else if (clampWhite && (value > 1.)) {
            return 1.;
        }

        return value;
    }

    float clamp(float value) const
    {
        if (clampBlack && (value < 0.f)) {
            return 0.f;
        } else if (clampWhite && (value > 1.f)) {
            return 1.f;
        }

        return value;
    }

    // A channel curve at pos, with the master curve folded in for the modes that do so.
    double curveValue(int curve,
                      double pos) const
    {
        double value = tables.curves[curve].valueAt(pos);

        if (isStandard() && (curve >= ColorLookupOp::eCurveRed) && (curve <= ColorLookupOp::eCurveBlue)) {
            value += tables.curves[ColorLookupOp::eCurveMaster].valueAt(pos) - pos;
        }

        return value;
    }

    // Interpolated in the table inside its domain, evaluated directly outside it.
    float interpolate(int curve,
                      float value) const
    {
        if ((value < kLutMin) || (kLutMax < value)) {
            return static_cast<float>(clamp(curveValue(curve, value)));
        }
        const std::vector<float>& lut = tables.lut[curve];
        const double x = (value - kLutMin) / (kLutMax - kLutMin);
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
            result = (float)clamp(result + (interpolate(ColorLookupOp::eCurveMaster, value) - value));
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
            } else if (a >= 1.f) {
                // No anchor above a to interpolate toward: shift b by the move of a.
                b1 = b + a2;
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

        r = interpolate(ColorLookupOp::eCurveMaster, rold);
        b = interpolate(ColorLookupOp::eCurveMaster, bold);
        g = b + ((r - b) * (gold - bold) / (rold - bold));
    }

    void rgb(float r,
             float g,
             float b,
             float out[3]) const
    {
        switch (mode) {
        case ColorLookupOp::eMasterCurveModeStandard:
            out[0] = interpolate(ColorLookupOp::eCurveRed, r);
            out[1] = interpolate(ColorLookupOp::eCurveGreen, g);
            out[2] = interpolate(ColorLookupOp::eCurveBlue, b);
            break;
        case ColorLookupOp::eMasterCurveModeWeightedStandard: {
            const float r1 = interpolate(ColorLookupOp::eCurveRed, r);
            const float g1 = triangle(r, r1, g);
            const float b1 = triangle(r, r1, b);

            const float g2 = interpolate(ColorLookupOp::eCurveGreen, g);
            const float r2 = triangle(g, g2, r);
            const float b2 = triangle(g, g2, b);

            const float b3 = interpolate(ColorLookupOp::eCurveBlue, b);
            const float r3 = triangle(b, b3, r);
            const float g3 = triangle(b, b3, g);

            out[0] = clamp(r1 * 0.50f + r2 * 0.25f + r3 * 0.25f);
            out[1] = clamp(g1 * 0.25f + g2 * 0.50f + g3 * 0.25f);
            out[2] = clamp(b1 * 0.25f + b2 * 0.25f + b3 * 0.50f);
            break;
        }
        case ColorLookupOp::eMasterCurveModeFilmLike: {
            const double rcoef = r < 1e-8 ? 1. : (interpolate(ColorLookupOp::eCurveRed, r) / r);
            const double gcoef = g < 1e-8 ? 1. : (interpolate(ColorLookupOp::eCurveGreen, g) / g);
            const double bcoef = b < 1e-8 ? 1. : (interpolate(ColorLookupOp::eCurveBlue, b) / b);
            if (r >= g) {
                if (g > b) {
                    rgbTone(r, g, b);
                } else if (b > r) {
                    rgbTone(b, r, g);
                } else if (b > g) {
                    rgbTone(r, b, g);
                } else {
                    r = interpolate(ColorLookupOp::eCurveMaster, r);
                    g = interpolate(ColorLookupOp::eCurveMaster, g);
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
        case ColorLookupOp::eMasterCurveModeLuminance: {
            const float luma = static_cast<float>(ColorMath::luminance(luminanceMath, r, g, b));
            const double l = (std::max)(luma, 1.e-8f);
            const double coef = interpolate(ColorLookupOp::eCurveMaster, (float)l) / l;
            out[0] = static_cast<float>(clamp(coef * interpolate(ColorLookupOp::eCurveRed, r)));
            out[1] = static_cast<float>(clamp(coef * interpolate(ColorLookupOp::eCurveGreen, g)));
            out[2] = static_cast<float>(clamp(coef * interpolate(ColorLookupOp::eCurveBlue, b)));
            break;
        }
        }
    }
};

ColorLookupOp::Tables
buildTables(const KnobParametricPtr& table,
            bool standard,
            bool clampBlack,
            bool clampWhite)
{
    ColorLookupOp::Tables tables;

    if (table) {
        for (int curve = 0; curve < ColorLookupOp::eCurveCount; ++curve) {
            tables.curves[curve] = CurveSnapshot(table->getParametricCurve(curve));
        }
    }
    // Of the mode, only whether it is a standard one shapes the tables.
    const ColorLookupOp::MasterCurveModeEnum mode = standard ? ColorLookupOp::eMasterCurveModeStandard : ColorLookupOp::eMasterCurveModeFilmLike;
    const LookupMaths maths = { tables, mode, ColorMath::eLuminanceMathRec709, clampBlack, clampWhite };
    for (int curve = 0; curve < ColorLookupOp::eCurveCount; ++curve) {
        if ((curve == ColorLookupOp::eCurveMaster) && standard) {
            continue;
        }
        tables.lut[curve].resize(kLutIntervals + 1);
        for (int position = 0; position <= kLutIntervals; ++position) {
            const double parametricPos = kLutMin + (kLutMax - kLutMin) * double(position) / kLutIntervals;
            tables.lut[curve][position] = (float)maths.clamp(maths.curveValue(curve, parametricPos));
        }
    }
#ifdef NATRON_HAVE_GPU
    tables.gpuCurves.assign(ColorLookupOp::eCurveCount * kGpuCurveWords, 0);
    for (int curve = 0; curve < ColorLookupOp::eCurveCount; ++curve) {
        if (!tables.curves[curve].packGpu(&tables.gpuCurves[curve * kGpuCurveWords])) {
            tables.gpuCurves.clear();
            break;
        }
    }
#endif

    return tables;
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

std::shared_ptr<const ColorLookupOp::Tables>
ColorLookupOp::bakeTables(MasterCurveModeEnum mode,
                          bool black,
                          bool white) const
{
    const bool standard = (mode == eMasterCurveModeStandard) || (mode == eMasterCurveModeWeightedStandard);
    const KnobParametricPtr knob = lookupTable.lock();
    const auto build = [&knob, standard, black, white]() {
        return buildTables(knob, standard, black, white);
    };

    if (!knob) {
        return std::make_shared<const Tables>(build());
    }

    return _tables[(standard ? 1 : 0) | (black ? 2 : 0) | (white ? 4 : 0)].get(*knob, build);
}

ColorLookupOp::Params
ColorLookupOp::bake(const KnobReader& reader) const
{
    Params p;

    p.mode = (MasterCurveModeEnum)reader.value(masterCurveMode);
    p.luminanceMath = (ColorMath::LuminanceMathEnum)reader.value(luminanceMath);
    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);
    p.tables = bakeTables(p.mode, p.clampBlack, p.clampWhite);

    return p;
}

bool
ColorLookupOp::isIdentity(const Params& params) const
{
    // Even identity curves round through the float tables, so no Params leave the source
    // bit-for-bit unchanged.
    Q_UNUSED(params);

    return false;
}

void
ColorLookupOp::row(const Params& params,
                   const RowIO& io) const
{
    const LookupMaths maths = { *params.tables, params.mode, params.luminanceMath, params.clampBlack, params.clampWhite };

    maths.processRow(io);
}

GpuOpRef
ColorLookupOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    if (!params.tables || params.tables->gpuCurves.empty()) {
        return GpuOpRef::none();
    }
    const Tables& tables = *params.tables;
    std::vector<std::uint32_t> words(kGpuTotalWords, 0);
    words[0] = (std::uint32_t)params.mode;
    words[1] = (std::uint32_t)params.luminanceMath;
    words[2] = (params.clampBlack ? 1u : 0u) | (params.clampWhite ? 2u : 0u);
    CurveSnapshot::packGpuTableRange(kLutMin, kLutMax, kLutIntervals, &words[kGpuTableRange]);
    std::copy(tables.gpuCurves.begin(), tables.gpuCurves.end(), words.begin() + kGpuCurveBase);
    for (int curve = 0; curve < eCurveCount; ++curve) {
        const CurveSnapshot& snapshot = tables.curves[curve];
        putFloat(words, kGpuYMin + curve, (float)snapshot.yMin());
        putFloat(words, kGpuYMax + curve, (float)snapshot.yMax());
        words[kGpuKeyCount + curve] = (std::uint32_t)snapshot.keys().size();
        for (std::size_t i = 0; i < tables.lut[curve].size(); ++i) {
            putFloat(words, kGpuLutBase + curve * kGpuLutSize + (int)i, tables.lut[curve][i]);
        }
    }
    GpuOpRef ref = GpuOpRef::packed((int)gpu::GpuKernelId::colorlookup_wrapped, std::move(words));
    // Two-component pixels take a per-channel path the wrapped op does not see.
    ref.acceptedComponents = GpuOpRef::componentsBit(1) | GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
} // ColorLookupOp::gpu

struct ColorLookup::BackgroundState {
    std::mutex mutex;
    std::vector<unsigned long long> counts;

    BackgroundState()
        : mutex()
        , counts()
    {
    }
};

ColorLookup::ColorLookup(NodePtr node)
    : ImageNode<ColorLookupOp>(node)
    , _background(std::make_shared<BackgroundState>())
{
}

ColorLookup::~ColorLookup()
{
}

ColorLookupOp::Params
ColorLookup::bakeColorLookup(double time,
                             ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
ColorLookup::colorLookupRow(const ColorLookupOp::Params& params,
                            const RowIO& io) const
{
    op().row(params, io);
}

void
ColorLookup::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

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
        op().lookupTable = table;
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
        op().masterCurveMode = mode;
    }

    op().luminanceMath = ColorMath::addLuminanceMathKnob(this, page);
    if (KnobChoicePtr luminanceMath = op().luminanceMath.lock()) {
        luminanceMath->setHintToolTip(tr("Formula used to compute luminance from RGB values (only used by 'Set Master' and by the luminance master curve mode)."));
    }

    KnobBoolPtr clampBlack = createKnob<KnobBool>(tr("Clamp Black"));
    clampBlack->setName(kColorLookupParamClampBlack);
    clampBlack->setHintToolTip(tr("All colors below 0 on output are set to 0."));
    clampBlack->setDefaultValue(false);
    clampBlack->setAddNewLine(false);
    page->addKnob(clampBlack);
    op().clampBlack = clampBlack;

    KnobBoolPtr clampWhite = createKnob<KnobBool>(tr("Clamp White"));
    clampWhite->setName(kColorLookupParamClampWhite);
    clampWhite->setHintToolTip(tr("All colors above 1 on output are set to 1."));
    clampWhite->setDefaultValue(false);
    page->addKnob(clampWhite);
    op().clampWhite = clampWhite;

    addStageKnobs(page);

    installBackgroundPainter();
    refreshBackgroundControls();
} // ColorLookup::initializeKnobs

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
    KnobParametricPtr table = op().lookupTable.lock();
    KnobBoolPtr hasBackground = _hasBackgroundInteract.lock();

    if (!table || !hasBackground) {
        return;
    }
    const KnobParametricWPtr weakTable = table;
    const KnobChoiceWPtr weakDisplay = _display;
    const std::shared_ptr<BackgroundState> state = _background;

    table->setBackgroundPainter([weakTable, weakDisplay, state](const ParametricBackgroundContext& context) {
        ParametricBackground background;
        KnobParametricPtr lookup = weakTable.lock();
        KnobChoicePtr display = weakDisplay.lock();

        if (!lookup || !display) {
            return background;
        }
        const int mode = display->getValue();

        if (mode == (int)eDisplayColorRamp) {
            const double x0 = (std::max)(kLutMin, context.xMin);
            const double x1 = (std::min)(kLutMax, context.xMax);
            if (!(x1 > x0)) {
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
                bottom[i] = ParametricBackgroundVertex(position, kLutMin, rgb[0], rgb[1], rgb[2]);
                top[i] = ParametricBackgroundVertex(position, kLutMax, rgb[0], rgb[1], rgb[2]);
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
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                counts = state->counts;
            }
            if (counts.size() != 3 * (std::size_t)kHistogramBins) {
                return background;
            }
            // The first and last bins collect everything outside the table's domain and would flatten the rest.
            unsigned long long peak = 0;
            for (int c = 0; c < 3; ++c) {
                for (int i = 1; i < kHistogramBins - 1; ++i) {
                    peak = (std::max)(peak, counts[c * kHistogramBins + i]);
                }
            }
            if (peak == 0) {
                return background;
            }
            const double binSize = (kLutMax - kLutMin) / kHistogramBins;
            background.additiveQuads.reserve(counts.size());
            for (int c = 0; c < 3; ++c) {
                for (int i = 0; i < kHistogramBins; ++i) {
                    const unsigned long long count = counts[c * kHistogramBins + i];
                    if (count == 0) {
                        continue;
                    }
                    const double minX = kLutMin + i * binSize;
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
    if (KnobParametricPtr table = op().lookupTable.lock()) {
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
    KnobParametricPtr table = op().lookupTable.lock();

    if (!table || !getInput(0)) {
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
                    if (v >= kLutMax) {
                        bin = kHistogramBins - 1;
                    } else if (v >= kLutMin) {
                        bin = (int)std::floor(kHistogramBins * (v - kLutMin) / (kLutMax - kLutMin));
                    }
                    ++counts[c * kHistogramBins + bin];
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(_background->mutex);
        _background->counts.swap(counts);
    }
    table->notifyBackgroundChanged();
}

void
ColorLookup::addControlPointFromButton(KnobI* button,
                                       double time,
                                       ViewIdx view)
{
    KnobParametricPtr table = op().lookupTable.lock();

    if (!table) {
        return;
    }
    double source[4];
    double target[4];
    readColor(_source, time, view, source);
    readColor(_target, time, view, target);

    if (isKnob(button, _setMaster)) {
        const ColorMath::LuminanceMathEnum math = (ColorMath::LuminanceMathEnum)readChoice(op().luminanceMath, time, view);
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
            } else if (KnobParametricPtr table = op().lookupTable.lock()) {
                table->notifyBackgroundChanged();
            }
        } else if (KnobParametricPtr table = op().lookupTable.lock()) {
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
    return false;
}

NATRON_NAMESPACE_EXIT
