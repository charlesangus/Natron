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

#ifndef Tests_WarpChainTestEffects_h
#define Tests_WarpChainTestEffects_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDWarpChainProvider "test.natron.built-in.WarpChainProvider"
#define kTestPluginIDWarpChainConsumer "test.natron.built-in.WarpChainConsumer"

NATRON_NAMESPACE_ENTER

const double kWarpChainTestCx = 256.;
const double kWarpChainTestCy = 256.;
const double kWarpChainTestShrink = 0.5;
const double kWarpChainTestBend = 2e-4;
const double kWarpChainTestSupport = 2.;

/**
 * @brief Pulls a point halfway towards the centre, bent along x by the square of its height
 * above the centre, so its pullback is not the bounding box of the window's corners.
 **/
class WarpChainTestMap
    : public InverseWarpMap {
public:
    explicit WarpChainTestMap(const RenderScale& scale)
        : InverseWarpMap(scale, 1.)
    {
    }

    virtual void mapRow(std::size_t n,
                        double* sx,
                        double* sy,
                        double* J) const OVERRIDE FINAL
    {
        const OfxPointD s = getScale().toOfxPointD();

        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(sx[i]) || !std::isfinite(sy[i])) {
                sx[i] = sy[i] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            const double dx = sx[i] / s.x - kWarpChainTestCx;
            const double dy = sy[i] / s.y - kWarpChainTestCy;
            sx[i] = s.x * (kWarpChainTestCx + kWarpChainTestShrink * dx + kWarpChainTestBend * dy * dy);
            sy[i] = s.y * (kWarpChainTestCy + kWarpChainTestShrink * dy);
            if (J) {
                const double dxy = 2. * kWarpChainTestBend * dy * s.x / s.y;
                double* j = &J[i * 4];
                const double j2 = j[2], j3 = j[3];
                j[0] = kWarpChainTestShrink * j[0] + dxy * j2;
                j[1] = kWarpChainTestShrink * j[1] + dxy * j3;
                j[2] = kWarpChainTestShrink * j2;
                j[3] = kWarpChainTestShrink * j3;
            }
        }
    }
};

/**
 * @brief Offers a WarpChainTestMap of its only input while offersWarp is set, and records the
 * windows it is asked to build maps over. Never renders: a concatenating consumer skips it.
 **/
class WarpChainTestProvider
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new WarpChainTestProvider(n);
    }

    explicit WarpChainTestProvider(NodePtr n)
        : NativeEffectBase(n)
        , offersWarp(true)
    {
    }

    std::atomic<bool> offersWarp;

    std::vector<RectD> getMapWindows() const
    {
        std::lock_guard<std::mutex> k(_windowsMutex);

        return _windows;
    }

    // The Disable knob is one of the default knobs only a settings panel creates, and the
    // disabled-provider test needs it: without it setNodeDisabled() does nothing.
    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual bool getCanInverseWarp() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool canOfferInverseWarp(double /*time*/,
                                     ViewIdx /*view*/,
                                     bool /*draftRender*/,
                                     int* inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *inputNb = 0;

        return offersWarp;
    }

    virtual InverseWarpMapConstPtr getInverseWarpMap(double /*time*/,
                                                     ViewIdx /*view*/,
                                                     const RenderScale& scale,
                                                     bool /*draftRender*/,
                                                     const RectD& window) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        {
            std::lock_guard<std::mutex> k(_windowsMutex);
            _windows.push_back(window);
        }

        return std::make_shared<WarpChainTestMap>(scale);
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
    virtual StatusEnum render(const RenderActionArgs& /*args*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eStatusFailed;
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDWarpChainProvider,
        .label = "Test Warp Chain Provider",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    mutable std::mutex _windowsMutex;
    std::vector<RectD> _windows;
};

/**
 * @brief Applies whatever its only input concatenates, taking inverse warp chains while
 * acceptsWarps is set, sampled with a filter of kWarpChainTestSupport pixels.
 **/
class WarpChainTestConsumer
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new WarpChainTestConsumer(n);
    }

    explicit WarpChainTestConsumer(NodePtr n)
        : NativeEffectBase(n)
        , acceptsWarps(true)
    {
    }

    std::atomic<bool> acceptsWarps;

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

    virtual bool getInputsHoldingTransform(std::list<int>* inputs) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        inputs->push_back(0);

        return true;
    }

    virtual bool acceptsInverseWarpChain(int inputNb,
                                         double /*time*/,
                                         ViewIdx /*view*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return (inputNb == 0) && acceptsWarps;
    }

    virtual double getInverseWarpChainFilterSupport(int /*inputNb*/,
                                                    double /*time*/,
                                                    ViewIdx /*view*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kWarpChainTestSupport;
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
    virtual StatusEnum render(const RenderActionArgs& /*args*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eStatusFailed;
    }

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDWarpChainConsumer,
        .label = "Test Warp Chain Consumer",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_WarpChainTestEffects_h
