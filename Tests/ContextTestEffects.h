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

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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
#define kTestPluginIDContextFormat "test.natron.built-in.ContextFormat"
#define kTestPluginIDContextRegion "test.natron.built-in.ContextRegion"
#define kTestPluginIDContextUnion "test.natron.built-in.ContextUnion"
#define kTestPluginIDContextSet "test.natron.built-in.ContextSet"

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
 * @brief The value of the named variable in context, 0 when unset.
 **/
inline double
contextTestValue(const RenderContextPtr& context,
                 const std::string& name)
{
    const RenderContextValue* value = context ? context->find(RenderContextName::plain(name)) : 0;
    const double* asDouble = value ? std::get_if<double>(value) : 0;

    return asDouble ? *asDouble : 0.;
}

/**
 * @brief The names in a comma-separated list, empty ones skipped.
 **/
inline std::vector<std::string>
contextTestNames(const std::string& list)
{
    std::vector<std::string> names;
    std::stringstream ss(list);
    std::string name;
    while (std::getline(ss, name, ',')) {
        if (!name.empty()) {
            names.push_back(name);
        }
    }

    return names;
}

/**
 * @brief The region of definition calls of the context test effects, by node and by the value of
 * the test variable in the context the call ran under.
 **/
class ContextTestRegistry {
public:
    static void noteRegionOfDefinition(const Node* node,
                                       double value)
    {
        std::lock_guard<std::mutex> k(mutex());
        ++regions()[std::make_pair(node, value)];
    }

    static int regionsOfDefinition(const Node* node,
                                   double value)
    {
        std::lock_guard<std::mutex> k(mutex());
        std::map<std::pair<const Node*, double>, int>::const_iterator found = regions().find(std::make_pair(node, value));

        return found == regions().end() ? 0 : found->second;
    }

    // The right edge of a region of definition answered under the given value.
    static void noteRegionRight(const Node* node,
                                double value,
                                double right)
    {
        std::lock_guard<std::mutex> k(mutex());
        rights()[std::make_pair(node, value)].insert(right);
    }

    // The right edges of the regions of definition node answered under value.
    static std::set<double> regionRights(const Node* node,
                                         double value)
    {
        std::lock_guard<std::mutex> k(mutex());
        std::map<std::pair<const Node*, double>, std::set<double>>::const_iterator found = rights().find(std::make_pair(node, value));

        return found == rights().end() ? std::set<double>() : found->second;
    }

    static void reset()
    {
        std::lock_guard<std::mutex> k(mutex());
        regions().clear();
        rights().clear();
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex m;

        return m;
    }

    static std::map<std::pair<const Node*, double>, int>& regions()
    {
        static std::map<std::pair<const Node*, double>, int> r;

        return r;
    }

    static std::map<std::pair<const Node*, double>, std::set<double>>& rights()
    {
        static std::map<std::pair<const Node*, double>, std::set<double>> r;

        return r;
    }
};

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

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const double v = contextTestValue(getCurrentRenderContext());
        ContextTestRegistry::noteRegionOfDefinition(getNode().get(), v);
        if (regionOfDefinitionFromInputs()) {
            const StatusEnum stat = EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
            ContextTestRegistry::noteRegionRight(getNode().get(), v, rod->x2);

            return stat;
        }
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = regionWidth(getCurrentRenderContext());
        rod->y2 = kCountingTestSize;
        ContextTestRegistry::noteRegionRight(getNode().get(), v, rod->x2);

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
     * @brief The width of the region of definition, from the origin, under context.
     **/
    virtual double regionWidth(const RenderContextPtr& /*context*/) const
    {
        return kCountingTestSize;
    }

    /**
     * @brief Whether the region of definition is the host's default, the union of the inputs'.
     **/
    virtual bool regionOfDefinitionFromInputs() const
    {
        return false;
    }

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
        for (const std::string& name : variables()) {
            reads.add(RenderContextName::plain(name));
        }

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

        KnobStringPtr variables = createKnob<KnobString>(std::string("Variables"));
        variables->setName("variables");
        variables->setAnimationEnabled(false);
        variables->setDefaultValue(contextTestVariable().name);
        page->addKnob(variables);
        _variables = variables;
    }

    // The variables read, comma-separated in the variables knob.
    std::vector<std::string> variables() const
    {
        KnobStringPtr variablesKnob = _variables.lock();

        return contextTestNames(variablesKnob ? variablesKnob->getValue() : contextTestVariable().name);
    }

    virtual double value(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        KnobDoublePtr valueKnob = _value.lock();
        double sum = valueKnob ? valueKnob->getValue() : 0.;
        for (const std::string& name : variables()) {
            sum += contextTestValue(context, name);
        }

        return sum;
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
    KnobStringWPtr _variables;
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

    virtual RenderContextReadSet getOverriddenContextVariables(int inputNb) const OVERRIDE FINAL
    {
        RenderContextReadSet overridden;
        if (inputNb == 0) {
            KnobStringPtr variableKnob = _variable.lock();
            overridden.add(variableKnob ? RenderContextName::plain(variableKnob->getValue()) : contextTestVariable());
        }

        return overridden;
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
        KnobStringPtr variableKnob = _variable.lock();
        const RenderContextName variable = variableKnob ? RenderContextName::plain(variableKnob->getValue()) : contextTestVariable();
        std::vector<RenderContextPtr> contexts;
        for (int i = 0; i < count; ++i) {
            contexts.push_back(RenderContext::Builder(context).set(variable, RenderContextValue(double(equal ? first : first + i))).build());
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

        KnobStringPtr variable = createKnob<KnobString>(std::string("Variable"));
        variable->setName("variable");
        variable->setAnimationEnabled(false);
        variable->setDefaultValue(contextTestVariable().name);
        page->addKnob(variable);
        _variable = variable;
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
    KnobStringWPtr _variable;
};

/**
 * @brief Reads the test variable and, under a value k > 0, requests its own output under k - 1, the
 * way a feedback loop requests its previous iteration. With selfTarget set it requests its own
 * output under the context it renders under instead; with cycle set, under 1 - k, so that the
 * contexts 0 and 1 request each other; with previousFrame set, at the previous frame under the same
 * context while the frame is past 0.
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
        KnobBoolPtr cycleKnob = _cycle.lock();
        KnobBoolPtr previousFrameKnob = _previousFrame.lock();
        const bool selfTarget = selfTargetKnob && selfTargetKnob->getValue();
        const bool cycle = cycleKnob && cycleKnob->getValue();
        const bool previousFrame = previousFrameKnob && previousFrameKnob->getValue();
        const double k = contextTestValue(context);
        ContextNodeRequest request;
        request.node = getNode();
        request.time = time;
        request.view = view;
        request.roi = roi;
        if (previousFrame) {
            if (time <= 0.) {
                return;
            }
            request.context = context;
            request.time = time - 1.;
        } else if (cycle) {
            request.context = RenderContext::Builder(context).set(contextTestVariable(), RenderContextValue(1. - k)).build();
        } else if (selfTarget) {
            request.context = context;
        } else if (k > 0.) {
            request.context = RenderContext::Builder(context).set(contextTestVariable(), RenderContextValue(k - 1.)).build();
        } else {
            return;
        }
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

        KnobBoolPtr cycle = createKnob<KnobBool>(std::string("Cycle"));
        cycle->setName("cycle");
        cycle->setAnimationEnabled(false);
        cycle->setDefaultValue(false);
        page->addKnob(cycle);
        _cycle = cycle;

        KnobBoolPtr previousFrame = createKnob<KnobBool>(std::string("Previous Frame"));
        previousFrame->setName("previousFrame");
        previousFrame->setAnimationEnabled(false);
        previousFrame->setDefaultValue(false);
        page->addKnob(previousFrame);
        _previousFrame = previousFrame;
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
    KnobBoolWPtr _cycle;
    KnobBoolWPtr _previousFrame;
};

/**
 * @brief Reads the test variable v, when its reads knob is set, into its output format, which is
 * 32 + v wide, and counts its metadata derivations.
 **/
class ContextFormatTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextFormatTestEffect(n);
    }

    explicit ContextFormatTestEffect(NodePtr n)
        : ContextTestEffectBase(n)
        , _derivations(0)
    {
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        KnobBoolPtr readsKnob = _reads.lock();
        if (!readsKnob || readsKnob->getValue()) {
            reads.add(contextTestVariable());
        }

        return reads;
    }

    // How often getPreferredMetadataAt() ran.
    int derivations() const
    {
        return _derivations.load();
    }

    // The output width under a context where the test variable is v.
    static int formatWidth(double v)
    {
        return kCountingTestSize + (int)v;
    }

private:
    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        ++_derivations;
        metadata.setOutputFormat(RectI(0, 0, formatWidth(contextTestValue(getCurrentRenderContext())), kCountingTestSize));

        return eStatusOK;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr readsKnob = createKnob<KnobBool>(std::string("Reads"));
        readsKnob->setName("reads");
        readsKnob->setAnimationEnabled(false);
        readsKnob->setDefaultValue(true);
        page->addKnob(readsKnob);
        _reads = readsKnob;
    }

    virtual double value(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        return contextTestValue(context);
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextFormat,
        .label = "Test Context Format",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobBoolWPtr _reads;
    std::atomic<int> _derivations;
};

/**
 * @brief Reads the test variable v into its region of definition, kCountingTestSize + v wide, and
 * into its output, which is v.
 **/
class ContextRegionTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextRegionTestEffect(n);
    }

    explicit ContextRegionTestEffect(NodePtr n)
        : ContextTestEffectBase(n)
    {
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        reads.add(contextTestVariable());

        return reads;
    }

    // The width of the region of definition under a context where the test variable is v.
    static double regionWidthUnder(double v)
    {
        return kCountingTestSize + v;
    }

private:
    virtual double regionWidth(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        return regionWidthUnder(contextTestValue(context));
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual double value(const RenderContextPtr& context) const OVERRIDE FINAL
    {
        return contextTestValue(context);
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextRegion,
        .label = "Test Context Region",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

/**
 * @brief Reads no variable: its region of definition is the host's default, the union of its
 * input's, and its output is its input.
 **/
class ContextUnionTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextUnionTestEffect(n);
    }

    explicit ContextUnionTestEffect(NodePtr n)
        : ContextTestEffectBase(n)
    {
    }

private:
    virtual bool regionOfDefinitionFromInputs() const OVERRIDE FINAL
    {
        return true;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual double value(const RenderContextPtr& /*context*/) const OVERRIDE FINAL
    {
        return 0.;
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextUnion,
        .label = "Test Context Union",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

/**
 * @brief Requests its input under a single context of its own, which sets the test variable to its
 * value knob, through getInputContexts() alone: its render pulls the input with getImage() and no
 * InputContextScope. Its output is its input.
 **/
class ContextSetTestEffect
    : public ContextTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContextSetTestEffect(n);
    }

    explicit ContextSetTestEffect(NodePtr n)
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
        KnobDoublePtr valueKnob = _value.lock();
        contexts->push_back(RenderContext::Builder(context).set(contextTestVariable(), RenderContextValue(valueKnob ? valueKnob->getValue() : 0.)).build());

        return true;
    }

    virtual RenderContextReadSet getOverriddenContextVariables(int inputNb) const OVERRIDE FINAL
    {
        RenderContextReadSet overridden;
        if (inputNb == 0) {
            overridden.add(contextTestVariable());
        }

        return overridden;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobDoublePtr value = createKnob<KnobDouble>(std::string("Value"));
        value->setName("value");
        value->setAnimationEnabled(false);
        value->setDefaultValue(5.);
        page->addKnob(value);
        _value = value;
    }

    virtual double value(const RenderContextPtr& /*context*/) const OVERRIDE FINAL
    {
        return 0.;
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContextSet,
        .label = "Test Context Set",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobDoubleWPtr _value;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_ContextTestEffects_h
