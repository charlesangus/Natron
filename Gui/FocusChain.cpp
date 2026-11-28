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

#include "FocusChain.h"

#include <vector>

NATRON_NAMESPACE_ENTER

namespace FocusChain {
QWidget*
end(QWidget* w)
{
    return w ? w->window()->previousInFocusChain() : 0;
}

QWidget*
focusableAtOrBefore(QWidget* w)
{
    if (!w) {
        return 0;
    }
    QWidget* window = w->window();
    for (QWidget* it = w; it && (it != window); it = it->previousInFocusChain()) {
        if (it->focusPolicy() != Qt::NoFocus) {
            return it;
        }
    }

    return 0;
}

QWidget*
placeAfter(QWidget* after,
           QWidget* mark,
           QWidget* scope)
{
    if (!mark || !scope) {
        return 0;
    }
    QWidget* window = mark->window();
    std::vector<QWidget*> joined;
    for (QWidget* w = mark->nextInFocusChain(); w && (w != window) && (w != mark); w = w->nextInFocusChain()) {
        if ((w->focusPolicy() != Qt::NoFocus) && scope->isAncestorOf(w)) {
            joined.push_back(w);
        }
    }
    if (joined.empty()) {
        return 0;
    }
    if (!after || (after->window() != window) || (after->focusPolicy() == Qt::NoFocus)) {
        return joined.back();
    }

    // Built in order with nothing made in between, the widgets already follow after.
    QWidget* next = after->nextInFocusChain();
    while (next && (next != window) && (next != after) && (next->focusPolicy() == Qt::NoFocus)) {
        next = next->nextInFocusChain();
    }
    if (next == joined.front()) {
        return joined.back();
    }

    // Every focusable widget is placed on its own: setTabOrder() carries a widget's descendants along only
    // when one of them is its focus proxy.
    for (QWidget* w : joined) {
        QWidget::setTabOrder(after, w);
        after = w;
    }

    return joined.back();
}
} // namespace FocusChain

NATRON_NAMESPACE_EXIT
