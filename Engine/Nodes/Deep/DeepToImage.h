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

#ifndef Engine_Nodes_Deep_DeepToImage_h
#define Engine_Nodes_Deep_DeepToImage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPTOIMAGE "fr.natron.DeepToImage"

#define kDeepToImageParamChannels "channels"

NATRON_NAMESPACE_ENTER

/**
 * @brief Flattens the layers of its deep input that its channel set selects into float images:
 * every pixel's samples composited front-to-back, tidied first if the input does not declare
 * them tidy. This is the same flatten the Viewer applies to a deep stream it displays, but as a
 * node in the graph -- the one way to carry on with deep data as an image mid-graph, so that the
 * point where the depth information is thrown away is always visible.
 *
 * Every layer is composited with the deep alpha A, whether or not the rows select alpha. The
 * output colour plane is the narrowest colour layout covering the colour bits the rows select,
 * so rgb gives RGB and leaves A out of the image; with no colour row there is no colour plane.
 * A selected channel the deep stream lacks reads zero. None of the deep input's layers pass
 * through: the output holds only what is flattened here.
 **/
class DeepToImage
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepToImage(node);
    }

    explicit DeepToImage(NodePtr node)
        : NativeEffectBase(node)
        , _channels()
    {
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    // The input is deep: its layers are not flat images this node could hand on unrendered.
    virtual EffectInstance::PassThroughEnum isPassThroughForNonRenderedLayers() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return EffectInstance::ePassThroughBlockNonRenderedLayers;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    // Every render() call pulls the deep input over its own window, and the deep cache grows
    // bounds rather than tiling: splitting one frame's window across threads would have each of
    // them re-render the input over a different sub-window in turn.
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

private:
    /**
     * @brief What the rows select: the colour bits flattened from the deep R, G, B and A (none
     * when no colour row is selected), and each other layer with the deep channel name of each of
     * its channels, empty for a channel the rows leave out.
     **/
    struct Selection {
        std::bitset<4> colorBits;
        std::vector<std::pair<ImageLayerDesc, std::vector<std::string>>> layers;
    };

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The rows resolved against the deep input's present layers at (time, view).
     **/
    Selection resolveSelection(double time, ViewIdx view) WARN_UNUSED_RETURN;

    Selection selectionFor(const std::list<ImageLayerDesc>& present) const WARN_UNUSED_RETURN;

    KnobChannelSetWPtr _channels;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepToImage_h
