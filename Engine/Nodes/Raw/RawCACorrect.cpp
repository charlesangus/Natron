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
 * The maths are a port of darktable's raw chromatic aberrations module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7), src/iop/cacorrect.c, Copyright (C) 2010-2026 darktable
 * developers, after RawTherapee's CA_correct_RT.cc by Emil Martinec and Ingo Weyrich,
 * distributed under the GNU GPL version 3 or later. The kernel is in RawCACorrectKernel.cpp.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawCACorrect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawCACorrectInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawCACorrectNodeSpec = {
    .id = PLUGINID_NATRON_RAWCACORRECT,
    .label = "RawCACorrect",
    .description = "Corrects lateral chromatic aberration on a prepared Bayer mosaic: measures how far red and blue are displaced "
                   "from green across the whole image and moves them back. X-Trans mosaics and data without a colour filter array "
                   "pass through. Goes after RawPrepare and before RawDemosaic.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWCACORRECT,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWCACORRECT ".png",
    .outputKind = eDataKindRaw,
    .inputs = kRawCACorrectInputs,
};

constexpr ImagePipelineSpec kRawCACorrectPipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
    .rawStage = RawStageRequirement::Prepared,
};
} // namespace

struct RawCACorrect::Settings {
    RawLayout layout;
    RectI sourceRoD;
    int iterations = 2;
    bool avoidShift = false;
    float scaler = 1.f;
    // Why the node passes through, empty when it corrects.
    std::string passThrough;
};

RawCACorrect::RawCACorrect(NodePtr node)
    : StagedImageEffect(node)
    , _iterations()
    , _avoidShift()
    , _message()
    , _frameMutex()
    , _frameKey(0)
    , _messageHash(0)
    , _frame()
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawCACorrect::~RawCACorrect()
{
}

void
RawCACorrect::purgeCaches()
{
    std::lock_guard<std::mutex> lock(_frameMutex);

    _frame.reset();
}

const NodeSpec*
RawCACorrect::nodeSpec() const
{
    return staticNodeSpec<kRawCACorrectNodeSpec>();
}

const ImagePipelineSpec&
RawCACorrect::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawCACorrectPipeline, kRawCACorrectNodeSpec>();
}

void
RawCACorrect::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    std::vector<ChoiceOption> counts;
    counts.push_back(ChoiceOption("1", tr("Once").toStdString(), ""));
    counts.push_back(ChoiceOption("2", tr("Twice").toStdString(), ""));
    counts.push_back(ChoiceOption("3", tr("Three Times").toStdString(), ""));
    counts.push_back(ChoiceOption("4", tr("Four Times").toStdString(), ""));
    counts.push_back(ChoiceOption("5", tr("Five Times").toStdString(), ""));
    KnobChoicePtr iterations = createKnob<KnobChoice>(tr("Iterations"));
    iterations->setName(kRawCACorrectParamIterations);
    iterations->setHintToolTip(tr("How many times the aberration is measured and corrected; each pass measures what the previous one left."));
    iterations->populateChoices(counts);
    iterations->setDefaultValue(1);
    iterations->setAnimationEnabled(false);
    page->addKnob(iterations);
    _iterations = iterations;

    KnobBoolPtr avoidShift = createKnob<KnobBool>(tr("Avoid Colour Shift"));
    avoidShift->setName(kRawCACorrectParamAvoidShift);
    avoidShift->setHintToolTip(tr("Measures the colour cast the correction leaves in red and blue, smooths it over a wide area "
                                  "and divides it out."));
    avoidShift->setDefaultValue(false);
    avoidShift->setAnimationEnabled(false);
    page->addKnob(avoidShift);
    _avoidShift = avoidShift;
}

std::optional<RawCACorrect::Settings>
RawCACorrect::resolveSettings(double time,
                              ViewIdx view)
{
    Settings s;

    if (!RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &s.sourceRoD)) {
        return std::nullopt;
    }
    const ImageMetadata source = getSourceMetadata(time, view);
    s.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    const KnobReader reader(time, view);
    s.iterations = std::max(1, std::min(5, (int)reader.value(_iterations) + 1));
    s.avoidShift = reader.value(_avoidShift);
    const std::array<double, 3> maximum = RawMetadata::getProcessedMaximum(source).value_or(std::array<double, 3> { 1., 1., 1. });
    s.scaler = std::fmax(1.0f, (float)std::max({ maximum[0], maximum[1], maximum[2] }));

    const QString label = QString::fromUtf8(getNode()->getLabel_mt_safe().c_str());
    if (s.layout.period() == 6) {
        s.passThrough = tr("%1: chromatic aberration is only corrected on a Bayer mosaic, so the X-Trans data passes through unchanged.").arg(label).toStdString();
    } else if (!s.layout.hasCfa()) {
        s.passThrough = tr("%1: chromatic aberration is only corrected on a Bayer mosaic, so data without a colour filter array passes through unchanged.").arg(label).toStdString();
    } else if ((s.sourceRoD.width() < RawKernels::kCaCorrectMinimumSize) || (s.sourceRoD.height() < RawKernels::kCaCorrectMinimumSize)) {
        s.passThrough = tr("%1: the image is too small to measure chromatic aberration in, so it passes through unchanged.").arg(label).toStdString();
    }

    return s;
}

void
RawCACorrect::postFrameMessage(const Frame& frame,
                               U64 renderHash)
{
    _messageHash = renderHash;
    if (frame.corrected) {
        _message.withdraw(getNode());

        return;
    }
    const QString label = QString::fromUtf8(getNode()->getLabel_mt_safe().c_str());
    _message.post(getNode(), eMessageTypeWarning, tr("%1: the image has too little detail to measure chromatic aberration in, so it passes through unchanged.").arg(label).toStdString());
}

bool
RawCACorrect::isOpIdentity(double time,
                           const RenderScale& /*scale*/,
                           const RectI& /*roi*/,
                           ViewIdx view)
{
    const std::optional<Settings> settings = resolveSettings(time, view);

    if (!settings) {
        return false;
    }
    const U64 renderHash = getRenderHash();
    std::lock_guard<std::mutex> lock(_frameMutex);
    if (!settings->passThrough.empty()) {
        _messageHash = 0;
        _message.post(getNode(), eMessageTypeWarning, settings->passThrough);

        return true;
    }
    if (_frame && (_frameKey == RawNodeSupport::frameKey(renderHash, time, view))) {
        postFrameMessage(*_frame, renderHash);
    } else if (_messageHash != renderHash) {
        _messageHash = 0;
        _message.withdraw(getNode());
    }

    return false;
}

void
RawCACorrect::getRegionsOfInterest(double time,
                                   const RenderScale& scale,
                                   const RectD& outputRoD,
                                   const RectD& renderWindow,
                                   ViewIdx view,
                                   RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    const std::optional<Settings> settings = resolveSettings(time, view);
    if (!input || !settings || !settings->passThrough.empty()) {
        return;
    }
    const double par = getAspectRatio(getSourceInput());
    (*ret)[input] = PassExec::toCanonical(settings->sourceRoD, scale, par);
}

StatusEnum
RawCACorrect::renderOp(const RenderActionArgs& args,
                       const StagePlan& /*plan*/)
{
    const std::optional<Settings> found = resolveSettings(args.time, args.view);

    if (!found || !found->passThrough.empty()) {
        return eStatusFailed;
    }
    const Settings& s = *found;
    const RectI& rod = s.sourceRoD;
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
    const U64 renderHash = getRenderHash();
    const U64 key = RawNodeSupport::frameKey(renderHash, args.time, args.view);

    std::shared_ptr<const Frame> frame;
    {
        // Held while the frame is computed, so that the renders of the other tiles wait for it
        // instead of each measuring the whole image again.
        std::lock_guard<std::mutex> lock(_frameMutex);
        if (!_frame || (_frameKey != key)) {
            RawNodeSupport::noteFrameComputed(kRawCACorrectFrame, key);
            std::shared_ptr<Frame> computed = std::make_shared<Frame>();
            computed->mosaic.allocate(rod, 1);
            RawNodeSupport::readTopDown(source, &computed->mosaic, parallel);
            const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(s.layout, rod);
            computed->corrected = RawKernels::caCorrect(computed->mosaic.data.data(), rod.width(), rod.height(), filters, s.iterations, s.avoidShift, s.scaler, parallel);
            if (cancel.check()) {
                return eStatusFailed;
            }
            _frame = computed;
            _frameKey = key;
        }
        frame = _frame;
        postFrameMessage(*frame, renderHash);
    }

    const RawNodeSupport::TopDownBuffer& mosaic = frame->mosaic;
    const RectI& roi = args.roi;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int dstComps = (int)dst->getComponentsCount();
        Image::WriteAccess access(dst.get());
        parallel(roi.height(), [&](int r) {
            const int y = roi.y1 + r;
            float* out = (float*)access.pixelAt(roi.x1, y);
            if (!out) {
                return;
            }
            for (int x = roi.x1; x < roi.x2; ++x, out += dstComps) {
                const bool inside = (x >= mosaic.rect.x1) && (x < mosaic.rect.x2) && (y >= mosaic.rect.y1) && (y < mosaic.rect.y2);
                const float value = inside ? mosaic.data[mosaic.offset(x, y)] : 0.f;
                for (int c = 0; c < dstComps; ++c) {
                    out[c] = (c == 0) ? value : 0.f;
                }
            }
        });
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // RawCACorrect::renderOp

NATRON_NAMESPACE_EXIT
