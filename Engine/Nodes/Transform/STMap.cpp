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

#include "STMap.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Transform/WarpGpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
void
appendUnique(const ImageLayerDesc& layer,
             std::list<ImageLayerDesc>* layers)
{
    if (std::find(layers->begin(), layers->end(), layer) == layers->end()) {
        layers->push_back(layer);
    }
}

// The region of definition of input in canonical coordinates at time, view and scale.
bool
inputRegion(const EffectInstancePtr& input,
            double time,
            ViewIdx view,
            const RenderScale& scale,
            RectD* rod)
{
    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) != eStatusFailed;
}

// One channel of the UV input, locked on the calling thread; with no image it reads 0.
struct UVPlane {
    ImagePtr image;
    int channel = -1;
    int nComps = 0;
    RectI bounds;
    std::shared_ptr<Image::ReadAccess> access;

    void readRow(int x0,
                 int y,
                 int width,
                 float* row) const
    {
        if (!access) {
            std::fill(row, row + width, 0.f);

            return;
        }
        PassExec::readChannelRow(image.get(), access.get(), bounds, nComps, channel, x0, y, width, 0.f, row);
    }
};
} // namespace

STMap::STMap(NodePtr node)
    : StagedImageEffect(node)
{
}

STMap::~STMap()
{
}

const NodeSpec*
STMap::nodeSpec() const
{
    return staticNodeSpec<kSTMapNodeSpec>();
}

const ImagePipelineSpec&
STMap::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kSTMapPipeline, kSTMapNodeSpec>();
}

std::string
STMap::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kSTMapInputUV:
        return tr("The map: for each output pixel, the normalized position in the Source format to read, in the channels "
                  "U Channel and V Channel name, from any of its layers. It sets the output format.")
            .toStdString();
    case kSTMapInputSource:
        return tr("The image to distort.").toStdString();
    case kSTMapInputMask:
        return tr("The mask limiting where the distortion applies.").toStdString();
    default:
        break;
    }

    return std::string();
}

void
STMap::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobChannelSelectPtr channelU = createKnob<KnobChannelSelect>(tr("U Channel"));
    channelU->setName(kSTMapParamChannelU);
    channelU->setAnimationEnabled(false);
    channelU->setDefaultValue(channelU->encode(std::string(kNatronColorViewRGBA) + ".R"));
    channelU->setHintToolTip(tr("The channel of the UV input, in any of its layers, holding U: the horizontal position in "
                                "the Source format, 0 at its left edge and 1 at its right edge. A channel the input does "
                                "not have, or None, reads 0."));
    page->addKnob(channelU);
    _channelU = channelU;

    KnobChannelSelectPtr channelV = createKnob<KnobChannelSelect>(tr("V Channel"));
    channelV->setName(kSTMapParamChannelV);
    channelV->setAnimationEnabled(false);
    channelV->setDefaultValue(channelV->encode(std::string(kNatronColorViewRGBA) + ".G"));
    channelV->setHintToolTip(tr("The channel of the UV input, in any of its layers, holding V: the vertical position in "
                                "the Source format, 0 at its bottom edge and 1 at its top edge. A channel the input does "
                                "not have, or None, reads 0."));
    page->addKnob(channelV);
    _channelV = channelV;

    KnobDoublePtr uvOffset = createKnob<KnobDouble>(tr("UV Offset"), 2);
    uvOffset->setName(kSTMapParamUVOffset);
    uvOffset->setDefaultValue(0., 0);
    uvOffset->setDefaultValue(0., 1);
    uvOffset->setDisplayMinimum(0., 0);
    uvOffset->setDisplayMaximum(1., 0);
    uvOffset->setDisplayMinimum(0., 1);
    uvOffset->setDisplayMaximum(1., 1);
    uvOffset->setHintToolTip(tr("Subtracted from U and V before they are multiplied by UV Scale."));
    page->addKnob(uvOffset);
    _uvOffset = uvOffset;

    KnobDoublePtr uvScale = createKnob<KnobDouble>(tr("UV Scale"), 2);
    uvScale->setName(kSTMapParamUVScale);
    uvScale->setDefaultValue(1., 0);
    uvScale->setDefaultValue(1., 1);
    uvScale->setDisplayMinimum(0., 0);
    uvScale->setDisplayMaximum(100., 0);
    uvScale->setDisplayMinimum(0., 1);
    uvScale->setDisplayMaximum(100., 1);
    uvScale->setHintToolTip(tr("U and V are multiplied by this once UV Offset is subtracted."));
    page->addKnob(uvScale);
    _uvScale = uvScale;

    std::vector<ChoiceOption> wrapOptions;
    wrapOptions.push_back(ChoiceOption(kSTMapWrapClamp, "Clamp", tr("Positions outside the Source format read its edge pixels.").toStdString()));
    wrapOptions.push_back(ChoiceOption(kSTMapWrapRepeat, "Repeat", tr("The Source format repeats: U - floor(U).").toStdString()));
    wrapOptions.push_back(ChoiceOption(kSTMapWrapMirror, "Mirror", tr("The Source format repeats mirrored.").toStdString()));

    KnobChoicePtr wrapU = createKnob<KnobChoice>(tr("U Wrap Mode"));
    wrapU->setName(kSTMapParamWrapU);
    wrapU->populateChoices(wrapOptions);
    wrapU->setDefaultValue(0);
    wrapU->setAddNewLine(false);
    wrapU->setHintToolTip(tr("How a U outside 0..1 maps into the Source format."));
    page->addKnob(wrapU);
    _wrapU = wrapU;

    KnobChoicePtr wrapV = createKnob<KnobChoice>(tr("V Wrap Mode"));
    wrapV->setName(kSTMapParamWrapV);
    wrapV->populateChoices(wrapOptions);
    wrapV->setDefaultValue(0);
    wrapV->setHintToolTip(tr("How a V outside 0..1 maps into the Source format."));
    page->addKnob(wrapV);
    _wrapV = wrapV;

    KnobChoicePtr formatFrom = createKnob<KnobChoice>(tr("Format From"));
    formatFrom->setName(kSTMapParamFormatFrom);
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kSTMapFormatFromUVFormat, "UV Format", tr("The output covers the format of the UV input.").toStdString()));
        options.push_back(ChoiceOption(kSTMapFormatFromUVRoD, "UV RoD", tr("The output covers the region of definition of the UV input.").toStdString()));
        formatFrom->populateChoices(options);
    }
    formatFrom->setDefaultValue(0);
    formatFrom->setHintToolTip(tr("The region the output covers."));
    page->addKnob(formatFrom);
    _formatFrom = formatFrom;

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
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(channelU, kSTMapInputUV, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(channelV, kSTMapInputUV, LayerKnobSpec::eRoleInputBound);
    }

    addStageKnobs(page);
} // STMap::initializeKnobs

void
STMap::readSettings(double time,
                    ViewIdx view,
                    Settings* settings) const
{
    KnobChoicePtr filter = _filter.lock();
    if (filter) {
        settings->filter = (Resampler::FilterEnum)filter->getValueAtTime(time, 0, view);
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
    const auto wrapMode = [time, view](const KnobChoicePtr& knob) {
        const int index = knob ? knob->getValueAtTime(time, 0, view) : 0;

        return (Resampler::WrapMode)((int)Resampler::eWrapClamp + std::clamp(index, 0, 2));
    };
    settings->wrapU = wrapMode(_wrapU.lock());
    settings->wrapV = wrapMode(_wrapV.lock());
} // STMap::readSettings

void
STMap::resolveUVChannels(double time,
                         ViewIdx view,
                         UVChannel* u,
                         UVChannel* v)
{
    *u = UVChannel();
    *v = UVChannel();
    if (!getInput(kSTMapInputUV)) {
        return;
    }
    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, kSTMapInputUV, &present);

    const KnobChannelSelectPtr knobs[2] = { _channelU.lock(), _channelV.lock() };
    UVChannel* const channels[2] = { u, v };
    for (int i = 0; i < 2; ++i) {
        if (knobs[i] && !knobs[i]->isNone()) {
            channels[i]->valid = knobs[i]->resolve(present, &channels[i]->layer, &channels[i]->channel) && (channels[i]->channel >= 0);
        }
    }
}

bool
STMap::getSourceDomain(double time,
                       ViewIdx view,
                       const RenderScale& scale,
                       RectI* domain)
{
    EffectInstancePtr source = getInput(kSTMapInputSource);
    if (!source) {
        return false;
    }
    const RectI format = source->getOutputFormat();
    if (!format.isNull()) {
        domain->x1 = (int)std::floor(format.x1 * scale.toOfxPointD().x);
        domain->y1 = (int)std::floor(format.y1 * scale.toOfxPointD().y);
        domain->x2 = (int)std::ceil(format.x2 * scale.toOfxPointD().x);
        domain->y2 = (int)std::ceil(format.y2 * scale.toOfxPointD().y);

        return !domain->isNull();
    }
    RectD rod;
    if (!inputRegion(source, time, view, scale, &rod) || rod.isNull()) {
        return false;
    }
    *domain = rod.toPixelEnclosing(scale, source->getAspectRatio(-1));

    return !domain->isNull();
}

Route
STMap::route(double time,
             ViewIdx view)
{
    Q_UNUSED(time);
    Q_UNUSED(view);
    if (!getInput(kSTMapInputUV) || !getInput(kSTMapInputSource)) {
        return Route::black();
    }

    return Route::none();
}

StatusEnum
STMap::getPreferredMetadata(NodeMetadata& metadata)
{
    // The output planes are the Source's, whatever the UV input carries.
    if (getInput(kSTMapInputSource)) {
        metadata.setNComps(-1, metadata.getNComps(kSTMapInputSource));
    }
    EffectInstancePtr uv = getInput(kSTMapInputUV);
    if (uv) {
        const RectI format = uv->getOutputFormat();
        if (!format.isNull()) {
            metadata.setOutputFormat(format);
        }
    }

    return eStatusOK;
}

StatusEnum
STMap::getRegionOfDefinition(U64 /*hash*/,
                             double time,
                             const RenderScale& scale,
                             ViewIdx view,
                             RectD* rod)
{
    EffectInstancePtr uv = getInput(kSTMapInputUV);
    EffectInstancePtr source = getInput(kSTMapInputSource);
    if (!uv || !source) {
        rod->clear();

        return eStatusOK;
    }

    KnobChoicePtr formatFrom = _formatFrom.lock();
    const bool fromRoD = formatFrom && (formatFrom->getValueAtTime(time, 0, view) == 1);
    const RectI format = uv->getOutputFormat();
    if (!fromRoD && !format.isNull()) {
        const double par = uv->getAspectRatio(-1);
        *rod = RectD(format.x1 * par, format.y1, format.x2 * par, format.y2);
    } else if (!inputRegion(uv, time, view, scale, rod)) {
        return eStatusFailed;
    }

    // Under a mask or a partial mix the Source shows through outside the warped region.
    if (isMaskApplied() || (getMixValue(time, view) != 1.)) {
        RectD sourceRoD;
        if (inputRegion(source, time, view, scale, &sourceRoD) && !sourceRoD.isNull()) {
            if (rod->isNull()) {
                *rod = sourceRoD;
            } else {
                rod->merge(sourceRoD);
            }
        }
    }

    return eStatusOK;
} // STMap::getRegionOfDefinition

void
STMap::getRegionsOfInterest(double time,
                            const RenderScale& scale,
                            const RectD& outputRoD,
                            const RectD& renderWindow,
                            ViewIdx view,
                            RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    // The Source is read wherever the map points, and at the window for the blend.
    EffectInstancePtr source = getInput(kSTMapInputSource);
    RectD sourceRoI = renderWindow;
    if (source) {
        RectD sourceRoD;
        if (inputRegion(source, time, view, scale, &sourceRoD) && !sourceRoD.isNull()) {
            sourceRoI.merge(sourceRoD);
        }
        (*ret)[source] = sourceRoI;
    }

    // The Jacobians read UV one pixel beyond the window on every side.
    EffectInstancePtr uv = getInput(kSTMapInputUV);
    if (uv) {
        const double par = getAspectRatio(-1);
        const double dx = par / scale.toOfxPointD().x;
        const double dy = 1. / scale.toOfxPointD().y;
        RectD uvRoI(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 + dx, renderWindow.y2 + dy);
        if (uv == source) {
            uvRoI.merge(sourceRoI);
        }
        (*ret)[uv] = uvRoI;
    }
} // STMap::getRegionsOfInterest

bool
STMap::getInputLayersRead(int inputNb,
                          double time,
                          ViewIdx view,
                          std::list<ImageLayerDesc>* layers)
{
    if (inputNb != kSTMapInputUV) {
        return false;
    }
    UVChannel u;
    UVChannel v;
    resolveUVChannels(time, view, &u, &v);
    if (u.valid) {
        appendUnique(u.layer, layers);
    }
    if (v.valid) {
        appendUnique(v.layer, layers);
    }

    return !layers->empty();
}

bool
STMap::isOpIdentity(double /*time*/,
                    const RenderScale& /*scale*/,
                    const RectI& /*roi*/,
                    ViewIdx /*view*/)
{
    return false;
}

StatusEnum
STMap::renderOp(const RenderActionArgs& args,
                const StagePlan& plan)
{
    Settings settings;
    readSettings(args.time, args.view, &settings);
    if (args.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const bool jacobians = (settings.filter != Resampler::eFilterImpulse);

    RectI domain;
    const bool hasDomain = getSourceDomain(args.time, args.view, args.mappedScale, &domain);

    // U and V, fetched and locked here, on the calling thread; the band threads only read
    // through these.
    UVPlane uvPlanes[2];
    if (hasDomain) {
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
                // A selected layer is read as it is stored, not in the clip's colour layout.
                uvImages[i] = getImage(kSTMapInputUV, args.time, args.mappedScale, args.view, NULL, &uvChannels[i].layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &uvRoI);
            }
            if (!PassExec::isFloatImage(uvImages[i])) {
                return eStatusFailed;
            }
            if (!uvImages[i] || (uvChannels[i].channel >= (int)uvImages[i]->getComponentsCount())) {
                continue;
            }
            uvPlanes[i].image = uvImages[i];
            uvPlanes[i].channel = uvChannels[i].channel;
            uvPlanes[i].nComps = (int)uvImages[i]->getComponentsCount();
            uvPlanes[i].bounds = uvImages[i]->getBounds();
            uvPlanes[i].access = std::make_shared<Image::ReadAccess>(uvImages[i].get());
        }
    }

    const double uOffset = settings.uOffset;
    const double vOffset = settings.vOffset;
    const double uScale = settings.uScale;
    const double vScale = settings.vScale;
    const double domainX1 = domain.x1;
    const double domainY1 = domain.y1;
    const double domainWidth = domain.width();
    const double domainHeight = domain.height();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (hasDomain && resolveInputPlaneForRender(kSTMapInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            src = getImage(kSTMapInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (src && !PassExec::isFloatImage(src)) {
            return eStatusFailed;
        }

        // The whole Source in the output plane's layout, locked here like the UV planes.
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::vector<float> converted;
        Resampler::SourceImage source((const float*)0, RectI(), nComps);
        if (src) {
            srcBounds = src->getBounds();
            const int srcNComps = (int)src->getComponentsCount();
            srcAccess = std::make_shared<Image::ReadAccess>(src.get());
            if (srcBounds.isNull()) {
                src.reset();
            } else if (srcNComps == nComps) {
                source = Resampler::SourceImage((const float*)srcAccess->pixelAt(srcBounds.x1, srcBounds.y1), srcBounds, nComps);
            } else {
                const int width = srcBounds.width();
                converted.resize((std::size_t)width * srcBounds.height() * nComps);
                for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
                    PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, srcBounds.x1, y, width, nComps, &converted[(std::size_t)(y - srcBounds.y1) * width * nComps]);
                }
                source = Resampler::SourceImage(&converted[0], srcBounds, nComps);
            }
        }
        const bool sampled = (bool)src;

        const auto row = [&](const RowIO& io) {
            const int n = io.width + 2;
            std::vector<float> out((std::size_t)io.width * io.nComps, 0.f);
            if (sampled) {
                // Rows y - 1, y and y + 1 of pixel coordinates, one pixel wider than the window
                // on each side so that the Jacobians do not depend on where the window ends.
                std::vector<float> uRow(n);
                std::vector<float> vRow(n);
                std::vector<float> sx((std::size_t)3 * n);
                std::vector<float> sy((std::size_t)3 * n);
                for (int r = 0; r < 3; ++r) {
                    if (!jacobians && (r != 1)) {
                        continue;
                    }
                    const int y = io.y - 1 + r;
                    uvPlanes[0].readRow(io.x0 - 1, y, n, &uRow[0]);
                    uvPlanes[1].readRow(io.x0 - 1, y, n, &vRow[0]);
                    float* sxRow = &sx[(std::size_t)r * n];
                    float* syRow = &sy[(std::size_t)r * n];
                    for (int i = 0; i < n; ++i) {
                        const double u = (uRow[i] - uOffset) * uScale;
                        const double v = (vRow[i] - vOffset) * vScale;
                        sxRow[i] = (float)(domainX1 + u * domainWidth);
                        syRow[i] = (float)(domainY1 + v * domainHeight);
                    }
                }
                std::vector<float> jacobian;
                if (jacobians) {
                    jacobian.resize((std::size_t)4 * n);
                    Resampler::coordJacobiansWithinBounds(&sx[0], &sx[n], &sx[2 * n], &sy[0], &sy[n], &sy[2 * n], io.x0 - 1, io.y, n,
                                                          uvPlanes[0].access ? uvPlanes[0].bounds : RectI(), uvPlanes[1].access ? uvPlanes[1].bounds : RectI(), 0., 0., &jacobian[0]);
                }
                Resampler::resampleRowFromCoords(source, &sx[n + 1], &sy[n + 1], jacobians ? &jacobian[4] : 0, io.width,
                                                 settings.filter, settings.clamp, settings.blackOutside, settings.wrapU, settings.wrapV,
                                                 &out[0], io.nComps, &domain);
            }
            for (int c = 0; c < io.nComps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < io.width; ++i) {
                    io.dst[(std::size_t)i * io.nComps + c] = out[(std::size_t)i * io.nComps + c];
                }
            }
        };

        EffectInstance::RenderActionArgs planeArgs = args;
        planeArgs.outputLayers.clear();
        planeArgs.outputLayers.push_back(*it);
        const StatusEnum status = runStagesCpu(*this, plan, planeArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }

    return eStatusOK;
} // STMap::renderOp

bool
STMap::resolveGpuUVLayer(double time,
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
    const_cast<STMap*>(this)->resolveUVChannels(time, view, &u, &v);
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
STMap::canRenderOpOnGpu(const GpuQuery& query) const
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
STMap::getOpGpuInputLayer(int inputNb,
                          const ImageLayerDesc& plane,
                          double time,
                          ViewIdx view,
                          ImageLayerDesc* layer) const
{
    if (inputNb == kSTMapInputUV) {
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
STMap::getOpGpuInputRoI(int inputNb,
                        const RectI& outputRect,
                        const RenderScale& scale) const
{
    if ((inputNb == kSTMapInputSource) || (inputNb == kSTMapInputUV)) {
        return warpGpuInputRoI(*this, inputNb, kSTMapInputSource, kSTMapInputUV, outputRect, scale);
    }

    return StagedImageEffect::getOpGpuInputRoI(inputNb, outputRect, scale);
}

StatusEnum
STMap::renderOpGpu(const GpuRenderArgs& args)
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
    RectI domain;
    const bool hasDomain = getSourceDomain(query.time, query.view, query.mappedScale, &domain);

    WarpGpuParams params;
    params.filter = (uint32_t)settings.filter;
    params.flags = (settings.clamp ? kWarpGpuClamp : 0u) | (settings.blackOutside ? kWarpGpuBlackOutside : 0u) | ((settings.filter != Resampler::eFilterImpulse) ? kWarpGpuJacobian : 0u) | (hasDomain ? kWarpGpuSample : 0u);
    params.wrapU = (uint32_t)settings.wrapU;
    params.wrapV = (uint32_t)settings.wrapV;
    if (hasDomain) {
        params.domain[0] = domain.x1;
        params.domain[1] = domain.y1;
        params.domain[2] = domain.x2;
        params.domain[3] = domain.y2;
    }
    setWarpGpuDouble(settings.uOffset, params.uOffset);
    setWarpGpuDouble(settings.vOffset, params.vOffset);
    setWarpGpuDouble(settings.uScale, params.uScale);
    setWarpGpuDouble(settings.vScale, params.vScale);
    setWarpGpuDouble(domain.x1, params.originX);
    setWarpGpuDouble(domain.y1, params.originY);
    setWarpGpuDouble(domain.width(), params.width);
    setWarpGpuDouble(domain.height(), params.height);

    return recordWarpGpu(*this, resolveGpuPlan(query), args, (int)gpu::GpuKernelId::stmap, params, kSTMapInputUV, uvLayer, uChannel, vChannel);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
