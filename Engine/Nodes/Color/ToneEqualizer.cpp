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

/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/toneequal.c.
 * GPL-3.0-or-later; see ToneEqualizerKernels.h.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ToneEqualizer.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ToneEqualizerGpu.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/OverlaySupport.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

using namespace ToneEqualizerKernels;

namespace {
inline constexpr InputSpec kToneEqualizerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kToneEqualizerNodeSpec = {
    .id = PLUGINID_NATRON_TONEEQUALIZER,
    .label = "ToneEqualizer",
    .description = "Relights the image by exposure zone, as darktable's tone equalizer module does: a gain per band, nine bands "
                   "from -8 EV to 0 EV joined by a smooth curve.\n"
                   "\n"
                   "Each pixel's band is read from a luminance mask that an edge-aware filter smooths, so that local contrast "
                   "inside a zone is kept while the zones are dodged and burned. Works on scene-linear data, before any view "
                   "transform.\n"
                   "\n"
                   "Output Mask shows the luminance mask, Mask Exposure Histogram Zones the band each pixel falls in. With the "
                   "node's controls open, the viewer shows the mask exposure, the band and the correction under the cursor.",
    .grouping = PLUGIN_GROUP_COLOR,
    .majorVersion = PLUGIN_MAJOR_NATRON_TONEEQUALIZER,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/ToneEqualizer.png",
    .inputs = kToneEqualizerInputs,
};

constexpr ImagePipelineSpec kToneEqualizerPipeline = {
    .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Custom,
};

struct BandKnob {
    const char* name;
    const char* label;
    // darktable's slider description.
    const char* description;
};

const BandKnob kBandKnobs[kBands] = {
    { kToneEqualizerParamBlacks, "Blacks", "blacks" },
    { kToneEqualizerParamDeepShadows, "Deep Shadows", "deep shadows" },
    { kToneEqualizerParamShadows, "Shadows", "shadows" },
    { kToneEqualizerParamLightShadows, "Light Shadows", "light shadows" },
    { kToneEqualizerParamMidtones, "Midtones", "mid-tones" },
    { kToneEqualizerParamDarkHighlights, "Dark Highlights", "dark highlights" },
    { kToneEqualizerParamHighlights, "Highlights", "highlights" },
    { kToneEqualizerParamWhites, "Whites", "whites" },
    { kToneEqualizerParamSpeculars, "Speculars", "speculars" },
};

bool
effectRoDPixels(EffectInstance& effect,
                double time,
                const RenderScale& scale,
                ViewIdx view,
                RectI* out)
{
    RectD rod;
    bool isProjectFormat = false;

    if (effect.getRegionOfDefinition_public(effect.getRenderHash(), time, scale, view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    *out = PassExec::toPixelEnclosing(rod, scale, effect.getRenderMetadata(time, view)->getPixelAspectRatio(-1));

    return true;
}

double
sliderFromSmoothing(float smoothing)
{
    return std::log((double)smoothing) / std::log(M_SQRT2) - 1.;
}

template <typename KNOB, typename T>
void
setIfChanged(const std::shared_ptr<KNOB>& knob,
             T value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}
} // namespace

struct ToneEqualizer::Resolved {
    ToneEqualizerKernels::Settings settings;
    std::array<float, kCenters> factors = {};
    // Whether the curve's fit is stable; darktable warns when it is not.
    bool stable = true;
    OutputEnum output = eOutputResult;
    // The source's size at render scale 1, which the smoothing diameter is relative to.
    int fullWidth = 0;
    int fullHeight = 0;
};

ToneEqualizer::ToneEqualizer(NodePtr node)
    : StagedImageEffect(node)
    , _maskMutex()
    , _maskKey()
    , _mask()
    , _interact()
    , _message()
{
}

ToneEqualizer::~ToneEqualizer()
{
}

const NodeSpec*
ToneEqualizer::nodeSpec() const
{
    return staticNodeSpec<kToneEqualizerNodeSpec>();
}

const ImagePipelineSpec&
ToneEqualizer::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kToneEqualizerPipeline, kToneEqualizerNodeSpec>();
}

void
ToneEqualizer::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    const auto addDouble = [&](const char* label,
                               const char* name,
                               const QString& tip,
                               double def,
                               double min,
                               double max,
                               double displayMin,
                               double displayMax) {
        KnobDoublePtr knob = createKnob<KnobDouble>(tr(label).toStdString());
        knob->setName(name);
        knob->setHintToolTip(tip.toStdString());
        knob->setDefaultValue(def);
        knob->setMinimum(min);
        knob->setMaximum(max);
        knob->setDisplayMinimum(displayMin);
        knob->setDisplayMaximum(displayMax);
        page->addKnob(knob);

        return knob;
    };

    {
        std::vector<ChoiceOption> options;
        for (const Preset& p : presets()) {
            options.push_back(ChoiceOption(p.id, tr(p.label).toStdString(), std::string()));
        }
        KnobChoicePtr preset = createKnob<KnobChoice>(tr("Preset").toStdString());
        preset->setName(kToneEqualizerParamPreset);
        preset->setHintToolTip(tr("One of darktable's built-in presets. Apply Preset writes it into every control.").toStdString());
        preset->populateChoices(options);
        preset->setDefaultValue(0);
        preset->setAnimationEnabled(false);
        preset->setEvaluateOnChange(false);
        preset->setAddNewLine(false);
        page->addKnob(preset);
        _preset = preset;

        KnobButtonPtr apply = createKnob<KnobButton>(tr("Apply Preset").toStdString());
        apply->setName(kToneEqualizerParamApplyPreset);
        apply->setHintToolTip(tr("Sets every control to the chosen preset.").toStdString());
        apply->setEvaluateOnChange(false);
        page->addKnob(apply);
        _applyPreset = apply;
    }

    {
        KnobChoicePtr output = createKnob<KnobChoice>(tr("Output").toStdString());
        output->setName(kToneEqualizerParamOutput);
        output->setHintToolTip(tr("Result is the equalized image. Mask shows the luminance mask the bands are read from, from -8 EV "
                                  "(black) to 0 EV (white) with a gamma of 2 for legibility in the shadows: choose the estimator "
                                  "and filter that give the most contrast between the areas to dodge and the areas to burn. Mask "
                                  "Exposure Histogram Zones shows the band each pixel falls in, in the same greys.")
                                   .toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("result", tr("Result").toStdString(), std::string()));
        options.push_back(ChoiceOption("mask", tr("Mask").toStdString(), std::string()));
        options.push_back(ChoiceOption("zones", tr("Mask Exposure Histogram Zones").toStdString(), std::string()));
        output->populateChoices(options);
        output->setDefaultValue((int)eOutputResult);
        page->addKnob(output);
        _output = output;
    }

    {
        KnobStringPtr readout = createKnob<KnobString>(tr("Cursor Readout").toStdString());
        readout->setName(kToneEqualizerParamCursorReadout);
        readout->setHintToolTip(tr("The mask exposure under the viewer cursor, the band it falls in and the correction applied there.").toStdString());
        readout->setAsLabel();
        readout->setEvaluateOnChange(false);
        readout->setIsPersistent(false);
        page->addKnob(readout);
        _cursorReadout = readout;
    }

    for (int i = 0; i < kBands; ++i) {
        const BandKnob& b = kBandKnobs[i];
        _gains[i] = addDouble(b.label,
                              b.name,
                              tr("Exposure correction, in EV, of the band at %1 EV (darktable's \"%2\").").arg(i + (int)kMinEv).arg(QString::fromUtf8(b.description)),
                              0.,
                              -2.,
                              2.,
                              -2.,
                              2.);
    }

    {
        KnobDoublePtr knob = addDouble("Curve Smoothing",
                                       kToneEqualizerParamCurveSmoothing,
                                       tr("Positive values will produce more progressive tone transitions but the curve might become "
                                          "oscillatory in some settings. Negative values will avoid oscillations and behave more robustly "
                                          "but may produce brutal tone transitions and damage local contrast."),
                                       0.,
                                       -2.33,
                                       1.67,
                                       -1.,
                                       1.);
        knob->setDecimals(2);
        _curveSmoothing = knob;
    }

    {
        KnobDoublePtr knob = addDouble("Mask Exposure Compensation",
                                       kToneEqualizerParamMaskExposureCompensation,
                                       tr("Use this to slide the mask average exposure along the bands, in EV, for a better control of the "
                                          "exposure correction with the available bands."),
                                       0.,
                                       -16.,
                                       16.,
                                       -4.,
                                       4.);
        knob->setAddNewLine(false);
        _maskExposureCompensation = knob;
        KnobButtonPtr button = createKnob<KnobButton>(tr("Auto Mask Exposure Compensation").toStdString());
        button->setName(kToneEqualizerParamAutoMaskExposureCompensation);
        button->setHintToolTip(tr("Auto-adjust the average exposure, from the mask of the current frame.").toStdString());
        button->setEvaluateOnChange(false);
        page->addKnob(button);
        _autoMaskExposureCompensation = button;
    }

    {
        KnobDoublePtr knob = addDouble("Mask Contrast Compensation",
                                       kToneEqualizerParamMaskContrastCompensation,
                                       tr("Use this to counter the averaging effect of the guided filter and dilate the mask contrast "
                                          "around -4 EV, in EV. This allows to spread the exposure histogram over more bands for a better "
                                          "control of the exposure correction. Used by Guided Filter and EIGF."),
                                       0.,
                                       -16.,
                                       16.,
                                       -2.,
                                       2.);
        knob->setAddNewLine(false);
        _maskContrastCompensation = knob;
        KnobButtonPtr button = createKnob<KnobButton>(tr("Auto Mask Contrast Compensation").toStdString());
        button->setName(kToneEqualizerParamAutoMaskContrastCompensation);
        button->setHintToolTip(tr("Auto-adjust the contrast, from the mask of the current frame.").toStdString());
        button->setEvaluateOnChange(false);
        page->addKnob(button);
        _autoMaskContrastCompensation = button;
    }

    {
        KnobChoicePtr knob = createKnob<KnobChoice>(tr("Luminance Estimator").toStdString());
        knob->setName(kToneEqualizerParamLuminanceEstimator);
        knob->setHintToolTip(tr("Preview the mask and choose the estimator that gives you the higher contrast between areas to dodge "
                                "and areas to burn.")
                                 .toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("mean", tr("RGB Average").toStdString(), std::string()));
        options.push_back(ChoiceOption("lightness", tr("HSL Lightness").toStdString(), std::string()));
        options.push_back(ChoiceOption("value", tr("HSV Value / RGB Max").toStdString(), std::string()));
        options.push_back(ChoiceOption("norm1", tr("RGB Sum").toStdString(), std::string()));
        options.push_back(ChoiceOption("norm2", tr("RGB Euclidean Norm").toStdString(), std::string()));
        options.push_back(ChoiceOption("powerNorm", tr("RGB Power Norm").toStdString(), std::string()));
        options.push_back(ChoiceOption("geomean", tr("RGB Geometric Mean").toStdString(), std::string()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eEstimatorNorm2);
        page->addKnob(knob);
        _luminanceEstimator = knob;
    }

    {
        KnobChoicePtr knob = createKnob<KnobChoice>(tr("Preserve Details").toStdString());
        knob->setName(kToneEqualizerParamPreserveDetails);
        knob->setHintToolTip(tr("No affects global and local contrast (safe if you only add contrast).\n"
                                "Guided Filter only affects global contrast and tries to preserve local contrast.\n"
                                "Averaged Guided Filter is a geometric mean of the No and Guided Filter methods.\n"
                                "EIGF (exposure-independent guided filter) is a guided filter that is exposure-independent: it "
                                "smooths shadows and highlights the same way (contrary to the guided filter, which smooths the "
                                "highlights less).\n"
                                "Averaged EIGF is a geometric mean of the No and EIGF methods.")
                                 .toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("no", tr("No").toStdString(), std::string()));
        options.push_back(ChoiceOption("averagedGuidedFilter", tr("Averaged Guided Filter").toStdString(), std::string()));
        options.push_back(ChoiceOption("guidedFilter", tr("Guided Filter").toStdString(), std::string()));
        options.push_back(ChoiceOption("averagedEigf", tr("Averaged EIGF").toStdString(), std::string()));
        options.push_back(ChoiceOption("eigf", tr("EIGF").toStdString(), std::string()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eDetailsEigf);
        page->addKnob(knob);
        _preserveDetails = knob;
    }

    {
        KnobIntPtr knob = createKnob<KnobInt>(tr("Filter Diffusion").toStdString());
        knob->setName(kToneEqualizerParamFilterDiffusion);
        knob->setHintToolTip(tr("Number of passes of the guided filter to apply. Helps diffusing the edges of the filter at the expense "
                                "of speed.")
                                 .toStdString());
        knob->setDefaultValue(1);
        knob->setMinimum(1);
        knob->setMaximum(20);
        knob->setDisplayMinimum(1);
        knob->setDisplayMaximum(5);
        page->addKnob(knob);
        _filterDiffusion = knob;
    }

    _smoothingDiameter = addDouble("Smoothing Diameter",
                                   kToneEqualizerParamSmoothingDiameter,
                                   tr("Diameter of the blur in percent of the largest image size. Big values are slow to render."),
                                   5.,
                                   0.01,
                                   100.,
                                   1.,
                                   45.);

    _edgesRefinement = addDouble("Edges Refinement",
                                 kToneEqualizerParamEdgesRefinement,
                                 tr("Precision of the feathering: higher values force the mask to follow edges more closely but may "
                                    "void the effect of the smoothing; lower values give smoother gradients and better smoothing but "
                                    "may lead to inaccurate edges taping and halos."),
                                 1.,
                                 0.01,
                                 10000.,
                                 0.1,
                                 50.);

    _maskQuantization = addDouble("Mask Quantization",
                                  kToneEqualizerParamMaskQuantization,
                                  tr("0 disables the quantization. Higher values, in EV, posterize the luminance mask to help the "
                                     "guiding produce piece-wise smooth areas when using high Edges Refinement values."),
                                  0.,
                                  0.,
                                  2.,
                                  0.,
                                  2.);

    addStageKnobs(page);
} // ToneEqualizer::initializeKnobs

std::optional<ToneEqualizer::Resolved>
ToneEqualizer::resolve(double time,
                       ViewIdx view)
{
    if (!_output.lock()) {
        return std::nullopt;
    }
    Resolved r;
    ToneEqualizerKernels::Settings& s = r.settings;
    const KnobReader reader(time, view);

    for (int i = 0; i < kBands; ++i) {
        s.gains[i] = (float)reader.value(_gains[i]);
    }
    s.blending = (float)reader.value(_smoothingDiameter);
    s.smoothing = smoothingFromSlider(reader.value(_curveSmoothing));
    s.feathering = (float)reader.value(_edgesRefinement);
    s.quantization = (float)reader.value(_maskQuantization);
    s.contrastBoost = (float)reader.value(_maskContrastCompensation);
    s.exposureBoost = (float)reader.value(_maskExposureCompensation);
    s.details = (DetailsEnum)std::clamp((int)reader.value(_preserveDetails), 0, (int)eDetailsCount - 1);
    s.estimator = (EstimatorEnum)std::clamp((int)reader.value(_luminanceEstimator), 0, (int)eEstimatorCount - 1);
    s.iterations = (int)reader.value(_filterDiffusion);
    r.output = (OutputEnum)std::clamp((int)reader.value(_output), 0, (int)eOutputCount - 1);
    r.stable = solveFactors(s.gains, s.smoothing, &r.factors);

    RectI fullRoD;
    if (RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &fullRoD)) {
        r.fullWidth = fullRoD.width();
        r.fullHeight = fullRoD.height();
    }

    return r;
} // ToneEqualizer::resolve

bool
ToneEqualizer::knobChanged(KnobI* k,
                           ValueChangedReasonEnum /*reason*/,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    const KnobIPtr apply = _applyPreset.lock();
    const KnobIPtr autoExposure = _autoMaskExposureCompensation.lock();
    const KnobIPtr autoContrast = _autoMaskContrastCompensation.lock();

    if (apply && (k == apply.get())) {
        applyPreset();

        return true;
    }
    if (autoExposure && (k == autoExposure.get())) {
        autoAdjust(true);

        return true;
    }
    if (autoContrast && (k == autoContrast.get())) {
        autoAdjust(false);

        return true;
    }

    return false;
}

void
ToneEqualizer::applyPreset()
{
    const KnobChoicePtr presetKnob = _preset.lock();

    if (!presetKnob) {
        return;
    }
    const int index = presetKnob->getValue();
    const std::vector<Preset>& all = presets();
    if ((index < 0) || (index >= (int)all.size())) {
        return;
    }
    const ToneEqualizerKernels::Settings& s = all[index].settings;
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    for (int i = 0; i < kBands; ++i) {
        setIfChanged(_gains[i].lock(), (double)s.gains[i]);
    }
    setIfChanged(_curveSmoothing.lock(), sliderFromSmoothing(s.smoothing));
    setIfChanged(_smoothingDiameter.lock(), (double)s.blending);
    setIfChanged(_edgesRefinement.lock(), (double)s.feathering);
    setIfChanged(_maskQuantization.lock(), (double)s.quantization);
    setIfChanged(_maskContrastCompensation.lock(), (double)s.contrastBoost);
    setIfChanged(_maskExposureCompensation.lock(), (double)s.exposureBoost);
    setIfChanged(_preserveDetails.lock(), (int)s.details);
    setIfChanged(_luminanceEstimator.lock(), (int)s.estimator);
    setIfChanged(_filterDiffusion.lock(), s.iterations);
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
}

void
ToneEqualizer::autoAdjust(bool exposure)
{
    const double time = getCurrentTime();
    const ViewIdx view(0);
    const std::optional<Mask> mask = maskForAdjust(time, view, getOverlayInteractRenderScale());

    if (!mask || !mask->values || mask->values->empty()) {
        return;
    }
    const Deciles deciles = maskDeciles(mask->values->data(), mask->values->size());
    const ToneEqualizerKernels::Settings& used = mask->settings;
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    if (exposure) {
        const float value = autoExposureBoost(deciles, used.exposureBoost, used.contrastBoost);
        if (std::isfinite(value)) {
            setIfChanged(_maskExposureCompensation.lock(), (double)std::clamp(value, -16.f, 16.f));
        }
    } else {
        const float value = autoContrastBoost(deciles, used.exposureBoost, used.contrastBoost, used.details, used.feathering);
        if (std::isfinite(value)) {
            setIfChanged(_maskContrastCompensation.lock(), (double)std::clamp(value, -16.f, 16.f));
        }
    }
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
}

std::optional<ToneEqualizer::Mask>
ToneEqualizer::lastMask() const
{
    std::lock_guard<std::mutex> lock(_maskMutex);

    return _mask;
}

void
ToneEqualizer::keepMask(const Mask& mask)
{
    std::lock_guard<std::mutex> lock(_maskMutex);

    _mask = mask;
}

std::optional<ToneEqualizer::Mask>
ToneEqualizer::maskForAdjust(double time,
                             ViewIdx view,
                             const RenderScale& scale)
{
    const std::optional<Resolved> r = resolve(time, view);

    if (!r) {
        return std::nullopt;
    }
    const double s = scale.toOfxPointD().x;
    if (const std::optional<Mask> kept = lastMask()) {
        if ((kept->time == time) && (kept->view == view) && (kept->scale == s) && sameMask(kept->settings, r->settings)) {
            return kept;
        }
    }

    // Nothing rendered for these knobs yet: read the source, undivided, at scale.
    const int inputNb = getSourceInput();
    EffectInstancePtr input = getInput(inputNb);
    if (!input) {
        return std::nullopt;
    }
    RectD sourceRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &sourceRoD, &isProjectFormat) == eStatusFailed) {
        return std::nullopt;
    }
    RectI roi;
    ImagePtr src = getImage(inputNb, time, scale, view, &sourceRoD, NULL, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return std::nullopt;
    }
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(inputNb);
    const RectI bounds = src->getBounds();
    const int w = bounds.width();
    const int h = bounds.height();
    if ((w <= 0) || (h <= 0)) {
        return std::nullopt;
    }
    const int nComps = (int)src->getComponentsCount();
    std::vector<float> rgb((std::size_t)w * h * 3, 0.f);
    {
        Image::ReadAccess access(src.get());
        for (int row = 0; row < h; ++row) {
            const float* pix = (const float*)access.pixelAt(bounds.x1, bounds.y2 - 1 - row);
            if (!pix) {
                continue;
            }
            float* line = rgb.data() + (std::size_t)row * w * 3;
            for (int x = 0; x < w; ++x, pix += nComps) {
                for (int c = 0; c < 3; ++c) {
                    line[3 * x + c] = pix[(nComps >= 3) ? c : 0];
                }
            }
        }
    }
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    const EdgeAware::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, nThreads);
    std::shared_ptr<std::vector<float>> values = std::make_shared<std::vector<float>>((std::size_t)w * h);
    const int fullWidth = (r->fullWidth > 0) ? r->fullWidth : w;
    const int fullHeight = (r->fullHeight > 0) ? r->fullHeight : h;
    computeMask(rgb.data(), w, h, r->settings, maskRadius(r->settings.blending, fullWidth, fullHeight, s), values->data(), parallel);

    Mask mask;
    mask.settings = r->settings;
    mask.time = time;
    mask.view = view;
    mask.scale = s;
    mask.par = par;
    mask.bounds = bounds;
    mask.values = values;

    return mask;
} // ToneEqualizer::maskForAdjust

ToneEqualizerInteract::Reading
ToneEqualizer::readingAt(const Mask& mask,
                         const QPointF& canonical,
                         double time,
                         ViewIdx view)
{
    ToneEqualizerInteract::Reading reading;

    if (!mask.values || (mask.par <= 0.)) {
        return reading;
    }
    const int px = (int)std::floor(canonical.x() * mask.scale / mask.par);
    const int py = (int)std::floor(canonical.y() * mask.scale);
    const int w = mask.bounds.width();
    const int h = mask.bounds.height();
    const float luminance = sampleMask(mask.values->data(), w, h, px - mask.bounds.x1, mask.bounds.y2 - 1 - py);
    if (std::isnan(luminance)) {
        return reading;
    }
    const std::optional<Resolved> r = resolve(time, view);
    if (!r) {
        return reading;
    }
    reading.valid = true;
    reading.exposure = std::log2(luminance);
    reading.band = bandOf(luminance);
    reading.correction = std::log2(pixelCorrection(reading.exposure, r->factors, r->settings.smoothing));

    return reading;
}

void
ToneEqualizer::showReadout(const ToneEqualizerInteract::Reading& reading)
{
    if (const KnobStringPtr knob = _cursorReadout.lock()) {
        const std::string text = ToneEqualizerInteract::summaryText(reading);
        if (knob->getValue() != text) {
            knob->setValue(text);
        }
    }
}

void
ToneEqualizer::drawOverlay(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx view)
{
    QPointF pos;

    if (!_interact.cursor(&pos)) {
        return;
    }
    ToneEqualizerInteract::Reading reading;
    if (const std::optional<Mask> mask = lastMask()) {
        reading = readingAt(*mask, pos, time, view);
    }
    _interact.draw(getCurrentViewportForOverlays(), reading);
}

bool
ToneEqualizer::onOverlayPenMotion(double time,
                                  const RenderScale& /*renderScale*/,
                                  ViewIdx view,
                                  const QPointF& /*viewportPos*/,
                                  const QPointF& pos,
                                  double /*pressure*/,
                                  double /*timestamp*/)
{
    _interact.setCursor(pos);
    ToneEqualizerInteract::Reading reading;
    if (const std::optional<Mask> mask = lastMask()) {
        reading = readingAt(*mask, pos, time, view);
    }
    showReadout(reading);
    redrawOverlayInteract();

    return false;
}

bool
ToneEqualizer::onOverlayWheel(double time,
                              const RenderScale& /*renderScale*/,
                              ViewIdx view,
                              const QPointF& /*viewportPos*/,
                              const QPointF& pos,
                              double delta,
                              KeyboardModifiers modifiers)
{
    _interact.setCursor(pos);
    const std::optional<Mask> mask = lastMask();
    if (!mask) {
        return false;
    }
    ToneEqualizerInteract::Reading reading = readingAt(*mask, pos, time, view);
    // Off the image the wheel zooms the viewer.
    if (!reading.valid) {
        return false;
    }
    bool editedBurst = false;
    const int steps = _interact.takeWheelSteps(delta, &editedBurst);
    if (steps == 0) {
        return true;
    }
    const std::optional<Resolved> r = resolve(time, view);
    if (!r) {
        return true;
    }
    const float offset = (steps > 0 ? 1.f : -1.f) * ToneEqualizerInteract::wheelStepEv(modifiers);
    std::array<float, kBands> gains = r->settings.gains;
    bool changed = false;
    for (int i = 0; i < std::abs(steps); ++i) {
        std::array<float, kBands> next;
        // darktable keeps the bands when the refit is unstable.
        if (!scrollGains(gains, r->settings.smoothing, reading.exposure, offset, &next)) {
            break;
        }
        gains = next;
        changed = true;
    }
    if (changed) {
        AppInstancePtr app = getApp();
        const bool undoable = app && !app->isCreatingPythonGroup();
        if (undoable) {
            setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
            if (editedBurst) {
                setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOn);
            }
        }
        for (int i = 0; i < kBands; ++i) {
            setIfChanged(_gains[i].lock(), (double)gains[i]);
        }
        if (undoable) {
            setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
        }
        _interact.markWheelEdit();
        reading = readingAt(*mask, pos, time, view);
    }
    showReadout(reading);
    redrawOverlayInteract();

    return true;
}

bool
ToneEqualizer::onOverlayFocusLost(double /*time*/,
                                  const RenderScale& /*renderScale*/,
                                  ViewIdx /*view*/)
{
    _interact.clearCursor();
    redrawOverlayInteract();

    return false;
}

void
ToneEqualizer::purgeCaches()
{
    std::lock_guard<std::mutex> lock(_maskMutex);

    _maskKey.clear();
    _mask.reset();
}

bool
ToneEqualizer::isOpIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view)
{
    const std::optional<Resolved> r = resolve(time, view);

    if (r && !r->stable) {
        _message.post(getNode(), eMessageTypeWarning, tr("The interpolation is unstable, decrease the curve smoothing.").toStdString());
    } else {
        _message.withdraw(getNode());
    }

    return false;
}

void
ToneEqualizer::getRegionsOfInterest(double time,
                                    const RenderScale& scale,
                                    const RectD& outputRoD,
                                    const RectD& renderWindow,
                                    ViewIdx view,
                                    RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    // The mask's filter runs over the whole frame.
    EffectInstancePtr input = getInput(getSourceInput());
    if (!input) {
        return;
    }
    RectD sourceRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &sourceRoD, &isProjectFormat) != eStatusFailed) {
        (*ret)[input] = sourceRoD;
    }
}

StatusEnum
ToneEqualizer::renderOp(const RenderActionArgs& args,
                        const StagePlan& plan)
{
    using namespace PassExec;

    const std::optional<Resolved> found = resolve(args.time, args.view);
    if (!found) {
        return eStatusFailed;
    }
    const Resolved& r = *found;
    const ToneEqualizerKernels::Settings& s = r.settings;
    const int sourceInput = plan.sourceInput;
    const double renderScale = args.mappedScale.toOfxPointD().x;
    EffectInstancePtr input = (sourceInput >= 0) ? getInput(sourceInput) : EffectInstancePtr();

    ImagePtr mask;
    if (plan.applyMask && (plan.maskChannel >= 0)) {
        RectI maskRoI;
        mask = getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        if (mask && (plan.maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    std::vector<PlaneJob> jobs;
    std::vector<std::string> layerIds;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            continue;
        }
        job.nComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, job.nComps, args.processChannels);
        ImageLayerDesc sourceLayer;
        if (input && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            job.src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (plan.divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &plan.divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
            if (job.divisor && (plan.divisorChannel < (int)job.divisor->getComponentsCount())) {
                job.divisorChannel = plan.divisorChannel;
                job.skipChannel = Node::getUnPremultSkipChannel(it->first.isColorLayer() ? job.dst->getComponents() : it->first, plan.divisorLayer, plan.divisorChannel);
            } else {
                job.divisor.reset();
            }
        }
        if (!isFloatImage(job.dst) || !isFloatImage(job.src) || !isFloatImage(job.divisor)) {
            return eStatusFailed;
        }
        jobs.push_back(job);
        layerIds.push_back(it->first.getLayerID());
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }
    const RectI& roi = args.roi;
    if ((roi.width() <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }
    RectI rodPixel;
    if (!effectRoDPixels(*this, args.time, args.mappedScale, args.view, &rodPixel)) {
        return eStatusFailed;
    }

    MaskInput maskSource;
    maskSource.applied = plan.applyMask;
    maskSource.invert = plan.maskInvert;
    maskSource.image = mask;
    maskSource.channel = plan.maskChannel;
    maskSource.lock();
    for (PlaneJob& job : jobs) {
        job.lock();
    }

    RectI window(0, 0, 0, 0);
    const bool processing = input && (plan.mix != 0.) && intersectRects(roi, rodPixel, &window) && !isEmptyRect(window);

    std::vector<float> lut;
    if (processing && (r.output == eOutputResult)) {
        lut.resize(kLutSize);
        correctionLut(r.factors, s.smoothing, lut.data());
    }
    const double par = getRenderMetadata(args.time, args.view)->getPixelAspectRatio(sourceInput);
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    const EdgeAware::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, nThreads);
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const PlaneJob& job = jobs[j];
        const ProcessedPlanes planes(job);
        WindowLayout layout;
        layout.roi = roi;
        layout.processWindow = window;
        layout.processing = processing && (job.nComps >= 3) && (planes.count() > 0);
        layout.bufferRect = window;
        layout.buffer = NULL;
        layout.mix = (float)plan.mix;
        std::vector<float> result;
        if (layout.processing) {
            // The mask depends on the source and the mask controls only, so band edits reuse it.
            const U64 sourceKey = RawNodeSupport::frameKey(input->getRenderHash(), args.time, args.view);
            const std::string key = std::to_string(sourceKey) + '|' + std::to_string(renderScale) + '|' + layerIds[j] + '|' + plan.divisorLayer.getLayerID() + '|'
                + std::to_string(plan.divisorChannel) + '|' + std::to_string(rodPixel.x1) + ',' + std::to_string(rodPixel.y1) + ',' + std::to_string(rodPixel.x2) + ','
                + std::to_string(rodPixel.y2);
            std::shared_ptr<const std::vector<float>> values;
            {
                std::lock_guard<std::mutex> lock(_maskMutex);
                if (_mask && (_maskKey == key) && sameMask(_mask->settings, s)) {
                    values = _mask->values;
                }
            }
            const RectI bufferRect = values ? window : rodPixel;

            PlaneJob fillJob = job;
            fillJob.channels.set(0).set(1).set(2);
            const ProcessedPlanes fillPlanes(fillJob);
            const int bw = bufferRect.width();
            const int bh = bufferRect.height();
            const std::size_t planeSize = (std::size_t)bw * bh;
            std::vector<float> filled(planeSize * fillPlanes.count());
            fillBuffer(fillJob, fillPlanes, bufferRect, Boundary::Zero, nThreads, cancel, filled.data());
            if (cancel.check()) {
                return eStatusOK;
            }

            if (!values) {
                std::lock_guard<std::mutex> lock(_maskMutex);
                if (_mask && (_maskKey == key) && sameMask(_mask->settings, s)) {
                    values = _mask->values;
                } else {
                    // darktable's buffers run from the top row, which the guided filter's sampling grid follows.
                    std::vector<float> rgb(planeSize * 3);
                    parallel(bh, [&](int row) {
                        const std::size_t y = (std::size_t)(bh - 1 - row);
                        float* line = rgb.data() + (std::size_t)row * bw * 3;
                        for (int c = 0; c < 3; ++c) {
                            const float* plane = filled.data() + (std::size_t)fillPlanes.planeOfChannel[c] * planeSize + y * bw;
                            for (int x = 0; x < bw; ++x) {
                                line[3 * x + c] = plane[x];
                            }
                        }
                    });
                    RawNodeSupport::noteFrameComputed(kToneEqualizerFrameMask, sourceKey);
                    std::shared_ptr<std::vector<float>> computed = std::make_shared<std::vector<float>>(planeSize);
                    const int fullWidth = (r.fullWidth > 0) ? r.fullWidth : bw;
                    const int fullHeight = (r.fullHeight > 0) ? r.fullHeight : bh;
                    computeMask(rgb.data(), bw, bh, s, maskRadius(s.blending, fullWidth, fullHeight, renderScale), computed->data(), parallel);
                    if (cancel.check()) {
                        return eStatusOK;
                    }
                    Mask kept;
                    kept.settings = s;
                    kept.time = args.time;
                    kept.view = args.view;
                    kept.scale = renderScale;
                    kept.par = par;
                    kept.bounds = rodPixel;
                    kept.values = computed;
                    _mask = kept;
                    _maskKey = key;
                    values = computed;
                }
            }

            const int rw = rodPixel.width();
            const int ww = window.width();
            const int wh = window.height();
            const int wx = window.x1 - bufferRect.x1;
            const int wy = window.y1 - bufferRect.y1;
            const float* maskValues = values->data();
            const std::size_t windowSize = (std::size_t)ww * wh;
            result.resize(windowSize * planes.count());
            for (int p = 0; p < planes.count(); ++p) {
                const int c = planes.channel[p];
                float* dstPlane = result.data() + (std::size_t)p * windowSize;
                parallel(wh, [&](int y) {
                    float* line = dstPlane + (std::size_t)y * ww;
                    const float* src = filled.data() + (std::size_t)fillPlanes.planeOfChannel[c] * planeSize + (std::size_t)(y + wy) * bw + wx;
                    if (c >= 3) {
                        std::memcpy(line, src, sizeof(float) * ww);

                        return;
                    }
                    const int pixelY = window.y1 + y;
                    const float* maskLine = maskValues + (std::size_t)(rodPixel.y2 - 1 - pixelY) * rw + (window.x1 - rodPixel.x1);
                    switch (r.output) {
                    case eOutputResult:
                        for (int x = 0; x < ww; ++x) {
                            line[x] = lutCorrection(lut.data(), maskLine[x]) * src[x];
                        }
                        break;
                    case eOutputMask:
                        for (int x = 0; x < ww; ++x) {
                            line[x] = displayMask(maskLine[x]);
                        }
                        break;
                    case eOutputZones:
                    case eOutputCount:
                        for (int x = 0; x < ww; ++x) {
                            line[x] = zoneDisplay(maskLine[x]);
                        }
                        break;
                    }
                });
            }
            if (cancel.check()) {
                return eStatusOK;
            }
            layout.buffer = result.data();
        }
        writeWindow(job, planes, maskSource, layout, nThreads, cancel);
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // ToneEqualizer::renderOp

bool
ToneEqualizer::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4) || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    // The mask runs over the whole frame, in buffers of its size beside the divided source; a
    // frame past the kernels' 32-bit indices renders on the CPU. The device's binding limit is
    // only known when recording, where record() refuses a frame over it.
    RectI rodPixel;
    if (!effectRoDPixels(*const_cast<ToneEqualizer*>(this), query.time, query.mappedScale, query.view, &rodPixel)) {
        return false;
    }
    const uint64_t floats = (uint64_t)std::max(0, rodPixel.width()) * (uint64_t)std::max(0, rodPixel.height()) * (uint64_t)std::max(4, query.nComps);

    return floats <= UINT32_MAX;
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
ToneEqualizer::getOpGpuInputLayer(int inputNb,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
ToneEqualizer::getOpGpuInputRoI(int inputNb,
                                double time,
                                ViewIdx view,
                                const RectI& outputRect,
                                const RenderScale& scale) const
{
    if (inputNb != getSourceInput()) {
        return outputRect;
    }
    RectI need = outputRect;
    RectI rodPixel;
    if (effectRoDPixels(*const_cast<ToneEqualizer*>(this), time, scale, view, &rodPixel)) {
        need.merge(rodPixel);
    }

    return need;
}

StatusEnum
ToneEqualizer::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    using namespace PassExec;

    const GpuQuery& query = args.query;
    const StagePlan plan = resolveGpuPlan(query);
    const int sourceInput = plan.sourceInput;
    const int nComps = query.nComps;
    const RectI& rect = args.outputRect;
    if (!canRenderOpOnGpu(query) || (sourceInput < 0) || (sourceInput >= (int)args.inputs.size())) {
        return eStatusFailed;
    }
    const GpuImageView& src = args.inputs[sourceInput];
    const GpuImageView& dst = args.output;
    EffectInstancePtr input = getInput(sourceInput);
    if (!input || !isUsableGpuView(src, nComps) || !isUsableGpuView(dst, nComps) || isEmptyRect(rect) || !dst.bounds.contains(rect)) {
        return eStatusFailed;
    }
    const std::optional<Resolved> found = resolve(query.time, query.view);
    if (!found) {
        return eStatusFailed;
    }
    const Resolved& r = *found;
    const ToneEqualizerKernels::Settings& s = r.settings;
    const double time = query.time;
    const ViewIdx view = query.view;
    const RenderScale& scale = query.mappedScale;
    const double renderScale = scale.toOfxPointD().x;

    RectI rodPixel;
    if (!effectRoDPixels(*this, time, scale, view, &rodPixel)) {
        return eStatusFailed;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    // fillBuffer() reads the source as zero outside the input image, as the divide does outside its
    // view.
    RectI valid;
    intersectRects(src.bounds, toPixelEnclosing(srcRoD, scale, getRenderMetadata(time, view)->getPixelAspectRatio(sourceInput)), &valid);
    GpuRenderArgs wrapArgs(args);
    wrapArgs.inputs[sourceInput] = cropGpuView(src, valid);

    const std::bitset<4> processed = processedBitsForImage(query.plane, nComps, plan.processChannels);
    RectI window(0, 0, 0, 0);
    const bool processing = (plan.mix != 0.) && (nComps >= 3) && processed.any() && intersectRects(rect, rodPixel, &window) && !isEmptyRect(window);
    RectI dividedRect = rect;
    if (processing) {
        dividedRect.merge(rodPixel);
    }

    gpu::GpuRecordContext& ctx = args.context;
    const uint64_t maxFloats = std::min<uint64_t>(UINT32_MAX, ctx.maxScratchBytes() / sizeof(float));
    GpuImageView divided;
    divided.bounds = dividedRect;
    divided.nComps = nComps;
    divided.rowStrideFloats = dividedRect.width() * nComps;
    const uint64_t dividedFloats = (uint64_t)dividedRect.area() * nComps;
    if ((dividedFloats > maxFloats) || !ctx.scratch(dividedFloats * sizeof(float), divided.buffer) || !divided.buffer
        || (recordStagesGpuDivide(plan, wrapArgs, divided) != eStatusOK)) {
        return eStatusFailed;
    }
    ctx.computeBarrier();
    if (!processing) {
        return recordStagesGpuFinish(plan, wrapArgs, divided);
    }

    GpuImageView filtered;
    filtered.bounds = rect;
    filtered.nComps = nComps;
    filtered.rowStrideFloats = rect.width() * nComps;
    const uint64_t filteredFloats = (uint64_t)rect.area() * nComps;
    if ((filteredFloats > maxFloats) || !ctx.scratch(filteredFloats * sizeof(float), filtered.buffer) || !filtered.buffer) {
        return eStatusFailed;
    }
    const int fullWidth = (r.fullWidth > 0) ? r.fullWidth : rodPixel.width();
    const int fullHeight = (r.fullHeight > 0) ? r.fullHeight : rodPixel.height();
    ToneEqualizerGpu::Params params;
    params.settings = s;
    params.factors = r.factors;
    params.output = r.output;
    params.radius = maskRadius(s.blending, fullWidth, fullHeight, renderScale);
    // The mask runs over the whole frame for every strip; there is no device frame cache to
    // share it through, as the CPU path's mask is.
    if (!ToneEqualizerGpu::record(ctx, divided, rodPixel, params, filtered)) {
        return eStatusFailed;
    }
    ctx.computeBarrier();

    return recordStagesGpuFinish(plan, wrapArgs, filtered);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // ToneEqualizer::renderOpGpu

NATRON_NAMESPACE_EXIT
