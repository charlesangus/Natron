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

#ifndef Gui_NodeGraph_h
#define Gui_NodeGraph_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsView>
#include <QDialog>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"
#include "Global/QtCompat.h"

#include "Engine/NodeGraphI.h"
#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/NodeGraphConnect.h"
#include "Gui/NodeGraphPickUp.h"
#include "Gui/PanelWidget.h"

NATRON_NAMESPACE_ENTER

class NodeGraphPrivate;

class NodeGraph
    : public QGraphicsView, public NodeGraphI, public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    explicit NodeGraph(Gui* gui,
                       const NodeCollectionPtr& group,
                       QGraphicsScene* scene = 0,
                       QWidget *parent = 0);

    virtual ~NodeGraph();

    static void makeFullyQualifiedLabel(Node* node, std::string* ret);
    NodeCollectionPtr getGroup() const;
    const std::list<NodeGuiPtr> & getSelectedNodes() const;
    NodeGuiPtr createNodeGUI(const NodePtr & node,
                             const CreateNodeArgs& args);

    void selectNode(const NodeGuiPtr & n, bool addToSelection);

    void deselectNode(const NodeGuiPtr& n);

    void setSelection(const NodesGuiList& nodes);

    void clearSelection();

    ///The visible portion of the graph, in scene coordinates.
    QRectF visibleSceneRect() const;
    QRect visibleWidgetRect() const;

    void deselect();

    bool areAllNodesVisible();

    /**
     * @brief Recompose the navigator from its cached thumbnail and the visible rectangle
     **/
    void updateNavigator();

    /**
     * @brief Call when the nodes' bounds or appearance change: the navigator thumbnail is
     * rendered again shortly after, once for any number of calls in between.
     **/
    void invalidateNavigatorContent();

    const NodesGuiList & getAllActiveNodes() const;
    NodesGuiList getAllActiveNodes_mt_safe() const;

    void moveToTrash(NodeGui* node);

    void restoreFromTrash(NodeGui* node);

    QGraphicsItem* getRootItem() const;
    virtual void notifyGuiClosing() OVERRIDE FINAL;
    void discardScenePointer();


    /**
     * @brief Removes the given node from the nodegraph, using the undo/redo stack.
     **/
    void removeNode(const NodeGuiPtr & node);

    void centerOnItem(QGraphicsItem* item);

    using PanelWidget::pushUndoCommand;

    /**
     * @brief Records the command as changing the state of the given nodes, so that their
     * panels' undo waits for it to be undone first.
     **/
    void pushUndoCommand(QUndoCommand* command, const NodesGuiList& touchedNodes);

    void deleteNodePermanantly(const NodeGuiPtr& n);

    NodesGuiList getNodesWithinBackdrop(const NodeGuiPtr& node) const;

    void selectAllNodes(bool onlyInVisiblePortion);

    /**
     * @brief Calls setParentItem(NULL) on all items of the scene to avoid Qt to double delete the nodes.
     **/
    void invalidateAllNodesParenting();

    bool areKnobLinksVisible() const;

    void refreshNodesKnobsAtTime(bool onlyTimeEvaluationKnobs, SequenceTime time);

    void copyNodesAndCreateInGroup(const NodesGuiList& nodes,
                                   const NodeCollectionPtr& group,
                                   std::list<std::pair<std::string, NodeGuiPtr> >& createdNodes);

    virtual void onNodesCleared() OVERRIDE FINAL;

    virtual void addNodesToCreationOf(const NodePtr& created,
                                      const NodesList& added) OVERRIDE FINAL;

    void setLastSelectedViewer(ViewerTab* tab);

    ViewerTab* getLastSelectedViewer() const;

    /**
     * @brief Given the node, it tries to move it to the ideal position
     * according to the position of the selected node and its inputs/outputs.
     * This is used when creating a node to position it correctly.
     * It will move the inputs / outputs slightly to fit this node into the nodegraph
     * so they do not overlap.
     **/
    void moveNodesForIdealPosition(const NodeGuiPtr &n,
                                   const NodeGuiPtr& selected,
                                   bool autoConnect);

    void copyNodes(const NodesGuiList& nodes, NodeClipBoard& clipboard);

    void pasteCliboard(const NodeClipBoard& clipboard, std::list<std::pair<std::string, NodeGuiPtr> >* newNodes);

    void duplicateSelectedNodes(const QPointF& pos);
    bool pasteNodeClipBoards(const QPointF& pos);

    /**
     * @brief Pastes a copy of each selected node, centred on pos, and makes it a Linked Node of the node
     * it was copied from: each of its knobs is linked two-way to the original's.
     **/
    void createLinkedNodes(const QPointF& pos);

    QPointF getRootPos() const;

    bool isDoingNavigatorRender() const;

    // The saved anti-aliasing preference, cached because the graph's items read it on every paint.
    bool isAntiAliasingEnabled() const;

    // True when the view is zoomed out past the point where node names become too small to read:
    // nodes then draw their body only, and edges their line only.
    bool isLowDetail() const;

    /**
     * @brief While a batch is open, a node that moves or resizes does not restack itself above
     * the nodes it overlaps at once: the outermost endNodeStackingBatch() does it in one pass for
     * every node that moved meanwhile.
     **/
    void beginNodeStackingBatch();
    void endNodeStackingBatch();

    // Returns false when no batch is open, in which case the caller restacks the node itself.
    bool deferNodeStacking(NodeGui* node);

    // Stacks each node above the visible nodes it overlaps, in order.
    void stackNodesAboveOverlapping(const std::vector<NodeGui*>& nodes);

    /**
     * @brief Queues a refresh of the node's state indicator and rendering edges. The refresh
     * timer only runs while nodes are queued.
     **/
    void scheduleNodeRenderStateRefresh(NodeGui* node);

public Q_SLOTS:

    void deleteSelection();

    void connectCurrentViewerToSelection(int inputNB, bool isASide);

    void updateCacheSizeText();

    void onRefreshNodesRenderStateTimerTimeout();

    void showMenu(const QPoint & pos);

    void toggleCacheInfo();

    void togglePreviewsForSelectedNodes();

    void showSelectedNodeSettingsPanel();

    void toggleAutoPreview();

    void toggleSelectedNodesEnabled();

    void forceRefreshAllPreviews();

    void toggleKnobLinksVisible();

    void switchInputs1and2ForSelectedNodes();

    void connectLaterSelectedNodesIntoFirstSelected();

    void connectEarlierSelectedNodesIntoLastSelected();

    void connectFirstSelectedIntoLaterSelectedNodes();

    void connectLastSelectedIntoOtherSelectedNodes();

    void extractSelectedNode();

    void createGroupFromSelection();

    void expandSelectedGroups();

    ///All these actions also work for backdrops
    /////////////////////////////////////////////
    ///Copy selected nodes to the clipboard, wiping previous clipboard
    void copySelectedNodes();

    void cutSelectedNodes();
    bool pasteNodeClipBoards();
    void duplicateSelectedNodes();
    void createLinkedNodes();

    /**
     * @brief Unlinks every knob of the selected nodes, and of the nodes in selected backdrops, from its
     * link groups that are not permanent.
     **/
    void unlinkSelectedNodes();
    /////////////////////////////////////////////

    void centerOnAllNodes();

    void toggleHideInputs();

    void popCreateNodeDialog();

    void popFindDialog( const QPoint& pos = QPoint(0, 0) );

    void renameNode();

    void onFindNodeDialogFinished();

    void refreshAllKnobsGui();

    void onNodeNameEditDialogFinished();

    void toggleAutoTurbo();

    void onGroupNameChanged(const QString& name);
    void onGroupScriptNameChanged(const QString& name);


    void onAutoScrollTimerTriggered();

    void onSettingsSaved();

private:

    NodeGraph(const NodeGraph&) = delete;

    NodeGraph& operator=(const NodeGraph&) = delete;

    void showNodePanel(bool casIsCtrl, bool casIsShift, NodeGui* nearbyNode);

    void checkForSpliceHint();

    void onNavigatorRenderTimerTimeout();

    void buildSplicePlan();

    void clearSpliceHint();

    void resetSplicePlan();

    void cancelSplice();

    bool connectInputArrowToNode(Edge* arrow, const NodeGuiPtr& target);

    bool moveInputArrowDestinationToNode(Edge* arrow, const NodeGuiPtr& target);

    bool connectOutputArrowToNode(Edge* arrow, const NodeGuiPtr& target);

    void applyConnectPlan(const std::vector<ConnectPlanConnection>& plan, const QString& undoText);

    bool hasPickedUpArrow() const;

    Edge* getPickedUpArrow() const;

    void pickUpArrow(Edge* arrow, PickUpEndEnum end);

    void forgetPickedUpArrow();

    void dragPickedUpArrowToCursor();

    void dropPickedUpArrow();

    bool handlePickUpPress(QMouseEvent* e);

    void moveSelectedNodesBy(bool shiftdown, bool controlDown, const QPointF& lastMousePosScene, const QPointF& newPos, const QRectF& visibleSceneR, bool userEdit);

    void scrollViewIfNeeded(const QPointF& scenePos);

    void checkAndStartAutoScrollTimer(const QPointF& scenePos);

    bool isNearbyNavigator(const QPoint& widgetPos, QPointF& scenePos) const;

    virtual void enterEvent(QEnterEvent* e) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual void keyReleaseEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual bool event(QEvent* e) OVERRIDE FINAL;
    virtual bool viewportEvent(QEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void resizeEvent(QResizeEvent* e) OVERRIDE FINAL;
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;
    virtual void focusInEvent(QFocusEvent* e) OVERRIDE FINAL;
    virtual void focusOutEvent(QFocusEvent* e) OVERRIDE FINAL;

private:

    enum NearbyItemEnum
    {
        eNearbyItemNone = 0,
        eNearbyItemNode,
        eNearbyItemBackdropResizeHandle,
        eNearbyItemBackdropFrame,
        eNearbyItemNodeEdge,
        eNearbyItemEdgeBendPoint,
    };

    void getNodesWithinViewportRect(const QRect& rect, std::set<NodeGui*>* nodes) const;

    // The graph items, text included unless skipText, whose shape meets a viewport rectangle.
    void getGraphItemsMeetingViewportRect(const QRect& rect, bool skipText, std::vector<QGraphicsItem*>* hits) const;

    NearbyItemEnum hasItemNearbyMouse(const QPoint& mousePosViewport,
                                      NodeGui** node,
                                      Edge** edge);

    void moveRootInternal(double dx, double dy);

    void wheelEventInternal(bool ctrlDown, double delta);

    std::unique_ptr<NodeGraphPrivate> _imp;
};


struct FindNodeDialogPrivate;
class FindNodeDialog
    : public QDialog
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    FindNodeDialog(NodeGraph* graph,
                   QWidget* parent);

    virtual ~FindNodeDialog();

public Q_SLOTS:

    void onButtonClicked(QAbstractButton* button);

    void updateFindResults(const QString& filter);

    void updateFindResultsWithCurrentFilter();
    void forceUpdateFindResults();

private:


    void selectNextResult();

    virtual void changeEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    std::unique_ptr<FindNodeDialogPrivate> _imp;
};

struct EditNodeNameDialogPrivate;
class EditNodeNameDialog
    : public QDialog
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    EditNodeNameDialog(const NodeGuiPtr& node,
                       QWidget* parent);

    virtual ~EditNodeNameDialog();

    QString getTypedName() const;

    NodeGuiPtr getNode() const;

private:

    virtual void changeEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    std::unique_ptr<EditNodeNameDialogPrivate> _imp;
};

// Opens a node stacking batch on graph, if any, for the scope.
class NodeStackingBatch {
public:
    explicit NodeStackingBatch(NodeGraph* graph)
        : _graph(graph)
    {
        if (_graph) {
            _graph->beginNodeStackingBatch();
        }
    }

    ~NodeStackingBatch()
    {
        if (_graph) {
            _graph->endNodeStackingBatch();
        }
    }

    NodeStackingBatch(const NodeStackingBatch&) = delete;
    NodeStackingBatch& operator=(const NodeStackingBatch&) = delete;

private:
    NodeGraph* _graph;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraph_h
