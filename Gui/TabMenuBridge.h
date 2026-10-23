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

#ifndef Gui_TabMenuBridge_h
#define Gui_TabMenuBridge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QString>

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The C++ side of the Python tab menu (tabtabtab_natron). The node graph calls launch(),
 * and the adapter triggers the chosen action through triggerAction(), by way of GuiApp.
 * GUI thread only; at most one menu is open at a time.
 **/
namespace TabMenuBridge {
/**
 * @brief Triggers action with a node creation context on the graph that last called launch(): nodes
 * the action creates with no collection go into that graph, and the first one with no position lands
 * at the cursor position recorded then. The context is cleared once the action returns. Without such
 * a graph the action is triggered as is.
 **/
void triggerAction(QAction* action);

/**
 * @brief Records the graph and the cursor's scene position, then opens the tab menu.
 * A Python failure is reported in the Script Editor and on stderr.
 **/
void launch(NodeGraph* graph);

/**
 * @brief Puts the bundled tabtabtab folder (<bin>/../Resources/tabtabtab) on sys.path and imports the adapter.
 **/
void bootstrap();
} // namespace TabMenuBridge

NATRON_NAMESPACE_EXIT

#endif // Gui_TabMenuBridge_h
