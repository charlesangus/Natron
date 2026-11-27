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

#ifndef Tests_ContinuationTestEffects_h
#define Tests_ContinuationTestEffects_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QThread>

#include "ContextTestEffects.h"
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

#define kTestPluginIDContinuationSource "test.natron.built-in.ContinuationSource"
#define kTestPluginIDProbe "test.natron.built-in.Probe"
#define kTestPluginIDRecursiveProbe "test.natron.built-in.RecursiveProbe"

NATRON_NAMESPACE_ENTER

const int kProbeTestContexts = 16;

inline RenderContextName
recursiveProbeVariable()
{
    return RenderContextName::plain("d");
}

class ContinuationTestRegistry {
public:
    typedef std::function<std::shared_ptr<void>()> CaptureFactory;

    static void reset()
    {
        probes() = 0;
        continuations() = 0;
        claimsGpu() = false;
        supportsGpuContinuation() = false;
        setContinuationCapture(CaptureFactory());
    }

    // Off by default, so that the registered test plugin keeps to the capability invariant of the built-in nodes.
    static void setProbeClaimsGpu(bool claims)
    {
        claimsGpu() = claims;
    }

    static bool probeClaimsGpu()
    {
        return claimsGpu().load();
    }

    // Whether the probe also says its GPU path renders after its continuation; off by default.
    static void setProbeSupportsGpuContinuation(bool supports)
    {
        supportsGpuContinuation() = supports;
    }

    static bool probeSupportsGpuContinuation()
    {
        return supportsGpuContinuation().load();
    }

    static void noteProbe()
    {
        ++probes();
    }

    static void noteContinuation()
    {
        ++continuations();
    }

    static int probeCount()
    {
        return probes().load();
    }

    static int continuationCount()
    {
        return continuations().load();
    }

    static void setContinuationCapture(const CaptureFactory& factory)
    {
        std::lock_guard<std::mutex> lock(captureMutex());
        captureFactory() = factory;
    }

    static std::shared_ptr<void> makeContinuationCapture()
    {
        CaptureFactory factory;
        {
            std::lock_guard<std::mutex> lock(captureMutex());
            factory = captureFactory();
        }

        return factory ? factory() : std::shared_ptr<void>();
    }

private:
    static std::mutex& captureMutex()
    {
        static std::mutex m;

        return m;
    }

    static CaptureFactory& captureFactory()
    {
        static CaptureFactory f;

        return f;
    }

    static std::atomic<bool>& claimsGpu()
    {
        static std::atomic<bool> b(false);

        return b;
    }

    static std::atomic<bool>& supportsGpuContinuation()
    {
        static std::atomic<bool> b(false);

        return b;
    }

    static std::atomic<int>& probes()
    {
        static std::atomic<int> n(0);

        return n;
    }

    static std::atomic<int>& continuations()
    {
        static std::atomic<int> n(0);

        return n;
    }
};

class ContinuationTestEffectBase
    : public NativeEffectBase {
public:
    explicit ContinuationTestEffectBase(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE
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

    // Uncached so that every render the counters see is one the frame asked for, not one an earlier frame left.
    virtual bool shouldCacheOutput(bool /*isFrameVaryingOrAnimated*/,
                                   double /*time*/,
                                   ViewIdx /*view*/,
                                   int /*visitsCount*/) const OVERRIDE
    {
        return false;
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
    static RenderContextPtr renderContext(const ParallelRenderArgsPtr& frameArgs)
    {
        return frameArgs ? frameArgs->context : RenderContextPtr();
    }

    static StatusEnum fill(const RenderActionArgs& args,
                           float value)
    {
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
                        pixel[c] = value;
                    }
                }
            }
        }

        return eStatusOK;
    }

    static RenderContinuationRequest wholeOutputRequest(const NodePtr& node,
                                                        const RenderContextPtr& context,
                                                        double time,
                                                        ViewIdx view)
    {
        RenderContinuationRequest request;
        request.node = node;
        request.context = context;
        request.time = time;
        request.view = view;
        request.roi = RectD(0., 0., kCountingTestSize, kCountingTestSize);

        return request;
    }

    bool pullFirstPixel(const NodePtr& node,
                        const RenderContextPtr& context,
                        double time,
                        ViewIdx view,
                        unsigned int mipmapLevel,
                        float* value)
    {
        std::map<ImageLayerDesc, ImagePtr> planes;
        if ((pullRenderContinuationRequest(wholeOutputRequest(node, context, time, view), mipmapLevel, &planes) != eRenderRoIRetCodeOk) || planes.empty()) {
            return false;
        }
        const ImagePtr& image = planes.begin()->second;
        if (!image || (image->getBitDepth() != eImageBitDepthFloat) || image->getBounds().isNull()) {
            return false;
        }
        Image::ReadAccess access = image->getReadRights();
        const float* pixel = (const float*)access.pixelAt(image->getBounds().x1, image->getBounds().y1);
        if (!pixel) {
            return false;
        }
        *value = pixel[0];

        return true;
    }
};

class ContinuationSourceTestEffect
    : public ContinuationTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ContinuationSourceTestEffect(n);
    }

    explicit ContinuationSourceTestEffect(NodePtr n)
        : ContinuationTestEffectBase(n)
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
        value->setDefaultValue(0.);
        page->addKnob(value);
        _value = value;

        KnobIntPtr delayMs = createKnob<KnobInt>(std::string("Delay Ms"));
        delayMs->setName("delayMs");
        delayMs->setAnimationEnabled(false);
        delayMs->setDefaultValue(0);
        page->addKnob(delayMs);
        _delayMs = delayMs;

        KnobBoolPtr varying = createKnob<KnobBool>(std::string("Varying"));
        varying->setName("varying");
        varying->setAnimationEnabled(false);
        varying->setDefaultValue(false);
        page->addKnob(varying);
        _varying = varying;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        CountingTestRegistry::ConcurrentRender concurrent;
        CountingTestRegistry::noteRender(getNode().get());

        KnobIntPtr delayKnob = _delayMs.lock();
        if (delayKnob && (delayKnob->getValue() > 0)) {
            // Lets the renders of concurrent tasks overlap.
            QThread::msleep((unsigned long)delayKnob->getValue());
        }
        KnobDoublePtr valueKnob = _value.lock();
        double value = (valueKnob ? valueKnob->getValue() : 0.) + contextTestValue(renderContext(getFrameArgs()));
        KnobBoolPtr varyingKnob = _varying.lock();
        if (varyingKnob && varyingKnob->getValue()) {
            value *= CountingTestRegistry::renders(getNode());
        }

        return fill(args, (float)value);
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDContinuationSource,
        .label = "Test Continuation Source",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobDoubleWPtr _value;
    KnobIntWPtr _delayMs;
    KnobBoolWPtr _varying;
};

class ProbeTestEffect
    : public ContinuationTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ProbeTestEffect(n);
    }

    explicit ProbeTestEffect(NodePtr n)
        : ContinuationTestEffectBase(n)
    {
    }

    // Source is left out, so that only the render continuation requests it.
    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        FramesNeededMap frames;
        if (getInput(kControlInput)) {
            RangeD range;
            range.min = range.max = time;
            frames[kControlInput][view].push_back(range);
        }

        return frames;
    }

    virtual bool offersRenderContinuation() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    // Claims a GPU path only when a test asks, so that only its continuation keeps placement from taking it.
    virtual std::optional<OpKind> opKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (ContinuationTestRegistry::probeClaimsGpu()) {
            return OpKind::Custom;
        }
        return std::nullopt;
    }

    virtual bool canRenderOnGpu(const GpuQuery& /*query*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return ContinuationTestRegistry::probeClaimsGpu();
    }

    virtual bool supportsRenderContinuationOnGpu() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return ContinuationTestRegistry::probeSupportsGpuContinuation();
    }

    virtual bool getRenderContinuation(const RenderContinuationArgs& args,
                                       RenderContinuation* continuation) OVERRIDE FINAL
    {
        ContinuationTestRegistry::noteProbe();
        const NodePtr source = inputNode(kSourceInput);
        const NodePtr control = inputNode(kControlInput);
        unsigned int mask = 0;
        if (!source || !control || !readMask(control, args.time, args.view, args.mipmapLevel, &mask)) {
            return false;
        }
        KnobBoolPtr fullKnob = _full.lock();
        const bool full = fullKnob && fullKnob->getValue();
        for (int i = 0; i < kProbeTestContexts; ++i) {
            if (full || (mask & (1u << i))) {
                continuation->requests.push_back(wholeOutputRequest(source, sourceContext(args.context, i), args.time, args.view));
            }
        }
        KnobBoolPtr requestControlKnob = _requestControl.lock();
        if (requestControlKnob && requestControlKnob->getValue()) {
            continuation->requests.push_back(wholeOutputRequest(control, args.context, args.time, args.view));
        }
        const std::shared_ptr<void> capture = ContinuationTestRegistry::makeContinuationCapture();
        continuation->continuation = [capture]() {
            (void)capture;
            ContinuationTestRegistry::noteContinuation();

            return eStatusOK;
        };

        return true;
    }

private:
    static constexpr int kSourceInput = 0;
    static constexpr int kControlInput = 1;

    NodePtr inputNode(int inputNb) const
    {
        const EffectInstancePtr input = getInput(inputNb);

        return input ? input->getNode() : NodePtr();
    }

    static RenderContextPtr sourceContext(const RenderContextPtr& context,
                                          int i)
    {
        return RenderContext::Builder(context).set(contextTestVariable(), RenderContextValue(double(i))).build();
    }

    bool readMask(const NodePtr& control,
                  double time,
                  ViewIdx view,
                  unsigned int mipmapLevel,
                  unsigned int* mask)
    {
        float value = 0.f;
        if (!pullFirstPixel(control, renderContext(getFrameArgs()), time, view, mipmapLevel, &value) || (value < 0.f)) {
            return false;
        }
        *mask = (unsigned int)value;

        return true;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr full = createKnob<KnobBool>(std::string("Full"));
        full->setName("full");
        full->setAnimationEnabled(false);
        full->setDefaultValue(false);
        page->addKnob(full);
        _full = full;

        KnobBoolPtr requestControl = createKnob<KnobBool>(std::string("Request Control"));
        requestControl->setName("requestControl");
        requestControl->setAnimationEnabled(false);
        requestControl->setDefaultValue(false);
        page->addKnob(requestControl);
        _requestControl = requestControl;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const NodePtr source = inputNode(kSourceInput);
        const NodePtr control = inputNode(kControlInput);
        const unsigned int mipmapLevel = args.mappedScale.toMipmapLevel();
        unsigned int mask = 0;
        if (!source || !control || !readMask(control, args.time, args.view, mipmapLevel, &mask)) {
            return eStatusFailed;
        }
        const RenderContextPtr context = renderContext(getFrameArgs());
        float sum = 0.f;
        for (int i = 0; i < kProbeTestContexts; ++i) {
            if (!(mask & (1u << i))) {
                continue;
            }
            float value = 0.f;
            if (!pullFirstPixel(source, sourceContext(context, i), args.time, args.view, mipmapLevel, &value)) {
                return eStatusFailed;
            }
            sum += value;
        }

        return fill(args, sum);
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
        InputSpec { .label = "Control", .kind = eDataKindImage, .optional = true },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDProbe,
        .label = "Test Probe",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobBoolWPtr _full;
    KnobBoolWPtr _requestControl;
};

class RecursiveProbeTestEffect
    : public ContinuationTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new RecursiveProbeTestEffect(n);
    }

    explicit RecursiveProbeTestEffect(NodePtr n)
        : ContinuationTestEffectBase(n)
    {
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        reads.add(recursiveProbeVariable());

        return reads;
    }

    virtual bool offersRenderContinuation() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool shouldCacheOutput(bool /*isFrameVaryingOrAnimated*/,
                                   double /*time*/,
                                   ViewIdx /*view*/,
                                   int /*visitsCount*/) const OVERRIDE FINAL
    {
        KnobBoolPtr cacheKnob = _cacheOutput.lock();

        return cacheKnob && cacheKnob->getValue();
    }

    virtual bool getRenderContinuation(const RenderContinuationArgs& args,
                                       RenderContinuation* continuation) OVERRIDE FINAL
    {
        ContinuationTestRegistry::noteProbe();
        const double k = contextTestValue(args.context, recursiveProbeVariable().name);
        if (k >= depth()) {
            return false;
        }
        continuation->requests.push_back(wholeOutputRequest(getNode(), deeperContext(args.context, k), args.time, args.view));
        continuation->continuation = []() {
            ContinuationTestRegistry::noteContinuation();

            return eStatusOK;
        };

        return true;
    }

private:
    double depth() const
    {
        KnobIntPtr depthKnob = _depth.lock();

        return depthKnob ? depthKnob->getValue() : 0;
    }

    static RenderContextPtr deeperContext(const RenderContextPtr& context,
                                          double k)
    {
        return RenderContext::Builder(context).set(recursiveProbeVariable(), RenderContextValue(k + 1.)).build();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobIntPtr depthKnob = createKnob<KnobInt>(std::string("Depth"));
        depthKnob->setName("depth");
        depthKnob->setAnimationEnabled(false);
        depthKnob->setDefaultValue(EffectInstance::kMaxRenderContinuationDepth);
        page->addKnob(depthKnob);
        _depth = depthKnob;

        KnobBoolPtr cacheOutput = createKnob<KnobBool>(std::string("Cache Output"));
        cacheOutput->setName("cacheOutput");
        cacheOutput->setAnimationEnabled(false);
        cacheOutput->setDefaultValue(false);
        page->addKnob(cacheOutput);
        _cacheOutput = cacheOutput;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        CountingTestRegistry::noteRender(getNode().get());
        const RenderContextPtr context = renderContext(getFrameArgs());
        const double k = contextTestValue(context, recursiveProbeVariable().name);
        float value = (float)k;
        if (k < depth()) {
            float deeper = 0.f;
            if (!pullFirstPixel(getNode(), deeperContext(context, k), args.time, args.view, args.mappedScale.toMipmapLevel(), &deeper)) {
                return eStatusFailed;
            }
            value += deeper;
        }

        return fill(args, value);
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDRecursiveProbe,
        .label = "Test Recursive Probe",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobIntWPtr _depth;
    KnobBoolWPtr _cacheOutput;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_ContinuationTestEffects_h
