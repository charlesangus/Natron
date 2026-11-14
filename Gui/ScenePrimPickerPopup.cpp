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

#include "ScenePrimPickerPopup.h"

#include <atomic>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Gui/Gui.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneNodeStageSource.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/SceneViewport.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {

struct MatchResult {
    QStringList matched;
    // Literal paths of the knob that no prim of the scene has, and none can be inside a payload.
    QStringList absent;
    // Literal paths of the knob under a payload the scene leaves unloaded.
    QStringList unloaded;
    QString error;
};

bool
isSearchable(const std::string& expression)
{
    std::string error;

    return !PrimSelector::isBlank(expression) && PrimSelector::validate(expression, &error);
}

MatchResult
evaluateMatches(const SceneGraphStageSourcePtr& source,
                const std::string& expression,
                const std::atomic<bool>& cancelled)
{
    MatchResult result;
    PrimSelector::Reach reach;
    std::string reachError;
    const bool literalOnly = PrimSelector::getReach(expression, &reach, &reachError) && reach.literalOnly;

    source->readStage([&](const SceneStage* stage) {
        if (!stage) {
            result.error = ScenePrimPickerPopup::tr("The scene could not be read.");

            return;
        }
        std::vector<PXR_NS::SdfPath> paths;
        std::string error;
        if (!SceneOps::selectPrims(stage->getStage(), expression, &paths, &error, [&cancelled]() { return cancelled.load(); })) {
            result.error = QString::fromStdString(error);

            return;
        }
        QStringList matched;
        for (const PXR_NS::SdfPath& path : paths) {
            matched << QString::fromStdString(path.GetString());
        }
        if (!literalOnly) {
            result.matched = matched;

            return;
        }
        const QSet<QString> found(matched.begin(), matched.end());
        for (const std::string& literal : reach.literals) {
            const QString path = QString::fromStdString(literal);
            if (found.contains(path)) {
                result.matched << path;
            } else if (SceneOps::countUnloadedPayloads(stage->getStage(), { literal }) > 0) {
                result.unloaded << path;
            } else {
                result.absent << path;
            }
        }
    });

    return result;
}

} // namespace

struct ScenePrimPickerPopup::Implementation {
    Gui* gui = nullptr;
    NodePtr node;
    bool multiple = false;
    std::string expression;
    // The tree's selected paths in the order they were selected.
    QStringList order;
    bool synced = false;
    // What the knob's text picks, with whatever Search Unloaded added to it: OK with only these
    // checked leaves the knob alone.
    QSet<QString> baseline;
    std::shared_ptr<SceneNodeStageSource> source;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<SceneOps::UnloadedSearch> search;
    QTimer* searchTimer = nullptr;
    SceneGraphModel* model = nullptr;
    QLabel* status = nullptr;
    QTreeView* tree = nullptr;
    QListWidget* extras = nullptr;
    QPushButton* useViewer = nullptr;
    QPushButton* searchUnloaded = nullptr;
    QDialogButtonBox* buttons = nullptr;
    QPointer<SceneViewport> viewport;

    SceneViewport* selectionViewport() const
    {
        ViewerTab* viewer = SceneSelectionActions::selectionViewer(gui);

        return (viewer && viewer->isShowingScene()) ? viewer->getSceneViewport() : nullptr;
    }

    QStringList selectedRows() const
    {
        QStringList paths;
        for (const QModelIndex& row : tree->selectionModel()->selectedRows(0)) {
            paths << model->pathOf(row);
        }

        return paths;
    }

    QStringList checkedExtras() const
    {
        QStringList paths;
        for (int i = 0; i < extras->count(); ++i) {
            const QListWidgetItem* item = extras->item(i);
            if (item->checkState() == Qt::Checked) {
                paths << item->data(Qt::UserRole).toString();
            }
        }

        return paths;
    }

    bool hasExtra(const QString& path) const
    {
        for (int i = 0; i < extras->count(); ++i) {
            if (extras->item(i)->data(Qt::UserRole).toString() == path) {
                return true;
            }
        }

        return false;
    }

    void addExtra(const QString& path,
                  const QString& why)
    {
        if (hasExtra(path)) {
            return;
        }
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(path, why), extras);
        item->setData(Qt::UserRole, path);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        extras->setVisible(true);
    }

    // Selects the paths' rows, opening their ancestors; only the first path for a single pick.
    // Returns the paths that have a row.
    QStringList selectPaths(const QStringList& paths,
                            bool replace)
    {
        QItemSelection selection;
        QModelIndex last;
        QStringList selected;
        if (replace) {
            order.clear();
        }
        for (const QString& path : paths) {
            const QModelIndex index = model->indexOfPath(path, true);
            if (!index.isValid()) {
                continue;
            }
            for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
                tree->expand(parent);
            }
            selection.select(index, index.sibling(index.row(), SceneGraphModel::eColumnCount - 1));
            if (!order.contains(path)) {
                order << path;
            }
            selected << path;
            last = index;
            if (!multiple) {
                break;
            }
        }
        tree->selectionModel()->select(selection, replace ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::Select);
        if (last.isValid()) {
            tree->selectionModel()->setCurrentIndex(last, QItemSelectionModel::NoUpdate);
            tree->scrollTo(last);
        }

        return selected;
    }
};

ScenePrimPickerPopup::ScenePrimPickerPopup(Gui* gui,
                                           const NodePtr& node,
                                           bool multiple,
                                           const QString& expression,
                                           double time,
                                           QWidget* parent)
    : QDialog(parent)
    , _imp(new Implementation)
{
    _imp->gui = gui;
    _imp->node = node;
    _imp->multiple = multiple;
    _imp->expression = expression.toStdString();
    const NodePtr sceneNode = node ? node->getInput(0) : NodePtr();

    setObjectName(QString::fromUtf8("scenePrimPicker"));
    setWindowTitle(multiple ? tr("Pick Prims") : tr("Pick Prim"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(480, 560);

    QVBoxLayout* layout = new QVBoxLayout(this);
    _imp->status = new QLabel(this);
    _imp->status->setObjectName(QString::fromUtf8("scenePrimPickerStatus"));
    _imp->status->setWordWrap(true);
    layout->addWidget(_imp->status);

    _imp->model = new SceneGraphModel(this);
    _imp->tree = new QTreeView(this);
    _imp->tree->setObjectName(QString::fromUtf8("scenePrimPickerTree"));
    _imp->tree->setModel(_imp->model);
    _imp->tree->setSelectionMode(multiple ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
    _imp->tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->tree->setUniformRowHeights(true);
    _imp->tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _imp->tree->header()->setStretchLastSection(false);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnName, QHeaderView::Stretch);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnType, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnKind, QHeaderView::ResizeToContents);
    layout->addWidget(_imp->tree, 1);

    _imp->extras = new QListWidget(this);
    _imp->extras->setObjectName(QString::fromUtf8("scenePrimPickerExtras"));
    _imp->extras->setToolTip(tr("Paths the tree does not list: prims inside payloads that are not loaded, and paths the scene does not have. Checked paths are kept."));
    _imp->extras->setMaximumHeight(120);
    _imp->extras->setVisible(false);
    layout->addWidget(_imp->extras);

    _imp->buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    _imp->buttons->setObjectName(QString::fromUtf8("scenePrimPickerButtons"));
    _imp->useViewer = _imp->buttons->addButton(tr("Use Viewer Selection"), QDialogButtonBox::ActionRole);
    _imp->useViewer->setObjectName(QString::fromUtf8("scenePrimPickerUseViewer"));
    _imp->useViewer->setToolTip(tr("Takes the prims selected in the 3D view instead of those picked here."));
    _imp->searchUnloaded = _imp->buttons->addButton(tr("Search Unloaded"), QDialogButtonBox::ActionRole);
    _imp->searchUnloaded->setObjectName(QString::fromUtf8("scenePrimPickerSearchUnloaded"));
    _imp->searchUnloaded->setToolTip(tr("Looks for the prims the knob's text matches inside payloads that are not loaded, without loading them in the viewer."));
    layout->addWidget(_imp->buttons);

    _imp->searchTimer = new QTimer(this);
    _imp->searchTimer->setInterval(50);
    QObject::connect(_imp->searchTimer, &QTimer::timeout, this, &ScenePrimPickerPopup::pollSearch);
    QObject::connect(_imp->searchUnloaded, &QPushButton::clicked, this, &ScenePrimPickerPopup::onSearchUnloaded);
    QObject::connect(_imp->extras, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!_imp->multiple && item->checkState() == Qt::Checked) {
            const QSignalBlocker blocker(_imp->extras);
            for (int i = 0; i < _imp->extras->count(); ++i) {
                if (_imp->extras->item(i) != item) {
                    _imp->extras->item(i)->setCheckState(Qt::Unchecked);
                }
            }
            _imp->tree->selectionModel()->clearSelection();
        }
        updateButtons();
    });
    QObject::connect(_imp->buttons, &QDialogButtonBox::accepted, this, &ScenePrimPickerPopup::onAccepted);
    QObject::connect(_imp->buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(_imp->useViewer, &QPushButton::clicked, this, &ScenePrimPickerPopup::onUseViewerSelection);
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        _imp->order = SceneSelectionActions::keepSelectionOrder(_imp->order, _imp->selectedRows());
        if (!_imp->multiple && !_imp->order.isEmpty()) {
            const QSignalBlocker blocker(_imp->extras);
            for (int i = 0; i < _imp->extras->count(); ++i) {
                _imp->extras->item(i)->setCheckState(Qt::Unchecked);
            }
        }
        updateButtons();
    });
    QObject::connect(_imp->tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!_imp->multiple && index.isValid()) {
            onAccepted();
        }
    });
    _imp->viewport = _imp->selectionViewport();
    if (_imp->viewport) {
        QObject::connect(_imp->viewport.data(), &SceneViewport::selectionChanged, this, &ScenePrimPickerPopup::updateButtons);
    }

    if (sceneNode) {
        _imp->status->setText(tr("Reading the scene of %1...").arg(QString::fromStdString(sceneNode->getLabel())));
        QPointer<ScenePrimPickerPopup> self(this);
        _imp->source = std::make_shared<SceneNodeStageSource>(sceneNode, this, [self](bool ok, const QString& error) {
            if (self) {
                self->onSynced(ok, error);
            } }, false);
        _imp->source->request(time);
        _imp->model->setSource(_imp->source);
    } else {
        _imp->status->setText(tr("The node's input is not connected, so there is no scene to pick from."));
    }
    updateButtons();
}

ScenePrimPickerPopup::~ScenePrimPickerPopup()
{
    _imp->cancelled->store(true);
    if (_imp->search) {
        _imp->search->cancel();
    }
    if (_imp->source) {
        _imp->source->stop();
    }
}

QStringList
ScenePrimPickerPopup::getTreeSelection() const
{
    return SceneSelectionActions::keepSelectionOrder(_imp->order, _imp->selectedRows());
}

QStringList
ScenePrimPickerPopup::getPickedPaths() const
{
    QStringList paths = getTreeSelection();

    for (const QString& path : _imp->checkedExtras()) {
        if (!paths.contains(path)) {
            paths << path;
        }
    }
    if (!_imp->multiple && paths.size() > 1) {
        paths = paths.mid(0, 1);
    }

    return paths;
}

void
ScenePrimPickerPopup::onSynced(bool ok,
                               const QString& error)
{
    _imp->model->refresh();
    if (!ok) {
        _imp->status->setText(error);
        updateButtons();

        return;
    }
    _imp->status->setText(_imp->multiple ? tr("Select the prims; Ctrl-click adds one, Shift-click a range.") : tr("Select one prim."));
    if (!_imp->synced) {
        _imp->synced = true;
        _imp->tree->expandToDepth(0);
        if (!isSearchable(_imp->expression)) {
            setProperty("natronPickerMatchesRead", true);
        } else {
            QPointer<ScenePrimPickerPopup> self(this);
            const SceneGraphStageSourcePtr source = _imp->source;
            const std::string expression = _imp->expression;
            const std::shared_ptr<std::atomic<bool>> cancelled = _imp->cancelled;
            QThreadPool::globalInstance()->start([self, source, expression, cancelled]() {
                const MatchResult result = evaluateMatches(source, expression, *cancelled);
                if (cancelled->load()) {
                    return;
                }
                QMetaObject::invokeMethod(qApp, [self, result]() {
                    if (self) {
                        self->onMatchesRead(result.matched, result.absent, result.unloaded, result.error);
                    } }, Qt::QueuedConnection);
            });
        }
    }
    setProperty("natronPickerSynced", true);
    updateButtons();
}

void
ScenePrimPickerPopup::onMatchesRead(const QStringList& matched,
                                    const QStringList& absent,
                                    const QStringList& unloaded,
                                    const QString& error)
{
    if (!error.isEmpty()) {
        _imp->status->setText(error);
        setProperty("natronPickerMatchesRead", true);

        return;
    }
    const QStringList selected = _imp->selectPaths(matched, true);
    if (_imp->multiple || selected.isEmpty()) {
        for (const QString& path : unloaded) {
            if (_imp->multiple || _imp->extras->count() == 0) {
                _imp->addExtra(path, tr("unloaded"));
            }
        }
        for (const QString& path : absent) {
            if (_imp->multiple || _imp->extras->count() == 0) {
                _imp->addExtra(path, tr("not in input"));
            }
        }
    }
    const QStringList picked = getPickedPaths();
    _imp->baseline = QSet<QString>(picked.begin(), picked.end());
    setProperty("natronPickerMatchesRead", true);
    updateButtons();
}

void
ScenePrimPickerPopup::onSearchUnloaded()
{
    if (!_imp->node || PrimSelector::isBlank(_imp->expression)) {
        return;
    }
    if (_imp->search) {
        _imp->search->cancel();
    }
    setProperty("natronPickerSearchDone", false);
    _imp->status->setText(tr("Searching the unloaded payloads..."));
    _imp->search = SceneOps::UnloadedSearch::start(_imp->node->getEffectInstance(), 0, _imp->expression);
    _imp->searchTimer->start();
    updateButtons();
}

void
ScenePrimPickerPopup::pollSearch()
{
    if (!_imp->search) {
        _imp->searchTimer->stop();

        return;
    }
    const SceneOps::UnloadedSearch::StatusEnum status = _imp->search->getStatus();
    if (status == SceneOps::UnloadedSearch::eStatusRunning) {
        return;
    }
    if (status != SceneOps::UnloadedSearch::eStatusDone) {
        _imp->searchTimer->stop();
        _imp->status->setText(status == SceneOps::UnloadedSearch::eStatusFailed ? tr("Search Unloaded: %1").arg(QString::fromStdString(_imp->search->getError())) : tr("Search Unloaded was cancelled."));
        _imp->search.reset();
        updateButtons();

        return;
    }
    QStringList inTree;
    QStringList inPayloads;
    const bool read = _imp->source->tryReadStage([&](const SceneStage* stage) {
        for (const std::string& match : _imp->search->getMatches()) {
            const QString path = QString::fromStdString(match);
            if (stage && stage->getStage()->GetPrimAtPath(PXR_NS::SdfPath(match))) {
                inTree << path;
            } else {
                inPayloads << path;
            }
        }
    });
    if (!read) {
        return;
    }
    _imp->searchTimer->stop();
    _imp->search.reset();
    if (_imp->multiple) {
        _imp->selectPaths(inTree, false);
    }
    for (const QString& path : inPayloads) {
        if (_imp->multiple || (_imp->extras->count() == 0 && getPickedPaths().isEmpty())) {
            _imp->addExtra(path, tr("unloaded"));
        }
    }
    const QStringList picked = getPickedPaths();
    for (const QString& path : inTree + inPayloads) {
        if (picked.contains(path)) {
            _imp->baseline.insert(path);
        }
    }
    _imp->status->setText(inPayloads.isEmpty() ? tr("Search Unloaded: no match inside an unloaded payload.") : tr("Search Unloaded: %n match(es) inside unloaded payloads, listed below.", nullptr, inPayloads.size()));
    setProperty("natronPickerSearchDone", true);
    updateButtons();
}

void
ScenePrimPickerPopup::onAccepted()
{
    const QStringList paths = getPickedPaths();

    if (paths.isEmpty()) {
        return;
    }
    if (QSet<QString>(paths.begin(), paths.end()) != _imp->baseline) {
        Q_EMIT primsPicked(paths);
    }
    accept();
}

void
ScenePrimPickerPopup::onUseViewerSelection()
{
    const QStringList paths = _imp->viewport ? _imp->viewport->getSelection() : QStringList();

    if (paths.isEmpty()) {
        return;
    }
    Q_EMIT primsPicked(paths);
    accept();
}

void
ScenePrimPickerPopup::updateButtons()
{
    _imp->buttons->button(QDialogButtonBox::Ok)->setEnabled(!getPickedPaths().isEmpty());
    _imp->searchUnloaded->setEnabled(_imp->synced && _imp->node && !_imp->search && isSearchable(_imp->expression));
    const bool viewerSelection = _imp->viewport && !_imp->viewport->getSelection().isEmpty();
    _imp->useViewer->setEnabled(viewerSelection);
    _imp->useViewer->setToolTip(viewerSelection ? tr("Takes the prims selected in the 3D view instead of those picked here.")
                                                : tr("No prims are selected in the 3D view."));
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ScenePrimPickerPopup.cpp"
