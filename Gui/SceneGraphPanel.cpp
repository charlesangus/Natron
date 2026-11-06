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
#include <list>
#include <shared_mutex>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
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
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneNodeStageSource.h"
#include "Gui/ScenePrimInspector.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/ViewerTab.h"

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

const int kSelectionPollMs = 250;

// The 3D viewer's stage. The inspector's worker may hold this while the viewer is destroyed on the
// main thread, which a QPointer cannot guard across threads: the viewport's destructor waits here
// for the read under way.
class ViewerStageSource
    : public SceneGraphStageSource {
public:
    explicit ViewerStageSource(SceneViewport* viewport)
        : _gate(std::make_shared<Gate>())
    {
        _gate->renderer = &viewport->getRenderer();
        std::shared_ptr<Gate> gate = _gate;
        QObject::connect(viewport, &SceneViewport::closing, viewport, [gate]() {
            std::unique_lock<std::shared_mutex> lock(gate->mutex);
            gate->renderer = nullptr;
        });
    }

    void readStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        std::shared_lock<std::shared_mutex> lock(_gate->mutex);

        if (_gate->renderer) {
            _gate->renderer->readStage(read);
        } else {
            read(nullptr);
        }
    }

    U64 getEditCount() const override
    {
        std::shared_lock<std::shared_mutex> lock(_gate->mutex);

        return _gate->renderer ? _gate->renderer->getStageEditCount() : 0;
    }

private:
    struct Gate {
        std::shared_mutex mutex;
        SceneViewportRenderer* renderer = nullptr;
    };

    std::shared_ptr<Gate> _gate;
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
        addItem(followLabel, QString());
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

    // The first item's text, which unpins.
    QString followLabel = QObject::tr("(Active Viewer)");

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

} // anon namespace

struct SceneGraphPanel::Implementation {
    SceneGraphPanel* panel;
    Gui* gui;

    QToolButton* pinButton = nullptr;
    QComboBox* followChooser = nullptr;
    SceneNodeChooser* nodeChooser = nullptr;
    QLabel* status = nullptr;
    QLineEdit* filter = nullptr;
    QStackedWidget* stack = nullptr;
    QTreeView* tree = nullptr;
    QListView* results = nullptr;
    QLabel* noScene = nullptr;
    ScenePrimInspector* inspector = nullptr;
    FilterResultsModel* resultsModel = nullptr;
    SceneGraphModel* model = nullptr;

    QPointer<ViewerTab> viewer;
    QPointer<SceneViewport> viewport;
    QPointer<TimeLine> viewerTimeLine;

    // The viewer's stage, for viewport.
    SceneGraphStageSourcePtr viewerSource;

    // The node shown when the panel does not follow the viewer: the pinned node, or the selected
    // node when following the selection. Its own stage is used while the viewer shows another.
    std::shared_ptr<SceneNodeStageSource> pinned;
    // Why the last evaluation of pinned failed, empty when it did not.
    QString pinnedError;
    bool userPinned = false;
    U64 pinnedMarkCount = 0;
    QTimer pinnedPoll;
    // Sources let go of whose workers have not quit yet, kept until they have.
    std::vector<std::shared_ptr<SceneNodeStageSource>> retired;
    QTimer retiredReap;

    SceneGraphPanel::FollowEnum follow = SceneGraphPanel::eFollowViewer;
    NodeWPtr selectedNode;
    QTimer selectionPoll;

    // Set while the panel and the viewer hand a selection to each other.
    bool syncingSelection = false;
    // The selected paths in the order they were selected, the viewer's when the panel shows its
    // selection; see SceneSelectionActions::keepSelectionOrder().
    QStringList selectionOrder;

    // The rows the user expanded, kept across resets.
    QSet<QString> expanded;
    // Set while the panel, not the user, opens or closes rows.
    bool restoringExpansion = false;

    // The filter's walk over the stage, a step at a time on the main thread.
    QTimer filterDebounce;
    QTimer filterStep;
    QString filterPattern;
    QRegularExpression filterExpression;
    bool filterWildcard = false;
    SceneGraphPrimWalk filterWalk;
    QStringList filterMatches;
    int filterCap = kFilterCap;
    bool filterCapped = false;

    QTimer inspectorRefresh;

    Implementation(SceneGraphPanel* panel,
                   Gui* gui)
        : panel(panel)
        , gui(gui)
    {
    }

    bool isPinned() const
    {
        return userPinned && pinned;
    }

    // Whether the rows are the viewer's prims, so selections go both ways.
    bool sharesViewerSelection() const
    {
        if (!viewport) {
            return false;
        }
        if (!pinned) {
            return follow == SceneGraphPanel::eFollowViewer;
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

    // Whether a node can be created from the panel's selection now, even an empty one; when it
    // can't, the create key is left to the view, where Tab moves the focus.
    bool canCreateFromSelection() const
    {
        return SceneSelectionActions::createBlocker(gui, panel->getShownNode()).isEmpty();
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
        if (viewerSource && (model->getSource() == viewerSource) && viewer && viewer->getTimeLine()) {
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
        panel->setProperty("natronSceneGraphFollow", (follow == SceneGraphPanel::eFollowSelection) ? QString::fromUtf8("selection") : QString::fromUtf8("viewer"));
        panel->setProperty("natronSceneGraphNode", node ? QString::fromStdString(node->getFullyQualifiedName()) : QString());
        panel->setProperty("natronSceneGraphHasScene", hasScene());
        panel->setProperty("natronSceneGraphFilterBusy", filterStep.isActive() || filterDebounce.isActive());
        const SceneGraphStageSourcePtr& source = model->getSource();
        panel->setProperty("natronSceneGraphStageEditCount", QVariant::fromValue<qulonglong>(source ? source->getEditCount() : 0));
        // Twirling a row must not add to it: the rows' open state never reaches the stage.
        panel->setProperty("natronSceneGraphStageNotices", QVariant::fromValue<qulonglong>(model->getStageNoticeCount()));
    }

    // The scene node last selected in the node graph, or null when none is selected.
    NodePtr selectedSceneNode() const
    {
        NodeGraph* graph = gui ? gui->getLastSelectedGraph() : nullptr;
        if (!graph && gui) {
            graph = gui->getNodeGraph();
        }
        if (!graph) {
            return NodePtr();
        }
        const NodesGuiList& selection = graph->getSelectedNodes();
        for (NodesGuiList::const_reverse_iterator it = selection.rbegin(); it != selection.rend(); ++it) {
            NodePtr node = *it ? (*it)->getNode() : NodePtr();
            EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
            if (effect && node->isActivated() && effect->producesSceneData()) {
                return node;
            }
        }

        return NodePtr();
    }

    void updateStatus()
    {
        NodePtr node = panel->getShownNode();
        QString text;

        if (isPinned()) {
            const QString name = node ? QString::fromStdString(node->getFullyQualifiedName()) : SceneGraphPanel::tr("nothing");
            EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
            if (pinnedError.isEmpty()) {
                text = SceneGraphPanel::tr("Pinned to %1").arg(name);
            } else if (!effect || !node->isActivated() || !effect->producesSceneData()) {
                text = SceneGraphPanel::tr("The pinned node %1 puts out no scene.").arg(name);
            } else {
                text = SceneGraphPanel::tr("Pinned to %1, which failed: %2").arg(name, pinnedError);
            }
        } else if (follow == SceneGraphPanel::eFollowSelection) {
            text = node ? SceneGraphPanel::tr("Selected: %1").arg(QString::fromStdString(node->getFullyQualifiedName()))
                        : SceneGraphPanel::tr("Select a node that puts out a scene");
        } else if (viewer) {
            text = node ? SceneGraphPanel::tr("%1 shows %2").arg(QString::fromStdString(viewer->getInternalNode() ? viewer->getInternalNode()->getScriptName_mt_safe() : std::string()), QString::fromStdString(node->getFullyQualifiedName()))
                        : SceneGraphPanel::tr("%1 shows no scene").arg(QString::fromStdString(viewer->getInternalNode() ? viewer->getInternalNode()->getScriptName_mt_safe() : std::string()));
        } else {
            text = SceneGraphPanel::tr("No viewer");
        }
        status->setText(text);
        if (!filterPattern.isEmpty()) {
            stack->setCurrentWidget(results);
        } else if (hasScene()) {
            stack->setCurrentWidget(tree);
        } else {
            stack->setCurrentWidget(noScene);
        }
        publishState();
    }

    // The source of what the panel shows: the viewer's stage when it shows that scene.
    SceneGraphStageSourcePtr wantedSource() const
    {
        if (pinned) {
            return sharesViewerSelection() ? viewerSource : pinned;
        }

        return (follow == SceneGraphPanel::eFollowViewer) ? viewerSource : SceneGraphStageSourcePtr();
    }

    void applySource()
    {
        const SceneGraphStageSourcePtr wanted = wantedSource();
        if (model->getSource() == wanted) {
            return;
        }
        model->setSource(wanted);
        if (pinned && (wanted == pinned)) {
            // Not evaluated while the viewer showed it.
            NodePtr node = pinned->getNode();
            pinnedMarkCount = node ? node->getHashMarkCount() : 0;
            pinned->request(currentFrame());
        }
    }

    void retirePinned()
    {
        if (!pinned) {
            return;
        }
        pinned->requestStop();
        retired.push_back(pinned);
        pinned.reset();
        retiredReap.start();
    }

    void reapRetired()
    {
        retired.erase(std::remove_if(retired.begin(), retired.end(), [](const std::shared_ptr<SceneNodeStageSource>& source) {
                          return source->isFinished();
                      }),
                      retired.end());
        if (retired.empty()) {
            retiredReap.stop();
        }
    }

    void connectViewport(SceneViewport* newViewport)
    {
        if (viewport == newViewport) {
            if (viewerSource && (model->getSource() == viewerSource)) {
                model->refresh();
            } else {
                applySource();
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
        }
        viewerSource = viewport ? std::make_shared<ViewerStageSource>(viewport) : SceneGraphStageSourcePtr();
        applySource();
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
                QObject::connect(viewerTimeLine, &TimeLine::frameChanged, panel, [this]() { inspectorRefresh.start(); });
            }
        }
        connectViewport((viewer && viewer->isShowingScene()) ? viewer->getSceneViewport() : nullptr);
        if (viewport && sharesViewerSelection()) {
            panel->showSelection(viewport->getSelection());
        }
        updateStatus();
        inspectorRefresh.start();
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
        filterWalk.stop();
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
        filterWalk.start();
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

        if (!source) {
            filterWalk.stop();

            return true;
        }
        source->readStage([&](const SceneStage* sceneStage) {
            filterWalk.step(sceneStage, kFilterStepMs, filterCap, [this](const QString& name) { return filterMatchesName(name); }, &filterMatches);
        });
        filterCapped = !filterWalk.isOver() && (filterMatches.size() >= filterCap);

        return filterWalk.isOver() || filterCapped;
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
        selectionOrder = paths;
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
        } else {
            results->selectionModel()->clearCurrentIndex();
        }
        syncingSelection = wasSyncing;
    }

    void selectTreeRows(const QStringList& paths)
    {
        const bool wasSyncing = syncingSelection;
        const bool wasRestoring = restoringExpansion;
        QItemSelection selection;
        QSet<QString> selected;
        QModelIndex current;

        syncingSelection = true;
        restoringExpansion = true;
        // A path shown twice keeps its last place, so the current prim stays last.
        const QStringList ordered = paths;
        selectionOrder.clear();
        {
            QSet<QString> seen;
            for (int i = (int)ordered.size() - 1; i >= 0; --i) {
                if (!seen.contains(ordered[i])) {
                    seen.insert(ordered[i]);
                    selectionOrder.prepend(ordered[i]);
                }
            }
        }
        // The current prim is last; the cap keeps it.
        for (int i = std::max(0, (int)ordered.size() - kShownSelectionCap); i < ordered.size(); ++i) {
            const QString& path = ordered[i];
            const QModelIndex index = model->indexOfPath(path, true);
            if (!index.isValid()) {
                continue;
            }
            if (!selected.contains(path)) {
                selected.insert(path);
                for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
                    if (tree->isExpanded(parent)) {
                        break;
                    }
                    tree->expand(parent);
                }
                selection.select(index, index.sibling(index.row(), SceneGraphModel::eColumnCount - 1));
            }
            current = index;
        }
        tree->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        if (current.isValid()) {
            tree->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
            tree->scrollTo(current);
        }
        restoringExpansion = wasRestoring;
        syncingSelection = wasSyncing;
    }

    QString currentPath() const
    {
        // A selected prim the filter hides has no result row, only its row in the tree.
        if (stack->currentWidget() == results) {
            const QModelIndex current = results->selectionModel()->currentIndex();

            if (current.isValid() && !resultsModel->isMoreRow(current)) {
                return current.data().toString();
            }
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
    _imp->pinButton->setToolTip(tr("Keeps showing the scene of the node shown now, whichever viewer is active, whatever it views and whatever is selected."));
    headerLayout->addWidget(_imp->pinButton);
    _imp->followChooser = new QComboBox(header);
    _imp->followChooser->setObjectName(QString::fromUtf8("sceneGraphFollow"));
    _imp->followChooser->addItem(tr("Viewer"));
    _imp->followChooser->addItem(tr("Node Selection"));
    _imp->followChooser->setToolTip(tr("What the panel follows when not pinned. Viewer: the scene the active viewer shows. "
                                       "Node Selection: the scene of the node last selected in the node graph that puts out "
                                       "one; selecting other nodes keeps it."));
    headerLayout->addWidget(_imp->followChooser);
    _imp->nodeChooser = new SceneNodeChooser(gui, header);
    _imp->nodeChooser->setObjectName(QString::fromUtf8("sceneGraphNode"));
    _imp->nodeChooser->setToolTip(tr("The node whose scene is shown: the one followed, or a node pinned by name."));
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

    _imp->inspector = new ScenePrimInspector(splitter);
    splitter->addWidget(_imp->stack);
    splitter->addWidget(_imp->inspector);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    _imp->filterDebounce.setSingleShot(true);
    _imp->filterDebounce.setInterval(200);
    _imp->filterStep.setInterval(0);
    _imp->inspectorRefresh.setSingleShot(true);
    _imp->inspectorRefresh.setInterval(0);
    _imp->pinnedPoll.setInterval(kPinnedPollMs);
    _imp->retiredReap.setInterval(kPinnedPollMs);
    _imp->selectionPoll.setInterval(kSelectionPollMs);

    QObject::connect(_imp->pinButton, &QToolButton::toggled, this, &SceneGraphPanel::onPinToggled);
    QObject::connect(_imp->nodeChooser, qOverload<int>(&QComboBox::activated), this, &SceneGraphPanel::onNodeChosen);
    QObject::connect(_imp->filter, &QLineEdit::textChanged, &_imp->filterDebounce, qOverload<>(&QTimer::start));
    QObject::connect(&_imp->filterDebounce, &QTimer::timeout, this, &SceneGraphPanel::onFilterEdited);
    QObject::connect(&_imp->filterStep, &QTimer::timeout, this, &SceneGraphPanel::onFilterStep);
    QObject::connect(&_imp->inspectorRefresh, &QTimer::timeout, this, &SceneGraphPanel::refreshInspector);
    // Each page has its own current row, and showing a page changes neither of them.
    QObject::connect(_imp->stack, &QStackedWidget::currentChanged, &_imp->inspectorRefresh, qOverload<>(&QTimer::start));
    QObject::connect(&_imp->pinnedPoll, &QTimer::timeout, this, &SceneGraphPanel::onPinnedPoll);
    QObject::connect(&_imp->retiredReap, &QTimer::timeout, this, [this]() { _imp->reapRetired(); });
    QObject::connect(&_imp->selectionPoll, &QTimer::timeout, this, &SceneGraphPanel::onSelectionPoll);
    QObject::connect(_imp->followChooser, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        setFollow((index == (int)eFollowSelection) ? eFollowSelection : eFollowViewer);
    });
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onTreeSelectionChanged);
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->results->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onResultsSelectionChanged);
    QObject::connect(_imp->results->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->results, &QListView::clicked, this, &SceneGraphPanel::onResultActivated);
    // Queued: the model fetches no rows while it is resetting, so nothing could be reopened then.
    QObject::connect(_imp->model, &QAbstractItemModel::modelReset, this, &SceneGraphPanel::onModelReset, Qt::QueuedConnection);
    // Keys are claimed in the views' own event filter so a window-level shortcut cannot take them.
    _imp->tree->installEventFilter(this);
    _imp->results->installEventFilter(this);
    _imp->results->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(_imp->tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->tree, pos); });
    QObject::connect(_imp->results, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->results, pos); });
    QObject::connect(_imp->tree, &QTreeView::expanded, this, &SceneGraphPanel::onExpanded);
    QObject::connect(_imp->tree, &QTreeView::collapsed, this, &SceneGraphPanel::onCollapsed);
    if (gui) {
        QObject::connect(gui, &Gui::activeViewerChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        QObject::connect(gui, &Gui::viewersChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        GuiAppInstancePtr app = gui->getApp();
        if (app && app->getTimeLine()) {
            QObject::connect(app->getTimeLine().get(), &TimeLine::frameChanged, this, [this]() { _imp->inspectorRefresh.start(); });
        }
    }
    _imp->followViewer();
}

SceneGraphPanel::~SceneGraphPanel()
{
    _imp->inspector->stop();
    // Waits, once: no worker may call back into a destroyed panel.
    for (const std::shared_ptr<SceneNodeStageSource>& source : _imp->retired) {
        source->stop();
    }
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
    _imp->userPinned = bool(node);
    if (node) {
        showNodeScene(node);
    } else if (_imp->follow == eFollowSelection) {
        if (NodePtr selected = _imp->selectedSceneNode()) {
            _imp->selectedNode = selected;
        }
        showNodeScene(_imp->selectedNode.lock());
    } else {
        showNodeScene(NodePtr());
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

void
SceneGraphPanel::showNodeScene(const NodePtr& node)
{
    if (_imp->pinned && node && (_imp->pinned->getNode() == node)) {
        return;
    }
    _imp->retirePinned();
    _imp->pinnedError.clear();
    _imp->pinnedPoll.stop();
    if (node) {
        QPointer<SceneGraphPanel> self(this);
        _imp->pinned = std::make_shared<SceneNodeStageSource>(node, this, [self](bool ok, const QString& error) {
            if (self) {
                self->onPinnedSynced(ok, error);
            } });
        _imp->pinnedPoll.start();
    }
    _imp->applySource();
}

void
SceneGraphPanel::setFollow(FollowEnum follow)
{
    if (_imp->follow != follow) {
        _imp->follow = follow;
        const bool blocked = _imp->followChooser->blockSignals(true);
        _imp->followChooser->setCurrentIndex((int)follow);
        _imp->followChooser->blockSignals(blocked);
        _imp->nodeChooser->followLabel = (follow == eFollowSelection) ? tr("(Selected Node)") : tr("(Active Viewer)");
        _imp->nodeChooser->setItemText(0, _imp->nodeChooser->followLabel);
    }
    if (follow == eFollowSelection) {
        _imp->selectedNode = _imp->selectedSceneNode();
        _imp->selectionPoll.start();
    } else {
        _imp->selectedNode.reset();
        _imp->selectionPoll.stop();
    }
    if (!_imp->userPinned) {
        showNodeScene((follow == eFollowSelection) ? _imp->selectedNode.lock() : NodePtr());
    }
    _imp->followViewer();
}

SceneGraphPanel::FollowEnum
SceneGraphPanel::getFollow() const
{
    return _imp->follow;
}

NodePtr
SceneGraphPanel::getPinnedNode() const
{
    return _imp->isPinned() ? _imp->pinned->getNode() : NodePtr();
}

NodePtr
SceneGraphPanel::getShownNode() const
{
    if (_imp->pinned) {
        return _imp->pinned->getNode();
    }
    if (_imp->follow == eFollowSelection) {
        return NodePtr();
    }

    return (_imp->viewer && _imp->viewport) ? _imp->viewer->getSceneViewedNode() : NodePtr();
}

void
SceneGraphPanel::showSelection(const QStringList& paths)
{
    _imp->selectTreeRows(paths);
    if (!_imp->filterPattern.isEmpty()) {
        _imp->selectResults(paths);
    }
    _imp->inspectorRefresh.start();
}

QStringList
SceneGraphPanel::getSelection() const
{
    const QStringList selected = _imp->selectedPaths((_imp->stack->currentWidget() == _imp->results) ? (QAbstractItemView*)_imp->results : (QAbstractItemView*)_imp->tree);

    return SceneSelectionActions::keepSelectionOrder(_imp->selectionOrder, selected);
}

void
SceneGraphPanel::copySelectedPaths()
{
    const QStringList paths = getSelection();

    if (!paths.isEmpty()) {
        QApplication::clipboard()->setText(paths.join(QLatin1Char(' ')));
    }
}

void
SceneGraphPanel::showRowMenu(QAbstractItemView* view, const QPoint& pos)
{
    const QStringList selection = getSelection();
    QMenu menu(view);
    QAction* copyPath = menu.addAction(tr("Copy Path"));
    copyPath->setObjectName(QString::fromUtf8("sceneGraphCopyPath"));
    copyPath->setEnabled(view->indexAt(pos).isValid() && !selection.isEmpty());
    QObject::connect(copyPath, &QAction::triggered, this, &SceneGraphPanel::copySelectedPaths);
    menu.addSeparator();
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
    create->setEnabled(_imp->canCreateFromSelection());
    QAction* chosen = menu.exec(view->viewport()->mapToGlobal(pos));
    if (chosen == setOnNode) {
        _imp->runSelectionAction(1, selection);
    } else if (chosen == create) {
        _imp->runSelectionAction(2, selection);
    }
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
    if (_imp->viewerSource && (_imp->model->getSource() == _imp->viewerSource)) {
        _imp->model->refresh();
        if (!_imp->filterPattern.isEmpty()) {
            _imp->filterDebounce.start();
        }
    }
    _imp->updateStatus();
    _imp->inspectorRefresh.start();
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
    if (_imp->syncingSelection) {
        return;
    }
    _imp->selectionOrder = SceneSelectionActions::keepSelectionOrder(_imp->selectionOrder, _imp->selectedPaths(_imp->tree));
    if (!_imp->sharesViewerSelection()) {
        return;
    }
    _imp->syncingSelection = true;
    _imp->viewport->setSelection(_imp->selectionOrder);
    _imp->syncingSelection = false;
}

void
SceneGraphPanel::onResultsSelectionChanged()
{
    if (_imp->syncingSelection) {
        return;
    }
    if (!_imp->sharesViewerSelection()) {
        _imp->selectionOrder = SceneSelectionActions::keepSelectionOrder(_imp->selectionOrder, _imp->selectedPaths(_imp->results));

        return;
    }
    // Selected prims the filter hides stay selected.
    const QStringList viewerSelection = _imp->viewport->getSelection();
    QStringList paths = viewerSelection;
    for (const QString& match : _imp->filterMatches) {
        paths.removeAll(match);
    }
    paths << _imp->selectedPaths(_imp->results);
    _imp->selectionOrder = SceneSelectionActions::keepSelectionOrder(viewerSelection, paths);
    _imp->syncingSelection = true;
    _imp->viewport->setSelection(_imp->selectionOrder);
    _imp->selectTreeRows(_imp->selectionOrder);
    _imp->syncingSelection = false;
}

void
SceneGraphPanel::onResultActivated(const QModelIndex& index)
{
    if (!_imp->resultsModel->isMoreRow(index)) {
        return;
    }
    _imp->filterCap += kFilterCap;
    // Gone at once, so a second click on it is a click on nothing, and it never turns into a
    // selectable path.
    const bool wasSyncing = _imp->syncingSelection;
    _imp->syncingSelection = true;
    _imp->resultsModel->hasMoreRow = false;
    _imp->resultsModel->removeRows(index.row(), 1);
    _imp->syncingSelection = wasSyncing;
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
        if (_imp->userPinned) {
            pinNode(NodePtr());
        } else {
            _imp->selectedNode.reset();
            showNodeScene(NodePtr());
            _imp->followViewer();
        }

        return;
    }
    const U64 markCount = node->getHashMarkCount();
    // While the viewer shows the node, the viewer evaluates it.
    if ((markCount != _imp->pinnedMarkCount) && (_imp->model->getSource() == _imp->pinned)) {
        _imp->pinnedMarkCount = markCount;
        _imp->pinned->request(_imp->currentFrame());
    }
}

void
SceneGraphPanel::onPinnedSynced(bool ok,
                                const QString& error)
{
    if (!_imp->pinned || (_imp->model->getSource() != _imp->pinned)) {
        return;
    }
    _imp->pinnedError = ok ? QString() : (error.isEmpty() ? tr("unknown error") : error);
    _imp->model->refresh();
    if (!_imp->filterPattern.isEmpty()) {
        _imp->filterDebounce.start();
    }
    _imp->updateStatus();
    _imp->inspectorRefresh.start();
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
    if (_imp->restoringExpansion) {
        return;
    }
    _imp->expanded.insert(_imp->model->pathOf(index));
    _imp->publishState();
}

void
SceneGraphPanel::onCollapsed(const QModelIndex& index)
{
    if (_imp->restoringExpansion) {
        return;
    }
    _imp->expanded.remove(_imp->model->pathOf(index));
    _imp->publishState();
}

void
SceneGraphPanel::onSelectionPoll()
{
    if ((_imp->follow != eFollowSelection) || _imp->userPinned) {
        return;
    }
    // Selecting no scene node keeps the scene shown: only choosing another one switches.
    NodePtr node = _imp->selectedSceneNode();
    if (!node || (node == _imp->selectedNode.lock())) {
        return;
    }
    _imp->selectedNode = node;
    showNodeScene(node);
    _imp->followViewer();
}

void
SceneGraphPanel::onCurrentChanged(const QModelIndex& /*current*/)
{
    _imp->inspectorRefresh.start();
}

void
SceneGraphPanel::refreshInspector()
{
    _imp->inspector->setPrim(_imp->model->getSource(), _imp->currentPath(), _imp->currentFrame(), _imp->framesPerSecond());
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
    // Copy and the 3D viewer's selection keys work on the panel's selection. A key press is taken
    // before the view's own handling, which would spend Tab on moving the focus.
    if (((watched == _imp->tree) || (watched == _imp->results)) && ((e->type() == QEvent::ShortcutOverride) || (e->type() == QEvent::KeyPress))) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (k->matches(QKeySequence::Copy)) {
            if (e->type() == QEvent::KeyPress) {
                copySelectedPaths();
            }
            e->accept();

            return true;
        }
        const int action = Implementation::selectionActionForKey(k->modifiers(), k->key());
        const QStringList selection = (action != 0) ? getSelection() : QStringList();
        if ((action == 1) || ((action == 2) && _imp->canCreateFromSelection())) {
            e->accept();
            if (e->type() == QEvent::KeyPress) {
                _imp->runSelectionAction(action, selection);
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
