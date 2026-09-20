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

#ifndef NATRON_GUI_CHANNELCOLOR_H
#define NATRON_GUI_CHANNELCOLOR_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

/**
 * @brief The colour every channel toggle in the GUI uses for a channel named
 * R/G/B/A (or their long forms). Returns false, leaving *color untouched, for
 * any other name so the caller keeps its neutral look.
 **/
inline bool
getChannelColorFromName(const std::string& name,
                        QColor* color)
{
    if ((name == "R") || (name == "r") || (name == "red")) {
        color->setRgbF(0.851643, 0.196936, 0.196936);

        return true;
    }
    if ((name == "G") || (name == "g") || (name == "green")) {
        color->setRgbF(0, 0.654707, 0);

        return true;
    }
    if ((name == "B") || (name == "b") || (name == "blue")) {
        color->setRgbF(0.345293, 0.345293, 1);

        return true;
    }
    if ((name == "A") || (name == "a") || (name == "alpha")) {
        color->setRgbF(0.398979, 0.398979, 0.398979);

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_CHANNELCOLOR_H
