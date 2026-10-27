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

#ifndef Gui_SceneSelectionActions_h
#define Gui_SceneSelectionActions_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What the 3D view and the Scene Graph panel do with the prims selected in a viewer: write
 * them into the prim knob of the current node, or create a node after the viewed node with them.
 **/
namespace SceneSelectionActions {
/**
 * @brief The viewer whose 3D selection the Scene Graph panel follows: the active viewer, else
 * the first one whose node is activated.
 **/
ViewerTab* selectionViewer(Gui* gui);

/**
 * @brief The current node: the activated node whose settings panel was opened last.
 **/
NodePtr currentNode(Gui* gui);

/**
 * @brief The knob of node that takes prims: its first prim pattern knob, else its first prim
 * path knob. Null when it has neither.
 **/
KnobStringPtr primKnob(const NodePtr& node);

/**
 * @brief Why paths can't be set on the current node now, or empty when they can.
 **/
QString setOnNodeBlocker(Gui* gui, const QStringList& paths);

/**
 * @brief Writes paths into primKnob(currentNode(gui)) as one undo step: the first path for a path
 * knob, all of them joined with spaces for a pattern knob. When it can't, shows why in the status
 * bar and returns false.
 **/
bool setOnCurrentNode(Gui* gui, const QStringList& paths);

/**
 * @brief Why a node can't be created after viewed now, or empty when it can.
 **/
QString createBlocker(Gui* gui, const NodePtr& viewed);

/**
 * @brief Opens the tab menu listing the node types that take a scene and prims. The node chosen
 * is inserted after viewed, taking its outputs, with its prim knob set to paths, as one undo step.
 **/
void createFromSelection(Gui* gui, const NodePtr& viewed, const QStringList& paths);

/**
 * @brief Shows text in gui's status bar for a few seconds.
 **/
void showHint(Gui* gui, const QString& text);
} // namespace SceneSelectionActions

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneSelectionActions_h
