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

#ifndef NATRON_ENGINE_PYTHONUNDOCOMMANDS_H
#define NATRON_ENGINE_PYTHONUNDOCOMMANDS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/UndoRecorder.h"

NATRON_NAMESPACE_ENTER

/*
 * How the Python API makes its edits undoable. Each function and guard below does nothing unless
 * the application's recorder is recording (AppInstance::getRecordingUndoRecorder()), so scripts
 * run while loading a project, from hooks or without a window behave as before.
 */

/**
 * @brief Call before the Python API changes knob's values, animation, expression or links. The
 * first call for a knob in a step records the state the step started from; undoing the step
 * restores it with everything the step changed on the knob. Knobs of panels made by Python are
 * not recorded: those panels have no undo of their own and are not part of the project.
 * Returns false, recording nothing and with a Python RuntimeError set, if a running IsolatedJob
 * reads the knob and the calling thread is outside it: the caller then returns to Python without
 * changing the knob. Call with the GIL held.
 **/
bool recordPythonKnobEdit(const KnobIPtr& knob) WARN_UNUSED_RETURN;

/**
 * @brief Call once a node created from Python exists. Undo removes it, redo brings it back.
 **/
void recordPythonNodeCreated(const NodePtr& node);

/**
 * @brief Removes node the way deleting it in the node graph does, so that undo can bring it back
 * with its connections. Destroys it outright when nothing records.
 **/
void destroyNodeFromPython(const NodePtr& node, bool autoReconnect);

/**
 * @brief Records what a Python call changes on one input of a node.
 **/
class PythonInputUndoGuard {
public:
    PythonInputUndoGuard(const NodePtr& node,
                         int inputNb);

    ~PythonInputUndoGuard();

private:
    PythonInputUndoGuard(const PythonInputUndoGuard&) = delete;
    PythonInputUndoGuard& operator=(const PythonInputUndoGuard&) = delete;

    UndoRecorderPtr _recorder;
    NodePtr _node;
    int _inputNb;
    NodeWPtr _before;
};

/**
 * @brief Records what a Python call changes on a node's script name and label.
 **/
class PythonNodeNameUndoGuard {
public:
    explicit PythonNodeNameUndoGuard(const NodePtr& node);

    ~PythonNodeNameUndoGuard();

private:
    PythonNodeNameUndoGuard(const PythonNodeNameUndoGuard&) = delete;
    PythonNodeNameUndoGuard& operator=(const PythonNodeNameUndoGuard&) = delete;

    UndoRecorderPtr _recorder;
    NodePtr _node;
    std::string _scriptName;
    std::string _label;
};

struct PythonNodeLayout;

/**
 * @brief Records what a Python call changes on a node's position, size and colour.
 **/
class PythonNodeLayoutUndoGuard {
public:
    explicit PythonNodeLayoutUndoGuard(const NodePtr& node);

    ~PythonNodeLayoutUndoGuard();

private:
    PythonNodeLayoutUndoGuard(const PythonNodeLayoutUndoGuard&) = delete;
    PythonNodeLayoutUndoGuard& operator=(const PythonNodeLayoutUndoGuard&) = delete;

    UndoRecorderPtr _recorder;
    NodePtr _node;
    std::shared_ptr<PythonNodeLayout> _before;
};

struct PythonUserKnobs;

/**
 * @brief Records what a Python call changes on a node's user knobs and page order. Holders that
 * are not nodes are not recorded.
 **/
class PythonUserKnobsUndoGuard {
public:
    explicit PythonUserKnobsUndoGuard(KnobHolder* holder);

    ~PythonUserKnobsUndoGuard();

private:
    PythonUserKnobsUndoGuard(const PythonUserKnobsUndoGuard&) = delete;
    PythonUserKnobsUndoGuard& operator=(const PythonUserKnobsUndoGuard&) = delete;

    UndoRecorderPtr _recorder;
    NodePtr _node;
    std::shared_ptr<PythonUserKnobs> _before;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PYTHONUNDOCOMMANDS_H
