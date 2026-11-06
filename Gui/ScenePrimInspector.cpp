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

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractTableModel>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

using SceneOps::SceneOpinion;
using SceneOps::ScenePrimArc;
using SceneOps::ScenePrimComposition;

const int kMaxCellLength = 200;

// A plain-text tooltip never wraps by itself, so a long value would run off a 1920 pixel screen.
const int kTipLineLength = 160;

// Long enough that a read finishing normally does not flicker the rows.
const int kGreyAfterMs = 100;

struct InspectorCell {
    QString text;
    QString tip;
    // The node a double-click opens the settings of.
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

InspectorCell
cellOf(const QString& full)
{
    InspectorCell cell;
    const bool cut = full.size() > kMaxCellLength;

    cell.text = cut ? (full.left(kMaxCellLength) + QString::fromUtf8("...")) : full;
    // Uniform row heights would clip a value's later lines.
    cell.text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    cell.tip = wrappedForTip(full);

    return cell;
}

QString
valueText(const VtValue& value)
{
    if (value.IsEmpty()) {
        return QString();
    }
    std::ostringstream os;
    os << value;

    return QString::fromStdString(os.str());
}

QString
sourceLabel(UsdResolveInfoSource source)
{
    switch (source) {
    case UsdResolveInfoSourceFallback:
        return ScenePrimInspector::tr("Fallback");
    case UsdResolveInfoSourceDefault:
        return ScenePrimInspector::tr("Default Value");
    case UsdResolveInfoSourceTimeSamples:
        return ScenePrimInspector::tr("Time Samples");
    case UsdResolveInfoSourceValueClips:
        return ScenePrimInspector::tr("Value Clips");
    case UsdResolveInfoSourceSpline:
        return ScenePrimInspector::tr("Spline");
    case UsdResolveInfoSourceNone:
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
    } else if (!layerIdentifier.empty() && !SdfLayer::IsAnonymousLayerIdentifier(layerIdentifier)) {
        cell.text = QString::fromStdString(SdfLayer::SplitIdentifier(layerIdentifier).first);
    }
    cell.tip = QString::fromStdString(layerIdentifier);
    cell.node = node;

    return cell;
}

InspectorCell
layerAuthorCell(const SdfLayerHandle& layer,
                bool schemaFallback)
{
    return authorCell(SceneOps::getLayerOwner(layer), layer ? layer->GetIdentifier() : std::string(), !layer && schemaFallback);
}

// The prim is valid only inside the callback: with the lock released, a sync can recompose it away.
bool
readPrim(const ScenePrimInspectorRead& read,
         const std::function<void(const SceneStage& stage, const UsdPrim& prim)>& readThePrim)
{
    const std::string pathString = read.getPath().toStdString();

    return read.readStage([&](const SceneStage* sceneStage) {
        if (!sceneStage || !SdfPath::IsValidPathString(pathString)) {
            return;
        }
        const UsdPrim prim = sceneStage->getStage()->GetPrimAtPath(SdfPath(pathString));
        if (prim) {
            readThePrim(*sceneStage, prim);
        }
    });
}

// The lock is taken per row, and a value is turned into text outside it: an attribute can hold
// millions of points, and the main thread may be waiting for the lock. A VtValue shares its data
// by reference counts and needs no lock. A sync between two rows starts a new read anyway.
bool
readAttributes(const ScenePrimInspectorRead& read,
               std::vector<InspectorRow>* rows)
{
    std::vector<TfToken> names;

    if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) {
            for (const UsdAttribute& attribute : prim.GetAttributes()) {
                names.push_back(attribute.GetName());
            }
        })) {
        return false;
    }
    rows->reserve(names.size());
    for (const TfToken& name : names) {
        bool defined = false;
        QString type;
        QString source;
        VtValue value;
        InspectorCell author;
        if (!readPrim(read, [&](const SceneStage& stage, const UsdPrim& prim) {
                const UsdAttribute attribute = prim.GetAttribute(name);
                if (!attribute.IsDefined()) {
                    return;
                }
                defined = true;
                const UsdTimeCode time(SceneOps::frameToTimeCode(stage.getPayload(), read.getFrame(), read.getFramesPerSecond()));
                type = QString::fromStdString(attribute.GetTypeName().GetAsToken().GetString());
                attribute.Get(&value, time);
                const UsdResolveInfoSource resolved = attribute.GetResolveInfo(time).GetSource();
                source = sourceLabel(resolved);
                std::string layerIdentifier;
                const NodePtr node = SceneOps::authoringNodeOf(attribute, time, &layerIdentifier);
                author = authorCell(node, layerIdentifier, resolved == UsdResolveInfoSourceFallback);
            })) {
            return false;
        }
        if (defined) {
            rows->push_back({ cellOf(QString::fromStdString(name.GetString())), cellOf(type), cellOf(valueText(value)), cellOf(source), author });
        }
    }

    return true;
}

// The strongest spec authoring targets, else the strongest declaring the relationship.
SdfLayerHandle
targetsLayer(const UsdRelationship& relationship)
{
    SdfLayerHandle declaring;

    for (const SdfPropertySpecHandle& spec : relationship.GetPropertyStack()) {
        if (!spec) {
            continue;
        }
        if (spec->HasField(SdfFieldKeys->TargetPaths)) {
            return spec->GetLayer();
        }
        if (!declaring) {
            declaring = spec->GetLayer();
        }
    }

    return declaring;
}

bool
readRelationships(const ScenePrimInspectorRead& read,
                  std::vector<InspectorRow>* rows)
{
    std::vector<TfToken> names;

    if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) {
            for (const UsdRelationship& relationship : prim.GetRelationships()) {
                names.push_back(relationship.GetName());
            }
        })) {
        return false;
    }
    rows->reserve(names.size());
    for (const TfToken& name : names) {
        bool defined = false;
        QStringList targetTexts;
        InspectorCell author;
        if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) {
                const UsdRelationship relationship = prim.GetRelationship(name);
                if (!relationship.IsDefined()) {
                    return;
                }
                defined = true;
                SdfPathVector targets;
                relationship.GetTargets(&targets);
                for (const SdfPath& target : targets) {
                    targetTexts << QString::fromStdString(target.GetString());
                }
                author = layerAuthorCell(targetsLayer(relationship), true);
            })) {
            return false;
        }
        if (defined) {
            rows->push_back({ cellOf(QString::fromStdString(name.GetString())), cellOf(targetTexts.join(QString::fromUtf8(", "))), author });
        }
    }

    return true;
}

// A prim's specifier composes to its strongest non-over, whichever layer last wrote an over.
SdfLayerHandle
metadataLayer(const SdfPrimSpecHandleVector& stack,
              const TfToken& key)
{
    const bool isSpecifier = (key == SdfFieldKeys->Specifier);
    SdfLayerHandle strongest;

    for (const SdfPrimSpecHandle& spec : stack) {
        if (!spec || !spec->HasField(key)) {
            continue;
        }
        if (!strongest) {
            strongest = spec->GetLayer();
        }
        if (!isSpecifier || (spec->GetSpecifier() != SdfSpecifierOver)) {
            return spec->GetLayer();
        }
    }

    return strongest;
}

bool
readMetadata(const ScenePrimInspectorRead& read,
             std::vector<InspectorRow>* rows)
{
    struct Entry {
        std::string key;
        VtValue value;
        InspectorCell author;
    };
    std::vector<Entry> entries;
    QString prototype;

    if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) {
            const UsdMetadataValueMap metadata = prim.GetAllMetadata();
            const SdfPrimSpecHandleVector stack = prim.GetPrimStack();
            // A prototype has no prim index, so its empty stack says nothing of who authored its
            // metadata.
            const bool schemaFallback = !prim.IsPrototype();
            entries.reserve(metadata.size());
            for (const auto& item : metadata) {
                entries.push_back({ item.first.GetString(), item.second, layerAuthorCell(metadataLayer(stack, item.first), schemaFallback) });
            }
            if (prim.IsInstance()) {
                prototype = QString::fromStdString(prim.GetPrototype().GetPath().GetString());
            } else if (prim.IsInstanceProxy()) {
                prototype = QString::fromStdString(prim.GetPrimInPrototype().GetPath().GetString());
            } else if (prim.IsPrototype()) {
                prototype = ScenePrimInspector::tr("This prim is a prototype");
            }
        })) {
        return false;
    }
    rows->reserve(entries.size() + 1);
    if (!prototype.isEmpty()) {
        rows->push_back({ cellOf(ScenePrimInspector::tr("Prototype")), cellOf(prototype), InspectorCell() });
    }
    for (const Entry& entry : entries) {
        if (read.isCancelled()) {
            return false;
        }
        rows->push_back({ cellOf(QString::fromStdString(entry.key)), cellOf(valueText(entry.value)), entry.author });
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

InspectorRow
opinionRow(const QString& scope,
           const SceneOpinion& opinion)
{
    QString value;

    if (!opinion.primDescription.empty()) {
        value = QString::fromStdString(opinion.primDescription);
    } else if (opinion.numTimeSamples > 0) {
        value = ScenePrimInspector::tr("%n time sample(s)", nullptr, (int)opinion.numTimeSamples);
    } else if (opinion.hasSpline) {
        value = ScenePrimInspector::tr("Spline");
    } else {
        value = valueText(opinion.value);
    }

    return { cellOf(scope),
             authorCell(opinion.owner, opinion.layerIdentifier, false),
             cellOf(QString::fromStdString(opinion.layerIdentifier)),
             cellOf(value),
             cellOf(layerOffsetText(opinion)) };
}

bool
readOpinions(const ScenePrimInspectorRead& read,
             std::vector<InspectorRow>* rows)
{
    std::vector<SceneOpinion> primOpinions;
    std::vector<SceneOpinion> propertyOpinions;
    const std::string property = read.getProperty().toStdString();

    if (!readPrim(read, [&](const SceneStage& stage, const UsdPrim& prim) {
            primOpinions = SceneOps::getPrimOpinions(prim);
            if (!property.empty()) {
                const UsdTimeCode time(SceneOps::frameToTimeCode(stage.getPayload(), read.getFrame(), read.getFramesPerSecond()));
                propertyOpinions = SceneOps::getPropertyOpinions(prim, TfToken(property), time);
            }
        })) {
        return false;
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

bool
readComposition(const ScenePrimInspectorRead& read,
                std::vector<InspectorRow>* rows)
{
    ScenePrimComposition composition;

    if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) { composition = SceneOps::getPrimComposition(prim); })) {
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

    return true;
}

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
        if ((role == Qt::ToolTipRole) && !cell.tip.isEmpty()) {
            return cell.tip;
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

    // The node of the cell at index, else of the first cell of its row that has one.
    NodePtr getNode(const QModelIndex& index) const
    {
        if (!index.isValid() || (index.row() >= (int)_rows.size())) {
            return NodePtr();
        }
        const InspectorRow& row = _rows[index.row()];
        if ((index.column() < (int)row.size()) && row[index.column()].node.lock()) {
            return row[index.column()].node.lock();
        }
        for (const InspectorCell& cell : row) {
            if (NodePtr node = cell.node.lock()) {
                return node;
            }
        }

        return NodePtr();
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
            const NodePtr node = _model->getNode(index);
            if (node && _openNode) {
                _openNode(node);
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

    // Called with the first column of the selected row, or empty when none is.
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

// The USDA of the shown node's layers, or of the selected prim's composed definition. The text is
// built only by read(), which the inspector calls while this tab is the one showing.
class LayerTextTab
    : public ScenePrimInspectorTab {
public:
    LayerTextTab()
        : _widget(new QWidget)
        , _mode(new QComboBox(_widget))
        , _text(new QPlainTextEdit(_widget))
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

    std::function<void()> read(const ScenePrimInspectorRead& read) override
    {
        const int mode = _modeValue;
        std::shared_ptr<const std::string> text;

        if (mode == eLayerModeNodeContribution) {
            text = readNodeContribution(read);
        } else {
            text = readPrimDefinition(read);
        }
        if (!text) {
            return std::function<void()>();
        }

        return [this, text, mode]() { show(*text, mode); };
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
        _text->clear();
    }

private:
    // Null when the read was cancelled.
    std::shared_ptr<const std::string> readNodeContribution(const ScenePrimInspectorRead& read)
    {
        ScenePayload payload;

        if (!read.readStage([&payload](const SceneStage* stage) {
                if (stage) {
                    payload = stage->getPayload();
                }
            })) {
            return std::shared_ptr<const std::string>();
        }
        const std::vector<SdfLayerRefPtr> layers = SceneOps::getOwnedLayers(payload, read.getNode());
        std::vector<const SdfLayer*> identity;

        identity.reserve(layers.size());
        for (const SdfLayerRefPtr& layer : layers) {
            identity.push_back(get_pointer(layer));
        }
        const U64 editCount = read.getEditCount();

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
    // thread may be waiting for it.
    static std::shared_ptr<const std::string> readPrimDefinition(const ScenePrimInspectorRead& read)
    {
        UsdStageRefPtr definition;
        std::vector<TfToken> names;

        if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) {
                definition = SceneOps::beginPrimDefinition(prim);
                for (const UsdProperty& property : prim.GetAuthoredProperties()) {
                    names.push_back(property.GetName());
                }
            })) {
            return std::shared_ptr<const std::string>();
        }
        for (const TfToken& name : names) {
            if (!readPrim(read, [&](const SceneStage&, const UsdPrim& prim) { SceneOps::flattenPropertyInto(definition, prim.GetProperty(name)); })) {
                return std::shared_ptr<const std::string>();
            }
        }
        const std::string text = SceneOps::exportDefinitionText(definition);

        if (read.isCancelled()) {
            return std::shared_ptr<const std::string>();
        }

        return std::make_shared<const std::string>(text);
    }

    void show(const std::string& text,
              int mode)
    {
        QString shown = QString::fromStdString(text);

        if (shown.isEmpty()) {
            shown = (mode == eLayerModeNodeContribution) ? ScenePrimInspector::tr("# The shown node authors no layer.")
                                                         : ScenePrimInspector::tr("# The selected prim has no composed definition.");
        }
        if ((mode == _shownMode) && (shown == _shownText)) {
            return;
        }
        _shownMode = mode;
        _shownText = shown;
        _text->setPlainText(shown);
    }

    QWidget* _widget;
    QComboBox* _mode;
    QPlainTextEdit* _text;
    std::function<void()> _requestRead;
    std::atomic<int> _modeValue { eLayerModeNodeContribution };
    int _shownMode = -1;
    QString _shownText;
    // Worker only.
    std::shared_ptr<const std::string> _cachedText;
    std::vector<const SdfLayer*> _cachedIdentity;
    U64 _cachedEditCount = 0;
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
    // A read is owed for when the inspector is shown again.
    bool readOwed = false;
    bool busy = false;
    bool stale = false;
    QString shownPath;
    qulonglong readsStarted = 0;
    qulonglong readsFinished = 0;
    qulonglong readsCancelled = 0;

    std::atomic<U64> generation { 0 };
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
        std::lock_guard<std::mutex> lock(queueMutex);
        hasRequest = false;
        request = Request();
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
        if (!source || path.isEmpty() || (current < 0)) {
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
            }
            std::function<void()> show;
            {
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
    addTableTab(tr("Composition"), "sceneGraphComposition",
                QStringList() << tr("Arc") << tr("Target Prim") << tr("Target Layer") << tr("Introduced By") << tr("Notes"), readComposition, false);
    addTableTab(tr("Opinions"), "sceneGraphOpinions",
                QStringList() << tr("Opinion") << tr("Authored By") << tr("Layer") << tr("Value") << tr("Layer Offset"), readOpinions, false);
    addTab(std::make_unique<LayerTextTab>());
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
    }
    _imp->path = path;
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
    _imp->startRead();
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
