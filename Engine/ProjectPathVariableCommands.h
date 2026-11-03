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

#ifndef NATRON_ENGINE_PROJECTPATHVARIABLECOMMANDS_H
#define NATRON_ENGINE_PROJECTPATHVARIABLECOMMANDS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoKnobRef.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An undoable edit of the project paths (see Project::addPathVariable()) together with the
 * values of the file paths and project paths it rewrites, undone and redone as one step. The
 * first redo() makes the edit.
 **/
class PathVariableCommand
    : public UndoCommand {
public:
    PathVariableCommand(const ProjectPtr& project,
                        const std::string& text);

    virtual ~PathVariableCommand();

    /**
     * @brief Sets knob, a string parameter, from oldValue to newValue on redo() and back on
     * undo(), after the edit of the project paths on redo() and before undoing it.
     **/
    void addValueEdit(const KnobIPtr& knob, const std::string& oldValue, const std::string& newValue);

    /**
     * @brief The knobs addValueEdit() was given that still exist.
     **/
    KnobsVec getEditedKnobs() const;

    /**
     * @brief Why the last redo() made no change, or empty when it made one.
     **/
    const std::string& getError() const
    {
        return _error;
    }

protected:
    void applyValues(bool newValues) const;

    ProjectWPtr _project;
    std::string _error;

private:
    struct ValueEdit {
        UndoKnobRef knob;
        std::string oldValue;
        std::string newValue;
    };

    std::vector<ValueEdit> _edits;
};

/**
 * @brief Creates a project path with Project::addPathVariable(); the value edits typically spell
 * a file path through it. undo() deletes it.
 **/
class AddPathVariableCommand
    : public PathVariableCommand {
public:
    AddPathVariableCommand(const ProjectPtr& project,
                           const std::string& name,
                           const std::string& value);

    virtual ~AddPathVariableCommand();

    virtual void redo() OVERRIDE FINAL;
    virtual void undo() OVERRIDE FINAL;

private:
    std::string _name;
    std::string _value;
    bool _created;
};

/**
 * @brief Renames a project path, and spells [newName] every file path of the project's nodes and
 * every project path that started with [oldName] when the command was made (see
 * Project::getKnobsSpellingPathVariable()). The variable may already have been renamed by the
 * time the command is made, as the parameter dialog does it: redo() then only rewrites the values.
 **/
class RenamePathVariableCommand
    : public PathVariableCommand {
public:
    RenamePathVariableCommand(const ProjectPtr& project,
                              const std::string& oldName,
                              const std::string& newName,
                              const std::string& oldLabel,
                              const std::string& newLabel);

    virtual ~RenamePathVariableCommand();

    /**
     * @brief Whether some value spells the variable, so that the rename has values to rewrite.
     **/
    bool hasValueEdits() const;

    virtual void redo() OVERRIDE FINAL;
    virtual void undo() OVERRIDE FINAL;

private:
    std::string _oldName;
    std::string _newName;
    std::string _oldLabel;
    std::string _newLabel;
    bool _hasEdits;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PROJECTPATHVARIABLECOMMANDS_H
