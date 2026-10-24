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

#ifndef Gui_WindowPlacement_h
#define Gui_WindowPlacement_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include <QPoint>
#include <QRect>
#include <QSize>

class QWidget;

NATRON_NAMESPACE_ENTER

/**
 * @brief Index of the first rectangle in `geoms` that contains `p`, or `fallback` if none does.
 **/
int screenIndexAt(const std::vector<QRect>& geoms, QPoint p, int fallback);

/**
 * @brief Top-left that centres a window of `size` in `avail`. A window larger than `avail` in
 * either dimension is pinned to `avail`'s edge on that axis; `avail`'s origin is kept, so screens
 * at negative coordinates work.
 **/
QPoint centredTopLeft(const QRect& avail, QSize size);

/**
 * @brief Moves `w` to the centre of the screen the user is working on: the screen of `anchor`'s
 * window if it is visible and not minimised, else the screen under the cursor, else the primary screen.
 **/
void placeOnUserScreen(QWidget* w, const QWidget* anchor);

NATRON_NAMESPACE_EXIT

#endif // Gui_WindowPlacement_h
