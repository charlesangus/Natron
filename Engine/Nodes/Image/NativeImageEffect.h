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
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/NativeEffectBase.h"

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
 * scale supported, and FullySafe. The host does not split a render window across threads, so a
 * point operator's render() splits it into row bands itself, over the global pool and within the
 * thread budget the host grants the render (AppManager::getNCPUsAvailableForEffect()), the way
 * an OpenFX plug-in does through the multithread suite; the bands only run the pure pixel
 * pipeline, so the kernel must be safe to call concurrently. It is not multiplanar: the host's
 * layer knob decides which planes are rendered and which channels of each are processed, and
 * render() is called once per plane.
 *
 * A point operator returns true from isPointOp() and builds a PixelKernel in makeKernel(); the
 * base's render() then does the whole per-pixel pipeline in one pass over each row of the
 * window, so the host neither copies unprocessed channels nor multiplies back:
 *  1. divide every channel but the divisor itself by the "(Un)premult by" channel where it is
 *     usable (Image::unPremultDivisorIsUsable());
 *  2. run the kernel;
 *  3. multiply the processed channels back (Image::premultiplyValue());
 *  4. blend each processed channel with the undivided source by mask x mix, with the OpenFX
 *     ofxsMaskMixPix() arithmetic;
 *  5. pass the unprocessed channels through from the source.
 * A pixel outside the source reads as zero, outside the divisor as a divisor of one (neither
 * divided nor multiplied), and outside the mask as a mask value of zero.
 *
 * The first input described as a mask is the one the pipeline reads; addMaskMixKnobs() declares
 * the maskInvert and mix knobs it reads. Every image render() fetches (the source plane, the
 * divisor plane and the mask) is one the default request planning already declares: the mask
 * layer through the mask selector, the divisor layer through the "(Un)premult by" selector, at
 * the render window, so the scheduler pulls nothing it did not plan.
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
    NativeImageTraits _traits;
    KnobDoubleWPtr _mix;
    KnobBoolWPtr _maskInvert;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_NativeImageEffect_h
