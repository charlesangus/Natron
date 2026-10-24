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

#include "ChannelCopy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

#include "Engine/EffectInstance.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/NodeMetadata.h"
#include "Engine/RectI.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

// Mirrors ResolvedLayer::channelBit: a single-channel colour plane is alpha, on bit 3.
int
colorBitOfIndex(int nComps,
                int index)
{
    return (nComps == 1) ? 3 : index;
}

int
colorChannelOnBit(const ImageLayerDesc& layout,
                  int nAvailable,
                  int bit)
{
    const int nComps = layout.getNumComponents();

    for (int i = 0; (i < nComps) && (i < nAvailable) && (i < 4); ++i) {
        if (colorBitOfIndex(nComps, i) == bit) {
            return i;
        }
    }

    return -1;
}

template <typename SRCPIX, typename DSTPIX>
void
copyRow(const Image::ReadAccess* src,
        Image::WriteAccess* dst,
        int y,
        int x1,
        int x2,
        int dstComps,
        const std::vector<int>& srcChannels)
{
    for (int x = x1; x < x2; ++x) {
        DSTPIX* out = (DSTPIX*)dst->pixelAt(x, y);
        if (!out) {
            continue;
        }
        const SRCPIX* in = src ? (const SRCPIX*)src->pixelAt(x, y) : NULL;
        for (int c = 0; c < dstComps; ++c) {
            const int s = (c < (int)srcChannels.size()) ? srcChannels[c] : -1;
            out[c] = (in && (s >= 0)) ? Image::convertPixelDepth<SRCPIX, DSTPIX>(in[s]) : DSTPIX(0);
        }
    }
}

typedef void (*CopyRowFunc)(const Image::ReadAccess*, Image::WriteAccess*, int, int, int, int, const std::vector<int>&);

template <typename DSTPIX>
CopyRowFunc
selectCopyRowForSource(ImageBitDepthEnum srcDepth)
{
    switch (srcDepth) {
    case eImageBitDepthByte:
        return &copyRow<unsigned char, DSTPIX>;
    case eImageBitDepthShort:
        return &copyRow<unsigned short, DSTPIX>;
    case eImageBitDepthFloat:
        return &copyRow<float, DSTPIX>;
    default:
        return NULL;
    }
}

CopyRowFunc
selectCopyRow(ImageBitDepthEnum srcDepth,
              ImageBitDepthEnum dstDepth)
{
    switch (dstDepth) {
    case eImageBitDepthByte:
        return selectCopyRowForSource<unsigned char>(srcDepth);
    case eImageBitDepthShort:
        return selectCopyRowForSource<unsigned short>(srcDepth);
    case eImageBitDepthFloat:
        return selectCopyRowForSource<float>(srcDepth);
    default:
        return NULL;
    }
}

struct RoutePair {
    int dst;
    int src;
};

template <typename SRCPIX, typename DSTPIX>
void
routeRow(const Image::ReadAccess* src,
         Image::WriteAccess* dst,
         int y,
         int x1,
         int x2,
         const std::vector<RoutePair>& pairs)
{
    for (int x = x1; x < x2; ++x) {
        DSTPIX* out = (DSTPIX*)dst->pixelAt(x, y);
        if (!out) {
            continue;
        }
        const SRCPIX* in = (const SRCPIX*)src->pixelAt(x, y);
        for (const RoutePair& p : pairs) {
            out[p.dst] = in ? Image::convertPixelDepth<SRCPIX, DSTPIX>(in[p.src]) : DSTPIX(0);
        }
    }
}

typedef void (*RouteRowFunc)(const Image::ReadAccess*, Image::WriteAccess*, int, int, int, const std::vector<RoutePair>&);

template <typename DSTPIX>
RouteRowFunc
selectRouteRowForSource(ImageBitDepthEnum srcDepth)
{
    switch (srcDepth) {
    case eImageBitDepthByte:
        return &routeRow<unsigned char, DSTPIX>;
    case eImageBitDepthShort:
        return &routeRow<unsigned short, DSTPIX>;
    case eImageBitDepthFloat:
        return &routeRow<float, DSTPIX>;
    default:
        return NULL;
    }
}

RouteRowFunc
selectRouteRow(ImageBitDepthEnum srcDepth,
               ImageBitDepthEnum dstDepth)
{
    switch (dstDepth) {
    case eImageBitDepthByte:
        return selectRouteRowForSource<unsigned char>(srcDepth);
    case eImageBitDepthShort:
        return selectRouteRowForSource<unsigned short>(srcDepth);
    case eImageBitDepthFloat:
        return selectRouteRowForSource<float>(srcDepth);
    default:
        return NULL;
    }
}

template <typename DSTPIX>
void
constantRow(Image::WriteAccess* dst,
            int y,
            int x1,
            int x2,
            const std::vector<std::pair<int, float>>& constants)
{
    std::vector<DSTPIX> values(constants.size());

    for (std::size_t i = 0; i < constants.size(); ++i) {
        values[i] = Image::convertPixelDepth<float, DSTPIX>(constants[i].second);
    }
    for (int x = x1; x < x2; ++x) {
        DSTPIX* out = (DSTPIX*)dst->pixelAt(x, y);
        if (!out) {
            continue;
        }
        for (std::size_t i = 0; i < constants.size(); ++i) {
            out[constants[i].first] = values[i];
        }
    }
}

typedef void (*ConstantRowFunc)(Image::WriteAccess*, int, int, int, const std::vector<std::pair<int, float>>&);

ConstantRowFunc
selectConstantRow(ImageBitDepthEnum dstDepth)
{
    switch (dstDepth) {
    case eImageBitDepthByte:
        return &constantRow<unsigned char>;
    case eImageBitDepthShort:
        return &constantRow<unsigned short>;
    case eImageBitDepthFloat:
        return &constantRow<float>;
    default:
        return NULL;
    }
}

bool
sameLayer(const ImageLayerDesc& a,
          const ImageLayerDesc& b)
{
    return (a == b) && (a.getChannels() == b.getChannels());
}

// The channel of a source laid out as layout, with nAvailable channels, that component reads, or
// -1 when it has none.
int
routeSourceChannel(const ChannelRouteComponent& component,
                   const ImageLayerDesc& layout,
                   int nAvailable)
{
    if (component.kind == ChannelRouteComponent::eKindColorBit) {
        return colorChannelOnBit(layout, nAvailable, component.index);
    }

    return ((component.index >= 0) && (component.index < nAvailable)) ? component.index : -1;
}

// The distinct layers route reads input inputNb in, in the order it first names them.
std::vector<ImageLayerDesc>
routeLayersOfInput(const ChannelRoute& route,
                   int inputNb)
{
    std::vector<ImageLayerDesc> layers;

    for (const ChannelRouteSource& source : route.sources) {
        if (source.inputNb != inputNb) {
            continue;
        }
        bool known = false;
        for (const ImageLayerDesc& layer : layers) {
            known = known || sameLayer(layer, source.layer);
        }
        if (!known) {
            layers.push_back(source.layer);
        }
    }

    return layers;
}

#ifdef NATRON_HAVE_GPU
// Mirrors ChannelRouteParams in Engine/Gpu/Kernels/channelroute.slang.
struct ChannelRouteGpuParams {
    uint32_t width;
    uint32_t height;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t dstComps;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t srcComps;
    uint32_t validX1;
    uint32_t validY1;
    uint32_t validX2;
    uint32_t validY2;
    uint32_t select[4];
    uint32_t constants[4];
};
static_assert(sizeof(ChannelRouteGpuParams) == 80, "must match ChannelRouteParams in channelroute.slang");
static_assert(offsetof(ChannelRouteGpuParams, select) == 48, "must match ChannelRouteParams in channelroute.slang");

const uint32_t kRouteSkip = 0xFFFFFFFFu;
const uint32_t kRouteConstant = 0xFFFFFFFEu;

// The float index of pixel (x, y) of view, false when the view is malformed or does not hold its
// whole bounds within 32-bit indexing.
bool
routeViewIndex(const GpuImageView& view,
               int x,
               int y,
               uint32_t* index)
{
    if (!view.buffer || ((view.byteOffset % sizeof(float)) != 0) || (view.nComps < 1) || (view.rowStrideFloats < view.bounds.width() * view.nComps)) {
        return false;
    }
    const uint64_t base = view.byteOffset / sizeof(float);
    if ((base + (uint64_t)view.bounds.height() * (uint64_t)view.rowStrideFloats) > UINT32_MAX) {
        return false;
    }
    *index = (uint32_t)(base + (uint64_t)(y - view.bounds.y1) * (uint64_t)view.rowStrideFloats + (uint64_t)(x - view.bounds.x1) * (uint64_t)view.nComps);

    return true;
}

uint32_t
clampedThreads(int64_t v,
               uint32_t n)
{
    return (uint32_t)std::clamp<int64_t>(v, 0, (int64_t)n);
}
#endif // NATRON_HAVE_GPU
} // namespace

const ImageLayerDesc*
ChannelCopy::findColorStorage(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer() && (it->getNumComponents() > 0)) {
            return &(*it);
        }
    }

    return NULL;
}

ImageLayerDesc
ChannelCopy::metadataColorStorage(const EffectInstance& effect,
                                  int inputNb,
                                  double time,
                                  ViewIdx view)
{
    EffectInstancePtr input = effect.getInput(inputNb);
    NodeMetadataConstPtr metadata = input ? input->getMetadata(time, view) : NodeMetadataConstPtr();

    if (!metadata || (metadata->getComponentsType(-1) != kNatronColorLayerID)) {
        return ImageLayerDesc();
    }
    // The stream's own layout, which a colour output may carry narrower than its clip's.
    const int nComps = (metadata->getOutputStorageNComps() > 0) ? metadata->getOutputStorageNComps() : metadata->getNComps(-1);
    if (nComps <= 0) {
        return ImageLayerDesc();
    }

    return ImageLayerDesc::mapNCompsToColorLayer(nComps);
}

std::vector<int>
ChannelCopy::mapColorChannels(const ImageLayerDesc& plane,
                              const Image& source,
                              const std::bitset<4>& copiedBits)
{
    const int nPlaneComps = plane.getNumComponents();
    std::vector<int> srcChannels((std::size_t)nPlaneComps, -1);
    const ImageLayerDesc& sourceLayout = source.getComponents();
    const int nAvailable = (int)source.getComponentsCount();

    for (int c = 0; (c < nPlaneComps) && (c < 4); ++c) {
        const int bit = colorBitOfIndex(nPlaneComps, c);
        if (copiedBits[bit]) {
            srcChannels[c] = colorChannelOnBit(sourceLayout, nAvailable, bit);
        }
    }

    return srcChannels;
}

StatusEnum
ChannelCopy::copyChannels(const Image::ReadAccess* src,
                          ImageBitDepthEnum srcDepth,
                          Image* dst,
                          const RectI& roi,
                          const std::vector<int>& srcChannels,
                          const EffectInstance& effect)
{
    const CopyRowFunc copyRowFunc = selectCopyRow(srcDepth, dst->getBitDepth());

    if (!copyRowFunc) {
        return eStatusFailed;
    }

    // The image can be wider than its plane (a plane with no same-sized supported layout); its
    // extra channels are written as 0.
    const int dstComps = (int)dst->getComponentsCount();
    Image::WriteAccess dstAccess(dst);
    for (int y = roi.y1; y < roi.y2; ++y) {
        if (effect.aborted()) {
            return eStatusOK;
        }
        copyRowFunc(src, &dstAccess, y, roi.x1, roi.x2, dstComps, srcChannels);
    }

    return eStatusOK;
}

bool
ChannelRoute::isValid(int nInputs) const
{
    for (const ChannelRouteSource& source : sources) {
        if ((source.inputNb < 0) || (source.inputNb >= nInputs) || (source.layer.getNumComponents() == 0)) {
            return false;
        }
    }
    for (const ChannelRouteComponent& component : components) {
        if ((component.kind != ChannelRouteComponent::eKindConstant) && ((component.source < 0) || (component.source >= (int)sources.size()))) {
            return false;
        }
    }

    return true;
}

StatusEnum
ChannelCopy::applyChannelRoute(const ChannelRoute& route,
                               const std::vector<ImagePtr>& sourceImages,
                               Image* dst,
                               const RectI& roi,
                               const EffectInstance& effect)
{
    if (sourceImages.size() != route.sources.size()) {
        return eStatusFailed;
    }
    const ConstantRowFunc constantRowFunc = selectConstantRow(dst->getBitDepth());
    if (!constantRowFunc) {
        return eStatusFailed;
    }

    struct Group {
        const Image* image;
        RouteRowFunc rowFunc;
        std::vector<RoutePair> pairs;
    };
    std::vector<Group> groups(route.sources.size());
    for (std::size_t s = 0; s < route.sources.size(); ++s) {
        const Image* image = sourceImages[s].get();
        if (image && (image->getBitDepth() != eImageBitDepthFloat) && (route.sources[s].nonFloat == ChannelRouteSource::eNonFloatReadsZero)) {
            image = NULL;
        }
        groups[s].image = image;
        groups[s].rowFunc = NULL;
        if (image) {
            groups[s].rowFunc = selectRouteRow(image->getBitDepth(), dst->getBitDepth());
            if (!groups[s].rowFunc) {
                return eStatusFailed;
            }
        }
    }

    const int dstComps = (int)dst->getComponentsCount();
    std::vector<std::pair<int, float>> constants;
    for (int c = 0; c < dstComps; ++c) {
        if (c >= (int)route.components.size()) {
            constants.push_back(std::make_pair(c, 0.f));
            continue;
        }
        const ChannelRouteComponent& component = route.components[c];
        if (component.kind == ChannelRouteComponent::eKindConstant) {
            constants.push_back(std::make_pair(c, component.constant));
            continue;
        }
        if ((component.source < 0) || (component.source >= (int)groups.size())) {
            return eStatusFailed;
        }
        Group& group = groups[component.source];
        const int channel = group.image ? routeSourceChannel(component, group.image->getComponents(), (int)group.image->getComponentsCount()) : -1;
        if (channel < 0) {
            constants.push_back(std::make_pair(c, 0.f));
        } else {
            group.pairs.push_back(RoutePair { c, channel });
        }
    }

    // An image read by several sources is locked once.
    std::map<const Image*, Image::ReadAccessPtr> readAccesses;
    std::vector<const Image::ReadAccess*> accesses(groups.size(), NULL);
    for (std::size_t s = 0; s < groups.size(); ++s) {
        if (!groups[s].image || groups[s].pairs.empty()) {
            continue;
        }
        Image::ReadAccessPtr& access = readAccesses[groups[s].image];
        if (!access) {
            access = std::make_shared<Image::ReadAccess>(groups[s].image);
        }
        accesses[s] = access.get();
    }

    Image::WriteAccess dstAccess(dst);
    for (int y = roi.y1; y < roi.y2; ++y) {
        if (effect.aborted()) {
            return eStatusOK;
        }
        for (std::size_t s = 0; s < groups.size(); ++s) {
            if (accesses[s]) {
                groups[s].rowFunc(accesses[s], &dstAccess, y, roi.x1, roi.x2, groups[s].pairs);
            }
        }
        if (!constants.empty()) {
            constantRowFunc(&dstAccess, y, roi.x1, roi.x2, constants);
        }
    }

    return eStatusOK;
} // ChannelCopy::applyChannelRoute

StatusEnum
ChannelCopy::runChannelRouteCpu(EffectInstance& effect,
                                const EffectInstance::RenderActionArgs& args,
                                const std::vector<ChannelRoute>& routes)
{
    if (routes.size() != args.outputLayers.size()) {
        return eStatusFailed;
    }

    struct Fetched {
        int inputNb;
        ImageLayerDesc layer;
        ImagePtr image;
    };
    std::vector<Fetched> fetched;
    std::vector<std::vector<ImagePtr>> sourceImages(routes.size());
    for (std::size_t i = 0; i < routes.size(); ++i) {
        for (const ChannelRouteSource& source : routes[i].sources) {
            std::vector<Fetched>::const_iterator found = fetched.begin();
            while ((found != fetched.end()) && ((found->inputNb != source.inputNb) || !sameLayer(found->layer, source.layer))) {
                ++found;
            }
            if (found == fetched.end()) {
                Fetched f;
                f.inputNb = source.inputNb;
                f.layer = source.layer;
                if (effect.getInput(source.inputNb)) {
                    RectI inputRoI;
                    f.image = effect.getImage(source.inputNb, args.time, args.mappedScale, args.view, NULL, &source.layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
                }
                fetched.push_back(f);
                found = fetched.end() - 1;
            }
            sourceImages[i].push_back(found->image);
        }
    }

    std::size_t i = 0;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it, ++i) {
        if (!it->second) {
            continue;
        }
        const StatusEnum stat = applyChannelRoute(routes[i], sourceImages[i], it->second.get(), args.roi, effect);
        if ((stat != eStatusOK) || effect.aborted()) {
            return stat;
        }
    }

    return eStatusOK;
} // ChannelCopy::runChannelRouteCpu

ChannelRouteEffect::ChannelRouteEffect(NodePtr node)
    : NativeEffectBase(node)
{
}

ChannelRouteEffect::~ChannelRouteEffect()
{
}

StatusEnum
ChannelRouteEffect::render(const RenderActionArgs& args)
{
    std::vector<ChannelRoute> routes;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        ChannelRoute route;
        if (!channelRoute(args.time, args.view, it->first, &route) || !route.isValid(getNInputs())) {
            return eStatusFailed;
        }
        routes.push_back(route);
    }

    return ChannelCopy::runChannelRouteCpu(*this, args, routes);
}

bool
ChannelRouteEffect::resolveGpuRoute(double time,
                                    ViewIdx view,
                                    const ImageLayerDesc& plane,
                                    ChannelRoute* route) const
{
    // Describing a route only reads knobs and the inputs' layers.
    ChannelRouteEffect* self = const_cast<ChannelRouteEffect*>(this);

    return self->channelRoute(time, view, plane, route) && route->isValid(getNInputs());
}

bool
ChannelRouteEffect::canRenderOnGpu(const GpuQuery& query) const
{
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask || (query.mix != 1.) || query.hasConcatenatedInput) {
        return false;
    }
    if ((query.nComps < 1) || (query.nComps > 4) || (query.plane.getNumComponents() != query.nComps)) {
        return false;
    }
    ChannelRoute route;

    return resolveGpuRoute(query.time, query.view, query.plane, &route) && !route.cpuOnly;
}

bool
ChannelRouteEffect::getGpuInputLayer(int inputNb,
                                     const ImageLayerDesc& plane,
                                     double time,
                                     ViewIdx view,
                                     ImageLayerDesc* layer) const
{
    ChannelRoute route;

    if (!resolveGpuRoute(time, view, plane, &route)) {
        return false;
    }
    const std::vector<ImageLayerDesc> layers = routeLayersOfInput(route, inputNb);
    *layer = layers.empty() ? plane : layers.front();

    return true;
}

bool
ChannelRouteEffect::getGpuExtraInputLayers(int inputNb,
                                           const ImageLayerDesc& plane,
                                           double time,
                                           ViewIdx view,
                                           std::vector<ImageLayerDesc>* layers) const
{
    ChannelRoute route;

    if (!resolveGpuRoute(time, view, plane, &route)) {
        return false;
    }
    const std::vector<ImageLayerDesc> read = routeLayersOfInput(route, inputNb);
    if (read.size() > 1) {
        layers->insert(layers->end(), read.begin() + 1, read.end());
    }

    return true;
}

bool
ChannelRouteEffect::readsGpuInput(int inputNb,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view) const
{
    ChannelRoute route;

    if (!resolveGpuRoute(time, view, plane, &route)) {
        return true;
    }

    return !routeLayersOfInput(route, inputNb).empty();
}

StatusEnum
ChannelRouteEffect::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    ChannelRoute route;
    if (!canRenderOnGpu(query) || !resolveGpuRoute(query.time, query.view, query.plane, &route)) {
        return eStatusFailed;
    }

    const GpuImageView& dst = args.output;
    const RectI& rect = args.outputRect;
    if (rect.isNull() || (dst.nComps < 1) || (dst.nComps > 4) || !dst.bounds.contains(rect)) {
        return eStatusFailed;
    }
    ChannelRouteGpuParams base {};
    base.width = (uint32_t)rect.width();
    base.height = (uint32_t)rect.height();
    base.dstStride = (uint32_t)dst.rowStrideFloats;
    base.dstComps = (uint32_t)dst.nComps;
    if (!routeViewIndex(dst, rect.x1, rect.y1, &base.dstOffset)) {
        return eStatusFailed;
    }
    for (int c = 0; c < 4; ++c) {
        base.select[c] = kRouteSkip;
    }

    // The view each source reads: the input's own for the first layer of that input, a layer
    // input for the others, and none for a missing view, whose components then read 0.
    std::vector<const GpuImageView*> views(route.sources.size(), nullptr);
    for (std::size_t s = 0; s < route.sources.size(); ++s) {
        const ChannelRouteSource& source = route.sources[s];
        const std::vector<ImageLayerDesc> layers = routeLayersOfInput(route, source.inputNb);
        const GpuImageView* view = nullptr;
        if (sameLayer(layers.front(), source.layer)) {
            if (source.inputNb < (int)args.inputs.size()) {
                view = &args.inputs[source.inputNb];
            }
        } else {
            for (const GpuLayerInput& layerInput : args.layerInputs) {
                if ((layerInput.inputNb == source.inputNb) && sameLayer(layerInput.layer, source.layer)) {
                    view = &layerInput.view;
                    break;
                }
            }
        }
        if (view && view->buffer && !view->bounds.isNull()) {
            views[s] = view;
        }
    }

    std::vector<ChannelRouteGpuParams> sourcePasses(route.sources.size(), base);
    std::vector<bool> sourceUsed(route.sources.size(), false);
    ChannelRouteGpuParams constantPass = base;
    bool constantUsed = false;
    for (int c = 0; c < dst.nComps; ++c) {
        float constant = 0.f;
        if (c < (int)route.components.size()) {
            const ChannelRouteComponent& component = route.components[c];
            if (component.kind == ChannelRouteComponent::eKindConstant) {
                constant = component.constant;
            } else if (const GpuImageView* view = views[component.source]) {
                const int channel = routeSourceChannel(component, route.sources[component.source].layer, view->nComps);
                if (channel >= 0) {
                    sourcePasses[component.source].select[c] = (uint32_t)channel;
                    sourceUsed[component.source] = true;
                    continue;
                }
            }
        }
        constantPass.select[c] = kRouteConstant;
        std::memcpy(&constantPass.constants[c], &constant, sizeof(float));
        constantUsed = true;
    }

    gpu::GpuRecordContext& ctx = args.context;
    bool first = true;
    for (std::size_t s = 0; s < route.sources.size(); ++s) {
        if (!sourceUsed[s]) {
            continue;
        }
        const GpuImageView& src = *views[s];
        ChannelRouteGpuParams& p = sourcePasses[s];
        p.srcComps = (uint32_t)src.nComps;
        p.srcStride = (uint32_t)src.rowStrideFloats;
        p.validX1 = clampedThreads((int64_t)src.bounds.x1 - rect.x1, p.width);
        p.validX2 = clampedThreads((int64_t)src.bounds.x2 - rect.x1, p.width);
        p.validY1 = clampedThreads((int64_t)src.bounds.y1 - rect.y1, p.height);
        p.validY2 = clampedThreads((int64_t)src.bounds.y2 - rect.y1, p.height);
        if ((p.validX1 < p.validX2) && (p.validY1 < p.validY2)) {
            if (!routeViewIndex(src, rect.x1 + (int)p.validX1, rect.y1 + (int)p.validY1, &p.srcOffset)) {
                return eStatusFailed;
            }
        } else {
            p.validX1 = p.validX2 = p.validY1 = p.validY2 = 0;
        }
        if (!first) {
            ctx.computeBarrier();
        }
        first = false;
        if (!ctx.dispatch(gpu::GpuKernelId::channelroute, { src.buffer, dst.buffer }, p, { p.width, p.height, 1 })) {
            return eStatusFailed;
        }
    }
    if (constantUsed) {
        if (!first) {
            ctx.computeBarrier();
        }
        // The constant pass reads no source, but the binding still needs a buffer.
        if (!ctx.dispatch(gpu::GpuKernelId::channelroute, { dst.buffer, dst.buffer }, constantPass, { constantPass.width, constantPass.height, 1 })) {
            return eStatusFailed;
        }
    }

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // ChannelRouteEffect::renderGpu

NATRON_NAMESPACE_EXIT
