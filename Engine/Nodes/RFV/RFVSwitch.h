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

#ifndef Engine_Nodes_RFV_RFVSwitch_h
#define Engine_Nodes_RFV_RFVSwitch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_RFVSWITCH "RFVSwitch"
#define PLUGIN_MAJOR_NATRON_RFVSWITCH 1

#define kRFVSwitchParamVariable "variable"
#define kRFVSwitchParamVariableLabel "Variable"
#define kRFVSwitchParamVariableHint "The name of the Reverse-Flow Variable whose value chooses the input, as rfv reads it: " \
                                    "the value the nearest Set node below sets on the path being rendered, otherwise the project's."
#define kRFVSwitchParamPatterns "patterns"
#define kRFVSwitchParamPatternsLabel "Patterns"
#define kRFVSwitchParamPatternsHint "One pattern per line; line 1 chooses input 0, line 2 input 1, and so on. The first line "     \
                                    "matching the variable's whole value wins. A line starting with re: is an ECMAScript regular " \
                                    "expression; any other line is a glob, where * matches any characters, ? one, [abc] one of "   \
                                    "those and [!abc] one not among them. An empty line never matches. A value that is not a "     \
                                    "string is matched as Python's str() gives it; a list never matches."
#define kRFVSwitchParamIfNoMatch "ifNoMatch"
#define kRFVSwitchParamIfNoMatchLabel "If No Match"
#define kRFVSwitchParamIfNoMatchHint "What the node outputs when the variable is unset, is a list, or matches no line."
#define kRFVSwitchIfNoMatchError "Error"
#define kRFVSwitchIfNoMatchErrorHint "Fail the render, naming the variable and its value."
#define kRFVSwitchIfNoMatchFirstInput "First Input"
#define kRFVSwitchIfNoMatchFirstInputHint "Output input 0."

#define kRFVSwitchInputCount 16

NATRON_NAMESPACE_ENTER

enum RFVSwitchIfNoMatchEnum {
    eRFVSwitchIfNoMatchError = 0,
    eRFVSwitchIfNoMatchFirstInput,
};

inline constexpr InputSpec kRFVSwitchInputs[] = {
    InputSpec { .label = "", .kind = eDataKindPolymorphic, .role = InputRole::Variadic, .optional = true, .count = kRFVSwitchInputCount, .firstNumber = 0 },
};

/**
 * @brief Passes through, unchanged and whatever its data kind, the input whose pattern line first
 * matches the value of a Reverse-Flow Variable. The value is read when a render asks the node for
 * its route, under the context the render reached it with, so two paths through Set nodes setting
 * different values can choose different inputs from one RFVSwitch. Input i is shown while
 * Patterns has a line i or the input is connected; input 0 always is.
 **/
class RFVSwitch
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RFVSwitch(node);
    }

    explicit RFVSwitch(NodePtr node)
        : NativeEffectBase(node)
        , _variable()
        , _patterns()
        , _ifNoMatch()
    {
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RFVSWITCH,
        .label = "RFVSwitch",
        .description = "Passes through the input whose line in Patterns first matches the value of a Reverse-Flow "
                       "Variable on the path being rendered. Line 1 chooses input 0, line 2 input 1, and so on; a line "
                       "starting with re: is a regular expression, any other a glob, both matched against the whole "
                       "value. With no match the render fails, or input 0 is passed through.",
        .grouping = PLUGIN_GROUP_OTHER,
        .majorVersion = PLUGIN_MAJOR_NATRON_RFVSWITCH,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.switchPlugin.png",
        .outputKind = eDataKindPolymorphic,
        .inputs = kRFVSwitchInputs,
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

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    /**
     * @brief The input the first matching line chooses, at time under the context the calling
     * thread queries this node under (see EffectInstance::getCurrentRenderContext()). Black when
     * that input is not connected. With no match, input 0 or, under Error, None with an error
     * posted, which render() then fails.
     **/
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The variable, and what the project's variable of that name reads through rfv, so that
     * the node's context key holds the value it matches.
     **/
    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The content of the project's variables getExtraContextReads() names: their values are
     * the ones matched where no Set node sets them, which the context key leaves out.
     **/
    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;

    /**
     * @brief The variable as rfv gives it to an expression of this node at time and view, as
     * Python's str() gives it. False when it is unset or a list; error then receives why, if the
     * read failed.
     **/
    bool readVariable(const std::string& name,
                      double time,
                      ViewIdx view,
                      std::string* value,
                      std::string* error) const;

    // Shows the inputs Patterns has a line for, and those connected.
    void refreshInputsVisibility();

    KnobStringWPtr _variable;
    KnobStringWPtr _patterns;
    KnobChoiceWPtr _ifNoMatch;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_RFV_RFVSwitch_h
