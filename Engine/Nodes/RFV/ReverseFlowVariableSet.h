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

#ifndef Engine_Nodes_RFV_ReverseFlowVariableSet_h
#define Engine_Nodes_RFV_ReverseFlowVariableSet_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_REVERSEFLOWVARIABLESET "ReverseFlowVariableSet"

#define kReverseFlowVariableSetParamPage "rfvPage"
#define kReverseFlowVariableSetParamPageLabel "Variables"
#define kReverseFlowVariableSetParamVariables "rfvVariables"
#define kReverseFlowVariableSetParamVariablesLabel "Variables"
#define kReverseFlowVariableSetParamGlobalPage "rfvGlobalPage"
#define kReverseFlowVariableSetParamGlobalPageLabel "Global Variables"
#define kReverseFlowVariableSetParamGlobalVariables "rfvGlobalVariables"
#define kReverseFlowVariableSetParamGlobalVariablesLabel "Global Variables"
#define kReverseFlowVariableSetParamExistingVariable "rfvExistingVariable"
#define kReverseFlowVariableSetParamExistingVariableLabel "Existing variable"
#define kReverseFlowVariableSetParamExistingVariableHint "Adds to this node's Variables group a variable declared on the project or on another "                      \
                                                         "ReverseFlowVariableSet node, with the same type, dimensions and default, starting from its current value. " \
                                                         "The new variable is not linked to the one it was taken from."
#define kReverseFlowVariableSetExistingVariablePrompt "Add existing variable..."

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kReverseFlowVariableSetInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindPolymorphic },
};

/**
 * @brief Declares Reverse-Flow Variables for everything upstream of it. The variables are the
 * children of its RFV group, a user group on a user page so that the variables the user adds are
 * saved with the node. The node forwards its input unchanged, whatever its data kind.
 **/
class ReverseFlowVariableSet
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ReverseFlowVariableSet(node);
    }

    explicit ReverseFlowVariableSet(NodePtr node)
        : NativeEffectBase(node)
        , _existingVariable()
    {
    }

    /**
     * @brief Lists in the existing-variable drop-down, after its prompt, the variables
     * RFV::getExistingVariables() finds for this node.
     **/
    void refreshExistingVariables();

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_REVERSEFLOWVARIABLESET,
        .label = "ReverseFlowVariableSet",
        .description = "Sets Reverse-Flow Variables for the nodes upstream of it. Each parameter in its "
                       "Variables group is one variable, named by the parameter's script name. "
                       "The node passes its input through unchanged.",
        .grouping = PLUGIN_GROUP_OTHER,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindPolymorphic,
        .inputs = kReverseFlowVariableSetInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                   double time) OVERRIDE FINAL;

    KnobChoiceWPtr _existingVariable;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_RFV_ReverseFlowVariableSet_h
