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

#ifndef Tests_IdentityHopTestEffect_h
#define Tests_IdentityHopTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDFrameValue "test.natron.built-in.FrameValue"
#define kTestPluginIDTimeShiftIdentity "test.natron.built-in.TimeShiftIdentity"
#define kTestPluginIDViewSelfIdentity "test.natron.built-in.ViewSelfIdentity"
#define kTestPluginIDDeepFrameValue "test.natron.built-in.DeepFrameValue"
#define kTestPluginIDDeepTimeShiftIdentity "test.natron.built-in.DeepTimeShiftIdentity"
#define kTestPluginIDDeepViewSelfIdentity "test.natron.built-in.DeepViewSelfIdentity"

NATRON_NAMESPACE_ENTER

// Covers the TiledIdentityTestEffect window, so every identity tile lands inside the source.
const int kIdentityHopTestSize = 512;
const int kIdentityHopDeepSize = 4;

// What a frame value source holds at time and view: the pair it was rendered at is readable back.
inline float
identityHopFrameValue(double time,
                      ViewIdx view)
{
    return (float)time + 1000.f * (float)view.value();
}

/**
 * @brief Fills its output with identityHopFrameValue() of the time and view it renders at.
 * With selfIdentityAtOtherViews, it is an identity of itself at view 0 for every other view.
 **/
template <bool selfIdentityAtOtherViews>
class FrameValueTestEffectT
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new FrameValueTestEffectT(n);
    }

    explicit FrameValueTestEffectT(NodePtr n)
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
        rod->x2 = kIdentityHopTestSize;
        rod->y2 = kIdentityHopTestSize;

        return eStatusOK;
    }

    virtual bool isIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (!selfIdentityAtOtherViews || (view.value() == 0)) {
            return false;
        }
        *inputNb = -2;
        *inputTime = time;
        *inputView = ViewIdx(0);

        return true;
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
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const float value = identityHopFrameValue(args.time, args.view);

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

    static constexpr NodeSpec kNodeSpec = {
        .id = selfIdentityAtOtherViews ? kTestPluginIDViewSelfIdentity : kTestPluginIDFrameValue,
        .label = selfIdentityAtOtherViews ? "Test View Self Identity" : "Test Frame Value",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

typedef FrameValueTestEffectT<false> FrameValueTestEffect;
typedef FrameValueTestEffectT<true> ViewSelfIdentityTestEffect;

/**
 * @brief Always an identity of its input at time - 1, at the view it is asked for.
 **/
class TimeShiftIdentityTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new TimeShiftIdentityTestEffect(n);
    }

    explicit TimeShiftIdentityTestEffect(NodePtr n)
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

    virtual bool isIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *inputNb = 0;
        *inputTime = time - 1.;
        *inputView = view;

        return true;
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
    // An identity never renders, so a render reaching here means the hop was not taken.
    virtual StatusEnum render(const RenderActionArgs& /*args*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eStatusFailed;
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDTimeShiftIdentity,
        .label = "Test Time Shift Identity",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

/**
 * @brief A deep source of one opaque sample per pixel over a kIdentityHopDeepSize square, its colour
 * identityHopFrameValue() of the time and view it renders at. With selfIdentityAtOtherViews, it
 * is an identity of itself at view 0 for every other view.
 **/
template <bool selfIdentityAtOtherViews>
class DeepFrameValueTestEffectT
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DeepFrameValueTestEffectT(n);
    }

    explicit DeepFrameValueTestEffectT(NodePtr n)
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

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kIdentityHopDeepSize;
        rod->y2 = kIdentityHopDeepSize;

        return eStatusOK;
    }

    virtual void getDeepLayers(double /*time*/,
                               ViewIdx /*view*/,
                               std::list<ImageLayerDesc>* layers) OVERRIDE FINAL
    {
        DeepLayers::groupDeepChannels(channelNames(), layers);
    }

    virtual bool isIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (!selfIdentityAtOtherViews || (view.value() == 0)) {
            return false;
        }
        *inputNb = -2;
        *inputTime = time;
        *inputView = ViewIdx(0);

        return true;
    }

private:
    static std::vector<std::string> channelNames()
    {
        std::vector<std::string> names;

        names.push_back("R");
        names.push_back("G");
        names.push_back("B");
        names.push_back("A");

        return names;
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = selfIdentityAtOtherViews ? kTestPluginIDDeepViewSelfIdentity : kTestPluginIDDeepFrameValue,
        .label = selfIdentityAtOtherViews ? "Test Deep View Self Identity" : "Test Deep Frame Value",
        .outputKind = eDataKindDeep,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const float value = identityHopFrameValue(args.time, args.view);

        return renderDeepTwoPass(args, channelNames(), 3, [](int /*x*/, int /*y*/) -> U32 { return 1; }, [value](int /*x*/, int /*y*/, const MutableDeepPixelView& out) {
            for (int s = 0; s < out.numSamples; ++s) {
                out.z[s] = 1.f;
                out.zback[s] = 1.f;
                for (int c = 0; c < out.numChannels; ++c) {
                    out.channels[c][s] = (c == 3) ? 1.f : value;
                }
            } }, true);
    }
};

typedef DeepFrameValueTestEffectT<false> DeepFrameValueTestEffect;
typedef DeepFrameValueTestEffectT<true> DeepViewSelfIdentityTestEffect;

/**
 * @brief The deep TimeShiftIdentityTestEffect: always an identity of its deep input at time - 1.
 **/
class DeepTimeShiftIdentityTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DeepTimeShiftIdentityTestEffect(n);
    }

    explicit DeepTimeShiftIdentityTestEffect(NodePtr n)
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

    virtual bool isIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *inputNb = 0;
        *inputTime = time - 1.;
        *inputView = view;

        return true;
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindDeep, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDDeepTimeShiftIdentity,
        .label = "Test Deep Time Shift Identity",
        .outputKind = eDataKindDeep,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    // An identity never renders, so a render reaching here means the hop was not taken.
    virtual StatusEnum renderDeep(const DeepRenderActionArgs& /*args*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eStatusFailed;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_IdentityHopTestEffect_h
