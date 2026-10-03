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

#ifndef Engine_Nodes_Deep_DeepExpression_h
#define Engine_Nodes_Deep_DeepExpression_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPEXPRESSION "fr.natron.DeepExpression"

#define kDeepExpressionParamLayer "layer"
#define kDeepExpressionParamExpressionPrefix "expression"
#define kDeepExpressionParamExpressionZ "expressionZ"
#define kDeepExpressionParamExpressionZBack "expressionZBack"
#define kDeepExpressionLayerSlotCount 4

NATRON_NAMESPACE_ENTER

/**
 * @brief Rewrites the channels of one layer of the deep input, chosen by the layer knob, and the
 * Z and ZBack depths, each from its own per-sample expression (see DeepExpressionEvaluator for
 * the language) reading the input's samples. The expression fields expression0..3 stand for the
 * layer's channels in order: a colour view's channels by bit (alpha's only field is A), any
 * other layer's under the deep names DeepLayers::channelName() gives them. A channel with an
 * empty expression is left as it is, sharing its storage with the input; so is the sample table.
 * A colour channel the input lacks is created; a non-colour layer the input lacks at a frame
 * fails that frame's render. Writing Z or ZBack moves samples, so the output is then no longer
 * known to be tidy.
 **/
class DeepExpression
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepExpression(node);
    }

    explicit DeepExpression(NodePtr node)
        : NativeEffectBase(node)
        , _layer()
        , _layerExpressions()
        , _expressionZ()
        , _expressionZBack()
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

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getDeepLayers(double time, ViewIdx view, std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

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

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief What the layer knob names at (time, view), against the Source's present layers.
     * Returns false when it names a non-colour layer the Source does not present; a colour view
     * always resolves. *names gets the deep channel name each expression field writes and
     * *labels the channel names the fields are labelled with, one entry per field in use.
     **/
    bool resolveLayerChannels(double time,
                              ViewIdx view,
                              std::vector<std::string>* names,
                              std::vector<std::string>* labels) WARN_UNUSED_RETURN;

    /**
     * @brief The expressions at (time, view), by output channel name, those left empty or blank
     * omitted. *layerMissing is set when the layer does not resolve while one of its fields holds
     * an expression; those fields are then omitted.
     **/
    void getExpressions(double time,
                        ViewIdx view,
                        std::vector<std::string>* channels,
                        std::vector<std::string>* expressions,
                        bool* layerMissing);

    void refreshExpressionLabels();

    KnobLayerSelectWPtr _layer;
    std::vector<KnobStringWPtr> _layerExpressions;
    KnobStringWPtr _expressionZ;
    KnobStringWPtr _expressionZBack;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepExpression_h
