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
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
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
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneKindWalk.h"
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
#include "Gui/GuiMacros.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneGraphUnfolder.h"
#include "Gui/SceneNodeStageSource.h"
#include "Gui/ScenePrimInspector.h"
#include "Gui/ScenePurposeDelegate.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/ThemedIcon.h"
#include "Gui/UndoHistory.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {

// How many prims a filter lists before it offers more.
const int kFilterCap = 1000;

// How long one step of the filter walk may hold the stage, in milliseconds.
const int kFilterStepMs = 8;

// Whether the filter's text is a path expression rather than a name: an absolute path, a
// predicate or a collection reference, none of which a prim name holds.
bool
isExpressionFilter(const QString& text)
{
    return text.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char('%')) || text.contains(QLatin1Char('{'));
}

// How many paths of a viewer selection are shown in the tree.
const int kShownSelectionCap = 2000;

bool
putsOutScene(const NodePtr& node)
{
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    return effect && node->isActivated() && effect->producesSceneData();
}

// How often sources let go of are checked for their workers having quit, in milliseconds.
const int kRetiredReapMs = 250;

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

    NodeSourceBar* sourceBar = nullptr;
    QLabel* status = nullptr;
    QString armedPick;
    QLineEdit* filter = nullptr;
    QToolButton* applyButton = nullptr;
    // Beside the field: why the expression is invalid, or the unloaded branches not searched.
    QLabel* filterMessage = nullptr;
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

    // Set while the panel and the viewer hand a selection to each other.
    bool syncingSelection = false;
    // The selected paths in the order they were selected, the viewer's when the panel shows its
    // selection; see SceneSelectionActions::keepSelectionOrder().
    QStringList selectionOrder;

    // The rows the user expanded, kept across resets, one set per shown node: a stage's reset
    // reopens only its own rows. The sets go with the stages the panel keeps, the shown one and
    // those of cachedStages.
    SceneGraphExpansionMemory expansionMemory { kStageCacheSize + 1 };
    // The shown node's set.
    SceneGraphExpandedPaths* expanded = expansionMemory.select(std::shared_ptr<const void>());
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
    QString filterError;
    // Set while the walk an edit of the expression started is under way: its matches become the
    // selection as they are found. A walk restarted by an edit of the stage leaves the selection be.
    bool filterSelects = false;

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
    // The scope the toolbar's commands act in: "selected", "below" or a kind, per panel, not saved.
    QComboBox* bulkScope = nullptr;
    std::vector<std::pair<QString, QToolButton*>> bulkButtons;
    QLabel* bulkStatus = nullptr;
    QTimer bulkRefresh;
    // Opens and closes branches of the tree a slice at a time; its rows never load a payload.
    std::unique_ptr<SceneGraphUnfolder> unfolder;
    // The node shown when the unfolder last ran or the panel last looked: a run, the status line
    // it wrote and the rows it owes reopening all belong to that node's stage.
    NodeWPtr expansionNode;
    const Node* expansionKey = nullptr;
    // The model's reset count when leaving a node last stopped a run or cleared its status.
    int expansionLeftAtReset = -1;
    bool bulkStatusFromUnfolder = false;
    QToolButton* unfoldButton = nullptr;
    QToolButton* foldButton = nullptr;
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
        return sourceBar->isPinned() && pinned;
    }

    // Whether the rows are the viewer's prims, so selections go both ways.
    bool sharesViewerSelection() const
    {
        if (!viewport) {
            return false;
        }
        if (!pinned) {
            return sourceBar->getFollow() == NodeSourceBar::eFollowViewer;
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
            const NodePtr shown = panel->getShownNode();
            if (shown && viewer && viewport && viewport->isGeometryContext() && (shown == viewer->getSceneViewedNode())) {
                // The rows are the prims the viewer made of the geometry.
                viewport->createFromPrims(paths);
            } else if (shown && (shown->getEffectiveOutputDataKind() == eDataKindGeometry)) {
                SceneSelectionActions::createFromComponents(gui, shown, GeoDetailConstPtr(), GeoComponentSelection());
            } else {
                SceneSelectionActions::createFromSelection(gui, shown, paths);
            }
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

        panel->setProperty("natronSceneArmedPick", armedPick);
        panel->setProperty("natronSceneGraphPinned", isPinned());
        panel->setProperty("natronSceneGraphFollow", NodeSourceBar::followName(sourceBar->getFollow()));
        panel->setProperty("natronSceneGraphNode", node ? QString::fromStdString(node->getFullyQualifiedName()) : QString());
        panel->setProperty("natronSceneGraphHasScene", hasScene());
        panel->setProperty("natronSceneGraphChangedHereOnly", changedHereOnly());
        panel->setProperty("natronSceneGraphChangedHereBusy", changedHereBusy);
        panel->setProperty("natronSceneGraphChangedHereCount", model->getChangedHereCount());
        panel->setProperty("natronSceneGraphChangedHereRows", changedHereModel->getRowCount());
        panel->setProperty("natronSceneGraphFilterBusy", filterStep.isActive() || filterDebounce.isActive() || filterDeferred);
        panel->setProperty("natronSceneGraphFilterExpression", filterWalk.hasExpression());
        panel->setProperty("natronSceneGraphFilterError", filterError);
        panel->setProperty("natronSceneGraphFilterUnloaded", filterPattern.isEmpty() ? 0 : filterWalk.getUnloadedBranchCount());
        const SceneGraphStageSourcePtr& source = model->getSource();
        panel->setProperty("natronSceneGraphStageEditCount", QVariant::fromValue<qulonglong>(source ? source->getEditCount() : 0));
        // Twirling a row must not add to it: the rows' open state never reaches the stage.
        panel->setProperty("natronSceneGraphStageNotices", QVariant::fromValue<qulonglong>(model->getStageNoticeCount()));
        panel->setProperty("natronSceneGraphComposeCount", QVariant::fromValue<qulonglong>(composeCount));
        panel->setProperty("natronSceneGraphSwitching", switching);
        panel->setProperty("natronSceneGraphCachedStages", (int)cachedStages.size());
        panel->setProperty("natronSceneGraphTimersActive", pinnedPoll.isActive() || editSettle.isActive() || sourceBar->isSettling() || retiredReap.isActive());
        panel->setProperty("natronSceneGraphTopPath", topPath);
        panel->setProperty("natronSceneGraphUnfolding", unfolder && unfolder->isRunning());
        panel->setProperty("natronSceneGraphUnfoldSlices", unfolder ? unfolder->getProgress().slices : 0);
        panel->setProperty("natronSceneGraphUnfoldedCount", (int)expanded->size());
        panel->setProperty("natronSceneGraphRememberedStages", (int)expansionMemory.size());
        panel->setProperty("natronSceneGraphRestoring", unfolder && unfolder->isRestoring());
        panel->setProperty("natronSceneGraphRestoreSlices", unfolder ? unfolder->getRestoreSliceCount() : 0);
        panel->setProperty("natronSceneGraphResetCount", model->getResetCount());
        panel->setProperty("natronSceneGraphExpansionLeftAtReset", expansionLeftAtReset);
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
        } else if (sourceBar->getFollow() == NodeSourceBar::eFollowSelection) {
            text = node ? SceneGraphPanel::tr("Selected: %1").arg(QString::fromStdString(node->getFullyQualifiedName()))
                        : SceneGraphPanel::tr("Select a node that puts out a scene");
        } else if (viewer) {
            text = node ? SceneGraphPanel::tr("%1 shows %2").arg(QString::fromStdString(viewer->getInternalNode() ? viewer->getInternalNode()->getScriptName_mt_safe() : std::string()), QString::fromStdString(node->getFullyQualifiedName()))
                        : SceneGraphPanel::tr("%1 shows no scene").arg(QString::fromStdString(viewer->getInternalNode() ? viewer->getInternalNode()->getScriptName_mt_safe() : std::string()));
        } else {
            text = SceneGraphPanel::tr("No viewer");
        }
        if (!armedPick.isEmpty()) {
            text = SceneGraphPanel::tr("Pick for %1: select prims, Enter writes them, Esc cancels").arg(armedPick);
        }
        bulkRefresh.start();
        status->setText(text);
        status->setStyleSheet(armedPick.isEmpty() ? QString() : QString::fromUtf8("QLabel { color: #e6a050; font-weight: bold; }"));
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

        return (sourceBar->getFollow() == NodeSourceBar::eFollowViewer) ? viewerSource : SceneGraphStageSourcePtr();
    }

    // Another shown node means another stage, even through the same source: a run walking the
    // stage left would keep reading it, and the model only takes the new stage in once no read is
    // in flight, so the run stops here rather than at the model's reset. The remembered rows are
    // the new node's from here on. What was owed reopening was owed from the set left, which the
    // unfolder drops with it; the new node's whole set is owed instead, so that its rows reopen
    // whether the new stage comes in by a reset or by an update, and the reopening's end scrolls
    // back and lets go of holdTopPath.
    void leaveExpansionNode()
    {
        const NodePtr shown = panel->getShownNode();
        if ((shown.get() == expansionKey) && (!expansionKey || !expansionNode.expired())) {
            return;
        }
        expansionNode = shown;
        expansionKey = shown.get();
        expanded = expansionMemory.select(shown);
        if (!unfolder) {
            return;
        }
        const bool stopped = unfolder->isRunning();
        unfolder->setExpandedPaths(expanded);
        insertedUnder.clear();
        restoreAll = true;
        expansionDeferred = false;
        viewRestore.start();
        const bool cleared = bulkStatusFromUnfolder;
        if (cleared) {
            // It counts rows of the stage left.
            bulkStatus->clear();
            bulkStatus->setVisible(false);
            bulkStatusFromUnfolder = false;
        }
        if (stopped || cleared) {
            expansionLeftAtReset = model->getResetCount();
        }
        publishState();
    }

    void applySource()
    {
        leaveExpansionNode();
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
        // The reopening waited for the rows, which may be the ones on show already.
        if (!on && (holdTopPath || restoreAll || !insertedUnder.isEmpty())) {
            viewRestore.start();
        }
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
            // The viewer may show the node whose stage goes; its rows stay remembered.
            const NodePtr evicted = cachedStages.front().source->getNode();
            if (evicted != panel->getShownNode()) {
                expansionMemory.forget(evicted);
            }
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
                if (node) {
                    expansionMemory.forget(node);
                }
                retireSource(it->source);
                it = cachedStages.erase(it);
            } else {
                ++it;
            }
        }
        expansionMemory.forgetGone();
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
        updateBakeButton();
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
            // The viewport stays across a change of viewed node, which can move the wanted source
            // between the viewer's stage and the shown node's own.
            if (viewerSource && (model->getSource() == viewerSource) && (wantedSource() == viewerSource)) {
                leaveExpansionNode();
                model->refresh();
            } else {
                applySource();
            }

            return;
        }
        if (viewport) {
            // Unlinked, its walk would go on unseen and out of Esc's reach.
            viewport->cancelScopedDraw();
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
            QObject::connect(viewport, &SceneViewport::scopedDrawProgress, panel, [this](const QString& text, bool running) {
                showScopedDrawProgress(text, running);
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
            const bool pinnedShown = pinned && (model->getSource() == pinned);
            // A batch under way counts: cancelling it drops the levels a load to a kind asks for after it.
            cancelLoadsButton->setEnabled(pinnedShown ? pinned->hasPendingLoads() : (viewport && viewport->getRenderer().hasOutstandingStageLoads()));
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
                QObject::connect(viewer, &ViewerTab::sceneViewedNodeChanged, panel, [this]() {
                    updateBakeButton();
                    applySource();
                    updateStatus();
                });
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
            updateBakeButton();
        } else {
            return;
        }
        model->markLoading(paths);
    }

    void requestUnload(const QStringList& paths)
    {
        for (const QString& path : paths) {
            const std::pair<SceneGraphExpandedPaths::iterator, SceneGraphExpandedPaths::iterator> below = expandedPathsBelow(*expanded, path);
            const QStringList open(below.first, below.second);
            for (const QString& row : open) {
                closeRow(row);
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

    /**
     * @brief Closes, as requestUnload() does, the remembered open rows an Unload Below kind at roots
     * unloads, and every row below them: the payload rows below a row the walk to kind matches,
     * which the rows' Kind column tells by walkToKind()'s stop rule, a root of kind walked through.
     * Rows not read are below an unloaded payload, which this unload leaves alone.
     **/
    void closeRowsUnloadedBelowKind(const QStringList& roots,
                                    const QString& kind)
    {
        const PXR_NS::TfToken target(kind.toStdString());
        // 0 the walk goes on below the row, 1 it stops there, 2 it stops there on a match.
        QHash<QString, int> stops;
        const auto stopAt = [&](const QString& path) {
            QHash<QString, int>::const_iterator found = stops.constFind(path);
            if (found != stops.constEnd()) {
                return found.value();
            }
            const QModelIndex index = model->indexOfPath(path, false);
            const QString rowKind = index.isValid() ? index.siblingAtColumn(SceneGraphModel::eColumnKind).data(Qt::DisplayRole).toString() : QString();
            bool matches = false;
            const int stop = kindWalkStopsAt(PXR_NS::TfToken(rowKind.toStdString()), target, &matches) ? (matches ? 2 : 1) : 0;
            stops.insert(path, stop);

            return stop;
        };
        QStringList unloaded;
        for (const QString& root : roots) {
            const std::pair<SceneGraphExpandedPaths::iterator, SceneGraphExpandedPaths::iterator> below = expandedPathsBelow(*expanded, root);
            for (SceneGraphExpandedPaths::iterator it = below.first; it != below.second; ++it) {
                const QString& path = *it;
                if ((!unloaded.isEmpty() && path.startsWith(unloaded.back() + QLatin1Char('/'))) || (model->getPayloadState(path) == SceneGraphModel::ePayloadStateNone)) {
                    continue;
                }
                QString ancestor = root;
                int from = (root == QString::fromUtf8("/")) ? 1 : (int)root.size() + 1;
                for (;;) {
                    int stop = stopAt(ancestor);
                    if ((stop == 2) && (ancestor == root)) {
                        stop = 0;
                    }
                    if (stop != 0) {
                        if (stop == 2) {
                            unloaded.push_back(path);
                        }
                        break;
                    }
                    const int slash = path.indexOf(QLatin1Char('/'), from);
                    if (slash < 0) {
                        break;
                    }
                    ancestor = path.left(slash);
                    from = slash + 1;
                }
            }
        }
        for (const QString& path : unloaded) {
            const std::pair<SceneGraphExpandedPaths::iterator, SceneGraphExpandedPaths::iterator> below = expandedPathsBelow(*expanded, path);
            const QStringList open(below.first, below.second);
            for (const QString& row : open) {
                closeRow(row);
            }
            closeRow(path);
        }
    }

    static bool isBulkDrawAction(const QString& action)
    {
        SceneDrawStateEnum state;

        return sceneDrawStateFromName(action.toStdString(), &state);
    }

    // "Component", or a custom kind as it is registered.
    static QString kindLabel(const QString& kind)
    {
        if (!isBuiltInKindScope(PXR_NS::TfToken(kind.toStdString())) || kind.isEmpty()) {
            return kind;
        }

        return kind.left(1).toUpper() + kind.mid(1);
    }

    // What action does in scope, for its button's tooltip.
    static QString bulkToolTip(const QString& action,
                               const QString& scope)
    {
        const bool below = (scope == QString::fromUtf8("below"));
        const bool selected = (scope == QString::fromUtf8("selected"));
        const QString kind = SceneGraphModel::kindPluralText(scope);

        if (action == QString::fromUtf8("load")) {
            if (selected) {
                return SceneGraphPanel::tr("Load: loads the payloads of the selected rows, without what they hold.");
            }
            if (below) {
                return SceneGraphPanel::tr("Load Below: loads the selected rows and every payload below them.");
            }

            return SceneGraphPanel::tr("Load To %1: loads level by level from the selected rows down to the %2 below them, "
                                       "and the %2 themselves; payloads below the %2 stay unloaded.")
                .arg(kindLabel(scope), kind);
        }
        if (action == QString::fromUtf8("unload")) {
            if (selected) {
                return SceneGraphPanel::tr("Unload: unloads the selected rows and everything below them.");
            }
            if (below) {
                return SceneGraphPanel::tr("Unload already unloads everything below the rows; pick another scope.");
            }

            return SceneGraphPanel::tr("Unload Below %1: keeps the %2 below the selected rows loaded and unloads the payloads below them.").arg(kindLabel(scope), kind);
        }
        if (action == QString::fromUtf8("clearBelow")) {
            if (selected) {
                return SceneGraphPanel::tr("Clear Below: clears the draw states set below the selected rows; each row keeps its own.");
            }
            if (below) {
                return SceneGraphPanel::tr("Every other command in Below scope clears the states below already; pick another scope.");
            }

            return SceneGraphPanel::tr("Clear Below Each %1: clears the draw states set below the loaded %2 below the selected rows.").arg(kindLabel(scope), kind);
        }
        SceneDrawStateEnum state = eSceneDrawStateFull;
        sceneDrawStateFromName(action.toStdString(), &state);
        const QString label = SceneGraphModel::drawStateLabel((int)state);
        if (selected) {
            return SceneGraphPanel::tr("%1: sets %1 on the selected rows in the linked viewer.").arg(label);
        }
        if (below) {
            return SceneGraphPanel::tr("%1 Below: sets %1 on the selected rows in the linked viewer and clears the states set below them, as one change.").arg(label);
        }

        return SceneGraphPanel::tr("%1 To %2: sets %1 on the loaded %3 below the selected rows in the linked viewer; "
                                   "nothing inside an unloaded payload changes.")
            .arg(label, kindLabel(scope), kind);
    }

    QString bulkScopeName() const
    {
        return bulkScope ? bulkScope->currentData().toString() : QString::fromUtf8("selected");
    }

    // Whether any of paths is unloaded or not known yet, and whether any is not unloaded.
    void payloadStatesOf(const QStringList& paths,
                         bool* anyUnloaded,
                         bool* anyOther) const
    {
        *anyUnloaded = false;
        *anyOther = false;
        for (const QString& path : paths) {
            // A row of the filter's list may not have been read, so nothing is known of its payload.
            const bool read = model->indexOfPath(path, false).isValid();
            const SceneGraphModel::PayloadStateEnum state = model->getPayloadState(path);
            const bool unloaded = (state == SceneGraphModel::ePayloadStateUnloaded) || (state == SceneGraphModel::ePayloadStateFailed);
            *anyUnloaded = *anyUnloaded || unloaded || !read;
            *anyOther = *anyOther || !unloaded;
        }
    }

    bool bulkActionEnabled(const QString& action,
                           const QString& scope,
                           const QStringList& paths) const
    {
        const bool below = (scope == QString::fromUtf8("below"));
        const bool selected = (scope == QString::fromUtf8("selected"));

        if (paths.isEmpty()) {
            return false;
        }
        if ((action == QString::fromUtf8("load")) || (action == QString::fromUtf8("unload"))) {
            if (!loadsOnShownStage() || ((action == QString::fromUtf8("unload")) && below)) {
                return false;
            }
            if (!selected) {
                return true;
            }
            bool anyUnloaded = false;
            bool anyOther = false;
            payloadStatesOf(paths, &anyUnloaded, &anyOther);

            return (action == QString::fromUtf8("load")) ? anyUnloaded : anyOther;
        }
        if (action == QString::fromUtf8("clearBelow")) {
            return viewport && !below;
        }

        return bool(viewport);
    }

    void updateBulkButtons()
    {
        if (!bulkScope) {
            return;
        }
        const QString scope = bulkScopeName();
        const QStringList selection = panel->getSelection();

        for (const std::pair<QString, QToolButton*>& entry : bulkButtons) {
            QToolButton* button = entry.second;
            button->setEnabled(bulkActionEnabled(entry.first, scope, selection));
            button->setToolTip(bulkToolTip(entry.first, scope));
            if (!button->isCheckable()) {
                continue;
            }
            SceneDrawStateEnum state = eSceneDrawStateFull;
            sceneDrawStateFromName(entry.first.toStdString(), &state);
            bool all = viewport && !selection.isEmpty();
            for (const QString& path : selection) {
                if (!all) {
                    break;
                }
                const QModelIndex index = model->indexOfPath(path, false);
                const QVariant shown = index.data(SceneGraphModel::eDrawStateRole);
                all = shown.isValid() && (shown.toInt() == (int)state)
                    && (index.data(SceneGraphModel::eDrawStateOriginRole).toInt() == (int)eSceneDrawOriginViewer);
            }
            button->setChecked(all);
        }
        for (QToolButton* button : { unfoldButton, foldButton }) {
            if (button) {
                // They act on the tree's rows, not on the filter's results or Changed Here's.
                button->setEnabled(!selection.isEmpty() && (shownRowsView() == tree));
                button->setToolTip(unfoldToolTip(button == foldButton, scope));
            }
        }
    }

    // Runs action in scope on paths, on the stage the rows are read from for loads and in the
    // linked viewer for draw states, and shows what it did. Returns that summary, empty when the
    // command does not apply.
    QString runBulk(const QString& action,
                    const QStringList& paths,
                    const QString& scope)
    {
        QString summary;

        if (viewport && viewport->isScopedDrawRunning()) {
            viewport->cancelScopedDraw();
        }
        if (bulkActionEnabled(action, scope, paths)) {
            const bool kind = (scope != QString::fromUtf8("selected")) && (scope != QString::fromUtf8("below"));
            if (action == QString::fromUtf8("load")) {
                if (!kind) {
                    requestLoad(paths, scope == QString::fromUtf8("below"));
                    summary = SceneGraphModel::loadingText(paths.size(), scope);
                } else if (showsViewerStage()) {
                    summary = viewport->loadRows(paths, scope);
                    updateBakeButton();
                } else if (pinned && (model->getSource() == pinned)) {
                    pinned->requestLoadToKind(paths, scope.toStdString());
                    updateBakeButton();
                    summary = SceneGraphModel::loadingText(paths.size(), scope);
                }
            } else if (action == QString::fromUtf8("unload")) {
                if (!kind) {
                    requestUnload(paths);
                    summary = SceneGraphModel::unloadingText(paths.size(), scope);
                } else if (showsViewerStage()) {
                    bool queued = false;
                    summary = viewport->unloadRows(paths, scope, &queued);
                    if (queued) {
                        closeRowsUnloadedBelowKind(paths, scope);
                    }
                } else if (pinned && (model->getSource() == pinned)) {
                    closeRowsUnloadedBelowKind(paths, scope);
                    pinned->requestUnloadBelowKind(paths, scope.toStdString());
                    updateBakeButton();
                    summary = SceneGraphModel::unloadingText(paths.size(), scope);
                }
            } else if (viewport) {
                summary = viewport->setDrawStateOnRowsScoped(paths, action, scope);
            }
        }
        bulkStatus->setText(summary);
        bulkStatus->setVisible(!summary.isEmpty());
        bulkStatusFromUnfolder = false;
        bulkRefresh.start();

        return summary;
    }

    // The command of bindings a key is bound to in the Shortcut Editor, null when none is.
    static const SceneGraphBulkKeyBinding* commandForKey(const std::vector<SceneGraphBulkKeyBinding>& bindings,
                                                         Qt::KeyboardModifiers modifiers,
                                                         int key)
    {
        GuiApplicationManager* manager = dynamic_cast<GuiApplicationManager*>(AppManager::instance());

        if (!manager || (key == 0) || (key == Qt::Key_unknown)) {
            return nullptr;
        }
        for (const SceneGraphBulkKeyBinding& binding : bindings) {
            if (manager->matchesKeybind(kShortcutGroupViewer3D, binding.actionID, modifiers, key)) {
                return &binding;
            }
        }

        return nullptr;
    }

    static const SceneGraphBulkKeyBinding* bulkCommandForKey(Qt::KeyboardModifiers modifiers,
                                                             int key)
    {
        return commandForKey(getSceneGraphBulkKeyBindings(), modifiers, key);
    }

    static const SceneGraphBulkKeyBinding* expandCommandForKey(Qt::KeyboardModifiers modifiers,
                                                               int key)
    {
        return commandForKey(getSceneGraphFoldKeyBindings(), modifiers, key);
    }

    // Unfolds or folds the tree's rows at paths in scope: Selected and Below unfold all below,
    // a kind unfolds to it; folding is always all below.
    void runUnfold(bool fold,
                   const QStringList& paths,
                   const QString& scope)
    {
        if (paths.isEmpty()) {
            return;
        }
        leaveExpansionNode();
        if (viewport && viewport->isScopedDrawRunning()) {
            viewport->cancelScopedDraw();
        }
        if (fold) {
            unfolder->start(SceneGraphUnfolder::eModeFoldBelow, paths);
        } else if ((scope == QString::fromUtf8("selected")) || (scope == QString::fromUtf8("below"))) {
            unfolder->start(SceneGraphUnfolder::eModeUnfoldBelow, paths);
        } else {
            unfolder->start(SceneGraphUnfolder::eModeUnfoldToKind, paths, scope);
        }
    }

    void showScopedDrawProgress(const QString& text,
                                bool running)
    {
        bulkStatus->setText(text);
        bulkStatus->setVisible(!text.isEmpty());
        bulkStatusFromUnfolder = false;
        if (!running) {
            bulkRefresh.start();
        }
        publishState();
    }

    // Whether Esc has a long command to stop: an expansion, or a kind-scoped draw state edit's walk.
    bool hasStoppable() const
    {
        return (unfolder && unfolder->isRunning()) || (viewport && viewport->isScopedDrawRunning());
    }

    void stopStoppable()
    {
        if (unfolder && unfolder->isRunning()) {
            unfolder->cancel();
        }
        if (viewport && viewport->isScopedDrawRunning()) {
            viewport->cancelScopedDraw();
        }
    }

    void showUnfoldProgress(const SceneGraphUnfolder::Progress& progress)
    {
        bulkStatus->setText(SceneGraphUnfolder::progressText(progress));
        bulkStatus->setVisible(true);
        bulkStatusFromUnfolder = true;
        if (!progress.running && (holdTopPath || restoreAll || !insertedUnder.isEmpty())) {
            viewRestore.start();
        }
        publishState();
    }

    static QString unfoldToolTip(bool fold,
                                 const QString& scope)
    {
        if (fold) {
            return SceneGraphPanel::tr("Fold All Below: closes the selected rows and every row below them, whatever the scope. Esc stops it.");
        }
        if ((scope == QString::fromUtf8("selected")) || (scope == QString::fromUtf8("below"))) {
            return SceneGraphPanel::tr("Unfold All Below: opens every row below the selected rows. Rows of unloaded payloads stay closed "
                                       "and unloaded. Esc stops it.");
        }

        return SceneGraphPanel::tr("Unfold To %1: opens the rows from the selected rows down to the %2 below them and leaves the %2 "
                                   "closed. Rows of unloaded payloads stay closed and unloaded. Esc stops it.")
            .arg(kindLabel(scope), SceneGraphModel::kindPluralText(scope));
    }

    // The row whose branch arrow is at pos in the tree's viewport, invalid when pos is on none.
    QModelIndex branchArrowAt(const QPoint& pos) const
    {
        const QModelIndex at = tree->indexAt(pos);

        if (!at.isValid()) {
            return QModelIndex();
        }
        const QModelIndex index = at.siblingAtColumn(0);
        if (!model->hasChildren(index) || (!index.parent().isValid() && !tree->rootIsDecorated())) {
            return QModelIndex();
        }
        // The name column's rect starts after the row's indentation, whose last step holds the arrow.
        const QRect rect = tree->visualRect(index);
        if ((pos.y() < rect.top()) || (pos.y() > rect.bottom())) {
            return QModelIndex();
        }

        return ((pos.x() >= rect.left() - tree->indentation()) && (pos.x() < rect.left())) ? index : QModelIndex();
    }

    void closeRow(const QString& path)
    {
        const QModelIndex index = model->indexOfPath(path, false);

        if (index.isValid()) {
            tree->collapse(index);
        }
        expanded->erase(path);
    }

    // Hands what is owed reopening to the unfolder, which reopens it a slice at a time and then
    // calls onRestored().
    void restoreView()
    {
        // The rows stay the stage left's until the new one is read, whose reset restores; a run's
        // end restarts viewRestore.
        if (!unfolder || switching || unfolder->isRunning()) {
            return;
        }
        const bool all = restoreAll;
        const QStringList under = insertedUnder.values();
        restoreAll = false;
        insertedUnder.clear();
        expansionDeferred = false;
        if (model->rowCount(QModelIndex()) == 0) {
            // Before a stage's top rows are read, every remembered path would ask the worker for
            // them, holding back the stage's rows and its edits. Their insertion restores.
            restoreScroll();

            return;
        }
        unfolder->restore(all, under);
    }

    // The unfolder's reopening ended; one it could not finish for reads the model owed is redone
    // once they are done.
    void onRestored(bool deferred)
    {
        expansionDeferred = deferred;
        restoreScroll();
        publishState();
    }

    void rememberTopPath()
    {
        if (holdTopPath || restoringExpansion || (unfolder && unfolder->isRestoring())) {
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
        filterError.clear();
        QString error;
        const bool parsed = filterWalk.setExpression(isExpressionFilter(filterPattern) ? filterPattern : QString(), &error);
        if (!parsed) {
            filterError = error;
        }
        if (filterPattern.isEmpty() || !parsed) {
            filterSelects = false;
            resultsModel->hasMoreRow = false;
            resultsModel->setStringList(QStringList());
            updateFilterMessage();
            updateStatus();

            return;
        }
        updateFilterMessage();
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
            filterCapped = false;

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

    void updateFilterMessage()
    {
        const int unloaded = filterPattern.isEmpty() ? 0 : filterWalk.getUnloadedBranchCount();
        QString text;

        if (!filterError.isEmpty()) {
            text = filterError;
        } else if (unloaded > 0) {
            text = (unloaded == 1) ? SceneGraphPanel::tr("1 unloaded branch not searched: load it to search inside.")
                                   : SceneGraphPanel::tr("%1 unloaded branches not searched: load them to search inside.").arg(unloaded);
        }
        filterMessage->setText(text);
        filterMessage->setStyleSheet(filterError.isEmpty() ? QString() : QString::fromUtf8("QLabel { color: #e05050; }"));
        filterMessage->setVisible(!text.isEmpty());
        filter->setToolTip(filterError.isEmpty() ? filterToolTip() : filterError);
        applyButton->setEnabled(filterWalk.hasExpression() && filterError.isEmpty());
    }

    static QString filterToolTip()
    {
        return SceneGraphPanel::tr("Text without /, { or % lists the prims whose names hold it; * and ? match any text and any character.\n"
                                   "Text that starts with / or holds a {predicate} is a path expression, as prim pattern knobs take it: "
                                   "/World/* selects the children of /World, //*Light* any prim whose name holds Light, at any depth, "
                                   "//{kind:component} every component, /World//{isa:Mesh} the meshes below /World, "
                                   "//{purpose:proxy} the prims whose purpose, authored or inherited, is proxy "
                                   "(default, render, proxy or guide). "
                                   "Separate patterns with spaces for their union, and combine them with &, - and ~.\n"
                                   "Here only, not in prim pattern knobs, {loaded:false} matches prims whose payload is unloaded and "
                                   "{active:false} inactive prims. The matches are selected as they are found; "
                                   "Apply To Node writes the expression to the prim pattern of the current node.");
    }

    // Selects the matches of an edited expression in the viewer, and their rows.
    void selectFilterMatches()
    {
        const bool wasSyncing = syncingSelection;

        syncingSelection = true;
        if (sharesViewerSelection()) {
            viewport->setSelection(filterMatches);
        }
        selectionOrder = filterMatches;
        // Selecting the tree's rows can read long sibling lists; it waits until the tree shows.
        treeSelectionOwed = true;
        syncingSelection = wasSyncing;
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
        } else if (filterSelects) {
            selectResults(filterMatches);
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
            if (treeSelectionDeferred) {
                // The rest are asked for too, so that their reads queue with the first.
                model->indexOfPath(path, true, &treeSelectionDeferred);
                continue;
            }
            const QModelIndex index = model->indexOfPath(path, true, &treeSelectionDeferred);
            if (treeSelectionDeferred) {
                continue;
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
        if (treeSelectionDeferred) {
            // The rows keep showing the selection they had until the stage can be read.
            treeSelectionOwed = true;
            restoringExpansion = wasRestoring;
            syncingSelection = wasSyncing;

            return;
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

    NodeSourceBar::ToolTips toolTips;
    toolTips.pin = tr("Keeps showing the scene of the node shown now, whichever viewer is active, whatever it views and whatever is selected.");
    toolTips.follow = tr("What the panel follows when not pinned. Viewer: the scene the active viewer shows. "
                         "Node Selection: the scene of the node last selected in the node graph that puts out "
                         "one; selecting other nodes keeps it.");
    toolTips.node = tr("The node whose scene is shown: the one followed, or a node pinned by name.");
    _imp->sourceBar = new NodeSourceBar(gui, QString::fromUtf8("sceneGraph"), putsOutScene, toolTips, this);
    _imp->sourceBar->setShownNode([this]() { return getShownNode(); });
    layout->addWidget(_imp->sourceBar);

    _imp->status = new QLabel(this);
    _imp->status->setObjectName(QString::fromUtf8("sceneGraphStatus"));
    _imp->status->setContentsMargins(4, 0, 4, 0);
    layout->addWidget(_imp->status);

    _imp->filter = new QLineEdit(this);
    _imp->filter->setObjectName(QString::fromUtf8("sceneGraphFilter"));
    _imp->filter->setPlaceholderText(tr("Filter by name, or by a path expression such as //{kind:component}"));
    _imp->filter->setToolTip(Implementation::filterToolTip());
    _imp->filter->setClearButtonEnabled(true);
    _imp->applyButton = new QToolButton(this);
    _imp->applyButton->setObjectName(QString::fromUtf8("sceneGraphApplyToNode"));
    _imp->applyButton->setText(tr("Apply To Node"));
    _imp->applyButton->setToolTip(tr("Writes the filter's path expression to the prim pattern of the current node, "
                                     "the one whose settings were opened last, as one undoable edit."));
    _imp->applyButton->setEnabled(false);
    _imp->filterMessage = new QLabel(this);
    _imp->filterMessage->setObjectName(QString::fromUtf8("sceneGraphFilterMessage"));
    _imp->filterMessage->setContentsMargins(4, 0, 4, 0);
    _imp->filterMessage->setWordWrap(true);
    _imp->filterMessage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _imp->filterMessage->setVisible(false);
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
    layout->addLayout(filterLayout);
    layout->addWidget(_imp->filterMessage);

    _imp->resetDrawStatesButton = new QToolButton(this);
    _imp->resetDrawStatesButton->setObjectName(QString::fromUtf8("sceneGraphResetDrawStates"));
    _imp->resetDrawStatesButton->setText(tr("Reset to Graph"));
    _imp->resetDrawStatesButton->setToolTip(tr("Clears every draw state set in the linked viewer, so that every prim is drawn as the graph says."));
    _imp->resetDrawStatesButton->setEnabled(false);
    _imp->bakeDrawStatesButton = new QToolButton(this);
    _imp->bakeDrawStatesButton->setObjectName(QString::fromUtf8("sceneGraphBakeDrawStates"));
    _imp->bakeDrawStatesButton->setText(tr("Bake to Node"));
    _imp->bakeDrawStatesButton->setToolTip(tr("Writes every draw state set in the linked viewer into a USDExpandHierarchy below the viewed node, "
                                              "one rule per prim, and clears them from the viewer, which then views that node. The viewed node "
                                              "takes the rules itself when it is a USDExpandHierarchy, and so does one directly below it."));
    _imp->bakeDrawStatesButton->setEnabled(false);
    _imp->cancelLoadsButton = new QToolButton(this);
    _imp->cancelLoadsButton->setObjectName(QString::fromUtf8("sceneGraphCancelLoads"));
    _imp->cancelLoadsButton->setText(tr("Cancel Loads"));
    _imp->cancelLoadsButton->setToolTip(tr("Drops the payload loads not started yet on the stage the rows are read from: the linked viewer's, "
                                           "queued for prims set to Geometry or Load + Draw, as switching the viewer to another node does, or the "
                                           "pinned node's. A Load To walk stops after the level under way. A load under way finishes, and what is "
                                           "loaded stays."));
    _imp->cancelLoadsButton->setEnabled(false);

    QHBoxLayout* bulkLayout = new QHBoxLayout();
    bulkLayout->setContentsMargins(0, 0, 2, 0);
    bulkLayout->setSpacing(2);
    _imp->bulkScope = new QComboBox(this);
    _imp->bulkScope->setObjectName(QString::fromUtf8("sceneGraphBulkScope"));
    _imp->bulkScope->setToolTip(tr("What the commands beside act on. Selected: the selected rows. Below: the selected rows and "
                                   "everything below them. To <Kind>: the prims of that kind below the selected rows, down to the "
                                   "first one on each branch; a custom kind matches itself only. A custom kind derived from none of "
                                   "assembly, component and subcomponent stops no branch: Load To it loads every payload on a branch "
                                   "that holds no prim of it."));
    _imp->bulkScope->addItem(tr("Selected"), QString::fromUtf8("selected"));
    _imp->bulkScope->addItem(tr("Below"), QString::fromUtf8("below"));
    _imp->bulkScope->addItem(tr("To Assembly"), QString::fromUtf8("assembly"));
    _imp->bulkScope->addItem(tr("To Component"), QString::fromUtf8("component"));
    _imp->bulkScope->addItem(tr("To Subcomponent"), QString::fromUtf8("subcomponent"));
    const std::vector<PXR_NS::TfToken> customKinds = registeredCustomKinds();
    if (!customKinds.empty()) {
        _imp->bulkScope->insertSeparator(_imp->bulkScope->count());
    }
    for (const PXR_NS::TfToken& kind : customKinds) {
        const QString name = QString::fromStdString(kind.GetString());
        _imp->bulkScope->addItem(tr("To %1").arg(name), name);
        const QString tip = (sceneKindLevel(kind) == eSceneKindLevelPassThrough)
            ? tr("The %1 prims below the selected rows. %1 derives from none of assembly, component and subcomponent, so no "
                 "other kind stops the walk: Load To %1 loads every payload on a branch that holds no %1 prim.")
                  .arg(name)
            : tr("The %1 prims below the selected rows, down to the first prim of its level on each branch; prims of that "
                 "level of another kind are not acted on.")
                  .arg(name);
        _imp->bulkScope->setItemData(_imp->bulkScope->count() - 1, tip, Qt::ToolTipRole);
    }
    bulkLayout->addWidget(_imp->bulkScope);
    const int layoutSpacing = bulkLayout->spacing();
    const int groupGap = 16;
    const int iconGap = groupGap / 2;
    const auto addGap = [bulkLayout, layoutSpacing](int gap) {
        bulkLayout->addSpacing(std::max(0, gap - 2 * layoutSpacing));
    };
    const auto addDivider = [this, bulkLayout, &addGap, groupGap](int height) {
        QFrame* divider = new QFrame(this);
        divider->setFrameShape(QFrame::VLine);
        divider->setFrameShadow(QFrame::Sunken);
        divider->setFixedHeight(height);
        addGap(groupGap / 2);
        bulkLayout->addWidget(divider, 0, Qt::AlignVCenter);
        addGap(groupGap / 2);
    };
    const auto addSection = [this, bulkLayout, &addGap, iconGap](const char* objectName, const char* iconPath, const QString& tip) {
        addGap(iconGap);
        QLabel* icon = new QLabel(this);
        icon->setObjectName(QString::fromUtf8(objectName));
        icon->setFrameStyle(QFrame::NoFrame);
        icon->setContentsMargins(0, 0, 0, 0);
        icon->setAutoFillBackground(false);
        icon->setStyleSheet(QString::fromUtf8("QLabel { background: transparent; border: none; margin: 0; padding: 0; }"));
        icon->setFixedSize(16, 16);
        icon->setToolTip(tip);
        ThemedIcon::setLabelIcon(icon, ThemedIcon::icon(QString::fromUtf8(iconPath), 16), 16);
        bulkLayout->addWidget(icon, 0, Qt::AlignVCenter);
        addGap(iconGap);
    };
    struct BulkButton {
        const char* action;
        const char* label;
    };
    const auto addBulkButtons = [this, bulkLayout](const std::vector<BulkButton>& buttons) {
        for (const BulkButton& entry : buttons) {
            const QString action = QString::fromUtf8(entry.action);
            QToolButton* button = new QToolButton(this);
            button->setObjectName(QString::fromUtf8("sceneGraphBulk_") + action);
            button->setText(tr(entry.label));
            button->setCheckable(Implementation::isBulkDrawAction(action));
            button->setEnabled(false);
            QObject::connect(button, &QToolButton::clicked, this, [this, action]() {
                _imp->runBulk(action, getSelection(), _imp->bulkScopeName());
            });
            _imp->bulkButtons.push_back(std::make_pair(action, button));
            bulkLayout->addWidget(button);
        }
    };
    addSection("sceneGraphBulkLoadIcon", NATRON_ICONS_PATH "lucide/package-open.svg", tr("Load"));
    addBulkButtons({
        { "load", QT_TR_NOOP("Load") },
        { "unload", QT_TR_NOOP("Unload") },
    });
    addSection("sceneGraphBulkDisplayIcon", NATRON_ICONS_PATH "lucide/eye.svg", tr("Display"));
    addBulkButtons({
        { "bbox", QT_TR_NOOP("BBox") },
        { "passThrough", QT_TR_NOOP("Pass Through") },
        { "geometry", QT_TR_NOOP("Geometry") },
        { "loadAndDraw", QT_TR_NOOP("Load + Draw") },
        { "hide", QT_TR_NOOP("Hide") },
    });
    addDivider(12);
    addBulkButtons({
        { "clearBelow", QT_TR_NOOP("Clear Below") },
    });
    bulkLayout->addWidget(_imp->resetDrawStatesButton);
    addDivider(12);
    const auto addUnfoldButton = [this, bulkLayout](const char* objectName, const QString& label, const char* iconPath, bool fold) {
        QToolButton* button = new QToolButton(this);
        button->setObjectName(QString::fromUtf8(objectName));
        button->setText(label);
        button->setIcon(ThemedIcon::icon(QString::fromUtf8(iconPath), 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setEnabled(false);
        QObject::connect(button, &QToolButton::clicked, this, [this, fold]() { _imp->runUnfold(fold, getSelection(), _imp->bulkScopeName()); });
        bulkLayout->addWidget(button);

        return button;
    };
    _imp->unfoldButton = addUnfoldButton("sceneGraphUnfold", tr("Unfold"), NATRON_ICONS_PATH "lucide/chevrons-down.svg", false);
    _imp->foldButton = addUnfoldButton("sceneGraphFold", tr("Fold"), NATRON_ICONS_PATH "lucide/chevrons-down-up.svg", true);
    bulkLayout->addStretch(1);
    layout->addLayout(bulkLayout);
    _imp->bulkStatus = new QLabel(this);
    _imp->bulkStatus->setObjectName(QString::fromUtf8("sceneGraphBulkStatus"));
    _imp->bulkStatus->setContentsMargins(4, 0, 4, 0);
    _imp->bulkStatus->setWordWrap(true);
    _imp->bulkStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _imp->bulkStatus->setVisible(false);
    layout->addWidget(_imp->bulkStatus);
    _imp->bulkRefresh.setSingleShot(true);
    _imp->bulkRefresh.setInterval(0);
    QObject::connect(&_imp->bulkRefresh, &QTimer::timeout, this, [this]() { _imp->updateBulkButtons(); });
    QObject::connect(_imp->bulkScope, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this]() { _imp->bulkRefresh.start(); });

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    QWidget* treePane = new QWidget(splitter);
    treePane->setObjectName(QString::fromUtf8("sceneGraphTreePane"));
    QVBoxLayout* treePaneLayout = new QVBoxLayout(treePane);
    treePaneLayout->setContentsMargins(0, 0, 0, 0);
    treePaneLayout->setSpacing(2);
    _imp->stack = new QStackedWidget(treePane);
    treePaneLayout->addWidget(_imp->stack, 1);
    QHBoxLayout* nodeLayout = new QHBoxLayout();
    nodeLayout->setContentsMargins(0, 0, 2, 0);
    nodeLayout->setSpacing(4);
    nodeLayout->addWidget(_imp->applyButton);
    nodeLayout->addWidget(_imp->bakeDrawStatesButton);
    nodeLayout->addStretch(1);
    nodeLayout->addWidget(_imp->cancelLoadsButton);
    treePaneLayout->addLayout(nodeLayout);

    _imp->model = new SceneGraphModel(this);
    _imp->model->setReadsOnWorker(true);
    // Made hidden: keeping rows nobody sees in step with a heavy stage would stall the window.
    _imp->model->setDeferred(!isVisible());
    _imp->tree = new QTreeView(_imp->stack);
    _imp->tree->setObjectName(QString::fromUtf8("sceneGraphTree"));
    _imp->tree->setModel(_imp->model);
    QObject::connect(_imp->model, &SceneGraphModel::deferredReadsDone, this, [this]() {
        // As for viewRestore: a run's reads drain many times over, and its end restores once.
        if (_imp->expansionDeferred && !(_imp->unfolder && _imp->unfolder->isRunning())) {
            _imp->restoreView();
        }
        // Behind the filter's results too: showSelection() selects the tree's rows there, and a
        // pick the filter hides is selected in the tree alone.
        if (_imp->treeSelectionDeferred && (_imp->stack->currentWidget() != _imp->changedHereTree)) {
            _imp->selectTreeRows(_imp->selectionOrder);
        }
    });
    _imp->tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _imp->tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->tree->setUniformRowHeights(true);
    _imp->tree->setItemDelegateForColumn(SceneGraphModel::eColumnName, new PayloadBadgeDelegate(_imp->tree));
    _imp->tree->setItemDelegateForColumn(SceneGraphModel::eColumnDrawState, new DrawStateDelegate(_imp->tree));
    _imp->tree->setItemDelegateForColumn(SceneGraphModel::eColumnPurpose, new ScenePurposeDelegate(_imp->tree));
    _imp->tree->setDragEnabled(true);
    _imp->tree->setDragDropMode(QAbstractItemView::DragOnly);
    _imp->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _imp->tree->header()->setStretchLastSection(false);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnName, QHeaderView::Stretch);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnType, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnKind, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnPurpose, QHeaderView::ResizeToContents);
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
    _imp->changedHereTree->setItemDelegateForColumn(SceneGraphModel::eColumnPurpose, new ScenePurposeDelegate(_imp->changedHereTree));
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
    splitter->addWidget(treePane);
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
    _imp->viewRestore.setSingleShot(true);
    _imp->viewRestore.setInterval(0);

    QObject::connect(_imp->sourceBar, &NodeSourceBar::targetChanged, this, [this]() {
        showNodeScene(_imp->sourceBar->getTarget());
        _imp->followViewer();
    });
    QObject::connect(_imp->sourceBar, &NodeSourceBar::settlingChanged, this, [this]() { _imp->publishState(); });
    QObject::connect(_imp->filter, &QLineEdit::textChanged, &_imp->filterDebounce, qOverload<>(&QTimer::start));
    QObject::connect(&_imp->filterDebounce, &QTimer::timeout, this, &SceneGraphPanel::onFilterEdited);
    QObject::connect(&_imp->filterStep, &QTimer::timeout, this, &SceneGraphPanel::onFilterStep);
    QObject::connect(_imp->applyButton, &QToolButton::clicked, this, &SceneGraphPanel::onApplyToNode);
    QObject::connect(&_imp->inspectorRefresh, &QTimer::timeout, this, &SceneGraphPanel::refreshInspector);
    QObject::connect(_imp->resetDrawStatesButton, &QToolButton::clicked, this, [this]() {
        if (_imp->viewport) {
            _imp->viewport->clearDrawStates();
        }
    });
    QObject::connect(_imp->cancelLoadsButton, &QToolButton::clicked, this, [this]() {
        if (_imp->pinned && (_imp->model->getSource() == _imp->pinned)) {
            _imp->pinned->cancelPendingLoads();
            _imp->model->settleLoads();
            _imp->updateBakeButton();
        } else if (_imp->viewport) {
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
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SceneGraphPanel::onTreeSelectionChanged);
    for (QAbstractItemView* view : { (QAbstractItemView*)_imp->tree, (QAbstractItemView*)_imp->results, (QAbstractItemView*)_imp->changedHereTree }) {
        QObject::connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { _imp->bulkRefresh.start(); });
    }
    QObject::connect(_imp->model, &QAbstractItemModel::dataChanged, this, [this]() { _imp->bulkRefresh.start(); });
    // The model announces draw-state and changed-here updates on the top-level rows only, so that
    // thousands of open rows don't cost a header layout each; the rows below repaint from this.
    QObject::connect(_imp->model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& topLeft) {
        if (!topLeft.parent().isValid()) {
            _imp->tree->viewport()->update();
        }
    });
    QObject::connect(_imp->model, &QAbstractItemModel::modelReset, this, [this]() { _imp->bulkRefresh.start(); });
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
    // Shift-clicks on branch arrows are taken before the view toggles the row.
    _imp->tree->viewport()->installEventFilter(this);
    _imp->results->installEventFilter(this);
    _imp->changedHereTree->installEventFilter(this);
    _imp->results->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(_imp->tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->tree, pos); });
    QObject::connect(_imp->changedHereTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->changedHereTree, pos); });
    QObject::connect(_imp->results, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showRowMenu(_imp->results, pos); });
    _imp->unfolder.reset(new SceneGraphUnfolder(_imp->tree, _imp->model, _imp->expanded));
    _imp->unfolder->setProgressCallback([this](const SceneGraphUnfolder::Progress& progress) { _imp->showUnfoldProgress(progress); });
    _imp->unfolder->setRestoreCallback([this](bool deferred) { _imp->onRestored(deferred); });
    QObject::connect(_imp->tree, &QTreeView::expanded, this, &SceneGraphPanel::onExpanded);
    QObject::connect(_imp->tree, &QTreeView::collapsed, this, &SceneGraphPanel::onCollapsed);
    QObject::connect(_imp->tree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { _imp->rememberTopPath(); });
    // Rows going, then coming back, as when the viewer shows another node and returns: the rows
    // that come back reopen, and the tree scrolls back to the row it had at its top.
    // A run's reads each add and take away a loading row, and the reopenings' own reads add rows
    // below rows they reopen and will reach: neither owes a reopening, which would make a wide run
    // or reopening quadratic. Rows a load brings in during a run do, once the run ends. The reset
    // stops a run before it gets here.
    const auto unfolderOwnsRows = [this]() {
        return _imp->unfolder && (_imp->unfolder->isRunning() || _imp->unfolder->isApplying());
    };
    const auto holdTop = [this, unfolderOwnsRows]() {
        _imp->holdTopPath = true;
        if (!unfolderOwnsRows()) {
            _imp->restoreAll = true;
        }
        _imp->viewRestore.start();
    };
    QObject::connect(_imp->model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        _imp->holdTopPath = true;
        _imp->restoreAll = true;
        _imp->insertedUnder.clear();
        _imp->viewRestore.start();
    });
    QObject::connect(_imp->model, &QAbstractItemModel::rowsAboutToBeRemoved, this, holdTop);
    QObject::connect(_imp->model, &QAbstractItemModel::layoutAboutToBeChanged, this, holdTop);
    QObject::connect(_imp->model, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex& parent) {
        const QString parentPath = parent.isValid() ? _imp->model->pathOf(parent) : QString();
        if (!_imp->expanded->empty() && !(_imp->unfolder && _imp->unfolder->ownsRowsUnder(parentPath))) {
            if (parent.isValid()) {
                _imp->insertedUnder.insert(parentPath);
            } else {
                _imp->restoreAll = true;
            }
        }
        _imp->viewRestore.start();
    });
    QObject::connect(&_imp->viewRestore, &QTimer::timeout, this, [this]() { _imp->restoreView(); });
    if (gui) {
        QObject::connect(gui, &Gui::activeViewerChanged, this, &SceneGraphPanel::onActiveViewerChanged);
        QObject::connect(gui, &Gui::viewersChanged, this, &SceneGraphPanel::onActiveViewerChanged);
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
    _imp->sourceBar->pinNode(node);
}

void
SceneGraphPanel::releaseNodeStages()
{
    _imp->sourceBar->release();
    showNodeScene(NodePtr());
    for (const Implementation::CachedStage& cached : _imp->cachedStages) {
        _imp->expansionMemory.forget(cached.source->getNode());
        _imp->retireSource(cached.source);
    }
    _imp->cachedStages.clear();
    _imp->expansionMemory.forgetGone();
    _imp->followViewer();
    _imp->publishState();
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
SceneGraphPanel::setFollow(NodeSourceBar::FollowEnum follow)
{
    _imp->sourceBar->setFollow(follow);
}

NodeSourceBar::FollowEnum
SceneGraphPanel::getFollow() const
{
    return _imp->sourceBar->getFollow();
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
    if (_imp->sourceBar->getFollow() == NodeSourceBar::eFollowSelection) {
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
    } else if (_imp->model->isDeferred()) {
        // Selecting reads the rows down to the paths; the show reads them on the worker.
        _imp->selectionOrder = paths;
        _imp->treeSelectionOwed = true;
    } else {
        _imp->selectTreeRows(paths);
    }
    if (!_imp->filterPattern.isEmpty()) {
        _imp->selectResults(paths);
    }
    _imp->inspectorRefresh.start();
}

void
SceneGraphPanel::setArmedPick(const QString& label)
{
    _imp->armedPick = label;
    _imp->updateStatus();
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
    return viewer && !_imp->sourceBar->isPinned() && (_imp->sourceBar->getFollow() == NodeSourceBar::eFollowViewer) && (_imp->viewer == viewer);
}

QStringList
SceneGraphPanel::getSelection() const
{
    if (_imp->treeSelectionOwed && _imp->model->isDeferred() && (_imp->shownRowsView() == _imp->tree)) {
        return _imp->selectionOrder;
    }
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
    _imp->payloadStatesOf(selection, &anyUnloaded, &anyOther);
    const bool onViewerStage = _imp->loadsOnShownStage();
    std::vector<std::pair<QString, QString>> kindScopes = {
        { QString::fromUtf8("assembly"), tr("Assembly") },
        { QString::fromUtf8("component"), tr("Component") },
        { QString::fromUtf8("subcomponent"), tr("Subcomponent") },
    };
    for (const PXR_NS::TfToken& kind : registeredCustomKinds()) {
        kindScopes.push_back(std::make_pair(QString::fromStdString(kind.GetString()), QString::fromStdString(kind.GetString())));
    }
    // Only the tree's rows open and close; the filter's list and Changed Here have no branches to walk.
    const bool canUnfold = (view == _imp->tree) && !selection.isEmpty();
    const auto addUnfold = [this, &selection, canUnfold](QMenu* into, const QString& text, const QString& objectName, bool fold, const QString& scope) {
        QAction* entry = into->addAction(text);
        entry->setObjectName(objectName);
        entry->setEnabled(canUnfold);
        entry->setToolTip(Implementation::unfoldToolTip(fold, scope));
        entry->setShortcut(Implementation::keySequenceOf(objectName.toUtf8().constData()));
        QObject::connect(entry, &QAction::triggered, this, [this, selection, fold, scope]() { _imp->runUnfold(fold, selection, scope); });
    };
    addUnfold(&menu, tr("Unfold All Below"), QString::fromUtf8("sceneGraphUnfoldBelow"), false, QString::fromUtf8("below"));
    QMenu* unfoldTo = menu.addMenu(tr("Unfold To"));
    unfoldTo->setObjectName(QString::fromUtf8("sceneGraphUnfoldToMenu"));
    unfoldTo->setToolTipsVisible(true);
    unfoldTo->setEnabled(canUnfold);
    for (const std::pair<QString, QString>& kind : kindScopes) {
        addUnfold(unfoldTo, kind.second, QString::fromUtf8("sceneGraphUnfoldTo_") + kind.first, false, kind.first);
    }
    addUnfold(&menu, tr("Fold All Below"), QString::fromUtf8("sceneGraphFoldBelow"), true, QString::fromUtf8("below"));
    menu.addSeparator();
    const auto addBulk = [this, &selection](QMenu* into, const QString& text, const QString& objectName, const QString& action, const QString& scope, bool enabled) {
        QAction* entry = into->addAction(text);
        entry->setObjectName(objectName);
        entry->setEnabled(enabled);
        entry->setShortcut(Implementation::keySequenceOf((QString::fromUtf8("sceneGraphBulk_") + action + QLatin1Char('_') + scope).toUtf8().constData()));
        QObject::connect(entry, &QAction::triggered, this, [this, selection, action, scope]() { _imp->runBulk(action, selection, scope); });

        return entry;
    };
    const QString loadAction = QString::fromUtf8("load");
    const QString unloadAction = QString::fromUtf8("unload");
    QMenu* loadMenu = menu.addMenu(tr("Load"));
    loadMenu->setObjectName(QString::fromUtf8("sceneGraphLoadMenu"));
    loadMenu->setEnabled(onViewerStage && !selection.isEmpty());
    addBulk(loadMenu, tr("Selected"), QString::fromUtf8("sceneGraphLoad"), loadAction, QString::fromUtf8("selected"), onViewerStage && anyUnloaded);
    addBulk(loadMenu, tr("Below"), QString::fromUtf8("sceneGraphLoadWithDescendants"), loadAction, QString::fromUtf8("below"), onViewerStage && !selection.isEmpty());
    loadMenu->addSeparator();
    for (const std::pair<QString, QString>& kind : kindScopes) {
        addBulk(loadMenu, tr("To %1").arg(kind.second), QString::fromUtf8("sceneGraphLoadTo_") + kind.first, loadAction, kind.first, onViewerStage && !selection.isEmpty());
    }
    QMenu* unloadMenu = menu.addMenu(tr("Unload"));
    unloadMenu->setObjectName(QString::fromUtf8("sceneGraphUnloadMenu"));
    unloadMenu->setEnabled(onViewerStage && !selection.isEmpty());
    addBulk(unloadMenu, tr("Selected"), QString::fromUtf8("sceneGraphUnload"), unloadAction, QString::fromUtf8("selected"), onViewerStage && anyOther);
    unloadMenu->addSeparator();
    for (const std::pair<QString, QString>& kind : kindScopes) {
        addBulk(unloadMenu, tr("Below %1").arg(kind.second), QString::fromUtf8("sceneGraphUnloadBelow_") + kind.first, unloadAction, kind.first, onViewerStage && !selection.isEmpty());
    }
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
    drawMenu->addSeparator();
    const bool canDraw = !drawViewer.isNull() && !selection.isEmpty();
    QMenu* drawBelow = drawMenu->addMenu(tr("Below"));
    drawBelow->setObjectName(QString::fromUtf8("sceneGraphDrawStateBelowMenu"));
    for (int state = (int)eSceneDrawStateBBox; state <= (int)eSceneDrawStateHidden; ++state) {
        const QString name = QString::fromUtf8(sceneDrawStateName((SceneDrawStateEnum)state));
        addBulk(drawBelow, SceneGraphModel::drawStateLabel(state), QString::fromUtf8("sceneGraphDrawStateBelow_") + name, name, QString::fromUtf8("below"), canDraw);
    }
    for (const std::pair<QString, QString>& kind : kindScopes) {
        QMenu* drawTo = drawMenu->addMenu(tr("To %1").arg(kind.second));
        drawTo->setObjectName(QString::fromUtf8("sceneGraphDrawStateToMenu_") + kind.first);
        for (int state = (int)eSceneDrawStateBBox; state <= (int)eSceneDrawStateHidden; ++state) {
            const QString name = QString::fromUtf8(sceneDrawStateName((SceneDrawStateEnum)state));
            addBulk(drawTo, SceneGraphModel::drawStateLabel(state), QString::fromUtf8("sceneGraphDrawStateTo_") + kind.first + QLatin1Char('_') + name, name, kind.first, canDraw);
        }
        drawTo->addSeparator();
        addBulk(drawTo, tr("Clear Below Each"), QString::fromUtf8("sceneGraphDrawStateClearBelowEach_") + kind.first, QString::fromUtf8("clearBelow"), kind.first, canDraw);
    }
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
        _imp->leaveExpansionNode();
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
    const QString pattern = _imp->filter->text().trimmed();
    const bool edited = (pattern != _imp->filterPattern);

    _imp->filterPattern = pattern;
    _imp->filterCap = kFilterCap;
    _imp->filterSelects = isExpressionFilter(pattern) && (edited || _imp->filterSelects);
    _imp->startFilter();
}

void
SceneGraphPanel::onApplyToNode()
{
    // The field may have been edited since the debounced filter last read it.
    if (_imp->filterDebounce.isActive() || (_imp->filter->text().trimmed() != _imp->filterPattern)) {
        _imp->filterDebounce.stop();
        onFilterEdited();
    }
    if (!_imp->filterWalk.hasExpression() || !_imp->filterError.isEmpty()) {
        return;
    }
    SceneSelectionActions::applyPatternToCurrentNode(_imp->gui, _imp->filterPattern.toStdString());
}

void
SceneGraphPanel::onFilterStep()
{
    const bool over = _imp->stepFilter();

    if (over) {
        _imp->filterStep.stop();
    }
    if (over || (_imp->filterMatches.size() != _imp->resultsModel->rowCount())) {
        if (_imp->filterSelects) {
            _imp->selectFilterMatches();
        }
        _imp->showFilterResults();
    }
    // A walk stopped at the result cap goes on when More is clicked, and selects what it finds then.
    if (over && !_imp->filterCapped) {
        _imp->filterSelects = false;
    }
    _imp->updateFilterMessage();
    _imp->publishState();
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
        _imp->sourceBar->dropTarget();

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
    if (!_imp->model->isDeferred()) {
        _imp->restoreView();
    }
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
    _imp->expanded->insert(path);
    // The unfolder opens no unloaded payload row, and publishes once a slice rather than per row.
    if (_imp->unfolder && _imp->unfolder->isApplying()) {
        return;
    }
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
    _imp->expanded->erase(_imp->model->pathOf(index));
    if (_imp->unfolder && _imp->unfolder->isApplying()) {
        return;
    }
    _imp->publishState();
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
    const NodePtr shown = _imp->modelNode();
    ViewerInstancePtr shownBy;
    if (shown && _imp->viewer && (shown == _imp->viewer->getSceneViewedNode()) && _imp->viewer->getInternalNode()) {
        shownBy = std::dynamic_pointer_cast<ViewerInstance>(_imp->viewer->getInternalNode()->shared_from_this());
    }
    _imp->inspector->setNode(shown, shownBy);
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

void
SceneGraphPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!_imp->model->isDeferred()) {
        return;
    }
    _imp->model->setDeferred(false);
    if (_imp->restoreAll) {
        _imp->restoreView();
    }
    if (_imp->treeSelectionOwed && (_imp->stack->currentWidget() == _imp->tree)) {
        _imp->askForOwedRows();
    }
    _imp->updateStatus();
    _imp->inspectorRefresh.start();
}

void
SceneGraphPanel::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    // A minimised window's hide is spontaneous, and its rows are kept up to date for its return.
    if (!e->spontaneous()) {
        _imp->model->setDeferred(true);
    }
}

bool
SceneGraphPanel::eventFilter(QObject* watched,
                             QEvent* e)
{
    // Shift-click on a branch arrow expands all below it, or, on an open row, collapses all below.
    // A plain click is the view's, which loads an unloaded row it opens.
    if ((watched == _imp->tree->viewport()) && ((e->type() == QEvent::MouseButtonPress) || (e->type() == QEvent::MouseButtonRelease) || (e->type() == QEvent::MouseButtonDblClick))) {
        QMouseEvent* m = static_cast<QMouseEvent*>(e);
        const Qt::KeyboardModifiers held = m->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if ((m->button() == Qt::LeftButton) && (held == Qt::ShiftModifier)) {
            const QModelIndex index = _imp->branchArrowAt(m->position().toPoint());
            if (index.isValid()) {
                if (e->type() == QEvent::MouseButtonPress) {
                    _imp->runUnfold(_imp->tree->isExpanded(index), QStringList(_imp->model->pathOf(index)), QString::fromUtf8("below"));
                }
                e->accept();

                return true;
            }
        }
    }
    if ((watched == _imp->tree) && ((e->type() == QEvent::ShortcutOverride) || (e->type() == QEvent::KeyPress))) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if ((k->key() == Qt::Key_Escape) && _imp->hasStoppable()) {
            if (e->type() == QEvent::KeyPress) {
                _imp->stopStoppable();
            }
            e->accept();

            return true;
        }
        if (const SceneGraphBulkKeyBinding* command = Implementation::expandCommandForKey(k->modifiers(), k->key())) {
            if (e->type() == QEvent::KeyPress) {
                _imp->runUnfold(command->action == "fold", getSelection(), QString::fromStdString(command->scope));
            }
            e->accept();

            return true;
        }
        // The view's own * expands recursively, which would load every payload it opened.
        const Qt::KeyboardModifiers held = k->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
        if ((e->type() == QEvent::KeyPress) && (k->key() == Qt::Key_Asterisk) && (held == Qt::NoModifier)) {
            const QModelIndex current = _imp->tree->currentIndex();
            if (current.isValid()) {
                _imp->runUnfold(false, QStringList(_imp->model->pathOf(current.siblingAtColumn(0))), QString::fromUtf8("below"));
            }
            e->accept();

            return true;
        }
    }
    // Copy, the 3D viewer's selection keys and the toolbar commands' keys work on the panel's selection. A key press is taken
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
        if (const SceneGraphBulkKeyBinding* bulk = Implementation::bulkCommandForKey(k->modifiers(), k->key())) {
            e->accept();
            if (e->type() == QEvent::KeyPress) {
                _imp->runBulk(QString::fromStdString(bulk->action), getSelection(), QString::fromStdString(bulk->scope));
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
    if ((e->key() == Qt::Key_Escape) && _imp->hasStoppable()) {
        _imp->stopStoppable();
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
