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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ProjectPathVariableCommands.h"

#include "Engine/Knob.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

PathVariableCommand::PathVariableCommand(const ProjectPtr& project,
                                         const std::string& text)
    : UndoCommand(text)
    , _project(project)
    , _error()
    , _edits()
{
}

PathVariableCommand::~PathVariableCommand()
{
}

void
PathVariableCommand::addValueEdit(const KnobIPtr& knob,
                                  const std::string& oldValue,
                                  const std::string& newValue)
{
    if (!knob || !dynamic_cast<KnobStringBase*>(knob.get()) || (oldValue == newValue)) {
        return;
    }
    ValueEdit edit;
    edit.knob = UndoKnobRef(knob);
    edit.oldValue = oldValue;
    edit.newValue = newValue;
    _edits.push_back(edit);
}

KnobsVec
PathVariableCommand::getEditedKnobs() const
{
    KnobsVec ret;

    for (std::vector<ValueEdit>::const_iterator it = _edits.begin(); it != _edits.end(); ++it) {
        KnobIPtr knob = it->knob.lock();
        if (knob) {
            ret.push_back(knob);
        }
    }

    return ret;
}

void
PathVariableCommand::applyValues(bool newValues) const
{
    const std::size_t count = _edits.size();

    for (std::size_t i = 0; i < count; ++i) {
        const ValueEdit& edit = newValues ? _edits[i] : _edits[count - 1 - i];
        KnobStringBase* isString = dynamic_cast<KnobStringBase*>(edit.knob.lock().get());
        if (isString) {
            ignore_result(isString->setValue(newValues ? edit.newValue : edit.oldValue, ViewSpec::all(), 0, eValueChangedReasonUserEdited, nullptr));
        }
    }
}

AddPathVariableCommand::AddPathVariableCommand(const ProjectPtr& project,
                                               const std::string& name,
                                               const std::string& value)
    : PathVariableCommand(project, "Add Path Variable " + name)
    , _name(name)
    , _value(value)
    , _created(false)
{
}

AddPathVariableCommand::~AddPathVariableCommand()
{
}

void
AddPathVariableCommand::redo()
{
    ProjectPtr project = _project.lock();

    _error.clear();
    if (!project || _created) {
        return;
    }
    if (!project->addPathVariable(_name, _value, &_error)) {
        return;
    }
    _created = true;
    applyValues(true);
}

void
AddPathVariableCommand::undo()
{
    ProjectPtr project = _project.lock();

    if (!project || !_created) {
        return;
    }
    applyValues(false);
    project->removePathVariable(_name);
    _created = false;
}

RenamePathVariableCommand::RenamePathVariableCommand(const ProjectPtr& project,
                                                     const std::string& oldName,
                                                     const std::string& newName,
                                                     const std::string& oldLabel,
                                                     const std::string& newLabel)
    : PathVariableCommand(project, "Rename Path Variable " + oldName + " to " + newName)
    , _oldName(oldName)
    , _newName(newName)
    , _oldLabel(oldLabel)
    , _newLabel(newLabel)
    , _hasEdits(false)
{
    if (!project) {
        return;
    }
    const KnobsVec knobs = project->getKnobsSpellingPathVariable(oldName);
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        KnobStringBase* isString = dynamic_cast<KnobStringBase*>(it->get());
        if (!isString) {
            continue;
        }
        const std::string oldValue = isString->getValue();
        std::string newValue = oldValue;
        if (Project::renamePathVariableInValue(oldName, newName, &newValue)) {
            addValueEdit(*it, oldValue, newValue);
            _hasEdits = true;
        }
    }
}

RenamePathVariableCommand::~RenamePathVariableCommand()
{
}

bool
RenamePathVariableCommand::hasValueEdits() const
{
    return _hasEdits;
}

void
RenamePathVariableCommand::redo()
{
    ProjectPtr project = _project.lock();

    _error.clear();
    if (!project) {
        return;
    }
    if (project->getKnobByName(_oldName) && !project->renamePathVariable(_oldName, _newName, _newLabel)) {
        _error = "Could not rename the project path " + _oldName + " to " + _newName;

        return;
    }
    applyValues(true);
}

void
RenamePathVariableCommand::undo()
{
    ProjectPtr project = _project.lock();

    if (!project) {
        return;
    }
    project->renamePathVariable(_newName, _oldName, _oldLabel);
    applyValues(false);
}

NATRON_NAMESPACE_EXIT
