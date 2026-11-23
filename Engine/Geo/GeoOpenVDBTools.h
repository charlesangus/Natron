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

#ifndef Engine_Geo_GeoOpenVDBTools_h
#define Engine_Geo_GeoOpenVDBTools_h

#include "Engine/Geo/GeoOpenVDB.h"

// OpenVDB's tools, behind the same macro guard as GeoOpenVDB.h. Kept apart from it because these
// headers are heavy and only the translation units that convert grids need them.
#pragma push_macro("foreach")
#pragma push_macro("emit")
#undef foreach
#undef emit
#include <openvdb/math/Proximity.h>
#include <openvdb/tools/FastSweeping.h>
#include <openvdb/tools/GridTransformer.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <openvdb/tools/MeshToVolume.h>
#include <openvdb/tools/ParticlesToLevelSet.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <openvdb/tree/LeafManager.h>
#pragma pop_macro("emit")
#pragma pop_macro("foreach")

#endif // Engine_Geo_GeoOpenVDBTools_h
