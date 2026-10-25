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

#include "PythonUndoRecorder.h"

#include <list>

#include <QString>
#include <QUndoCommand>

#include "Engine/AppInstance.h"
#include "Engine/UndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {

// Past the ids of GlobalDefines.h, which only GUI commands use.
const int kPythonUndoCommandCompressionID = 25;

class PythonUndoCommandAdapter
    : public QUndoCommand {
public:
    PythonUndoCommandAdapter(const UndoCommandPtr& command,
                             const AppInstanceWPtr& app)
        : QUndoCommand()
        , _command(command)
        , _app(app)
    {
        setText(QString::fromUtf8(command->getText().c_str()));
    }

    virtual int id() const OVERRIDE FINAL
    {
        return kPythonUndoCommandCompressionID;
    }

    virtual bool mergeWith(const QUndoCommand* other) OVERRIDE FINAL
    {
        const PythonUndoCommandAdapter* isAdapter = dynamic_cast<const PythonUndoCommandAdapter*>(other);

        return isAdapter && _command->mergeWith(isAdapter->_command);
    }

    // The edit was made before it was recorded, so the history never redoes it on its own. The
    // Python callbacks it fired are recorded too and must not run again.
    virtual void undo() OVERRIDE FINAL
    {
        UndoReplayGuard replaying(_app.lock());
        _command->undo();
    }

    virtual void redo() OVERRIDE FINAL
    {
        UndoReplayGuard replaying(_app.lock());
        _command->redo();
    }

private:
    UndoCommandPtr _command;
    AppInstanceWPtr _app;
};

} // anon namespace

PythonUndoRecorder::PythonUndoRecorder(UndoHistory* history,
                                       const AppInstancePtr& app)
    : UndoRecorderI()
    , _history(history)
    , _app(app)
{
}

PythonUndoRecorder::~PythonUndoRecorder()
{
}

void
PythonUndoRecorder::openStep(const std::string& text)
{
    if (_history) {
        _history->openStep(QString::fromUtf8(text.c_str()));
    }
}

void
PythonUndoRecorder::closeStep()
{
    if (_history) {
        _history->closeStep();
    }
}

bool
PythonUndoRecorder::isCapturing() const
{
    return _history && _history->isCapturing();
}

std::uint64_t
PythonUndoRecorder::getCaptureSerial() const
{
    return _history ? _history->getCaptureSerial() : 0;
}

void
PythonUndoRecorder::record(const UndoCommandPtr& command,
                           const UndoTouched& touched)
{
    if (!command || !_history) {
        return;
    }
    UndoScope scope;
    for (std::list<NodePtr>::const_iterator it = touched.nodes.begin(); it != touched.nodes.end(); ++it) {
        scope.touchNode(*it);
    }
    for (std::list<KnobIPtr>::const_iterator it = touched.knobs.begin(); it != touched.knobs.end(); ++it) {
        scope.touchKnob(*it);
    }
    // Like an edit made in a panel, a record of one node's edit is offered by that node's panel.
    scope.ownByOnlyTouched();
    _history->recordApplied(new PythonUndoCommandAdapter(command, _app), scope);
}

NATRON_NAMESPACE_EXIT
