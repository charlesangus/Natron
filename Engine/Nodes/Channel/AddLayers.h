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

#ifndef Engine_Nodes_Channel_AddLayers_h
#define Engine_Nodes_Channel_AddLayers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_ADDLAYERS "fr.natron.AddLayers"

#define kAddLayersParamLayers "layers"

NATRON_NAMESPACE_ENTER

/**
 * @brief Brings project-registry layers into the stream, zero-filled, wherever the input lacks them.
 *
 * The rows name registry layers (a target knob), so a layer need not exist upstream. A chosen
 * non-colour layer the input already carries passes through untouched; one it lacks is produced
 * as zeros. The colour views (rgba, rgb, alpha, xy) alias the one colour storage plane by channel
 * bit: with S the input's colour bits and A the bits the rows select, A within S leaves the plane
 * alone, and otherwise the plane is widened to narrowestColorStorageCovering(S | A), each bit of
 * S copied from the input's same bit and every other bit written as 0.
 *
 * Non-colour layers are resolved per frame against the input's present layers at the render's
 * (time, view).
 **/
class AddLayers
    : public NativeEffectBase {
public:
    struct Outcome {
        /**
         * @brief The input's colour storage plane; the None layout when it presents none.
         **/
        ImageLayerDesc inputStorage;

        bool widensColor;

        /**
         * @brief The colour plane this node renders when widensColor; otherwise unused.
         **/
        ImageLayerDesc outputStorage;

        std::bitset<4> inputBits;

        /**
         * @brief The registry's non-colour layers the rows choose and the input lacks.
         **/
        std::list<ImageLayerDesc> addedLayers;

        Outcome()
            : inputStorage()
            , widensColor(false)
            , outputStorage()
            , inputBits()
            , addedLayers()
        {
        }

        bool producesNothing() const
        {
            return !widensColor && addedLayers.empty();
        }
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new AddLayers(node);
    }

    explicit AddLayers(NodePtr node);

    virtual ~AddLayers();

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

    /**
     * @brief What this node produces at (time, view), by the rule in the class comment.
     **/
    Outcome computeOutcome(double time, ViewIdx view) WARN_UNUSED_RETURN;

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

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The registry's storage planes, colour folded to RGBA, that the rows resolve against.
     **/
    void listRegistryPlanes(double time, ViewIdx view, std::list<ImageLayerDesc>* planes) const;

    std::string buildSubLabel();

    void refreshSubLabel();

    KnobChannelSetWPtr _layers;
    KnobStringWPtr _subLabel;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Channel_AddLayers_h
