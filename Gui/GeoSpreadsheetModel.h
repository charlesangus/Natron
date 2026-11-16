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

#ifndef Gui_GeoSpreadsheetModel_h
#define Gui_GeoSpreadsheetModel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QtCore/QAbstractTableModel>

#include "Engine/Geo/GeoSpreadsheet.h"

class QTableView;

NATRON_NAMESPACE_ENTER

/**
 * @brief A table over one GeoSpreadsheetSource snapshot and one attribute class. data() reads only the rows it is
 * asked for, a block at a time, so the cost of showing a table is independent of its row count. Main thread only.
 **/
class GeoSpreadsheetModel : public QAbstractTableModel {
    Q_OBJECT

public:
    typedef std::shared_ptr<const GeoSpreadsheetSource> SourcePtr;
    typedef std::shared_ptr<const std::vector<GeoIndex>> RowOrderPtr;
    // Indexed by element number, the view row showing it, or -1 for an element the order leaves out.
    typedef std::shared_ptr<const std::vector<GeoIndex>> RowInversePtr;

    // Rows read per cache fill; a fill reads at most this many rows times the column count.
    static constexpr int kBlockRows = 128;

    explicit GeoSpreadsheetModel(QObject* parent = nullptr);
    ~GeoSpreadsheetModel() override;

    /**
     * @brief Replaces the snapshot, class and row order in one model reset. A null order is the identity.
     * The inverse belongs to the order and is dropped with it.
     **/
    void setSnapshot(SourcePtr source, GeoAttribClassEnum cls, RowOrderPtr order = RowOrderPtr(), RowInversePtr inverse = RowInversePtr());
    void setClass(GeoAttribClassEnum cls);
    /** @brief The order's size is the row count; element numbers outside the class show as empty rows. */
    void setRowOrder(RowOrderPtr order, RowInversePtr inverse = RowInversePtr());

    const SourcePtr& source() const
    {
        return _source;
    }

    GeoAttribClassEnum attribClass() const
    {
        return _cls;
    }

    // Null when the rows are in element order.
    const RowOrderPtr& rowOrder() const
    {
        return _order;
    }

    // Null when the rows are in element order, or when the order came without one.
    const RowInversePtr& rowInverse() const
    {
        return _inverse;
    }

    /** @brief The element number shown at a view row. */
    GeoIndex elementAt(std::int64_t viewRow) const;

    /** @brief The view row showing an element; -1 when the order leaves it out or has no inverse. */
    std::int64_t viewRowOf(GeoIndex element) const;

    /** @brief Cells requested from the source since construction or the last reset; for tests. */
    std::uint64_t cellsRead() const
    {
        return _cellsRead;
    }

    void resetCellsRead()
    {
        _cellsRead = 0;
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    struct Block {
        std::int64_t first = -1;
        std::int64_t rows = 0;
        std::uint64_t stamp = 0;
        std::vector<std::string> cells;
    };

    void rebuildColumns();
    const Block& blockFor(std::int64_t viewRow) const;
    void fillBlock(Block& block, std::int64_t first) const;

    SourcePtr _source;
    GeoAttribClassEnum _cls;
    RowOrderPtr _order;
    RowInversePtr _inverse;
    std::vector<int> _allColumns;
    std::int64_t _rows;

    mutable std::array<Block, 2> _blocks;
    mutable std::uint64_t _clock;
    mutable std::uint64_t _cellsRead;
};

/**
 * @brief Fixed row heights and column widths measured from the visible rows only, so laying out a table never
 * touches rows beyond the viewport. Call again after the model or the font changes.
 **/
void applyGeoSpreadsheetMetrics(QTableView* view, int sampleRows = 48);

NATRON_NAMESPACE_EXIT

#endif // Gui_GeoSpreadsheetModel_h
