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

#ifndef Engine_Nodes_Channel_ChannelCopy_h
#define Engine_Nodes_Channel_ChannelCopy_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <optional>
#include <vector>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/NativeEffectBase.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Colour-plane lookup and channel copying shared by the Channel nodes that rebuild the
 * colour storage plane from their input's (RemoveLayers, AddLayers).
 **/
namespace ChannelCopy {

/**
 * @brief The colour storage plane of `layers`, or NULL when they carry none.
 **/
const ImageLayerDesc* findColorStorage(const std::list<ImageLayerDesc>& layers);

/**
 * @brief The colour storage layout the metadata of input `inputNb` declare, or the None layout
 * when that input is not connected. Metadata do not vary with time, so neither does this,
 * whereas the input's present layers may.
 **/
ImageLayerDesc metadataColorStorage(const EffectInstance& effect, int inputNb);

/**
 * @brief For each channel of the colour plane `plane`, the index of the channel of `source`
 * on the same colour bit when `copiedBits` holds that bit, otherwise -1.
 **/
std::vector<int> mapColorChannels(const ImageLayerDesc& plane, const Image& source, const std::bitset<4>& copiedBits);

/**
 * @brief Writes each channel c of `dst` over `roi` from channel srcChannels[c] of `src`, and 0
 * where that index is -1 or missing or `src` is NULL. `src` may have another depth than `dst`,
 * since the Channel nodes fetch their input without mapping it to their clip preferences.
 * Returns eStatusFailed for an unsupported depth, and stops early when `effect` is aborted.
 **/
StatusEnum copyChannels(const Image::ReadAccess* src,
                        ImageBitDepthEnum srcDepth,
                        Image* dst,
                        const RectI& roi,
                        const std::vector<int>& srcChannels,
                        const EffectInstance& effect);

} // namespace ChannelCopy

/**
 * @brief One input layer a ChannelRoute reads: input inputNb fetched in layer, as stored.
 **/
struct ChannelRouteSource {
    /**
     * @brief How the CPU reads the source when it is stored at a depth other than float.
     **/
    enum NonFloatEnum {
        eNonFloatReadsZero = 0,
        eNonFloatConverts
    };

    int inputNb;
    ImageLayerDesc layer;
    NonFloatEnum nonFloat;

    ChannelRouteSource()
        : inputNb(-1)
        , layer()
        , nonFloat(eNonFloatReadsZero)
    {
    }

    ChannelRouteSource(int inputNb_,
                       const ImageLayerDesc& layer_,
                       NonFloatEnum nonFloat_)
        : inputNb(inputNb_)
        , layer(layer_)
        , nonFloat(nonFloat_)
    {
    }
};

/**
 * @brief What one output component of a ChannelRoute holds: a constant, the channel of a source
 * at an index, or the channel of a source's colour plane on a colour bit (see
 * ResolvedLayer::channelBit). A colour bit is resolved against the source's layout as it is
 * fetched, so it finds the same channel whichever colour layout the input stores. A channel the
 * source does not have reads 0.
 **/
struct ChannelRouteComponent {
    enum KindEnum {
        eKindConstant = 0,
        eKindChannel,
        eKindColorBit
    };

    KindEnum kind;
    int source;
    int index;
    float constant;

    ChannelRouteComponent()
        : kind(eKindConstant)
        , source(-1)
        , index(-1)
        , constant(0.f)
    {
    }

    static ChannelRouteComponent zero()
    {
        return ChannelRouteComponent();
    }

    static ChannelRouteComponent one()
    {
        ChannelRouteComponent c;

        c.constant = 1.f;

        return c;
    }

    static ChannelRouteComponent channel(int source,
                                         int index)
    {
        ChannelRouteComponent c;

        c.kind = eKindChannel;
        c.source = source;
        c.index = index;

        return c;
    }

    static ChannelRouteComponent colorBit(int source,
                                          int bit)
    {
        ChannelRouteComponent c;

        c.kind = eKindColorBit;
        c.source = source;
        c.index = bit;

        return c;
    }
};

/**
 * @brief How a channel node fills one output plane at a time and view, which the CPU
 * (ChannelCopy::runChannelRouteCpu) and the GPU (ChannelRouteEffect::renderGpu) both run.
 * components holds one entry per component of the plane; an output image wider than the plane
 * gets 0 in its extra components. Every source pixel outside the source's bounds, and every
 * source that is disconnected, missing or (with eNonFloatReadsZero) not float, reads 0.
 * cpuOnly keeps the GPU off, for a route the GPU's float-only inputs cannot reproduce.
 **/
struct ChannelRoute {
    std::vector<ChannelRouteSource> sources;
    std::vector<ChannelRouteComponent> components;
    bool cpuOnly;

    ChannelRoute()
        : sources()
        , components()
        , cpuOnly(false)
    {
    }

    /**
     * @brief Whether every component names a source of the route and every source an input.
     **/
    bool isValid(int nInputs) const WARN_UNUSED_RETURN;
};

namespace ChannelCopy {

/**
 * @brief Fills dst over roi as route says, sourceImages[i] being route.sources[i] as fetched
 * (null when it reads 0). Images may be listed more than once; each is locked once. Returns
 * eStatusFailed for an invalid route, a destination depth other than byte, short or float, or a
 * converted source at any other depth, and stops early when effect is aborted.
 **/
StatusEnum applyChannelRoute(const ChannelRoute& route,
                             const std::vector<ImagePtr>& sourceImages,
                             Image* dst,
                             const RectI& roi,
                             const EffectInstance& effect);

/**
 * @brief Renders args.outputLayers, routes[i] filling the i-th one. Every source of every route
 * is fetched, once per input and layer, before any image is locked: fetching renders upstream,
 * which may write into a cached image this render would otherwise already hold a read lock on.
 **/
StatusEnum runChannelRouteCpu(EffectInstance& effect,
                              const EffectInstance::RenderActionArgs& args,
                              const std::vector<ChannelRoute>& routes);

} // namespace ChannelCopy

/**
 * @brief A channel node whose output planes are each described by a ChannelRoute. The CPU
 * render and the GPU render both run that route, so a node implements channelRoute() only; the
 * node keeps its own components, regions, identities and hash.
 *
 * On the GPU, each source's input arrives in its layer: the first layer a route reads of an input
 * is getGpuInputLayer()'s, the others getGpuExtraInputLayers()'s. The GPU reads float only.
 **/
class ChannelRouteEffect
    : public NativeEffectBase {
public:
    explicit ChannelRouteEffect(NodePtr node);

    virtual ~ChannelRouteEffect();

    /**
     * @brief The route filling plane at time and view. False when the node cannot describe the
     * plane, which keeps the GPU off and fails the CPU render. Called from the render, placement
     * and recording threads, so it must depend only on its arguments, the node's settings and
     * its inputs.
     **/
    virtual bool channelRoute(double time, ViewIdx view, const ImageLayerDesc& plane, ChannelRoute* route) WARN_UNUSED_RETURN = 0;

    virtual std::optional<OpKind> opKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return OpKind::Point;
    }

    virtual bool canRenderOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getGpuInputRoI(int /*inputNb*/,
                                 const RectI& outputRect,
                                 const RenderScale& /*scale*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return outputRect;
    }

    /**
     * @brief The first layer the route of plane reads input inputNb in, or plane itself when the
     * route does not read that input.
     **/
    virtual bool getGpuInputLayer(int inputNb,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The other layers the route of plane reads input inputNb in, each once.
     **/
    virtual bool getGpuExtraInputLayers(int inputNb,
                                        const ImageLayerDesc& plane,
                                        double time,
                                        ViewIdx view,
                                        std::vector<ImageLayerDesc>* layers) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief One channelroute dispatch per source, each writing the components it feeds, then one
     * writing the constants and every component whose source has no image.
     **/
    virtual StatusEnum renderGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool resolveGpuRoute(double time, ViewIdx view, const ImageLayerDesc& plane, ChannelRoute* route) const WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Channel_ChannelCopy_h
