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

#ifndef Engine_Nodes_Geometry_GeoPyBuffer_h
#define Engine_Nodes_Geometry_GeoPyBuffer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Python view of a detail being built, handed to a script as `geo`.
 *
 * Attributes are read as dense copies in element-number order, tombstones skipped, shaped
 * (N, tupleSize), or (N,) for a tuple size of 1. Numeric attributes and groups are arrays that
 * export the buffer protocol (numpy.frombuffer, memoryview) and also index like sequences;
 * string attributes are plain lists. Every value read or assigned is scattered back by commit(),
 * which keeps the pages whose content did not change shared with the input.
 *
 * Element counts never change while the script runs: added and removed points and primitives
 * are queued and applied by commit(), after the attribute writes, with new elements numbered
 * after every existing one.
 *
 * Every member must be called with the GIL held. On destruction the Python objects the script
 * may still reference stop working (they raise RuntimeError), and arrays it kept keep their own
 * copy of the data, so nothing Python holds can reach the builder afterwards.
 *
 * The script-facing API:
 *   geo.points, geo.prims, geo.vertices, geo.detail   element classes, each with:
 *       len(c), c["name"], c["name"] = values, del c["name"], "name" in c,
 *       c.attributes(), c.attribute_info("name"),
 *       c.groups(), c.group("name"), c.set_group("name", mask), c.remove_group("name")
 *   geo.prim_types(), geo.prim_vertex_counts(), geo.prim_vertices(), geo.prim_points(i),
 *   geo.vertex_points(), geo.vertex_prims()                   topology, read only
 *   geo.add_points(count_or_positions), geo.add_prim(points, type="polygon"),
 *   geo.add_prims(counts, points, type="polygon"),
 *   geo.remove_points(numbers), geo.remove_prims(numbers, delete_unused_points=False)
 **/
class GeoPySession {
public:
    /** @brief Throws std::runtime_error when the Python types cannot be created. */
    explicit GeoPySession(GeoDetailBuilder& builder);
    ~GeoPySession();

    GeoPySession(const GeoPySession&) = delete;
    GeoPySession& operator=(const GeoPySession&) = delete;

    /** @brief The `geo` object; a borrowed reference that lives as long as the session. */
    PyObject* geometry() const;

    /**
     * @brief Writes the script's values and queued element changes into the builder. False with
     * @p error set when a value no longer fits its attribute (a string list resized or holding
     * other than strings) or an element change is invalid; the builder may then be partly
     * written and must be discarded.
     **/
    bool commit(std::string* error);

    struct Impl;

private:
    std::unique_ptr<Impl> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoPyBuffer_h
