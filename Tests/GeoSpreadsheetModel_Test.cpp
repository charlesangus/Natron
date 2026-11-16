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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/GeoSpreadsheetModel.h"

#include <QAbstractItemModelTester>
#include <QPixmap>
#include <QTableView>

NATRON_NAMESPACE_USING

namespace {

constexpr std::int64_t kHugeRows = 10000000;

class FakeSource : public GeoSpreadsheetSource {
public:
    explicit FakeSource(std::int64_t rows)
        : _rows(rows)
    {
        _columns.push_back({ "id", false, "" });
        _columns.push_back({ "P", true, "" });
        _columns.push_back({ "broken", false, "no such attribute" });
    }

    std::int64_t rowCount(GeoAttribClassEnum cls) const override
    {
        return cls == eGeoAttribClassPoint ? _rows : 1;
    }

    const std::vector<GeoSpreadsheetColumn>& columns(GeoAttribClassEnum) const override
    {
        return _columns;
    }

    void readCells(GeoAttribClassEnum cls,
                   std::int64_t firstRow,
                   std::int64_t count,
                   std::span<const int> columns,
                   std::vector<std::string>& out) const override
    {
        const std::int64_t n = std::max<std::int64_t>(0, std::min(count, rowCount(cls) - firstRow));
        out.clear();
        out.reserve((std::size_t)n * columns.size());
        for (std::int64_t r = 0; r < n; ++r) {
            for (int c : columns) {
                out.push_back(c == 0 ? "r" + std::to_string(firstRow + r) : c == 1 ? "[3 values]"
                                                                                   : "");
            }
        }
    }

    GeoSpreadsheetSortKey sortKey(GeoAttribClassEnum, int, std::int64_t row) const override
    {
        return (double)row;
    }

    std::string fullValue(GeoAttribClassEnum, int column, std::int64_t row) const override
    {
        return "full " + std::to_string(column) + " " + std::to_string(row);
    }

private:
    std::int64_t _rows;
    std::vector<GeoSpreadsheetColumn> _columns;
};

QString
cell(const GeoSpreadsheetModel& m, int row, int col, int role = Qt::DisplayRole)
{
    return m.data(m.index(row, col), role).toString();
}

} // namespace

TEST(GeoSpreadsheetModelTest, HugeSourceReadsOnlyTheAskedRow)
{
    GeoSpreadsheetModel model;
    model.setSnapshot(std::make_shared<FakeSource>(kHugeRows), eGeoAttribClassPoint);
    EXPECT_EQ(model.rowCount(), kHugeRows);
    EXPECT_EQ(model.columnCount(), 3);
    EXPECT_EQ(model.cellsRead(), 0u);
    EXPECT_EQ(cell(model, (int)kHugeRows - 1, 0), QStringLiteral("r9999999"));
    EXPECT_LE(model.cellsRead(), (std::uint64_t)GeoSpreadsheetModel::kBlockRows * 3);
}

TEST(GeoSpreadsheetModelTest, PaintingAViewportReadsBoundedCells)
{
    GeoSpreadsheetModel model;
    model.setSnapshot(std::make_shared<FakeSource>(kHugeRows), eGeoAttribClassPoint);
    QTableView view;
    view.setModel(&model);
    view.resize(400, 40 * 20);
    applyGeoSpreadsheetMetrics(&view);
    view.show();
    model.resetCellsRead();
    view.grab();
    // A viewport of at most 40 rows spans at most two aligned blocks, and a block read covers kBlockRows rows of
    // every column, so the cache granularity sets the bound rather than the 40 rows themselves.
    EXPECT_GT(model.cellsRead(), 0u);
    EXPECT_LE(model.cellsRead(), (std::uint64_t)2 * GeoSpreadsheetModel::kBlockRows * model.columnCount());
}

TEST(GeoSpreadsheetModelTest, RowOrderMapsViewRowToElement)
{
    GeoSpreadsheetModel model;
    auto order = std::make_shared<std::vector<GeoIndex>>(std::vector<GeoIndex> { 7, 3, 4, 5, 100, 0 });
    model.setSnapshot(std::make_shared<FakeSource>(200), eGeoAttribClassPoint, order);
    QAbstractItemModelTester tester(&model);
    EXPECT_EQ(model.rowCount(), 6);
    const int expected[] = { 7, 3, 4, 5, 100, 0 };
    for (int r = 0; r < 6; ++r) {
        EXPECT_EQ(model.headerData(r, Qt::Vertical).toInt(), expected[r]);
        EXPECT_EQ(cell(model, r, 0), QStringLiteral("r%1").arg(expected[r]));
        EXPECT_EQ(cell(model, r, 1, Qt::ToolTipRole), QStringLiteral("full 1 %1").arg(expected[r]));
    }
    model.resetCellsRead();
    model.setRowOrder(std::make_shared<std::vector<GeoIndex>>(std::vector<GeoIndex> { 9, 8 }));
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(cell(model, 0, 0), QStringLiteral("r9"));
}

TEST(GeoSpreadsheetModelTest, ToolTipsCarryFullValueAndProblem)
{
    GeoSpreadsheetModel model;
    model.setSnapshot(std::make_shared<FakeSource>(10), eGeoAttribClassPoint);
    EXPECT_EQ(cell(model, 2, 1), QStringLiteral("[3 values]"));
    EXPECT_EQ(cell(model, 2, 1, Qt::ToolTipRole), QStringLiteral("full 1 2"));
    EXPECT_TRUE(model.data(model.index(2, 0), Qt::ToolTipRole).isNull());
    EXPECT_EQ(model.headerData(2, Qt::Horizontal, Qt::ToolTipRole).toString(), QStringLiteral("no such attribute"));
    EXPECT_TRUE(model.headerData(0, Qt::Horizontal, Qt::ToolTipRole).isNull());
}

TEST(GeoSpreadsheetModelTest, ClassSwapIsOneReset)
{
    GeoSpreadsheetModel model;
    model.setSnapshot(std::make_shared<FakeSource>(50), eGeoAttribClassPoint);
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    model.setClass(eGeoAttribClassDetail);
    EXPECT_EQ(resets, 1);
    EXPECT_EQ(model.rowCount(), 1);
}
