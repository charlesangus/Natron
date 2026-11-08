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

#ifndef Engine_Nodes_Geometry_ObjParser_h
#define Engine_Nodes_Geometry_ObjParser_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reads Wavefront OBJ geometry into a detail.
 *
 * Every "v" becomes a point, in file order, used or not; a "v" with six or seven values carries
 * a colour, stored as the point attribute Cd. Each "f" becomes a polygon of any vertex count
 * and each "l" a polyline. "vt" texture coordinates become the vertex attribute uv (u, v, w)
 * and "vn" normals the vertex attribute N, created only when a face references them. Indices
 * may be negative, counting back from the last element read. "g" names the primitive groups
 * the following faces join, with characters other than letters, digits and '_' replaced by '_';
 * "o" sets the primitive string attribute name. Lines may continue with a trailing backslash.
 * Materials, smoothing groups and free-form geometry are ignored.
 **/
class ObjParser {
public:
    /** @brief Appends the file's geometry to @p builder; false with @p error set when it cannot be read or is malformed. */
    static bool parseFile(const std::string& path, GeoDetailBuilder& builder, std::string* error);

    /** @brief As parseFile(), from the text of an OBJ file. */
    static bool parseString(std::string_view text, GeoDetailBuilder& builder, std::string* error);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_ObjParser_h
