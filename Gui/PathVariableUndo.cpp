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

#include "PathVariableUndo.h"

#include <QUndoCommand>

#include "Engine/Project.h"
#include "Engine/ProjectPathVariableCommands.h"

#include "Gui/Gui.h"
#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_ENTER

namespace {
class PathVariableUndoCommand
    : public QUndoCommand {
public:
    explicit PathVariableUndoCommand(const std::shared_ptr<PathVariableCommand>& command)
        : QUndoCommand()
        , _command(command)
    {
        setText(QString::fromUtf8(command->getText().c_str()));
    }

    virtual ~PathVariableUndoCommand()
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        _command->redo();
        // A command that changed nothing must not leave an entry to undo.
        if (!_command->getError().empty()) {
            setObsolete(true);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        _command->undo();
    }

private:
    std::shared_ptr<PathVariableCommand> _command;
};
} // anon namespace

std::string
pushPathVariableCommand(Gui* gui,
                        const ProjectPtr& project,
                        const std::shared_ptr<PathVariableCommand>& command)
{
    if (!command) {
        return std::string();
    }
    UndoHistory* history = gui ? gui->getUndoHistory() : 0;
    if (!history || !project) {
        command->redo();

        return command->getError();
    }
    UndoScope scope(project.get());
    const KnobsVec knobs = command->getEditedKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        scope.touchKnob(*it);
    }
    history->push(new PathVariableUndoCommand(command), scope);

    return command->getError();
}

NATRON_NAMESPACE_EXIT
