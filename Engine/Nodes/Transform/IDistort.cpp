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

#include "IDistort.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/WarpGpu.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// The UV rows one band keeps: the coordinates of rows y - 1, y and y + 1 over the window grown
// by one pixel on each side, rolled down as the band moves up.
struct CoordRows {
    std::vector<float> sx[3];
    std::vector<float> sy[3];
};
} // anonymous namespace

IDistort::IDistort(NodePtr node)
    : StagedImageEffect(node)
{
}

IDistort::~IDistort()
{
}

const NodeSpec*
IDistort::nodeSpec() const
{
    return &kIDistortNodeSpec;
}

const ImagePipelineSpec&
IDistort::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kIDistortPipeline, kIDistortNodeSpec>();
}

std::string
IDistort::getInputHint(int inputNb) const
{
    if (inputNb == kIDistortInputSource) {
        return tr("The image to distort.").toStdString();
    }
    if (inputNb == kIDistortInputUV) {
        return tr("The displacement, in pixels, read from the U and V channels.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

void
IDistort::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
IDistort::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChannelSelectPtr channelU = createKnob<KnobChannelSelect>(tr("U Channel"));
    channelU->setName(kIDistortParamChannelU);
    channelU->setAnimationEnabled(false);
    channelU->setDefaultValue(channelU->encode(std::string(kNatronColorViewRGBA) + ".R"));
    channelU->setHintToolTip(tr("The channel of the UV input, in any of its layers, the horizontal displacement is "
                                "read from. A channel the UV input does not have, or None, reads as 0."));
    page->addKnob(channelU);
    _channelU = channelU;

    KnobChannelSelectPtr channelV = createKnob<KnobChannelSelect>(tr("V Channel"));
    channelV->setName(kIDistortParamChannelV);
    channelV->setAnimationEnabled(false);
    channelV->setDefaultValue(channelV->encode(std::string(kNatronColorViewRGBA) + ".G"));
    channelV->setHintToolTip(tr("The channel of the UV input, in any of its layers, the vertical displacement is "
                                "read from. A channel the UV input does not have, or None, reads as 0."));
    page->addKnob(channelV);
    _channelV = channelV;

    KnobDoublePtr uvOffset = createKnob<KnobDouble>(tr("UV Offset"), 2);
    uvOffset->setName(kIDistortParamUVOffset);
    uvOffset->setHintToolTip(tr("Offset to subtract from the U and V channels (useful if these were stored in a file "
                                "that cannot handle negative numbers)."));
    uvOffset->setDimensionName(0, "U");
    uvOffset->setDimensionName(1, "V");
    uvOffset->setDefaultValue(0., 0);
    uvOffset->setDefaultValue(0., 1);
    uvOffset->setDisplayMinimum(0., 0);
    uvOffset->setDisplayMinimum(0., 1);
    uvOffset->setDisplayMaximum(1., 0);
    uvOffset->setDisplayMaximum(1., 1);
    page->addKnob(uvOffset);
    _uvOffset = uvOffset;

    KnobDoublePtr uvScale = createKnob<KnobDouble>(tr("UV Scale"), 2);
    uvScale->setName(kIDistortParamUVScale);
    uvScale->setHintToolTip(tr("Scale factor to apply to the U and V channels after subtracting the offset (useful if "
                               "these were stored in a file that can only store integer values). The displacement is "
                               "in full-resolution pixels."));
    uvScale->setDimensionName(0, "U");
    uvScale->setDimensionName(1, "V");
    uvScale->setDefaultValue(1., 0);
    uvScale->setDefaultValue(1., 1);
    uvScale->setDisplayMinimum(0., 0);
    uvScale->setDisplayMinimum(0., 1);
    uvScale->setDisplayMaximum(100., 0);
    uvScale->setDisplayMaximum(100., 1);
    page->addKnob(uvScale);
    _uvScale = uvScale;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    _clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(false);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    addStageKnobs(page);

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(channelU, kIDistortInputUV, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(channelV, kIDistortInputUV, LayerKnobSpec::eRoleInputBound);
    }

    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
} // IDistort::initializeKnobs

void
IDistort::updateClampEnabled(double time,
                             ViewIdx view)
{
    KnobChoicePtr filter = _filter.lock();
    KnobBoolPtr clamp = _clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
IDistort::onKnobsLoaded()
{
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

bool
IDistort::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec view,
                      double time,
                      bool /*originatedFromMainThread*/)
{
    KnobChoicePtr filter = _filter.lock();
    if (filter && (k == filter.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }

    return false;
}

void
IDistort::readSettings(double time,
                       ViewIdx view,
                       Settings* settings) const
{
    KnobChoicePtr filter = _filter.lock();
    if (filter) {
        const int value = filter->getValueAtTime(time, 0, view);
        if ((value >= (int)Resampler::eFilterImpulse) && (value <= (int)Resampler::eFilterNotch)) {
            settings->filter = (Resampler::FilterEnum)value;
        }
    }
    KnobBoolPtr clamp = _clamp.lock();
    if (clamp) {
        settings->clamp = clamp->getValueAtTime(time, 0, view);
    }
    KnobBoolPtr blackOutside = _blackOutside.lock();
    if (blackOutside) {
        settings->blackOutside = blackOutside->getValueAtTime(time, 0, view);
    }
    KnobDoublePtr uvOffset = _uvOffset.lock();
    if (uvOffset) {
        settings->uOffset = uvOffset->getValueAtTime(time, 0, view);
        settings->vOffset = uvOffset->getValueAtTime(time, 1, view);
    }
    KnobDoublePtr uvScale = _uvScale.lock();
    if (uvScale) {
        settings->uScale = uvScale->getValueAtTime(time, 0, view);
        settings->vScale = uvScale->getValueAtTime(time, 1, view);
    }
}

void
IDistort::resolveUVChannels(double time,
                            ViewIdx view,
                            UVChannel* u,
                            UVChannel* v)
{
    *u = UVChannel();
    *v = UVChannel();
    if (!getInput(kIDistortInputUV)) {
        return;
    }
    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, kIDistortInputUV, &present);

    const KnobChannelSelectPtr knobs[2] = { _channelU.lock(), _channelV.lock() };
    UVChannel* const channels[2] = { u, v };
    for (int i = 0; i < 2; ++i) {
        if (knobs[i] && !knobs[i]->isNone()) {
            channels[i]->valid = knobs[i]->resolve(present, &channels[i]->layer, &channels[i]->channel) && (channels[i]->channel >= 0);
        }
    }
}

StatusEnum
IDistort::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    *rod = RectD();
    EffectInstancePtr input = getInput(kIDistortInputSource);
    if (!input) {
        return eStatusOK;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;
    const StatusEnum status = input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat);
    if (status == eStatusFailed) {
        *rod = RectD();
    }

    return status;
}

void
IDistort::getRegionsOfInterest(double time,
                               const RenderScale& scale,
                               const RectD& outputRoD,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    const auto addRegion = [ret](const EffectInstancePtr& input, const RectD& region) {
        RoIMap::iterator found = ret->find(input);
        if (found == ret->end()) {
            ret->insert(std::make_pair(input, region));
        } else {
            found->second.merge(region);
        }
    };

    // Any displacement may land anywhere in the Source, so it is read whole.
    EffectInstancePtr source = getInput(kIDistortInputSource);
    if (source) {
        const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
        RectD sourceRoD;
        bool isProjectFormat = false;
        if ((source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &sourceRoD, &isProjectFormat) != eStatusFailed) && !sourceRoD.isNull()) {
            addRegion(source, sourceRoD);
        }
    }

    // The Jacobians read the UV of the neighbouring pixels.
    EffectInstancePtr uv = getInput(kIDistortInputUV);
    if (uv) {
        const double par = getAspectRatio(-1);
        RectI pixels = PassExec::toPixelEnclosing(renderWindow, scale, par);
        pixels.x1 -= 1;
        pixels.y1 -= 1;
        pixels.x2 += 1;
        pixels.y2 += 1;
        addRegion(uv, PassExec::toCanonical(pixels, scale, par));
    }
}

bool
IDistort::getInputLayersRead(int inputNb,
                             double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers)
{
    if (inputNb != kIDistortInputUV) {
        return false;
    }
    UVChannel u;
    UVChannel v;
    resolveUVChannels(time, view, &u, &v);
    if (u.valid) {
        layers->push_back(u.layer);
    }
    if (v.valid && (!u.valid || !(v.layer == u.layer))) {
        layers->push_back(v.layer);
    }

    return !layers->empty();
}

bool
IDistort::isOpIdentity(double time,
                       const RenderScale& /*scale*/,
                       const RectI& /*roi*/,
                       ViewIdx view)
{
    if (!getInput(kIDistortInputUV)) {
        return true;
    }
    Settings settings;
    readSettings(time, view, &settings);
    if ((settings.uScale == 0.) && (settings.vScale == 0.)) {
        return true;
    }
    KnobChannelSelectPtr channelU = _channelU.lock();
    KnobChannelSelectPtr channelV = _channelV.lock();

    return (!channelU || channelU->isNone()) && (!channelV || channelV->isNone());
}

StatusEnum
IDistort::renderOp(const RenderActionArgs& args,
                   const StagePlan& plan)
{
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    const int sourceInput = plan.sourceInput;

    Settings settings;
    readSettings(args.time, args.view, &settings);
    if (args.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const bool impulse = settings.filter == Resampler::eFilterImpulse;
    const OfxPointD s = args.mappedScale.toOfxPointD();
    const double uScale = settings.uScale * s.x;
    const double vScale = settings.vScale * s.y;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!PassExec::isFloatImage(mask)) {
        return eStatusFailed;
    }

    UVChannel uvChannels[2];
    resolveUVChannels(args.time, args.view, &uvChannels[0], &uvChannels[1]);
    ImagePtr uvImages[2];
    for (int i = 0; i < 2; ++i) {
        if (!uvChannels[i].valid) {
            continue;
        }
        if ((i == 1) && uvChannels[0].valid && (uvChannels[1].layer == uvChannels[0].layer)) {
            uvImages[1] = uvImages[0];
        } else {
            RectI uvRoI;
            uvImages[i] = getImage(kIDistortInputUV, args.time, args.mappedScale, args.view, NULL, &uvChannels[i].layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &uvRoI);
        }
        if (!PassExec::isFloatImage(uvImages[i])) {
            return eStatusFailed;
        }
        if (uvImages[i] && (uvChannels[i].channel >= (int)uvImages[i]->getComponentsCount())) {
            uvChannels[i].valid = false;
        }
    }

    struct Plane {
        ImagePtr dst;
        ImagePtr src;
        int nComps = 0;
        int srcNComps = 0;
        std::bitset<4> channels;
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::shared_ptr<Image::WriteAccess> dstAccess;
        std::vector<float> converted;
        Resampler::SourceImage source;
    };
    std::vector<std::unique_ptr<Plane>> planes;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        std::unique_ptr<Plane> plane(new Plane());
        plane->dst = it->second;
        if (plane->dst) {
            plane->nComps = (int)plane->dst->getComponentsCount();
            plane->channels = PassExec::processedBitsForImage(it->first, plane->nComps, args.processChannels);
            ImageLayerDesc sourceLayer;
            if ((sourceInput >= 0) && getInput(sourceInput) && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                RectI srcRoI;
                plane->src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &srcRoI);
            }
            if (!PassExec::isFloatImage(plane->dst) || !PassExec::isFloatImage(plane->src)) {
                return eStatusFailed;
            }
        }
        planes.push_back(std::move(plane));
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked and their bounds read here, on the calling thread; the band threads only
    // read through these.
    PassExec::MaskInput maskSource;
    maskSource.image = mask;
    maskSource.lock();
    std::shared_ptr<Image::ReadAccess> uvAccess[2];
    RectI uvBounds[2];
    int uvNComps[2] = { 0, 0 };
    for (int i = 0; i < 2; ++i) {
        if (!uvChannels[i].valid || !uvImages[i]) {
            continue;
        }
        if ((i == 1) && (uvImages[1] == uvImages[0]) && uvAccess[0]) {
            uvAccess[1] = uvAccess[0];
            uvBounds[1] = uvBounds[0];
            uvNComps[1] = uvNComps[0];
            continue;
        }
        uvBounds[i] = uvImages[i]->getBounds();
        uvNComps[i] = (int)uvImages[i]->getComponentsCount();
        uvAccess[i] = std::make_shared<Image::ReadAccess>(uvImages[i].get());
    }
    for (std::size_t p = 0; p < planes.size(); ++p) {
        Plane& plane = *planes[p];
        if (!plane.dst) {
            continue;
        }
        const int nComps = plane.nComps;
        if (plane.src) {
            plane.srcBounds = plane.src->getBounds();
            const int srcNComps = (int)plane.src->getComponentsCount();
            plane.srcNComps = srcNComps;
            plane.srcAccess = std::make_shared<Image::ReadAccess>(plane.src.get());
            if (srcNComps == nComps) {
                plane.source = Resampler::SourceImage((const float*)plane.srcAccess->pixelAt(plane.srcBounds.x1, plane.srcBounds.y1), plane.srcBounds, nComps);
            } else {
                const int srcWidth = plane.srcBounds.width();
                plane.converted.assign((std::size_t)srcWidth * plane.srcBounds.height() * nComps, 0.f);
                for (int y = plane.srcBounds.y1; y < plane.srcBounds.y2; ++y) {
                    PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, srcNComps, plane.srcBounds.x1, y, srcWidth, nComps,
                                            &plane.converted[(std::size_t)(y - plane.srcBounds.y1) * srcWidth * nComps]);
                }
                plane.source = Resampler::SourceImage(plane.converted.empty() ? (const float*)0 : &plane.converted[0], plane.srcBounds, nComps);
            }
        } else {
            plane.source = Resampler::SourceImage((const float*)0, RectI(), nComps);
        }
        plane.dstAccess = std::make_shared<Image::WriteAccess>(plane.dst.get());
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    std::vector<std::pair<std::size_t, RectI>> bands;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        if (!planes[p]->dst) {
            continue;
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(std::make_pair(p, bandRects[b]));
        }
    }

    // The coordinates of a row run over the window grown by one pixel on each side, so that the
    // Jacobians at the window's edges see their true neighbours.
    const int gridX0 = roi.x1 - 1;
    const int gridWidth = width + 2;
    const UVChannel* const uvChannel = uvChannels;
    const auto coordRow = [&](int y, float* uRow, float* vRow, float* sx, float* sy) {
        float* const values[2] = { uRow, vRow };
        for (int i = 0; i < 2; ++i) {
            if (uvChannel[i].valid && uvImages[i]) {
                PassExec::readChannelRow(uvImages[i].get(), uvAccess[i].get(), uvBounds[i], uvNComps[i], uvChannel[i].channel, gridX0, y, gridWidth, 0.f, values[i]);
            } else {
                std::fill(values[i], values[i] + gridWidth, 0.f);
            }
        }
        const double cy = (double)y + 0.5;
        for (int i = 0; i < gridWidth; ++i) {
            sx[i] = (float)((double)(gridX0 + i) + 0.5 + ((double)uRow[i] - settings.uOffset) * uScale);
            sy[i] = (float)(cy + ((double)vRow[i] - settings.vOffset) * vScale);
        }
    };

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const Plane& plane = *planes[bands[bandIndex].first];
        const RectI& band = bands[bandIndex].second;
        const int nComps = plane.nComps;
        const bool hasSource = plane.source.isValid();
        const std::size_t rowSize = (std::size_t)width * nComps;
        const bool direct = stageFinishIsCopy(doMask, mix, false, nComps, plane.channels);
        std::vector<float> distortedRow(direct ? 0 : rowSize);
        std::vector<float> referenceRow(direct ? 0 : rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> uRow(gridWidth);
        std::vector<float> vRow(gridWidth);
        std::vector<float> jacobian(impulse ? 0 : (std::size_t)gridWidth * 4);
        CoordRows rows;
        for (int r = 0; r < 3; ++r) {
            rows.sx[r].resize(gridWidth);
            rows.sy[r].resize(gridWidth);
        }
        // rows.*[0] holds row y - 1, [1] row y and [2] row y + 1; only [1] when impulse.
        bool primed = false;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % PassExec::kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)plane.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                primed = false;
                continue;
            }
            float* const outRow = direct ? dstPix : distortedRow.data();
            if (!hasSource) {
                std::fill(outRow, outRow + rowSize, 0.f);
            } else if (impulse) {
                coordRow(y, uRow.data(), vRow.data(), rows.sx[1].data(), rows.sy[1].data());
                Resampler::resampleRowFromCoords(plane.source, rows.sx[1].data() + 1, rows.sy[1].data() + 1, NULL, width, settings.filter, settings.clamp, settings.blackOutside,
                                                 Resampler::eWrapNone, Resampler::eWrapNone, outRow, nComps);
            } else {
                if (!primed) {
                    coordRow(y - 1, uRow.data(), vRow.data(), rows.sx[0].data(), rows.sy[0].data());
                    coordRow(y, uRow.data(), vRow.data(), rows.sx[1].data(), rows.sy[1].data());
                    primed = true;
                } else {
                    for (int r = 0; r < 2; ++r) {
                        rows.sx[r].swap(rows.sx[r + 1]);
                        rows.sy[r].swap(rows.sy[r + 1]);
                    }
                }
                coordRow(y + 1, uRow.data(), vRow.data(), rows.sx[2].data(), rows.sy[2].data());
                Resampler::coordJacobiansWithinBounds(rows.sx[0].data(), rows.sx[1].data(), rows.sx[2].data(), rows.sy[0].data(), rows.sy[1].data(), rows.sy[2].data(),
                                                      gridX0, y, gridWidth, uvBounds[0], uvBounds[1], 1., 1., jacobian.data());
                Resampler::resampleRowFromCoords(plane.source, rows.sx[1].data() + 1, rows.sy[1].data() + 1, jacobian.data() + 4, width, settings.filter, settings.clamp, settings.blackOutside,
                                                 Resampler::eWrapNone, Resampler::eWrapNone, outRow, nComps);
            }
            if (direct) {
                continue;
            }
            PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, plane.src ? plane.srcNComps : nComps, roi.x1, y, width, nComps, &referenceRow[0]);
            if (doMask) {
                PassExec::readChannelRow(mask.get(), maskSource.access.get(), maskSource.bounds, maskSource.nComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }

            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &referenceRow[(std::size_t)i * nComps];
                const float* outPix = &distortedRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !plane.channels[pixelKernelChannelBit(nComps, c)]) {
                        if (passThrough) {
                            dstPix[c] = srcPix[c];
                        }
                        continue;
                    }
                    float v = outPix[c];
                    if (alpha == 0.f) {
                        v = srcPix[c];
                    } else if (alpha != 1.f) {
                        v = v * alpha + (1.f - alpha) * srcPix[c];
                    }
                    dstPix[c] = v;
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // IDistort::renderOp

bool
IDistort::resolveGpuUVLayer(double time,
                            ViewIdx view,
                            ImageLayerDesc* layer,
                            int* uChannel,
                            int* vChannel) const
{
    *layer = ImageLayerDesc();
    *uChannel = -1;
    *vChannel = -1;
    UVChannel u;
    UVChannel v;
    const_cast<IDistort*>(this)->resolveUVChannels(time, view, &u, &v);
    if (u.valid && v.valid && !(u.layer == v.layer)) {
        return false;
    }
    const UVChannel& used = u.valid ? u : v;
    if (!used.valid) {
        return true;
    }
    if ((used.layer.getNumComponents() < 1) || (used.layer.getNumComponents() > 4)) {
        return false;
    }
    *layer = used.layer;
    *uChannel = u.valid ? u.channel : -1;
    *vChannel = v.valid ? v.channel : -1;

    return true;
}

bool
IDistort::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    ImageLayerDesc layer;
    int uChannel = -1;
    int vChannel = -1;

    return resolveGpuUVLayer(query.time, query.view, &layer, &uChannel, &vChannel);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
IDistort::getOpGpuInputLayer(int inputNb,
                             const ImageLayerDesc& plane,
                             double time,
                             ViewIdx view,
                             ImageLayerDesc* layer) const
{
    if (inputNb == kIDistortInputUV) {
        int uChannel = -1;
        int vChannel = -1;
        if (resolveGpuUVLayer(time, view, layer, &uChannel, &vChannel) && (layer->getNumComponents() > 0)) {
            return true;
        }
    }
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
IDistort::getOpGpuInputRoI(int inputNb,
                           const RectI& outputRect,
                           const RenderScale& scale) const
{
    if ((inputNb == kIDistortInputSource) || (inputNb == kIDistortInputUV)) {
        return warpGpuInputRoI(*this, inputNb, kIDistortInputSource, kIDistortInputUV, outputRect, scale);
    }

    return StagedImageEffect::getOpGpuInputRoI(inputNb, outputRect, scale);
}

StatusEnum
IDistort::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    ImageLayerDesc uvLayer;
    int uChannel = -1;
    int vChannel = -1;
    if (!canRenderOpOnGpu(query) || !resolveGpuUVLayer(query.time, query.view, &uvLayer, &uChannel, &vChannel)) {
        return eStatusFailed;
    }
    Settings settings;
    readSettings(query.time, query.view, &settings);
    if (query.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const OfxPointD s = query.mappedScale.toOfxPointD();

    WarpGpuParams params;
    params.filter = (uint32_t)settings.filter;
    params.flags = (settings.clamp ? kWarpGpuClamp : 0u) | (settings.blackOutside ? kWarpGpuBlackOutside : 0u) | ((settings.filter != Resampler::eFilterImpulse) ? kWarpGpuJacobian : 0u) | kWarpGpuSample;
    setWarpGpuDouble(settings.uOffset, params.uOffset);
    setWarpGpuDouble(settings.vOffset, params.vOffset);
    setWarpGpuDouble(settings.uScale * s.x, params.uScale);
    setWarpGpuDouble(settings.vScale * s.y, params.vScale);

    return recordWarpGpu(*this, resolveGpuPlan(query), args, (int)gpu::GpuKernelId::idistort, params, kIDistortInputUV, uvLayer, uChannel, vChannel);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
