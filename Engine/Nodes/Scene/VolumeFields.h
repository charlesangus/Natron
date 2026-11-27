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

#ifndef Engine_Nodes_Scene_VolumeFields_h
#define Engine_Nodes_Scene_VolumeFields_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief What an OpenVDB file holds, read from its grids' metadata without loading any voxels.
 **/
namespace VolumeFields {
struct Grid {
    std::string name;
    // OpenVDB's name for the grid's value type: float, double, vec3s, int32, bool and so on.
    std::string valueType;
    // The world-space box around the active voxels' extents, as the writer recorded it; false when
    // the grid is empty or the file records no bounds.
    bool hasBounds = false;
    double boundsMin[3] = { 0., 0., 0. };
    double boundsMax[3] = { 0., 0., 0. };
    double voxelSize[3] = { 0., 0., 0. };
};

/**
 * @brief The grids of the OpenVDB file at path, in file order. False with error set, and grids
 * empty, when the file is missing or is not a readable OpenVDB file; never throws.
 **/
bool readGrids(const std::string& path,
               std::vector<Grid>* grids,
               std::string* error) WARN_UNUSED_RETURN;
} // namespace VolumeFields

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_VolumeFields_h
