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

#ifndef Engine_Nodes_Image_NativeImageEffect_h
#define Engine_Nodes_Image_NativeImageEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/NativeEffectBase.h"

namespace gpu {
struct GpuBlurImage;
struct GpuBlurRect;
struct GpuWrap;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief The host-facing traits of a NativeImageEffect subclass, fixed per class.
 *
 * hostUnPremult asks for the host "(Un)premult by" selector, which the base then honours in
 * render(). generator gives a target layer select instead of the input-bound channel set.
 * processesAllLayers starts the channel set on "All" rather than the colour layer.
 * defaultChannels is the R, G, B, A row the channel set starts with.
 **/
struct NativeImageTraits {
    bool hostUnPremult;
    bool generator;
    bool processesAllLayers;
    bool defaultChannels[4];

    NativeImageTraits()
        : hostUnPremult(false)
        , generator(false)
        , processesAllLayers(false)
    {
        for (int i = 0; i < 4; ++i) {
            defaultChannels[i] = true;
        }
    }
};

/**
 * @brief Base for flat 2D native image nodes: float-only, tile-capable, multi-resolution, render
 * scale supported, and FullySafe. The host does not split a render window across threads, so
 * render() splits it into row bands itself over the global pool, within the thread budget the
 * host grants (AppManager::getNCPUsAvailableForEffect()); the kernel must therefore be safe to
 * call concurrently. Band threads carry no render TLS: they never take an image lock or read
 * image bounds (both happen on the calling thread before the bands start) and learn of an abort
 * only through RenderCancellation. Every image render() fetches is one the default request
 * planning already declares at the render window, so the scheduler pulls nothing it did not plan.
 **/
class NativeImageEffect
    : public NativeEffectBase {
public:
    explicit NativeImageEffect(NodePtr node,
                               const NativeImageTraits& traits = NativeImageTraits());

    virtual ~NativeImageEffect();

    virtual bool supportsTiles() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsMultiResolution() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual bool wantsHostUnPremultSelector() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _traits.hostUnPremult;
    }

    virtual bool rendersUnprocessedChannels() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool defaultProcessesAllLayers() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _traits.processesAllLayers;
    }

    virtual bool isHostChannelSelectorSupported(bool* defaultR,
                                                bool* defaultG,
                                                bool* defaultB,
                                                bool* defaultA) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief True when the node is a point operator rendered by the base's pipeline from
     * makeKernel().
     **/
    virtual bool isPointOp() const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief Whether renderGpu() can produce this query. Nodes that cannot are rendered on the
     * CPU. The base accepts a point operator's float query of 1 to 4 channels when gpuKernelId()
     * names a kernel and any "(Un)premult by" channel lies in the rendered plane.
     **/
    virtual bool canRenderOnGpu(const GpuQuery& query) const WARN_UNUSED_RETURN;

    /**
     * @brief The region of input inputNb that renderGpu() reads to produce outputRect; wider
     * than outputRect for a node with a halo.
     **/
    virtual RectI getGpuInputRoI(int /*inputNb*/,
                                 const RectI& outputRect,
                                 const RenderScale& /*scale*/) const WARN_UNUSED_RETURN
    {
        return outputRect;
    }

    /**
     * @brief The layer renderGpu() reads input inputNb in when this node renders plane at time and
     * view; the region engine hands that input over in it. The base answers plane, except for an
     * input described as a mask, which is read in the layer its mask channel was resolved in, so
     * that the channel indexes the mask view's own layout. False when the GPU path cannot read
     * the input at all, which keeps the node on the CPU.
     **/
    virtual bool getGpuInputLayer(int inputNb,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  ImageLayerDesc* layer) const WARN_UNUSED_RETURN;

    /**
     * @brief Records the dispatches that fill args.output over args.outputRect. Only called
     * when canRenderOnGpu() accepted args.query. The base records a point operator's
     * makeGpuKernel() description wrapped as render() wraps makeKernel()'s, with the mask,
     * mix, mask inversion and "(Un)premult by" read at the query's time and view.
     **/
    virtual StatusEnum renderGpu(const GpuRenderArgs& args) WARN_UNUSED_RETURN;

    /**
     * @brief The input render() reads as its mask: the first input described as a mask, or -1.
     **/
    int getMaskInput() const WARN_UNUSED_RETURN;

    /**
     * @brief Splits roi into the horizontal row bands a point operator's render() runs in
     * parallel for nThreads threads: one band when nThreads is 1 or the window is too small to
     * be worth sharing. Exposed so tests can reason about the partition.
     **/
    static void makeRowBands(const RectI& roi,
                             int nThreads,
                             std::vector<RectI>* bands);

protected:
    /**
     * @brief Builds the point operator's kernel from the knob values at context. Called once
     * per render call; a null kernel fails the render.
     **/
    virtual PixelKernelPtr makeKernel(const KernelContext& context) WARN_UNUSED_RETURN;

    /**
     * @brief The GPU kernel the point operator would render query with, as a gpu::GpuKernelId,
     * or -1 to keep these settings on the CPU. A cheap check of settings, called during
     * placement; the base's canRenderOnGpu() adds the format and "(Un)premult by" rules.
     **/
    virtual int gpuKernelId(const GpuQuery& /*query*/) const WARN_UNUSED_RETURN
    {
        return -1;
    }

    /**
     * @brief Fills gpu with the point operator's GPU description at context, read from the
     * same knob values as makeKernel(). Called once per renderGpu(); false fails it.
     **/
    virtual bool makeGpuKernel(const KernelContext& /*context*/,
                               PixelKernelGpu* /*gpu*/) WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief Whether the operator itself leaves its input unchanged at (time, view) over roi,
     * whatever mask and mix say. The base's isIdentity() adds the mask and mix rules and makes
     * input 0 the identity input.
     **/
    virtual bool isIdentityOp(double /*time*/,
                              const RenderScale& /*scale*/,
                              const RectI& /*roi*/,
                              ViewIdx /*view*/) WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief Declares maskInvert (false) and mix (1, range 0..1) with the OpenFX names, labels
     * and hints, and adds them to page. The host moves both to the end of the page.
     **/
    void addMaskMixKnobs(const KnobPagePtr& page);

    double getMixValue(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The component of the rendered plane that "(Un)premult by" divides by at query's
     * time and view, -1 when there is none. False when the divisor lies outside the plane, which
     * canRenderOnGpu() must refuse.
     **/
    bool getGpuUnPremultComponent(const GpuQuery& query, int* component) WARN_UNUSED_RETURN;

    /**
     * @brief For a spatial node's renderGpu(): records the source over scratch.bounds into
     * scratch, divided by the "(Un)premult by" channel where that is usable, as render()'s
     * spatial nodes fill their buffer. Every channel is written; the source reads zero outside its
     * view. scratch has the plane's channel count.
     **/
    StatusEnum recordGpuDivide(const GpuRenderArgs& args,
                               const GpuImageView& scratch) WARN_UNUSED_RETURN;

    /**
     * @brief For a spatial node's renderGpu(): writes args.output over args.outputRect from
     * filtered, the filter's result over a rect containing outputRect, as render()'s spatial
     * nodes write their window: processed channels multiplied back by the divisor and blended
     * with the undivided source by mask x mix, unprocessed channels passed through. With
     * alphaThreshold > 0, a filtered alpha below it reads zero first. Recorded work that wrote
     * filtered must be ordered before it with a compute barrier.
     **/
    StatusEnum recordGpuFinish(const GpuRenderArgs& args,
                               const GpuImageView& filtered,
                               float alphaThreshold = 0.f) WARN_UNUSED_RETURN;

    /**
     * @brief Whether view holds nComps float components per pixel in a buffer, with rows at least
     * as long as its bounds.
     **/
    static bool isUsableGpuView(const GpuImageView& view, int nComps) WARN_UNUSED_RETURN;

    /**
     * @brief view over r, a rect inside view.bounds; no buffer when r is empty, which reads as zero.
     **/
    static GpuImageView cropGpuView(const GpuImageView& view, const RectI& r) WARN_UNUSED_RETURN;

    /**
     * @brief The component of a mask view of viewNComps components that holds maskChannel of
     * maskLayer, or -1 when the view does not hold it. The region engine hands a mask over in
     * maskLayer, but a caller may hand a colour mask over in another colour layout.
     **/
    static int getGpuMaskViewComponent(const ImageLayerDesc& maskLayer, int maskChannel, int viewNComps) WARN_UNUSED_RETURN;

#ifdef NATRON_HAVE_GPU
    static gpu::GpuBlurRect toGpuBlurRect(const RectI& r) WARN_UNUSED_RETURN;
    static gpu::GpuBlurImage toGpuBlurImage(const GpuImageView& view) WARN_UNUSED_RETURN;
#endif

    bool getMaskInvertValue(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Whether render() reads a mask: the mask input is connected and enabled.
     **/
    bool isMaskApplied() const WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

private:
    // The wrap of args' source, mask, mix, processed channels and divisor, for the caller to
    // give an output and a destination.
    bool makeGpuWrap(const GpuRenderArgs& args, gpu::GpuWrap* wrap) WARN_UNUSED_RETURN;

    NativeImageTraits _traits;
    KnobDoubleWPtr _mix;
    KnobBoolWPtr _maskInvert;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_NativeImageEffect_h
