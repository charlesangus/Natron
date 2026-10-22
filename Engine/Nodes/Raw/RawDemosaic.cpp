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
#include <cmath>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawDemosaicInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawDemosaicNodeSpec = {
    .id = PLUGINID_NATRON_RAWDEMOSAIC,
    .label = "RawDemosaic",
    .description = "Reconstructs full RGB pixels from the colour filter array of prepared raw data: RCD, PPG, AMaZE or VNG4 "
                   "for a Bayer sensor, Markesteijn or VNG for an X-Trans one. Data without a colour filter array passes "
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
    VNG,
    Markesteijn1,
    Markesteijn3
};

// How far each algorithm reads around a pixel it outputs, so that the pixel does not depend on
// where the buffer it runs over ends: darktable's tiling overlaps, checked bit for bit against
// whole-image runs. AMaZE instead aligns its buffer with its own tile grid.
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
    case Algorithm::PassThrough:
        break;
    }

    return 0;
}

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

U64
renderKey(U64 hash,
          double time,
          ViewIdx view)
{
    return hash ^ (U64)(std::int64_t)std::llround(time * 1000.) ^ ((U64)(int)view << 48);
}
} // namespace

struct RawDemosaic::Settings {
    RawLayout layout;
    RectI sourceRoD;
    Algorithm algorithm = Algorithm::PassThrough;
    RawDemosaicGreenEqEnum greenEq = eRawDemosaicGreenEqNone;
    float edgeThreshold = 0.f;
    int smoothing = 0;
    float processedMaximum = 1.f;
    float processedMinimum = 1.f;
    float iso = 0.f;
};

RawDemosaic::RawDemosaic(NodePtr node)
    : StagedImageEffect(node)
    , _bayerMethod()
    , _xtransMethod()
    , _greenEq()
    , _edgeThreshold()
    , _colorSmoothing()
    , _greenMutex()
    , _greenValid(false)
    , _greenKey(0)
    , _greenRatio(0.)
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
    smoothing->setHintToolTip(tr("Passes of a median filter on the colour differences after demosaicing, against colour artefacts."));
    smoothing->setDefaultValue(0);
    smoothing->setMinimum(0);
    smoothing->setMaximum(5);
    smoothing->setDisplayMinimum(0);
    smoothing->setDisplayMaximum(5);
    smoothing->setAnimationEnabled(false);
    page->addKnob(smoothing);
    _colorSmoothing = smoothing;
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
        case eRawDemosaicBayerVNG4:
            s.algorithm = Algorithm::VNG;
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
        case eRawDemosaicXTransVNG:
            s.algorithm = Algorithm::VNG;
            break;
        default:
            s.algorithm = Algorithm::Markesteijn1;
            break;
        }
    }
    if (tiny && (s.algorithm != Algorithm::PassThrough)) {
        s.algorithm = Algorithm::VNG;
    }
    s.edgeThreshold = (float)reader.value(_edgeThreshold);
    s.smoothing = std::max(0, std::min(5, (int)reader.value(_colorSmoothing)));
    const std::array<double, 3> maximum = RawMetadata::getProcessedMaximum(source).value_or(std::array<double, 3> { 1., 1., 1. });
    s.processedMaximum = std::fmax(1.0f, (float)std::max({ maximum[0], maximum[1], maximum[2] }));
    s.processedMinimum = std::fmax(1.0f, (float)std::min({ maximum[0], maximum[1], maximum[2] }));
    s.iso = (float)RawMetadata::getIso(source).value_or(0.);

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
    if (settings->algorithm == Algorithm::PassThrough) {
        return window.intersect(rod);
    }
    const bool local = (settings->greenEq == eRawDemosaicGreenEqLocal) || (settings->greenEq == eRawDemosaicGreenEqBoth);
    const int extra = settings->smoothing + (local ? 2 : 0);
    if (settings->algorithm != Algorithm::AMaZE) {
        return RawNodeSupport::expandForKernel(window, algorithmHalo(settings->algorithm) + extra, settings->layout.period(), kMinimumBuffer, rod);
    }
    const RectI grown = RectI(window.x1 - extra, window.y1 - extra, window.x2 + extra, window.y2 + extra).intersect(rod);
    if (grown.isNull()) {
        return grown;
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
    const RectI need = full ? settings->sourceRoD : sourceRegionFor(PassExec::toPixelEnclosing(renderWindow, scale, par), time, view);
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

    // The output is an image: what described the mosaic no longer describes it. The camera
    // matrix and the white balance stay for RawInputColor.
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

    RawNodeSupport::TopDownBuffer rgb;
    if (s.algorithm == Algorithm::PassThrough) {
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
                const U64 key = renderKey(getRenderHash(), args.time, args.view);
                double ratio = 0.;
                {
                    std::lock_guard<std::mutex> lock(_greenMutex);
                    if (!_greenValid || (_greenKey != key)) {
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
        case Algorithm::VNG:
            RawKernels::vng(out.data.data(), in, width, height, filters, xtrans, false, parallel);
            break;
        case Algorithm::Markesteijn1:
            RawKernels::markesteijn(out.data.data(), in, width, height, xtrans, 1, parallel);
            break;
        case Algorithm::Markesteijn3:
            RawKernels::markesteijn(out.data.data(), in, width, height, xtrans, 3, parallel);
            break;
        case Algorithm::PassThrough:
            break;
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

NATRON_NAMESPACE_EXIT
