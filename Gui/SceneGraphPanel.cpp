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

#include "SceneGraphPanel.h"

// Before Qt's OpenGL headers.
#include "Gui/SceneViewport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Project.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneNodeStageSource.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/ViewerTab.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// How many prims a filter lists before it offers more.
const int kFilterCap = 1000;

// How long one step of the filter walk may hold the stage, in milliseconds.
const int kFilterStepMs = 8;

// How many paths of a viewer selection are shown in the tree.
const int kShownSelectionCap = 2000;

// How often a pinned node is checked for edits and deletion, in milliseconds.
const int kPinnedPollMs = 250;

const int kMaxValueLength = 200;

// The 3D viewer's stage.
class ViewerStageSource
    : public SceneGraphStageSource {
public:
    explicit ViewerStageSource(SceneViewport* viewport)
        : _viewport(viewport)
    {
    }

    void readStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        if (_viewport) {
            _viewport->getRenderer().readStage(read);
        } else {
            read(nullptr);
        }
    }

    U64 getEditCount() const override
    {
        return _viewport ? _viewport->getRenderer().getStageEditCount() : 0;
    }

private:
    QPointer<SceneViewport> _viewport;
};

// The filter's matches, which drag their paths as the tree's rows do.
class FilterResultsModel
    : public QStringListModel {
public:
    explicit FilterResultsModel(QObject* parent)
        : QStringListModel(parent)
    {
    }

    // The trailing row that asks for more matches, when there is one.
    bool hasMoreRow = false;

    bool isMoreRow(const QModelIndex& index) const
    {
        return hasMoreRow && index.isValid() && (index.row() == rowCount() - 1);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        if (!index.isValid()) {
            return Qt::NoItemFlags;
        }
        if (isMoreRow(index)) {
            return Qt::ItemIsEnabled;
        }

        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    }

    QStringList mimeTypes() const override
    {
        return QStringList() << SceneGraphModel::primPathsMimeType() << QString::fromUtf8("text/plain");
    }

    QMimeData* mimeData(const QModelIndexList& indexes) const override
    {
        QStringList paths;

        for (const QModelIndex& index : indexes) {
            if (index.isValid() && !isMoreRow(index) && !paths.contains(index.data().toString())) {
                paths << index.data().toString();
            }
        }
        if (paths.isEmpty()) {
            return nullptr;
        }
        QMimeData* mime = new QMimeData;
        const QString joined = paths.join(QLatin1Char('\n'));
        mime->setData(SceneGraphModel::primPathsMimeType(), joined.toUtf8());
        mime->setText(joined);

        return mime;
    }

    Qt::DropActions supportedDragActions() const override
    {
        return Qt::CopyAction;
    }
};

// Lists the nodes putting out a scene each time it opens.
class SceneNodeChooser
    : public QComboBox {
public:
    SceneNodeChooser(Gui* gui,
                     QWidget* parent)
        : QComboBox(parent)
        , _gui(gui)
    {
    }

    void populate(const NodePtr& current)
    {
        clear();
        addItem(QObject::tr("(Active Viewer)"), QString());
        GuiAppInstancePtr app = _gui ? _gui->getApp() : GuiAppInstancePtr();
        if (!app || !app->getProject()) {
            return;
        }
        NodesList nodes;
        app->getProject()->getNodes_recursive(nodes, true);
        QStringList names;
        for (const NodePtr& node : nodes) {
            EffectInstancePtr effect = node->getEffectInstance();
            if (effect && node->isActivated() && effect->producesSceneData()) {
                names << QString::fromStdString(node->getFullyQualifiedName());
            }
        }
        names.sort();
        for (const QString& name : names) {
            addItem(name, name);
        }
        const int index = current ? findData(QString::fromStdString(current->getFullyQualifiedName())) : 0;
        setCurrentIndex(std::max(0, index));
    }

    std::function<NodePtr()> currentNode;

    void showPopup() override
    {
        const bool blocked = blockSignals(true);
        populate(currentNode ? currentNode() : NodePtr());
        blockSignals(blocked);
        QComboBox::showPopup();
    }

private:
    Gui* _gui;
};

QString
valueString(const VtValue& value)
{
    if (value.IsEmpty()) {
        return QString();
    }
    std::ostringstream os;
    os << value;
    std::string text = os.str();
    if ((int)text.size() > kMaxValueLength) {
        text = text.substr(0, kMaxValueLength) + "...";
    }

    return QString::fromStdString(text);
}

} // anon namespace

// Shows the rows of folded prims in italics, marked in the name column.
class FoldedRowDelegate
    : public QStyledItemDelegate {
public:
    FoldedRowDelegate(SceneGraphModel* model,
                      QObject* parent)
        : QStyledItemDelegate(parent)
        , model(model)
    {
    }

    QSet<QString> folded;

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (folded.isEmpty() || !folded.contains(model->pathOf(index))) {
            return;
        }
        option->font.setItalic(true);
        if (index.column() == SceneGraphModel::eColumnName) {
            option->text = SceneGraphPanel::tr("%1 (folded)").arg(option->text);
        }
    }

private:
    SceneGraphModel* model;
};

struct SceneGraphPanel::Implementation {
    SceneGraphPanel* panel;
    Gui* gui;

    QToolButton* pinButton = nullptr;
    QToolButton* foldButton = nullptr;
    QToolButton* unfoldButton = nullptr;
    FoldedRowDelegate* foldDelegate = nullptr;
    SceneNodeChooser* nodeChooser = nullptr;
    QLabel* status = nullptr;
    QLineEdit* filter = nullptr;
    QStackedWidget* stack = nullptr;
    QTreeView* tree = nullptr;
    QListView* results = nullptr;
    QLabel* noScene = nullptr;
    QTreeView* attributes = nullptr;
    QStandardItemModel* attributeModel = nullptr;
    FilterResultsModel* resultsModel = nullptr;
    SceneGraphModel* model = nullptr;

    QPointer<ViewerTab> viewer;
    QPointer<SceneViewport> viewport;
    QPointer<TimeLine> viewerTimeLine;

    std::shared_ptr<SceneNodeStageSource> pinned;
    U64 pinnedMarkCount = 0;
    QTimer pinnedPoll;

    // Set while the panel and the viewer hand a selection to each other.
    bool syncingSelection = false;

    // The rows the user expanded, kept across resets.
    QSet<QString> expanded;
    bool restoringExpansion = false;

    // The filter's walk over the stage, a step at a time on the main thread.
    QTimer filterDebounce;
    QTimer filterStep;
    QString filterPattern;
    QRegularExpression filterExpression;
    bool filterWildcard = false;
    std::vector<SdfPath> filterStack;
    QStringList filterMatches;
    int filterCap = kFilterCap;
    bool filterCapped = false;

    QTimer attributeRefresh;

    Implementation(SceneGraphPanel* panel,
                   Gui* gui)
        : panel(panel)
        , gui(gui)
    {
    }

    bool isPinned() const
    {
        return bool(pinned);
    }

    // Whether the rows are the viewer's prims, so selections go both ways.
    bool sharesViewerSelection() const
    {
        if (!viewport) {
            return false;
        }
        if (!pinned) {
            return true;
        }
        NodePtr node = pinned->getNode();

        return node && viewer && (node == viewer->getSceneViewedNode());
    }

    ViewerTab* followedViewer() const
    {
        return SceneSelectionActions::selectionViewer(gui);
    }

    // Which of the 3D viewer's selection actions key is bound to: 1 sets the selection on the
    // current node, 2 creates a node from it, 0 neither.
    static int selectionActionForKey(Qt::KeyboardModifiers modifiers,
                                     int key)
    {
        GuiApplicationManager* manager = dynamic_cast<GuiApplicationManager*>(AppManager::instance());

        if (!manager) {
            return 0;
        }
        if (manager->matchesKeybind(kShortcutGroupViewer3D, kShortcutIDActionViewer3DSetSelectionOnNode, modifiers, key)) {
            return 1;
        }
        if (manager->matchesKeybind(kShortcutGroupViewer3D, kShortcutIDActionViewer3DCreateFromSelection, modifiers, key)) {
            return 2;
        }

        return 0;
    }

    static QKeySequence keySequenceOf(const char* actionID)
    {
        GuiApplicationManager* manager = dynamic_cast<GuiApplicationManager*>(AppManager::instance());
        const std::list<QKeySequence> sequences = manager ? manager->getKeySequenceForAction(QString::fromUtf8(kShortcutGroupViewer3D), QString::fromUtf8(actionID)) : std::list<QKeySequence>();

        return sequences.empty() ? QKeySequence() : sequences.front();
    }

    void runSelectionAction(int action,
                            const QStringList& paths)
    {
        if (action == 1) {
            SceneSelectionActions::setOnCurrentNode(gui, paths);
        } else if (action == 2) {
            SceneSelectionActions::createFromSelection(gui, panel->getShownNode(), paths);
        }
    }

    double currentFrame() const
    {
        if (!pinned && viewer && viewer->getTimeLine()) {
            return viewer->getTimeLine()->currentFrame();
        }
        GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();

        return (app && app->getTimeLine()) ? app->getTimeLine()->currentFrame() : 0.;
    }

    double framesPerSecond() const
    {
        GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();

        return app ? app->getProjectFrameRate() : 24.;
    }

    bool hasScene() const
    {
        return model->hasChildren(QModelIndex());
    }

    void publishState()
    {
        NodePtr node = panel->getShownNode();

        panel->setProperty("natronSceneGraphPinned", isPinned());
        panel->setProperty("natronSceneGraphNode", node ? QString::fromStdString(node->getFullyQualifiedName()) : QString());
        panel->setProperty("natronSceneGraphHasScene", hasScene());
        panel->setProperty("natronSceneGraphFilterBusy", filterStep.isActive() || filterDebounce.isActive());
    }

    // Folds live on the viewer's stage: a pinned scene is not drawn, so it has none.
    SceneViewport* foldViewport() const
    {
        return pinned ? nullptr : viewport.data();
    }

    void updateFolds()
    {
        if (!foldDelegate) {
            return;
        }
        SceneViewport* target = foldViewport();
        const QStringList folded = target ? target->getFoldedPrims() : QStringList();
        foldDelegate->folded = QSet<QString>(folded.begin(), folded.end());
        tree->viewport()->update();
        const bool canFold = target && hasScene();
        foldButton->setEnabled(canFold);
        unfoldButton->setEnabled(canFold);
        panel->setProperty("natronSceneGraphFolded", folded);
    }

    void foldSelection(bool folded)
    {
        SceneViewport* target = foldViewport();
        const QStringList paths = panel->getSelection();
        if (target && !paths.isEmpty()) {
            target->setFolded(paths, folded);
        }
    }

    void updateStatus()
    {
        NodePtr node = panel->getShownNode();
        QString text;

        if (pinned) {
            text = SceneGraphPanel::tr("Pinned to %1").arg(node ? QString::fromStdString(node->getFullyQualifiedName()) : SceneGraphPanel::tr("nothing"));
        } else if (viewer) {
            text = node ? SceneGraphPanel::tr("%1 shows %2").arg(QString::fromStdString(viewer->getInternalNode() ? viewer->getInternalNode()->getScriptName_mt_safe() : std::string()), QString::fromStdString(node->getFullyQualifiedName()))
                        : SceneGraphPanel::tr("%1 shows no scene").arg(QString::fromStdString(viewer->getInternalNode() ? viewer->getInternalNode()->getScriptName_mt_safe() : std::string()));
        } else {
            text = SceneGraphPanel::tr("No viewer");
        }
        status->setText(text);
        updateFolds();
        if (!filterPattern.isEmpty()) {
            stack->setCurrentWidget(results);
        } else if (hasScene()) {
            stack->setCurrentWidget(tree);
        } else {
            stack->setCurrentWidget(noScene);
        }
        publishState();
    }

    void connectViewport(SceneViewport* newViewport)
    {
        if (viewport == newViewport) {
            if (!pinned) {
                model->refresh();
            }

            return;
        }
        if (viewport) {
            QObject::disconnect(viewport, nullptr, panel, nullptr);
        }
        viewport = newViewport;
        if (viewport) {
            QObject::connect(viewport, &SceneViewport::sceneSynced, panel, &SceneGraphPanel::onSceneSynced);
            QObject::connect(viewport, &SceneViewport::selectionChanged, panel, &SceneGraphPanel::onViewportSelectionChanged);
            QObject::connect(viewport, &SceneViewport::foldsChanged, panel, [this]() { updateFolds(); });
        }
        if (!pinned) {
            model->setSource(viewport ? std::make_shared<ViewerStageSource>(viewport) : SceneGraphStageSourcePtr());
        }
    }

    void followViewer()
    {
        ViewerTab* newViewer = followedViewer();

        if (newViewer != viewer) {
            if (viewer) {
                QObject::disconnect(viewer, nullptr, panel, nullptr);
            }
            viewer = newViewer;
            if (viewer) {
                QObject::connect(viewer, &ViewerTab::sceneViewChanged, panel, &SceneGraphPanel::onViewerSceneChanged);
            }
            if (viewerTimeLine) {
                QObject::disconnect(viewerTimeLine, nullptr, panel, nullptr);
            }
            viewerTimeLine = viewer ? viewer->getTimeLine().get() : nullptr;
            if (viewerTimeLine) {
                QObject::connect(viewerTimeLine, &TimeLine::frameChanged, panel, [this]() { attributeRefresh.start(); });
            }
        }
        connectViewport((viewer && viewer->isShowingScene()) ? viewer->getSceneViewport() : nullptr);
        if (!pinned && viewport) {
            panel->showSelection(viewport->getSelection());
        }
        updateStatus();
        attributeRefresh.start();
    }

    QStringList selectedPaths(QAbstractItemView* view) const
    {
        QStringList paths;
        const QModelIndexList rows = view->selectionModel()->selectedRows(0);
        const QModelIndex current = view->selectionModel()->currentIndex();
        QString currentPath;

        for (const QModelIndex& row : rows) {
            if (view == results && resultsModel->isMoreRow(row)) {
                continue;
            }
            const QString path = (view == tree) ? model->pathOf(row) : row.data().toString();
            if (current.isValid() && (current.row() == row.row()) && (current.parent() == row.parent())) {
                currentPath = path;
            } else {
                paths << path;
            }
        }
        if (!currentPath.isEmpty()) {
            paths << currentPath;
        }

        return paths;
    }

    void restoreExpansion()
    {
        QStringList paths = expanded.values();

        std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return a.size() < b.size(); });
        restoringExpansion = true;
        for (const QString& path : paths) {
            const QModelIndex index = model->indexOfPath(path, true);
            if (index.isValid()) {
                tree->expand(index);
            }
        }
        restoringExpansion = false;
    }

    void startFilter()
    {
        filterStep.stop();
        filterMatches.clear();
        filterStack.clear();
        filterCapped = false;
        if (filterPattern.isEmpty()) {
            resultsModel->hasMoreRow = false;
            resultsModel->setStringList(QStringList());
            updateStatus();

            return;
        }
        filterWildcard = filterPattern.contains(QLatin1Char('*')) || filterPattern.contains(QLatin1Char('?'));
        if (filterWildcard) {
            filterExpression = QRegularExpression(QRegularExpression::wildcardToRegularExpression(filterPattern, QRegularExpression::UnanchoredWildcardConversion),
                                                  QRegularExpression::CaseInsensitiveOption);
        }
        filterStack.push_back(SdfPath::AbsoluteRootPath());
        filterStep.start();
        updateStatus();
    }

    bool filterMatchesName(const QString& name) const
    {
        return filterWildcard ? filterExpression.match(name).hasMatch() : name.contains(filterPattern, Qt::CaseInsensitive);
    }

    // Walks the stage for kFilterStepMs; returns whether the walk is over.
    bool stepFilter()
    {
        SceneGraphStageSourcePtr source = model->getSource();
        bool over = true;

        if (!source) {
            filterStack.clear();

            return true;
        }
        const Usd_PrimFlagsPredicate predicate = UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract);
        source->readStage([&](const SceneStage* sceneStage) {
            if (!sceneStage) {
                filterStack.clear();

                return;
            }
            const UsdStageRefPtr& stage = sceneStage->getStage();
            QElapsedTimer timer;
            timer.start();
            int visited = 0;
            while (!filterStack.empty() && (filterMatches.size() < filterCap)) {
                if (((++visited % 256) == 0) && (timer.elapsed() >= kFilterStepMs)) {
                    break;
                }
                const SdfPath path = filterStack.back();
                filterStack.pop_back();
                const UsdPrim prim = stage->GetPrimAtPath(path);
                if (!prim) {
                    continue;
                }
                if (!prim.IsPseudoRoot() && filterMatchesName(QString::fromStdString(prim.GetName().GetString()))) {
                    filterMatches << QString::fromStdString(path.GetString());
                }
                const TfTokenVector names = prim.GetFilteredChildrenNames(predicate);
                for (TfTokenVector::const_reverse_iterator it = names.rbegin(); it != names.rend(); ++it) {
                    filterStack.push_back(path.AppendChild(*it));
                }
            }
            over = filterStack.empty() || (filterMatches.size() >= filterCap);
        });
        filterCapped = !filterStack.empty() && (filterMatches.size() >= filterCap);

        return over;
    }

    void showFilterResults()
    {
        QStringList rows = filterMatches;

        resultsModel->hasMoreRow = filterCapped;
        if (filterCapped) {
            rows << SceneGraphPanel::tr("More...");
        }
        const bool wasSyncing = syncingSelection;
        syncingSelection = true;
        resultsModel->setStringList(rows);
        syncingSelection = wasSyncing;
        if (viewport && sharesViewerSelection()) {
            selectResults(viewport->getSelection());
        }
    }

    void selectResults(const QStringList& paths)
    {
        const bool wasSyncing = syncingSelection;
        QItemSelection selection;
        QModelIndex current;

        syncingSelection = true;
        for (const QString& path : paths) {
            const int row = filterMatches.indexOf(path);
            if (row >= 0) {
                current = resultsModel->index(row, 0);
                selection.select(current, current);
            }
        }
        results->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        if (current.isValid()) {
            results->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        }
        syncingSelection = wasSyncing;
    }

    QString currentPath() const
    {
        if (stack->currentWidget() == results) {
            const QModelIndex current = results->selectionModel()->currentIndex();

            return (current.isValid() && !resultsModel->isMoreRow(current)) ? current.data().toString() : QString();
        }

        return model->pathOf(tree->selectionModel()->currentIndex());
    }
};

SceneGraphPanel::SceneGraphPanel(Gui* gui)
    : QWidget(gui)
    , PanelWidget(this, gui)
    , _imp(new Implementation(this, gui))
{
    setObjectName(QString::fromUtf8("sceneGraph"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    QWidget* header = new QWidget(this);
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(2, 2, 2, 0);
    headerLayout->setSpacing(4);
    _imp->pinButton = new QToolButton(header);
    _imp->pinButton->setObjectName(QString::fromUtf8("sceneGraphPin"));
    _imp->pinButton->setText(tr("Pin"));
    _imp->pinButton->setCheckable(true);
    _imp->pinButton->setToolTip(tr("Keeps showing the scene of the node shown now, whichever viewer is active and whatever it views."));
    headerLayout->addWidget(_imp->pinButton);
    _imp->foldButton = new QToolButton(header);
    _imp->foldButton->setObjectName(QString::fromUtf8("sceneGraphFold"));
    _imp->foldButton->setText(tr("Fold"));
    _imp->foldButton->setToolTip(tr("Draws the selected prims in the viewer as their bounding boxes and unloads the payloads below them, "
                                    "for this viewed node, until unfolded."));
    headerLayout->addWidget(_imp->foldButton);
    _imp->unfoldButton = new QToolButton(header);
    _imp->unfoldButton->setObjectName(QString::fromUtf8("sceneGraphUnfold"));
    _imp->unfoldButton->setText(tr("Unfold"));
    _imp->unfoldButton->setToolTip(tr("Draws the selected folded prims in full again, their payloads loaded."));
    headerLayout->addWidget(_imp->unfoldButton);
    _imp->nodeChooser = new SceneNodeChooser(gui, header);
    _imp->nodeChooser->setObjectName(QString::fromUtf8("sceneGraphNode"));
    _imp->nodeChooser->setToolTip(tr("The node whose scene is shown: the active viewer's, or a node pinned by name."));
    _imp->nodeChooser->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _imp->nodeChooser->setMinimumContentsLength(12);
    _imp->nodeChooser->currentNode = [this]() { return getPinnedNode(); };
    _imp->nodeChooser->populate(NodePtr());
    headerLayout->addWidget(_imp->nodeChooser, 1);
    layout->addWidget(header);

    _imp->status = new QLabel(this);
    _imp->status->setObjectName(QString::fromUtf8("sceneGraphStatus"));
    _imp->status->setContentsMargins(4, 0, 4, 0);
    layout->addWidget(_imp->status);

    _imp->filter = new QLineEdit(this);
    _imp->filter->setObjectName(QString::fromUtf8("sceneGraphFilter"));
    _imp->filter->setPlaceholderText(tr("Filter by name (* and ? match any text and any character)"));
    _imp->filter->setClearButtonEnabled(true);
    layout->addWidget(_imp->filter);

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    _imp->stack = new QStackedWidget(splitter);

    _imp->model = new SceneGraphModel(this);
    _imp->tree = new QTreeView(_imp->stack);
    _imp->tree->setObjectName(QString::fromUtf8("sceneGraphTree"));
    _imp->tree->setModel(_imp->model);
    _imp->tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _imp->tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->tree->setUniformRowHeights(true);
    _imp->tree->setDragEnabled(true);
    _imp->tree->setDragDropMode(QAbstractItemView::DragOnly);
    _imp->foldDelegate = new FoldedRowDelegate(_imp->model, _imp->tree);
    _imp->tree->setItemDelegate(_imp->foldDelegate);
    _imp->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _imp->tree->header()->setStretchLastSection(false);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnName, QHeaderView::Stretch);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnType, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnKind, QHeaderView::ResizeToContents);
    _imp->stack->addWidget(_imp->tree);

    _imp->resultsModel = new FilterResultsModel(this);
    _imp->results = new QListView(_imp->stack);
    _imp->results->setObjectName(QString::fromUtf8("sceneGraphResults"));
    _imp->results->setModel(_imp->resultsModel);
    _imp->results->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _imp->results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _imp->results->setUniformItemSizes(true);
    _imp->results->setDragEnabled(true);
    _imp->results->setDragDropMode(QAbstractItemView::DragOnly);
    _imp->stack->addWidget(_imp->results);

    _imp->noScene = new QLabel(tr("No scene"), _imp->stack);
    _imp->noScene->setAlignment(Qt::AlignCenter);
    _imp->noScene->setEnabled(false);
    _imp->stack->addWidget(_imp->noScene);
    _imp->stack->setCurrentWidget(_imp->noScene);

    _imp->attributeModel = new QStandardItemModel(0, 4, this);
    _imp->attributeModel->setHorizontalHeaderLabels(QStringList() << tr("Attribute") << tr("Value") << tr("Animated") << tr("Authored By"));
    _imp->attributes = new QTreeView(splitter);
    _imp->attributes->setObjectName(QString::fromUtf8("sceneGraphAttributes"));
    _imp->attributes->setModel(_imp->attributeModel);
    _imp->attributes->setRootIsDecorated(false);
    _imp->attributes->setUniformRowHeights(true);
    _imp->attributes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _imp->attributes->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->attributes->header()->setSectionResizeMode(QHeaderView::Interactive);
    splitter->addWidget(_imp->stack);
    splitter->addWidget(_imp->attributes);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    _imp->filterDebounce.setSingleShot(true);
    _imp->filterDebounce.setInterval(200);
    _imp->filterStep.setInterval(0);
    _imp->attributeRefresh.setSingleShot(true);
    _imp->attributeRefresh.setInterval(0);
    _imp->pinnedPoll.setInterval(kPinnedPollMs);

    QObject::connect(_imp->pinButton, &QToolButton::toggled, this, &SceneGraphPanel::onPinToggled);
    QObject::connect(_imp->foldButton, &QToolButton::clicked, this, [this]() { _imp->foldSelection(true); });
    QObject::connect(_imp->unfoldButton, &QToolButton::clicked, this, [this]() { _imp->foldSelection(false); });
    QObject::connect(_imp->tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QStringList selection = getSelection();
        QMenu menu(_imp->tree);
        QAction* fold = nullptr;
        QAction* unfold = nullptr;
        if (_imp->foldViewport() && !selection.isEmpty()) {
            fold = menu.addAction(tr("Fold"));
            unfold = menu.addAction(tr("Unfold"));
            menu.addSeparator();
        }
        QAction* setOnNode = menu.addAction(tr(kShortcutDescActionViewer3DSetSelectionOnNode));
        setOnNode->setObjectName(QString::fromUtf8("sceneSetSelectionOnNode"));
        setOnNode->setShortcut(Implementation::keySequenceOf(kShortcutIDActionViewer3DSetSelectionOnNode));
        const QString blocker = SceneSelectionActions::setOnNodeBlocker(_imp->gui, selection);
        setOnNode->setEnabled(blocker.isEmpty());
        setOnNode->setToolTip(blocker);
        setOnNode->setStatusTip(blocker);
        QAction* create = menu.addAction(tr(kShortcutDescActionViewer3DCreateFromSelection));
        create->setObjectName(QString::fromUtf8("sceneCreateFromSelection"));
        create->setShortcut(Implementation::keySequenceOf(kShortcutIDActionViewer3DCreateFromSelection));
        create->setEnabled(SceneSelectionActions::createBlocker(_imp->gui, getShownNode()).isEmpty());
        QAction* chosen = menu.exec(_imp->tree->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if ((chosen == fold) || (chosen == unfold)) {
            _imp->foldSelection(chosen == fold);
        } else if (chosen == setOnNode) {
            _imp->runSelectionAction(1, selection);
        } else if (chosen == create) {
            _imp->runSelectionAction(2, selection);
        }
    });
    _imp->tree->installEventFilter(this);
    _imp->results->installEventFilter(this);
    QObject::connect(_imp->nodeChooser, qOverload<int>(&QComboBox::activated), this, &SceneGraphPanel::onNodeChosen);
    QObject::connect(_imp->filter, &QLineEdit::textChanged, &_imp->filterDebounce, qOverload<>(&QTimer::start));
    QObject::connect(&_imp->filterDebounce, &QTimer::timeout, this, &SceneGraphPanel::onFilterEdited);
    QObject::connect(&_imp->filterStep, &QTimer::timeout, this, &SceneGraphPanel::onFilterStep);
    QObject::connect(&_imp->attributeRefresh, &QTimer::timeout, this, &SceneGraphPanel::refreshAttributes);
    QObject::connect(&_imp->pinnedPoll, &QTimer::timeout, this, &SceneGraphPanel::onPinnedPoll);
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onTreeSelectionChanged);
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->results->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onResultsSelectionChanged);
    QObject::connect(_imp->results->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->results, &QListView::activated, this, &SceneGraphPanel::onResultActivated);
    QObject::connect(_imp->results, &QListView::clicked, this, &SceneGraphPanel::onResultActivated);
    QObject::connect(_imp->model, &QAbstractItemModel::modelReset, this, &SceneGraphPanel::onModelReset);
    QObject::connect(_imp->tree, &QTreeView::expanded, this, &SceneGraphPanel::onExpanded);
    QObject::connect(_imp->tree, &QTreeView::collapsed, this, &SceneGraphPanel::onCollapsed);
    if (gui) {
        QObject::connect(gui, &Gui::activeViewerChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        QObject::connect(gui, &Gui::viewersChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        GuiAppInstancePtr app = gui->getApp();
        if (app && app->getTimeLine()) {
            QObject::connect(app->getTimeLine().get(), &TimeLine::frameChanged, this, [this]() { _imp->attributeRefresh.start(); });
        }
    }
    _imp->followViewer();
}

SceneGraphPanel::~SceneGraphPanel()
{
    if (_imp->pinned) {
        _imp->pinned->stop();
    }
}

SceneGraphModel*
SceneGraphPanel::getModel() const
{
    return _imp->model;
}

void
SceneGraphPanel::pinNode(const NodePtr& node)
{
    if (_imp->pinned) {
        _imp->pinned->stop();
        _imp->pinned.reset();
    }
    _imp->pinnedPoll.stop();
    if (node) {
        QPointer<SceneGraphPanel> self(this);
        _imp->pinned = std::make_shared<SceneNodeStageSource>(node, this, [self](bool ok, const QString& error) {
            if (self) {
                self->onPinnedSynced(ok, error);
            }
        });
        _imp->pinnedMarkCount = node->getHashMarkCount();
        _imp->pinned->request(_imp->currentFrame());
        _imp->model->setSource(_imp->pinned);
        _imp->pinnedPoll.start();
    } else {
        _imp->model->setSource(_imp->viewport ? std::make_shared<ViewerStageSource>(_imp->viewport) : SceneGraphStageSourcePtr());
    }
    {
        const bool blocked = _imp->pinButton->blockSignals(true);
        _imp->pinButton->setChecked(bool(node));
        _imp->pinButton->blockSignals(blocked);
    }
    {
        const bool blocked = _imp->nodeChooser->blockSignals(true);
        _imp->nodeChooser->populate(node);
        _imp->nodeChooser->blockSignals(blocked);
    }
    _imp->followViewer();
}

NodePtr
SceneGraphPanel::getPinnedNode() const
{
    return _imp->pinned ? _imp->pinned->getNode() : NodePtr();
}

NodePtr
SceneGraphPanel::getShownNode() const
{
    if (_imp->pinned) {
        return _imp->pinned->getNode();
    }

    return (_imp->viewer && _imp->viewport) ? _imp->viewer->getSceneViewedNode() : NodePtr();
}

void
SceneGraphPanel::showSelection(const QStringList& paths)
{
    const bool wasSyncing = _imp->syncingSelection;

    _imp->syncingSelection = true;
    QItemSelection selection;
    QModelIndex current;
    int shown = 0;
    // The current prim is last; the cap keeps it.
    for (int i = std::max(0, (int)paths.size() - kShownSelectionCap); i < paths.size(); ++i) {
        const QModelIndex index = _imp->model->indexOfPath(paths[i], true);
        if (!index.isValid()) {
            continue;
        }
        for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
            if (_imp->tree->isExpanded(parent)) {
                break;
            }
            _imp->tree->expand(parent);
        }
        selection.select(index, index.sibling(index.row(), SceneGraphModel::eColumnCount - 1));
        current = index;
        ++shown;
    }
    _imp->tree->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    if (current.isValid()) {
        _imp->tree->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        _imp->tree->scrollTo(current);
    }
    if (!_imp->filterPattern.isEmpty()) {
        _imp->selectResults(paths);
    }
    _imp->syncingSelection = wasSyncing;
    _imp->attributeRefresh.start();
}

QStringList
SceneGraphPanel::getSelection() const
{
    return _imp->selectedPaths((_imp->stack->currentWidget() == _imp->results) ? (QAbstractItemView*)_imp->results : (QAbstractItemView*)_imp->tree);
}

void
SceneGraphPanel::onActiveViewerChanged()
{
    _imp->followViewer();
}

void
SceneGraphPanel::onViewerSceneChanged()
{
    _imp->followViewer();
}

void
SceneGraphPanel::onSceneSynced()
{
    if (!_imp->pinned) {
        _imp->model->refresh();
        if (!_imp->filterPattern.isEmpty()) {
            _imp->filterDebounce.start();
        }
    }
    _imp->updateStatus();
    _imp->attributeRefresh.start();
}

void
SceneGraphPanel::onViewportSelectionChanged(const QStringList& paths)
{
    if (_imp->syncingSelection || !_imp->sharesViewerSelection()) {
        return;
    }
    showSelection(paths);
}

void
SceneGraphPanel::onTreeSelectionChanged()
{
    if (_imp->syncingSelection || !_imp->sharesViewerSelection()) {
        return;
    }
    _imp->syncingSelection = true;
    _imp->viewport->setSelection(_imp->selectedPaths(_imp->tree));
    _imp->syncingSelection = false;
}

void
SceneGraphPanel::onResultsSelectionChanged()
{
    if (_imp->syncingSelection || !_imp->sharesViewerSelection()) {
        return;
    }
    // Selected prims the filter hides stay selected.
    QStringList paths = _imp->viewport->getSelection();
    for (const QString& match : _imp->filterMatches) {
        paths.removeAll(match);
    }
    paths << _imp->selectedPaths(_imp->results);
    _imp->syncingSelection = true;
    _imp->viewport->setSelection(paths);
    _imp->syncingSelection = false;
}

void
SceneGraphPanel::onResultActivated(const QModelIndex& index)
{
    if (!_imp->resultsModel->isMoreRow(index)) {
        return;
    }
    _imp->filterCap += kFilterCap;
    _imp->resultsModel->hasMoreRow = false;
    _imp->filterStep.start();
    _imp->publishState();
}

void
SceneGraphPanel::onFilterEdited()
{
    _imp->filterPattern = _imp->filter->text().trimmed();
    _imp->filterCap = kFilterCap;
    _imp->startFilter();
}

void
SceneGraphPanel::onFilterStep()
{
    const bool over = _imp->stepFilter();

    if (over) {
        _imp->filterStep.stop();
    }
    if (over || (_imp->filterMatches.size() != _imp->resultsModel->rowCount())) {
        _imp->showFilterResults();
    }
    _imp->publishState();
}

void
SceneGraphPanel::onPinToggled(bool pinned)
{
    pinNode(pinned ? getShownNode() : NodePtr());
}

void
SceneGraphPanel::onNodeChosen(int index)
{
    const QString name = _imp->nodeChooser->itemData(index).toString();
    GuiAppInstancePtr app = _imp->gui ? _imp->gui->getApp() : GuiAppInstancePtr();
    NodePtr node = (app && !name.isEmpty()) ? app->getNodeByFullySpecifiedName(name.toStdString()) : NodePtr();

    pinNode(node);
}

void
SceneGraphPanel::onPinnedPoll()
{
    if (!_imp->pinned) {
        _imp->pinnedPoll.stop();

        return;
    }
    NodePtr node = _imp->pinned->getNode();
    if (!node || !node->isActivated()) {
        pinNode(NodePtr());

        return;
    }
    const U64 markCount = node->getHashMarkCount();
    if (markCount != _imp->pinnedMarkCount) {
        _imp->pinnedMarkCount = markCount;
        _imp->pinned->request(_imp->currentFrame());
    }
}

void
SceneGraphPanel::onPinnedSynced(bool /*ok*/,
                                const QString& /*error*/)
{
    if (!_imp->pinned) {
        return;
    }
    _imp->model->refresh();
    if (!_imp->filterPattern.isEmpty()) {
        _imp->filterDebounce.start();
    }
    _imp->updateStatus();
    _imp->attributeRefresh.start();
}

void
SceneGraphPanel::onModelReset()
{
    _imp->restoreExpansion();
    if (_imp->viewport && _imp->sharesViewerSelection()) {
        showSelection(_imp->viewport->getSelection());
    }
    _imp->updateStatus();
}

void
SceneGraphPanel::onExpanded(const QModelIndex& index)
{
    if (!_imp->restoringExpansion) {
        _imp->expanded.insert(_imp->model->pathOf(index));
    }
}

void
SceneGraphPanel::onCollapsed(const QModelIndex& index)
{
    _imp->expanded.remove(_imp->model->pathOf(index));
}

void
SceneGraphPanel::onCurrentChanged(const QModelIndex& /*current*/)
{
    _imp->attributeRefresh.start();
}

void
SceneGraphPanel::refreshAttributes()
{
    const QString path = _imp->currentPath();
    SceneGraphStageSourcePtr source = _imp->model->getSource();
    struct Row {
        QString name, value, animated, author, authorTip;
    };
    std::vector<Row> rows;

    if (!path.isEmpty() && source) {
        const double frame = _imp->currentFrame();
        const double fps = _imp->framesPerSecond();
        source->readStage([&](const SceneStage* sceneStage) {
            if (!sceneStage) {
                return;
            }
            const std::string pathString = path.toStdString();
            if (!SdfPath::IsValidPathString(pathString)) {
                return;
            }
            const UsdPrim prim = sceneStage->getStage()->GetPrimAtPath(SdfPath(pathString));
            if (!prim) {
                return;
            }
            const UsdTimeCode time(SceneOps::frameToTimeCode(sceneStage->getPayload(), frame, fps));
            for (const UsdAttribute& attribute : prim.GetAttributes()) {
                Row row;
                row.name = QString::fromStdString(attribute.GetName().GetString());
                VtValue value;
                if (attribute.Get(&value, time)) {
                    row.value = valueString(value);
                }
                const UsdResolveInfoSource resolved = attribute.GetResolveInfo(time).GetSource();
                if ((resolved == UsdResolveInfoSourceTimeSamples) || (resolved == UsdResolveInfoSourceValueClips) || (resolved == UsdResolveInfoSourceSpline)) {
                    row.animated = tr("Animated");
                }
                std::string layerIdentifier;
                NodePtr node = SceneOps::authoringNodeOf(attribute, time, &layerIdentifier);
                if (node) {
                    row.author = QString::fromStdString(node->getFullyQualifiedName());
                } else if (resolved == UsdResolveInfoSourceFallback) {
                    row.author = tr("Schema Fallback");
                } else if (!layerIdentifier.empty() && !SdfLayer::IsAnonymousLayerIdentifier(layerIdentifier)) {
                    row.author = QString::fromStdString(layerIdentifier);
                }
                row.authorTip = QString::fromStdString(layerIdentifier);
                rows.push_back(row);
            }
        });
    }
    _imp->attributeModel->removeRows(0, _imp->attributeModel->rowCount());
    for (const Row& row : rows) {
        QList<QStandardItem*> items;
        items << new QStandardItem(row.name) << new QStandardItem(row.value) << new QStandardItem(row.animated) << new QStandardItem(row.author);
        items[1]->setToolTip(row.value);
        items[3]->setToolTip(row.authorTip);
        _imp->attributeModel->appendRow(items);
    }
}

void
SceneGraphPanel::enterEvent(QEnterEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
SceneGraphPanel::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QWidget::leaveEvent(e);
}

bool
SceneGraphPanel::eventFilter(QObject* watched,
                             QEvent* e)
{
    // The 3D viewer's selection keys work on the panel's selection too. A key press is taken
    // before the view's own handling, which would spend Tab on moving the focus.
    if (((watched == _imp->tree) || (watched == _imp->results)) && ((e->type() == QEvent::ShortcutOverride) || (e->type() == QEvent::KeyPress))) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        const int action = Implementation::selectionActionForKey(k->modifiers(), k->key());
        if (action != 0) {
            e->accept();
            if (e->type() == QEvent::KeyPress) {
                _imp->runSelectionAction(action, getSelection());
            }

            return true;
        }
    }

    return QWidget::eventFilter(watched, e);
}

void
SceneGraphPanel::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Escape) && _imp->filter->hasFocus() && !_imp->filter->text().isEmpty()) {
        _imp->filter->clear();
        e->accept();

        return;
    }
    handleUnCaughtKeyPressEvent(e);
    QWidget::keyPressEvent(e);
}

void
SceneGraphPanel::keyReleaseEvent(QKeyEvent* e)
{
    handleUnCaughtKeyUpEvent(e);
    QWidget::keyReleaseEvent(e);
}

void
SceneGraphPanel::mousePressEvent(QMouseEvent* e)
{
    takeClickFocus();
    QWidget::mousePressEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneGraphPanel.cpp"
