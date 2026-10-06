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

#ifndef Engine_Nodes_Channel_RemoveLayers_h
#define Engine_Nodes_Channel_RemoveLayers_h

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

#define PLUGINID_NATRON_REMOVELAYERS "fr.natron.RemoveLayers"

#define kRemoveLayersParamOperation "operation"
#define kRemoveLayersParamChannels "channels"

NATRON_NAMESPACE_ENTER

/**
 * @brief Takes layers out of the stream (remove), or keeps only the chosen ones (keep).
 *
 * Non-colour layers are whole-layer selections forwarded or hidden through the pass-through
 * path; this node renders no pixels for them. The colour views (rgba, rgb, alpha, xy) alias the
 * one colour storage plane by channel bit, so a colour row narrows or drops that plane: with S
 * the input's colour bits and C the bits the rows select, the kept bits K are S & ~C (remove)
 * or S & C (keep). K == S passes the plane through, an empty K drops it, and any other K is
 * rendered as narrowestColorStorageCovering(K), each kept bit copied from the input's same bit
 * and every other bit of that layout written as 0.
 *
 * Rows resolve against the input's present layers at the render's (time, view); a row naming
 * a layer the input lacks selects nothing and posts no error.
 **/
class RemoveLayers
    : public NativeEffectBase {
public:
    enum OperationEnum {
        eOperationRemove = 0,
        eOperationKeep
    };

    struct ColorOutcome {
        enum KindEnum {
            eKindUnchanged,
            eKindDropped,
            eKindNarrowed
        };

        KindEnum kind;

        /**
         * @brief The input's colour storage plane; the None layout when it presents none.
         **/
        ImageLayerDesc inputStorage;

        /**
         * @brief The plane this node renders when kind is eKindNarrowed; otherwise unused.
         **/
        ImageLayerDesc outputStorage;

        std::bitset<4> keptBits;

        ColorOutcome()
            : kind(eKindUnchanged)
            , inputStorage()
            , outputStorage()
            , keptBits()
        {
        }
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RemoveLayers(node);
    }

    explicit RemoveLayers(NodePtr node);

    virtual ~RemoveLayers();

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    OperationEnum getOperation() const WARN_UNUSED_RETURN;

    /**
     * @brief What becomes of the colour plane at (time, view), by the rule in the class comment.
     **/
    ColorOutcome computeColorOutcome(double time, ViewIdx view) WARN_UNUSED_RETURN;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onChannelsSelectorRefreshed() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual void filterPassThroughLayers(double time,
                                         ViewIdx view,
                                         std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Input 0's present layers at (time, view), and the rows resolved against them.
     **/
    void resolveSelection(double time,
                          ViewIdx view,
                          std::list<ImageLayerDesc>* present,
                          std::vector<ResolvedLayer>* resolved);

    ColorOutcome colorOutcomeFor(const std::list<ImageLayerDesc>& present,
                                 const std::vector<ResolvedLayer>& resolved) const WARN_UNUSED_RETURN;

    /**
     * @brief The IDs of the present non-colour layers this node hides.
     **/
    std::set<std::string> droppedLayerIDs(const std::list<ImageLayerDesc>& present,
                                          const std::vector<ResolvedLayer>& resolved) const WARN_UNUSED_RETURN;

    std::string buildSubLabel();

    void refreshSubLabel();

    KnobChoiceWPtr _operation;
    KnobChannelSetWPtr _channels;
    KnobStringWPtr _subLabel;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Channel_RemoveLayers_h
