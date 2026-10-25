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

#ifndef NATRON_ENGINE_UNDORECORDER_H
#define NATRON_ENGINE_UNDORECORDER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <list>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The nodes and knobs whose state an undo record changes. The record goes obsolete once
 * any of them is destroyed.
 **/
struct UndoTouched {
    std::list<NodePtr> nodes;
    std::list<KnobIPtr> knobs;
};

/**
 * @brief Where edits that have no undo command of their own, those made from Python, are
 * recorded into the project window's undo history.
 *
 * A step groups everything recorded until it closes into one undo entry; steps opened inside
 * an open step add to it. Records are also taken while the history executes a pushed command,
 * so that the edits its callbacks make undo with it. Outside both nothing is recorded. An
 * application without a window has no recorder, see AppInstance::getUndoRecorder().
 **/
class UndoRecorderI {
public:
    UndoRecorderI();

    virtual ~UndoRecorderI();

    virtual void openStep(const std::string& text) = 0;

    virtual void closeStep() = 0;

    /**
     * @brief Whether a step or a pushed command is open to take records.
     **/
    virtual bool isCapturing() const = 0;

    /**
     * @brief Identifies the outermost open step or pushed command; it changes whenever a new
     * one opens.
     **/
    virtual std::uint64_t getCaptureSerial() const = 0;

    /**
     * @brief Records command, whose edit has already been made: undo and redo run it later.
     **/
    virtual void record(const UndoCommandPtr& command, const UndoTouched& touched) = 0;

    void suppress();

    void unsuppress();

    bool isSuppressed() const;

    /**
     * @brief Whether a record would be kept now: something is capturing and nothing suppresses
     * recording. AppInstance::getRecordingUndoRecorder() adds the conditions of the application.
     **/
    bool isRecording() const;

    /**
     * @brief True the first time knob is claimed in the outermost open step or pushed command.
     * The caller then records the knob's state before its first edit, which covers every later
     * edit of that knob until the step closes.
     **/
    bool claimKnob(const KnobIPtr& knob);

private:
    int _suppressDepth;
    std::uint64_t _claimSerial;
    std::list<KnobIWPtr> _claimedKnobs;
};

typedef std::shared_ptr<UndoRecorderI> UndoRecorderPtr;

/**
 * @brief Opens a step on the application's recorder for the guard's lifetime. Does nothing
 * without a recorder or off the main thread.
 **/
class UndoStepGuard {
public:
    UndoStepGuard(const AppInstancePtr& app,
                  const std::string& text);

    ~UndoStepGuard();

private:
    UndoStepGuard(const UndoStepGuard&) = delete;
    UndoStepGuard& operator=(const UndoStepGuard&) = delete;

    UndoRecorderPtr _recorder;
};

/**
 * @brief Keeps the application's recorder from recording for the guard's lifetime: Python
 * that runs on its own (project hooks, render callbacks, startup scripts) is not a user edit.
 **/
class UndoSuppressGuard {
public:
    explicit UndoSuppressGuard(const AppInstancePtr& app);

    ~UndoSuppressGuard();

private:
    UndoSuppressGuard(const UndoSuppressGuard&) = delete;
    UndoSuppressGuard& operator=(const UndoSuppressGuard&) = delete;

    UndoRecorderPtr _recorder;
};

/**
 * @brief Marks the application as replaying recorded edits for the guard's lifetime, which
 * keeps Python callbacks from running: the records reproduce what they did.
 **/
class UndoReplayGuard {
public:
    explicit UndoReplayGuard(const AppInstancePtr& app);

    ~UndoReplayGuard();

private:
    UndoReplayGuard(const UndoReplayGuard&) = delete;
    UndoReplayGuard& operator=(const UndoReplayGuard&) = delete;

    AppInstanceWPtr _app;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_UNDORECORDER_H
