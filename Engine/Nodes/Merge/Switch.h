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

#ifndef Engine_Nodes_Merge_Switch_h
#define Engine_Nodes_Merge_Switch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_SWITCH "fr.natron.Switch"
#define PLUGIN_MAJOR_NATRON_SWITCH 1

#define kSwitchParamWhich "which"

#define kSwitchInputCount 64

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kSwitchInputs[] = {
    InputSpec { .label = "", .role = InputRole::Variadic, .optional = true, .count = kSwitchInputCount, .firstNumber = 0 },
};

/**
 * @brief Switch: outputs one of its 64 inputs "0".."63" unchanged, the one `which` names at the
 * frame being rendered. A `which` beyond the highest connected input chooses that input; with
 * no input connected, or the chosen one unconnected, the output is transparent black with an
 * empty region of definition.
 **/
struct SwitchOp {
    struct Params {
        int which;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_SWITCH,
        .label = "Switch",
        .description = "Outputs the input whose number \"which\" selects, unchanged. \"which\" can be animated "
                       "to switch between inputs over time; beyond the highest connected input it selects that "
                       "input. With no input connected the output is transparent black.",
        .grouping = PLUGIN_GROUP_MERGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_SWITCH,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Switch.png",
        .inputs = kSwitchInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::none(),
        .op = OpKind::Route,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    Route route(const Params& params,
                const RouteContext& context) const;

    KnobIntWPtr which;
};

static_assert(RouteOp<SwitchOp>);

class Switch
    : public ImageNode<SwitchOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Switch(node);
    }

    explicit Switch(NodePtr node);

    virtual ~Switch();

private:
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    // Sets the display range of `which` to the highest connected input.
    void updateRange();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_Switch_h
