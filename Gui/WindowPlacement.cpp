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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "WindowPlacement.h"

#include <algorithm>

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

NATRON_NAMESPACE_ENTER

int
screenIndexAt(const std::vector<QRect>& geoms, QPoint p, int fallback)
{
    for (std::size_t i = 0; i < geoms.size(); ++i) {
        if (geoms[i].contains(p)) {
            return (int)i;
        }
    }

    return fallback;
}

QPoint
centredTopLeft(const QRect& avail, QSize size)
{
    const int dx = std::max(0, (avail.width() - size.width()) / 2);
    const int dy = std::max(0, (avail.height() - size.height()) / 2);

    return QPoint(avail.left() + dx, avail.top() + dy);
}

void
placeOnUserScreen(QWidget* w, const QWidget* anchor)
{
    QScreen* screen = 0;

    if (anchor) {
        const QWidget* win = anchor->window();
        if (win->isVisible() && !win->isMinimized()) {
            screen = win->screen();
        }
    }
    if (!screen) {
        screen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }
    w->move(centredTopLeft(screen->availableGeometry(), w->size()));
}

NATRON_NAMESPACE_EXIT
