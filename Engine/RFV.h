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

#ifndef NATRON_ENGINE_RFV_H
#define NATRON_ENGINE_RFV_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/UndoCommand.h"
#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reverse-Flow Variables (RFVs): declaration. A variable is a direct child of a group knob
 * flagged with KnobGroup::setAsRFVGroup(); its script name is the variable name. Only the
 * ReverseFlowVariableSet node and the project carry such a group. These are pure functions over
 * the knobs; they hold no render state.
 **/
namespace RFV {

struct VariableDeclaration {
    std::string name;
    KnobIPtr knob;
};

/**
 * @brief Whether the knob holds a value and can therefore be a variable: groups, pages, buttons
 * and separators cannot.
 **/
bool isVariableKnobType(const KnobIPtr& knob);

/**
 * @brief Whether the knob is one of its plug-in's own controls rather than a parameter a user,
 * a script or a saved project made: declared by the plug-in and not a user knob. Such a knob in
 * an RFV group, like the Set node's existing-variable drop-down, is not a variable.
 **/
bool isPluginControl(const KnobIPtr& knob);

/**
 * @brief Whether the RFV group admits the knob as a child: a plug-in control of the group's own
 * holder when the group too is declared by the plug-in, or any other knob that can be a variable
 * (see isVariableKnobType()).
 **/
bool isAdmittedInRFVGroup(const KnobGroup& group, const KnobIPtr& knob);

/**
 * @brief Whether the name is a valid Python identifier: ASCII letters, digits and underscores,
 * not starting with a digit, and not a Python keyword.
 **/
bool isValidVariableName(const std::string& name);

/**
 * @brief Whether the name is taken on the app's Python object: an attribute of its App class (or
 * its GUI subclass), the script name of a top-level node, or any other attribute the object
 * holds. A project variable is declared as app.<name>, so such a name would hide the attribute.
 * The name of a project parameter declared as app.<name> (see isDeclaredOnApp()) is only reported
 * when the App class has it.
 **/
bool isAppAttributeName(const AppInstancePtr& app, const std::string& name);

/**
 * @brief Whether a knob of the project is declared on the app's Python object as app.<name>: a
 * knob the project itself declares, or a project variable. A parameter a script made on the
 * project with the ordinary creators is not, until it is added as a variable.
 **/
bool isDeclaredOnApp(const KnobI& knob);

/**
 * @brief The variables the holder declares, in the order of its RFV groups and of their children.
 * A plug-in control is skipped. A child whose type cannot be a variable, whose name is not a
 * valid identifier, or whose name another knob of the holder also has, is left out and described
 * in errors.
 **/
std::vector<VariableDeclaration> getDeclaredVariables(const KnobHolder* holder, std::vector<std::string>* errors = nullptr);

/**
 * @brief Whether the knob is a variable of a node: a direct child of an RFV group of an effect,
 * such as a ReverseFlowVariableSet node, rather than of the project's, and not a plug-in control.
 **/
bool isNodeVariable(const KnobIPtr& knob);

/**
 * @brief Has every ReverseFlowVariableSet node of the project sort its variables between its
 * Variables and Global Variables groups by the names the project now declares (see
 * ReverseFlowVariableSet::refreshVariableGroups()), and rebuilds the user knobs of those whose
 * variables moved.
 **/
void refreshNodeVariableGroups(const Project& project);

/**
 * @brief The variables the holder could take over from elsewhere in its project: the project's,
 * then those of every other node with an RFV group. A name declared more than once is listed
 * once, with its first declaration, and a name the holder already has a knob for is left out.
 **/
std::vector<VariableDeclaration> getExistingVariables(const KnobHolder* holder);

/**
 * @brief Adds to the holder's first RFV group a variable named after one getExistingVariables()
 * lists, with the type, dimensions, label, hint and defaults of that declaration, then calls
 * recreateUserKnobs(), which has a ReverseFlowVariableSet node move it to its Global Variables
 * group when the project declares the name. It starts with the declaration's value at the
 * current time, without animation, and is not linked to it. This records nothing in the undo
 * history.
 * @returns the new variable, or null with error set when the name is not listed, the holder has
 * no RFV group, or the group refuses the knob.
 **/
KnobIPtr addExistingVariable(KnobHolder* holder, const std::string& name, std::string* error = nullptr);

/**
 * @brief Whether addAsVariable() would move the knob into the holder's RFV group. When it would
 * not, reason says why: the knob is missing or belongs to another holder, the holder has no RFV
 * group, the knob is not a user knob, holds no value (see isVariableKnobType()) or is already a
 * variable, its name is not a valid identifier or another knob of the holder has it, or, for the
 * project, the name is taken on the app object (see isAppAttributeName()).
 **/
bool canAddAsVariable(const KnobHolder* holder, const KnobIPtr& knob, std::string* reason = nullptr);

/**
 * @brief Moves a user knob of the holder, made anywhere on it, into the holder's first RFV group,
 * so that it becomes a variable under its script name, then calls recreateUserKnobs(). A user page
 * the move leaves empty is deleted. A project variable is declared as app.<name>. This records
 * nothing in the undo history.
 * @returns false with error set, moving nothing, when canAddAsVariable() is false or the group
 * refuses the knob.
 **/
bool addAsVariable(KnobHolder* holder, const KnobIPtr& knob, std::string* error = nullptr);

/**
 * @brief Whether promoteToProject() would copy the knob into the project. When it would not,
 * reason says why: the knob is not a node's variable, or the project already has a parameter
 * with its name, or its name is taken on the app object (see isAppAttributeName()).
 **/
bool canPromoteToProject(const KnobIPtr& knob, std::string* reason = nullptr);

/**
 * @brief Copies a node's variable into the project's RFV group, with the same type, dimensions,
 * label, hint, values and animation, and declares it as app.<name>. The copy is not linked to
 * the node's variable, which stays, editable, and goes on overriding the project's. This records
 * nothing in the undo history: PromoteToProjectCommand is the undoable form.
 * @returns the project's new variable, or null with error set when canPromoteToProject() is false
 * or the project's RFV group refuses the copy.
 **/
KnobIPtr promoteToProject(const KnobIPtr& knob, std::string* error = nullptr);

/**
 * @brief promoteToProject() as one undo step. The first redo() promotes the node's variable; undo()
 * deletes the project's copy and drops app.<name>; a later redo() recreates the copy as undo()
 * found it, with its definition, values and animation, even once the node's variable is gone.
 **/
class PromoteToProjectCommand
    : public UndoCommand {
public:
    explicit PromoteToProjectCommand(const KnobIPtr& variable);

    virtual ~PromoteToProjectCommand();

    virtual void redo() OVERRIDE FINAL;

    virtual void undo() OVERRIDE FINAL;

    /**
     * @brief The project's copy after a redo(), or null when it failed (see getError()) or after
     * an undo().
     **/
    KnobIPtr getProjectVariable() const;

    /**
     * @brief Why the last redo() or undo() did nothing, or empty.
     **/
    const std::string& getError() const
    {
        return _error;
    }

private:
    /**
     * @brief The project's copy: the knob redo() made while it is still in the project, otherwise
     * the child of the project's RFV group that now has its name.
     **/
    KnobIPtr findProjectVariable(const Project& project) const;

    KnobIWPtr _variable;
    ProjectWPtr _project;
    std::string _name;
    KnobIWPtr _copy;
    std::shared_ptr<Serialization::KnobSerialization> _removed;
    std::string _error;
};

} // namespace RFV

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_RFV_H
