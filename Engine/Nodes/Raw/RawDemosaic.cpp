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
 * The driver of darktable's demosaic module, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/demosaic.c, Copyright (C) 2010-2026 darktable developers, distributed under the GNU
 * GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawDemosaic.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawDemosaicInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawDemosaicNodeSpec = {
    .id = PLUGINID_NATRON_RAWDEMOSAIC,
    .label = "RawDemosaic",
    .description = "Reconstructs full RGB pixels from the colour filter array of prepared raw data: RCD, PPG, AMaZE, VNG4 or LMMSE "
                   "for a Bayer sensor, Markesteijn, VNG or Frequency Domain Chroma for an X-Trans one, RCD, AMaZE or Markesteijn 3-pass blended with VNG in flat areas (dual), "
                   "or the photosites shown as they are. Data without a colour filter array passes "
                   "through. Outputs an image with an alpha of 1; RawInputColor then takes it to the working space.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWDEMOSAIC,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWDEMOSAIC ".png",
    .outputKind = eDataKindImage,
    .inputs = kRawDemosaicInputs,
};

constexpr ImagePipelineSpec kRawDemosaicPipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
    .rawStage = RawStageRequirement::Prepared,
};

enum class Algorithm {
    PassThrough,
    PPG,
    RCD,
    AMaZE,
    LMMSE,
    VNG,
    Markesteijn1,
    Markesteijn3,
    FDC,
    PhotositeMonochrome,
    PhotositeColor
};

// How far each algorithm reads around a pixel it outputs, so that the pixel does not depend on
// where the buffer it runs over ends: darktable's tiling overlaps, checked bit for bit against
// whole-image runs. AMaZE, LMMSE and FDC instead align their buffers with their own tile grids.
int
algorithmHalo(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::PPG:

        return 8;
    case Algorithm::RCD:

        return 10;
    case Algorithm::VNG:

        return 6;
    case Algorithm::Markesteijn1:

        return 12;
    case Algorithm::Markesteijn3:

        return 18;
    case Algorithm::AMaZE:
    case Algorithm::LMMSE:
    case Algorithm::FDC:
    case Algorithm::PassThrough:
    case Algorithm::PhotositeMonochrome:
    case Algorithm::PhotositeColor:
        break;
    }

    return 0;
}

// How far a pixel of dual demosaicing's blend mask reads the high-frequency output: 1 for the
// Scharr gradient and 4 for darktable's 9x9 blur. The VNG side reads VNG's halo plus one per
// smoothing pass of its 2.
const int kDualMaskReach = 5;
const int kDualLowReach = 6 + 2;

const int kAmazeGrid = 128;
const int kAmazeTileEnd = 144;
const int kMinimumBuffer = 32;

// The buffer [*b0, *b1) AMaZE runs over to output [w0, w1) of an axis of n photosites, its start
// on the tile grid AMaZE lays from the image's first photosite and one block before the window,
// so the tile covering the window's first photosite reads real data before it. endMargin more
// photosites past the last tile's end let a pass run before AMaZE, which leaves the buffer's
// last photosites unprocessed, finish every photosite that tile reads.
void
amazeSpan(int w0,
          int w1,
          int n,
          int endMargin,
          int* b0,
          int* b1)
{
    *b0 = std::max(0, kAmazeGrid * ((w0 / kAmazeGrid) - 1));
    *b1 = std::min(n, kAmazeGrid * ((w1 - 1) / kAmazeGrid) + kAmazeTileEnd + endMargin);
}

// darktable's reload_defaults() Contrast Sensitivity for the source: _get_variance_threshold()
// from the file's white level and ISO.
float
sourceCaptureThreshold(const ImageMetadata& source)
{
    return RawKernels::captureVarianceThreshold(RawMetadata::getSensorWhiteLevel(source).value_or(0.), RawMetadata::getIso(source).value_or(0.));
}

// The frame area _calc_auto_radius() measures, as a region of rod.
RectI
captureRadiusRect(const RectI& rod,
                  RawKernels::CaptureRadiusArea* area)
{
    *area = RawKernels::captureRadiusArea(rod.width(), rod.height());

    return RectI(rod.x1 + area->x, rod.y2 - area->y - area->height, rod.x1 + area->x + area->width, rod.y2 - area->y);
}

const char* const kCaptureRadiusHint = QT_TRANSLATE_NOOP("RawDemosaic",
                                                         "Capture sharpening's radius should reflect the overall Gaussian-type blur of the camera sensor, possibly the anti-aliasing filter and the lens. "
                                                         "Increasing it too far soon leads to artefacts like halos and ringing, especially with many iterations.\n\n"
                                                         "0 measures the radius from the centre of the raw data of each frame.");
const char* const kCaptureThresholdHint = QT_TRANSLATE_NOOP("RawDemosaic",
                                                            "Restricts capture sharpening to areas of high local contrast: increase it to exclude flat areas in very dark or noisy images, "
                                                            "decrease it for well exposed, low noise images. Until overridden it shows the image's default, "
                                                            "from its ISO and whether the sensor has more than 12 bits.");
const char* const kCaptureGroupName = "captureSharpenControls";
} // namespace

struct RawDemosaic::Settings {
    RawLayout layout;
    RectI sourceRoD;
    Algorithm algorithm = Algorithm::PassThrough;
    RawDemosaicGreenEqEnum greenEq = eRawDemosaicGreenEqNone;
    float edgeThreshold = 0.f;
    int smoothing = 0;
    int lmmseRefine = (int)eRawDemosaicLmmseMedian;
    bool dual = false;
    float dualThreshold = 0.2f;
    float wb[3] = { 1.f, 1.f, 1.f };
    float processedMaximum = 1.f;
    float processedMinimum = 1.f;
    float iso = 0.f;
    // Capture sharpening: on when enabled and darktable supports it for the method, mono for data
    // without a CFA of one channel (darktable's true monochrome). capture.radius is the Radius
    // knob's, 0 or less for the automatic one; the threshold is resolved.
    bool capture = false;
    bool mono = false;
    RawKernels::CaptureSettings captureSettings;
    float wbApplied[3] = { 1.f, 1.f, 1.f };
};

RawDemosaic::RawDemosaic(NodePtr node)
    : StagedImageEffect(node)
    , _bayerMethod()
    , _xtransMethod()
    , _greenEq()
    , _edgeThreshold()
    , _colorSmoothing()
    , _lmmseRefine()
    , _dualThreshold()
    , _greenMutex()
    , _greenValid(false)
    , _greenKey(0)
    , _greenRatio(0.)
    , _captureMutex()
    , _captureValid(false)
    , _captureKey(0)
    , _captureRadiusValue(0.)
    , _lastAutoCaptureRadius()
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawDemosaic::~RawDemosaic()
{
}

const NodeSpec*
RawDemosaic::nodeSpec() const
{
    return staticNodeSpec<kRawDemosaicNodeSpec>();
}

const ImagePipelineSpec&
RawDemosaic::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawDemosaicPipeline, kRawDemosaicNodeSpec>();
}

void
RawDemosaic::addAcceptedComponents(int inputNb,
                                   std::list<ImageLayerDesc>* comps)
{
    if (inputNb == -1) {
        comps->push_back(ImageLayerDesc::getRGBAComponents());

        return;
    }
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
}

void
RawDemosaic::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    std::vector<ChoiceOption> bayer;
    bayer.push_back(ChoiceOption("rcd", "RCD", tr("Ratio Corrected Demosaicing: sharp and free of artefacts on most images.").toStdString()));
    bayer.push_back(ChoiceOption("ppg", "PPG", tr("Patterned Pixel Grouping: fast.").toStdString()));
    bayer.push_back(ChoiceOption("amaze", "AMaZE", tr("Aliasing Minimization and Zipper Elimination: the most detail, slower.").toStdString()));
    bayer.push_back(ChoiceOption("vng4", "VNG4", tr("Variable Number of Gradients, four colours: smooth, for images with maze artefacts.").toStdString()));
    bayer.push_back(ChoiceOption("passthroughMonochrome", tr("passthrough (monochrome)").toStdString(), tr("Every photosite's value in all three channels, with no demosaicing.").toStdString()));
    bayer.push_back(ChoiceOption("photositeColor", tr("photosite color (debug)").toStdString(), tr("Every photosite's value in its own colour's channel only, with no demosaicing.").toStdString()));
    bayer.push_back(ChoiceOption("lmmse", "LMMSE", tr("Linear Minimum Mean Square-error Estimation: suited best to high ISO images, slow.").toStdString()));
    bayer.push_back(ChoiceOption("rcdDual", tr("RCD (dual)").toStdString(), tr("RCD where the image has detail, VNG4 where it is flat, blended by Dual Threshold.").toStdString()));
    bayer.push_back(ChoiceOption("amazeDual", tr("AMaZE (dual)").toStdString(), tr("AMaZE where the image has detail, VNG4 where it is flat, blended by Dual Threshold.").toStdString()));
    KnobChoicePtr bayerMethod = createKnob<KnobChoice>(tr("Method (Bayer)"));
    bayerMethod->setName(kRawDemosaicParamBayerMethod);
    bayerMethod->setHintToolTip(tr("The demosaicing method for a Bayer sensor."));
    bayerMethod->populateChoices(bayer);
    bayerMethod->setDefaultValue((int)eRawDemosaicBayerRCD);
    bayerMethod->setAnimationEnabled(false);
    page->addKnob(bayerMethod);
    _bayerMethod = bayerMethod;

    std::vector<ChoiceOption> xtrans;
    xtrans.push_back(ChoiceOption("markesteijn1", tr("Markesteijn 1-pass").toStdString(), tr("Frank Markesteijn's method in one pass.").toStdString()));
    xtrans.push_back(ChoiceOption("markesteijn3", tr("Markesteijn 3-pass").toStdString(), tr("Frank Markesteijn's method in three passes: fewer artefacts, slower.").toStdString()));
    xtrans.push_back(ChoiceOption("vng", "VNG", tr("Variable Number of Gradients: fast.").toStdString()));
    xtrans.push_back(ChoiceOption("passthroughMonochrome", tr("passthrough (monochrome)").toStdString(), tr("Every photosite's value in all three channels, with no demosaicing.").toStdString()));
    xtrans.push_back(ChoiceOption("photositeColor", tr("photosite color (debug)").toStdString(), tr("Every photosite's value in its own colour's channel only, with no demosaicing.").toStdString()));
    xtrans.push_back(ChoiceOption("markesteijn3Dual", tr("Markesteijn 3-pass (dual)").toStdString(), tr("Markesteijn 3-pass where the image has detail, VNG where it is flat, blended by Dual Threshold.").toStdString()));
    xtrans.push_back(ChoiceOption("fdc", tr("Frequency Domain Chroma").toStdString(), tr("Markesteijn 1-pass for the luma, with the chroma taken from the mosaic's frequency domain: fewer colour artefacts at high ISO. Up to ISO 1600 the chroma is a per-pixel hybrid with Markesteijn's.").toStdString()));
    KnobChoicePtr xtransMethod = createKnob<KnobChoice>(tr("Method (X-Trans)"));
    xtransMethod->setName(kRawDemosaicParamXTransMethod);
    xtransMethod->setHintToolTip(tr("The demosaicing method for an X-Trans sensor."));
    xtransMethod->populateChoices(xtrans);
    xtransMethod->setDefaultValue((int)eRawDemosaicXTransMarkesteijn1);
    xtransMethod->setAnimationEnabled(false);
    page->addKnob(xtransMethod);
    _xtransMethod = xtransMethod;

    std::vector<ChoiceOption> greens;
    greens.push_back(ChoiceOption("none", tr("Disabled").toStdString(), ""));
    greens.push_back(ChoiceOption("local", tr("Local Average").toStdString(), tr("Matches each green photosite to its neighbours where the image is flat.").toStdString()));
    greens.push_back(ChoiceOption("full", tr("Full Average").toStdString(), tr("Scales the first greens by the ratio of the two greens over the whole image.").toStdString()));
    greens.push_back(ChoiceOption("both", tr("Full and Local Average").toStdString(), ""));
    KnobChoicePtr greenEq = createKnob<KnobChoice>(tr("Match Greens"));
    greenEq->setName(kRawDemosaicParamGreenEq);
    greenEq->setHintToolTip(tr("Equilibrates the two greens of a Bayer sensor before demosaicing, against maze artefacts."));
    greenEq->populateChoices(greens);
    greenEq->setDefaultValue((int)eRawDemosaicGreenEqNone);
    greenEq->setAnimationEnabled(false);
    page->addKnob(greenEq);
    _greenEq = greenEq;

    KnobDoublePtr edgeThreshold = createKnob<KnobDouble>(tr("Edge Threshold"));
    edgeThreshold->setName(kRawDemosaicParamEdgeThreshold);
    edgeThreshold->setHintToolTip(tr("PPG only: the threshold of a median filter on the greens before demosaicing; 0 turns it off."));
    edgeThreshold->setDefaultValue(0.);
    edgeThreshold->setMinimum(0.);
    edgeThreshold->setMaximum(1.);
    edgeThreshold->setDisplayMinimum(0.);
    edgeThreshold->setDisplayMaximum(1.);
    page->addKnob(edgeThreshold);
    _edgeThreshold = edgeThreshold;

    KnobIntPtr smoothing = createKnob<KnobInt>(tr("Color Smoothing"));
    smoothing->setName(kRawDemosaicParamColorSmoothing);
    smoothing->setHintToolTip(tr("Passes of a median filter on the colour differences after demosaicing, against colour artefacts. Off for the dual methods."));
    smoothing->setDefaultValue(0);
    smoothing->setMinimum(0);
    smoothing->setMaximum(5);
    smoothing->setDisplayMinimum(0);
    smoothing->setDisplayMaximum(5);
    smoothing->setAnimationEnabled(false);
    page->addKnob(smoothing);
    _colorSmoothing = smoothing;

    std::vector<ChoiceOption> refines;
    refines.push_back(ChoiceOption("basic", tr("basic").toStdString(), ""));
    refines.push_back(ChoiceOption("median", tr("median").toStdString(), ""));
    refines.push_back(ChoiceOption("3xMedian", tr("3x median").toStdString(), ""));
    refines.push_back(ChoiceOption("refineMedians", tr("refine & medians").toStdString(), ""));
    refines.push_back(ChoiceOption("2xRefineMedians", tr("2x refine + medians").toStdString(), ""));
    KnobChoicePtr lmmseRefine = createKnob<KnobChoice>(tr("LMMSE Refine"));
    lmmseRefine->setName(kRawDemosaicParamLmmseRefine);
    lmmseRefine->setHintToolTip(tr("LMMSE refinement steps. The median steps average the output, refine adds some recalculation of the red and blue channels."));
    lmmseRefine->populateChoices(refines);
    lmmseRefine->setDefaultValue((int)eRawDemosaicLmmseMedian);
    lmmseRefine->setAnimationEnabled(false);
    page->addKnob(lmmseRefine);
    _lmmseRefine = lmmseRefine;

    KnobDoublePtr dualThreshold = createKnob<KnobDouble>(tr("Dual Threshold"));
    dualThreshold->setName(kRawDemosaicParamDualThreshold);
    dualThreshold->setHintToolTip(tr("Dual methods only: how much local detail keeps the high-frequency method's output; flatter areas take VNG's. "
                                     "0 keeps the high-frequency method everywhere."));
    dualThreshold->setDefaultValue(0.2);
    dualThreshold->setMinimum(0.);
    dualThreshold->setMaximum(1.);
    dualThreshold->setDisplayMinimum(0.);
    dualThreshold->setDisplayMaximum(1.);
    page->addKnob(dualThreshold);
    _dualThreshold = dualThreshold;

    KnobBoolPtr captureSharpen = createKnob<KnobBool>(tr("Capture Sharpen"));
    captureSharpen->setName(kRawDemosaicParamCaptureSharpen);
    captureSharpen->setHintToolTip(tr("Capture sharpening recovers details lost to in-camera blurring, which can be caused by diffraction, "
                                      "the anti-aliasing filter or other sources of Gaussian-type blur."));
    captureSharpen->setDefaultValue(false);
    captureSharpen->setAnimationEnabled(false);
    page->addKnob(captureSharpen);
    _captureSharpen = captureSharpen;

    KnobGroupPtr captureGroup = createKnob<KnobGroup>(tr("Capture Sharpen Controls"));
    captureGroup->setName(kCaptureGroupName);
    captureGroup->setHintToolTip(tr("The controls of capture sharpening, shown while it is on.").toStdString());
    captureGroup->setDefaultValue(true);
    page->addKnob(captureGroup);

    KnobIntPtr captureIterations = createKnob<KnobInt>(tr("Iterations"));
    captureIterations->setName(kRawDemosaicParamCaptureIterations);
    captureIterations->setHintToolTip(tr("Capture sharpening's strength, by iterations: one per step up to 11, rising to 50 at 25."));
    captureIterations->setDefaultValue(8);
    captureIterations->setMinimum(1);
    captureIterations->setMaximum(25);
    captureIterations->setDisplayMinimum(1);
    captureIterations->setDisplayMaximum(25);
    captureIterations->setAnimationEnabled(false);
    captureGroup->addKnob(captureIterations);
    _captureIterations = captureIterations;

    KnobDoublePtr captureRadius = createKnob<KnobDouble>(tr("Radius"));
    captureRadius->setName(kRawDemosaicParamCaptureRadius);
    captureRadius->setHintToolTip(tr(kCaptureRadiusHint));
    captureRadius->setDefaultValue(0.);
    captureRadius->setMinimum(0.);
    captureRadius->setMaximum(1.5);
    captureRadius->setDisplayMinimum(0.);
    captureRadius->setDisplayMaximum(1.5);
    captureRadius->setAnimationEnabled(false);
    captureGroup->addKnob(captureRadius);
    _captureRadius = captureRadius;

    KnobDoublePtr captureThreshold = createKnob<KnobDouble>(tr("Contrast Sensitivity"));
    captureThreshold->setName(kRawDemosaicParamCaptureThreshold);
    captureThreshold->setHintToolTip(tr(kCaptureThresholdHint));
    captureThreshold->setDefaultValue(0.4);
    captureThreshold->setMinimum(0.);
    captureThreshold->setMaximum(1.);
    captureThreshold->setDisplayMinimum(0.);
    captureThreshold->setDisplayMaximum(1.);
    captureThreshold->setAnimationEnabled(false);
    captureGroup->addKnob(captureThreshold);
    _captureThreshold = captureThreshold;

    KnobBoolPtr captureThresholdOverride = createKnob<KnobBool>(tr("Override Contrast Sensitivity"));
    captureThresholdOverride->setName(kRawDemosaicParamCaptureThresholdOverride);
    captureThresholdOverride->setHintToolTip(tr("Use the contrast sensitivity above instead of the image's default. "
                                                "An overriding 0 takes the image's default, rounded down to a hundredth."));
    captureThresholdOverride->setDefaultValue(false);
    captureThresholdOverride->setAnimationEnabled(false);
    captureGroup->addKnob(captureThresholdOverride);
    _captureThresholdOverride = captureThresholdOverride;

    KnobDoublePtr captureBoost = createKnob<KnobDouble>(tr("Corner Boost"));
    captureBoost->setName(kRawDemosaicParamCaptureBoost);
    captureBoost->setHintToolTip(tr("Further increases the sharpening radius towards the image's corners, in pixels; the sharp centre of the image is not affected."));
    captureBoost->setDefaultValue(0.);
    captureBoost->setMinimum(0.);
    captureBoost->setMaximum(1.5);
    captureBoost->setDisplayMinimum(0.);
    captureBoost->setDisplayMaximum(1.5);
    captureBoost->setAnimationEnabled(false);
    captureGroup->addKnob(captureBoost);
    _captureBoost = captureBoost;

    KnobDoublePtr captureCenter = createKnob<KnobDouble>(tr("Sharp Center"));
    captureCenter->setName(kRawDemosaicParamCaptureCenter);
    captureCenter->setHintToolTip(tr("The size of the sharp image centre Corner Boost leaves alone, as a fraction of half the image's shorter side beyond the half it always leaves."));
    captureCenter->setDefaultValue(0.);
    captureCenter->setMinimum(0.);
    captureCenter->setMaximum(1.);
    captureCenter->setDisplayMinimum(0.);
    captureCenter->setDisplayMaximum(1.);
    captureCenter->setAnimationEnabled(false);
    captureGroup->addKnob(captureCenter);
    _captureCenter = captureCenter;

    refreshCaptureOverride();
    refreshMethodKnobsVisibility();
}

void
RawDemosaic::refreshMethodKnobsVisibility()
{
    const KnobChoicePtr bayerMethod = _bayerMethod.lock();
    const KnobChoicePtr xtransMethod = _xtransMethod.lock();
    const KnobChoicePtr greenEq = _greenEq.lock();
    const KnobDoublePtr edgeThreshold = _edgeThreshold.lock();
    const KnobIntPtr colorSmoothing = _colorSmoothing.lock();
    const KnobChoicePtr lmmseRefine = _lmmseRefine.lock();
    const KnobDoublePtr dualThreshold = _dualThreshold.lock();

    if (!bayerMethod || !xtransMethod || !greenEq || !edgeThreshold || !colorSmoothing || !lmmseRefine || !dualThreshold) {
        return;
    }
    int period = 0;
    const EffectInstancePtr input = getInput(getSourceInput());
    if (input) {
        if (const std::optional<RawLayout> layout = RawLayout::fromMetadata(getSourceMetadata(getCurrentTime(), ViewIdx(0)))) {
            period = layout->period();
        }
    }
    // darktable's capture_support: not for the photosite modes, and for data without a CFA only
    // when it is monochrome (darktable's true monochrome; colour data skips demosaic entirely).
    bool captureSupport = true;
    const int bayer = bayerMethod->getValue();
    const int xtrans = xtransMethod->getValue();
    const bool bayerDual = (bayer == (int)eRawDemosaicBayerRCDDual) || (bayer == (int)eRawDemosaicBayerAMaZEDual);
    const bool xtransDual = (xtrans == (int)eRawDemosaicXTransMarkesteijn3Dual);

    if (period == 2) {
        const bool passing = (bayer == (int)eRawDemosaicBayerPassthroughMonochrome) || (bayer == (int)eRawDemosaicBayerPhotositeColor);
        bayerMethod->setSecret(false);
        xtransMethod->setSecret(true);
        edgeThreshold->setSecret(bayer != (int)eRawDemosaicBayerPPG);
        greenEq->setSecret(passing);
        colorSmoothing->setSecret(passing || bayerDual);
        dualThreshold->setSecret(!bayerDual);
        lmmseRefine->setSecret(bayer != (int)eRawDemosaicBayerLMMSE);
        captureSupport = !passing;
    } else if (period == 6) {
        const bool passing = (xtrans == (int)eRawDemosaicXTransPassthroughMonochrome) || (xtrans == (int)eRawDemosaicXTransPhotositeColor);
        bayerMethod->setSecret(true);
        xtransMethod->setSecret(false);
        edgeThreshold->setSecret(true);
        greenEq->setSecret(true);
        colorSmoothing->setSecret(passing || xtransDual);
        dualThreshold->setSecret(!xtransDual);
        lmmseRefine->setSecret(true);
        captureSupport = !passing;
    } else {
        // With no source, or one without a CFA, the sensor is unknown: both lists and every knob
        // either of them uses stay available.
        bayerMethod->setSecret(false);
        xtransMethod->setSecret(false);
        edgeThreshold->setSecret(false);
        greenEq->setSecret(false);
        colorSmoothing->setSecret(false);
        dualThreshold->setSecret(!bayerDual && !xtransDual);
        lmmseRefine->setSecret(bayer != (int)eRawDemosaicBayerLMMSE);
        captureSupport = !input || (input->getMetadataNComps(-1) == 1);
    }
    refreshCaptureKnobsVisibility(captureSupport);
}

void
RawDemosaic::refreshCaptureKnobsVisibility(bool support)
{
    const KnobBoolPtr enabled = _captureSharpen.lock();
    const KnobIntPtr iterations = _captureIterations.lock();
    const KnobDoublePtr radius = _captureRadius.lock();
    const KnobDoublePtr threshold = _captureThreshold.lock();
    const KnobBoolPtr thresholdOverride = _captureThresholdOverride.lock();
    const KnobDoublePtr boost = _captureBoost.lock();
    const KnobDoublePtr center = _captureCenter.lock();

    if (!enabled || !iterations || !radius || !threshold || !thresholdOverride || !boost || !center) {
        return;
    }
    // darktable's gui_changed(): the switch shows where capture sharpening is supported, the
    // controls while it is on, and Sharp Center while Corner Boost is not 0.
    const bool doCapture = support && enabled->getValue();
    enabled->setSecret(!support);
    if (const KnobIPtr group = getKnobByName(kCaptureGroupName)) {
        group->setSecret(!doCapture);
    }
    iterations->setSecret(!doCapture);
    radius->setSecret(!doCapture);
    threshold->setSecret(!doCapture);
    thresholdOverride->setSecret(!doCapture);
    boost->setSecret(!doCapture);
    center->setSecret(!doCapture || (boost->getValue() == 0.));
}

void
RawDemosaic::refreshCaptureOverride()
{
    const KnobBoolPtr thresholdOverride = _captureThresholdOverride.lock();
    const KnobDoublePtr threshold = _captureThreshold.lock();

    if (thresholdOverride && threshold) {
        threshold->setAllDimensionsEnabled(thresholdOverride->getValue());
    }
}

void
RawDemosaic::showSourceCaptureThreshold(double time)
{
    const KnobBoolPtr thresholdOverride = _captureThresholdOverride.lock();
    const KnobDoublePtr threshold = _captureThreshold.lock();

    if (!thresholdOverride || !threshold || !getInput(getSourceInput())) {
        return;
    }
    const float value = sourceCaptureThreshold(getSourceMetadata(time, ViewIdx(0)));
    threshold->setHintToolTip(tr(kCaptureThresholdHint) + QLatin1String("\n\n") + tr("The image's default: %1.").arg((double)value, 0, 'f', 3));
    if (!thresholdOverride->getValue()) {
        threshold->setValue((double)value);
    }
}

void
RawDemosaic::showAutoCaptureRadius(double radius,
                                   bool reliable)
{
    QString text = tr(kCaptureRadiusHint) + QLatin1String("\n\n") + tr("Measured on the last frame rendered with radius 0: %1 px.").arg(radius, 0, 'f', 2);
    if (!reliable) {
        text += QLatin1Char(' ') + tr("The frame is too small for a reliable measure.");
    }
    const std::string hint = text.toStdString();
    const KnobDoubleWPtr weak = _captureRadius;
    const auto apply = [weak, hint]() {
        if (const KnobDoublePtr knob = weak.lock()) {
            knob->setHintToolTip(hint);
        }
    };
    // Setting a tooltip signals the GUI, which only the main thread may do.
    if (!qApp) {
        return;
    }
    if (QThread::currentThread() == qApp->thread()) {
        apply();
    } else {
        QMetaObject::invokeMethod(qApp, apply, Qt::QueuedConnection);
    }
}

std::optional<double>
RawDemosaic::lastAutoCaptureRadius() const
{
    std::lock_guard<std::mutex> lock(_captureMutex);

    return _lastAutoCaptureRadius;
}

void
RawDemosaic::purgeCaches()
{
    {
        std::lock_guard<std::mutex> lock(_greenMutex);
        _greenValid = false;
    }
    std::lock_guard<std::mutex> lock(_captureMutex);
    _captureValid = false;
}

void
RawDemosaic::onKnobsLoaded()
{
    refreshCaptureOverride();
    refreshMethodKnobsVisibility();
    showSourceCaptureThreshold(getCurrentTime());
}

void
RawDemosaic::onInputChanged(int /*inputNo*/)
{
    refreshMethodKnobsVisibility();
    showSourceCaptureThreshold(getCurrentTime());
}

void
RawDemosaic::onMetadataRefreshed(const NodeMetadata& metadata)
{
    StagedImageEffect::onMetadataRefreshed(metadata);
    refreshMethodKnobsVisibility();
}

bool
RawDemosaic::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double time,
                         bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr bayerMethod = _bayerMethod.lock();
    const KnobChoicePtr xtransMethod = _xtransMethod.lock();
    const KnobBoolPtr captureSharpen = _captureSharpen.lock();
    const KnobDoublePtr captureBoost = _captureBoost.lock();
    const KnobBoolPtr thresholdOverride = _captureThresholdOverride.lock();

    if (!k) {
        return false;
    }
    if (thresholdOverride && (k == thresholdOverride.get())) {
        refreshCaptureOverride();
        if (!thresholdOverride->getValue()) {
            showSourceCaptureThreshold(time);
        }

        return true;
    }
    if ((k != bayerMethod.get()) && (k != xtransMethod.get()) && (k != captureSharpen.get()) && (k != captureBoost.get())) {
        return false;
    }
    refreshMethodKnobsVisibility();

    return true;
}

std::optional<RawDemosaic::Settings>
RawDemosaic::resolveSettings(double time,
                             ViewIdx view)
{
    Settings s;

    if (!RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &s.sourceRoD)) {
        return std::nullopt;
    }
    const ImageMetadata source = getSourceMetadata(time, view);
    s.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    const KnobReader reader(time, view);
    const int width = s.sourceRoD.width();
    const int height = s.sourceRoD.height();
    // darktable falls back on VNG below 16 photosites, and AMaZE reads 32 rows and columns in.
    // The whole image decides, never the buffer a tile runs over, so every tile agrees.
    const bool tiny = (width < 16) || (height < 16);

    if (s.layout.period() == 2) {
        switch (reader.value(_bayerMethod)) {
        case eRawDemosaicBayerPPG:
            s.algorithm = Algorithm::PPG;
            break;
        case eRawDemosaicBayerAMaZE:
            s.algorithm = ((width < 33) || (height < 33)) ? Algorithm::VNG : Algorithm::AMaZE;
            break;
        case eRawDemosaicBayerAMaZEDual:
            s.algorithm = ((width < 33) || (height < 33)) ? Algorithm::VNG : Algorithm::AMaZE;
            s.dual = true;
            break;
        case eRawDemosaicBayerRCDDual:
            s.algorithm = Algorithm::RCD;
            s.dual = true;
            break;
        case eRawDemosaicBayerVNG4:
            s.algorithm = Algorithm::VNG;
            break;
        case eRawDemosaicBayerLMMSE:
            s.algorithm = Algorithm::LMMSE;
            break;
        case eRawDemosaicBayerPassthroughMonochrome:
            s.algorithm = Algorithm::PhotositeMonochrome;
            break;
        case eRawDemosaicBayerPhotositeColor:
            s.algorithm = Algorithm::PhotositeColor;
            break;
        default:
            s.algorithm = Algorithm::RCD;
            break;
        }
        const int greenEq = reader.value(_greenEq);
        s.greenEq = ((greenEq >= 0) && (greenEq <= (int)eRawDemosaicGreenEqBoth)) ? (RawDemosaicGreenEqEnum)greenEq : eRawDemosaicGreenEqNone;
    } else if (s.layout.period() == 6) {
        switch (reader.value(_xtransMethod)) {
        case eRawDemosaicXTransMarkesteijn3:
            // Three passes mirror 18 photosites in from the edges, more than a 16 photosite
            // image has.
            s.algorithm = ((width < 19) || (height < 19)) ? Algorithm::VNG : Algorithm::Markesteijn3;
            break;
        case eRawDemosaicXTransMarkesteijn3Dual:
            s.algorithm = ((width < 19) || (height < 19)) ? Algorithm::VNG : Algorithm::Markesteijn3;
            s.dual = true;
            break;
        case eRawDemosaicXTransVNG:
            s.algorithm = Algorithm::VNG;
            break;
        case eRawDemosaicXTransFDC:
            s.algorithm = Algorithm::FDC;
            break;
        case eRawDemosaicXTransPassthroughMonochrome:
            s.algorithm = Algorithm::PhotositeMonochrome;
            break;
        case eRawDemosaicXTransPhotositeColor:
            s.algorithm = Algorithm::PhotositeColor;
            break;
        default:
            s.algorithm = Algorithm::Markesteijn1;
            break;
        }
    }
    // The photosite modes are exempt from darktable's fallback on tiny images, as it exempts them.
    const bool point = (s.algorithm == Algorithm::PassThrough) || (s.algorithm == Algorithm::PhotositeMonochrome) || (s.algorithm == Algorithm::PhotositeColor);
    if (tiny && !point) {
        s.algorithm = Algorithm::VNG;
    }
    s.edgeThreshold = (float)reader.value(_edgeThreshold);
    // darktable's commit_params() turns colour smoothing off for a dual method, tiny image or not;
    // dual_demosaic() itself does nothing below 16 photosites.
    s.smoothing = s.dual ? 0 : std::max(0, std::min(5, (int)reader.value(_colorSmoothing)));
    s.dual = s.dual && !tiny;
    s.dualThreshold = (float)std::max(0., std::min(1., (double)reader.value(_dualThreshold)));
    if (const std::optional<std::array<double, 3>> applied = RawMetadata::getWbApplied(source)) {
        for (int c = 0; c < 3; ++c) {
            s.wb[c] = 1.0f / (float)(*applied)[c];
        }
    }
    s.lmmseRefine = std::max((int)eRawDemosaicLmmseBasic, std::min((int)eRawDemosaicLmmse2xRefineMedians, (int)reader.value(_lmmseRefine)));
    const std::array<double, 3> maximum = RawMetadata::getProcessedMaximum(source).value_or(std::array<double, 3> { 1., 1., 1. });
    s.processedMaximum = std::fmax(1.0f, (float)std::max({ maximum[0], maximum[1], maximum[2] }));
    s.processedMinimum = std::fmax(1.0f, (float)std::min({ maximum[0], maximum[1], maximum[2] }));
    s.iso = (float)RawMetadata::getIso(source).value_or(0.);

    // Capture sharpening, as demosaic.c's process() decides it: not for the photosite modes, and
    // for data without a CFA only when it is monochrome.
    if (reader.value(_captureSharpen)) {
        if (s.algorithm == Algorithm::PassThrough) {
            const EffectInstancePtr input = getInput(getSourceInput());
            s.mono = (s.layout.period() == 0) && input && (input->getMetadataNComps(-1) == 1);
            s.capture = s.mono;
        } else {
            s.capture = (s.algorithm != Algorithm::PhotositeMonochrome) && (s.algorithm != Algorithm::PhotositeColor);
        }
    }
    if (s.capture) {
        RawKernels::CaptureSettings& cs = s.captureSettings;
        cs.iterations = RawKernels::captureIterations(std::max(1, std::min(25, (int)reader.value(_captureIterations))));
        cs.radius = (float)std::max(0., std::min(1.5, (double)reader.value(_captureRadius)));
        cs.boost = (float)std::max(0., std::min(1.5, (double)reader.value(_captureBoost)));
        cs.center = (float)std::max(0., std::min(1., (double)reader.value(_captureCenter)));
        const float sourceThreshold = sourceCaptureThreshold(source);
        if (!reader.value(_captureThresholdOverride)) {
            cs.threshold = sourceThreshold;
        } else {
            const float threshold = (float)reader.value(_captureThreshold);
            // commit_params(): a threshold of 0 or less takes the image's, cut to a hundredth.
            cs.threshold = (threshold <= 0.0f) ? 0.01f * (float)(int)(100.0f * sourceThreshold) : threshold;
        }
        if (const std::optional<std::array<double, 3>> applied = RawMetadata::getWbApplied(source)) {
            for (int c = 0; c < 3; ++c) {
                s.wbApplied[c] = (float)(*applied)[c];
            }
        }
    }

    return s;
} // RawDemosaic::resolveSettings

RectI
RawDemosaic::sourceRegionFor(const RectI& window,
                             double time,
                             ViewIdx view)
{
    const std::optional<Settings> settings = resolveSettings(time, view);

    if (!settings) {
        return window;
    }
    const RectI& rod = settings->sourceRoD;
    // Capture sharpening runs between the method and the dual blend, so its reach adds to the
    // method's own and the dual mask's.
    const int capture = settings->capture ? RawKernels::captureHalo(settings->captureSettings.iterations) : 0;
    if (settings->algorithm == Algorithm::PassThrough) {
        return settings->capture ? RawNodeSupport::expandForKernel(window, capture, 1, kMinimumBuffer, rod) : window.intersect(rod);
    }
    const bool local = (settings->greenEq == eRawDemosaicGreenEqLocal) || (settings->greenEq == eRawDemosaicGreenEqBoth);
    const int extra = settings->smoothing + (local ? 2 : 0);
    if ((settings->algorithm != Algorithm::AMaZE) && (settings->algorithm != Algorithm::LMMSE) && (settings->algorithm != Algorithm::FDC)) {
        const int own = algorithmHalo(settings->algorithm) + capture;
        const int halo = settings->dual ? std::max(own + kDualMaskReach, kDualLowReach) : own;
        return RawNodeSupport::expandForKernel(window, halo + extra, settings->layout.period(), kMinimumBuffer, rod);
    }
    // AMaZE's span leaves at least 16 photosites past the grown window on each inner side, more
    // than VNG and its smoothing read.
    const int grow = extra + (settings->dual ? kDualMaskReach : 0) + capture;
    const RectI grown = RectI(window.x1 - grow, window.y1 - grow, window.x2 + grow, window.y2 + grow).intersect(rod);
    if (grown.isNull()) {
        return grown;
    }
    if (settings->algorithm == Algorithm::LMMSE) {
        // The span ends 4 photosites past the last tile's reads, beyond the 2 local green
        // matching leaves untouched.
        int c0, c1, r0, r1;
        RawKernels::lmmseSpan(grown.x1 - rod.x1, grown.x2 - rod.x1, rod.width(), &c0, &c1);
        RawKernels::lmmseSpan(rod.y2 - grown.y2, rod.y2 - grown.y1, rod.height(), &r0, &r1);

        return RectI(rod.x1 + c0, rod.y2 - r1, rod.x1 + c1, rod.y2 - r0);
    }
    if (settings->algorithm == Algorithm::FDC) {
        int c0, c1, r0, r1;
        RawKernels::fdcSpan(grown.x1 - rod.x1, grown.x2 - rod.x1, rod.width(), &c0, &c1);
        RawKernels::fdcSpan(rod.y2 - grown.y2, rod.y2 - grown.y1, rod.height(), &r0, &r1);

        return RectI(rod.x1 + c0, rod.y2 - r1, rod.x1 + c1, rod.y2 - r0);
    }
    // Local green equilibration leaves the 2 last rows and columns of its buffer as they were.
    const int endMargin = local ? 2 : 0;
    int c0, c1, r0, r1;
    amazeSpan(grown.x1 - rod.x1, grown.x2 - rod.x1, rod.width(), endMargin, &c0, &c1);
    amazeSpan(rod.y2 - grown.y2, rod.y2 - grown.y1, rod.height(), endMargin, &r0, &r1);

    return RectI(rod.x1 + c0, rod.y2 - r1, rod.x1 + c1, rod.y2 - r0);
}

bool
RawDemosaic::isOpIdentity(double /*time*/,
                          const RenderScale& /*scale*/,
                          const RectI& /*roi*/,
                          ViewIdx /*view*/)
{
    return false;
}

void
RawDemosaic::getRegionsOfInterest(double time,
                                  const RenderScale& scale,
                                  const RectD& outputRoD,
                                  const RectD& renderWindow,
                                  ViewIdx view,
                                  RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    const std::optional<Settings> settings = resolveSettings(time, view);
    if (!input || !settings) {
        return;
    }
    const double par = getAspectRatio(getSourceInput());
    const bool full = (settings->greenEq == eRawDemosaicGreenEqFull) || (settings->greenEq == eRawDemosaicGreenEqBoth);
    RectI need = full ? settings->sourceRoD : sourceRegionFor(PassExec::toPixelEnclosing(renderWindow, scale, par), time, view);
    if (settings->capture && (settings->captureSettings.radius <= 0.f)) {
        // The automatic radius measures the centre of the frame, whatever the window.
        RawKernels::CaptureRadiusArea area;
        const RectI centre = captureRadiusRect(settings->sourceRoD, &area);
        if (!centre.isNull()) {
            need.merge(centre);
        }
    }
    (*ret)[input] = PassExec::toCanonical(need, scale, par);
}

StatusEnum
RawDemosaic::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

ImageMetadata
RawDemosaic::deriveOutputMetadata(double time,
                                  ViewIdx view)
{
    ImageMetadata metadata = StagedImageEffect::deriveOutputMetadata(time, view);

    // darktable's demosaic sets DT_IMAGE_MONOCHROME_BAYER while a passthrough (monochrome) method
    // is chosen, which dt_image_is_monochrome() reports from then on.
    const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
    const KnobChoicePtr bayerMethod = _bayerMethod.lock();
    const KnobChoicePtr xtransMethod = _xtransMethod.lock();
    if (layout && (layout->period() == 2) && bayerMethod && (bayerMethod->getValueAtTime(time) == (int)eRawDemosaicBayerPassthroughMonochrome)) {
        RawMetadata::setMonochrome(&metadata);
    } else if (layout && (layout->period() == 6) && xtransMethod && (xtransMethod->getValueAtTime(time) == (int)eRawDemosaicXTransPassthroughMonochrome)) {
        RawMetadata::setMonochrome(&metadata);
    }

    // The output is an image: what described the mosaic no longer describes it. The camera
    // matrix and the white balance stay for RawInputColor, and raw/monochrome for
    // RawColorCalibration.
    metadata.remove(kRawMetadataCfaPattern);
    metadata.remove(kRawMetadataCfaOrigin);
    metadata.remove(kRawMetadataStage);
    metadata.remove(kRawMetadataBlackLevels);
    metadata.remove(kRawMetadataWhiteLevel);

    return metadata;
}

StatusEnum
RawDemosaic::renderOp(const RenderActionArgs& args,
                      const StagePlan& /*plan*/)
{
    const std::optional<Settings> found = resolveSettings(args.time, args.view);

    if (!found) {
        return eStatusFailed;
    }
    const Settings& s = *found;
    const int sourceInput = getSourceInput();

    RawNodeSupport::LockedSource source;
    ImageLayerDesc layer;
    if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &layer, NULL)) {
        RectI roiPixel;
        source.image = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
    }
    if (!source.image || (source.image->getBitDepth() != eImageBitDepthFloat)) {
        return eStatusFailed;
    }
    source.lock();

    RenderCancellation cancel(this);
    const RawKernels::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, appPTR->getNCPUsAvailableForEffect());
    const RectI& roi = args.roi;
    const RectI& rod = s.sourceRoD;

    // The capture sharpening settings for this frame, the automatic radius measured once per
    // frame key on the raw data, before green equilibration, as _calc_auto_radius() does.
    RawKernels::CaptureSettings capture = s.captureSettings;
    if (s.capture && (capture.radius <= 0.f)) {
        const U64 key = RawNodeSupport::frameKey(getRenderHash(), args.time, args.view);
        bool measured = false;
        bool reliable = true;
        {
            std::lock_guard<std::mutex> lock(_captureMutex);
            if (!_captureValid || (_captureKey != key)) {
                RawNodeSupport::noteFrameComputed(kRawDemosaicFrameCaptureRadius, key);
                RawKernels::CaptureRadiusArea area;
                const RectI centre = captureRadiusRect(rod, &area);
                reliable = area.reliable;
                double radius = 0.5;
                if (area.reliable && !centre.isNull()) {
                    RawNodeSupport::TopDownBuffer measure;
                    measure.allocate(centre, 1);
                    RawNodeSupport::readTopDown(source, &measure, parallel);
                    const std::uint32_t frameFilters = s.mono ? 0u : ((s.layout.period() == 2) ? RawNodeSupport::bayerFiltersAt(s.layout, rod) : RawKernels::kFiltersXTrans);
                    std::uint8_t frameXTrans[6][6] = {};
                    if (s.layout.period() == 6) {
                        RawNodeSupport::xtransAt(s.layout, rod, frameXTrans);
                    }
                    radius = RawKernels::captureAutoRadius(measure.data.data(), centre.width(), centre.height(), frameFilters, frameXTrans, s.wbApplied, parallel);
                }
                _captureRadiusValue = radius;
                _captureKey = key;
                _captureValid = !cancel.check();
                measured = _captureValid;
                if (measured) {
                    _lastAutoCaptureRadius = radius;
                }
            }
            capture.radius = (float)_captureRadiusValue;
        }
        if (measured) {
            showAutoCaptureRadius((double)capture.radius, reliable);
        }
    }

    RawNodeSupport::TopDownBuffer rgb;
    if ((s.algorithm == Algorithm::PassThrough) && s.capture) {
        // darktable's true monochrome: the values themselves are the mosaic.
        const RectI need = sourceRegionFor(roi, args.time, args.view);
        RawNodeSupport::TopDownBuffer mono;
        mono.allocate(need, 1);
        RawNodeSupport::readTopDown(source, &mono, parallel);
        rgb.allocate(need, 4);
        parallel(need.height(), [&](int r) {
            const std::size_t base = (std::size_t)r * need.width();
            for (int i = 0; i < need.width(); ++i) {
                const float v = mono.data[base + i];
                float* q = &rgb.data[(base + i) * 4];
                q[0] = q[1] = q[2] = q[3] = v;
            }
        });
        RawKernels::captureSharpen(rgb.data.data(), mono.data.data(), need.width(), need.height(), rod.y2 - need.y2, need.x1 - rod.x1, rod.width(), rod.height(), 0u, NULL, s.wbApplied, capture, parallel);
    } else if (s.algorithm == Algorithm::PassThrough) {
        RawNodeSupport::TopDownBuffer in;
        in.allocate(roi, source.nComps);
        RawNodeSupport::readTopDown(source, &in, parallel);
        rgb.allocate(roi, 3);
        const int inComps = source.nComps;
        parallel(roi.height(), [&](int r) {
            const std::size_t base = (std::size_t)r * roi.width();
            for (int i = 0; i < roi.width(); ++i) {
                const float* p = &in.data[(base + i) * inComps];
                float* q = &rgb.data[(base + i) * 3];
                for (int c = 0; c < 3; ++c) {
                    q[c] = (inComps >= 3) ? p[c] : p[0];
                }
            }
        });
    } else {
        const RectI need = sourceRegionFor(roi, args.time, args.view);
        RawNodeSupport::TopDownBuffer mosaic;
        mosaic.allocate(need, 1);
        RawNodeSupport::readTopDown(source, &mosaic, parallel);
        const int width = need.width();
        const int height = need.height();
        const std::uint32_t filters = (s.layout.period() == 2) ? RawNodeSupport::bayerFiltersAt(s.layout, need) : RawKernels::kFiltersXTrans;
        std::uint8_t xtrans[6][6] = {};
        if (s.layout.period() == 6) {
            RawNodeSupport::xtransAt(s.layout, need, xtrans);
        }

        std::vector<float> greens;
        const float* in = mosaic.data.data();
        if (s.greenEq != eRawDemosaicGreenEqNone) {
            greens = mosaic.data;
            if ((s.greenEq == eRawDemosaicGreenEqFull) || (s.greenEq == eRawDemosaicGreenEqBoth)) {
                const std::uint32_t imageFilters = RawNodeSupport::bayerFiltersAt(s.layout, rod);
                const U64 key = RawNodeSupport::frameKey(getRenderHash(), args.time, args.view);
                double ratio = 0.;
                {
                    std::lock_guard<std::mutex> lock(_greenMutex);
                    if (!_greenValid || (_greenKey != key)) {
                        RawNodeSupport::noteFrameComputed(kRawDemosaicFrameGreenRatio, key);
                        RawNodeSupport::TopDownBuffer whole;
                        whole.allocate(rod, 1);
                        RawNodeSupport::readTopDown(source, &whole, parallel);
                        _greenRatio = RawKernels::greenEquilibrationFullRatio(whole.data.data(), rod.width(), rod.height(), imageFilters, parallel);
                        _greenKey = key;
                        _greenValid = !cancel.check();
                    }
                    ratio = _greenRatio;
                }
                RawKernels::greenEquilibrationFullApply(greens.data(), width, height, rod.y2 - need.y2, need.x1 - rod.x1, rod.width(), rod.height(), imageFilters, ratio, parallel);
            }
            if ((s.greenEq == eRawDemosaicGreenEqLocal) || (s.greenEq == eRawDemosaicGreenEqBoth)) {
                const std::vector<float> averaged = greens;
                RawKernels::greenEquilibrationLocal(greens.data(), averaged.data(), width, height, filters, 0.0001f * s.iso, parallel);
            }
            in = greens.data();
        }

        RawNodeSupport::TopDownBuffer out;
        out.allocate(need, 4);
        switch (s.algorithm) {
        case Algorithm::PPG:
            RawKernels::ppg(out.data.data(), in, width, height, filters, s.edgeThreshold, 100000, parallel);
            break;
        case Algorithm::RCD:
            RawKernels::rcd(out.data.data(), in, width, height, filters, s.processedMaximum, parallel);
            break;
        case Algorithm::AMaZE:
            RawKernels::amaze(in, out.data.data(), width, height, filters, s.processedMinimum, parallel);
            break;
        case Algorithm::LMMSE:
            RawKernels::lmmse(out.data.data(), in, width, height, filters, s.lmmseRefine, s.processedMaximum, parallel);
            break;
        case Algorithm::VNG:
            RawKernels::vng(out.data.data(), in, width, height, filters, xtrans, false, parallel);
            break;
        case Algorithm::Markesteijn1:
            RawKernels::markesteijn(out.data.data(), in, width, height, xtrans, 1, parallel);
            break;
        case Algorithm::Markesteijn3:
            RawKernels::markesteijn(out.data.data(), in, width, height, xtrans, 3, parallel);
            break;
        case Algorithm::FDC:
            // darktable passes the image's ISO as an int.
            RawKernels::fdc(out.data.data(), in, width, height, xtrans, (int)s.iso, parallel);
            break;
        case Algorithm::PhotositeMonochrome:
            RawKernels::passthroughMonochrome(out.data.data(), in, width, height, parallel);
            break;
        case Algorithm::PhotositeColor:
            RawKernels::passthroughColor(out.data.data(), in, width, height, filters, xtrans, parallel);
            break;
        case Algorithm::PassThrough:
            break;
        }
        // demosaic.c's process(): capture sharpening on the method's output and the green
        // equilibrated mosaic, before the dual blend and colour smoothing.
        if (s.capture) {
            RawKernels::captureSharpen(out.data.data(), in, width, height, rod.y2 - need.y2, need.x1 - rod.x1, rod.width(), rod.height(), filters, xtrans, s.wbApplied, capture, parallel);
        }
        if (s.dual) {
            RawKernels::dualDemosaic(out.data.data(), in, width, height, filters, xtrans, s.dualThreshold, s.wb, parallel);
        }
        if (s.smoothing > 0) {
            RawKernels::colorSmoothing(out.data.data(), width, height, s.smoothing, parallel);
        }
        rgb = std::move(out);
    }

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int dstComps = (int)dst->getComponentsCount();
        Image::WriteAccess access(dst.get());
        parallel(roi.height(), [&](int r) {
            const int y = roi.y1 + r;
            float* outPix = (float*)access.pixelAt(roi.x1, y);
            if (!outPix) {
                return;
            }
            for (int x = roi.x1; x < roi.x2; ++x, outPix += dstComps) {
                const bool inside = (x >= rgb.rect.x1) && (x < rgb.rect.x2) && (y >= rgb.rect.y1) && (y < rgb.rect.y2);
                const float* p = inside ? &rgb.data[rgb.offset(x, y)] : NULL;
                for (int c = 0; c < dstComps; ++c) {
                    outPix[c] = (c == 3) ? 1.f : (p ? p[c] : 0.f);
                }
            }
        });
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // RawDemosaic::renderOp

bool
RawDemosaic::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps != 4)) {
        return false;
    }
    // Resolving only reads knobs and the source's metadata and region.
    const std::optional<Settings> settings = const_cast<RawDemosaic*>(this)->resolveSettings(query.time, query.view);
    if (!settings) {
        return false;
    }
    // Capture sharpening has no GPU path.
    if (settings->capture) {
        return false;
    }
    if (settings->algorithm == Algorithm::PassThrough) {
        return true;
    }
    if ((settings->algorithm == Algorithm::PhotositeMonochrome) || (settings->algorithm == Algorithm::PhotositeColor) || settings->dual) {
        return false;
    }

    const bool vng = (settings->algorithm == Algorithm::VNG) && ((settings->layout.period() == 2) || (settings->layout.period() == 6));

    return ((settings->algorithm == Algorithm::PPG) || (settings->algorithm == Algorithm::RCD) || vng) && (settings->greenEq == eRawDemosaicGreenEqNone) && (settings->smoothing == 0);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
RawDemosaic::getOpGpuInputLayer(int inputNb,
                                const ImageLayerDesc& plane,
                                double time,
                                ViewIdx view,
                                ImageLayerDesc* layer) const
{
    if (inputNb != getSourceInput()) {
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }

    // The source is read in its own layer, a mosaic's one channel, not in the RGBA output's.
    return const_cast<RawDemosaic*>(this)->resolveInputPlaneForRender(inputNb, time, view, layer, NULL);
}

RectI
RawDemosaic::getOpGpuInputRoI(int inputNb,
                              const RectI& outputRect,
                              const RenderScale& /*scale*/) const
{
    if (inputNb != getSourceInput()) {
        return outputRect;
    }

    return const_cast<RawDemosaic*>(this)->sourceRegionFor(outputRect, getCurrentTime(), getCurrentView());
}

#ifdef NATRON_HAVE_GPU
namespace {
// The pattern RawKernels::vng() runs with over a buffer of rect, from its top-left photosite: the
// X-Trans one, or VNG4's, the Bayer one with the green on the buffer's even rows as colour 3.
RawLayoutGpu
vngLayoutGpu(const RawLayout& layout,
             const RectI& rect)
{
    std::uint8_t colours[36] = {};

    if (layout.period() == 6) {
        std::uint8_t xtrans[6][6] = {};
        RawNodeSupport::xtransAt(layout, rect, xtrans);
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 6; ++col) {
                colours[row * 6 + col] = xtrans[row][col];
            }
        }

        return toRawLayoutGpu(6, colours);
    }
    const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(layout, rect);
    const std::uint32_t filters4 = filters | (((filters & 3) == 1) ? 0x03030303u : 0x0c0c0c0cu);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            colours[row * 2 + col] = (std::uint8_t)RawKernels::fcBayer((std::size_t)row, (std::size_t)col, filters4);
        }
    }

    return toRawLayoutGpu(2, colours);
}
} // namespace
#endif // NATRON_HAVE_GPU

StatusEnum
RawDemosaic::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI& rect = args.outputRect;
    const GpuImageView& out = args.output;
    if (!canRenderOpOnGpu(query) || rect.isNull() || !out.buffer || (out.nComps != 4) || !out.bounds.contains(rect) || ((out.byteOffset % sizeof(float)) != 0)) {
        return eStatusFailed;
    }
    const std::optional<Settings> found = resolveSettings(query.time, query.view);
    if (!found) {
        return eStatusFailed;
    }
    const Settings& s = *found;
    const int sourceInput = getSourceInput();
    const bool hasSource = (sourceInput >= 0) && (sourceInput < (int)args.inputs.size()) && args.inputs[sourceInput].buffer;
    const GpuImageView source = hasSource ? args.inputs[sourceInput] : GpuImageView();
    if (hasSource && (((source.byteOffset % sizeof(float)) != 0) || (source.nComps < 1) || (source.nComps > 4))) {
        return eStatusFailed;
    }
    const std::uint64_t srcEnd = source.byteOffset / sizeof(float) + (std::uint64_t)source.bounds.height() * (std::uint64_t)source.rowStrideFloats;
    const std::uint64_t dstEnd = out.byteOffset / sizeof(float) + (std::uint64_t)out.bounds.height() * (std::uint64_t)out.rowStrideFloats;
    if ((srcEnd > UINT32_MAX) || (dstEnd > UINT32_MAX)) {
        return eStatusFailed;
    }

    RawDemosaicGpuPush base = {};
    if (hasSource) {
        base.srcOffset = (std::uint32_t)(source.byteOffset / sizeof(float));
        base.srcStride = (std::uint32_t)source.rowStrideFloats;
        base.srcX1 = source.bounds.x1;
        base.srcY1 = source.bounds.y1;
        base.srcX2 = source.bounds.x2;
        base.srcY2 = source.bounds.y2;
        base.srcComps = (std::uint32_t)source.nComps;
    } else {
        base.srcComps = 1;
    }
    base.dstStride = (std::uint32_t)out.rowStrideFloats;
    const bool rcd = (s.algorithm == Algorithm::RCD);
    const bool vng = (s.algorithm == Algorithm::VNG);
    base.mode = (s.algorithm == Algorithm::PassThrough) ? kRawDemosaicGpuModePassThrough : (rcd ? kRawDemosaicGpuModeRcd : kRawDemosaicGpuModePpg);
    base.threshold = rcd ? 0.0f : s.edgeThreshold;
    base.scaler = s.processedMaximum;
    base.revScaler = 1.0f / s.processedMaximum;
    base.margin = rcd ? 10u : 100000u;

    // Bands of the window are demosaiced one after the other through one work buffer, each over
    // the source region the CPU would read for it, which a tile's result does not depend on.
    // PPG and RCD need under 16 floats a photosite, VNG its two RGBA planes.
    const std::uint64_t kMaxWorkFloats = std::uint64_t(1) << 25;
    const std::uint64_t floatsPerPhotosite = vng ? 8 : 16;
    std::vector<RectI> bands;
    std::vector<RectI> needs;
    std::uint64_t workFloats = 4;
    if (base.mode == kRawDemosaicGpuModePassThrough) {
        bands.push_back(rect);
        needs.push_back(RectI());
    } else {
        const RectI firstRow = sourceRegionFor(RectI(rect.x1, rect.y1, rect.x2, rect.y1 + 1), query.time, query.view);
        const std::uint64_t needWidth = (std::uint64_t)std::max(1, firstRow.width());
        const int overhead = std::max(0, firstRow.height() - 1);
        const int bandRows = (int)std::max<std::int64_t>(16, (std::int64_t)(kMaxWorkFloats / (floatsPerPhotosite * needWidth)) - overhead);
        for (int y = rect.y1; y < rect.y2; y += bandRows) {
            const RectI band(rect.x1, y, rect.x2, std::min(rect.y2, y + bandRows));
            const RectI need = sourceRegionFor(band, query.time, query.view);
            bands.push_back(band);
            needs.push_back(need);
            const std::uint64_t w = (std::uint64_t)std::max(0, need.width());
            const std::uint64_t h = (std::uint64_t)std::max(0, need.height());
            const std::uint64_t stride = (w + 1) & ~std::uint64_t(1);
            workFloats = std::max(workFloats, vng ? 8 * w * h : 9 * w * h + (13 * stride * h) / 2);
        }
    }
    if (workFloats > UINT32_MAX) {
        return eStatusFailed;
    }

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* work = nullptr;
    if (!ctx.scratch(workFloats * sizeof(float), work) || !work) {
        return eStatusFailed;
    }
    const gpu::GpuBuffer* src = hasSource ? source.buffer : work;
    const auto groups = [](std::uint32_t n) {
        return std::max<std::uint32_t>(n, 1);
    };
    const auto run = [&](gpu::GpuKernelId kernel, const auto& push, std::uint32_t x, std::uint32_t y) {
        if (!ctx.dispatch(kernel, { src, work, out.buffer }, push, { groups(x), groups(y), 1 })) {
            return false;
        }
        ctx.computeBarrier();

        return true;
    };

    for (std::size_t b = 0; b < bands.size(); ++b) {
        const RectI& band = bands[b];
        const RectI& need = needs[b];
        RawDemosaicGpuPush push = base;
        const std::uint32_t width = (std::uint32_t)std::max(0, need.width());
        const std::uint32_t height = (std::uint32_t)std::max(0, need.height());
        push.bufX1 = need.x1;
        push.bufY2 = need.y2;
        push.width = width;
        push.height = height;
        push.stride = (width + 1) & ~1u;
        push.filters = (base.mode == kRawDemosaicGpuModePassThrough) || need.isNull() ? 0u : RawNodeSupport::bayerFiltersAt(s.layout, need);
        push.dstOffset = (std::uint32_t)(out.byteOffset / sizeof(float) + (std::uint64_t)(band.y1 - out.bounds.y1) * (std::uint64_t)out.rowStrideFloats + (std::uint64_t)(band.x1 - out.bounds.x1) * 4);
        push.outX = band.x1;
        push.outY = band.y1;
        push.outWidth = (std::uint32_t)band.width();
        push.outHeight = (std::uint32_t)band.height();

        if (vng) {
            RawVngGpuPush vngPush = {};
            vngPush.width = width;
            vngPush.height = height;
            vngPush.srcOffset = base.srcOffset;
            vngPush.srcStride = base.srcStride;
            vngPush.srcComps = base.srcComps;
            if (hasSource) {
                vngPush.srcCol1 = source.bounds.x1 - need.x1;
                vngPush.srcCol2 = source.bounds.x2 - need.x1;
                vngPush.srcRow1 = need.y2 - source.bounds.y2;
                vngPush.srcRow2 = need.y2 - source.bounds.y1;
            }
            vngPush.dstOffset = push.dstOffset;
            vngPush.dstStride = push.dstStride;
            vngPush.outCol = band.x1 - need.x1;
            vngPush.outRow = need.y2 - 1 - band.y1;
            vngPush.outWidth = push.outWidth;
            vngPush.outHeight = push.outHeight;
            if ((width > 0) && (height > 0)) {
                vngPush.layout = vngLayoutGpu(s.layout, need);
                if (!run(gpu::GpuKernelId::vng_linear, vngPush, width, height) || !run(gpu::GpuKernelId::vng_gradient, vngPush, width, height)) {
                    return eStatusFailed;
                }
            }
            if (!run(gpu::GpuKernelId::vng_write, vngPush, vngPush.outWidth, vngPush.outHeight)) {
                return eStatusFailed;
            }
            continue;
        }

        if ((base.mode != kRawDemosaicGpuModePassThrough) && (width > 0) && (height > 0)) {
            if ((push.threshold > 0.0f) && !run(gpu::GpuKernelId::demosaic_median, push, width, height)) {
                return eStatusFailed;
            }
            if (!run(gpu::GpuKernelId::demosaic_ppg_green, push, width, height) || !run(gpu::GpuKernelId::demosaic_ppg_rb, push, width, height)) {
                return eStatusFailed;
            }
            if (rcd && (width >= 20) && (height >= 20)) {
                const std::uint32_t slots = push.stride / 2;
                const bool ok = run(gpu::GpuKernelId::rcd_populate, push, push.stride, height)
                    && run(gpu::GpuKernelId::rcd_vh, push, push.stride, height)
                    && run(gpu::GpuKernelId::rcd_lpf, push, slots, height)
                    && run(gpu::GpuKernelId::rcd_green, push, width, height)
                    && run(gpu::GpuKernelId::rcd_pq, push, slots, height)
                    && run(gpu::GpuKernelId::rcd_pqdir, push, slots, height)
                    && run(gpu::GpuKernelId::rcd_rb, push, width, height)
                    && run(gpu::GpuKernelId::rcd_rb_green, push, width, height);
                if (!ok) {
                    return eStatusFailed;
                }
            }
        }
        if (!run(gpu::GpuKernelId::demosaic_write, push, push.outWidth, push.outHeight)) {
            return eStatusFailed;
        }
    }

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // RawDemosaic::renderOpGpu

NATRON_NAMESPACE_EXIT
