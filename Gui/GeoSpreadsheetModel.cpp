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

#include "GeoSpreadsheetModel.h"

#include <algorithm>
#include <climits>

#include <QtGui/QFontMetrics>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QTableView>

NATRON_NAMESPACE_ENTER

GeoSpreadsheetModel::GeoSpreadsheetModel(QObject* parent)
    : QAbstractTableModel(parent)
    , _cls(eGeoAttribClassPoint)
    , _rows(0)
    , _clock(0)
    , _cellsRead(0)
{
}

GeoSpreadsheetModel::~GeoSpreadsheetModel() = default;

void
GeoSpreadsheetModel::rebuildColumns()
{
    _allColumns.clear();
    _rows = 0;
    if (_source) {
        const int n = (int)_source->columns(_cls).size();
        _allColumns.resize(n);
        for (int i = 0; i < n; ++i) {
            _allColumns[i] = i;
        }
        _rows = _order ? (std::int64_t)_order->size() : _source->rowCount(_cls);
    }
    for (Block& b : _blocks) {
        b = Block();
    }
    _cellsRead = 0;
}

void
GeoSpreadsheetModel::setSnapshot(SourcePtr source, GeoAttribClassEnum cls, RowOrderPtr order, RowInversePtr inverse)
{
    beginResetModel();
    _source = std::move(source);
    _cls = cls;
    _order = std::move(order);
    _inverse = std::move(inverse);
    rebuildColumns();
    endResetModel();
}

void
GeoSpreadsheetModel::setClass(GeoAttribClassEnum cls)
{
    setSnapshot(_source, cls, RowOrderPtr(), RowInversePtr());
}

void
GeoSpreadsheetModel::setRowOrder(RowOrderPtr order, RowInversePtr inverse)
{
    setSnapshot(_source, _cls, std::move(order), std::move(inverse));
}

std::int64_t
GeoSpreadsheetModel::viewRowOf(GeoIndex element) const
{
    if (!_source || (element < 0)) {
        return -1;
    }
    if (!_order) {
        return (element < _source->rowCount(_cls)) ? element : -1;
    }
    if (!_inverse || (element >= (GeoIndex)_inverse->size())) {
        return -1;
    }

    return (*_inverse)[(std::size_t)element];
}

GeoIndex
GeoSpreadsheetModel::elementAt(std::int64_t viewRow) const
{
    if (!_order) {
        return viewRow;
    }
    if (viewRow < 0 || viewRow >= (std::int64_t)_order->size()) {
        return -1;
    }
    return (*_order)[(std::size_t)viewRow];
}

int
GeoSpreadsheetModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return (int)std::min<std::int64_t>(_rows, INT_MAX);
}

int
GeoSpreadsheetModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    // A class with rows but no attributes still needs a cell per row for its rows to be selectable.
    if (_allColumns.empty() && (_rows > 0)) {
        return 1;
    }
    return (int)_allColumns.size();
}

int
GeoSpreadsheetModel::attributeColumnCount() const
{
    return (int)_allColumns.size();
}

void
GeoSpreadsheetModel::fillBlock(Block& block, std::int64_t first) const
{
    const std::int64_t rows = std::min<std::int64_t>(kBlockRows, _rows - first);
    const std::size_t cols = _allColumns.size();
    block.first = first;
    block.rows = rows;
    block.cells.assign((std::size_t)rows * cols, std::string());
    if (!_source || cols == 0) {
        return;
    }
    const std::int64_t sourceRows = _source->rowCount(_cls);
    std::vector<std::string> run;
    std::int64_t i = 0;
    while (i < rows) {
        const GeoIndex start = elementAt(first + i);
        if (start < 0 || start >= sourceRows) {
            ++i;
            continue;
        }
        std::int64_t length = 1;
        while (i + length < rows) {
            const GeoIndex next = elementAt(first + i + length);
            if (next != start + length || next >= sourceRows) {
                break;
            }
            ++length;
        }
        _source->readCells(_cls, start, length, _allColumns, run);
        _cellsRead += (std::uint64_t)length * cols;
        const std::size_t got = std::min<std::size_t>(run.size(), (std::size_t)length * cols);
        for (std::size_t k = 0; k < got; ++k) {
            block.cells[(std::size_t)i * cols + k] = std::move(run[k]);
        }
        i += length;
    }
}

const GeoSpreadsheetModel::Block&
GeoSpreadsheetModel::blockFor(std::int64_t viewRow) const
{
    const std::int64_t first = viewRow - viewRow % kBlockRows;
    Block* victim = &_blocks[0];
    for (Block& b : _blocks) {
        if (b.first == first) {
            b.stamp = ++_clock;
            return b;
        }
        if (b.stamp < victim->stamp) {
            victim = &b;
        }
    }
    fillBlock(*victim, first);
    victim->stamp = ++_clock;
    return *victim;
}

QVariant
GeoSpreadsheetModel::data(const QModelIndex& index, int role) const
{
    if (!_source || !index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() < 0 || index.column() >= attributeColumnCount()) {
        return QVariant();
    }
    const std::vector<GeoSpreadsheetColumn>& columns = _source->columns(_cls);
    const GeoSpreadsheetColumn& column = columns[(std::size_t)index.column()];
    if (role == Qt::DisplayRole) {
        const Block& block = blockFor(index.row());
        const std::size_t cols = _allColumns.size();
        const std::size_t at = (std::size_t)(index.row() - block.first) * cols + (std::size_t)index.column();
        return QString::fromStdString(block.cells[at]);
    }
    if (role == Qt::ToolTipRole && column.hasFullValue) {
        const GeoIndex element = elementAt(index.row());
        if (element < 0 || element >= _source->rowCount(_cls)) {
            return QVariant();
        }
        return QString::fromStdString(_source->fullValue(_cls, index.column(), element));
    }
    return QVariant();
}

QVariant
GeoSpreadsheetModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (!_source || section < 0) {
        return QVariant();
    }
    if (orientation == Qt::Vertical) {
        if (role == Qt::DisplayRole && section < rowCount()) {
            return QVariant::fromValue<qlonglong>(elementAt(section));
        }
        return QVariant();
    }
    const std::vector<GeoSpreadsheetColumn>& columns = _source->columns(_cls);
    if ((std::size_t)section >= columns.size()) {
        return QVariant();
    }
    if (role == Qt::DisplayRole) {
        return QString::fromStdString(columns[(std::size_t)section].name);
    }
    if (role == Qt::ToolTipRole && !columns[(std::size_t)section].problem.empty()) {
        return QString::fromStdString(columns[(std::size_t)section].problem);
    }
    return QVariant();
}

void
applyGeoSpreadsheetMetrics(QTableView* view, int sampleRows)
{
    QAbstractItemModel* model = view->model();
    if (!model) {
        return;
    }
    const QFontMetrics fm(view->font());
    const int rowHeight = fm.height() + 6;
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->verticalHeader()->setDefaultSectionSize(rowHeight);

    const int rows = model->rowCount();
    int first = std::max(0, view->rowAt(0));
    const int visible = std::max(1, view->viewport()->height() / rowHeight + 1);
    const int count = std::min({ sampleRows, visible, rows - first });
    const int maxWidth = fm.horizontalAdvance(QString(48, QLatin1Char('0')));
    for (int c = 0; c < model->columnCount(); ++c) {
        int width = fm.horizontalAdvance(model->headerData(c, Qt::Horizontal).toString()) + 24;
        for (int r = 0; r < count; ++r) {
            width = std::max(width, fm.horizontalAdvance(model->data(model->index(first + r, c)).toString()) + 12);
        }
        view->setColumnWidth(c, std::min(width, maxWidth));
    }
}

NATRON_NAMESPACE_EXIT
