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

#ifndef Tests_OverlayImageTestEffect_h
#define Tests_OverlayImageTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "CountingTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlayImage.h"

#define kTestPluginIDOverlayImage "test.natron.built-in.OverlayImage"

NATRON_NAMESPACE_ENTER

constexpr float kOverlayImageTestMaskValue = 0.25f;

/**
 * @brief Fills its output with its "value" knob and caches it, and on an overlay image pass adds a
 * "mask" tile over the render window. Its "showMask" knob is its view knob, "fail" fails the render.
 * With "readPlane" on, the pass reads the output plane instead and adds a "planeMask" tile, full
 * where the plane's first channel is below "maskValue".
 * Counts renders in CountingTestRegistry.
 **/
class OverlayImageTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new OverlayImageTestEffect(n);
    }

    explicit OverlayImageTestEffect(NodePtr n)
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

    virtual bool shouldCacheOutput(bool /*isFrameVaryingOrAnimated*/,
                                   double /*time*/,
                                   ViewIdx /*view*/,
                                   int /*visitsCount*/) const OVERRIDE FINAL
    {
        return true;
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

    virtual bool isOverlayImageViewOn() const OVERRIDE FINAL
    {
        KnobBoolPtr knob = _showMask.lock();

        return knob && knob->getValue();
    }

    virtual void releaseOverlayImageResources() OVERRIDE FINAL
    {
        ++_resourceReleases;
    }

    virtual std::list<ImageLayerDesc> overlayImageSourcePlanes(double /*time*/,
                                                               ViewIdx /*view*/) OVERRIDE FINAL
    {
        KnobBoolPtr knob = _readPlane.lock();
        std::list<ImageLayerDesc> layers;

        if (knob && knob->getValue()) {
            layers.push_back(ImageLayerDesc::getRGBAComponents());
        }

        return layers;
    }

    virtual void addOverlayImagesFromPlanes(const std::map<ImageLayerDesc, ImagePtr>& planes,
                                            double /*time*/,
                                            ViewIdx /*view*/,
                                            const RectD& canonicalWindow) OVERRIDE FINAL
    {
        KnobDoublePtr maskValueKnob = _maskValue.lock();
        const float maskValue = maskValueKnob ? (float)maskValueKnob->getValue() : kOverlayImageTestMaskValue;

        for (const std::pair<const ImageLayerDesc, ImagePtr>& plane : planes) {
            const unsigned int level = plane.second->getMipmapLevel();
            const RectI window = canonicalWindow.toPixelEnclosing(level, plane.second->getPixelAspectRatio());
            Image::ReadAccess access(plane.second.get());
            std::vector<unsigned char> values((size_t)window.width() * (size_t)window.height(), 0);
            size_t i = 0;
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x, ++i) {
                    if (reinterpret_cast<const float*>(access.pixelAt(x, y))[0] < maskValue) {
                        values[i] = 255;
                    }
                }
            }
            addOverlayImageTile("planeMask", OverlayColor::fromRGBA(0.f, 1.f, 0.f, 0.5f), window, level, std::move(values));
        }
    }

    int overlayImageResourceReleases() const
    {
        return _resourceReleases;
    }

    /**
     * @brief Makes the next overlay image render wait, after reading its mask value and before
     * adding its tile, until releaseHold().
     **/
    void armHold()
    {
        std::lock_guard<std::mutex> lock(_holdMutex);
        _holdArmed = true;
        _holding = false;
        _holdReleased = false;
    }

    /**
     * @brief Waits for the armed render to reach the hold. Returns false if it does not in time.
     **/
    bool waitUntilHeld()
    {
        std::unique_lock<std::mutex> lock(_holdMutex);

        return _holdChanged.wait_for(lock, std::chrono::seconds(60), [this] { return _holding; });
    }

    void releaseHold()
    {
        std::lock_guard<std::mutex> lock(_holdMutex);
        _holdArmed = false;
        _holdReleased = true;
        _holdChanged.notify_all();
    }

private:
    // One-shot, so that a newer pass run while this one waits goes straight through.
    void holdIfArmed()
    {
        std::unique_lock<std::mutex> lock(_holdMutex);

        if (!_holdArmed) {
            return;
        }
        _holdArmed = false;
        _holding = true;
        _holdChanged.notify_all();
        _holdChanged.wait(lock, [this] { return _holdReleased; });
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobDoublePtr value = createKnob<KnobDouble>(std::string("Value"));

        value->setName("value");
        value->setAnimationEnabled(false);
        value->setDefaultValue(1.);
        page->addKnob(value);
        _value = value;

        KnobBoolPtr showMask = createKnob<KnobBool>(std::string("Show Mask"));
        showMask->setName("showMask");
        showMask->setAnimationEnabled(false);
        showMask->setDefaultValue(false);
        showMask->setEvaluateOnChange(false);
        showMask->setHashExempt(true);
        page->addKnob(showMask);
        _showMask = showMask;

        KnobDoublePtr maskValue = createKnob<KnobDouble>(std::string("Mask Value"));
        maskValue->setName("maskValue");
        maskValue->setAnimationEnabled(false);
        maskValue->setDefaultValue(kOverlayImageTestMaskValue);
        maskValue->setEvaluateOnChange(false);
        maskValue->setHashExempt(true);
        page->addKnob(maskValue);
        _maskValue = maskValue;

        KnobBoolPtr readPlane = createKnob<KnobBool>(std::string("Read Plane"));
        readPlane->setName("readPlane");
        readPlane->setAnimationEnabled(false);
        readPlane->setDefaultValue(false);
        readPlane->setEvaluateOnChange(false);
        readPlane->setHashExempt(true);
        page->addKnob(readPlane);
        _readPlane = readPlane;

        KnobBoolPtr fail = createKnob<KnobBool>(std::string("Fail"));
        fail->setName("fail");
        fail->setAnimationEnabled(false);
        fail->setDefaultValue(false);
        page->addKnob(fail);
        _fail = fail;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        CountingTestRegistry::noteRender(getNode().get());

        KnobBoolPtr failKnob = _fail.lock();
        if (failKnob && failKnob->getValue()) {
            return eStatusFailed;
        }

        KnobDoublePtr valueKnob = _value.lock();
        const float value = valueKnob ? (float)valueKnob->getValue() : 0.f;
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            it->second->fill(args.roi, value, value, value, value);
        }

        if (isOverlayImageRender()) {
            const size_t nPixels = (size_t)args.roi.width() * (size_t)args.roi.height();
            // Read before the hold: a held pass stands for one that read the view before the switch.
            KnobDoublePtr maskValueKnob = _maskValue.lock();
            const unsigned char maskByte = overlayImageCoverageByte(maskValueKnob ? (float)maskValueKnob->getValue() : kOverlayImageTestMaskValue);
            holdIfArmed();
            addOverlayImageTile("mask",
                                OverlayColor::fromRGBA(1.f, 0.f, 0.f, 0.5f),
                                args.roi,
                                args.mappedScale.toMipmapLevel(),
                                std::vector<unsigned char>(nPixels, maskByte));
        } else {
            // Without a pass this must do nothing, which the test reads off the node having no frame.
            addOverlayImageTile("mask", OverlayColor::fromNode(), args.roi, 0, std::vector<unsigned char>(1, 255));
        }

        return eStatusOK;
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDOverlayImage,
        .label = "Test Overlay Image",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobDoubleWPtr _value;
    KnobBoolWPtr _showMask;
    KnobDoubleWPtr _maskValue;
    KnobBoolWPtr _readPlane;
    KnobBoolWPtr _fail;
    std::atomic<int> _resourceReleases { 0 };
    std::mutex _holdMutex;
    std::condition_variable _holdChanged;
    bool _holdArmed = false;
    bool _holding = false;
    bool _holdReleased = false;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_OverlayImageTestEffect_h
