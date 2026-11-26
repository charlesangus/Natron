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

#ifndef Engine_Nodes_InputConnectSwitch_h
#define Engine_Nodes_InputConnectSwitch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief For an effect's onInputChanged(changedInput): when input watchedInput has just been
 * connected, sets knob to value as an edit of its own, recorded into the undo entry of the
 * connection when one is being recorded. Does nothing on a disconnection, while a project or a
 * node tree (a paste) is being created, where the saved value wins, or while an undo entry is
 * replayed, whose records restore the knob themselves.
 **/
inline void
switchChoiceOnInputConnected(EffectInstance& effect,
                             int changedInput,
                             int watchedInput,
                             const KnobChoicePtr& knob,
                             int value)
{
    if ((changedInput != watchedInput) || !knob || !effect.getInput(watchedInput)) {
        return;
    }
    const AppInstancePtr app = effect.getApp();
    if (!app || app->isCreatingNodeTree() || app->isReplayingUndo()) {
        return;
    }
    const ProjectPtr project = app->getProject();
    if (!project || project->isLoadingProject() || project->isProjectClosing()) {
        return;
    }
    if (knob->getValue() == value) {
        return;
    }
    recordKnobEdit(knob);
    knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonPluginEdited, nullptr);
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_InputConnectSwitch_h
