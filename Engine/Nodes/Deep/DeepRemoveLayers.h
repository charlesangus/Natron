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

#ifndef Engine_Nodes_Deep_DeepRemoveLayers_h
#define Engine_Nodes_Deep_DeepRemoveLayers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPREMOVELAYERS "DeepRemoveLayers"

#define kDeepRemoveLayersParamOperation "operation"
#define kDeepRemoveLayersParamChannels "channels"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kDeepRemoveLayersInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindDeep },
};

/**
 * @brief Takes layers out of a deep stream (remove), or keeps only the chosen ones (keep), by
 * dropping channel buffers by name; every kept buffer and the sample table stay shared with the
 * input.
 *
 * Deep alpha is structural, so the colour rows can only take away R, G and B: with S the input's
 * colour bits and C the R/G/B bits the rows select, the kept colour bits are (S & ~C) | A
 * (remove) or (S & C) | A (keep). A, Z and ZBack are always kept, so removing rgba and removing
 * rgb are the same, and removing alpha changes nothing.
 *
 * Rows resolve against the input's present layers at the render's (time, view); a row naming
 * a layer the input lacks selects nothing and posts no error.
 **/
class DeepRemoveLayers
    : public NativeEffectBase {
public:
    enum OperationEnum {
        eOperationRemove = 0,
        eOperationKeep
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepRemoveLayers(node);
    }

    explicit DeepRemoveLayers(NodePtr node);

    virtual ~DeepRemoveLayers();

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

    OperationEnum getOperation() const WARN_UNUSED_RETURN;

private:
    /**
     * @brief What the rows make of input 0's present layers at one (time, view).
     **/
    struct Selection {
        std::list<ImageLayerDesc> present;

        // The input's colour storage bits, empty when it presents no colour plane.
        std::bitset<4> inputColorBits;

        // The colour bits the output keeps; A is always among them.
        std::bitset<4> keptColorBits;

        // The bits the colour rows asked for, before A was put back.
        std::bitset<4> selectedColorBits;

        // The IDs of the present non-colour layers this node drops.
        std::set<std::string> droppedLayerIDs;

        // The IDs the non-colour rows resolved to.
        std::set<std::string> selectedLayerIDs;

        bool dropsNothing() const
        {
            return (keptColorBits == inputColorBits) && droppedLayerIDs.empty();
        }
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_DEEPREMOVELAYERS,
        .label = "DeepRemoveLayers",
        .description = "Remove the chosen layers from the deep stream, or keep only them. Every other layer passes "
                       "through unchanged. The colour views rgba, rgb and alpha share one colour plane, and deep "
                       "alpha is part of every sample: removing rgba or rgb drops R, G and B, removing alpha "
                       "changes nothing, and A is always kept. Z and ZBack are always kept. A layer the input "
                       "does not have is ignored.",
        .grouping = PLUGIN_GROUP_DEEP,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindDeep,
        .inputs = kDeepRemoveLayersInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onChannelsSelectorRefreshed() OVERRIDE FINAL;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    Selection computeSelection(double time, ViewIdx view) WARN_UNUSED_RETURN;

    std::string buildSubLabel();

    void refreshSubLabel();

    KnobChoiceWPtr _operation;
    KnobChannelSetWPtr _channels;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepRemoveLayers_h
