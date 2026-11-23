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

#ifndef Engine_Geo_GeoSpreadsheet_h
#define Engine_Geo_GeoSpreadsheet_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

struct GeoSpreadsheetColumn {
    std::string name;
    // True when the cell text is a summary and fullValue() carries more (array attributes, transforms, bounds).
    bool hasFullValue = false;
    // Set when the column's values cannot be shown, saying why; its cells are then empty.
    std::string problem;
};

/** @brief The length of an array attribute element; arrays sort by length, not by content. */
struct GeoSpreadsheetArrayLength {
    std::int64_t length = 0;

    bool operator==(const GeoSpreadsheetArrayLength&) const = default;
};

typedef std::variant<double, std::string, GeoSpreadsheetArrayLength> GeoSpreadsheetSortKey;

/**
 * @brief A read-only, row-addressable view of element data for a spreadsheet. Implementations are immutable
 * snapshots, safe to read from any thread. Rows are dense element numbers, so a source never exposes tombstones.
 * The detail class has exactly one row.
 **/
class GeoSpreadsheetSource {
public:
    virtual ~GeoSpreadsheetSource();

    virtual std::int64_t rowCount(GeoAttribClassEnum cls) const = 0;

    virtual const std::vector<GeoSpreadsheetColumn>& columns(GeoAttribClassEnum cls) const = 0;

    /**
     * @brief Display text of rows [firstRow, firstRow + rowCount) clipped to the class's row count, for the listed
     * columns. @p out is resized to clippedRows * columns.size() and filled row-major.
     **/
    virtual void readCells(GeoAttribClassEnum cls,
                           std::int64_t firstRow,
                           std::int64_t rowCount,
                           std::span<const int> columns,
                           std::vector<std::string>& out) const
        = 0;

    /**
     * @brief readCells() for the listed rows in their order, so that a sorted or filtered block costs one call
     * rather than one per row. @p out is resized to rows.size() * columns.size(); a row out of range gives empty
     * cells. The default reads each run of consecutive rows through readCells().
     **/
    virtual void readRowCells(GeoAttribClassEnum cls,
                              std::span<const GeoIndex> rows,
                              std::span<const int> columns,
                              std::vector<std::string>& out) const;

    virtual GeoSpreadsheetSortKey sortKey(GeoAttribClassEnum cls,
                                          int column,
                                          std::int64_t row) const
        = 0;

    /** @brief The whole value of one cell, for tooltips. Equals the cell text when hasFullValue is false. */
    virtual std::string fullValue(GeoAttribClassEnum cls,
                                  int column,
                                  std::int64_t row) const
        = 0;
};

class GeoDetailSpreadsheetSource : public GeoSpreadsheetSource {
public:
    explicit GeoDetailSpreadsheetSource(GeoDetailConstPtr detail);
    ~GeoDetailSpreadsheetSource() override;

    const GeoDetailConstPtr& detail() const
    {
        return _detail;
    }

    std::int64_t rowCount(GeoAttribClassEnum cls) const override;
    const std::vector<GeoSpreadsheetColumn>& columns(GeoAttribClassEnum cls) const override;
    void readCells(GeoAttribClassEnum cls,
                   std::int64_t firstRow,
                   std::int64_t rowCount,
                   std::span<const int> columns,
                   std::vector<std::string>& out) const override;
    void readRowCells(GeoAttribClassEnum cls,
                      std::span<const GeoIndex> rows,
                      std::span<const int> columns,
                      std::vector<std::string>& out) const override;
    GeoSpreadsheetSortKey sortKey(GeoAttribClassEnum cls,
                                  int column,
                                  std::int64_t row) const override;
    std::string fullValue(GeoAttribClassEnum cls,
                          int column,
                          std::int64_t row) const override;

    struct Column {
        enum KindEnum {
            eAttribute,
            eVertexPoint,
            ePrimitiveKind,
            ePrimitiveTransform,
            ePrimitiveBounds,
            ePrimitiveSourcePath,
        };

        KindEnum kind = eAttribute;
        std::size_t attribute = 0;
        int component = 0;
    };

private:
    // Cells of the rows at offsets, kInvalidOffset giving empty ones.
    void offsetCells(GeoAttribClassEnum cls,
                     std::span<const GeoOffset> offsets,
                     std::span<const int> columns,
                     std::vector<std::string>& out) const;

    std::string intrinsicText(GeoOffset primitive,
                              Column::KindEnum kind,
                              bool full) const;

    GeoDetailConstPtr _detail;
    std::vector<GeoSpreadsheetColumn> _columns[kGeoAttribClassCount];
    std::vector<Column> _bindings[kGeoAttribClassCount];
};

/**
 * @brief @p offsets, a GeoElementMask indexed by storage offset (as geoSelectElements() fills it), re-indexed by
 * dense element number through @p map; tombstoned offsets drop out.
 **/
GeoElementMask geoSpreadsheetRowMask(const GeoIndexMap& map,
                                     const GeoElementMask& offsets);

/**
 * @brief The element numbers of one class in view order: the elements @p mask keeps (every element when null; an
 * element past the mask's size is dropped), sorted by column @p sortColumn (unsorted when negative). @p mask is
 * indexed by row, the source's dense element number, not by storage offset: a detail's offset mask goes through
 * geoSpreadsheetRowMask() first.
 *
 * Equal keys stay in ascending element order, descending included. Keys of different kinds in one column order by
 * kind, then value; NaN sorts after every number, descending included. Returns null when @p cancelled became true
 * before the order was complete; it is polled at least every few tens of thousands of rows.
 **/
std::shared_ptr<const std::vector<GeoIndex>> geoSpreadsheetBuildOrder(const GeoSpreadsheetSource& source,
                                                                      GeoAttribClassEnum cls,
                                                                      const GeoElementMask* mask,
                                                                      int sortColumn,
                                                                      bool descending,
                                                                      const std::atomic<bool>& cancelled);

/**
 * @brief Test hook: counts the attribute and topology pages readCells() reads, one per column and run of rows in
 * the same page.
 **/
std::uint64_t geoSpreadsheetPagesRead();
void geoSpreadsheetResetPagesRead();

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoSpreadsheet_h
