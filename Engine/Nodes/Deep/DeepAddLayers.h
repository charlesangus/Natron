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

#ifndef Engine_Nodes_Deep_DeepAddLayers_h
#define Engine_Nodes_Deep_DeepAddLayers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPADDLAYERS "fr.natron.DeepAddLayers"

#define kDeepAddLayersParamLayers "layers"

NATRON_NAMESPACE_ENTER

/**
 * @brief Brings project-registry layers into a deep stream as zero-filled channel buffers,
 * wherever the input lacks them; the sample table and every channel the input has stay shared
 * with the input, never detached or zeroed.
 *
 * A colour view adds only the R, G and B channels it names that the input lacks. Deep alpha is
 * structural, so it is always present and an alpha row adds nothing. A registry layer adds each
 * of its channels the input lacks, named as DeepLayers::channelName() names them.
 *
 * Rows resolve against the registry; whether the input lacks a channel is decided per frame,
 * from the input's present layers at the render's (time, view).
 **/
class DeepAddLayers
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepAddLayers(node);
    }

    explicit DeepAddLayers(NodePtr node);

    virtual ~DeepAddLayers();

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

    /**
     * @brief The channel names this node adds at (time, view) to a stream whose present layers
     * are present, in the order the rows name them.
     **/
    std::vector<std::string> computeAddedNames(double time, ViewIdx view, const std::list<ImageLayerDesc>& present) WARN_UNUSED_RETURN;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onChannelsSelectorRefreshed() OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::vector<std::string> addedNamesFor(const std::list<ImageLayerDesc>& registryPlanes, const std::set<std::string>& inputNames) const WARN_UNUSED_RETURN;

    /**
     * @brief The registry's storage planes, colour folded to RGBA, that the rows resolve against.
     **/
    void listRegistryPlanes(double time, ViewIdx view, std::list<ImageLayerDesc>* planes) const;

    /**
     * @brief Moves the node's hash when the rows now resolve to other registry layers.
     *
     * The registry is project state, not a knob, so registering or removing a layer would
     * otherwise leave cached deep renders of this node stale.
     **/
    void refreshForRegistryChange();

    std::string buildSubLabel();

    void refreshSubLabel();

    KnobChannelSetWPtr _layers;
    KnobStringWPtr _subLabel;
    std::string _registrySignature;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepAddLayers_h
