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

#include <set>
#include <string>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_RFVSWITCH "RFVSwitch"
#define PLUGIN_MAJOR_NATRON_RFVSWITCH 1

#define kRFVSwitchParamVariable "variable"
#define kRFVSwitchParamVariableLabel "Variable"
#define kRFVSwitchParamVariableHint "The name of the Reverse-Flow Variable whose value chooses the input, as rfv reads it: " \
                                    "the value the nearest Set node below sets on the path being rendered, otherwise the project's."
#define kRFVSwitchParamMatchVariable "matchVariable"
#define kRFVSwitchParamMatchVariableLabel "Match Variable"
#define kRFVSwitchParamMatchVariableHint "Writes one pattern row per option of the project's Choice variable named in Variable, " \
                                         "in menu order, so input N follows option N, and overwrites the rows already there. A "  \
                                         "label holding *, ? or [, or starting with re:, is written as an escaped re: pattern. "  \
                                         "Inputs stay where they are. The rows do not follow later edits to the options: press "  \
                                         "again to rewrite them. At most 16 options are written."
#define kRFVSwitchParamPatternCount "patternCount"
#define kRFVSwitchParamPatternCountLabel "Pattern Count"
#define kRFVSwitchParamPatternCountHint "How many pattern rows the node shows."
#define kRFVSwitchParamPattern "input"
#define kRFVSwitchParamPatternLabel "Input"
#define kRFVSwitchParamPatternHint "The pattern choosing this input. The first row matching the variable's whole value wins. A "  \
                                   "pattern starting with re: is an ECMAScript regular expression; any other is a glob, where * " \
                                   "matches any characters, ? one, [abc] one of those and [!abc] one not among them. An empty "   \
                                   "pattern never matches. The value is matched as text: True or False, a whole number as its "   \
                                   "digits (5.0 as 5), any other number in its shortest exact form, and a list as its items "     \
                                   "joined by \", \"."
#define kRFVSwitchParamAddAfter "addAfter"
#define kRFVSwitchParamAddAfterLabel "Add After"
#define kRFVSwitchParamAddAfterHint "Inserts an empty pattern below this one. The rows and the inputs below it move down one."
#define kRFVSwitchParamRemove "remove"
#define kRFVSwitchParamRemoveLabel "Remove"
#define kRFVSwitchParamRemoveHint "Removes this pattern and disconnects its input. The rows and the inputs below it move up " \
                                  "one. The last remaining pattern is emptied instead."
#define kRFVSwitchParamIfNoMatch "ifNoMatch"
#define kRFVSwitchParamIfNoMatchLabel "If No Match"
#define kRFVSwitchParamIfNoMatchHint "What the node outputs when the variable is unset or matches no pattern."
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
 * @brief Passes through, unchanged and whatever its data kind, the input whose pattern first
 * matches the value of a Reverse-Flow Variable. The value is read when a render asks the node for
 * its route, under the context the render reached it with, so two paths through RFVSet nodes
 * setting different values can choose different inputs from one RFVSwitch. Pattern row i chooses
 * input i; input i is shown while row i exists or the input is connected, and input 0 always is.
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
        , _matchVariable()
        , _patternCount()
        , _patterns()
        , _addAfter()
        , _remove()
        , _ifNoMatch()
        , _messageMutex()
        , _messageSequence(0)
        , _messageErrors()
        , _messageWarnings()
        , _postedMessage()
        , _matchNote()
    {
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RFVSWITCH,
        .label = "RFVSwitch",
        .description = "Passes through the input whose pattern first matches the value of a Reverse-Flow "
                       "Variable on the path being rendered. The pattern in row Input 0 chooses input 0, Input 1 input 1, "
                       "and so on; a pattern starting with re: is a regular expression, any other a glob, both matched "
                       "against the whole value as text. With no match the render fails, or input 0 is passed through.",
        .grouping = PLUGIN_GROUP_RFV,
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
     * @brief The input the first matching pattern chooses, at time under the context the calling
     * thread queries this node under (see EffectInstance::getCurrentRenderContext()). Black when
     * that input is not connected. With no match, input 0 or, under Error, None, which render()
     * then fails. In a render other than a preview, reports its outcome (see reportOutcome()).
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

    struct Outcome {
        // The chosen input, or -1 when the render must fail.
        int input = -1;
        std::string error;
        std::string warning;
    };

    // What route() decides at time and view, under the calling thread's context.
    Outcome evaluate(double time, ViewIdx view) const;

    /**
     * @brief For an integer object read from the project variable name when that is a Choice: the
     * label of the option it indexes. False, leaving label alone, otherwise.
     **/
    bool choiceLabel(const std::string& name,
                     PyObject* object,
                     std::string* label) const;

    /**
     * @brief The variable as rfv gives it to an expression of this node at time and view, as the
     * text patterns are matched against (see kRFVSwitchParamPatternHint). False when it is unset;
     * error then receives why, if the read failed.
     **/
    bool readVariable(const std::string& name,
                      double time,
                      ViewIdx view,
                      std::string* value,
                      std::string* error) const;

    /**
     * @brief Shows what the latest render reaching this node decided, outside previews: the errors
     * of every path that failed in it, else the pattern warnings, else nothing. An older render's
     * outcome is ignored. Takes back only a message this node posted. Posting an error drops the
     * node's cached actions, so the paths that succeeded run route() again and report.
     **/
    void reportOutcome(const Outcome& outcome);

    // Takes back the message reportOutcome() or postMatchNote() posted, if the node still shows it.
    void clearPostedMessage();

    /**
     * @brief Writes one row per option of the project's Choice variable named in Variable, leaving
     * the inputs connected where they are. Posts why when it writes nothing, and how many options
     * it left out past the last row.
     **/
    void matchVariableOptions();

    // Posts note as a warning, shown until a knob or input changes unless a render posts an error.
    void postMatchNote(const std::string& note);

    int patternCount() const;

    // The patterns of the shown rows, at most one per input.
    std::vector<std::string> patterns() const;

    // Secret rows past the count, and the add buttons when every row is shown.
    void refreshRows();

    // Inserts an empty row after row, or removes it, moving the rows and inputs below.
    void insertRowAfter(int row);
    void removeRow(int row);

    // Reconnects input i to inputs[i], for every input that changes.
    void reconnectInputs(const std::vector<NodePtr>& inputs);

    // Shows the inputs with a pattern row, and those connected.
    void refreshInputsVisibility();

    KnobStringWPtr _variable;
    KnobButtonWPtr _matchVariable;
    KnobIntWPtr _patternCount;
    std::vector<KnobStringWPtr> _patterns;
    std::vector<KnobButtonWPtr> _addAfter;
    std::vector<KnobButtonWPtr> _remove;
    KnobChoiceWPtr _ifNoMatch;

    mutable QMutex _messageMutex;
    U64 _messageSequence;
    std::set<std::string> _messageErrors;
    std::set<std::string> _messageWarnings;
    std::string _postedMessage;
    // What the last Match Variable press reported, shown while no render has anything to say.
    std::string _matchNote;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_RFV_RFVSwitch_h
