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

#ifndef Serialization_SerializationFwd_h
#define Serialization_SerializationFwd_h

#include "Global/Macros.h"

// Global/Macros.h only defines the NATRON_NAMESPACE_* macros and compiler
// portability macros; it pulls in no Engine, Gui, Qt, Python or boost
// headers, so including it here does not break the plain-data boundary.
NATRON_NAMESPACE_ENTER
namespace Serialization {
struct CurveSerialization;
struct GroupKnobChildSerialization;
struct GroupKnobSerialization;
struct KeyFrameSerialization;
struct KnobDimensionSerialization;
struct KnobMasterSerialization;
struct KnobSerialization;
}
NATRON_NAMESPACE_EXIT

#endif // Serialization_SerializationFwd_h
