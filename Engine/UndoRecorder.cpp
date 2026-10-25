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

#include "UndoRecorder.h"

#include <QCoreApplication>
#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/Knob.h"

NATRON_NAMESPACE_ENTER

UndoRecorderI::UndoRecorderI()
    : _suppressDepth(0)
    , _claimSerial(0)
    , _claimedKnobs()
{
}

UndoRecorderI::~UndoRecorderI()
{
}

void
UndoRecorderI::suppress()
{
    ++_suppressDepth;
}

void
UndoRecorderI::unsuppress()
{
    if (_suppressDepth > 0) {
        --_suppressDepth;
    }
}

bool
UndoRecorderI::isSuppressed() const
{
    return _suppressDepth > 0;
}

bool
UndoRecorderI::isRecording() const
{
    return !isSuppressed() && isCapturing();
}

bool
UndoRecorderI::claimKnob(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    std::uint64_t serial = getCaptureSerial();
    if (serial != _claimSerial) {
        _claimSerial = serial;
        _claimedKnobs.clear();
    }
    for (std::list<KnobIWPtr>::const_iterator it = _claimedKnobs.begin(); it != _claimedKnobs.end(); ++it) {
        if (it->lock() == knob) {
            return false;
        }
    }
    _claimedKnobs.push_back(knob);

    return true;
}

static bool
onMainThread()
{
    return qApp && (QThread::currentThread() == qApp->thread());
}

UndoStepGuard::UndoStepGuard(const AppInstancePtr& app,
                             const std::string& text)
    : _recorder()
{
    if (app && onMainThread()) {
        _recorder = app->getUndoRecorder();
    }
    if (_recorder) {
        _recorder->openStep(text);
    }
}

UndoStepGuard::~UndoStepGuard()
{
    if (_recorder) {
        _recorder->closeStep();
    }
}

UndoSuppressGuard::UndoSuppressGuard(const AppInstancePtr& app)
    : _recorder()
{
    if (app && onMainThread()) {
        _recorder = app->getUndoRecorder();
    }
    if (_recorder) {
        _recorder->suppress();
    }
}

UndoSuppressGuard::~UndoSuppressGuard()
{
    if (_recorder) {
        _recorder->unsuppress();
    }
}

UndoReplayGuard::UndoReplayGuard(const AppInstancePtr& app)
    : _app()
{
    if (app && onMainThread()) {
        _app = app;
        app->setReplayingUndo(true);
    }
}

UndoReplayGuard::~UndoReplayGuard()
{
    AppInstancePtr app = _app.lock();
    if (app) {
        app->setReplayingUndo(false);
    }
}

NATRON_NAMESPACE_EXIT
