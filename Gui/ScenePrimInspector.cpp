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

#include "ScenePrimInspector.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractTableModel>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"

NATRON_NAMESPACE_ENTER

namespace {

using SceneOps::SceneOpinion;
using SceneOps::ScenePrimArc;
using SceneOps::ScenePrimComposition;

const int kMaxCellLength = 200;

// A tooltip lays its whole text out on the main thread each time it shows; this much fills a
// 1080 pixel screen at most.
const int kMaxTipLength = 2000;

// A plain-text tooltip never wraps by itself, so a long value would run off a 1920 pixel screen.
const int kTipLineLength = 160;

// A text edit lays a line out whole, so one line of millions of characters stalls the main thread
// however slowly it is added.
const int kMaxShownLineLength = 4096;

const int kTextSliceMs = 8;
const int kTextChunkLength = 16 * 1024;

// Long enough that a read finishing normally does not flicker the rows.
const int kGreyAfterMs = 100;

struct InspectorCell {
    QString text;
    QString tip;
    // Formatted when asked for, past the cell's own text: it can hold millions of elements.
    PXR_NS::VtValue value;
    bool cut = false;
    NodeWPtr node;
};

typedef std::vector<InspectorCell> InspectorRow;

QString
wrappedForTip(const QString& text)
{
    if (text.size() <= kTipLineLength) {
        return text;
    }
    QString wrapped;
    wrapped.reserve(text.size() + text.size() / kTipLineLength + 1);
    int lineLength = 0;
    for (const QChar c : text) {
        wrapped += c;
        lineLength = (c == QLatin1Char('\n')) ? 0 : (lineLength + 1);
        if (lineLength == kTipLineLength) {
            wrapped += QLatin1Char('\n');
            lineLength = 0;
        }
    }

    return wrapped;
}

QString
tipOf(const InspectorCell& cell)
{
    QString text;
    bool cut = false;

    if (cell.value.IsEmpty()) {
        cut = cell.tip.size() > kMaxTipLength;
        text = cell.tip.left(kMaxTipLength);
    } else {
        text = QString::fromStdString(SceneOps::valueText(cell.value, kMaxTipLength, nullptr, &cut));
    }
    if (text.isEmpty()) {
        return QString();
    }
    QString tip = wrappedForTip(text);

    if (cut) {
        tip += QString::fromUtf8("...\n") + ScenePrimInspector::tr("Double-click to show the whole value.");
    }

    return tip;
}

InspectorCell
cellOf(const QString& full)
{
    InspectorCell cell;

    cell.cut = full.size() > kMaxCellLength;
    cell.text = cell.cut ? (full.left(kMaxCellLength) + QString::fromUtf8("...")) : full;
    // Uniform row heights would clip a value's later lines.
    cell.text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    cell.tip = full;

    return cell;
}

InspectorCell
valueCell(const PXR_NS::VtValue& value)
{
    InspectorCell cell;

    cell.text = QString::fromStdString(SceneOps::valueText(value, kMaxCellLength, nullptr, &cell.cut));
    if (cell.cut) {
        cell.text += QString::fromUtf8("...");
    }
    cell.text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    cell.value = value;

    return cell;
}

QString
sourceLabel(SceneOps::SceneValueSource source)
{
    switch (source) {
    case SceneOps::eSceneValueSourceFallback:
        return ScenePrimInspector::tr("Fallback");
    case SceneOps::eSceneValueSourceDefault:
        return ScenePrimInspector::tr("Default Value");
    case SceneOps::eSceneValueSourceTimeSamples:
        return ScenePrimInspector::tr("Time Samples");
    case SceneOps::eSceneValueSourceValueClips:
        return ScenePrimInspector::tr("Value Clips");
    case SceneOps::eSceneValueSourceSpline:
        return ScenePrimInspector::tr("Spline");
    case SceneOps::eSceneValueSourceNone:
        break;
    }

    return ScenePrimInspector::tr("No Value");
}

InspectorCell
authorCell(const NodePtr& node,
           const std::string& layerIdentifier,
           bool schemaFallback)
{
    InspectorCell cell;

    if (node) {
        cell.text = QString::fromStdString(node->getFullyQualifiedName());
    } else if (schemaFallback) {
        cell.text = ScenePrimInspector::tr("Schema Fallback");
    } else {
        cell.text = QString::fromStdString(SceneOps::layerFileOf(layerIdentifier));
    }
    cell.tip = QString::fromStdString(layerIdentifier);
    cell.node = node;

    return cell;
}

InspectorCell
authorCell(const SceneOps::SceneAuthor& author)
{
    return authorCell(author.node, author.layerIdentifier, author.schemaFallback);
}

// The lock is taken per row, and a value is turned into text outside it: an attribute can hold
// millions of points, and the main thread may be waiting for the lock. A VtValue shares its data
// by reference counts and needs no lock. A sync between two rows starts a new read anyway.
bool
readAttributes(const ScenePrimInspectorRead& read,
               std::vector<InspectorRow>* rows)
{
    const std::string path = read.getPath().toStdString();
    std::vector<std::string> names;

    if (!read.readStage([&](const SceneStage* stage) {
            if (stage) {
                names = SceneOps::getAttributeNames(*stage, path);
            }
        })) {
        return false;
    }
    rows->reserve(names.size());
    for (const std::string& name : names) {
        bool defined = false;
        SceneOps::ScenePrimAttribute attribute;
        if (!read.readStage([&](const SceneStage* stage) {
                defined = stage && SceneOps::readAttribute(*stage, path, name, read.getFrame(), read.getFramesPerSecond(), &attribute);
            })) {
            return false;
        }
        if (defined) {
            rows->push_back({ cellOf(QString::fromStdString(name)),
                              cellOf(QString::fromStdString(attribute.typeName)),
                              valueCell(attribute.value),
                              cellOf(sourceLabel(attribute.source)),
                              authorCell(attribute.author) });
        }
    }

    return true;
}

bool
readRelationships(const ScenePrimInspectorRead& read,
                  std::vector<InspectorRow>* rows)
{
    const std::string path = read.getPath().toStdString();
    std::vector<std::string> names;

    if (!read.readStage([&](const SceneStage* stage) {
            if (stage) {
                names = SceneOps::getRelationshipNames(*stage, path);
            }
        })) {
        return false;
    }
    rows->reserve(names.size());
    for (const std::string& name : names) {
        bool defined = false;
        SceneOps::ScenePrimRelationship relationship;
        if (!read.readStage([&](const SceneStage* stage) {
                defined = stage && SceneOps::readRelationship(*stage, path, name, &relationship);
            })) {
            return false;
        }
        if (defined) {
            QStringList targets;
            for (const std::string& target : relationship.targets) {
                targets << QString::fromStdString(target);
            }
            rows->push_back({ cellOf(QString::fromStdString(name)), cellOf(targets.join(QString::fromUtf8(", "))), authorCell(relationship.author) });
        }
    }

    return true;
}

bool
readMetadata(const ScenePrimInspectorRead& read,
             std::vector<InspectorRow>* rows)
{
    const std::string path = read.getPath().toStdString();
    SceneOps::ScenePrimMetadata metadata;

    if (!read.readStage([&](const SceneStage* stage) {
            if (stage) {
                SceneOps::readMetadata(*stage, path, &metadata);
            }
        })) {
        return false;
    }
    rows->reserve(metadata.fields.size() + 1);
    if (metadata.isPrototype) {
        rows->push_back({ cellOf(ScenePrimInspector::tr("Prototype")), cellOf(ScenePrimInspector::tr("This prim is a prototype")), InspectorCell() });
    } else if (!metadata.prototypePath.empty()) {
        rows->push_back({ cellOf(ScenePrimInspector::tr("Prototype")), cellOf(QString::fromStdString(metadata.prototypePath)), InspectorCell() });
    }
    for (const SceneOps::ScenePrimMetadata::Field& field : metadata.fields) {
        if (read.isCancelled()) {
            return false;
        }
        rows->push_back({ cellOf(QString::fromStdString(field.key)), valueCell(field.value), authorCell(field.author) });
    }

    return true;
}

QString
layerOffsetText(const SceneOpinion& opinion)
{
    if ((opinion.layerOffset == 0.) && (opinion.layerScale == 1.)) {
        return QString();
    }

    return ScenePrimInspector::tr("Offset %1, Scale %2").arg(opinion.layerOffset).arg(opinion.layerScale);
}

InspectorCell
opinionValueCell(const SceneOpinion& opinion)
{
    if (!opinion.primDescription.empty()) {
        return cellOf(QString::fromStdString(opinion.primDescription));
    }
    if (opinion.numTimeSamples > 0) {
        return cellOf(ScenePrimInspector::tr("%n time sample(s)", nullptr, (int)opinion.numTimeSamples));
    }
    if (opinion.hasSpline) {
        return cellOf(ScenePrimInspector::tr("Spline"));
    }

    return valueCell(opinion.value);
}

InspectorRow
opinionRow(const QString& scope,
           const SceneOpinion& opinion)
{
    return { cellOf(scope),
             authorCell(opinion.owner, opinion.layerIdentifier, false),
             cellOf(QString::fromStdString(opinion.layerIdentifier)),
             opinionValueCell(opinion),
             cellOf(layerOffsetText(opinion)) };
}

bool
readOpinions(const ScenePrimInspectorRead& read,
             std::vector<InspectorRow>* rows)
{
    const std::string path = read.getPath().toStdString();
    const std::string property = read.getProperty().toStdString();
    std::vector<SceneOpinion> primOpinions;
    std::vector<SceneOpinion> propertyOpinions;

    if (!read.readStage([&](const SceneStage* stage) {
            if (!stage) {
                return;
            }
            SceneOps::getPrimOpinions(*stage, path, &primOpinions);
            if (!property.empty()) {
                propertyOpinions = SceneOps::getPropertyOpinions(*stage, path, property, read.getFrame(), read.getFramesPerSecond());
            }
        })) {
        return false;
    }
    for (SceneOpinion& opinion : propertyOpinions) {
        if (!read.readStage([&opinion](const SceneStage* stage) {
                if (stage) {
                    SceneOps::readOpinionValue(&opinion);
                }
            })) {
            return false;
        }
    }
    rows->reserve(propertyOpinions.size() + primOpinions.size());
    const QString propertyScope = ScenePrimInspector::tr("Property %1").arg(QString::fromStdString(property));
    for (const SceneOpinion& opinion : propertyOpinions) {
        rows->push_back(opinionRow(propertyScope, opinion));
    }
    for (const SceneOpinion& opinion : primOpinions) {
        rows->push_back(opinionRow(ScenePrimInspector::tr("Prim"), opinion));
    }

    return true;
}

QString
arcLabel(ScenePrimArc::Type type)
{
    switch (type) {
    case ScenePrimArc::eRoot:
        return ScenePrimInspector::tr("Root");
    case ScenePrimArc::eInherit:
        return ScenePrimInspector::tr("Inherit");
    case ScenePrimArc::eVariant:
        return ScenePrimInspector::tr("Variant");
    case ScenePrimArc::eRelocate:
        return ScenePrimInspector::tr("Relocate");
    case ScenePrimArc::eReference:
        return ScenePrimInspector::tr("Reference");
    case ScenePrimArc::ePayload:
        return ScenePrimInspector::tr("Payload");
    case ScenePrimArc::eSpecialize:
        return ScenePrimInspector::tr("Specialize");
    }

    return QString();
}

// Whether a read of the same prim of the same stage at the same edit count may reuse the last result.
// The edit count is taken before the read's lock, so an edit landing during the read misses next time.
class InspectorReadKey {
public:
    bool matches(const ScenePrimInspectorRead& read,
                 U64 editCount) const
    {
        const SceneGraphStageSourcePtr& source = read.getSource();

        return _valid && !_source.owner_before(source) && !source.owner_before(_source) && (_path == read.getPath()) && (_editCount == editCount);
    }

    void set(const ScenePrimInspectorRead& read,
             U64 editCount)
    {
        _valid = true;
        _source = read.getSource();
        _path = read.getPath();
        _editCount = editCount;
    }

private:
    bool _valid = false;
    std::weak_ptr<SceneGraphStageSource> _source;
    QString _path;
    U64 _editCount = 0;
};

struct CompositionCache {
    InspectorReadKey key;
    std::vector<InspectorRow> rows;
};

// A UsdPrimCompositionQuery recomputes the arcs each time and they do not vary with the frame, so
// playback must not pay for one per frame.
bool
readComposition(const ScenePrimInspectorRead& read,
                std::vector<InspectorRow>* rows,
                CompositionCache* cache,
                std::atomic<qulonglong>* queries)
{
    const U64 editCount = read.getEditCount();

    if (cache->key.matches(read, editCount)) {
        *rows = cache->rows;

        return true;
    }
    const std::string path = read.getPath().toStdString();
    ScenePrimComposition composition;

    if (!read.readStage([&](const SceneStage* stage) {
            if (stage) {
                ++*queries;
                SceneOps::getPrimComposition(*stage, path, &composition);
            }
        })) {
        return false;
    }
    if (composition.isPrototype) {
        rows->push_back({ cellOf(ScenePrimInspector::tr("Prototype")), cellOf(ScenePrimInspector::tr("This prim is a prototype")), InspectorCell(), InspectorCell(), InspectorCell() });
    } else if (composition.isInstanceProxy) {
        rows->push_back({ cellOf(ScenePrimInspector::tr("Instance Proxy")), cellOf(QString::fromStdString(composition.prototypePath)), InspectorCell(), InspectorCell(), InspectorCell() });
    }
    for (const ScenePrimArc& arc : composition.arcs) {
        QStringList notes;
        if (arc.ancestral) {
            notes << ScenePrimInspector::tr("Ancestral");
        }
        if (arc.implicit) {
            notes << ScenePrimInspector::tr("Implicit");
        }
        if (!arc.loaded) {
            notes << ScenePrimInspector::tr("Not loaded");
        }
        const QString indent(arc.depth * 2, QLatin1Char(' '));
        rows->push_back({ cellOf(indent + arcLabel(arc.type)),
                          cellOf(QString::fromStdString(arc.targetPath)),
                          authorCell(arc.targetOwner, arc.targetLayer, false),
                          authorCell(arc.introducingOwner, arc.introducingLayer, false),
                          cellOf(notes.join(QString::fromUtf8(", "))) });
    }
    cache->key.set(read, editCount);
    cache->rows = *rows;

    return true;
}

// Adds a text of any size to a read-only text edit a slice per event loop pass, so the main thread
// keeps painting. What the loader last showed goes away with the new one all at once: a document
// can only be emptied in one call.
class SlicedTextLoader {
public:
    explicit SlicedTextLoader(QPlainTextEdit* edit)
        : _edit(edit)
    {
        _edit->document()->setUndoRedoEnabled(false);
        _edit->setProperty("natronSceneGraphTextLoading", false);
        _timer.setInterval(0);
        QObject::connect(&_timer, &QTimer::timeout, _edit, [this]() { step(); });
    }

    void show(const std::shared_ptr<const QString>& text)
    {
        _edit->clear();
        _text = text;
        _next = 0;
        step();
        // The edit's own cursor moved along with the first insertion.
        _edit->moveCursor(QTextCursor::Start);
    }

    void clear()
    {
        _text.reset();
        _timer.stop();
        _edit->setProperty("natronSceneGraphTextLoading", false);
        _edit->clear();
    }

private:
    void step()
    {
        QElapsedTimer elapsed;
        QTextCursor cursor(_edit->document());

        elapsed.start();
        cursor.movePosition(QTextCursor::End);
        cursor.beginEditBlock();
        do {
            const qsizetype lineEnd = _text->indexOf(QLatin1Char('\n'), std::min(_next + kTextChunkLength, _text->size()));
            const qsizetype end = (lineEnd < 0) ? _text->size() : (lineEnd + 1);
            cursor.insertText(_text->mid(_next, end - _next));
            _next = end;
        } while ((_next < _text->size()) && (elapsed.elapsed() < kTextSliceMs));
        cursor.endEditBlock();
        const bool loading = _next < _text->size();
        _edit->setProperty("natronSceneGraphTextLoading", loading);
        if (loading) {
            _timer.start();
        } else {
            _timer.stop();
            _text.reset();
        }
    }

    QPlainTextEdit* _edit;
    QTimer _timer;
    std::shared_ptr<const QString> _text;
    qsizetype _next = 0;
};

// The whole of a value too long for its cell and its tooltip.
class FullValueWindow
    : public QDialog {
public:
    FullValueWindow(const InspectorCell& cell,
                    QWidget* parent)
        : QDialog(parent)
        , _text(new QPlainTextEdit(this))
        , _loader(_text)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName(QString::fromUtf8("sceneGraphFullValue"));
        setWindowTitle(ScenePrimInspector::tr("Full Value"));
        resize(800, 600);
        _text->setObjectName(QString::fromUtf8("sceneGraphFullValueText"));
        _text->setReadOnly(true);
        _text->setLineWrapMode(QPlainTextEdit::NoWrap);
        _text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        _text->setProperty("natronSceneGraphTextLoading", true);
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(_text);
        _worker = std::thread([this, value = cell.value, tip = cell.tip]() {
            const QString full = value.IsEmpty() ? tip : QString::fromStdString(SceneOps::valueText(value, std::numeric_limits<std::size_t>::max(), &_cancelled));
            if (_cancelled) {
                return;
            }
            const std::shared_ptr<const QString> wrapped = std::make_shared<const QString>(wrappedForTip(full));
            QMetaObject::invokeMethod(this, [this, wrapped]() { _loader.show(wrapped); }, Qt::QueuedConnection);
        });
    }

    ~FullValueWindow() override
    {
        _cancelled = true;
        _worker.join();
    }

private:
    QPlainTextEdit* _text;
    SlicedTextLoader _loader;
    std::atomic<bool> _cancelled { false };
    std::thread _worker;
};

class InspectorTableModel
    : public QAbstractTableModel {
public:
    InspectorTableModel(const QStringList& headers,
                        QObject* parent)
        : QAbstractTableModel(parent)
        , _headers(headers)
    {
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : (int)_rows.size();
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : (int)_headers.size();
    }

    QVariant data(const QModelIndex& index,
                  int role) const override
    {
        if (!index.isValid() || (index.row() >= (int)_rows.size()) || (index.column() >= (int)_rows[index.row()].size())) {
            return QVariant();
        }
        const InspectorCell& cell = _rows[index.row()][index.column()];
        if (role == Qt::DisplayRole) {
            return cell.text;
        }
        if (role == Qt::ToolTipRole) {
            const QString tip = tipOf(cell);

            return tip.isEmpty() ? QVariant() : QVariant(tip);
        }
        if ((role == Qt::ForegroundRole) && _stale) {
            return _staleColor;
        }

        return QVariant();
    }

    QVariant headerData(int section,
                        Qt::Orientation orientation,
                        int role) const override
    {
        if ((orientation == Qt::Horizontal) && (role == Qt::DisplayRole) && (section >= 0) && (section < _headers.size())) {
            return _headers[section];
        }

        return QAbstractTableModel::headerData(section, orientation, role);
    }

    const InspectorCell* cellAt(const QModelIndex& index) const
    {
        if (!index.isValid() || (index.row() >= (int)_rows.size()) || (index.column() >= (int)_rows[index.row()].size())) {
            return nullptr;
        }

        return &_rows[index.row()][index.column()];
    }

    // The same rows at another frame or after a sync keep the view's scroll position and selection.
    void setRows(std::vector<InspectorRow> rows)
    {
        bool sameRows = (rows.size() == _rows.size());

        for (std::size_t i = 0; sameRows && (i < rows.size()); ++i) {
            sameRows = rows[i][0].text == _rows[i][0].text;
        }
        if (sameRows) {
            _rows = std::move(rows);
            if (!_rows.empty()) {
                Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
            }

            return;
        }
        beginResetModel();
        _rows = std::move(rows);
        endResetModel();
    }

    void setStale(bool stale,
                  const QColor& color)
    {
        if (stale == _stale) {
            return;
        }
        _stale = stale;
        _staleColor = color;
        if (!_rows.empty()) {
            Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), { Qt::ForegroundRole });
        }
    }

private:
    QStringList _headers;
    std::vector<InspectorRow> _rows;
    bool _stale = false;
    QColor _staleColor;
};

class InspectorTableTab
    : public ScenePrimInspectorTab {
public:
    typedef std::function<bool(const ScenePrimInspectorRead& read, std::vector<InspectorRow>* rows)> ReadRowsFunc;

    InspectorTableTab(const QString& title,
                      const QString& objectName,
                      const QStringList& headers,
                      const ReadRowsFunc& readRows)
        : _title(title)
        , _readRows(readRows)
        , _view(new QTreeView)
        , _model(new InspectorTableModel(headers, _view))
    {
        _view->setObjectName(objectName);
        _view->setModel(_model);
        _view->setRootIsDecorated(false);
        _view->setUniformRowHeights(true);
        _view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        _view->setSelectionBehavior(QAbstractItemView::SelectRows);
        _view->header()->setSectionResizeMode(QHeaderView::Interactive);
        QObject::connect(_view, &QTreeView::doubleClicked, _view, [this](const QModelIndex& index) {
            const InspectorCell* cell = _model->cellAt(index);
            if (!cell) {
                return;
            }
            const NodePtr node = cell->node.lock();
            if (node) {
                if (_openNode) {
                    _openNode(node);
                }
            } else if (cell->cut) {
                (new FullValueWindow(*cell, _view))->show();
            }
        });
        QObject::connect(_view->selectionModel(), &QItemSelectionModel::selectionChanged, _view, [this]() {
            if (!_rowSelected) {
                return;
            }
            const QModelIndexList selected = _view->selectionModel()->selectedRows();
            _rowSelected(selected.isEmpty() ? QString() : _model->data(selected.front(), Qt::DisplayRole).toString());
        });
    }

    void setOpenNode(const std::function<void(const NodePtr&)>& openNode)
    {
        _openNode = openNode;
    }

    void setRowSelected(const std::function<void(const QString&)>& rowSelected)
    {
        _rowSelected = rowSelected;
    }

    QString getTitle() const override
    {
        return _title;
    }

    QWidget* getWidget() const override
    {
        return _view;
    }

    std::function<void()> read(const ScenePrimInspectorRead& read) override
    {
        std::shared_ptr<std::vector<InspectorRow>> rows = std::make_shared<std::vector<InspectorRow>>();

        if (!_readRows(read, rows.get())) {
            return std::function<void()>();
        }

        return [this, rows]() { _model->setRows(std::move(*rows)); };
    }

    void setStale(bool stale) override
    {
        QColor color = _view->palette().color(QPalette::Text);

        color.setAlphaF(0.4);
        _model->setStale(stale, color);
    }

    void clear() override
    {
        _model->setRows(std::vector<InspectorRow>());
    }

    void clearSelection() override
    {
        _view->selectionModel()->clear();
    }

private:
    QString _title;
    ReadRowsFunc _readRows;
    QTreeView* _view;
    InspectorTableModel* _model;
    std::function<void(const NodePtr&)> _openNode;
    std::function<void(const QString&)> _rowSelected;
};

enum LayerMode {
    eLayerModeNodeContribution,
    eLayerModePrimDefinition
};

QString
withLongLinesBroken(const QString& text)
{
    QString broken;
    qsizetype start = 0;

    broken.reserve(text.size() + text.size() / kMaxShownLineLength + 1);
    while (start < text.size()) {
        const qsizetype newline = text.indexOf(QLatin1Char('\n'), start);
        const qsizetype end = (newline < 0) ? text.size() : newline;
        for (qsizetype i = start; i < end; i += kMaxShownLineLength) {
            if (i > start) {
                broken += QLatin1Char('\n');
            }
            broken += QStringView(text).mid(i, std::min<qsizetype>(kMaxShownLineLength, end - i));
        }
        if (newline >= 0) {
            broken += QLatin1Char('\n');
        }
        start = end + 1;
    }

    return broken;
}

// The USDA of the shown node's layers, or of the selected prim's composed definition. The text is
// built only by read(), which the inspector calls while this tab is the one showing.
class LayerTextTab
    : public ScenePrimInspectorTab {
public:
    explicit LayerTextTab(std::atomic<qulonglong>* definitionReads)
        : _widget(new QWidget)
        , _mode(new QComboBox(_widget))
        , _text(new QPlainTextEdit(_widget))
        , _loader(_text)
        , _definitionReads(definitionReads)
    {
        _widget->setObjectName(QString::fromUtf8("sceneGraphLayer"));
        _mode->setObjectName(QString::fromUtf8("sceneGraphLayerMode"));
        _mode->addItem(ScenePrimInspector::tr("Node Contribution"), (int)eLayerModeNodeContribution);
        _mode->addItem(ScenePrimInspector::tr("Prim Composed"), (int)eLayerModePrimDefinition);
        _mode->setToolTip(ScenePrimInspector::tr("Node Contribution: the shown node's own layers as USDA. "
                                                 "Prim Composed: the selected prim's composed definition as USDA, without its descendants."));
        _text->setObjectName(QString::fromUtf8("sceneGraphLayerText"));
        _text->setReadOnly(true);
        _text->setLineWrapMode(QPlainTextEdit::NoWrap);
        _text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        QVBoxLayout* layout = new QVBoxLayout(_widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        layout->addWidget(_mode);
        layout->addWidget(_text, 1);
        QObject::connect(_mode, &QComboBox::currentIndexChanged, _widget, [this]() {
            _modeValue = _mode->currentData().toInt();
            if (_requestRead) {
                _requestRead();
            }
        });
    }

    QString getTitle() const override
    {
        return ScenePrimInspector::tr("Layer");
    }

    QWidget* getWidget() const override
    {
        return _widget;
    }

    void setReadRequester(const std::function<void()>& requestRead) override
    {
        _requestRead = requestRead;
    }

    bool readsWithoutPrim() const override
    {
        return _modeValue == eLayerModeNodeContribution;
    }

    std::function<void()> read(const ScenePrimInspectorRead& read) override
    {
        const int mode = _modeValue;
        const std::shared_ptr<const std::string> source = (mode == eLayerModeNodeContribution) ? readNodeContribution(read) : readPrimDefinition(read);

        if (!source) {
            return std::function<void()>();
        }
        const std::shared_ptr<const QString> shown = shownTextOf(source, mode);

        if (read.isCancelled()) {
            return std::function<void()>();
        }

        return [this, shown, mode]() { show(shown, mode); };
    }

    void setStale(bool stale) override
    {
        QPalette palette = _text->palette();
        QColor color = QApplication::palette().color(QPalette::Text);

        if (stale) {
            color.setAlphaF(0.4);
        }
        palette.setColor(QPalette::Text, color);
        _text->setPalette(palette);
    }

    void clear() override
    {
        _shownMode = -1;
        _shownText.reset();
        _loader.clear();
    }

private:
    // Null when the read was cancelled.
    std::shared_ptr<const std::string> readNodeContribution(const ScenePrimInspectorRead& read)
    {
        const U64 editCount = read.getEditCount();
        ScenePayload payload;

        if (!read.readStage([&payload](const SceneStage* stage) {
                if (stage) {
                    payload = stage->getPayload();
                }
            })) {
            return std::shared_ptr<const std::string>();
        }
        const auto layers = SceneOps::getOwnedLayers(payload, read.getNode());
        const std::vector<const void*> identity = SceneOps::getLayerIdentities(layers);

        if (_cachedText && (_cachedIdentity == identity) && (_cachedEditCount == editCount)) {
            return _cachedText;
        }
        std::shared_ptr<const std::string> text = std::make_shared<const std::string>(SceneOps::exportLayersText(layers));

        if (read.isCancelled()) {
            return std::shared_ptr<const std::string>();
        }
        _cachedText = text;
        _cachedIdentity = identity;
        _cachedEditCount = editCount;

        return text;
    }

    // The lock is held per property: an attribute can hold millions of samples, and the main
    // thread may be waiting for it. A prim's definition does not vary with the frame, so playback
    // reuses it.
    std::shared_ptr<const std::string> readPrimDefinition(const ScenePrimInspectorRead& read)
    {
        const U64 editCount = read.getEditCount();

        if (_definitionKey.matches(read, editCount)) {
            return _cachedDefinition;
        }
        const std::string path = read.getPath().toStdString();
        PXR_NS::UsdStageRefPtr definition;
        std::vector<std::string> names;

        if (!read.readStage([&](const SceneStage* stage) {
                if (stage) {
                    ++*_definitionReads;
                    definition = SceneOps::beginPrimDefinition(*stage, path, &names);
                }
            })) {
            return std::shared_ptr<const std::string>();
        }
        for (const std::string& name : names) {
            if (!read.readStage([&](const SceneStage* stage) {
                    if (stage) {
                        SceneOps::flattenPropertyInto(definition, *stage, path, name);
                    }
                })) {
                return std::shared_ptr<const std::string>();
            }
        }
        std::shared_ptr<const std::string> text = std::make_shared<const std::string>(SceneOps::exportDefinitionText(definition));

        if (read.isCancelled()) {
            return std::shared_ptr<const std::string>();
        }
        _definitionKey.set(read, editCount);
        _cachedDefinition = text;

        return text;
    }

    // Reuses the last result's pointer for the same text, which is all show() has to compare.
    std::shared_ptr<const QString> shownTextOf(const std::shared_ptr<const std::string>& source,
                                               int mode)
    {
        if ((source == _convertedSource) && (mode == _convertedMode)) {
            return _converted;
        }
        QString text = QString::fromStdString(*source);

        if (text.isEmpty()) {
            text = (mode == eLayerModeNodeContribution) ? ScenePrimInspector::tr("# The shown node authors no layer.")
                                                        : ScenePrimInspector::tr("# The selected prim has no composed definition.");
        }
        text = withLongLinesBroken(text);
        _convertedSource = source;
        if (!_converted || (mode != _convertedMode) || (text != *_converted)) {
            _converted = std::make_shared<const QString>(std::move(text));
            _convertedMode = mode;
        }

        return _converted;
    }

    void show(const std::shared_ptr<const QString>& text,
              int mode)
    {
        if ((mode == _shownMode) && (text == _shownText)) {
            return;
        }
        _shownMode = mode;
        _shownText = text;
        _loader.show(text);
    }

    QWidget* _widget;
    QComboBox* _mode;
    QPlainTextEdit* _text;
    SlicedTextLoader _loader;
    std::atomic<qulonglong>* _definitionReads;
    std::function<void()> _requestRead;
    std::atomic<int> _modeValue { eLayerModeNodeContribution };
    int _shownMode = -1;
    std::shared_ptr<const QString> _shownText;
    // Worker only.
    std::shared_ptr<const std::string> _cachedText;
    std::vector<const void*> _cachedIdentity;
    U64 _cachedEditCount = 0;
    InspectorReadKey _definitionKey;
    std::shared_ptr<const std::string> _cachedDefinition;
    std::shared_ptr<const std::string> _convertedSource;
    std::shared_ptr<const QString> _converted;
    int _convertedMode = -1;
};

} // anon namespace

ScenePrimInspectorRead::ScenePrimInspectorRead(const SceneGraphStageSourcePtr& source,
                                               const QString& path,
                                               const QString& property,
                                               double frame,
                                               double framesPerSecond,
                                               const std::atomic<U64>& latestGeneration,
                                               U64 generation,
                                               const NodePtr& node)
    : _source(source)
    , _path(path)
    , _property(property)
    , _frame(frame)
    , _framesPerSecond(framesPerSecond)
    , _latestGeneration(latestGeneration)
    , _generation(generation)
    , _node(node)
{
}

const QString&
ScenePrimInspectorRead::getPath() const
{
    return _path;
}

const QString&
ScenePrimInspectorRead::getProperty() const
{
    return _property;
}

const NodePtr&
ScenePrimInspectorRead::getNode() const
{
    return _node;
}

const SceneGraphStageSourcePtr&
ScenePrimInspectorRead::getSource() const
{
    return _source;
}

U64
ScenePrimInspectorRead::getEditCount() const
{
    return _source->getEditCount();
}

double
ScenePrimInspectorRead::getFrame() const
{
    return _frame;
}

double
ScenePrimInspectorRead::getFramesPerSecond() const
{
    return _framesPerSecond;
}

bool
ScenePrimInspectorRead::isCancelled() const
{
    return _latestGeneration != _generation;
}

bool
ScenePrimInspectorRead::readStage(const std::function<void(const SceneStage* stage)>& read) const
{
    if (isCancelled()) {
        return false;
    }
    bool ran = false;
    _source->readStage([&](const SceneStage* stage) {
        if (!isCancelled()) {
            ran = true;
            read(stage);
        }
    });

    return ran;
}

struct ScenePrimInspector::Implementation {
    struct Request {
        SceneGraphStageSourcePtr source;
        QString path;
        QString property;
        double frame = 0.;
        double framesPerSecond = 24.;
        NodePtr node;
        ScenePrimInspectorTab* tab = nullptr;
        U64 generation = 0;
    };

    ScenePrimInspector* inspector;
    QLabel* primLabel = nullptr;
    QTabWidget* tabWidget = nullptr;
    std::vector<std::unique_ptr<ScenePrimInspectorTab>> tabs;
    QTimer greyTimer;

    SceneGraphStageSourcePtr source;
    QString path;
    NodePtr node;
    // The property the Opinions tab lists the specs of, besides the prim's.
    QString property;
    std::function<void(const NodePtr&)> openNode;
    double frame = 0.;
    double framesPerSecond = 24.;
    bool readOwed = false;
    bool busy = false;
    bool stale = false;
    QString shownPath;
    qulonglong readsStarted = 0;
    qulonglong readsFinished = 0;
    qulonglong readsCancelled = 0;
    std::atomic<qulonglong> compositionQueries { 0 };
    std::atomic<qulonglong> definitionReads { 0 };

    std::atomic<U64> generation { 0 };
    bool holdReads = false;
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    bool hasRequest = false;
    bool quit = false;
    Request request;
    std::thread worker;

    explicit Implementation(ScenePrimInspector* inspector)
        : inspector(inspector)
    {
    }

    void publishState()
    {
        inspector->setProperty("natronSceneGraphInspectorPath", shownPath);
        inspector->setProperty("natronSceneGraphInspectorBusy", busy);
        inspector->setProperty("natronSceneGraphInspectorStale", stale);
        inspector->setProperty("natronSceneGraphInspectorReadsStarted", readsStarted);
        inspector->setProperty("natronSceneGraphInspectorReadsFinished", readsFinished);
        inspector->setProperty("natronSceneGraphInspectorReadsCancelled", readsCancelled);
        inspector->setProperty("natronSceneGraphInspectorCompositionQueries", compositionQueries.load());
        inspector->setProperty("natronSceneGraphInspectorDefinitionReads", definitionReads.load());
    }

    void setStale(bool newStale)
    {
        stale = newStale;
        primLabel->setEnabled(!newStale);
        for (const std::unique_ptr<ScenePrimInspectorTab>& tab : tabs) {
            tab->setStale(newStale);
        }
        publishState();
    }

    void setShownPath(const QString& newPath)
    {
        shownPath = newPath;
        primLabel->setText(newPath);
        primLabel->setToolTip(newPath);
    }

    void cancelRead()
    {
        ++generation;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            hasRequest = false;
            request = Request();
        }
        queueChanged.notify_all();
    }

    void setHoldReads(bool hold)
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            holdReads = hold;
        }
        queueChanged.notify_all();
    }

    void startRead()
    {
        cancelRead();
        if (!inspector->isVisible()) {
            readOwed = true;
            busy = false;
            greyTimer.stop();
            publishState();

            return;
        }
        readOwed = false;
        const int current = tabWidget->currentIndex();
        if (!source || (current < 0) || (path.isEmpty() && !tabs[current]->readsWithoutPrim())) {
            busy = false;
            greyTimer.stop();
            for (const std::unique_ptr<ScenePrimInspectorTab>& tab : tabs) {
                tab->clear();
            }
            setShownPath(QString());
            setStale(false);

            return;
        }
        busy = true;
        ++readsStarted;
        // Not restarted, or reads replacing each other during playback would never grey the rows.
        if (!stale && !greyTimer.isActive()) {
            greyTimer.start();
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            request.source = source;
            request.path = path;
            request.node = node;
            request.property = property;
            request.frame = frame;
            request.framesPerSecond = framesPerSecond;
            request.tab = tabs[current].get();
            request.generation = generation;
            hasRequest = true;
        }
        queueChanged.notify_one();
        publishState();
    }

    void onReadEnded(U64 readGeneration,
                     const QString& readPath,
                     const std::function<void()>& show)
    {
        if (!show || (readGeneration != generation)) {
            ++readsCancelled;
            publishState();

            return;
        }
        show();
        ++readsFinished;
        busy = false;
        greyTimer.stop();
        setShownPath(readPath);
        setStale(false);
    }

    void run()
    {
        for (;;) {
            Request next;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueChanged.wait(lock, [this]() { return quit || hasRequest; });
                if (quit) {
                    return;
                }
                next = std::move(request);
                request = Request();
                hasRequest = false;
                queueChanged.wait(lock, [this, &next]() { return quit || !holdReads || (next.generation != generation); });
                if (quit) {
                    return;
                }
            }
            std::function<void()> show;
            if (next.generation == generation) {
                const ScenePrimInspectorRead read(next.source, next.path, next.property, next.frame, next.framesPerSecond, generation, next.generation, next.node);
                show = next.tab->read(read);
            }
            next.source.reset();
            next.node.reset();
            const U64 readGeneration = next.generation;
            const QString readPath = next.path;
            QMetaObject::invokeMethod(inspector, [this, readGeneration, readPath, show]() { onReadEnded(readGeneration, readPath, show); }, Qt::QueuedConnection);
        }
    }
};

ScenePrimInspector::ScenePrimInspector(QWidget* parent)
    : QWidget(parent)
    , _imp(new Implementation(this))
{
    setObjectName(QString::fromUtf8("sceneGraphInspector"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    _imp->primLabel = new QLabel(this);
    _imp->primLabel->setObjectName(QString::fromUtf8("sceneGraphInspectorPrim"));
    _imp->primLabel->setContentsMargins(4, 0, 4, 0);
    // A long prim path must not set the panel's minimum width.
    _imp->primLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(_imp->primLabel);

    _imp->tabWidget = new QTabWidget(this);
    _imp->tabWidget->setObjectName(QString::fromUtf8("sceneGraphInspectorTabs"));
    layout->addWidget(_imp->tabWidget, 1);

    _imp->greyTimer.setSingleShot(true);
    _imp->greyTimer.setInterval(kGreyAfterMs);
    QObject::connect(&_imp->greyTimer, &QTimer::timeout, this, [this]() {
        if (_imp->busy) {
            _imp->setStale(true);
        }
    });
    QObject::connect(_imp->tabWidget, &QTabWidget::currentChanged, this, [this]() { _imp->startRead(); });

    const auto addTableTab = [this](const QString& title,
                                    const char* objectName,
                                    const QStringList& headers,
                                    const InspectorTableTab::ReadRowsFunc& readRows,
                                    bool selectsProperty) {
        InspectorTableTab* tab = new InspectorTableTab(title, QString::fromUtf8(objectName), headers, readRows);
        tab->setOpenNode([this](const NodePtr& node) {
            if (_imp->openNode) {
                _imp->openNode(node);
            }
        });
        if (selectsProperty) {
            tab->setRowSelected([this](const QString& name) { _imp->property = name; });
        }
        addTab(std::unique_ptr<ScenePrimInspectorTab>(tab));
    };
    addTableTab(tr("Attributes"), "sceneGraphAttributes",
                QStringList() << tr("Attribute") << tr("Type") << tr("Value") << tr("Source") << tr("Authored By"), readAttributes, true);
    addTableTab(tr("Relationships"), "sceneGraphRelationships",
                QStringList() << tr("Relationship") << tr("Targets") << tr("Authored By"), readRelationships, true);
    addTableTab(tr("Metadata"), "sceneGraphMetadata",
                QStringList() << tr("Field") << tr("Value") << tr("Authored By"), readMetadata, false);
    const std::shared_ptr<CompositionCache> compositionCache = std::make_shared<CompositionCache>();
    addTableTab(tr("Composition"), "sceneGraphComposition", QStringList() << tr("Arc") << tr("Target Prim") << tr("Target Layer") << tr("Introduced By") << tr("Notes"), [this, compositionCache](const ScenePrimInspectorRead& read, std::vector<InspectorRow>* rows) { return readComposition(read, rows, compositionCache.get(), &_imp->compositionQueries); }, false);
    addTableTab(tr("Opinions"), "sceneGraphOpinions",
                QStringList() << tr("Opinion") << tr("Authored By") << tr("Layer") << tr("Value") << tr("Layer Offset"), readOpinions, false);
    addTab(std::make_unique<LayerTextTab>(&_imp->definitionReads));
    _imp->publishState();
    _imp->worker = std::thread([this]() { _imp->run(); });
}

ScenePrimInspector::~ScenePrimInspector()
{
    stop();
}

void
ScenePrimInspector::stop()
{
    ++_imp->generation;
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->quit = true;
    }
    _imp->queueChanged.notify_all();
    if (_imp->worker.joinable()) {
        _imp->worker.join();
    }
}

void
ScenePrimInspector::setNodeOpener(const std::function<void(const NodePtr& node)>& openNode)
{
    _imp->openNode = openNode;
}

void
ScenePrimInspector::setNode(const NodePtr& node)
{
    _imp->node = node;
}

void
ScenePrimInspector::addTab(std::unique_ptr<ScenePrimInspectorTab> tab)
{
    ScenePrimInspectorTab* added = tab.get();

    // Before the tab widget knows it: adding the first tab changes the current tab at once.
    _imp->tabs.push_back(std::move(tab));
    added->setReadRequester([this]() { _imp->startRead(); });
    _imp->tabWidget->addTab(added->getWidget(), added->getTitle());
}

void
ScenePrimInspector::setPrim(const SceneGraphStageSourcePtr& source,
                            const QString& path,
                            double frame,
                            double framesPerSecond)
{
    _imp->source = source;
    if (path != _imp->path) {
        _imp->property.clear();
        for (const std::unique_ptr<ScenePrimInspectorTab>& tab : _imp->tabs) {
            tab->clearSelection();
        }
    }
    _imp->path = path;
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
    _imp->startRead();
}

bool
ScenePrimInspector::event(QEvent* e)
{
    if (e->type() == QEvent::DynamicPropertyChange) {
        QDynamicPropertyChangeEvent* change = static_cast<QDynamicPropertyChangeEvent*>(e);
        if (change->propertyName() == "natronSceneGraphInspectorHoldReads") {
            _imp->setHoldReads(property("natronSceneGraphInspectorHoldReads").toBool());
        }
    }

    return QWidget::event(e);
}

void
ScenePrimInspector::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (_imp->readOwed) {
        _imp->startRead();
    }
}

void
ScenePrimInspector::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    if (_imp->busy) {
        _imp->cancelRead();
        _imp->busy = false;
        _imp->readOwed = true;
        _imp->greyTimer.stop();
        _imp->publishState();
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ScenePrimInspector.cpp"
