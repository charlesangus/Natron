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

#include "ScenePrimListPopup.h"

#include <array>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneViewport.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// Names read under one hold of the stage.
const int kBatchSize = 256;

// How long a read that an edit kept out waits before it asks again.
const int kRetryReadMs = 50;

// How often a requested load is looked for while it has not landed.
const int kLoadPollMs = 100;

// The matching rows the popup keeps listing ahead of the view before it stops reading.
const int kWantedRows = 60;

// The rows the test hook lists.
const int kPublishedRows = 200;

const char* const kRootPath = "/";

typedef std::array<U64, 3> EditKey;

EditKey
readEditKey(SceneViewport* viewport)
{
    EditKey key = { { 0, 0, 0 } };
    if (viewport) {
        const SceneViewportRenderer& renderer = viewport->getRenderer();
        key[0] = renderer.getStageEditCount();
        key[1] = renderer.getStageLoadEditCount();
        key[2] = renderer.getStageSyncSerial();
    }

    return key;
}

}

struct ScenePrimListModel::Private {
    struct Row {
        QString name;
        QString type;
    };

    QPointer<SceneViewport> viewport;
    QString parentPath;
    QList<Row> rows;
    // The last child read: the next batch continues after it.
    std::string lastName;
    bool complete = false;
    bool notLoaded = false;
    bool waitingForStage = false;
    EditKey seen = { { 0, 0, 0 } };
    QTimer retryTimer;
};

ScenePrimListModel::ScenePrimListModel(SceneViewport* viewport, const QString& parentPath, QObject* parent)
    : QAbstractListModel(parent)
    , _imp(new Private)
{
    _imp->viewport = viewport;
    _imp->parentPath = parentPath.isEmpty() ? QString::fromLatin1(kRootPath) : parentPath;
    _imp->retryTimer.setSingleShot(true);
    _imp->retryTimer.setInterval(kRetryReadMs);
    QObject::connect(&_imp->retryTimer, &QTimer::timeout, this, &ScenePrimListModel::fetchBatch);
    if (viewport) {
        QObject::connect(viewport, &SceneViewport::sceneSynced, this, &ScenePrimListModel::checkEdits);
        QObject::connect(viewport, &SceneViewport::stageLoadsChanged, this, &ScenePrimListModel::checkEdits);
    }
    _imp->seen = readEditKey(viewport);
    fetchBatch();
}

ScenePrimListModel::~ScenePrimListModel()
{
    delete _imp;
}

int
ScenePrimListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(_imp->rows.size());
}

QVariant
ScenePrimListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || (index.row() < 0) || (index.row() >= _imp->rows.size())) {
        return QVariant();
    }
    const Private::Row& row = _imp->rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        return row.name;
    case Qt::DecorationRole: {
        const QIcon icon = iconForType(row.type);
        return icon.isNull() ? QVariant() : QVariant(icon);
    }
    case Qt::ToolTipRole:
        return row.type;
    case ePathRole:
        return getChildPath(row.name);
    case eTypeRole:
        return row.type;
    default:
        break;
    }

    return QVariant();
}

bool
ScenePrimListModel::canFetchMore(const QModelIndex& parent) const
{
    return !parent.isValid() && !_imp->complete && !_imp->waitingForStage && !_imp->retryTimer.isActive();
}

void
ScenePrimListModel::fetchMore(const QModelIndex& parent)
{
    if (!parent.isValid()) {
        fetchBatch();
    }
}

bool
ScenePrimListModel::isComplete() const
{
    return _imp->complete;
}

bool
ScenePrimListModel::isNotLoaded() const
{
    return _imp->notLoaded;
}

bool
ScenePrimListModel::isWaitingForStage() const
{
    return _imp->waitingForStage;
}

QString
ScenePrimListModel::getParentPath() const
{
    return _imp->parentPath;
}

QString
ScenePrimListModel::getChildPath(const QString& name) const
{
    return (_imp->parentPath == QLatin1String(kRootPath) ? QString() : _imp->parentPath) + QLatin1Char('/') + name;
}

void
ScenePrimListModel::refresh()
{
    _imp->retryTimer.stop();
    beginResetModel();
    _imp->rows.clear();
    _imp->lastName.clear();
    _imp->complete = false;
    _imp->waitingForStage = false;
    _imp->seen = readEditKey(_imp->viewport);
    endResetModel();
    fetchBatch();
}

void
ScenePrimListModel::checkEdits()
{
    if (_imp->waitingForStage || (readEditKey(_imp->viewport) != _imp->seen)) {
        refresh();
    }
}

void
ScenePrimListModel::fetchBatch()
{
    _imp->retryTimer.stop();
    if (_imp->complete || !_imp->viewport) {
        return;
    }
    SceneViewportRenderer& renderer = _imp->viewport->getRenderer();
    if (!renderer.hasScene() || !_imp->viewport->isStageSynced()) {
        _imp->waitingForStage = true;
        Q_EMIT stateChanged();
        return;
    }

    struct Batch {
        QList<Private::Row> rows;
        std::string lastName;
        bool read = false;
        bool done = false;
        bool lost = false;
        bool notLoaded = false;
    } batch;
    const std::string parentPath = _imp->parentPath.toStdString();
    const std::string lastName = _imp->lastName;
    const bool ran = renderer.tryReadStage([&batch, &parentPath, &lastName](const SceneStage* stage) {
        const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();
        if (!usdStage) {
            return;
        }
        batch.read = true;
        const UsdPrim parent = (parentPath == kRootPath) ? usdStage->GetPseudoRoot() : (SdfPath::IsValidPathString(parentPath) ? usdStage->GetPrimAtPath(SdfPath(parentPath)) : UsdPrim());
        if (!parent) {
            batch.lost = true;
            batch.done = true;
            return;
        }
        batch.notLoaded = parent.HasAuthoredPayloads() && !parent.IsLoaded();
        const Usd_PrimFlagsPredicate predicate = childPredicate();
        UsdPrim prim;
        if (lastName.empty()) {
            const UsdPrimSiblingRange children = parent.GetFilteredChildren(predicate);
            if (!children.empty()) {
                prim = *children.begin();
            }
        } else {
            const UsdPrim last = parent.GetChild(TfToken(lastName));
            if (!last) {
                batch.lost = true;
                return;
            }
            prim = last.GetFilteredNextSibling(predicate);
        }
        for (int n = 0; prim && (n < kBatchSize); ++n, prim = prim.GetFilteredNextSibling(predicate)) {
            Private::Row row;
            batch.lastName = prim.GetName().GetString();
            row.name = QString::fromStdString(batch.lastName);
            row.type = QString::fromStdString(prim.GetTypeName().GetString());
            batch.rows.push_back(row);
        }
        batch.done = !prim;
    });

    if (!ran) {
        // The edit's end is not announced to this model, so it asks again by itself.
        _imp->retryTimer.start();
        return;
    }
    if (!batch.read) {
        _imp->waitingForStage = true;
        Q_EMIT stateChanged();
        return;
    }
    if (batch.lost && !batch.done) {
        // The child the walk stood on is gone: what was read may no longer be what is there.
        refresh();
        return;
    }
    _imp->waitingForStage = false;
    _imp->notLoaded = batch.notLoaded;
    if (!batch.rows.isEmpty()) {
        beginInsertRows(QModelIndex(), int(_imp->rows.size()), int(_imp->rows.size() + batch.rows.size() - 1));
        _imp->rows.append(batch.rows);
        _imp->lastName = batch.lastName;
        endInsertRows();
    }
    _imp->complete = batch.done;
    Q_EMIT stateChanged();
}

struct ScenePrimListPopup::Private {
    QPointer<SceneViewport> viewport;
    ScenePrimListModel* model = nullptr;
    QSortFilterProxyModel* proxy = nullptr;
    QLineEdit* filter = nullptr;
    QListView* view = nullptr;
    QWidget* notLoadedRow = nullptr;
    QPushButton* loadButton = nullptr;
    QLabel* statusLabel = nullptr;
    QTimer pumpTimer;
    QTimer loadTimer;
    QString highlightName;
    bool highlightPending = false;
    bool loading = false;
};

ScenePrimListPopup::ScenePrimListPopup(SceneViewport* viewport, const QString& parentPath, const QString& highlightName, QWidget* parent)
    : QFrame(parent, Qt::Popup)
    , _imp(new Private)
{
    _imp->viewport = viewport;
    _imp->highlightName = highlightName;
    _imp->highlightPending = !highlightName.isEmpty();
    setObjectName(QStringLiteral("scenePrimListPopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);

    _imp->model = new ScenePrimListModel(viewport, parentPath, this);
    _imp->proxy = new QSortFilterProxyModel(this);
    _imp->proxy->setSourceModel(_imp->model);
    _imp->proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    _imp->filter = new QLineEdit(this);
    _imp->filter->setObjectName(QStringLiteral("scenePrimListFilter"));
    _imp->filter->setPlaceholderText(tr("Filter"));
    _imp->filter->setClearButtonEnabled(true);
    _imp->filter->installEventFilter(this);
    layout->addWidget(_imp->filter);

    _imp->notLoadedRow = new QWidget(this);
    _imp->notLoadedRow->setObjectName(QStringLiteral("scenePrimListNotLoaded"));
    QHBoxLayout* notLoadedLayout = new QHBoxLayout(_imp->notLoadedRow);
    notLoadedLayout->setContentsMargins(2, 0, 2, 0);
    notLoadedLayout->addWidget(new QLabel(tr("Not loaded"), _imp->notLoadedRow), 1);
    _imp->loadButton = new QPushButton(tr("Load"), _imp->notLoadedRow);
    _imp->loadButton->setObjectName(QStringLiteral("scenePrimListLoad"));
    _imp->loadButton->setFocusPolicy(Qt::NoFocus);
    notLoadedLayout->addWidget(_imp->loadButton);
    layout->addWidget(_imp->notLoadedRow);

    _imp->statusLabel = new QLabel(this);
    layout->addWidget(_imp->statusLabel);

    _imp->view = new QListView(this);
    _imp->view->setObjectName(QStringLiteral("scenePrimListView"));
    _imp->view->setModel(_imp->proxy);
    _imp->view->setUniformItemSizes(true);
    _imp->view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _imp->view->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(_imp->view, 1);
    resize(300, 360);

    _imp->pumpTimer.setSingleShot(true);
    _imp->pumpTimer.setInterval(0);
    _imp->loadTimer.setInterval(kLoadPollMs);

    QObject::connect(&_imp->pumpTimer, &QTimer::timeout, this, &ScenePrimListPopup::pump);
    QObject::connect(&_imp->loadTimer, &QTimer::timeout, _imp->model, &ScenePrimListModel::checkEdits);
    QObject::connect(_imp->filter, &QLineEdit::textChanged, this, &ScenePrimListPopup::setFilterText);
    QObject::connect(_imp->loadButton, &QPushButton::clicked, this, &ScenePrimListPopup::load);
    QObject::connect(_imp->view, &QListView::clicked, this, [this](const QModelIndex& index) { pickIndex(index); });
    // Any restart of the list is the load landing or the stage changing under it: either way the
    // request is over, and a prim that still has an unloaded payload offers Load again.
    QObject::connect(_imp->model, &QAbstractItemModel::modelReset, this, [this]() {
        _imp->loading = false;
        _imp->loadTimer.stop();
    });
    QObject::connect(_imp->model, &ScenePrimListModel::stateChanged, this, &ScenePrimListPopup::onModelChanged);
    QObject::connect(_imp->model, &QAbstractItemModel::modelReset, this, &ScenePrimListPopup::onModelChanged);
    QObject::connect(_imp->proxy, &QAbstractItemModel::rowsInserted, this, &ScenePrimListPopup::onModelChanged);
    QObject::connect(_imp->proxy, &QAbstractItemModel::rowsRemoved, this, &ScenePrimListPopup::onModelChanged);
    QObject::connect(_imp->proxy, &QAbstractItemModel::layoutChanged, this, &ScenePrimListPopup::onModelChanged);
    QObject::connect(_imp->view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { publish(); });

    onModelChanged();
}

ScenePrimListPopup::~ScenePrimListPopup()
{
    delete _imp;
}

void
ScenePrimListPopup::popupAt(const QPoint& globalPos)
{
    move(globalPos);
    show();
    _imp->filter->setFocus();
}

void
ScenePrimListPopup::setFilterText(const QString& text)
{
    if (_imp->filter->text() != text) {
        _imp->filter->setText(text);
        return;
    }
    _imp->proxy->setFilterFixedString(text);
    pump();
    publish();
}

void
ScenePrimListPopup::pick(const QString& name)
{
    const QModelIndexList found = _imp->proxy->match(_imp->proxy->index(0, 0), Qt::DisplayRole, name, 1, Qt::MatchExactly);
    if (!found.isEmpty()) {
        pickIndex(found.front());
    }
}

void
ScenePrimListPopup::pickIndex(const QModelIndex& proxyIndex)
{
    if (!proxyIndex.isValid() || !_imp->viewport) {
        return;
    }
    const QString path = proxyIndex.data(ScenePrimListModel::ePathRole).toString();
    // The popup is gone before the selection changes the bar that opened it.
    QPointer<SceneViewport> viewport = _imp->viewport;
    close();
    if (viewport) {
        viewport->select(QStringList(path), SceneViewport::eSelectionModeReplace);
    }
}

void
ScenePrimListPopup::load()
{
    if (!_imp->viewport || _imp->loading) {
        return;
    }
    _imp->loading = true;
    _imp->loadButton->setEnabled(false);
    _imp->loadButton->setText(tr("Loading..."));
    _imp->viewport->requestLoad(QStringList(_imp->model->getParentPath()), false);
    _imp->loadTimer.start();
    publish();
}

void
ScenePrimListPopup::onModelChanged()
{
    const bool notLoaded = _imp->model->isNotLoaded();
    _imp->notLoadedRow->setVisible(notLoaded);
    _imp->loadButton->setEnabled(!_imp->loading);
    _imp->loadButton->setText(_imp->loading ? tr("Loading...") : tr("Load"));
    if (_imp->model->isWaitingForStage()) {
        _imp->statusLabel->setText(tr("Waiting for the scene"));
    } else if (_imp->model->isComplete() && !notLoaded && (_imp->model->rowCount() == 0)) {
        _imp->statusLabel->setText(tr("No children"));
    } else {
        _imp->statusLabel->clear();
    }
    _imp->statusLabel->setVisible(!_imp->statusLabel->text().isEmpty());

    if (_imp->highlightPending) {
        const QModelIndexList found = _imp->proxy->match(_imp->proxy->index(0, 0), Qt::DisplayRole, _imp->highlightName, 1, Qt::MatchExactly);
        if (!found.isEmpty()) {
            _imp->highlightPending = false;
            _imp->view->setCurrentIndex(found.front());
            _imp->view->scrollTo(found.front(), QAbstractItemView::PositionAtCenter);
        }
    }
    if (!_imp->pumpTimer.isActive()) {
        _imp->pumpTimer.start();
    }
    publish();
}

void
ScenePrimListPopup::pump()
{
    // A filter can reject every row read so far, which leaves the view nothing to scroll to and so
    // no reason to ask for more; the reads go on here, a batch per turn of the event loop.
    const bool enough = (_imp->proxy->rowCount() >= kWantedRows) && !_imp->highlightPending;
    if (enough || !_imp->model->canFetchMore(QModelIndex())) {
        return;
    }
    const bool filtered = !_imp->filter->text().isEmpty();
    if (!filtered && !_imp->highlightPending) {
        return;
    }
    _imp->model->fetchMore(QModelIndex());
}

void
ScenePrimListPopup::publish()
{
    QStringList rows;
    const int count = _imp->proxy->rowCount();
    for (int i = 0; (i < count) && (i < kPublishedRows); ++i) {
        rows.push_back(_imp->proxy->index(i, 0).data().toString());
    }
    setProperty("natronPopupParent", _imp->model->getParentPath());
    setProperty("natronPopupRows", rows);
    setProperty("natronPopupRowCount", count);
    setProperty("natronPopupLoadedCount", _imp->model->rowCount());
    setProperty("natronPopupComplete", _imp->model->isComplete());
    setProperty("natronPopupNotLoaded", _imp->model->isNotLoaded());
    setProperty("natronPopupWaiting", _imp->model->isWaitingForStage());
    setProperty("natronPopupCurrent", _imp->view->currentIndex().data().toString());
    setProperty("natronPopupFilter", _imp->filter->text());
}

bool
ScenePrimListPopup::eventFilter(QObject* watched, QEvent* e)
{
    if ((watched == _imp->filter) && (e->type() == QEvent::KeyPress)) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if ((key == Qt::Key_Down) || (key == Qt::Key_Up) || (key == Qt::Key_PageDown) || (key == Qt::Key_PageUp)) {
            if (!_imp->view->currentIndex().isValid() && (_imp->proxy->rowCount() > 0)) {
                _imp->view->setCurrentIndex(_imp->proxy->index(0, 0));
            } else {
                QCoreApplication::sendEvent(_imp->view, e);
            }

            return true;
        }
        if ((key == Qt::Key_Return) || (key == Qt::Key_Enter)) {
            QModelIndex index = _imp->view->currentIndex();
            if (!index.isValid() && (_imp->proxy->rowCount() > 0)) {
                index = _imp->proxy->index(0, 0);
            }
            pickIndex(index);

            return true;
        }
    }

    return QFrame::eventFilter(watched, e);
}

NATRON_NAMESPACE_EXIT
