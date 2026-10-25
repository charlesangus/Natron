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

#ifndef Tests_ContextTestEffects_h
#define Tests_ContextTestEffects_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "CountingTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDContextReader "test.natron.built-in.ContextReader"
#define kTestPluginIDContextFanOut "test.natron.built-in.ContextFanOut"
#define kTestPluginIDContextChain "test.natron.built-in.ContextChain"

NATRON_NAMESPACE_ENTER

/**
 * @brief The context variable the context test effects set and read.
 **/
inline RenderContextName
contextTestVariable()
{
    return RenderContextName::plain("v");
}

/**
 * @brief The value of the test variable in the context the effect renders under, 0 when unset.
 **/
inline double
contextTestValue(const RenderContextPtr& context)
{
    const RenderContextValue* value = context ? context->find(contextTestVariable()) : 0;
    const double* asDouble = value ? std::get_if<double>(value) : 0;

    return asDouble ? *asDouble : 0.;
}

/**
 * @brief A tiled RGBA float effect of kCountingTestSize square whose render fills its output with
 * value(), plus the first pixel of each input it pulls under inputContexts(), and counts its
 * renders in CountingTestRegistry.
 **/
class ContextTestEffectBase
    : public NativeEffectBase {
public:
    explicit ContextTestEffectBase(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kCountingTestSize;
        rod->y2 = kCountingTestSize;

        return eStatusOK;
    }

    virtual void addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

protected:
    /**
     * @brief What the output holds besides its inputs, under the context the effect renders under.
     **/
    virtual double value(const RenderContextPtr& context) const = 0;

    /**
     * @brief The contexts input 0 is pulled under, one pull each; the render's own when empty.
     **/
    virtual std::vector<RenderContextPtr> inputContexts(const RenderContextPtr& /*context*/) const
    {
        return std::vector<RenderContextPtr>();
    }

private:
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        CountingTestRegistry::noteRender(getNode().get());

        const ParallelRenderArgsPtr frameArgs = getParallelRenderArgsTLS();
        const RenderContextPtr context = frameArgs ? frameArgs->context : RenderContextPtr();
        float out = (float)value(context);
        if (getInput(0)) {
            std::vector<RenderContextPtr> contexts = inputContexts(context);
            const bool scoped = !contexts.empty();
            if (!scoped) {
                contexts.push_back(context);
            }
            for (const RenderContextPtr& inputContext : contexts) {
                float pixel = 0.f;
                if (scoped) {
                    InputContextScope scope(this, inputContext);
                    if (!readInputPixel(args, &pixel)) {
                        return eStatusFailed;
                    }
                } else if (!readInputPixel(args, &pixel)) {
                    return eStatusFailed;
                }
                out += pixel;
            }
        }

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
                return eStatusFailed;
            }
            const int numChannels = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < numChannels; ++c) {
                        pixel[c] = out;
                    }
                }
            }
        }

        return eStatusOK;
    }

    bool readInputPixel(const RenderActionArgs& args,
                        float* value)
    {
        RectI inputRoI;
        ImagePtr input = getImage(0, args.time, args.mappedScale, args.view, NULL, &ImageLayerDesc::getRGBAComponents(),
                                  false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
        if (!input || (input->getBitDepth() != eImageBitDepthFloat) || !input->getBounds().contains(args.roi.x1, args.roi.y1)) {
            return false;
        }
        Image::ReadAccess access = input->getReadRights();
        const float* pixel = (const float*)access.pixelAt(args.roi.x1, args.roi.y1);
        if (!pixel) {
            return false;
        }
        *value = pixel[0];

        return true;
    }
};

/**
 * @brief Reads the test variable: its output is the variable's value, plus its value knob, plus its
 * input.
 **/
class ContextReaderTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextReaderTestEffect(n);
    }

    explicit ContextReaderTestEffect(NodePtr n)
        : ContextTestEffectBase(n)
    {
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        reads.add(contextTestVariable());

        return reads;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobDoublePtr value = createKnob<KnobDouble>(std::string("Value"));
        value->setName("value");
        value->setAnimationEnabled(false);
        value->setDefaultValue(1.);
        page->addKnob(value);
        _value = value;
    }

    virtual double value(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        KnobDoublePtr valueKnob = _value.lock();

        return contextTestValue(context) + (valueKnob ? valueKnob->getValue() : 0.);
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextReader,
        .label = "Test Context Reader",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobDoubleWPtr _value;
};

/**
 * @brief Pulls its input under count contexts setting the test variable to first, first + 1, ...,
 * or all to first when equal is set, and sums them.
 **/
class ContextFanOutTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextFanOutTestEffect(n);
    }

    explicit ContextFanOutTestEffect(NodePtr n)
        : ContextTestEffectBase(n)
    {
    }

    virtual bool getInputContexts(int inputNb,
                                  double /*time*/,
                                  ViewIdx /*view*/,
                                  const RenderContextPtr& context,
                                  std::vector<RenderContextPtr>* contexts) const OVERRIDE FINAL
    {
        if (inputNb != 0) {
            return false;
        }
        *contexts = inputContexts(context);

        return true;
    }

protected:
    virtual std::vector<RenderContextPtr> inputContexts(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        KnobIntPtr countKnob = _count.lock();
        KnobIntPtr firstKnob = _first.lock();
        KnobBoolPtr equalKnob = _equal.lock();
        const int count = countKnob ? countKnob->getValue() : 0;
        const int first = firstKnob ? firstKnob->getValue() : 0;
        const bool equal = equalKnob && equalKnob->getValue();
        std::vector<RenderContextPtr> contexts;
        for (int i = 0; i < count; ++i) {
            contexts.push_back(RenderContext::Builder(context).set(contextTestVariable(), RenderContextValue(double(equal ? first : first + i))).build());
        }

        return contexts;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobIntPtr count = createKnob<KnobInt>(std::string("Count"));
        count->setName("count");
        count->setAnimationEnabled(false);
        count->setDefaultValue(3);
        page->addKnob(count);
        _count = count;

        KnobIntPtr first = createKnob<KnobInt>(std::string("First"));
        first->setName("first");
        first->setAnimationEnabled(false);
        first->setDefaultValue(0);
        page->addKnob(first);
        _first = first;

        KnobBoolPtr equal = createKnob<KnobBool>(std::string("Equal"));
        equal->setName("equal");
        equal->setAnimationEnabled(false);
        equal->setDefaultValue(false);
        page->addKnob(equal);
        _equal = equal;
    }

    virtual double value(const RenderContextPtr& /*context*/) const OVERRIDE FINAL
    {
        return 0.;
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextFanOut,
        .label = "Test Context Fan Out",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobIntWPtr _count;
    KnobIntWPtr _first;
    KnobBoolWPtr _equal;
};

/**
 * @brief Reads the test variable and, under a value k > 0, requests its own output under k - 1, the
 * way a feedback loop requests its previous iteration. With selfTarget set it requests its own
 * output under the context it renders under instead.
 **/
class ContextChainTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextChainTestEffect(n);
    }

    explicit ContextChainTestEffect(NodePtr n)
        : ContextTestEffectBase(n)
    {
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        reads.add(contextTestVariable());

        return reads;
    }

    virtual void getContextNodeRequests(double time,
                                        ViewIdx view,
                                        const RenderContextPtr& context,
                                        const RectD& roi,
                                        std::vector<ContextNodeRequest>* requests) const OVERRIDE FINAL
    {
        KnobBoolPtr selfTargetKnob = _selfTarget.lock();
        const bool selfTarget = selfTargetKnob && selfTargetKnob->getValue();
        const double k = contextTestValue(context);
        if (!selfTarget && (k <= 0.)) {
            return;
        }
        ContextNodeRequest request;
        request.node = getNode();
        request.context = selfTarget ? context : RenderContext::Builder(context).set(contextTestVariable(), RenderContextValue(k - 1.)).build();
        request.time = time;
        request.view = view;
        request.roi = roi;
        requests->push_back(request);
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr selfTarget = createKnob<KnobBool>(std::string("Self Target"));
        selfTarget->setName("selfTarget");
        selfTarget->setAnimationEnabled(false);
        selfTarget->setDefaultValue(false);
        page->addKnob(selfTarget);
        _selfTarget = selfTarget;
    }

    virtual double value(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        return contextTestValue(context);
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextChain,
        .label = "Test Context Chain",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobBoolWPtr _selfTarget;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_ContextTestEffects_h
