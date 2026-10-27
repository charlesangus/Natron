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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/denoiseprofile.c, with what it calls from src/common/nlmeans_core.c,
 * src/common/eaw.c and src/common/math.h. darktable is distributed under the GNU GPL, version 3
 * or (at your option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2011-2026 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DenoiseProfiled.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

using namespace DenoiseProfiledKernels;

namespace {
inline constexpr InputSpec kDenoiseProfiledInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kDenoiseProfiledNodeSpec = {
    .id = PLUGINID_NATRON_DENOISEPROFILED,
    .label = "DenoiseProfiled",
    .description = "Removes sensor noise using the noise profile of the camera at the plate's ISO, as darktable's denoise (profiled) "
                   "module does.\n"
                   "\n"
                   "The profile is found from the raw (or exif) make, model and ISO; without one, the generic Poissonian profile "
                   "and the manual Profile A and Profile B apply. Profiles are measured on camera RGB, so the node belongs after "
                   "RawDemosaic and before RawInputColor.\n"
                   "\n"
                   "Non-local means works best for lightness, wavelets for colour noise. Mix and the mask blend the result with "
                   "the input.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_DENOISEPROFILED,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/DenoiseProfiled.png",
    .inputs = kDenoiseProfiledInputs,
};

constexpr ImagePipelineSpec kDenoiseProfiledPipeline = {
    .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Custom,
};

const int kRgbCurveCount = 4;
const int kY0U0V0CurveCount = 2;

bool
isAuto(ModeEnum mode)
{
    return (mode == eModeNlmAuto) || (mode == eModeWaveletsAuto);
}

bool
isWavelets(ModeEnum mode)
{
    return (mode == eModeWavelets) || (mode == eModeWaveletsAuto);
}

std::string
metadataString(const ImageMetadata& metadata,
               const std::optional<std::string>& raw,
               const char* exifKey)
{
    if (raw && !raw->empty()) {
        return *raw;
    }

    return metadata.getString(exifKey).value_or(std::string());
}

std::optional<double>
exifNumber(const ImageMetadata& metadata,
           const char* key)
{
    if (const std::optional<double> d = metadata.getDouble(key)) {
        return d;
    }
    if (const std::optional<int> i = metadata.getInt(key)) {
        return (double)*i;
    }
    if (const std::optional<std::vector<int>> v = metadata.getIntVector(key)) {
        if (!v->empty()) {
            return (double)v->front();
        }
    }

    return std::nullopt;
}

void
addCurves(const KnobParametricPtr& knob,
          int nCurves,
          const char* const* names,
          const double (*colours)[3])
{
    knob->setParametricRange(0., 1.);
    knob->setMinimumsAndMaximums(std::vector<double>(nCurves, 0.), std::vector<double>(nCurves, 1.));
    knob->setDisplayMinimumsAndMaximums(std::vector<double>(nCurves, 0.), std::vector<double>(nCurves, 1.));
    for (int curve = 0; curve < nCurves; ++curve) {
        knob->setDimensionName(curve, names[curve]);
        knob->setCurveColor(curve, colours[curve][0], colours[curve][1], colours[curve][2]);
        for (int k = 0; k < kBands; ++k) {
            const StatusEnum status = knob->addControlPoint(eValueChangedReasonPluginEdited, curve, k / (double)(kBands - 1), 0.5, eKeyframeTypeSmooth);
            Q_UNUSED(status);
        }
    }
    knob->setDefaultCurvesFromCurves();
}

void
sampleCurves(const KnobParametricPtr& knob,
             int nCurves,
             int firstCurve,
             DenoiseProfiledKernels::Settings* settings)
{
    for (int curve = 0; curve < nCurves; ++curve) {
        for (int k = 0; k < kBands; ++k) {
            double v = 0.5;
            if (knob) {
                const StatusEnum status = knob->getValue(curve, k / (double)(kBands - 1), &v);
                Q_UNUSED(status);
            }
            settings->force[firstCurve + curve][k] = quantizeForce(v);
        }
    }
}

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
} // namespace

struct DenoiseProfiled::Resolved {
    DenoiseProfiledKernels::Settings settings;
    // The full-scale size of the source's larger side, which sets the wavelet scales.
    int fullSize = 0;
    std::string profileName;
    // What the node tells the user, empty when nothing.
    std::string message;
};

DenoiseProfiled::DenoiseProfiled(NodePtr node)
    : StagedImageEffect(node)
    , _profilesMutex()
    , _profilesValid(false)
    , _profilesKey()
    , _profiles()
    , _frameMutex()
    , _frameValid(false)
    , _frameKey()
    , _frame()
    , _message()
{
}

DenoiseProfiled::~DenoiseProfiled()
{
}

const NodeSpec*
DenoiseProfiled::nodeSpec() const
{
    return staticNodeSpec<kDenoiseProfiledNodeSpec>();
}

const ImagePipelineSpec&
DenoiseProfiled::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kDenoiseProfiledPipeline, kDenoiseProfiledNodeSpec>();
}

DenoiseProfiled::Camera
DenoiseProfiled::cameraOf(const ImageMetadata& metadata)
{
    Camera camera;

    camera.make = metadataString(metadata, RawMetadata::getMake(metadata), "exif/Make");
    camera.model = metadataString(metadata, RawMetadata::getModel(metadata), "exif/Model");
    std::optional<double> iso = RawMetadata::getIso(metadata);
    if (!iso) {
        iso = exifNumber(metadata, "exif/PhotographicSensitivity");
    }
    if (!iso) {
        iso = exifNumber(metadata, "exif/ISOSpeedRatings");
    }
    camera.iso = (iso && (*iso > 0.)) ? (int)std::lround(*iso) : 0;
    camera.isoShift = NoiseProfiles::highlightPreservationShift(RawMetadata::getHighlightPreservation(metadata).value_or(0.));

    return camera;
}

std::string
DenoiseProfiled::cameraSpaceNotice(const ImageMetadata& metadata)
{
    if (RawMetadata::getStage(metadata)) {
        return tr("The input is a raw mosaic: demosaic it first, with RawDemosaic, so that the colour channels are filled in.").toStdString();
    }
    if (!RawMetadata::getMake(metadata)) {
        return tr("The input does not come from a raw file. Noise profiles are measured on camera RGB before the input colour transform, so the noise is only roughly evened out.").toStdString();
    }
    if (RawMetadata::getCatApplied(metadata)) {
        return tr("The input has been through a chromatic adaptation. Noise profiles are measured on camera RGB, before RawInputColor; denoise there for the best result.").toStdString();
    }

    return std::string();
}

std::vector<NoiseProfiles::Profile>
DenoiseProfiled::profilesFor(const Camera& camera)
{
    const std::string key = camera.make + '\n' + camera.model;
    std::lock_guard<std::mutex> lock(_profilesMutex);

    if (!_profilesValid || (_profilesKey != key)) {
        _profiles = NoiseProfiles::matching(NoiseProfiles::database(), camera.make, camera.model);
        _profilesKey = key;
        _profilesValid = true;
    }

    return _profiles;
}

void
DenoiseProfiled::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobChoicePtr profile = createKnob<KnobChoice>(tr("Profile").toStdString());
    profile->setName(kDenoiseProfiledParamProfile);
    profile->setHintToolTip(tr("Profile used for variance stabilization. Auto finds the camera's profile at the plate's ISO, "
                               "interpolating between the ISOs profiled; Generic uses Profile A and Profile B; the other entries are the "
                               "camera's profiles.")
                                .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDenoiseProfiledParamProfileAuto, tr("Auto").toStdString(), std::string()));
        options.push_back(ChoiceOption(kDenoiseProfiledParamProfileGeneric, tr("Generic Poissonian").toStdString(), std::string()));
        profile->populateChoices(options);
    }
    profile->setAnimationEnabled(false);
    page->addKnob(profile);
    _profile = profile;

    KnobStringPtr profileUsed = createKnob<KnobString>(tr("Profile Used").toStdString());
    profileUsed->setName(kDenoiseProfiledParamProfileUsed);
    profileUsed->setHintToolTip(tr("The profile the node applies: the camera's at the plate's ISO, an interpolation between two "
                                   "ISOs, or the generic profile.")
                                    .toStdString());
    profileUsed->setAsLabel();
    profileUsed->setEvaluateOnChange(false);
    profileUsed->setIsPersistent(false);
    page->addKnob(profileUsed);
    _profileUsed = profileUsed;

    // darktable shows it only for a plate whose camera held exposure back by a whole EV or more.
    KnobBoolPtr compensate = createKnob<KnobBool>(tr("Compensate Highlight Preservation").toStdString());
    compensate->setName(kDenoiseProfiledParamCompensateHighlightPreservation);
    compensate->setHintToolTip(tr("If enabled, reduces the ISO used for denoise by the factor of the camera's hidden exposure bias "
                                  "used in HDR / highlight preservation / dynamic range / HLG tone modes.")
                                   .toStdString());
    compensate->setDefaultValue(true);
    compensate->setSecret(true);
    page->addKnob(compensate);
    _compensateHighlightPreservation = compensate;

    const char* const channelNames[3] = { "r", "g", "b" };
    KnobDoublePtr profileA = createKnob<KnobDouble>(tr("Profile A").toStdString(), 3);
    profileA->setName(kDenoiseProfiledParamProfileA);
    profileA->setHintToolTip(tr("The Poissonian (signal-dependent) part of the noise per channel, used with the Generic profile. "
                                "The transforms use the green value.")
                                 .toStdString());
    KnobDoublePtr profileB = createKnob<KnobDouble>(tr("Profile B").toStdString(), 3);
    profileB->setName(kDenoiseProfiledParamProfileB);
    profileB->setHintToolTip(tr("The Gaussian (constant) part of the noise per channel, used with the Generic profile. The "
                                "transforms use the green value.")
                                 .toStdString());
    for (int c = 0; c < 3; ++c) {
        profileA->setDimensionName(c, channelNames[c]);
        profileA->setDefaultValue(NoiseProfiles::generic().a[c], c);
        profileA->setMinimum(1e-10, c);
        profileA->setMaximum(1., c);
        profileA->setDisplayMinimum(0., c);
        profileA->setDisplayMaximum(0.01, c);
        profileA->setDecimals(8, c);
        profileB->setDimensionName(c, channelNames[c]);
        profileB->setDefaultValue(NoiseProfiles::generic().b[c], c);
        profileB->setMinimum(-1., c);
        profileB->setMaximum(1., c);
        profileB->setDisplayMinimum(-0.001, c);
        profileB->setDisplayMaximum(0.001, c);
        profileB->setDecimals(8, c);
    }
    page->addKnob(profileA);
    page->addKnob(profileB);
    _profileA = profileA;
    _profileB = profileB;

    KnobBoolPtr wbAdaptive = createKnob<KnobBool>(tr("WB Adaptive").toStdString());
    wbAdaptive->setName(kDenoiseProfiledParamWbAdaptive);
    wbAdaptive->setHintToolTip(tr("Adapt denoising according to the white balance coefficients. Should be enabled on a first "
                                  "instance for better denoising; should be disabled if an earlier instance has been used with a "
                                  "colour blending mode.")
                                   .toStdString());
    wbAdaptive->setDefaultValue(true);
    page->addKnob(wbAdaptive);
    _wbAdaptive = wbAdaptive;

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode").toStdString());
    mode->setName(kDenoiseProfiledParamMode);
    mode->setHintToolTip(tr("Method used in the denoising core. Non-local means works best for lightness blending, wavelets "
                            "work best for colour blending. The auto modes set the patch size, scattering, preserve shadows and "
                            "bias correction from the profile.")
                             .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDenoiseProfiledParamModeNlm, tr("Non-Local Means").toStdString(), std::string()));
        options.push_back(ChoiceOption(kDenoiseProfiledParamModeNlmAuto, tr("Non-Local Means Auto").toStdString(), std::string()));
        options.push_back(ChoiceOption(kDenoiseProfiledParamModeWavelets, tr("Wavelets").toStdString(), std::string()));
        options.push_back(ChoiceOption(kDenoiseProfiledParamModeWaveletsAuto, tr("Wavelets Auto").toStdString(), std::string()));
        options.push_back(ChoiceOption(kDenoiseProfiledParamModeVariance, tr("Compute Variance").toStdString(), tr("Passes the image through and measures the variance of the stabilised image.").toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeWavelets);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    KnobIntPtr patchSize = createKnob<KnobInt>(tr("Patch Size").toStdString());
    patchSize->setName(kDenoiseProfiledParamPatchSize);
    patchSize->setHintToolTip(tr("Radius of the patches to match. Increase for more sharpness on strong edges, and better "
                                 "denoising of smooth areas. If details are oversmoothed, reduce this value or increase the central "
                                 "pixel weight.")
                                  .toStdString());
    patchSize->setDefaultValue(1);
    patchSize->setMinimum(0);
    patchSize->setMaximum(12);
    patchSize->setDisplayMinimum(0);
    patchSize->setDisplayMaximum(8);
    page->addKnob(patchSize);
    _patchSize = patchSize;

    KnobIntPtr searchRadius = createKnob<KnobInt>(tr("Search Radius").toStdString());
    searchRadius->setName(kDenoiseProfiledParamSearchRadius);
    searchRadius->setHintToolTip(tr("Emergency use only: radius of the neighbourhood to search patches in. Increase for better "
                                    "denoising performance, but watch the long runtimes: large radii can be very slow.")
                                     .toStdString());
    searchRadius->setDefaultValue(7);
    searchRadius->setMinimum(1);
    searchRadius->setMaximum(30);
    searchRadius->setDisplayMinimum(1);
    searchRadius->setDisplayMaximum(30);
    page->addKnob(searchRadius);
    _searchRadius = searchRadius;

    KnobDoublePtr scattering = createKnob<KnobDouble>(tr("Scattering").toStdString());
    scattering->setName(kDenoiseProfiledParamScattering);
    scattering->setHintToolTip(tr("Scattering of the neighbourhood to search patches in. Increase for better coarse-grain noise "
                                  "reduction. Does not affect execution time.")
                                   .toStdString());
    scattering->setDefaultValue(0.);
    scattering->setMinimum(0.);
    scattering->setMaximum(20.);
    scattering->setDisplayMinimum(0.);
    scattering->setDisplayMaximum(1.);
    page->addKnob(scattering);
    _scattering = scattering;

    KnobDoublePtr centralPixelWeight = createKnob<KnobDouble>(tr("Central Pixel Weight").toStdString());
    centralPixelWeight->setName(kDenoiseProfiledParamCentralPixelWeight);
    centralPixelWeight->setHintToolTip(tr("Increase the weight of the central pixel of the patch in the patch comparison. Useful "
                                          "to recover details when the patch size is quite big.")
                                           .toStdString());
    centralPixelWeight->setDefaultValue(0.1);
    centralPixelWeight->setMinimum(0.);
    centralPixelWeight->setMaximum(10.);
    centralPixelWeight->setDisplayMinimum(0.);
    centralPixelWeight->setDisplayMaximum(1.);
    page->addKnob(centralPixelWeight);
    _centralPixelWeight = centralPixelWeight;

    KnobChoicePtr colorMode = createKnob<KnobChoice>(tr("Color Mode").toStdString());
    colorMode->setName(kDenoiseProfiledParamColorMode);
    colorMode->setHintToolTip(tr("Colour representation used within the wavelets. RGB keeps the RGB channels separated, while "
                                 "Y0U0V0 combines the channels to denoise chroma and luma separately.")
                                  .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDenoiseProfiledParamColorModeRgb, "RGB", std::string()));
        options.push_back(ChoiceOption(kDenoiseProfiledParamColorModeY0U0V0, "Y0U0V0", std::string()));
        colorMode->populateChoices(options);
    }
    colorMode->setDefaultValue((int)eColorModeY0U0V0);
    colorMode->setAnimationEnabled(false);
    page->addKnob(colorMode);
    _colorMode = colorMode;

    KnobParametricPtr rgbCurves = createKnob<KnobParametric>(tr("RGB Curves").toStdString(), kRgbCurveCount);
    rgbCurves->setName(kDenoiseProfiledParamRgbCurves);
    rgbCurves->setHintToolTip(tr("How strongly each wavelet band is denoised, from the coarsest on the left to the finest on the "
                                 "right, for all channels and on top of that for red, green and blue. 0.5 is neutral.")
                                  .toStdString());
    {
        const char* const names[kRgbCurveCount] = { "all", "red", "green", "blue" };
        const double colours[kRgbCurveCount][3] = { { 0.7, 0.7, 0.7 }, { 1., 0.3, 0.3 }, { 0.3, 1., 0.3 }, { 0.4, 0.4, 1. } };
        addCurves(rgbCurves, kRgbCurveCount, names, colours);
    }
    page->addKnob(rgbCurves);
    _rgbCurves = rgbCurves;

    KnobParametricPtr y0u0v0Curves = createKnob<KnobParametric>(tr("Y0U0V0 Curves").toStdString(), kY0U0V0CurveCount);
    y0u0v0Curves->setName(kDenoiseProfiledParamY0U0V0Curves);
    y0u0v0Curves->setHintToolTip(tr("How strongly each wavelet band is denoised, from the coarsest on the left to the finest on "
                                    "the right, for luma (Y0) and chroma (U0V0). 0.5 is neutral.")
                                     .toStdString());
    {
        const char* const names[kY0U0V0CurveCount] = { "Y0", "U0V0" };
        const double colours[kY0U0V0CurveCount][3] = { { 0.7, 0.7, 0.7 }, { 0.8, 0.4, 0.9 } };
        addCurves(y0u0v0Curves, kY0U0V0CurveCount, names, colours);
    }
    page->addKnob(y0u0v0Curves);
    _y0u0v0Curves = y0u0v0Curves;

    KnobDoublePtr adjustAutoset = createKnob<KnobDouble>(tr("Adjust Autoset").toStdString());
    adjustAutoset->setName(kDenoiseProfiledParamAdjustAutoset);
    adjustAutoset->setHintToolTip(tr("Controls the way parameters are autoset. Increase if shadows are not denoised enough or if "
                                     "chroma noise remains; this can happen if the image is underexposed.")
                                      .toStdString());
    adjustAutoset->setDefaultValue(1.);
    adjustAutoset->setMinimum(0.001);
    adjustAutoset->setMaximum(1000.);
    adjustAutoset->setDisplayMinimum(0.001);
    adjustAutoset->setDisplayMaximum(4.);
    page->addKnob(adjustAutoset);
    _adjustAutoset = adjustAutoset;

    KnobDoublePtr strength = createKnob<KnobDouble>(tr("Strength").toStdString());
    strength->setName(kDenoiseProfiledParamStrength);
    strength->setHintToolTip(tr("Finetune denoising strength.").toStdString());
    strength->setDefaultValue(1.);
    strength->setMinimum(0.001);
    strength->setMaximum(1000.);
    strength->setDisplayMinimum(0.001);
    strength->setDisplayMaximum(4.);
    strength->setDecimals(3);
    page->addKnob(strength);
    _strength = strength;

    KnobDoublePtr preserveShadows = createKnob<KnobDouble>(tr("Preserve Shadows").toStdString());
    preserveShadows->setName(kDenoiseProfiledParamPreserveShadows);
    preserveShadows->setHintToolTip(tr("Finetune shadows denoising. Decrease to denoise dark areas of the image more "
                                       "aggressively.")
                                        .toStdString());
    preserveShadows->setDefaultValue(1.);
    preserveShadows->setMinimum(0.);
    preserveShadows->setMaximum(1.8);
    preserveShadows->setDisplayMinimum(0.);
    preserveShadows->setDisplayMaximum(1.8);
    page->addKnob(preserveShadows);
    _preserveShadows = preserveShadows;

    KnobDoublePtr biasCorrection = createKnob<KnobDouble>(tr("Bias Correction").toStdString());
    biasCorrection->setName(kDenoiseProfiledParamBiasCorrection);
    biasCorrection->setHintToolTip(tr("Correct colour cast in shadows. Decrease if shadows are too purple, increase if shadows "
                                      "are too green.")
                                       .toStdString());
    biasCorrection->setDefaultValue(0.);
    biasCorrection->setMinimum(-1000.);
    biasCorrection->setMaximum(100.);
    biasCorrection->setDisplayMinimum(-10.);
    biasCorrection->setDisplayMaximum(10.);
    page->addKnob(biasCorrection);
    _biasCorrection = biasCorrection;

    KnobStringPtr variance = createKnob<KnobString>(tr("Variance").toStdString());
    variance->setName(kDenoiseProfiledParamVariance);
    variance->setHintToolTip(tr("Use only with a perfectly uniform image if you want to estimate the noise variance: the red, "
                                "green and blue variance of the stabilised image over the last region rendered, 1 where the "
                                "profile fits.")
                                 .toStdString());
    variance->setAsLabel();
    variance->setEvaluateOnChange(false);
    variance->setIsPersistent(false);
    page->addKnob(variance);
    _variance = variance;

    addStageKnobs(page);
    refreshVisibility();
} // DenoiseProfiled::initializeKnobs

void
DenoiseProfiled::refreshVisibility()
{
    const KnobChoicePtr modeKnob = _mode.lock();
    const KnobChoicePtr colorModeKnob = _colorMode.lock();
    const KnobChoicePtr profileKnob = _profile.lock();

    if (!modeKnob || !colorModeKnob || !profileKnob) {
        return;
    }
    const ModeEnum mode = (ModeEnum)modeKnob->getValue();
    const bool rgb = (colorModeKnob->getValue() == (int)eColorModeRgb);
    const bool generic = (profileKnob->getActiveEntry().id == kDenoiseProfiledParamProfileGeneric);
    const auto show = [](const KnobIPtr& knob, bool visible) {
        if (knob) {
            knob->setSecret(!visible);
        }
    };

    show(_patchSize.lock(), mode == eModeNlm);
    show(_searchRadius.lock(), mode == eModeNlm);
    show(_scattering.lock(), mode == eModeNlm);
    show(_centralPixelWeight.lock(), (mode == eModeNlm) || (mode == eModeNlmAuto));
    show(_colorMode.lock(), isWavelets(mode));
    show(_rgbCurves.lock(), isWavelets(mode) && rgb);
    show(_y0u0v0Curves.lock(), isWavelets(mode) && !rgb);
    show(_adjustAutoset.lock(), isAuto(mode));
    show(_preserveShadows.lock(), !isAuto(mode));
    show(_biasCorrection.lock(), !isAuto(mode));
    show(_profileA.lock(), generic);
    show(_profileB.lock(), generic);
    show(_variance.lock(), mode == eModeVariance);
}

void
DenoiseProfiled::refreshProfiles()
{
    const KnobChoicePtr profileKnob = _profile.lock();

    if (!profileKnob) {
        return;
    }
    const ImageMetadata source = getSourceMetadata(getCurrentTime(), ViewIdx(0));
    const Camera camera = cameraOf(source);
    const std::vector<NoiseProfiles::Profile> profiles = profilesFor(camera);

    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kDenoiseProfiledParamProfileAuto, tr("Auto").toStdString(), std::string()));
    options.push_back(ChoiceOption(kDenoiseProfiledParamProfileGeneric, tr("Generic Poissonian").toStdString(), std::string()));
    for (const NoiseProfiles::Profile& p : profiles) {
        options.push_back(ChoiceOption(p.name, p.name, std::string()));
    }
    const std::vector<ChoiceOption> current = profileKnob->getEntries_mt_safe();
    const bool same = std::equal(current.begin(), current.end(), options.begin(), options.end(),
                                 [](const ChoiceOption& a, const ChoiceOption& b) { return a.id == b.id; });
    if (!same) {
        // Pins the chosen name first, so the new menu restores it by name, not by position.
        (void)profileKnob->getActiveEntry();
        profileKnob->populateChoices(options);
    }

    // darktable's reload_defaults(): the defaults follow the camera's profile.
    const KnobBoolPtr compensate = _compensateHighlightPreservation.lock();
    if (compensate) {
        compensate->setSecret(camera.isoShift <= 0);
    }
    const int shift = (compensate && compensate->getValue()) ? camera.isoShift : 0;
    const float a = NoiseProfiles::autoProfile(profiles, camera.iso, shift).profile.a[1];
    if (KnobIntPtr knob = _patchSize.lock()) {
        if (!knob->hasModifications(0)) {
            knob->setDefaultValue(inferRadius(a));
        }
    }
    const std::pair<KnobDoubleWPtr, double> derived[3] = {
        { _scattering, inferScattering(a) },
        { _preserveShadows, inferShadows(a) },
        { _biasCorrection, inferBias(a) },
    };
    for (const std::pair<KnobDoubleWPtr, double>& d : derived) {
        if (KnobDoublePtr knob = d.first.lock()) {
            if (!knob->hasModifications(0)) {
                knob->setDefaultValue(d.second);
            }
        }
    }

    if (KnobStringPtr used = _profileUsed.lock()) {
        const std::optional<Resolved> r = resolve(getCurrentTime(), ViewIdx(0));
        used->setValue(r ? r->profileName : std::string());
    }
    refreshVisibility();
} // DenoiseProfiled::refreshProfiles

bool
DenoiseProfiled::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    const KnobIPtr mode = _mode.lock();
    const KnobIPtr colorMode = _colorMode.lock();
    const KnobIPtr profile = _profile.lock();
    const KnobIPtr profileA = _profileA.lock();
    const KnobIPtr profileB = _profileB.lock();
    const KnobIPtr compensate = _compensateHighlightPreservation.lock();

    if (compensate && (k == compensate.get())) {
        refreshProfiles();

        return true;
    }
    if ((k == mode.get()) || (k == colorMode.get())) {
        refreshVisibility();

        return true;
    }
    if ((k == profile.get()) || (k == profileA.get()) || (k == profileB.get())) {
        refreshVisibility();
        if (KnobStringPtr used = _profileUsed.lock()) {
            const std::optional<Resolved> r = resolve(getCurrentTime(), ViewIdx(0));
            used->setValue(r ? r->profileName : std::string());
        }

        return true;
    }

    return false;
}

void
DenoiseProfiled::onKnobsLoaded()
{
    refreshProfiles();
}

void
DenoiseProfiled::onInputChanged(int inputNo)
{
    StagedImageEffect::onInputChanged(inputNo);
    refreshProfiles();
}

void
DenoiseProfiled::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    StagedImageEffect::onCurrentFrameMetadataChanged(metadata);
    refreshProfiles();
}

void
DenoiseProfiled::purgeCaches()
{
    {
        std::lock_guard<std::mutex> lock(_profilesMutex);
        _profilesValid = false;
        _profiles.clear();
    }
    std::lock_guard<std::mutex> lock(_frameMutex);
    _frameValid = false;
    _frame.reset();
}

std::optional<std::array<double, 3>>
DenoiseProfiled::lastVariance() const
{
    std::lock_guard<std::mutex> lock(_varianceMutex);

    return _lastVariance;
}

void
DenoiseProfiled::showVariance(const std::array<double, 3>& variance)
{
    {
        std::lock_guard<std::mutex> lock(_varianceMutex);
        _lastVariance = variance;
    }
    const std::string text = tr("red %1, green %2, blue %3")
                                 .arg(variance[0], 0, 'f', 2)
                                 .arg(variance[1], 0, 'f', 2)
                                 .arg(variance[2], 0, 'f', 2)
                                 .toStdString();
    const KnobStringWPtr weak = _variance;
    const auto apply = [weak, text]() {
        if (const KnobStringPtr knob = weak.lock()) {
            knob->setValue(text);
        }
    };
    // Renders run off the main thread, and a knob change signals the GUI.
    if (!qApp) {
        return;
    }
    if (QThread::currentThread() == qApp->thread()) {
        apply();
    } else {
        QMetaObject::invokeMethod(qApp, apply, Qt::QueuedConnection);
    }
}

std::optional<DenoiseProfiled::Resolved>
DenoiseProfiled::resolve(double time,
                         ViewIdx view)
{
    const KnobChoicePtr profileKnob = _profile.lock();

    if (!profileKnob) {
        return std::nullopt;
    }
    Resolved r;
    DenoiseProfiledKernels::Settings& s = r.settings;
    const ImageMetadata source = getSourceMetadata(time, view);
    const Camera camera = cameraOf(source);
    const KnobReader reader(time, view);
    std::vector<std::string> notes;

    // The profile: darktable reads only the green fit in the transforms.
    const std::vector<NoiseProfiles::Profile> profiles = profilesFor(camera);
    const int shift = reader.value(_compensateHighlightPreservation) ? camera.isoShift : 0;
    const NoiseProfiles::AutoProfile autoProfile = NoiseProfiles::autoProfile(profiles, camera.iso, shift);
    const std::string chosen = profileKnob->getActiveEntry().id;
    if (chosen == kDenoiseProfiledParamProfileGeneric) {
        s.profileA = (float)reader.value(_profileA, 1);
        s.profileB = (float)reader.value(_profileB, 1);
        r.profileName = tr("generic, a %1, b %2").arg(s.profileA).arg(s.profileB).toStdString();
    } else {
        const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const NoiseProfiles::Profile& p) { return p.name == chosen; });
        const NoiseProfiles::Profile& p = (found != profiles.end()) ? *found : autoProfile.profile;
        s.profileA = p.a[1];
        s.profileB = p.b[1];
        if (found != profiles.end()) {
            r.profileName = p.name;
        } else {
            r.profileName = autoProfile.name;
            if (!autoProfile.detected) {
                if (NoiseProfiles::database().empty()) {
                    notes.push_back(tr("The noise profile database could not be read; the generic profile applies.").toStdString());
                } else if (profiles.empty()) {
                    notes.push_back(tr("No noise profile for make \"%1\", model \"%2\"; the generic profile applies.")
                                        .arg(QString::fromUtf8(camera.make.c_str()), QString::fromUtf8(camera.model.c_str()))
                                        .toStdString());
                } else {
                    notes.push_back(tr("No noise profile of make \"%1\", model \"%2\" covers ISO %3; the generic profile applies.")
                                        .arg(QString::fromUtf8(camera.make.c_str()), QString::fromUtf8(camera.model.c_str()))
                                        .arg(camera.iso)
                                        .toStdString());
                }
            }
        }
    }
    const std::string notice = cameraSpaceNotice(source);
    if (!notice.empty()) {
        notes.push_back(notice);
    }
    for (const std::string& n : notes) {
        r.message += (r.message.empty() ? std::string() : std::string("\n")) + n;
    }

    s.mode = (ModeEnum)std::clamp((int)reader.value(_mode), 0, (int)eModeVariance);
    s.colorMode = (reader.value(_colorMode) == (int)eColorModeRgb) ? eColorModeRgb : eColorModeY0U0V0;
    s.searchRadius = (int)reader.value(_searchRadius);
    s.centralPixelWeight = (float)reader.value(_centralPixelWeight);
    s.strength = (float)reader.value(_strength);
    if (isAuto(s.mode)) {
        const float gain = (float)reader.value(_adjustAutoset);
        s.radius = (float)inferRadius(s.profileA * gain);
        s.scattering = inferScattering(s.profileA * gain);
        s.shadows = inferShadows(s.profileA * gain);
        s.bias = inferBias(s.profileA * gain);
    } else {
        s.radius = (float)reader.value(_patchSize);
        s.scattering = (float)reader.value(_scattering);
        s.shadows = (float)reader.value(_preserveShadows);
        s.bias = (float)reader.value(_biasCorrection);
    }
    s.wb = wbFactors(RawMetadata::getWbApplied(source), (bool)reader.value(_wbAdaptive));
    sampleCurves(_rgbCurves.lock(), kRgbCurveCount, eCurveAll, &s);
    sampleCurves(_y0u0v0Curves.lock(), kY0U0V0CurveCount, eCurveY0, &s);

    RectI fullRoD;
    if (RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &fullRoD)) {
        r.fullSize = std::max(fullRoD.width(), fullRoD.height());
    }

    return r;
} // DenoiseProfiled::resolve

bool
DenoiseProfiled::isOpIdentity(double time,
                              const RenderScale& /*scale*/,
                              const RectI& /*roi*/,
                              ViewIdx view)
{
    const std::optional<Resolved> r = resolve(time, view);

    if (r && !r->message.empty()) {
        _message.post(getNode(), eMessageTypeWarning, r->message);
    } else {
        _message.withdraw(getNode());
    }

    return false;
}

void
DenoiseProfiled::getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    const std::optional<Resolved> r = resolve(time, view);
    if (!input || !r) {
        return;
    }
    if (isWavelets(r->settings.mode)) {
        RectD sourceRoD;
        bool isProjectFormat = false;
        if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &sourceRoD, &isProjectFormat) != eStatusFailed) {
            (*ret)[input] = sourceRoD;
        }

        return;
    }
    if (r->settings.mode == eModeVariance) {
        return;
    }
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(getSourceInput());
    const int halo = nlmGeometry(r->settings, scale.toOfxPointD().x).halo();
    const RectI window = PassExec::toPixelEnclosing(renderWindow, scale, par);
    const RectI need(window.x1 - halo, window.y1 - halo, window.x2 + halo, window.y2 + halo);
    (*ret)[input] = PassExec::toCanonical(need, scale, par);
}

StatusEnum
DenoiseProfiled::renderOp(const RenderActionArgs& args,
                          const StagePlan& plan)
{
    using namespace PassExec;

    const std::optional<Resolved> found = resolve(args.time, args.view);
    if (!found) {
        return eStatusFailed;
    }
    const Resolved& r = *found;
    const DenoiseProfiledKernels::Settings& s = r.settings;
    const int sourceInput = plan.sourceInput;
    const double renderScale = args.mappedScale.toOfxPointD().x;

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
        if ((sourceInput >= 0) && getInput(sourceInput) && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
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
    bool processing = (plan.mix != 0.) && intersectRects(roi, rodPixel, &window) && !isEmptyRect(window);
    RectI bufferRect(0, 0, 0, 0);
    if (processing) {
        if (isWavelets(s.mode)) {
            bufferRect = rodPixel;
        } else if (s.mode == eModeVariance) {
            bufferRect = window;
        } else {
            const int halo = nlmGeometry(s, renderScale).halo();
            processing = intersectRects(RectI(window.x1 - halo, window.y1 - halo, window.x2 + halo, window.y2 + halo), rodPixel, &bufferRect);
        }
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    const ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, nThreads);
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
            std::vector<float> rgb(planeSize * 3);
            parallel(bh, [&](int y) {
                for (int c = 0; c < 3; ++c) {
                    const float* plane = filled.data() + (std::size_t)fillPlanes.planeOfChannel[c] * planeSize + (std::size_t)y * bw;
                    float* line = rgb.data() + (std::size_t)y * bw * 3;
                    for (int x = 0; x < bw; ++x) {
                        line[3 * x + c] = plane[x];
                    }
                }
            });

            const int ww = window.width();
            const int wh = window.height();
            const int wx = window.x1 - bufferRect.x1;
            const int wy = window.y1 - bufferRect.y1;
            std::vector<float> denoised;
            const float* out = NULL;
            int outStride = ww;
            int outX = 0;
            int outY = 0;
            std::shared_ptr<const std::vector<float>> frame;
            if (s.mode == eModeVariance) {
                if (j == 0) {
                    showVariance(measureVariance(rgb.data(), planeSize, s));
                }
                out = rgb.data();
                outStride = bw;
                outX = wx;
                outY = wy;
            } else if (isWavelets(s.mode)) {
                const std::string key = std::to_string(RawNodeSupport::frameKey(getRenderHash(), args.time, args.view)) + '|' + std::to_string(renderScale) + '|' + layerIds[j]
                    + '|' + std::to_string(bufferRect.x1) + ',' + std::to_string(bufferRect.y1) + ',' + std::to_string(bw) + ',' + std::to_string(bh);
                std::lock_guard<std::mutex> lock(_frameMutex);
                if (!_frameValid || (_frameKey != key)) {
                    RawNodeSupport::noteFrameComputed(kDenoiseProfiledFrameWavelets, RawNodeSupport::frameKey(getRenderHash(), args.time, args.view));
                    _frame.reset();
                    std::shared_ptr<std::vector<float>> computed = std::make_shared<std::vector<float>>(planeSize * 3);
                    denoiseWavelets(rgb.data(), bw, bh, (r.fullSize > 0) ? r.fullSize : std::max(bw, bh), s, renderScale, computed->data(), parallel);
                    _frame = std::move(computed);
                    _frameKey = key;
                    _frameValid = !cancel.check();
                }
                frame = _frame;
                out = frame->data();
                outStride = bw;
                outX = wx;
                outY = wy;
            } else {
                denoised.resize((std::size_t)ww * wh * 3);
                denoiseNlm(rgb.data(), bw, bh, wx, wy, ww, wh, s, renderScale, denoised.data(), parallel);
                out = denoised.data();
            }
            if (cancel.check()) {
                return eStatusOK;
            }

            const std::size_t windowSize = (std::size_t)ww * wh;
            result.resize(windowSize * planes.count());
            for (int p = 0; p < planes.count(); ++p) {
                const int c = planes.channel[p];
                float* dstPlane = result.data() + (std::size_t)p * windowSize;
                parallel(wh, [&](int y) {
                    float* line = dstPlane + (std::size_t)y * ww;
                    if (c < 3) {
                        const float* src = out + ((std::size_t)(y + outY) * outStride + outX) * 3 + c;
                        for (int x = 0; x < ww; ++x) {
                            line[x] = src[3 * x];
                        }
                    } else {
                        const float* src = filled.data() + (std::size_t)fillPlanes.planeOfChannel[c] * planeSize + (std::size_t)(y + wy) * bw + wx;
                        std::memcpy(line, src, sizeof(float) * ww);
                    }
                });
            }
            layout.buffer = result.data();
        }
        writeWindow(job, planes, maskSource, layout, nThreads, cancel);
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // DenoiseProfiled::renderOp

NATRON_NAMESPACE_EXIT
