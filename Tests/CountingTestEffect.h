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

#ifndef Tests_CountingTestEffect_h
#define Tests_CountingTestEffect_h

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
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <QThread>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDCounting "test.natron.built-in.Counting"
#define kTestPluginIDCountingMerge "test.natron.built-in.CountingMerge"

NATRON_NAMESPACE_ENTER

const int kCountingTestSize = 32;

/**
 * @brief The render calls of every counting effect, keyed by node so that render clones count for their node.
 **/
class CountingTestRegistry {
public:
    struct Record {
        int renders = 0;
        std::set<Qt::HANDLE> threads;
    };

    typedef std::function<void(const Node* node)> RenderHook;

    /**
     * @brief Counts the render calls running at once for its lifetime.
     **/
    class ConcurrentRender {
    public:
        ConcurrentRender()
        {
            const int running = ++rendering();
            int seen = maxRendering().load();

            while ((running > seen) && !maxRendering().compare_exchange_weak(seen, running)) {
            }
        }

        ~ConcurrentRender()
        {
            --rendering();
        }

        ConcurrentRender(const ConcurrentRender&) = delete;
        ConcurrentRender& operator=(const ConcurrentRender&) = delete;
    };

    /**
     * @brief Leaves the count of the renders running untouched, so that a render in progress still balances it.
     **/
    static void reset()
    {
        std::lock_guard<std::mutex> k(mutex());

        records().clear();
        hook() = RenderHook();
        maxRendering() = 0;
    }

    /**
     * @brief The most render calls of counting effects that ran at once since the last reset.
     **/
    static int maxConcurrentRenders()
    {
        return maxRendering().load();
    }

    /**
     * @brief Called at the start of every render, outside the registry's lock.
     **/
    static void setRenderHook(const RenderHook& renderHook)
    {
        std::lock_guard<std::mutex> k(mutex());

        hook() = renderHook;
    }

    static void noteRender(const Node* node)
    {
        RenderHook renderHook;
        {
            std::lock_guard<std::mutex> k(mutex());
            Record& record = records()[node];
            ++record.renders;
            record.threads.insert(QThread::currentThreadId());
            renderHook = hook();
        }
        if (renderHook) {
            renderHook(node);
        }
    }

    static int renders(const NodePtr& node)
    {
        std::lock_guard<std::mutex> k(mutex());
        std::map<const Node*, Record>::const_iterator found = records().find(node.get());

        return found == records().end() ? 0 : found->second.renders;
    }

    static int totalRenders()
    {
        std::lock_guard<std::mutex> k(mutex());
        int total = 0;

        for (std::map<const Node*, Record>::const_iterator it = records().begin(); it != records().end(); ++it) {
            total += it->second.renders;
        }

        return total;
    }

    static bool renderedOnThread(Qt::HANDLE thread)
    {
        std::lock_guard<std::mutex> k(mutex());

        for (std::map<const Node*, Record>::const_iterator it = records().begin(); it != records().end(); ++it) {
            if (it->second.threads.count(thread)) {
                return true;
            }
        }

        return false;
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex m;

        return m;
    }

    static std::map<const Node*, Record>& records()
    {
        static std::map<const Node*, Record> r;

        return r;
    }

    static RenderHook& hook()
    {
        static RenderHook h;

        return h;
    }

    static std::atomic<int>& rendering()
    {
        static std::atomic<int> n(0);

        return n;
    }

    static std::atomic<int>& maxRendering()
    {
        static std::atomic<int> n(0);

        return n;
    }
};

/**
 * @brief Fills its output with a constant: its "value" knob plus the sum of what its connected inputs hold at the
 * corner of the render window. Never caches its output, so that every renderRoI on it reaches render(), fails
 * when its "fail" knob is set, and sleeps for its "delayMs" knob first, so that the renders of concurrent tasks
 * overlap.
 **/
class CountingTestEffectBase
    : public NativeEffectBase {
public:
    explicit CountingTestEffectBase(NodePtr n)
        : NativeEffectBase(n)
        , _value()
        , _fail()
        , _delayMs()
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
        // Not eRenderSafetyFullySafeFrame: host frame threading would split one task over several render calls.
        return eRenderSafetyFullySafe;
    }

    virtual bool shouldCacheOutput(bool /*isFrameVaryingOrAnimated*/,
                                   double /*time*/,
                                   ViewIdx /*view*/,
                                   int /*visitsCount*/) const OVERRIDE FINAL
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

        KnobBoolPtr fail = createKnob<KnobBool>(std::string("Fail"));
        fail->setName("fail");
        fail->setAnimationEnabled(false);
        fail->setDefaultValue(false);
        page->addKnob(fail);
        _fail = fail;

        KnobIntPtr delayMs = createKnob<KnobInt>(std::string("Delay"));
        delayMs->setName("delayMs");
        delayMs->setAnimationEnabled(false);
        delayMs->setDefaultValue(0);
        page->addKnob(delayMs);
        _delayMs = delayMs;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        CountingTestRegistry::ConcurrentRender concurrent;
        CountingTestRegistry::noteRender(getNode().get());

        KnobIntPtr delayKnob = _delayMs.lock();
        if (delayKnob && (delayKnob->getValue() > 0)) {
            QThread::msleep((unsigned long)delayKnob->getValue());
        }

        KnobBoolPtr failKnob = _fail.lock();
        if (failKnob && failKnob->getValue()) {
            return eStatusFailed;
        }

        KnobDoublePtr valueKnob = _value.lock();
        float value = valueKnob ? (float)valueKnob->getValue() : 0.f;
        for (int i = 0; i < getNInputs(); ++i) {
            if (!getInput(i)) {
                continue;
            }
            RectI inputRoI;
            ImagePtr input = getImage(i, args.time, args.mappedScale, args.view, NULL, &ImageLayerDesc::getRGBAComponents(),
                                      false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            if (!input || (input->getBitDepth() != eImageBitDepthFloat) || !input->getBounds().contains(args.roi.x1, args.roi.y1)) {
                return eStatusFailed;
            }
            Image::ReadAccess access = input->getReadRights();
            const float* pixel = (const float*)access.pixelAt(args.roi.x1, args.roi.y1);
            if (!pixel) {
                return eStatusFailed;
            }
            value += pixel[0];
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
                        pixel[c] = value;
                    }
                }
            }
        }

        return eStatusOK;
    }

    KnobDoubleWPtr _value;
    KnobBoolWPtr _fail;
    KnobIntWPtr _delayMs;
};

class CountingTestEffect
    : public CountingTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new CountingTestEffect(n);
    }

    explicit CountingTestEffect(NodePtr n)
        : CountingTestEffectBase(n)
    {
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDCounting;
        desc.label = "Test Counting";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }
};

class CountingMergeTestEffect
    : public CountingTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new CountingMergeTestEffect(n);
    }

    explicit CountingMergeTestEffect(NodePtr n)
        : CountingTestEffectBase(n)
    {
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDCountingMerge;
        desc.label = "Test Counting Merge";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("A", true, eDataKindImage));
        desc.inputs.push_back(NativeInputDescription("B", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_CountingTestEffect_h
