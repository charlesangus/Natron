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

#ifndef Engine_Nodes_Scene_USDMerge_h
#define Engine_Nodes_Scene_USDMerge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDMERGE "USDMerge"

#define kUSDMergeInputCount 64

#define kUSDMergeParamOpinionOrder "opinionOrder"

NATRON_NAMESPACE_ENTER

/**
 * @brief Unites the layer stacks of its scene inputs into one, authoring nothing itself. By
 * default inputs stack in input order, so a later input's layers are stronger than an earlier
 * one's and win where both author the same opinion; the Opinion Order knob reverses that. A
 * layer that arrives through more than one input (one source reaching the node along two
 * branches) is kept once, at its weakest position, so that it stays under every layer authored
 * downstream of it.
 **/
class USDMerge
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "", .kind = eDataKindScene, .role = InputRole::Variadic, .optional = true, .count = kUSDMergeInputCount, .firstNumber = 0 },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDMERGE,
        .label = "USDMerge",
        .description = "Unite the scenes of the inputs into one by stacking their layers. By default later inputs are "
                       "stronger: where two inputs author the same attribute, the higher-numbered input wins; Opinion "
                       "Order can make the lower-numbered input win instead. A layer that reaches the node through "
                       "more than one input is used once, under everything authored over it.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDMerge.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDMerge(node);
    }

    explicit USDMerge(NodePtr node);

    virtual ~USDMerge();

    /**
     * @brief The union of inputs, each one's layers stacked over the ones before, each layer kept
     * only at its first, weakest, position. The inputs are taken from the first to the last, so
     * that the last is strongest, or from the last to the first when lowerWins is set, so that
     * input 0 is strongest. A payload that holds every layer of the first non-empty input taken
     * is built on that input's stack, so a single input passes through as it is under both orders.
     **/
    static ScenePayload mergeStacks(const std::vector<ScenePayload>& inputs,
                                    bool lowerWins = false);

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobChoiceWPtr _opinionOrder;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDMerge_h
