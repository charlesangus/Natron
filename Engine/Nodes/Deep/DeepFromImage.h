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

#ifndef Engine_Nodes_Deep_DeepFromImage_h
#define Engine_Nodes_Deep_DeepFromImage_h

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

#define PLUGINID_NATRON_DEEPFROMIMAGE "fr.natron.DeepFromImage"

#define kDeepFromImageParamChannels "channels"
#define kDeepFromImageParamZChannel "zChannel"

NATRON_NAMESPACE_ENTER

/**
 * @brief Turns an image into deep data: one point sample per pixel holding the pixel's value of
 * every channel the channels knob selects, under the deep channel names DeepLayers::channelName()
 * gives them. A is always written, taken from the Source's colour plane read as RGBA, because
 * deep alpha is structural; a pixel whose A is not positive gets no sample. The depth comes from
 * the zChannel channel of the optional Z input, or from the depth knob when nothing is
 * connected there.
 **/
class DeepFromImage
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepFromImage(node);
    }

    explicit DeepFromImage(NodePtr node)
        : NativeEffectBase(node)
        , _depth()
        , _channels()
        , _zChannel()
    {
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getDeepLayers(double time, ViewIdx view, std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

private:
    /**
     * @brief What a render at some (time, view) converts. hasColor is whether the Source presents
     * a colour plane at all; colorBits are the R/G/B bits written, bit 3 (A) always set; planes
     * are the other selected Source layers, each with the indices of its selected channels.
     * opaque is set when the Source has no alpha, whether or not it has colour storage, and A
     * then reads as 1.
     **/
    struct Selection {
        bool hasColor;
        bool opaque;
        std::bitset<4> colorBits;
        std::vector<std::pair<ImageLayerDesc, std::vector<int>>> planes;

        Selection()
            : hasColor(false)
            , opaque(false)
            , colorBits()
            , planes()
        {
        }
    };

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void filterPassThroughLayers(double time, ViewIdx view, std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    Selection resolveSelection(double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief The deep channel name of each written channel, in the order: colour bits R, G, B, A,
     * then each plane's selected channels. Z and ZBack never appear and no name repeats.
     * (*sources)[i] is the (plane index, channel index) the i-th name reads, plane -1 standing
     * for the colour plane read as RGBA.
     **/
    static std::vector<std::string> channelNames(const Selection& selection,
                                                 std::vector<std::pair<int, int>>* sources);

    ImagePtr renderInputPlane(int inputNb,
                              const ImageLayerDesc& layer,
                              const DeepRenderActionArgs& args,
                              bool* failed) WARN_UNUSED_RETURN;

    KnobDoubleWPtr _depth;
    KnobChannelSetWPtr _channels;
    KnobChannelSelectWPtr _zChannel;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepFromImage_h
