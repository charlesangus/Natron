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
 * The maths are a port of darktable's raw denoise module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/rawdenoise.c, Copyright (C) 2011-2026 darktable developers,
 * distributed under the GNU GPL version 3 or later. The kernel is in RawDenoiseKernel.cpp.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawDenoise.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawDenoiseInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawDenoiseNodeSpec = {
    .id = PLUGINID_NATRON_RAWDENOISE,
    .label = "RawDenoise",
    .description = "Reduces noise on a prepared Bayer or X-Trans mosaic, before demosaicing, by removing the small wavelet "
                   "details of each colour. Data without a colour filter array passes through. Goes after RawPrepare.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWDENOISE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWDENOISE ".png",
    .outputKind = eDataKindRaw,
    .inputs = kRawDenoiseInputs,
};

constexpr ImagePipelineSpec kRawDenoisePipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
    .rawStage = RawStageRequirement::Prepared,
};

// The five bands reach 1 + 2 + 4 + 8 + 16 = 31 samples on each side. A Bayer colour is
// sampled every other photosite, plus one for the parity of the window's edge. X-Trans colours
// are filled in at every photosite from neighbours up to two photosites away, except where
// darktable leaves green unfilled in the image's last column: there green denoises over red's
// result, which reaches as far again.
const int kBayerHalo = 2 * 31 + 2;
const int kXTransHalo = 2 * (31 + 2);

// darktable's X-Trans extraction reads two columns in from each edge and its rows above.
const int kXTransMinimumSize = 6;

const char* const kChannelNames[4] = { "All", "Red", "Green", "Blue" };
const char* const kBandNames[RawKernels::kRawDenoiseBands] = { "Coarsest", "Coarse", "Medium", "Fine", "Finest" };
} // namespace

struct RawDenoise::Settings {
    RawLayout layout;
    RectI sourceRoD;
    float threshold = 0.f;
    float force[4][RawKernels::kRawDenoiseBands] = {};
    // Why the node passes through, empty when it denoises.
    std::string passThrough;
};

RawDenoise::RawDenoise(NodePtr node)
    : StagedImageEffect(node)
    , _threshold()
    , _strengths()
    , _message()
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawDenoise::~RawDenoise()
{
}

const NodeSpec*
RawDenoise::nodeSpec() const
{
    return staticNodeSpec<kRawDenoiseNodeSpec>();
}

const ImagePipelineSpec&
RawDenoise::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawDenoisePipeline, kRawDenoiseNodeSpec>();
}

std::string
RawDenoise::strengthKnobName(int channel,
                             int band)
{
    return std::string("strength") + kChannelNames[channel] + kBandNames[band];
}

void
RawDenoise::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobDoublePtr threshold = createKnob<KnobDouble>(tr("Threshold"));
    threshold->setName(kRawDenoiseParamThreshold);
    threshold->setHintToolTip(tr("How much noise is removed: the wavelet details below this level, scaled per band and channel by the "
                                 "strengths below, are taken out. 0 turns the node off."));
    threshold->setDefaultValue(0.01);
    threshold->setMinimum(0.);
    threshold->setMaximum(1.);
    threshold->setDisplayMinimum(0.);
    threshold->setDisplayMaximum(0.1);
    page->addKnob(threshold);
    _threshold = threshold;

    const QString groupLabels[4] = { tr("All Channels"), tr("Red"), tr("Green"), tr("Blue") };
    const QString bandLabels[RawKernels::kRawDenoiseBands] = { tr("Coarsest"), tr("Coarse"), tr("Medium"), tr("Fine"), tr("Finest") };
    for (int channel = 0; channel < 4; ++channel) {
        KnobGroupPtr group = createKnob<KnobGroup>(tr("%1 Strengths").arg(groupLabels[channel]));
        group->setName(std::string("strengths") + kChannelNames[channel]);
        group->setHintToolTip((channel == 0) ? tr("How strongly each wavelet band of every channel is denoised.").toStdString()
                                             : tr("How strongly each wavelet band of %1 is denoised, on top of the strengths for all channels.").arg(groupLabels[channel].toLower()).toStdString());
        group->setDefaultValue(false);
        page->addKnob(group);
        for (int band = 0; band < RawKernels::kRawDenoiseBands; ++band) {
            KnobDoublePtr knob = createKnob<KnobDouble>(bandLabels[band]);
            knob->setName(strengthKnobName(channel, band));
            knob->setHintToolTip((channel == 0) ? tr("How strongly the %1 wavelet band of every channel is denoised; 0.5 is neutral, 0 leaves "
                                                     "the band alone.")
                                                      .arg(bandLabels[band].toLower())
                                                : tr("How strongly the %1 wavelet band of %2 is denoised, on top of the strength for all "
                                                     "channels; 0.5 is neutral, 0 leaves the band alone.")
                                                      .arg(bandLabels[band].toLower(), groupLabels[channel].toLower()));
            knob->setDefaultValue(0.5);
            knob->setMinimum(0.);
            knob->setMaximum(1.);
            knob->setDisplayMinimum(0.);
            knob->setDisplayMaximum(1.);
            group->addKnob(knob);
            _strengths[channel][band] = knob;
        }
    }
}

std::optional<RawDenoise::Settings>
RawDenoise::resolveSettings(double time,
                            ViewIdx view)
{
    Settings s;

    if (!RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &s.sourceRoD)) {
        return std::nullopt;
    }
    const ImageMetadata source = getSourceMetadata(time, view);
    s.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    const KnobReader reader(time, view);
    s.threshold = (float)reader.value(_threshold);
    for (int channel = 0; channel < 4; ++channel) {
        for (int band = 0; band < RawKernels::kRawDenoiseBands; ++band) {
            s.force[channel][band] = (float)reader.value(_strengths[channel][band]);
        }
    }
    if (!s.layout.hasCfa()) {
        const QString label = QString::fromUtf8(getNode()->getLabel_mt_safe().c_str());
        s.passThrough = tr("%1: raw denoising works on a Bayer or X-Trans mosaic, so data without a colour filter array passes through unchanged.").arg(label).toStdString();
    }

    return s;
}

RectI
RawDenoise::sourceRegionFor(const RectI& window,
                            const Settings& settings)
{
    const int period = settings.layout.period();
    const int halo = (period == 6) ? kXTransHalo : kBayerHalo;

    return RawNodeSupport::expandForKernel(window, halo, period, 0, settings.sourceRoD);
}

bool
RawDenoise::isOpIdentity(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view)
{
    const std::optional<Settings> settings = resolveSettings(time, view);

    if (!settings) {
        return false;
    }
    if (!settings->passThrough.empty()) {
        _message.post(getNode(), eMessageTypeWarning, settings->passThrough);

        return true;
    }
    _message.withdraw(getNode());

    return !(settings->threshold > 0.f);
}

void
RawDenoise::getRegionsOfInterest(double time,
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
    const RectI need = sourceRegionFor(PassExec::toPixelEnclosing(renderWindow, scale, par), *settings);
    (*ret)[input] = PassExec::toCanonical(need, scale, par);
}

StatusEnum
RawDenoise::renderOp(const RenderActionArgs& args,
                     const StagePlan& /*plan*/)
{
    const std::optional<Settings> found = resolveSettings(args.time, args.view);

    if (!found || !found->passThrough.empty()) {
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
    const RectI need = sourceRegionFor(roi, s);

    RawNodeSupport::TopDownBuffer mosaic;
    mosaic.allocate(need, 1);
    RawNodeSupport::readTopDown(source, &mosaic, parallel);
    RawNodeSupport::TopDownBuffer denoised;
    denoised.allocate(need, 1);
    const int width = need.width();
    const int height = need.height();
    if ((width > 0) && (height > 0)) {
        if (s.layout.period() == 6) {
            if ((width < kXTransMinimumSize) || (height < kXTransMinimumSize)) {
                denoised.data = mosaic.data;
            } else {
                std::uint8_t xtrans[6][6] = {};
                RawNodeSupport::xtransAt(s.layout, need, xtrans);
                RawKernels::rawDenoiseXTrans(mosaic.data.data(), denoised.data.data(), width, height, xtrans, s.threshold, s.force, parallel);
            }
        } else {
            const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(s.layout, need);
            RawKernels::rawDenoiseBayer(mosaic.data.data(), denoised.data.data(), width, height, filters, s.threshold, s.force, parallel);
        }
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
            float* out = (float*)access.pixelAt(roi.x1, y);
            if (!out) {
                return;
            }
            for (int x = roi.x1; x < roi.x2; ++x, out += dstComps) {
                const bool inside = (x >= denoised.rect.x1) && (x < denoised.rect.x2) && (y >= denoised.rect.y1) && (y < denoised.rect.y2);
                const float value = inside ? denoised.data[denoised.offset(x, y)] : 0.f;
                for (int c = 0; c < dstComps; ++c) {
                    out[c] = (c == 0) ? value : 0.f;
                }
            }
        });
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // RawDenoise::renderOp

bool
RawDenoise::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }
    // Resolving only reads knobs and the source's metadata and region.
    const std::optional<Settings> settings = const_cast<RawDenoise*>(this)->resolveSettings(query.time, query.view);
    if (!settings) {
        return false;
    }

    return !settings->passThrough.empty() || (settings->layout.period() == 2) || (settings->layout.period() == 6);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
RawDenoise::getOpGpuInputLayer(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view,
                               ImageLayerDesc* layer) const
{
    if (inputNb != getSourceInput()) {
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }

    return const_cast<RawDenoise*>(this)->resolveInputPlaneForRender(inputNb, time, view, layer, NULL);
}

RectI
RawDenoise::getOpGpuInputRoI(int inputNb,
                             const RectI& outputRect,
                             const RenderScale& /*scale*/) const
{
    if (inputNb != getSourceInput()) {
        return outputRect;
    }
    const std::optional<Settings> settings = const_cast<RawDenoise*>(this)->resolveSettings(getCurrentTime(), getCurrentView());
    if (!settings || !settings->passThrough.empty() || !(settings->threshold > 0.f)) {
        return outputRect;
    }

    return sourceRegionFor(outputRect, *settings);
}

StatusEnum
RawDenoise::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI& rect = args.outputRect;
    const GpuImageView& out = args.output;
    const int nComps = query.nComps;
    if (!canRenderOpOnGpu(query) || rect.isNull() || !out.buffer || (out.nComps != nComps) || !out.bounds.contains(rect) || ((out.byteOffset % sizeof(float)) != 0)) {
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
    // isOpIdentity() keeps the engine from rendering these; a direct render copies the source.
    const bool identity = !s.passThrough.empty() || !(s.threshold > 0.f);
    const bool xtrans = (s.layout.period() == 6);

    RawDenoiseGpuPush base = {};
    if (hasSource) {
        base.srcOffset = (std::uint32_t)(source.byteOffset / sizeof(float));
        base.srcStride = (std::uint32_t)source.rowStrideFloats;
        base.srcComps = (std::uint32_t)source.nComps;
    } else {
        base.srcComps = 1;
    }
    base.dstStride = (std::uint32_t)out.rowStrideFloats;
    base.dstComps = (std::uint32_t)nComps;

    // Bands of the window are denoised one after the other through one work buffer, each over the
    // source region the CPU would read for it, which a tile's result does not depend on. A band
    // holds three planes of a colour: the whole region for X-Trans, a quarter of it for Bayer.
    const std::uint64_t kMaxWorkFloats = std::uint64_t(1) << 25;
    const std::uint64_t floatsPerPhotosite = xtrans ? 3 : 1;
    const auto planeFloatsFor = [xtrans](const RectI& need) {
        const std::uint64_t w = (std::uint64_t)std::max(0, need.width());
        const std::uint64_t h = (std::uint64_t)std::max(0, need.height());

        return xtrans ? w * h : ((w + 1) / 2) * ((h + 1) / 2);
    };
    std::vector<RectI> bands;
    std::vector<RectI> needs;
    std::uint64_t workFloats = 4;
    if (identity) {
        bands.push_back(rect);
        needs.push_back(rect);
    } else {
        const RectI firstRow = sourceRegionFor(RectI(rect.x1, rect.y1, rect.x2, rect.y1 + 1), s);
        const std::uint64_t needWidth = (std::uint64_t)std::max(1, firstRow.width());
        const int overhead = std::max(0, firstRow.height() - 1);
        const int bandRows = (int)std::max<std::int64_t>(16, (std::int64_t)(kMaxWorkFloats / (floatsPerPhotosite * needWidth)) - overhead);
        for (int y = rect.y1; y < rect.y2; y += bandRows) {
            const RectI band(rect.x1, y, rect.x2, std::min(rect.y2, y + bandRows));
            const RectI need = sourceRegionFor(band, s);
            bands.push_back(band);
            needs.push_back(need);
            workFloats = std::max(workFloats, 3 * planeFloatsFor(need));
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
    const auto run = [&](gpu::GpuKernelId kernel, const RawDenoiseGpuPush& push, std::uint32_t x, std::uint32_t y) {
        if (!ctx.dispatch(kernel, { src, work, out.buffer }, push, { std::max<std::uint32_t>(x, 1), std::max<std::uint32_t>(y, 1), 1 })) {
            return false;
        }
        ctx.computeBarrier();

        return true;
    };
    const auto denoisePlane = [&](RawDenoiseGpuPush& push, int color) {
        float noise[RawKernels::kRawDenoiseBands];
        RawKernels::rawDenoiseChannelNoise(noise, color, s.threshold, s.force);
        for (int lev = 0; lev < RawKernels::kRawDenoiseBands; ++lev) {
            push.level = (std::uint32_t)lev;
            push.thold = noise[lev];
            push.last = ((lev + 1) == RawKernels::kRawDenoiseBands) ? 1u : 0u;
            if (!run(gpu::GpuKernelId::rawdenoise_vert, push, push.planeWidth, push.planeHeight) || !run(gpu::GpuKernelId::rawdenoise_horiz, push, push.planeWidth, push.planeHeight)) {
                return false;
            }
        }

        return true;
    };

    for (std::size_t b = 0; b < bands.size(); ++b) {
        const RectI& band = bands[b];
        const RectI& need = needs[b];
        RawDenoiseGpuPush push = base;
        const int width = std::max(0, need.width());
        const int height = std::max(0, need.height());
        push.width = (std::uint32_t)width;
        push.height = (std::uint32_t)height;
        push.planeFloats = (std::uint32_t)planeFloatsFor(need);
        if (hasSource) {
            push.srcCol1 = source.bounds.x1 - need.x1;
            push.srcCol2 = source.bounds.x2 - need.x1;
            push.srcRow1 = need.y2 - source.bounds.y2;
            push.srcRow2 = need.y2 - source.bounds.y1;
        }
        push.dstOffset = (std::uint32_t)(out.byteOffset / sizeof(float) + (std::uint64_t)(band.y1 - out.bounds.y1) * (std::uint64_t)out.rowStrideFloats + (std::uint64_t)(band.x1 - out.bounds.x1) * (std::uint64_t)nComps);
        push.outCol = band.x1 - need.x1;
        push.outRow = need.y2 - 1 - band.y1;
        push.outWidth = (std::uint32_t)band.width();
        push.outHeight = (std::uint32_t)band.height();

        if (identity || (width == 0) || (height == 0) || (xtrans && ((width < kXTransMinimumSize) || (height < kXTransMinimumSize)))) {
            push.mode = identity ? kRawDenoiseGpuModeIdentity : kRawDenoiseGpuModeMosaic;
            if (!run(gpu::GpuKernelId::rawdenoise_write, push, push.outWidth, push.outHeight)) {
                return eStatusFailed;
            }
            continue;
        }

        if (xtrans) {
            std::uint8_t pattern[6][6] = {};
            RawNodeSupport::xtransAt(s.layout, need, pattern);
            for (int i = 0; i < 36; ++i) {
                push.pattern[i / 16] |= (std::uint32_t)(pattern[i / 6][i % 6] & 3) << ((i % 16) * 2);
            }
            push.mode = kRawDenoiseGpuModeXTrans;
            push.planeWidth = push.width;
            push.planeHeight = push.height;
            // The colours share one plane, in order: where the extraction writes nothing, a
            // colour denoises over what the previous one left, as on the CPU.
            for (int c = 0; c < 3; ++c) {
                push.channel = (std::uint32_t)c;
                if (!run(gpu::GpuKernelId::rawdenoise_xtrans_extract, push, push.width, push.height) || !denoisePlane(push, c)
                    || !run(gpu::GpuKernelId::rawdenoise_write, push, push.outWidth, push.outHeight)) {
                    return eStatusFailed;
                }
            }
            continue;
        }

        const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(s.layout, need);
        push.mode = kRawDenoiseGpuModeBayer;
        for (int c = 0; c < 4; ++c) {
            push.channel = (std::uint32_t)c;
            push.rowParity = (std::uint32_t)(c & 1);
            push.colParity = (std::uint32_t)(c >> 1);
            push.planeWidth = (std::uint32_t)((width - (int)push.colParity + 1) / 2);
            push.planeHeight = (std::uint32_t)((height - (int)push.rowParity + 1) / 2);
            if ((push.planeWidth == 0) || (push.planeHeight == 0)) {
                continue;
            }
            const int color = RawKernels::fcBayer((std::size_t)(c % 2), (std::size_t)(c / 2), filters);
            if (!run(gpu::GpuKernelId::rawdenoise_bayer_extract, push, push.planeWidth, push.planeHeight) || !denoisePlane(push, color)
                || !run(gpu::GpuKernelId::rawdenoise_write, push, push.outWidth, push.outHeight)) {
                return eStatusFailed;
            }
        }
    }

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // RawDenoise::renderOpGpu

NATRON_NAMESPACE_EXIT
