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

#ifndef Tests_CacheStressTestEffect_h
#define Tests_CacheStressTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <mutex>
#include <utility>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDStaleMemo "test.natron.built-in.StaleMemo"
#define kTestPluginIDQueuedEdit "test.natron.built-in.QueuedEdit"

NATRON_NAMESPACE_ENTER

const int kStaleMemoTestSize = 32;

/**
 * @brief A source with a deliberate member-cache hole: the first render remembers the value knob
 * and every later render reuses it, until purgeCaches() forgets it. A render after clearAllCaches()
 * is therefore right, and a render through the cache after a value edit is stale.
 **/
class StaleMemoTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new StaleMemoTestEffect(n);
    }

    explicit StaleMemoTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _value()
        , _memoMutex()
        , _hasMemo(false)
        , _memo(0.f)
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
        rod->x2 = kStaleMemoTestSize;
        rod->y2 = kStaleMemoTestSize;

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

    virtual void purgeCaches() OVERRIDE FINAL
    {
        std::lock_guard<std::mutex> k(_memoMutex);

        _hasMemo = false;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDStaleMemo,
        .label = "Test Stale Memo",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobDoublePtr value = createKnob<KnobDouble>(std::string("Value"));

        value->setName("value");
        value->setDefaultValue(1.);
        page->addKnob(value);
        _value = value;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        float value;
        {
            std::lock_guard<std::mutex> k(_memoMutex);
            if (!_hasMemo) {
                KnobDoublePtr valueKnob = _value.lock();
                _memo = valueKnob ? (float)valueKnob->getValueAtTime(args.time) : 0.f;
                _hasMemo = true;
            }
            value = _memo;
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
    std::mutex _memoMutex;
    bool _hasMemo;
    float _memo;
};

/**
 * @brief A one-input filter whose knob values queue while a render holds its node, as in an
 * interactive app (a background app applies them at once), and whose value knob runs a test hook
 * from its change callback.
 **/
class QueuedEditTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new QueuedEditTestEffect(n);
    }

    explicit QueuedEditTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , valueChanged()
        , _value()
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool canSetValue() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NodePtr node = getNode();

        return !node || !node->isNodeRendering();
    }

    KnobDoublePtr valueKnob() const
    {
        return _value.lock();
    }

    // Main thread only.
    std::function<void()> valueChanged;

protected:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/) OVERRIDE FINAL
    {
        KnobDoublePtr value = _value.lock();

        if (value && (k == value.get()) && valueChanged) {
            valueChanged();
        }

        return false;
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDQueuedEdit,
        .label = "Test Queued Edit",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobDoublePtr value = createKnob<KnobDouble>(std::string("Value"));

        value->setName("value");
        value->setDefaultValue(1.);
        page->addKnob(value);
        _value = value;
    }

    KnobDoubleWPtr _value;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_CacheStressTestEffect_h
