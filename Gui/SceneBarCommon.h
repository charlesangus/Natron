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

#ifndef Gui_SceneBarCommon_h
#define Gui_SceneBarCommon_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include <QString>

class QAbstractButton;
class QAction;
class QFrame;
class QToolButton;
class QWidget;

NATRON_NAMESPACE_ENTER

// What the 3D view's tool, display and top bars build their buttons from.
namespace SceneBar {

// The width of the tool and display bars, and the height of the top bar's buttons (logical px).
const int kBarSize = 28;

/**
 * @brief An icon button of a bar, named name, with the icon at iconPath under the icon resources
 * ("lucide/move.svg"). It never takes focus, so the 3D view keeps the keys.
 **/
QToolButton* makeIconButton(QWidget* parent, const char* name, const char* iconPath, bool checkable);

// A line between groups of buttons: horizontal in a vertical bar, vertical in a horizontal one.
QFrame* makeSeparator(QWidget* parent, bool inVerticalBar);

// Shows on button whether action is checked; a null action unchecks it.
void showActionChecked(QAbstractButton* button, const QAction* action);

// The tooltip text tip, with the shortcut line ToolTipActionShortcut fills in.
std::string tipWithShortcut(const QString& tip);

} // namespace SceneBar

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneBarCommon_h
