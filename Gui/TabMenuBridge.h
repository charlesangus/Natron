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

#include <functional>

#include <QString>

#include "Engine/Plugin.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The C++ side of the Python tab menu (tabtabtab_natron). The node graph calls launch(),
 * and the adapter calls back into nodeItemsJson() and createNode() through GuiApp.
 * GUI thread only; at most one menu is open at a time.
 **/
namespace TabMenuBridge {
/**
 * @brief Returns the create-node shortcut text for a plugin, or an empty string.
 * The first argument is the shortcut group (kShortcutGroupNodes + "/<grouping>..."), the second the plugin ID.
 **/
typedef std::function<QString(const QString&, const QString&)> ShortcutLookup;

/**
 * @brief A JSON array with one object per user-creatable plugin version in the registry:
 * {id, major, label, grouping[], icon, color "#rrggbb", shortcut}.
 **/
QString nodeItemsJson();

/**
 * @brief As nodeItemsJson(), over the given plugins and with the given shortcut lookup.
 **/
QString nodeItemsJson(const PluginsMap& plugins, const ShortcutLookup& shortcutFor);

/**
 * @brief Creates the plugin in the graph that last called launch(), at the cursor position recorded then,
 * connected after the selected node and with an undo entry. Returns false if there is no such graph,
 * the plugin is unknown, or creation fails.
 **/
bool createNode(const QString& pluginID, int majorVersion);

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
