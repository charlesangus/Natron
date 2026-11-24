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

#ifndef Global_EdgeMode_h
#define Global_EdgeMode_h

// Kept free of Engine, Qt and Python headers so the NatronGpu library can include it.

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a node's output returns for pixels outside its region of definition.
 **/
enum class EdgeMode : unsigned char {
    Stretch,
    Black
};

NATRON_NAMESPACE_EXIT

#endif // Global_EdgeMode_h
