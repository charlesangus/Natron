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

#ifndef Engine_Geo_GeoKernelTopology_h
#define Engine_Geo_GeoKernelTopology_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

enum class GeoKernelEditKind : std::uint8_t {
    eAddPoint, // position
    eAddPointFrom, // a: point to copy
    eAddPrimitive, // primitiveType, points
    eAddVertex, // a: primitive, b: point
    eRemovePoint, // a: point
    eRemovePrimitive, // a: primitive, b: also remove its unused points
    eSetPrimitiveVertex, // a: primitive, b: vertex within it, c: point
    eSetGroup, // a: group, b: element, c: value, d: GeoKernelGroupMode
};

enum class GeoKernelGroupMode : std::int32_t {
    eSet,
    eAdd,
    eRemove,
    eToggle,
};

/**
 * @brief One queued geometry edit. Element numbers are dense numbers of the input geometry, or the
 * provisional numbers earlier add calls returned.
 **/
struct GeoKernelEdit {
    GeoKernelEditKind kind = GeoKernelEditKind::eAddPoint;
    // Position of the issuing element within its work item: edits apply in this order.
    std::int32_t element = 0;
    std::int32_t handle = -1;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
    std::array<float, 3> position { 0.f, 0.f, 0.f };
    GeoPrimitiveTypeEnum primitiveType = eGeoPrimitivePolygon;
    std::vector<std::int32_t> points;
};

/** @brief The edits one work item queued, in call order. Touched by one thread at a time. */
struct GeoKernelEditQueue {
    std::vector<GeoKernelEdit> edits;
    std::int32_t newPoints = 0;
    std::int32_t newPrimitives = 0;
    std::int32_t newVertices = 0;
};

/**
 * @brief Numbers new elements while a kernel runs and applies the queued edits afterwards.
 *
 * Calls run batch by batch on many threads, so an add cannot know its final number. It returns a
 * provisional number past the input's elements that depends only on the work item and the order
 * of adds within it: item i owns a fixed block of numbers. After the run, items apply in order and
 * each item's edits in element order, so new elements are numbered as a single-threaded pass in
 * element order would number them, whatever the thread count. Numbers a script stores in
 * attributes stay provisional.
 *
 * Removals tombstone; nothing is defragmented.
 **/
class GeoKernelTopology {
public:
    GeoKernelTopology(const GeoDetail& input, std::size_t itemCount);

    /** @brief Throw KernelRunError when the item's block of numbers is used up. */
    std::int32_t newPoint(std::size_t item, GeoKernelEditQueue& queue) const;
    std::int32_t newPrimitive(std::size_t item, GeoKernelEditQueue& queue) const;
    std::int32_t newVertex(std::size_t item, GeoKernelEditQueue& queue) const;

    /** @brief Registers a group a call writes; groups are created even when no element joins. */
    std::int32_t group(GeoAttribClassEnum cls, const std::string& name);

    /**
     * @brief Applies the queues, one per work item in item order, to builder, which must hold
     * the input detail plus only attribute value changes. Throws KernelRunError for an edit
     * the data model cannot express.
     **/
    void apply(GeoDetailBuilder& builder, std::vector<GeoKernelEditQueue>& queues) const;

private:
    struct Numbering {
        std::int64_t inputCount = 0;
        std::int64_t stride = 0;
    };

    static Numbering numbering(GeoIndex inputCount, std::size_t itemCount);
    static std::int32_t provisional(const Numbering& n, std::size_t item, std::int32_t& counter, const char* what);

    const GeoDetail& _input;
    Numbering _points;
    Numbering _primitives;
    Numbering _vertices;
    std::vector<std::pair<GeoAttribClassEnum, std::string>> _groups;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoKernelTopology_h
