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

#ifndef Engine_Nodes_Scene_ScenePrimElements_h
#define Engine_Nodes_Scene_ScenePrimElements_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Engine/Geo/GeoSpreadsheet.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

class SceneStage;

enum ScenePrimElementsStatusEnum {
    eScenePrimElementsStatusOk,
    // No prim at the path.
    eScenePrimElementsStatusMissing,
    // The prim, or the nearest prim above the path, is in a payload that is not loaded.
    eScenePrimElementsStatusNotLoaded,
    eScenePrimElementsStatusCancelled
};

/** @brief A UsdGeomSubset of the prim: face subsets as primitive groups, point subsets as point groups. */
struct ScenePrimElementGroup {
    std::string name;
    GeoAttribClassEnum cls = eGeoAttribClassPrimitive;
    PXR_NS::VtIntArray indices;
};

/**
 * @brief A prim's element data as a spreadsheet: points are a point-based prim's points or a point
 * instancer's instances, vertices are a mesh's face-vertices, primitives are a mesh's faces or a
 * curves prim's curves, and the detail row holds the constant primvars. Primvars land in the class
 * of their interpolation. The values are the VtArrays the stage returned, shared by reference
 * count, and an indexed primvar is resolved row by row as it is read. A column whose values do not
 * fit its class's row count, or whose type cannot be shown, carries a problem and empty cells.
 * Immutable, and needs no stage lock.
 **/
class ScenePrimElementsSource : public GeoSpreadsheetSource {
public:
    struct Binding;

    ScenePrimElementsSource(const std::int64_t (&rowCounts)[kGeoAttribClassCount],
                            std::vector<Binding> bindings,
                            std::vector<ScenePrimElementGroup> groups);
    ~ScenePrimElementsSource() override;

    std::int64_t rowCount(GeoAttribClassEnum cls) const override;
    const std::vector<GeoSpreadsheetColumn>& columns(GeoAttribClassEnum cls) const override;
    void readCells(GeoAttribClassEnum cls,
                   std::int64_t firstRow,
                   std::int64_t rowCount,
                   std::span<const int> columns,
                   std::vector<std::string>& out) const override;
    GeoSpreadsheetSortKey sortKey(GeoAttribClassEnum cls,
                                  int column,
                                  std::int64_t row) const override;
    std::string fullValue(GeoAttribClassEnum cls,
                          int column,
                          std::int64_t row) const override;

    const std::vector<ScenePrimElementGroup>& groups() const
    {
        return _groups;
    }

    // An attribute or primvar as read, with the class it was placed in.
    struct Binding {
        std::string name;
        GeoAttribClassEnum cls = eGeoAttribClassPoint;
        // Of the attribute's type name: picks x/y/z or r/g/b column suffixes.
        PXR_NS::TfToken role;
        int elementSize = 1;
        bool indexed = false;
        PXR_NS::VtValue values;
        PXR_NS::VtIntArray indices;
    };

    struct ValueOps;

    struct Column {
        std::size_t binding = 0;
        int component = 0;
        // Shows elementSize values, or a whole constant array, in one cell.
        bool arrayCell = false;
        int arrayLength = 1;
        bool valid = true;
    };

private:
    // The element of values the row reads from, or -1 when its index is out of range.
    std::int64_t elementOf(const Column& col,
                           std::int64_t row) const;
    std::string cellText(const Column& col,
                         std::int64_t row,
                         bool full) const;

    std::int64_t _rowCounts[kGeoAttribClassCount] = {};
    std::vector<Binding> _bindings;
    std::vector<const ValueOps*> _ops;
    std::vector<ScenePrimElementGroup> _groups;
    std::vector<GeoSpreadsheetColumn> _columns[kGeoAttribClassCount];
    std::vector<Column> _columnBindings[kGeoAttribClassCount];
};

/**
 * @brief Reads a prim's elements a step at a time, so that a caller can take the stage lock for
 * each step and release it between them: the first step lists what the prim holds, and every later
 * one makes one Get of one attribute. The prim is looked up by path in every step, so a stage
 * edited between steps reads as it is then. Never loads a payload.
 **/
class ScenePrimElementsRead {
public:
    ScenePrimElementsRead(std::string primPath,
                          double frame,
                          double framesPerSecond);
    ~ScenePrimElementsRead();

    /**
     * @brief One read of stage, whose lock the caller holds. A null stage reads as a missing prim.
     * Returns whether another step is due.
     **/
    bool step(const SceneStage* stage);

    ScenePrimElementsStatusEnum getStatus() const;

    // The source of what the steps read, once they are done. Null unless the status is ok. Needs no stage lock.
    std::shared_ptr<const ScenePrimElementsSource> takeSource();

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief Calls read with the stage while holding its lock, or with null when there is no stage.
 * Returns false, without calling read, when the read is cancelled.
 **/
using ScenePrimElementsStageReader = std::function<bool(const std::function<void(const SceneStage* stage)>& read)>;

struct ScenePrimElements {
    ScenePrimElementsStatusEnum status = eScenePrimElementsStatusMissing;
    std::shared_ptr<const ScenePrimElementsSource> source;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_ScenePrimElements_h
