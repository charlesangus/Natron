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

#ifndef Engine_Geo_GeoOpenVDB_h
#define Engine_Geo_GeoOpenVDB_h

// OpenVDB and the oneTBB headers it pulls in use `foreach` and `emit` as ordinary identifiers,
// and Qt defines both as macros, so any translation unit that has already seen a Qt header fails
// to compile them. Include OpenVDB only through this header.
#pragma push_macro("foreach")
#pragma push_macro("emit")
#undef foreach
#undef emit
#include <openvdb/Grid.h>
#include <openvdb/io/io.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/LevelSetSphere.h>
#pragma pop_macro("emit")
#pragma pop_macro("foreach")

#endif // Engine_Geo_GeoOpenVDB_h
