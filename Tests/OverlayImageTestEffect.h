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

#include <list>
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

#define kTestPluginIDOverlayImage "test.natron.built-in.OverlayImage"

NATRON_NAMESPACE_ENTER

constexpr float kOverlayImageTestMaskValue = 0.25f;

/**
 * @brief Fills its output with its "value" knob and caches it, and on an overlay image pass adds a
 * "mask" tile over the render window. Its "showMask" knob is its view knob, "fail" fails the render.
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

        KnobBoolPtr showMask = createKnob<KnobBool>(std::string("Show Mask"));
        showMask->setName("showMask");
        showMask->setAnimationEnabled(false);
        showMask->setDefaultValue(false);
        showMask->setEvaluateOnChange(false);
        showMask->setHashExempt(true);
        page->addKnob(showMask);
        _showMask = showMask;

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
            addOverlayImageTile("mask",
                                OverlayColor::fromRGBA(1.f, 0.f, 0.f, 0.5f),
                                args.roi,
                                args.mappedScale.toMipmapLevel(),
                                std::vector<float>(nPixels, kOverlayImageTestMaskValue));
        } else {
            // Without a pass this must do nothing, which the test reads off the node having no frame.
            addOverlayImageTile("mask", OverlayColor::fromNode(), args.roi, 0, std::vector<float>(1, 1.f));
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
    KnobBoolWPtr _fail;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_OverlayImageTestEffect_h
