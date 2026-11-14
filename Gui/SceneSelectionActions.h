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

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"
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
 * @brief selected, in the order the user selected it: the paths of order still selected, in that
 * order, then the newly selected ones in the order selected lists them. Selections are kept in
 * this order everywhere, so the current prim, the one selected last, is last.
 **/
QStringList keepSelectionOrder(const QStringList& order, const QStringList& selected);

/**
 * @brief The text a prim knob with hint holds for paths: the first path for a path knob, all of
 * them joined with spaces in their order otherwise. Every writer of a prim knob uses it.
 **/
std::string primPathsText(KnobString::PrimPathHintEnum hint, const QStringList& paths);

/**
 * @brief Why paths can't be set on the current node now, or empty when they can.
 **/
QString setOnNodeBlocker(Gui* gui, const QStringList& paths);

/**
 * @brief Writes paths into primKnob(currentNode(gui)) as one undo step: the first path
 * for a path knob, all of them joined with spaces in their order for a pattern knob.
 * When it can't, shows why in the status bar and returns false.
 **/
bool setOnCurrentNode(Gui* gui, const QStringList& paths);

/**
 * @brief Why a node can't be created after viewed now, or empty when it can.
 **/
QString createBlocker(Gui* gui, const NodePtr& viewed);

/**
 * @brief Opens the tab menu listing the node types that take a scene and prims. The node chosen
 * reads viewed through its scene input and takes the place of viewed in viewed's first scene
 * output only, the other outputs staying on viewed, with its prim knob set to paths, as one undo
 * step. The knob holds paths before the node first evaluates, as setOnCurrentNode() writes them,
 * and is empty when paths is. A USDXformPrim is created in wrap mode, wrapping the first path,
 * unless paths is empty.
 **/
void createFromSelection(Gui* gui, const NodePtr& viewed, const QStringList& paths);

/**
 * @brief Why viewer's draw states can't be baked into a node now, or empty when they can.
 **/
QString bakeBlocker(ViewerTab* viewer);

/**
 * @brief Writes every draw state viewer's scene view sets into a USDExpandHierarchy, one rule
 * naming each path literally, and clears them from the view, as one undo step. The node is the
 * viewed node when that is a USDExpandHierarchy, else one directly below it, else a new one that
 * takes the viewed node's place in every input the viewed node fed; the viewer then views it.
 * Descendants' rules come before their ancestors', so that, as in the view, a box or hidden prim
 * keeps the states below it without drawing them. When it can't, shows why in the status bar
 * and returns false.
 **/
bool bakeToNode(Gui* gui, ViewerTab* viewer);

/**
 * @brief Shows text in gui's status bar for a few seconds.
 **/
void showHint(Gui* gui, const QString& text);
} // namespace SceneSelectionActions

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneSelectionActions_h
