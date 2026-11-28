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

#ifndef Gui_FocusChain_h
#define Gui_FocusChain_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

/**
 * @brief Qt appends a new widget to its window's focus chain, so a widget made after its neighbours
 * would follow the last widget the window gained, which Tab and the focus hand-off on hide then reach.
 * These place such widgets where they would sit had they been made with their neighbours.
 **/
namespace FocusChain {
/**
 * @brief The current last widget of w's window focus chain: widgets made from now on join the chain after it.
 **/
QWidget* end(QWidget* w);

/**
 * @brief w if it takes focus, otherwise the nearest widget before it in its window's focus chain that does,
 * or null.
 **/
QWidget* focusableAtOrBefore(QWidget* w);

/**
 * @brief Moves the widgets inside scope that take focus and joined the focus chain after mark so they follow
 * after, in their order. Costs the number of widgets that joined after mark.
 * @returns The last of those widgets, or null when none takes focus. Nothing moves when after is null.
 **/
QWidget* placeAfter(QWidget* after, QWidget* mark, QWidget* scope);
}

NATRON_NAMESPACE_EXIT

#endif // Gui_FocusChain_h
