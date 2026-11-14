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
#include <condition_variable>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringListModel>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneNodeStageSource.h"
#include "Gui/ScenePrimInspector.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/UndoHistory.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {

// How many prims a filter lists before it offers more.
const int kFilterCap = 1000;

// How long one step of the filter walk may hold the stage, in milliseconds.
const int kFilterStepMs = 8;

// How many paths of a viewer selection are shown in the tree.
const int kShownSelectionCap = 2000;

// How often sources let go of are checked for their workers having quit, in milliseconds.
const int kRetiredReapMs = 250;

// How long the node selection must stay put before the panel reads the selected node, so that
// clicking through nodes reads only the last.
const int kSelectionSettleMs = 150;

// How long edits must pause before the pinned node is read again, so that dragging a value reads
// it once the drag rests rather than for every step.
const int kEditSettleMs = 100;

// How many stages of nodes shown before are kept, besides the shown one, for a return that
// composes nothing.
const std::size_t kStageCacheSize = 4;

// expandAll() lays out every row on the main thread. Past this many the filtered tree opens one
// level and the user opens the rest.
const int kChangedHereExpandAllRows = 20000;

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
        SceneWriteBarrier::ReadScope scope;
        std::shared_lock<std::shared_mutex> lock(_gate->mutex);

        if (_gate->renderer) {
            _gate->renderer->readStage(read);
        } else {
            read(nullptr);
        }
    }

    bool tryReadStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        SceneWriteBarrier::ReadScope scope;
        std::shared_lock<std::shared_mutex> lock(_gate->mutex);

        if (!_gate->renderer) {
            read(nullptr);

            return true;
        }

        return _gate->renderer->tryReadStage(read);
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

// Paints a row's payload state at the end of its name, and before it a dashed square when the viewer
// can box the row only by loading it. The states are roles, not decorations, so that the name keeps
// its type's icon and is elided before it reaches the marks.
class PayloadBadgeDelegate
    : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const int state = index.data(SceneGraphModel::ePayloadStateRole).toInt();
        const bool unknownBounds = index.data(SceneGraphModel::eUnknownBoundsRole).toBool();

        if (state == SceneGraphModel::ePayloadStateNone) {
            QStyledItemDelegate::paint(painter, option, index);

            return;
        }
        const int kBadgeSize = 8;
        const int kBadgeMargin = 4;
        const int badges = unknownBounds ? 2 : 1;
        QStyleOptionViewItem nameOption(option);
        nameOption.rect.setRight(option.rect.right() - badges * (kBadgeSize + kBadgeMargin) - kBadgeMargin);
        QStyle* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, option.widget);
        QStyledItemDelegate::paint(painter, nameOption, index);

        const QRect badge(option.rect.right() - kBadgeSize - kBadgeMargin, option.rect.center().y() - kBadgeSize / 2, kBadgeSize, kBadgeSize);
        const QColor grey(170, 170, 170);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        switch (state) {
        case SceneGraphModel::ePayloadStateUnloaded:
            painter->setPen(QPen(grey, 1.5));
            painter->setBrush(Qt::NoBrush);
            break;
        case SceneGraphModel::ePayloadStateLoading:
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(230, 170, 50));
            break;
        case SceneGraphModel::ePayloadStateFailed:
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(220, 70, 60));
            break;
        default:
            painter->setPen(Qt::NoPen);
            painter->setBrush(grey);
            break;
        }
        painter->drawEllipse(badge);
        if (unknownBounds) {
            painter->setPen(QPen(QColor(230, 170, 50), 1., Qt::DotLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(QRectF(badge.translated(-(kBadgeSize + kBadgeMargin), 0)).adjusted(0.5, 0.5, -0.5, -0.5));
        }
        painter->restore();
    }
};

// The mark in front of a draw state a node's rule sets: a diamond, as a node rather than the viewer
// decides it.
QIcon
graphOriginIcon()
{
    static const QIcon icon = []() {
        const int kSize = 20;
        QPixmap pixmap(kSize, kSize);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(90, 180, 200));
        const QPointF diamond[4] = { QPointF(kSize / 2., 2.), QPointF(kSize - 2., kSize / 2.), QPointF(kSize / 2., kSize - 2.), QPointF(2., kSize / 2.) };
        painter.drawPolygon(diamond, 4);
        painter.end();

        return QIcon(pixmap);
    }();

    return icon;
}

// Edits the draw state column with a list of the states that opens with the editor. A state the
// viewer sets reads plain, one a node's rule sets on the row has a diamond in front of it, and
// one the row inherits is drawn dimmed and in italics, so that the rows set stand out.
class DrawStateDelegate
    : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent,
                          const QStyleOptionViewItem& /*option*/,
                          const QModelIndex& /*index*/) const override
    {
        QComboBox* editor = new QComboBox(parent);
        editor->setObjectName(QString::fromUtf8("sceneGraphDrawStateEditor"));
        for (int state = (int)eSceneDrawStateBBox; state <= (int)eSceneDrawStateHidden; ++state) {
            editor->addItem(SceneGraphModel::drawStateLabel(state), state);
        }
        editor->addItem(SceneGraphPanel::tr("Reset to Graph"), -1);
        editor->setItemData(editor->count() - 1, SceneGraphPanel::tr("Clears the state of the row and of every row below it."), Qt::ToolTipRole);
        DrawStateDelegate* self = const_cast<DrawStateDelegate*>(this);
        QObject::connect(editor, qOverload<int>(&QComboBox::activated), self, [self, editor]() {
            Q_EMIT self->commitData(editor);
            Q_EMIT self->closeEditor(editor);
        });
        QTimer::singleShot(0, editor, [editor]() {
            if (editor->isVisible()) {
                editor->showPopup();
            }
        });

        return editor;
    }

    void setEditorData(QWidget* editor,
                       const QModelIndex& index) const override
    {
        QComboBox* combo = static_cast<QComboBox*>(editor);
        combo->setCurrentIndex(combo->findData(index.data(Qt::EditRole)));
    }

    void setModelData(QWidget* editor,
                      QAbstractItemModel* model,
                      const QModelIndex& index) const override
    {
        QComboBox* combo = static_cast<QComboBox*>(editor);
        if (combo->currentIndex() >= 0) {
            model->setData(index, combo->currentData(), Qt::EditRole);
        }
    }

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.data(SceneGraphModel::eDrawStateInheritedRole).toBool()) {
            option->palette.setColor(QPalette::Text, option->palette.color(QPalette::Disabled, QPalette::Text));
            option->palette.setColor(QPalette::HighlightedText, option->palette.color(QPalette::Disabled, QPalette::HighlightedText));
        }
        if (index.data(SceneGraphModel::eDrawStateOriginRole).toInt() == (int)eSceneDrawOriginGraphPolicy) {
            const int kMarkSize = 10;
            option->features |= QStyleOptionViewItem::HasDecoration;
            option->icon = graphOriginIcon();
            option->decorationSize = QSize(kMarkSize, kMarkSize);
            option->decorationPosition = QStyleOptionViewItem::Left;
        }
    }
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
    QTimer editSettle;
    // Sources let go of whose workers have not quit yet, kept until they have.
    std::vector<std::shared_ptr<SceneNodeStageSource>> retired;
    QTimer retiredReap;

    struct CachedStage {
        std::shared_ptr<SceneNodeStageSource> source;
        U64 markCount = 0;
        QString error;
    };

    // Stages of nodes shown before, the least recently shown first.
    std::vector<CachedStage> cachedStages;
    // Set while the rows stay those of the stage left, greyed, until pinned's first evaluation.
    bool switching = false;
    // Evaluations that reached a stage of the panel's own.
    U64 composeCount = 0;

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
    // The row at the top of the tree when it last scrolled other than by rows coming and going.
    QString topPath;
    // Set from the moment rows are about to go until the tree is scrolled back to topPath.
    bool holdTopPath = false;
    QTimer viewRestore;
    // What viewRestore reopens: every remembered row, or only those below the rows rows were
    // inserted under since, so that fetching one row's children walks no other remembered row.
    bool restoreAll = false;
    QSet<QString> insertedUnder;

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

    struct ChangedHereRequest {
        SceneGraphStageSourcePtr source;
        NodePtr node;
        bool wantRows = false;
        U64 generation = 0;
    };

    QToolButton* changedHereButton = nullptr;
    QToolButton* resetDrawStatesButton = nullptr;
    QToolButton* bakeDrawStatesButton = nullptr;
    QToolButton* cancelLoadsButton = nullptr;
    QTreeView* changedHereTree = nullptr;
    SceneChangedHereModel* changedHereModel = nullptr;
    SceneGraphStageSourcePtr changedHereSource;
    NodeWPtr changedHereNode;
    U64 changedHereEditCount = 0;
    bool changedHereRead = false;
    bool changedHereRowsAsked = false;
    bool changedHereBusy = false;
    // The viewer's selection moved while the lazy tree was off screen. Selecting there can read a
    // whole sibling list, so it waits until that page shows again and the worker has read the rows.
    bool treeSelectionOwed = false;
    // Selecting or reopening tree rows found the stage held by an edit; both happen again once the
    // model has read what it owes.
    bool treeSelectionDeferred = false;
    bool expansionDeferred = false;
    // The filter's walk found the stage held by an edit. The edit's end restarts the filter.
    bool filterDeferred = false;
    struct OwedRowsRequest {
        SceneGraphStageSourcePtr source;
        QStringList paths;
        U64 editCount = 0;
        U64 generation = 0;
    };

    std::atomic<U64> owedRowsGeneration { 0 };
    bool hasOwedRowsRequest = false;
    OwedRowsRequest owedRowsRequest;
    std::atomic<U64> changedHereGeneration { 0 };
    std::mutex changedHereMutex;
    std::condition_variable changedHereQueued;
    bool hasChangedHereRequest = false;
    bool changedHereQuit = false;
    ChangedHereRequest changedHereRequest;
    std::thread changedHereWorker;

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
        panel->setProperty("natronSceneGraphChangedHereOnly", changedHereOnly());
        panel->setProperty("natronSceneGraphChangedHereBusy", changedHereBusy);
        panel->setProperty("natronSceneGraphChangedHereCount", model->getChangedHereCount());
        panel->setProperty("natronSceneGraphChangedHereRows", changedHereModel->getRowCount());
        panel->setProperty("natronSceneGraphFilterBusy", filterStep.isActive() || filterDebounce.isActive() || filterDeferred);
        const SceneGraphStageSourcePtr& source = model->getSource();
        panel->setProperty("natronSceneGraphStageEditCount", QVariant::fromValue<qulonglong>(source ? source->getEditCount() : 0));
        // Twirling a row must not add to it: the rows' open state never reaches the stage.
        panel->setProperty("natronSceneGraphStageNotices", QVariant::fromValue<qulonglong>(model->getStageNoticeCount()));
        panel->setProperty("natronSceneGraphComposeCount", QVariant::fromValue<qulonglong>(composeCount));
        panel->setProperty("natronSceneGraphSwitching", switching);
        panel->setProperty("natronSceneGraphCachedStages", (int)cachedStages.size());
        panel->setProperty("natronSceneGraphTimersActive", pinnedPoll.isActive() || editSettle.isActive() || selectionPoll.isActive() || retiredReap.isActive());
        panel->setProperty("natronSceneGraphTopPath", topPath);
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

        if (switching && node) {
            text = SceneGraphPanel::tr("Reading %1...").arg(QString::fromStdString(node->getFullyQualifiedName()));
        } else if (isPinned()) {
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
            stack->setCurrentWidget(changedHereOnly() ? changedHereTree : tree);
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
            setSwitching(false);

            return;
        }
        if (pinned && (wanted == pinned)) {
            // Not evaluated while the viewer showed it, nor while kept in the cache.
            NodePtr node = pinned->getNode();
            const U64 markCount = node ? node->getHashMarkCount() : 0;
            if (markCount != pinnedMarkCount) {
                pinnedMarkCount = markCount;
                pinned->request(currentFrame());
            }
            if (pinned->getComposeCount() == 0) {
                setSwitching(true);

                return;
            }
        }
        setSwitching(false);
        holdTopPath = true;
        model->setSource(wanted);
        inspectorRefresh.start();
        // The edit it was waiting out belongs to the stage just left.
        if (filterDeferred) {
            filterDeferred = false;
            filterStep.start();
        }
    }

    // The node whose stage the rows are read from. While switching that is still the node left,
    // not the one being read, which getShownNode() names.
    NodePtr modelNode() const
    {
        if (!switching) {
            return panel->getShownNode();
        }
        const SceneGraphStageSourcePtr& source = model->getSource();
        if (!source) {
            return NodePtr();
        }
        if (source == viewerSource) {
            return (viewer && viewport) ? viewer->getSceneViewedNode() : NodePtr();
        }
        const std::shared_ptr<SceneNodeStageSource> own = std::dynamic_pointer_cast<SceneNodeStageSource>(source);

        return own ? own->getNode() : NodePtr();
    }

    // Greys the rows of the stage being left while the next one is read.
    void setSwitching(bool on)
    {
        if (switching == on) {
            return;
        }
        switching = on;
        stack->setEnabled(!on);
        updateStatus();
    }

    void retireSource(const std::shared_ptr<SceneNodeStageSource>& source)
    {
        source->requestStop();
        retired.push_back(source);
        retiredReap.start();
    }

    // Keeps pinned for a later return, unless it has not composed yet, which abandons its work.
    void cachePinned()
    {
        if (!pinned) {
            return;
        }
        NodePtr node = pinned->getNode();
        if (!node || !node->isActivated() || (pinned->getComposeCount() == 0)) {
            retirePinned();

            return;
        }
        CachedStage cached;
        cached.source = pinned;
        cached.markCount = pinnedMarkCount;
        cached.error = pinnedError;
        cachedStages.push_back(cached);
        pinned.reset();
        while (cachedStages.size() > kStageCacheSize) {
            retireSource(cachedStages.front().source);
            cachedStages.erase(cachedStages.begin());
        }
    }

    // Makes node's cached stage pinned, if there is one.
    bool takeCached(const NodePtr& node)
    {
        for (std::vector<CachedStage>::iterator it = cachedStages.begin(); it != cachedStages.end(); ++it) {
            if (it->source->getNode() == node) {
                pinned = it->source;
                pinnedMarkCount = it->markCount;
                pinnedError = it->error;
                cachedStages.erase(it);

                return true;
            }
        }

        return false;
    }

    void dropDeletedCachedStages()
    {
        for (std::vector<CachedStage>::iterator it = cachedStages.begin(); it != cachedStages.end();) {
            NodePtr node = it->source->getNode();
            if (!node || !node->isActivated()) {
                retireSource(it->source);
                it = cachedStages.erase(it);
            } else {
                ++it;
            }
        }
    }

    // A worker's evaluation of source reached its stage.
    void onSourceSynced(const std::shared_ptr<SceneNodeStageSource>& source,
                        bool ok,
                        const QString& error)
    {
        ++composeCount;
        if (source && (source == pinned)) {
            panel->onPinnedSynced(ok, error);

            return;
        }
        for (CachedStage& cached : cachedStages) {
            if (cached.source == source) {
                cached.error = ok ? QString() : (error.isEmpty() ? SceneGraphPanel::tr("unknown error") : error);
            }
        }
        publishState();
    }

    // The panel's own stage serviced loads.
    void onSourceLoadsChanged(const std::shared_ptr<SceneNodeStageSource>& source)
    {
        if (!source || (source != pinned) || (model->getSource() != pinned)) {
            return;
        }
        model->settleLoads();
        if (!filterPattern.isEmpty()) {
            filterDebounce.start();
        }
        updateStatus();
        inspectorRefresh.start();
    }

    // Whether payloads can be loaded on the stage the rows are read from: the viewer's, or the
    // panel's own.
    bool loadsOnShownStage() const
    {
        return showsViewerStage() || (pinned && (model->getSource() == pinned));
    }

    void retirePinned()
    {
        if (!pinned) {
            return;
        }
        retireSource(pinned);
        pinned.reset();
    }

    void reapRetired()
    {
        retired.erase(std::remove_if(retired.begin(), retired.end(), [](const std::shared_ptr<SceneNodeStageSource>& source) {
                          return source->isFinished();
                      }),
                      retired.end());
        if (retired.empty()) {
            retiredReap.stop();
            publishState();
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
            QObject::connect(viewport, &SceneViewport::sceneSynced, panel, [this]() { updateBakeButton(); });
            QObject::connect(viewport, &SceneViewport::stageLoadsChanged, panel, &SceneGraphPanel::onStageLoadsChanged);
            QObject::connect(viewport, &SceneViewport::selectionChanged, panel, &SceneGraphPanel::onViewportSelectionChanged);
            QObject::connect(viewport, &SceneViewport::drawStatesChanged, panel, [this]() {
                model->refreshDrawStates();
                updateBakeButton();
            });
        }
        linkDrawStates();
        viewerSource = viewport ? std::make_shared<ViewerStageSource>(viewport) : SceneGraphStageSourcePtr();
        applySource();
    }

    void updateBakeButton()
    {
        if (bakeDrawStatesButton) {
            bakeDrawStatesButton->setEnabled(viewport && SceneSelectionActions::bakeBlocker(viewer.data()).isEmpty());
        }
        if (cancelLoadsButton) {
            cancelLoadsButton->setEnabled(viewport && viewport->getRenderer().hasPendingStageLoads());
        }
    }

    // The draw state column reads and edits the linked viewer, whatever node the panel shows.
    void linkDrawStates()
    {
        if (resetDrawStatesButton) {
            resetDrawStatesButton->setEnabled(!viewport.isNull());
        }
        updateBakeButton();
        if (!viewport) {
            model->setDrawStateAccess(SceneGraphModel::DrawStatesReader(), SceneGraphModel::DrawStateWriter());

            return;
        }
        const QPointer<SceneViewport> linked = viewport.data();
        model->setDrawStateAccess([linked](const std::function<void(const SceneDrawStates&)>& read) { return linked && linked->tryReadDrawStates(read); }, [linked](const QString& path, int state) {
            if (!linked) {
                return;
            }
            if (state < 0) {
                linked->resetDrawStatesToGraph(path);
            } else {
                linked->setDrawState(path, QString::fromUtf8(sceneDrawStateName((SceneDrawStateEnum)state)));
            } });
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
                QObject::connect(viewer, &ViewerTab::sceneViewedNodeChanged, panel, [this]() { updateBakeButton(); });
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
        // The same viewport can come back for another viewed node.
        updateBakeButton();
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
            const QString path = (view == results) ? row.data().toString() : row.data(SceneGraphModel::ePrimPathRole).toString();
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

    bool showsViewerStage() const
    {
        return viewport && viewerSource && (model->getSource() == viewerSource);
    }

    void requestLoad(const QStringList& paths,
                     bool withDescendants)
    {
        if (showsViewerStage()) {
            viewport->requestLoad(paths, withDescendants);
            updateBakeButton();
        } else if (pinned && (model->getSource() == pinned)) {
            pinned->requestLoad(paths, withDescendants);
        } else {
            return;
        }
        model->markLoading(paths);
    }

    void requestUnload(const QStringList& paths)
    {
        for (const QString& path : paths) {
            const QString below = path + QLatin1Char('/');
            for (const QString& open : expanded.values()) {
                if (open.startsWith(below)) {
                    closeRow(open);
                }
            }
            // Left open, an unloaded row could not be twirled to load it again.
            if (model->getPayloadState(path) != SceneGraphModel::ePayloadStateNone) {
                closeRow(path);
            }
        }
        if (showsViewerStage()) {
            viewport->requestUnload(paths);
        } else if (pinned && (model->getSource() == pinned)) {
            pinned->requestUnload(paths);
        }
    }

    void closeRow(const QString& path)
    {
        const QModelIndex index = model->indexOfPath(path, false);

        if (index.isValid()) {
            tree->collapse(index);
        }
        expanded.remove(path);
    }

    // Reopens the remembered rows, or only those below one of under.
    void restoreExpansion(const QSet<QString>* under = nullptr)
    {
        QStringList paths;

        for (const QString& path : expanded) {
            bool wanted = !under;
            if (under) {
                for (const QString& parent : *under) {
                    if ((path.size() > parent.size() + 1) && path.startsWith(parent) && (path.at(parent.size()) == QLatin1Char('/'))) {
                        wanted = true;
                        break;
                    }
                }
            }
            if (wanted) {
                paths.push_back(path);
            }
        }
        std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return a.size() < b.size(); });
        restoringExpansion = true;
        expansionDeferred = false;
        for (const QString& path : paths) {
            const QModelIndex index = model->indexOfPath(path, true, &expansionDeferred);
            if (expansionDeferred) {
                break;
            }
            // Opening an unloaded payload row would load it: it stays closed, and remembered,
            // until the user twirls it.
            const SceneGraphModel::PayloadStateEnum state = model->getPayloadState(path);
            if ((state == SceneGraphModel::ePayloadStateUnloaded) || (state == SceneGraphModel::ePayloadStateFailed)) {
                continue;
            }
            if (index.isValid()) {
                tree->expand(index);
            }
        }
        restoringExpansion = false;
    }

    // Reopens what viewRestore was started for, and keeps it to reopen again while the model owes
    // reads.
    void restorePendingExpansion()
    {
        expansionDeferred = false;
        if (!expanded.isEmpty() && (restoreAll || !insertedUnder.isEmpty())) {
            restoreExpansion(restoreAll ? nullptr : &insertedUnder);
        }
        if (!expansionDeferred) {
            restoreAll = false;
            insertedUnder.clear();
        }
    }

    void rememberTopPath()
    {
        if (holdTopPath || restoringExpansion) {
            return;
        }
        const QModelIndex top = tree->indexAt(QPoint(0, 0));
        const QString path = model->pathOf(top);
        if (!path.isEmpty() && (path != topPath)) {
            topPath = path;
            panel->setProperty("natronSceneGraphTopPath", topPath);
        }
    }

    // Scrolls topPath, or the nearest ancestor of it with a row on show, to the top of the tree.
    void restoreScroll()
    {
        if (!holdTopPath || expansionDeferred) {
            return;
        }
        QString path = topPath;
        while (!path.isEmpty()) {
            bool busy = false;
            const QModelIndex index = model->indexOfPath(path, true, &busy);
            if (busy) {
                // Tried again once the model has read what it owes.
                expansionDeferred = true;

                return;
            }
            bool shown = index.isValid();
            for (QModelIndex parent = index.parent(); shown && parent.isValid(); parent = parent.parent()) {
                shown = tree->isExpanded(parent);
            }
            if (shown) {
                tree->scrollTo(index, QAbstractItemView::PositionAtTop);
                break;
            }
            path = path.left(path.lastIndexOf(QLatin1Char('/')));
        }
        holdTopPath = false;
        publishState();
    }

    bool changedHereOnly() const
    {
        return changedHereButton && changedHereButton->isChecked();
    }

    void selectChangedHereRows(const QStringList& paths)
    {
        const bool wasSyncing = syncingSelection;
        QItemSelection selection;
        QModelIndex current;

        syncingSelection = true;
        for (int i = std::max(0, (int)paths.size() - kShownSelectionCap); i < paths.size(); ++i) {
            const QModelIndex index = changedHereModel->indexOfPath(paths[i]);
            if (index.isValid()) {
                current = index;
                selection.select(index, index.sibling(index.row(), SceneGraphModel::eColumnCount - 1));
            }
        }
        changedHereTree->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        if (current.isValid()) {
            changedHereTree->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
            changedHereTree->scrollTo(current);
        } else {
            changedHereTree->selectionModel()->clearCurrentIndex();
        }
        syncingSelection = wasSyncing;
    }

    void setChangedHereRows(const SceneChangedHereRowsPtr& rows)
    {
        if (rows == changedHereModel->getRows()) {
            return;
        }
        changedHereModel->setRows(rows);
        if (changedHereModel->getRowCount() <= kChangedHereExpandAllRows) {
            changedHereTree->expandAll();
        } else {
            changedHereTree->expandToDepth(0);
        }
        selectChangedHereRows(selectionOrder);
        inspectorRefresh.start();
    }

    void onChangedHereStackPageShown()
    {
        ++owedRowsGeneration;
        if (stack->currentWidget() == changedHereTree) {
            selectChangedHereRows(selectionOrder);
        } else if ((stack->currentWidget() == tree) && treeSelectionOwed) {
            askForOwedRows();
        }
    }

    void askForOwedRows()
    {
        const SceneGraphStageSourcePtr source = model->getSource();

        if (!source || selectionOrder.isEmpty()) {
            selectTreeRows(selectionOrder);

            return;
        }
        OwedRowsRequest request;
        request.source = source;
        request.paths = selectionOrder.mid(std::max(0, (int)selectionOrder.size() - kShownSelectionCap));
        request.editCount = source->getEditCount();
        request.generation = ++owedRowsGeneration;
        {
            std::lock_guard<std::mutex> lock(changedHereMutex);
            owedRowsRequest = std::move(request);
            hasOwedRowsRequest = true;
        }
        changedHereQueued.notify_one();
    }

    void readOwedRows(const OwedRowsRequest& request)
    {
        const U64 generation = request.generation;
        SceneGraphPathRowsBuilder builder(request.paths);

        while (!builder.isDone() && (owedRowsGeneration == generation)) {
            request.source->readStage([&](const SceneStage* stage) {
                if (owedRowsGeneration == generation) {
                    builder.readBatch(stage);
                }
            });
        }
        if (owedRowsGeneration != generation) {
            return;
        }
        QMetaObject::invokeMethod(panel, [this, generation, source = request.source, editCount = request.editCount, rows = builder.take()]() { onOwedRowsRead(generation, source, editCount, rows); }, Qt::QueuedConnection);
    }

    void onOwedRowsRead(U64 generation,
                        const SceneGraphStageSourcePtr& source,
                        U64 editCount,
                        const SceneGraphPathRowsPtr& rows)
    {
        if (owedRowsGeneration != generation) {
            return;
        }
        // Rows read across an edit may not be the model's. What they leave out is read below, on
        // this thread, as selecting always could.
        if ((source == model->getSource()) && (source->getEditCount() == editCount)) {
            model->addPathRows(rows);
        }
        selectTreeRows(selectionOrder);
    }

    // Asks the worker for the prims the shown node's own layers have specs for, and for the filter's
    // rows when the filter is on, unless the last read was for this node, stage and edit count.
    void updateChangedHere()
    {
        const SceneGraphStageSourcePtr source = model->getSource();
        const NodePtr node = modelNode();
        const U64 editCount = source ? source->getEditCount() : 0;
        const NodeWPtr nodeWeak = node;
        const bool sameNode = !changedHereNode.owner_before(nodeWeak) && !nodeWeak.owner_before(changedHereNode);
        const bool wantRows = changedHereOnly();

        if (changedHereRead && (source == changedHereSource) && sameNode && (editCount == changedHereEditCount) && (!wantRows || changedHereRowsAsked)) {
            return;
        }
        changedHereRead = true;
        changedHereRowsAsked = wantRows;
        changedHereSource = source;
        changedHereNode = node;
        changedHereEditCount = editCount;
        const U64 generation = ++changedHereGeneration;
        ChangedHereRequest request;

        if (source && node) {
            request.source = source;
            request.node = node;
            request.wantRows = wantRows;
            request.generation = generation;
        }
        {
            std::lock_guard<std::mutex> lock(changedHereMutex);
            hasChangedHereRequest = source && node;
            changedHereRequest = std::move(request);
        }
        if (!source || !node) {
            changedHereBusy = false;
            model->setChangedHere(QSet<QString>());
            setChangedHereRows(SceneChangedHereRowsPtr());

            return;
        }
        changedHereBusy = true;
        changedHereQueued.notify_one();
    }

    void onChangedHereRead(U64 generation,
                           const QSet<QString>& marked,
                           const SceneChangedHereRowsPtr& rows)
    {
        if (changedHereGeneration != generation) {
            return;
        }
        changedHereBusy = false;
        model->setChangedHere(marked);
        if (rows) {
            setChangedHereRows(rows);
        }
        publishState();
    }

    void runChangedHere()
    {
        // Reused when a read finds the rows it found last time, so that the main thread keeps the
        // filtered tree as it is: an edit elsewhere must not reset its selection and scroll position.
        SceneChangedHereRowsPtr lastRows;

        for (;;) {
            ChangedHereRequest next;
            {
                std::unique_lock<std::mutex> lock(changedHereMutex);
                changedHereQueued.wait(lock, [this]() { return changedHereQuit || hasChangedHereRequest || hasOwedRowsRequest; });
                if (changedHereQuit) {
                    return;
                }
                if (hasOwedRowsRequest) {
                    const OwedRowsRequest owed = std::move(owedRowsRequest);
                    owedRowsRequest = OwedRowsRequest();
                    hasOwedRowsRequest = false;
                    lock.unlock();
                    readOwedRows(owed);
                    continue;
                }
                next = std::move(changedHereRequest);
                changedHereRequest = ChangedHereRequest();
                hasChangedHereRequest = false;
            }
            const U64 generation = next.generation;
            const auto readChangedHereStage = [&](const std::function<void(const SceneStage*)>& read) {
                if (changedHereGeneration != generation) {
                    return;
                }
                next.source->readStage([&](const SceneStage* stage) {
                    if (changedHereGeneration == generation) {
                        read(stage);
                    }
                });
            };
            ScenePayload payload;

            readChangedHereStage([&payload](const SceneStage* stage) {
                if (stage) {
                    payload = stage->getPayload();
                }
            });
            if (changedHereGeneration != generation) {
                continue;
            }
            QSet<QString> marked;

            for (const std::string& authored : SceneOps::getAuthoredPrimPaths(payload, next.node)) {
                QString path = QString::fromStdString(authored);
                while ((path.size() > 1) && !marked.contains(path)) {
                    marked.insert(path);
                    path.truncate(path.lastIndexOf(QLatin1Char('/')));
                }
            }
            SceneChangedHereRowsPtr rows;

            if (next.wantRows) {
                SceneChangedHereRowsBuilder builder(marked);
                while (!builder.isDone() && (changedHereGeneration == generation)) {
                    readChangedHereStage([&builder](const SceneStage* stage) { builder.readBatch(stage); });
                }
                if (changedHereGeneration != generation) {
                    continue;
                }
                rows = builder.take();
                if (lastRows && lastRows->hasSameRows(*rows)) {
                    rows = lastRows;
                } else {
                    lastRows = rows;
                }
            }
            QMetaObject::invokeMethod(panel, [this, generation, marked, rows]() { onChangedHereRead(generation, marked, rows); }, Qt::QueuedConnection);
        }
    }

    void stopChangedHere()
    {
        ++changedHereGeneration;
        ++owedRowsGeneration;
        {
            std::lock_guard<std::mutex> lock(changedHereMutex);
            changedHereQuit = true;
        }
        changedHereQueued.notify_all();
        if (changedHereWorker.joinable()) {
            changedHereWorker.join();
        }
    }

    void startFilter()
    {
        filterStep.stop();
        filterDeferred = false;
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
        if (!source->tryReadStage([&](const SceneStage* sceneStage) {
                filterWalk.step(sceneStage, kFilterStepMs, filterCap, [this](const QString& name) { return filterMatchesName(name); }, &filterMatches);
            })) {
            filterStep.stop();
            filterDeferred = true;

            return false;
        }
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
        treeSelectionOwed = false;
        treeSelectionDeferred = false;
        ++owedRowsGeneration;
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
            const QModelIndex index = model->indexOfPath(path, true, &treeSelectionDeferred);
            if (treeSelectionDeferred) {
                // The rows keep showing the selection they had until the stage can be read.
                treeSelectionOwed = true;
                restoringExpansion = wasRestoring;
                syncingSelection = wasSyncing;

                return;
            }
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

    QAbstractItemView* shownRowsView() const
    {
        if (stack->currentWidget() == results) {
            return results;
        }

        return (stack->currentWidget() == changedHereTree) ? changedHereTree : tree;
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
        if (stack->currentWidget() == changedHereTree) {
            const QModelIndex current = changedHereTree->selectionModel()->currentIndex();

            if (current.isValid()) {
                return current.data(SceneGraphModel::ePrimPathRole).toString();
            }
        }
        if (treeSelectionOwed) {
            return selectionOrder.isEmpty() ? QString() : selectionOrder.back();
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
    _imp->changedHereButton = new QToolButton(this);
    _imp->changedHereButton->setObjectName(QString::fromUtf8("sceneGraphChangedHere"));
    _imp->changedHereButton->setText(tr("Changed Here"));
    _imp->changedHereButton->setCheckable(true);
    _imp->changedHereButton->setToolTip(tr("Lists only the prims the shown node's own layer authors something on, and their ancestors. "
                                           "The rows are marked in bold whether or not this is on."));
    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->setContentsMargins(0, 0, 2, 0);
    filterLayout->setSpacing(4);
    filterLayout->addWidget(_imp->filter, 1);
    filterLayout->addWidget(_imp->changedHereButton);
    _imp->resetDrawStatesButton = new QToolButton(this);
    _imp->resetDrawStatesButton->setObjectName(QString::fromUtf8("sceneGraphResetDrawStates"));
    _imp->resetDrawStatesButton->setText(tr("Reset to Graph"));
    _imp->resetDrawStatesButton->setToolTip(tr("Clears every draw state set in the linked viewer, so that every prim is drawn as the graph says."));
    _imp->resetDrawStatesButton->setEnabled(false);
    filterLayout->addWidget(_imp->resetDrawStatesButton);
    _imp->bakeDrawStatesButton = new QToolButton(this);
    _imp->bakeDrawStatesButton->setObjectName(QString::fromUtf8("sceneGraphBakeDrawStates"));
    _imp->bakeDrawStatesButton->setText(tr("Bake to Node"));
    _imp->bakeDrawStatesButton->setToolTip(tr("Writes every draw state set in the linked viewer into a USDExpandHierarchy below the viewed node, "
                                              "one rule per prim, and clears them from the viewer, which then views that node. The viewed node "
                                              "takes the rules itself when it is a USDExpandHierarchy, and so does one directly below it."));
    _imp->bakeDrawStatesButton->setEnabled(false);
    filterLayout->addWidget(_imp->bakeDrawStatesButton);
    _imp->cancelLoadsButton = new QToolButton(this);
    _imp->cancelLoadsButton->setObjectName(QString::fromUtf8("sceneGraphCancelLoads"));
    _imp->cancelLoadsButton->setText(tr("Cancel Loads"));
    _imp->cancelLoadsButton->setToolTip(tr("Drops the payload loads the linked viewer has queued for prims set to Full or Load and not started yet, "
                                           "as switching the viewer to another node does. A load under way finishes, and what is loaded stays."));
    _imp->cancelLoadsButton->setEnabled(false);
    filterLayout->addWidget(_imp->cancelLoadsButton);
    layout->addLayout(filterLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    _imp->stack = new QStackedWidget(splitter);

    _imp->model = new SceneGraphModel(this);
    _imp->tree = new QTreeView(_imp->stack);
    _imp->tree->setObjectName(QString::fromUtf8("sceneGraphTree"));
    _imp->tree->setModel(_imp->model);
    QObject::connect(_imp->model, &SceneGraphModel::deferredReadsDone, this, [this]() {
        if (_imp->expansionDeferred) {
            _imp->restorePendingExpansion();
            _imp->restoreScroll();
        }
        if (_imp->treeSelectionDeferred && (_imp->stack->currentWidget() == _imp->tree)) {
            _imp->selectTreeRows(_imp->selectionOrder);
        }
    });
    _imp->tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _imp->tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->tree->setUniformRowHeights(true);
    _imp->tree->setItemDelegateForColumn(SceneGraphModel::eColumnName, new PayloadBadgeDelegate(_imp->tree));
    _imp->tree->setItemDelegateForColumn(SceneGraphModel::eColumnDrawState, new DrawStateDelegate(_imp->tree));
    _imp->tree->setDragEnabled(true);
    _imp->tree->setDragDropMode(QAbstractItemView::DragOnly);
    _imp->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _imp->tree->header()->setStretchLastSection(false);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnName, QHeaderView::Stretch);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnType, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnKind, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnDrawState, QHeaderView::ResizeToContents);
    _imp->stack->addWidget(_imp->tree);

    _imp->changedHereModel = new SceneChangedHereModel(this);
    _imp->changedHereTree = new QTreeView(_imp->stack);
    _imp->changedHereTree->setObjectName(QString::fromUtf8("sceneGraphChangedHereTree"));
    _imp->changedHereTree->setModel(_imp->changedHereModel);
    _imp->changedHereTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _imp->changedHereTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->changedHereTree->setUniformRowHeights(true);
    _imp->changedHereTree->setDragEnabled(true);
    _imp->changedHereTree->setDragDropMode(QAbstractItemView::DragOnly);
    _imp->changedHereTree->setContextMenuPolicy(Qt::CustomContextMenu);
    _imp->changedHereTree->header()->setStretchLastSection(false);
    _imp->changedHereTree->header()->setSectionResizeMode(SceneGraphModel::eColumnName, QHeaderView::Stretch);
    // Its rows hold no draw state.
    _imp->changedHereTree->setColumnHidden(SceneGraphModel::eColumnDrawState, true);
    _imp->stack->addWidget(_imp->changedHereTree);

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
    if (gui) {
        _imp->inspector->setNodeOpener([gui](const NodePtr& node) {
            gui->showNodeSettingsPanel(std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()));
        });
    }
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
    // Both run once, on the signals that can call for them, so that nothing polls while idle.
    _imp->pinnedPoll.setSingleShot(true);
    _imp->pinnedPoll.setInterval(0);
    _imp->editSettle.setSingleShot(true);
    _imp->editSettle.setInterval(kEditSettleMs);
    _imp->retiredReap.setInterval(kRetiredReapMs);
    _imp->selectionPoll.setSingleShot(true);
    _imp->selectionPoll.setInterval(kSelectionSettleMs);
    _imp->viewRestore.setSingleShot(true);
    _imp->viewRestore.setInterval(0);

    QObject::connect(_imp->pinButton, &QToolButton::toggled, this, &SceneGraphPanel::onPinToggled);
    QObject::connect(_imp->nodeChooser, qOverload<int>(&QComboBox::activated), this, &SceneGraphPanel::onNodeChosen);
    QObject::connect(_imp->filter, &QLineEdit::textChanged, &_imp->filterDebounce, qOverload<>(&QTimer::start));
    QObject::connect(&_imp->filterDebounce, &QTimer::timeout, this, &SceneGraphPanel::onFilterEdited);
    QObject::connect(&_imp->filterStep, &QTimer::timeout, this, &SceneGraphPanel::onFilterStep);
    QObject::connect(&_imp->inspectorRefresh, &QTimer::timeout, this, &SceneGraphPanel::refreshInspector);
    QObject::connect(_imp->resetDrawStatesButton, &QToolButton::clicked, this, [this]() {
        if (_imp->viewport) {
            _imp->viewport->clearDrawStates();
        }
    });
    QObject::connect(_imp->cancelLoadsButton, &QToolButton::clicked, this, [this]() {
        if (_imp->viewport) {
            _imp->viewport->cancelPendingLoads();
        }
    });
    QObject::connect(_imp->bakeDrawStatesButton, &QToolButton::clicked, this, [this]() {
        if (_imp->viewer) {
            SceneSelectionActions::bakeToNode(_imp->gui, _imp->viewer.data());
        }
    });
    QObject::connect(_imp->changedHereButton, &QToolButton::toggled, this, [this]() {
        _imp->updateChangedHere();
        _imp->updateStatus();
    });
    QObject::connect(_imp->stack, &QStackedWidget::currentChanged, this, [this]() { _imp->onChangedHereStackPageShown(); });
    // Each page has its own current row, and showing a page changes neither of them.
    QObject::connect(_imp->stack, &QStackedWidget::currentChanged, &_imp->inspectorRefresh, qOverload<>(&QTimer::start));
    QObject::connect(&_imp->pinnedPoll, &QTimer::timeout, this, &SceneGraphPanel::onPinnedPoll);
    QObject::connect(&_imp->editSettle, &QTimer::timeout, this, [this]() {
        if (_imp->pinned) {
            _imp->pinnedPoll.start();
        }
    });
    QObject::connect(&_imp->retiredReap, &QTimer::timeout, this, [this]() { _imp->reapRetired(); });
    QObject::connect(&_imp->selectionPoll, &QTimer::timeout, this, &SceneGraphPanel::onSelectionPoll);
    QObject::connect(_imp->followChooser, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        setFollow((index == (int)eFollowSelection) ? eFollowSelection : eFollowViewer);
    });
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onTreeSelectionChanged);
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->results->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onResultsSelectionChanged);
    QObject::connect(_imp->results->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->changedHereTree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onChangedHereSelectionChanged);
    QObject::connect(_imp->changedHereTree->selectionModel(), &QItemSelectionModel::currentChanged, this, &SceneGraphPanel::onCurrentChanged);
    QObject::connect(_imp->results, &QListView::clicked, this, &SceneGraphPanel::onResultActivated);
    // Queued: the model fetches no rows while it is resetting, so nothing could be reopened then.
    QObject::connect(_imp->model, &QAbstractItemModel::modelReset, this, &SceneGraphPanel::onModelReset, Qt::QueuedConnection);
    // Keys are claimed in the views' own event filter so a window-level shortcut cannot take them.
    _imp->tree->installEventFilter(this);
    _imp->results->installEventFilter(this);
    _imp->changedHereTree->installEventFilter(this);
    _imp->results->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(_imp->tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->tree, pos); });
    QObject::connect(_imp->changedHereTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->changedHereTree, pos); });
    QObject::connect(_imp->results, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->results, pos); });
    QObject::connect(_imp->tree, &QTreeView::expanded, this, &SceneGraphPanel::onExpanded);
    QObject::connect(_imp->tree, &QTreeView::collapsed, this, &SceneGraphPanel::onCollapsed);
    QObject::connect(_imp->tree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { _imp->rememberTopPath(); });
    // Rows going, then coming back, as when the viewer shows another node and returns: the rows
    // that come back reopen, and the tree scrolls back to the row it had at its top.
    const auto holdTop = [this]() {
        _imp->holdTopPath = true;
        _imp->restoreAll = true;
        _imp->viewRestore.start();
    };
    QObject::connect(_imp->model, &QAbstractItemModel::modelAboutToBeReset, this, holdTop);
    QObject::connect(_imp->model, &QAbstractItemModel::rowsAboutToBeRemoved, this, holdTop);
    QObject::connect(_imp->model, &QAbstractItemModel::layoutAboutToBeChanged, this, holdTop);
    QObject::connect(_imp->model, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex& parent) {
        if (!_imp->expanded.isEmpty()) {
            if (parent.isValid()) {
                _imp->insertedUnder.insert(_imp->model->pathOf(parent));
            } else {
                _imp->restoreAll = true;
            }
        }
        _imp->viewRestore.start();
    });
    QObject::connect(&_imp->viewRestore, &QTimer::timeout, this, [this]() {
        _imp->restorePendingExpansion();
        _imp->restoreScroll();
    });
    if (gui) {
        QObject::connect(gui, &Gui::activeViewerChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        QObject::connect(gui, &Gui::viewersChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        QObject::connect(gui, &Gui::nodeSelectionChanged, this, [this]() {
            if ((_imp->follow == eFollowSelection) && !_imp->userPinned) {
                _imp->selectionPoll.start();
                _imp->publishState();
            }
        });
        // Edits and deletions that may leave the shown node's stage stale or the node gone.
        QObject::connect(gui, &Gui::nodeGraphTopologyChanged, &_imp->pinnedPoll, qOverload<>(&QTimer::start));
        if (UndoHistory* history = gui->getUndoHistory()) {
            QObject::connect(history, &UndoHistory::changed, &_imp->pinnedPoll, qOverload<>(&QTimer::start));
        }
        GuiAppInstancePtr app = gui->getApp();
        // Knob edits, by the user, a script or a button, that none of the above announce, upstream
        // of a node the viewer may not be showing.
        if (app) {
            QObject::connect(app.get(), &AppInstance::nodeEvaluated, &_imp->editSettle, qOverload<>(&QTimer::start));
        }
        if (app && app->getTimeLine()) {
            QObject::connect(app->getTimeLine().get(), &TimeLine::frameChanged, this, [this]() { _imp->inspectorRefresh.start(); });
        }
    }
    _imp->changedHereWorker = std::thread([this]() { _imp->runChangedHere(); });
    _imp->followViewer();
}

SceneGraphPanel::~SceneGraphPanel()
{
    _imp->inspector->stop();
    _imp->stopChangedHere();
    std::vector<std::shared_ptr<SceneNodeStageSource>> sources;
    sources.swap(_imp->retired);
    for (const Implementation::CachedStage& cached : _imp->cachedStages) {
        sources.push_back(cached.source);
    }
    _imp->cachedStages.clear();
    if (_imp->pinned) {
        sources.push_back(_imp->pinned);
        _imp->pinned.reset();
    }
    // Stopped sources call nothing back, so the panel can go before their workers quit.
    SceneNodeStageSource::reap(sources);
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
    _imp->cachePinned();
    _imp->pinnedError.clear();
    _imp->pinnedPoll.stop();
    if (node && !_imp->takeCached(node)) {
        QPointer<SceneGraphPanel> self(this);
        // The callbacks name their source, so that one kept in the cache is not taken for pinned.
        std::shared_ptr<std::weak_ptr<SceneNodeStageSource>> created = std::make_shared<std::weak_ptr<SceneNodeStageSource>>();
        _imp->pinned = std::make_shared<SceneNodeStageSource>(node, this, [self, created](bool ok, const QString& error) {
            if (self) {
                self->_imp->onSourceSynced(created->lock(), ok, error);
            } }, false);
        _imp->pinned->setLoadsChanged([self, created]() {
            if (self) {
                self->_imp->onSourceLoadsChanged(created->lock());
            }
        });
        *created = _imp->pinned;
        _imp->pinnedMarkCount = 0;
    }
    if (node) {
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
    if (_imp->stack->currentWidget() == _imp->changedHereTree) {
        _imp->selectionOrder = paths;
        _imp->treeSelectionOwed = true;
        _imp->selectChangedHereRows(paths);
    } else {
        _imp->selectTreeRows(paths);
    }
    if (!_imp->filterPattern.isEmpty()) {
        _imp->selectResults(paths);
    }
    _imp->inspectorRefresh.start();
}

void
SceneGraphPanel::revealPaths(const QStringList& paths)
{
    if (!paths.isEmpty()) {
        showSelection(paths);
    }
}

bool
SceneGraphPanel::followsViewer(const ViewerTab* viewer) const
{
    return viewer && !_imp->userPinned && (_imp->follow == eFollowViewer) && (_imp->viewer == viewer);
}

QStringList
SceneGraphPanel::getSelection() const
{
    const QStringList selected = _imp->selectedPaths(_imp->shownRowsView());

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
    bool anyUnloaded = false;
    bool anyOther = false;
    for (const QString& path : selection) {
        // A row of the filter's list may not have been read, so nothing is known of its payload.
        const bool read = _imp->model->indexOfPath(path, false).isValid();
        const SceneGraphModel::PayloadStateEnum state = _imp->model->getPayloadState(path);
        const bool unloaded = (state == SceneGraphModel::ePayloadStateUnloaded) || (state == SceneGraphModel::ePayloadStateFailed);
        anyUnloaded = anyUnloaded || unloaded || !read;
        anyOther = anyOther || !unloaded;
    }
    const bool onViewerStage = _imp->loadsOnShownStage();
    QAction* load = menu.addAction(tr("Load"));
    load->setObjectName(QString::fromUtf8("sceneGraphLoad"));
    load->setEnabled(onViewerStage && anyUnloaded);
    QObject::connect(load, &QAction::triggered, this, [this, selection]() { _imp->requestLoad(selection, false); });
    QAction* loadWithDescendants = menu.addAction(tr("Load With Descendants"));
    loadWithDescendants->setObjectName(QString::fromUtf8("sceneGraphLoadWithDescendants"));
    loadWithDescendants->setEnabled(onViewerStage && !selection.isEmpty());
    QObject::connect(loadWithDescendants, &QAction::triggered, this, [this, selection]() { _imp->requestLoad(selection, true); });
    QAction* unload = menu.addAction(tr("Unload"));
    unload->setObjectName(QString::fromUtf8("sceneGraphUnload"));
    unload->setEnabled(onViewerStage && anyOther);
    QObject::connect(unload, &QAction::triggered, this, [this, selection]() { _imp->requestUnload(selection); });
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
    menu.addSeparator();
    // The linked viewer's, whatever node the panel shows.
    const QPointer<SceneViewport> drawViewer = _imp->viewport.data();
    QMenu* drawMenu = menu.addMenu(tr("Draw State"));
    drawMenu->setObjectName(QString::fromUtf8("sceneGraphDrawStateMenu"));
    drawMenu->setEnabled(!drawViewer.isNull() && !selection.isEmpty());
    const QVariant shownState = (selection.size() == 1) ? _imp->model->indexOfPath(selection.front(), false).data(SceneGraphModel::eDrawStateRole) : QVariant();
    for (int state = (int)eSceneDrawStateBBox; state <= (int)eSceneDrawStateHidden; ++state) {
        const QString name = QString::fromUtf8(sceneDrawStateName((SceneDrawStateEnum)state));
        QAction* action = drawMenu->addAction(SceneGraphModel::drawStateLabel(state));
        action->setObjectName(QString::fromUtf8("sceneGraphDrawState_") + name);
        action->setCheckable(true);
        action->setChecked(shownState.isValid() && (shownState.toInt() == state));
        QObject::connect(action, &QAction::triggered, this, [drawViewer, selection, name]() {
            if (drawViewer) {
                drawViewer->setDrawStateOnRows(selection, name);
            }
        });
    }
    drawMenu->addSeparator();
    QAction* clearBelow = drawMenu->addAction(tr("Clear Below"));
    clearBelow->setObjectName(QString::fromUtf8("sceneGraphDrawStateClearBelow"));
    clearBelow->setToolTip(tr("Clears the states of the rows below; the row keeps its own."));
    QObject::connect(clearBelow, &QAction::triggered, this, [drawViewer, selection]() {
        if (drawViewer) {
            drawViewer->clearDrawStatesBelowRows(selection);
        }
    });
    QAction* resetRow = drawMenu->addAction(tr("Reset to Graph"));
    resetRow->setObjectName(QString::fromUtf8("sceneGraphDrawStateReset"));
    resetRow->setToolTip(tr("Clears the state of the row and of every row below it."));
    QObject::connect(resetRow, &QAction::triggered, this, [drawViewer, selection]() {
        if (drawViewer) {
            drawViewer->resetDrawStatesToGraphOnRows(selection);
        }
    });
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
SceneGraphPanel::onStageLoadsChanged()
{
    _imp->updateBakeButton();
    if (!_imp->showsViewerStage()) {
        return;
    }
    _imp->model->settleLoads();
    if (!_imp->filterPattern.isEmpty()) {
        _imp->filterDebounce.start();
    }
    _imp->updateStatus();
    _imp->inspectorRefresh.start();
}

void
SceneGraphPanel::onSceneSynced()
{
    // The viewer evaluates after an edit, which may leave the panel's own stage stale too.
    if (_imp->pinned) {
        _imp->pinnedPoll.start();
    }
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
    // Rows removed by an edit of the stage leave the selection: a selected prim that is gone is
    // kept, as selectTreeRows() keeps one it cannot show, so that its return restores it.
    if (_imp->syncingSelection || _imp->model->isChanging()) {
        return;
    }
    _imp->treeSelectionOwed = false;
    _imp->treeSelectionDeferred = false;
    ++_imp->owedRowsGeneration;
    _imp->selectionOrder = SceneSelectionActions::keepSelectionOrder(_imp->selectionOrder, _imp->selectedPaths(_imp->tree));
    if (!_imp->sharesViewerSelection()) {
        return;
    }
    _imp->syncingSelection = true;
    _imp->viewport->setSelection(_imp->selectionOrder);
    _imp->syncingSelection = false;
}

void
SceneGraphPanel::onChangedHereSelectionChanged()
{
    if (_imp->syncingSelection) {
        return;
    }
    _imp->treeSelectionOwed = true;
    if (!_imp->sharesViewerSelection()) {
        _imp->selectionOrder = SceneSelectionActions::keepSelectionOrder(_imp->selectionOrder, _imp->selectedPaths(_imp->changedHereTree));

        return;
    }
    // Selected prims the filter hides stay selected.
    const QStringList viewerSelection = _imp->viewport->getSelection();
    QStringList paths;

    for (const QString& path : viewerSelection) {
        if (!_imp->changedHereModel->contains(path)) {
            paths << path;
        }
    }
    paths << _imp->selectedPaths(_imp->changedHereTree);
    _imp->selectionOrder = SceneSelectionActions::keepSelectionOrder(viewerSelection, paths);
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
    _imp->filterDeferred = false;
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
    _imp->dropDeletedCachedStages();
    if (!_imp->pinned) {
        _imp->pinnedPoll.stop();
        _imp->publishState();

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
    _imp->publishState();
}

void
SceneGraphPanel::onPinnedSynced(bool ok,
                                const QString& error)
{
    if (_imp->pinned && (_imp->model->getSource() != _imp->pinned) && (_imp->wantedSource() == _imp->pinned)) {
        // The first evaluation of the node switched to: its rows replace the greyed ones.
        _imp->applySource();
        _imp->pinnedPoll.start();
    }
    if (!_imp->pinned || (_imp->model->getSource() != _imp->pinned)) {
        _imp->publishState();

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
    _imp->restoreAll = true;
    _imp->restorePendingExpansion();
    _imp->restoreScroll();
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
    const QString path = _imp->model->pathOf(index);
    _imp->expanded.insert(path);
    const SceneGraphModel::PayloadStateEnum state = _imp->model->getPayloadState(path);
    if (((state == SceneGraphModel::ePayloadStateUnloaded) || (state == SceneGraphModel::ePayloadStateFailed)) && _imp->loadsOnShownStage()) {
        _imp->requestLoad(QStringList(path), false);
    }
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
        _imp->publishState();

        return;
    }
    // Selecting no scene node keeps the scene shown: only choosing another one switches.
    NodePtr node = _imp->selectedSceneNode();
    if (!node || (node == _imp->selectedNode.lock())) {
        _imp->publishState();

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
    _imp->updateChangedHere();
    _imp->inspector->setNode(_imp->modelNode());
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
    if (((watched == _imp->tree) || (watched == _imp->results) || (watched == _imp->changedHereTree)) && ((e->type() == QEvent::ShortcutOverride) || (e->type() == QEvent::KeyPress))) {
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
