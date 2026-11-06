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

#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractTableModel>
#include <QColor>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
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

const int kMaxCellLength = 200;

// A plain-text tooltip never wraps by itself, so a long value would run off a 1920 pixel screen.
const int kTipLineLength = 160;

// Long enough that a read finishing normally does not flicker the rows.
const int kGreyAfterMs = 100;

struct InspectorCell {
    QString text;
    QString tip;
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
};

} // anon namespace

ScenePrimInspectorRead::ScenePrimInspectorRead(const SceneGraphStageSourcePtr& source,
                                               const QString& path,
                                               double frame,
                                               double framesPerSecond,
                                               const std::atomic<U64>& latestGeneration,
                                               U64 generation)
    : _source(source)
    , _path(path)
    , _frame(frame)
    , _framesPerSecond(framesPerSecond)
    , _latestGeneration(latestGeneration)
    , _generation(generation)
{
}

const QString&
ScenePrimInspectorRead::getPath() const
{
    return _path;
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
        double frame = 0.;
        double framesPerSecond = 24.;
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
                const ScenePrimInspectorRead read(next.source, next.path, next.frame, next.framesPerSecond, generation, next.generation);
                show = next.tab->read(read);
            }
            next.source.reset();
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

    addTab(std::unique_ptr<ScenePrimInspectorTab>(new InspectorTableTab(tr("Attributes"), QString::fromUtf8("sceneGraphAttributes"),
                                                                        QStringList() << tr("Attribute") << tr("Type") << tr("Value") << tr("Source") << tr("Authored By"),
                                                                        readAttributes)));
    addTab(std::unique_ptr<ScenePrimInspectorTab>(new InspectorTableTab(tr("Relationships"), QString::fromUtf8("sceneGraphRelationships"),
                                                                        QStringList() << tr("Relationship") << tr("Targets") << tr("Authored By"),
                                                                        readRelationships)));
    addTab(std::unique_ptr<ScenePrimInspectorTab>(new InspectorTableTab(tr("Metadata"), QString::fromUtf8("sceneGraphMetadata"),
                                                                        QStringList() << tr("Field") << tr("Value") << tr("Authored By"),
                                                                        readMetadata)));
    addTab(std::unique_ptr<ScenePrimInspectorTab>(new InspectorTableTab(tr("Composition"), QString::fromUtf8("sceneGraphComposition"),
                                                                        QStringList() << tr("Arc") << tr("Target Prim") << tr("Target Layer") << tr("Introduced By") << tr("Notes"),
                                                                        readComposition)));
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
ScenePrimInspector::addTab(std::unique_ptr<ScenePrimInspectorTab> tab)
{
    ScenePrimInspectorTab* added = tab.get();

    // Before the tab widget knows it: adding the first tab changes the current tab at once.
    _imp->tabs.push_back(std::move(tab));
    _imp->tabWidget->addTab(added->getWidget(), added->getTitle());
}

void
ScenePrimInspector::setPrim(const SceneGraphStageSourcePtr& source,
                            const QString& path,
                            double frame,
                            double framesPerSecond)
{
    _imp->source = source;
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
