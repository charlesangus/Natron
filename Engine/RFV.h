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
 * @brief Whether the name is a valid Python identifier: ASCII letters, digits and underscores,
 * not starting with a digit, and not a Python keyword.
 **/
bool isValidVariableName(const std::string& name);

/**
 * @brief Whether the name is an attribute of the app's Python App class (or its GUI subclass). A
 * project variable is declared as app.<name>, so such a name would hide the attribute.
 **/
bool isAppAttributeName(const AppInstancePtr& app, const std::string& name);

/**
 * @brief The variables the holder declares, in the order of its RFV groups' children. A child
 * whose type cannot be a variable, whose name is not a valid identifier, or whose name another
 * knob of the holder also has, is left out and described in errors.
 **/
std::vector<VariableDeclaration> getDeclaredVariables(const KnobHolder* holder, std::vector<std::string>* errors = nullptr);

/**
 * @brief Whether the knob is a variable of a node: a direct child of an RFV group of an effect,
 * such as a ReverseFlowVariableSet node, rather than of the project's.
 **/
bool isNodeVariable(const KnobIPtr& knob);

/**
 * @brief The variables the holder could take over from elsewhere in its project: the project's,
 * then those of every other node with an RFV group. A name declared more than once is listed
 * once, with its first declaration, and a name the holder already has a knob for is left out.
 **/
std::vector<VariableDeclaration> getExistingVariables(const KnobHolder* holder);

/**
 * @brief Adds to the holder's RFV group a variable named after one getExistingVariables() lists,
 * with the type, dimensions, label, hint and defaults of that declaration. It starts with the
 * declaration's value at the current time, without animation, and is not linked to it. This
 * records nothing in the undo history.
 * @returns the new variable, or null with error set when the name is not listed, the holder has
 * no RFV group, or the group refuses the knob.
 **/
KnobIPtr addExistingVariable(KnobHolder* holder, const std::string& name, std::string* error = nullptr);

/**
 * @brief Whether promoteToProject() would copy the knob into the project. When it would not,
 * reason says why: the knob is not a node's variable, or the project already has a parameter
 * with its name, or its name is an attribute of the App class.
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
