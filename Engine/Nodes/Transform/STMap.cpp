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
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Transform/WarpGpu.h"
#include "Engine/OverlayMapFetch.h"
#include "Engine/TimeLine.h"

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

// One channel of the UV input, locked on the calling thread. With no image it reads outside;
// outside its bounds it reads outside under EdgeMode::Black and its nearest pixel under Stretch.
struct UVPlane {
    ImagePtr image;
    int channel = -1;
    int nComps = 0;
    RectI bounds;
    std::shared_ptr<Image::ReadAccess> access;
    float outside = 0.f;
    EdgeMode edgeMode = EdgeMode::Black;

    void readRow(int x0,
                 int y,
                 int width,
                 float* row) const
    {
        if (!access) {
            std::fill(row, row + width, outside);

            return;
        }
        if ((edgeMode == EdgeMode::Stretch) && !bounds.isNull()) {
            PassExec::readChannelRow(image.get(), access.get(), bounds, nComps, channel, x0, y, width, EdgeMode::Stretch, row);

            return;
        }
        PassExec::readChannelRow(image.get(), access.get(), bounds, nComps, channel, x0, y, width, outside, row);
    }
};

// The bounds coordJacobiansWithinBounds() keeps differences inside: none for a Black UV input,
// whose 0 past its bounds is a value like any other.
RectI
uvEdgeBounds(const UVPlane& plane)
{
    return (plane.access && (plane.edgeMode == EdgeMode::Stretch)) ? plane.bounds : RectI();
}

// One source-pixel coordinate folded into [lo, hi) the way the render's resampler does it; a
// coordinate left non-finite is outside the source.
double
wrapSourceCoord(double s,
                Resampler::WrapMode mode,
                double lo,
                double hi)
{
    const double w = hi - lo;
    if (!(w > 0.)) {
        return s;
    }
    switch (mode) {
    case Resampler::eWrapClamp:
        return std::min(std::max(s, lo + 0.5), hi - 0.5);
    case Resampler::eWrapRepeat:
        return std::isfinite(s) ? s - w * std::floor((s - lo) / w) : s;
    case Resampler::eWrapMirror:
        if (std::isfinite(s)) {
            const double r = (s - lo) - 2. * w * std::floor((s - lo) / (2. * w));

            return lo + ((r <= w) ? r : (2. * w - r));
        }

        return s;
    case Resampler::eWrapNone:
        break;
    }

    return s;
}
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
    case kSTMapInputMap:
        return tr("How far each output pixel moves towards its UV position: not at all where the Map is black, all the "
                  "way where it is white. Read at the output pixel. With no Map connected, every pixel goes all the way.")
            .toStdString();
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

    KnobBoolPtr uvAlpha = createKnob<KnobBool>(tr("UV Alpha"));
    uvAlpha->setName(kSTMapParamUVAlpha);
    uvAlpha->setDefaultValue(true);
    uvAlpha->setHintToolTip(tr("When checked, the alpha of the layer U Channel reads from marks where the map holds a "
                               "position: output pixels where it is 0 or less are black. A layer without alpha counts as "
                               "opaque. Outside the UV input's region of definition the alpha counts as opaque when the "
                               "UV input is black outside, and holds its nearest edge pixel when it stretches."));
    page->addKnob(uvAlpha);
    _uvAlpha = uvAlpha;

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

    _filterKnobs.create(this, page, filter, clamp);

    _map.createKnobs(this, page, kMapWarpKnobHints);

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(channelU, kSTMapInputUV, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(channelV, kSTMapInputUV, LayerKnobSpec::eRoleInputBound);
    }
    _map.declareLayerKnob(node, kSTMapInputMap);

    addStageKnobs(page);

    _filterKnobs.updateVisibility(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
} // STMap::initializeKnobs

void
STMap::onKnobsLoaded()
{
    _filterKnobs.updateVisibility(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

bool
STMap::knobChanged(KnobI* k,
                   ValueChangedReasonEnum /*reason*/,
                   ViewSpec view,
                   double time,
                   bool /*originatedFromMainThread*/)
{
    KnobChoicePtr filter = _filter.lock();
    if (filter && (k == filter.get())) {
        _filterKnobs.updateVisibility(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }

    return false;
}

void
STMap::readSettings(double time,
                    ViewIdx view,
                    Settings* settings) const
{
    settings->filter = _filterKnobs.read(time, view);
    KnobBoolPtr clamp = _clamp.lock();
    if (clamp) {
        settings->clamp = clamp->getValueAtTime(time, 0, view);
    }
    settings->edgeMode = resolvedEdgeMode(time, view);
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
    KnobBoolPtr uvAlpha = _uvAlpha.lock();
    if (uvAlpha) {
        settings->uvAlpha = uvAlpha->getValueAtTime(time, 0, view);
    }
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

STMap::UVChannel
STMap::uvAlphaChannel(const UVChannel& u,
                      const UVChannel& v)
{
    const UVChannel& used = u.valid ? u : v;
    UVChannel alpha;
    if (!used.valid || !used.layer.isColorLayer() || !ImageLayerDesc::colorStorageBits(used.layer).test(3)) {
        return alpha;
    }
    alpha.channel = ImageLayerDesc::colorViewChannelIndex(ImageLayerDesc::colorViewForNComps(used.layer.getNumComponents()).getLayerID(), 3);
    alpha.layer = used.layer;
    alpha.valid = (alpha.channel >= 0) && (alpha.channel < used.layer.getNumComponents());

    return alpha;
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
    const NodeMetadataConstPtr sourceMetadata = source->getRenderMetadata(time, view);
    const RectI format = sourceMetadata->getOutputFormat();
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
    *domain = rod.toPixelEnclosing(scale, sourceMetadata->getPixelAspectRatio(-1));

    return !domain->isNull();
}

namespace {
U64
mixHash(U64 h,
        U64 v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);

    return h;
}

U64
mixHash(U64 h,
        double v)
{
    U64 bits = 0;

    std::memcpy(&bits, &v, sizeof(bits));

    return mixHash(h, bits);
}
} // anon namespace

OverlayPointMap::Result
STMap::mapOverlayPoint(double time,
                       ViewIdx view,
                       int inputNb,
                       OverlayPointMap::Direction direction,
                       const Point& in,
                       Point* out)
{
    if ((inputNb != kSTMapInputSource) || !getInput(kSTMapInputSource) || !getInput(kSTMapInputUV) || (getMixValue(time, view) == 0.)) {
        return OverlayPointMap::eIdentity;
    }
    RectI domain;
    if (!getSourceDomain(time, view, RenderScale::identity, &domain)) {
        return OverlayPointMap::eUnmappable;
    }
    Settings settings;
    readSettings(time, view, &settings);
    UVChannel uvChannels[2];
    resolveUVChannels(time, view, &uvChannels[0], &uvChannels[1]);
    std::vector<OverlayMapFetch::Channel> channels;
    int uIndex = -1;
    int vIndex = -1;
    for (int i = 0; i < 2; ++i) {
        if (!uvChannels[i].valid) {
            continue;
        }
        OverlayMapFetch::Channel c;
        c.layer = uvChannels[i].layer;
        c.index = uvChannels[i].channel;
        (i == 0 ? uIndex : vIndex) = (int)channels.size();
        channels.push_back(c);
    }
    // UV Alpha gates on the alpha of the layer U (else V) reads from, as the render does.
    int alphaIndex = -1;
    if (settings.uvAlpha && !channels.empty()) {
        const UVChannel alpha = uvAlphaChannel(uvChannels[0], uvChannels[1]);
        if (alpha.valid) {
            OverlayMapFetch::Channel c;
            c.layer = alpha.layer;
            c.index = alpha.channel;
            alphaIndex = (int)channels.size();
            channels.push_back(c);
        }
    }
    const EffectInstancePtr self = shared_from_this();
    OverlayPointMap::Result fetched = OverlayPointMap::eMapped;
    OverlayMapFetch::MapImageConstPtr uv;
    if (!channels.empty()) {
        uv = OverlayMapFetch::fetch(self, kSTMapInputUV, time, view, channels, &fetched);
        if (!uv) {
            return fetched;
        }
    }
    const NodeMetadataConstPtr metadata = getMetadata(time, view);
    const double outputPar = metadata ? metadata->getPixelAspectRatio(-1) : 1.;
    const double sourcePar = metadata ? metadata->getPixelAspectRatio(kSTMapInputSource) : 1.;
    const bool mapped = (bool)getInput(kSTMapInputMap);
    const MapOverlaySampler mapSampler = _map.overlaySampler(self, kSTMapInputMap, time, view);
    if (mapped && (mapSampler.status() == OverlayPointMap::eUnmappable)) {
        return OverlayPointMap::eUnmappable;
    }
    // A pending Map leaves the point unmoved until its copy arrives.
    const bool mapPending = mapped && (mapSampler.status() == OverlayPointMap::ePending);
    const OverlayPointMap::PointFn toSource = [&](const Point& p, Point* s) {
        float values[3] = { 0.f, 0.f, 1.f };
        if (uv && !uv->sample(p.x, p.y, values)) {
            return OverlayPointMap::eUnmappable;
        }
        float alpha = 1.f;
        if (uv && (alphaIndex >= 0)) {
            // The render gates per pixel, so the alpha is that of the pixel holding p.
            const double scale = (double)(1u << uv->mipmapLevel);
            const int ax = std::min(uv->bounds.x2 - 1, std::max(uv->bounds.x1, (int)std::floor(p.x / (scale * uv->par))));
            const int ay = std::min(uv->bounds.y2 - 1, std::max(uv->bounds.y1, (int)std::floor(p.y / scale)));
            const float* pix = uv->pixel(ax, ay);
            if (!pix) {
                return OverlayPointMap::eUnmappable;
            }
            alpha = pix[alphaIndex];
        }
        const double uRaw = (uIndex >= 0) ? values[uIndex] : 0.;
        const double vRaw = (vIndex >= 0) ? values[vIndex] : 0.;
        const double u = (uRaw - settings.uOffset) * settings.uScale;
        const double v = (vRaw - settings.vOffset) * settings.vScale;
        double px = domain.x1 + u * domain.width();
        double py = domain.y1 + v * domain.height();
        bool pinned = false;
        bool gated = true;
        if (mapped) {
            const double m = mapSampler.value(p);
            const double cx = p.x / outputPar;
            const double cy = p.y;
            px = cx + (px - cx) * m;
            py = cy + (py - cy) * m;
            // A Map of 0 or less is not gated by UV Alpha, and where it is 0 the pixel is the Source
            // pixel itself, never wrapped.
            pinned = (m == 0.);
            gated = (m > 0.);
        }
        if (gated && !(alpha > 0.f)) {
            return OverlayPointMap::eUnmappable;
        }
        if (!pinned) {
            px = wrapSourceCoord(px, settings.wrapU, domain.x1, domain.x2);
            py = wrapSourceCoord(py, settings.wrapV, domain.y1, domain.y2);
        }
        s->x = px * sourcePar;
        s->y = py;
        if (!std::isfinite(s->x) || !std::isfinite(s->y)) {
            return OverlayPointMap::eUnmappable;
        }

        return OverlayPointMap::eMapped;
    };
    OverlayPointMap::Result result = OverlayPointMap::eUnmappable;
    if (direction == OverlayPointMap::eDirectionInverse) {
        result = toSource(in, out);
    } else if (uv) {
        U64 key = mixHash((U64)0, (U64)(std::uintptr_t)this);
        for (const double v : { settings.uOffset, settings.vOffset, settings.uScale, settings.vScale, outputPar, sourcePar,
                                (double)domain.x1, (double)domain.y1, (double)domain.x2, (double)domain.y2 }) {
            key = mixHash(key, v);
        }
        key = mixHash(key, (U64)settings.wrapU);
        key = mixHash(key, (U64)settings.wrapV);
        key = mixHash(key, (U64)(alphaIndex >= 0 ? 1 : 0));
        key = mixHash(key, (U64)(mapped ? getInput(kSTMapInputMap)->getHash() : 0));
        // The node's own hash covers its Map knobs (invert, response, gamma).
        key = mixHash(key, getHash());
        const RectD sourceRegion(domain.x1 * sourcePar, domain.y1, domain.x2 * sourcePar, domain.y2);
        const std::function<std::shared_ptr<const OverlayPointMap::SeedGrid>()> buildSeeds = [&]() {
            std::shared_ptr<OverlayPointMap::SeedGrid> grid = std::make_shared<OverlayPointMap::SeedGrid>(sourceRegion, uv->bounds.width(), uv->bounds.height());
            for (int y = uv->bounds.y1; y < uv->bounds.y2; ++y) {
                for (int x = uv->bounds.x1; x < uv->bounds.x2; ++x) {
                    const Point c = uv->pixelCentre(x, y);
                    Point s;
                    if (toSource(c, &s) == OverlayPointMap::eMapped) {
                        grid->add(s, c);
                    }
                }
            }
            grid->fillHoles();

            return std::shared_ptr<const OverlayPointMap::SeedGrid>(grid);
        };
        // A grid scattered with the Map read as 1 is wrong once the Map arrives, so it is not kept.
        const std::shared_ptr<const OverlayPointMap::SeedGrid> seeds = mapPending ? buildSeeds() : uv->seedGrid(key, buildSeeds);
        Point seed = *out;
        if (seeds && !seeds->lookup(in, &seed)) {
            return mapPending ? OverlayPointMap::ePending : OverlayPointMap::eUnmappable;
        }
        OverlayPointMap::InvertOptions options;
        // The UV copy holds floats, which bounds how closely the polish can land.
        options.tolerance = 1e-4;
        result = OverlayPointMap::invertNumerically(toSource, in, seed, out, options);
    } else {
        return OverlayPointMap::eUnmappable;
    }

    return mapPending ? OverlayPointMap::ePending : result;
} // STMap::mapOverlayPoint

OverlayPointMap::Kind
STMap::overlayMapKind(double time,
                      ViewIdx view)
{
    return (getInput(kSTMapInputSource) && getInput(kSTMapInputUV) && (getMixValue(time, view) != 0.)) ? OverlayPointMap::eKindNonLinear : OverlayPointMap::eKindNone;
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
STMap::getPreferredMetadataAt(double time,
                              ViewIdx view,
                              NodeMetadata& metadata)
{
    if (route(time, view).kind == Route::Kind::Black) {
        return StagedImageEffect::getPreferredMetadataAt(time, view, metadata);
    }
    // The output planes are the Source's, whatever the UV input carries.
    if (getInput(kSTMapInputSource)) {
        metadata.setNComps(-1, metadata.getNComps(kSTMapInputSource));
    }
    const NodeMetadataConstPtr uvMetadata = getInputMetadataAt(kSTMapInputUV, time, view);
    if (uvMetadata) {
        const RectI format = uvMetadata->getOutputFormat();
        if (!format.isNull()) {
            metadata.setOutputFormat(format);
        }
    }
    // The defaults take the UV input's edge mode, which gives the format.
    metadata.setEdgeMode(resolvedEdgeMode(time, view));

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
    const NodeMetadataConstPtr uvMetadata = uv->getRenderMetadata(time, view);
    const RectI format = uvMetadata->getOutputFormat();
    if (!fromRoD && !format.isNull()) {
        const double par = uvMetadata->getPixelAspectRatio(-1);
        *rod = RectD(format.x1 * par, format.y1, format.x2 * par, format.y2);
    } else if (!inputRegion(uv, time, view, scale, rod)) {
        return eStatusFailed;
    }

    // Under a mask or a partial mix the Source shows through outside the warped region, and with
    // a Map wherever the Map holds a pixel in place.
    if (isMaskApplied() || (getMixValue(time, view) != 1.) || getInput(kSTMapInputMap)) {
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
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
        const double dx = par / scale.toOfxPointD().x;
        const double dy = 1. / scale.toOfxPointD().y;
        RectD uvRoI(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 + dx, renderWindow.y2 + dy);
        if (uv == source) {
            uvRoI.merge(sourceRoI);
        }
        (*ret)[uv] = uvRoI;
    }

    // The Map too, for the Jacobians.
    EffectInstancePtr map = getInput(kSTMapInputMap);
    if (map) {
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
        const double dx = par / scale.toOfxPointD().x;
        const double dy = 1. / scale.toOfxPointD().y;
        const RectD mapRoI(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 + dx, renderWindow.y2 + dy);
        RoIMap::iterator found = ret->find(map);
        if (found == ret->end()) {
            ret->insert(std::make_pair(map, mapRoI));
        } else {
            found->second.merge(mapRoI);
        }
    }
} // STMap::getRegionsOfInterest

bool
STMap::getInputLayersRead(int inputNb,
                          double time,
                          ViewIdx view,
                          std::list<ImageLayerDesc>* layers)
{
    if (inputNb == kSTMapInputMap) {
        return _map.getInputLayersRead(this, kSTMapInputMap, time, view, layers);
    }
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
    const bool jacobians = (settings.filter.filter != Resampler::eFilterImpulse);

    RectI domain;
    const bool hasDomain = getSourceDomain(args.time, args.view, args.mappedScale, &domain);

    // With warps concatenated into the Source, the image fetched is upstream of them: each
    // coordinate is wrapped in this node's Source as ever, then taken on through them.
    const NodeMetadataConstPtr metadata = getRenderMetadata(args.time, args.view);
    const double par = metadata ? metadata->getPixelAspectRatio(-1) : 1.;
    const InverseWarpChainConstPtr chain = hasDomain ? getPreparedWarpChain(kSTMapInputSource, args.time, args.view, args.mappedScale, PassExec::toCanonical(args.roi, args.mappedScale, par)) : InverseWarpChainConstPtr();
    if (chain && chain->hasError()) {
        return eStatusFailed;
    }

    // The Map, copied out over the window grown by one pixel, for the Jacobians, before any image
    // is locked: fetching renders upstream.
    const bool mapped = hasDomain && getInput(kSTMapInputMap);
    MapPlane mapPlane;
    if (mapped) {
        const MapInputParams mapParams = [&]() {
            const KnobReader reader(args.time, args.view);

            return _map.bake(reader);
        }();
        const RectI window(args.roi.x1 - 1, args.roi.y1 - 1, args.roi.x2 + 1, args.roi.y2 + 1);
        const StatusEnum status = _map.read(this, kSTMapInputMap, mapParams, args.time, args.view, args.mappedScale, window, &mapPlane);
        if (status != eStatusOK) {
            return status;
        }
    }

    // U and V, fetched and locked here, on the calling thread; the band threads only read
    // through these.
    UVPlane uvPlanes[2];
    // Off the map, as with no map at all, every position is valid, so an opaque map renders the
    // same with UV Alpha on or off.
    UVPlane alphaPlane;
    bool gated = false;
    if (hasDomain) {
        UVChannel uvChannels[2];
        resolveUVChannels(args.time, args.view, &uvChannels[0], &uvChannels[1]);
        ImagePtr uvImages[2];
        const EdgeMode uvEdgeMode = stageInputEdgeMode(*this, kSTMapInputUV, args.time, args.view);
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
            uvPlanes[i].edgeMode = uvEdgeMode;
            uvPlanes[i].access = std::make_shared<Image::ReadAccess>(uvImages[i].get());
        }
        if (settings.uvAlpha) {
            const UVChannel alpha = uvAlphaChannel(uvChannels[0], uvChannels[1]);
            const UVPlane& holder = uvChannels[0].valid ? uvPlanes[0] : uvPlanes[1];
            if (alpha.valid && holder.access && (alpha.channel < holder.nComps)) {
                alphaPlane = holder;
                alphaPlane.channel = alpha.channel;
                gated = true;
            }
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
        const bool chained = sampled && chain;

        // n coordinates (sx, sy) of this node's Source, with Jacobians J (null for the identity),
        // wrapped unless wrap is false, through the chain and sampled into out.
        const auto sampleThroughChain = [&](const float* sx, const float* sy, const float* J, int n, bool wrap, float* out) {
            std::vector<double> chainX(n), chainY(n), chainJ(jacobians ? (std::size_t)n * 4 : 0);
            for (int i = 0; i < n; ++i) {
                chainX[i] = sx[i];
                chainY[i] = sy[i];
            }
            for (std::size_t k = 0; k < chainJ.size(); ++k) {
                chainJ[k] = J ? (double)J[k] : (((k % 4) == 0) || ((k % 4) == 3) ? 1. : 0.);
            }
            double* const cj = chainJ.empty() ? NULL : chainJ.data();
            if (wrap) {
                Resampler::wrapRow(chainX.data(), chainY.data(), cj, n, settings.wrapU, settings.wrapV, domain);
            }
            chain->mapRow(n, chainX.data(), chainY.data(), cj);
            std::vector<float> fx(n), fy(n), fj(chainJ.size());
            for (int i = 0; i < n; ++i) {
                fx[i] = (float)chainX[i];
                fy[i] = (float)chainY[i];
            }
            for (std::size_t k = 0; k < chainJ.size(); ++k) {
                fj[k] = (float)chainJ[k];
            }
            Resampler::resampleRowFromCoords(source, fx.data(), fy.data(), fj.empty() ? NULL : fj.data(), n, settings.filter, settings.clamp, settings.edgeMode,
                                             Resampler::eWrapNone, Resampler::eWrapNone, out, nComps);
        };

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
                        const double u = productOrInfinity(uRow[i] - uOffset, uScale);
                        const double v = productOrInfinity(vRow[i] - vOffset, vScale);
                        double px = domainX1 + productOrInfinity(u, domainWidth);
                        double py = domainY1 + productOrInfinity(v, domainHeight);
                        if (mapped) {
                            // The Map moves the pixel from its own centre c to c + (p - c) * m.
                            const float m = mapPlane.at(io.x0 - 1 + i, y);
                            const double cx = (double)(io.x0 - 1 + i) + 0.5;
                            const double cy = (double)y + 0.5;
                            if (m == 0.f) {
                                px = cx;
                                py = cy;
                            } else if (m != 1.f) {
                                px = cx + productOrInfinity(px - cx, (double)m);
                                py = cy + productOrInfinity(py - cy, (double)m);
                            }
                        }
                        sxRow[i] = knobValueAsFloat(px);
                        syRow[i] = knobValueAsFloat(py);
                    }
                }
                std::vector<float> jacobian;
                if (jacobians) {
                    jacobian.resize((std::size_t)4 * n);
                    Resampler::coordJacobiansWithinBounds(&sx[0], &sx[n], &sx[2 * n], &sy[0], &sy[n], &sy[2 * n], io.x0 - 1, io.y, n,
                                                          uvEdgeBounds(uvPlanes[0]), uvEdgeBounds(uvPlanes[1]), 0., 0., &jacobian[0]);
                }
                if (chained) {
                    sampleThroughChain(&sx[n + 1], &sy[n + 1], jacobians ? &jacobian[4] : 0, io.width, true, &out[0]);
                } else {
                    Resampler::resampleRowFromCoords(source, &sx[n + 1], &sy[n + 1], jacobians ? &jacobian[4] : 0, io.width,
                                                     settings.filter, settings.clamp, settings.edgeMode, settings.wrapU, settings.wrapV,
                                                     &out[0], io.nComps, &domain);
                }
                if (gated) {
                    std::vector<float> alphaRow(io.width);
                    alphaPlane.readRow(io.x0, io.y, io.width, &alphaRow[0]);
                    for (int i = 0; i < io.width; ++i) {
                        // Where the Map is 0 the pixel keeps its own place, so the gate does not apply.
                        if (!(alphaRow[i] > 0.f) && (!mapped || (mapPlane.at(io.x0 + i, io.y) > 0.f))) {
                            std::fill(&out[(std::size_t)i * io.nComps], &out[(std::size_t)(i + 1) * io.nComps], 0.f);
                        }
                    }
                }
                if (mapped) {
                    // Where the Map is 0 the output is the Source pixel itself, never resampled
                    // through the wrap, which would move positions outside the Source format.
                    std::vector<float> held;
                    if (chained) {
                        std::vector<float> cx(io.width), cy(io.width, (float)io.y + 0.5f);
                        for (int i = 0; i < io.width; ++i) {
                            cx[i] = (float)(io.x0 + i) + 0.5f;
                        }
                        held.resize((std::size_t)io.width * io.nComps);
                        sampleThroughChain(cx.data(), cy.data(), NULL, io.width, false, held.data());
                    }
                    for (int i = 0; i < io.width; ++i) {
                        if (mapPlane.at(io.x0 + i, io.y) != 0.f) {
                            continue;
                        }
                        float* dst = &out[(std::size_t)i * io.nComps];
                        const float* pix = chained ? &held[(std::size_t)i * io.nComps] : source.pixel(io.x0 + i, io.y);
                        if (pix) {
                            std::copy(pix, pix + io.nComps, dst);
                        } else {
                            std::fill(dst, dst + io.nComps, 0.f);
                        }
                    }
                }
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
STMap::acceptsInverseWarpChain(int inputNb,
                               double time,
                               ViewIdx view) const
{
    if (inputNb != kSTMapInputSource) {
        return false;
    }
    const ResampleRegionStages stages = getResampleRegionStages(time, view);

    return !stages.doMasking && (stages.mix == 1.) && !stages.readsUnprocessed;
}

double
STMap::getInverseWarpChainFilterSupport(int /*inputNb*/,
                                        double time,
                                        ViewIdx view) const
{
    return Resampler::filterRoIExpansion(_filterKnobs.read(time, view)) + 0.5;
}

std::optional<EdgeMode>
STMap::getConcatenationEdgeMode(double time,
                                ViewIdx view) const
{
    return resolvedEdgeMode(time, view);
}

bool
STMap::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasConcatenatedInput || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    ImageLayerDesc layer;
    int uChannel = -1;
    int vChannel = -1;
    ImageLayerDesc mapLayer;
    int mapChannel = -1;

    return resolveGpuUVLayer(query.time, query.view, &layer, &uChannel, &vChannel) && resolveWarpGpuMapChannel(*this, _map, kSTMapInputMap, query.time, query.view, &mapLayer, &mapChannel);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
STMap::readsGpuInput(int inputNb,
                     const ImageLayerDesc& /*plane*/,
                     double time,
                     ViewIdx view) const
{
    if (inputNb != kSTMapInputMap) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return resolveWarpGpuMapChannel(*this, _map, kSTMapInputMap, time, view, &layer, &channel) && (channel >= 0);
}

bool
STMap::getOpGpuInputLayer(int inputNb,
                          const ImageLayerDesc& plane,
                          double time,
                          ViewIdx view,
                          ImageLayerDesc* layer) const
{
    if (inputNb == kSTMapInputMap) {
        int channel = -1;
        if (resolveWarpGpuMapChannel(*this, _map, kSTMapInputMap, time, view, layer, &channel) && (layer->getNumComponents() > 0)) {
            return true;
        }
    }
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
                        double time,
                        ViewIdx view,
                        const RectI& outputRect,
                        const RenderScale& scale) const
{
    if ((inputNb == kSTMapInputSource) || (inputNb == kSTMapInputUV)) {
        return warpGpuInputRoI(*this, inputNb, kSTMapInputSource, kSTMapInputUV, time, view, outputRect, scale);
    }
    if (inputNb == kSTMapInputMap) {
        return warpGpuInputRoI(*this, inputNb, kSTMapInputSource, kSTMapInputMap, time, view, outputRect, scale);
    }

    return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
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
    const Resampler::FilterSpec spec(settings.filter);
    params.filter = (uint32_t)spec.filter;
    params.lobes = spec.lobes;
    params.bicubicB = (float)spec.B;
    params.bicubicC = (float)spec.C;
    params.flags = ((settings.clamp && Resampler::filterHonoursClamp(settings.filter.filter)) ? kWarpGpuClamp : 0u) | (settings.edgeMode == EdgeMode::Black ? kWarpGpuEdgeBlack : 0u) | ((settings.filter.filter != Resampler::eFilterImpulse) ? kWarpGpuJacobian : 0u) | (hasDomain ? kWarpGpuSample : 0u);
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

    int alphaChannel = -1;
    if (settings.uvAlpha) {
        UVChannel u;
        UVChannel v;
        resolveUVChannels(query.time, query.view, &u, &v);
        const UVChannel alpha = uvAlphaChannel(u, v);
        if (alpha.valid && (alpha.layer == uvLayer)) {
            alphaChannel = alpha.channel;
        }
    }

    WarpGpuMap map;
    if (hasDomain) {
        map.input = kSTMapInputMap;
        if (!resolveWarpGpuMapChannel(*this, _map, kSTMapInputMap, query.time, query.view, &map.layer, &map.channel)) {
            return eStatusFailed;
        }
        map.params = _map.bake(KnobReader(query.time, query.view));
    }

    return recordWarpGpu(*this, resolveGpuPlan(query), args, (int)gpu::GpuKernelId::stmap, params, kSTMapInputUV, uvLayer, uChannel, vChannel, map, alphaChannel);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
