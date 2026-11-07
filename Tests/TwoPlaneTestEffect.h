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

#ifndef Tests_TwoPlaneTestEffect_h
#define Tests_TwoPlaneTestEffect_h

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

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDTwoPlaneSource "test.natron.built-in.TwoPlaneSource"
#define kTestPluginIDTwoPlaneReader "test.natron.built-in.TwoPlaneReader"

NATRON_NAMESPACE_ENTER

const float kTwoPlaneFirstU = 1.5f;
const float kTwoPlaneFirstV = -2.5f;
const float kTwoPlaneSecondU = -3.5f;
const float kTwoPlaneSecondV = 4.5f;

inline ImageLayerDesc
makeTwoPlaneLayer(const std::string& layerID)
{
    std::vector<std::string> channels;
    channels.push_back("U");
    channels.push_back("V");

    return ImageLayerDesc(layerID, layerID, "UV", channels);
}

// The two layers have the same channels, and their IDs sort opposite to the order the effects list them in, so that
// taking one order for the other shows in the pixels.
inline const ImageLayerDesc&
twoPlaneFirstLayer()
{
    static const ImageLayerDesc layer = makeTwoPlaneLayer("zTwoPlaneFirst");

    return layer;
}

inline const ImageLayerDesc&
twoPlaneSecondLayer()
{
    static const ImageLayerDesc layer = makeTwoPlaneLayer("aTwoPlaneSecond");

    return layer;
}

class TwoPlaneTestEffectBase
    : public NativeEffectBase {
public:
    explicit TwoPlaneTestEffectBase(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
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
};

/**
 * @brief Fills twoPlaneFirstLayer() and twoPlaneSecondLayer() with different constants in one render call. Caches its
 * output, so that a second render can read both planes back from the node cache, and counts its render calls in
 * CountingTestRegistry.
 **/
class TwoPlaneSourceTestEffect
    : public TwoPlaneTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new TwoPlaneSourceTestEffect(n);
    }

    explicit TwoPlaneSourceTestEffect(NodePtr n)
        : TwoPlaneTestEffectBase(n)
    {
    }

    virtual bool shouldCacheOutput(bool /*isFrameVaryingOrAnimated*/,
                                   double /*time*/,
                                   ViewIdx /*view*/,
                                   int /*visitsCount*/) const OVERRIDE FINAL
    {
        return true;
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDTwoPlaneSource,
        .label = "Test Two Plane Source",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        std::list<ImageLayerDesc>& produced = (*comps)[-1];
        produced.push_back(twoPlaneFirstLayer());
        produced.push_back(twoPlaneSecondLayer());
        (*comps)[0];

        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = -1;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        CountingTestRegistry::noteRender(getNode().get());

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != 2)) {
                return eStatusFailed;
            }
            float u;
            float v;
            if (it->first.getLayerID() == twoPlaneFirstLayer().getLayerID()) {
                u = kTwoPlaneFirstU;
                v = kTwoPlaneFirstV;
            } else if (it->first.getLayerID() == twoPlaneSecondLayer().getLayerID()) {
                u = kTwoPlaneSecondU;
                v = kTwoPlaneSecondV;
            } else {
                return eStatusFailed;
            }
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    pixel[0] = u;
                    pixel[1] = v;
                }
            }
        }

        return eStatusOK;
    }
};

/**
 * @brief Requests both planes of its input and writes them to RGBA: R and G from twoPlaneFirstLayer(), B and A from
 * twoPlaneSecondLayer().
 **/
class TwoPlaneReaderTestEffect
    : public TwoPlaneTestEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new TwoPlaneReaderTestEffect(n);
    }

    explicit TwoPlaneReaderTestEffect(NodePtr n)
        : TwoPlaneTestEffectBase(n)
    {
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDTwoPlaneReader,
        .label = "Test Two Plane Reader",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        (*comps)[-1].push_back(ImageLayerDesc::getRGBAComponents());
        std::list<ImageLayerDesc>& needed = (*comps)[0];
        needed.push_back(twoPlaneFirstLayer());
        needed.push_back(twoPlaneSecondLayer());

        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = -1;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const ImageLayerDesc* const layers[2] = { &twoPlaneFirstLayer(), &twoPlaneSecondLayer() };
        float values[4];

        for (int i = 0; i < 2; ++i) {
            RectI inputRoI;
            ImagePtr input = getImage(0, args.time, args.mappedScale, args.view, NULL, layers[i],
                                      false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            if (!input || (input->getBitDepth() != eImageBitDepthFloat) || !input->getBounds().contains(args.roi.x1, args.roi.y1)) {
                return eStatusFailed;
            }
            Image::ReadAccess access = input->getReadRights();
            const float* pixel = (const float*)access.pixelAt(args.roi.x1, args.roi.y1);
            if (!pixel) {
                return eStatusFailed;
            }
            values[i * 2] = pixel[0];
            values[i * 2 + 1] = pixel[1];
        }

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != 4)) {
                return eStatusFailed;
            }
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < 4; ++c) {
                        pixel[c] = values[c];
                    }
                }
            }
        }

        return eStatusOK;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_TwoPlaneTestEffect_h
