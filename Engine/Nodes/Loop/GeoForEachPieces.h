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

#ifndef Engine_Nodes_Loop_GeoForEachPieces_h
#define Engine_Nodes_Loop_GeoForEachPieces_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

enum GeoPieceElementsEnum {
    eGeoPieceElementsPrimitives = 0,
    eGeoPieceElementsPoints,
};

enum GeoPieceMethodEnum {
    // Elements sharing a value of an integer or string attribute.
    eGeoPieceMethodAttribute = 0,
    // Elements connected through shared points.
    eGeoPieceMethodConnectivity,
    // Every element on its own.
    eGeoPieceMethodEachElement,
};

struct GeoPieceSettings {
    GeoPieceElementsEnum elements = eGeoPieceElementsPrimitives;
    GeoPieceMethodEnum method = eGeoPieceMethodConnectivity;
    std::string attribute;

    bool operator==(const GeoPieceSettings&) const = default;
};

/**
 * @brief A detail's elements split into pieces. Under eGeoPieceMethodAttribute, pieces are in
 * ascending value order (numeric for integers, lexical for strings) and named by their value;
 * otherwise they are in the order of each piece's first element and named by their position in
 * that order. Immutable once built.
 **/
class GeoPiecePartition {
public:
    std::size_t size() const
    {
        return _starts.empty() ? 0 : _starts.size() - 1;
    }

    bool hasStringValues() const
    {
        return _stringValued;
    }

    std::int64_t intValue(std::size_t piece) const
    {
        return (_stringValued || _indexValued) ? std::int64_t(piece) : _intValues[piece];
    }

    /** @brief Empty unless the pieces are named by a string attribute. */
    const std::string& stringValue(std::size_t piece) const;

    /** @brief The piece named @p value, or -1. */
    std::ptrdiff_t find(std::int64_t value) const;
    std::ptrdiff_t find(const std::string& value) const;

    /** @brief The storage offsets of the piece's elements, ascending. */
    std::span<const GeoOffset> elements(std::size_t piece) const
    {
        return std::span<const GeoOffset>(_elements.data() + _starts[piece], _starts[piece + 1] - _starts[piece]);
    }

    GeoPieceElementsEnum elementClass() const
    {
        return _elementClass;
    }

private:
    friend class GeoForEachPieces;

    GeoPieceElementsEnum _elementClass = eGeoPieceElementsPrimitives;
    bool _stringValued = false;
    // Pieces named by their position need no lookup table.
    bool _indexValued = false;
    std::vector<std::int64_t> _intValues;
    std::vector<std::string> _stringValues;
    std::unordered_map<std::int64_t, std::size_t> _byInt;
    std::unordered_map<std::string, std::size_t> _byString;
    std::vector<std::size_t> _starts;
    std::vector<GeoOffset> _elements;

    // An edge group of the detail split by piece: the group's edges both points of which a piece
    // keeps, so that extracting a piece visits its own edges only.
    struct EdgeGroupPieces {
        std::string name;
        // The edges of piece k are edges[starts[k], starts[k + 1]), ascending.
        std::vector<std::size_t> starts;
        std::vector<GeoEdge> edges;
    };
    std::vector<EdgeGroupPieces> _edgeGroups;
};

typedef std::shared_ptr<const GeoPiecePartition> GeoPiecePartitionConstPtr;

class GeoForEachPieces {
public:
    /**
     * @brief @p detail split by @p settings, or null with @p error set: a missing attribute,
     * or one that is neither an integer nor a string with one component. Recent partitions are
     * kept by the detail's content hash, so the nodes of one loop share them. Any thread.
     **/
    static GeoPiecePartitionConstPtr partition(const GeoDetail& detail,
                                               const GeoPieceSettings& settings,
                                               std::string* error);

    /**
     * @brief A detail with only the elements of one piece: a primitive piece keeps the points
     * its primitives use, a point piece the primitives all of whose points it holds. Attributes,
     * groups, edge groups between kept points and packed details come along.
     **/
    static GeoDetail extract(const GeoDetail& detail,
                             const GeoPiecePartition& partition,
                             std::size_t piece);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_GeoForEachPieces_h
