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

#ifndef Gui_EdgeLabelMetrics_h
#define Gui_EdgeLabelMetrics_h

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QFont>
#include <QFontMetrics>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

// The size of an edge's input label, measured again only when its text or font changes: an edge
// lays its label out each time either end moves, which a drag or paste does for many edges at once.
class EdgeLabelMetrics {
public:
    // Returns whether the label had to be measured again.
    bool update(const QString& text,
                const QFont& font)
    {
        if (_valid && (text == _text) && (font == _font)) {
            return false;
        }
        const QFontMetrics fm(font);
        _text = text;
        _font = font;
        _width = fm.horizontalAdvance(text);
        _height = fm.height();
        _valid = true;

        return true;
    }

    int width() const
    {
        return _width;
    }

    int height() const
    {
        return _height;
    }

private:
    QString _text;
    QFont _font;
    int _width = 0;
    int _height = 0;
    bool _valid = false;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_EdgeLabelMetrics_h
