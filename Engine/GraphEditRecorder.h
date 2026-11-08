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

#ifndef Engine_GraphEditRecorder_h
#define Engine_GraphEditRecorder_h

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class GraphEditRecorder {
public:
    /**
     * @brief While one exists on a thread, the graph edits made on that thread are not recorded:
     * they replay operations that are already known.
     **/
    class Suppress {
    public:
        Suppress();
        ~Suppress();

        Suppress(const Suppress&) = delete;
        Suppress& operator=(const Suppress&) = delete;
    };

    static bool isSuppressed();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_GraphEditRecorder_h
