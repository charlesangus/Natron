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

#ifndef Gui_NodeGraphPrivate_h
#define Gui_NodeGraphPrivate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <set>
#include <list>
#include <map>
#include <utility>
#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsPixmapItem>
#include <QMutex>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"
#include "Gui/NodeGraphPickUp.h"
#include "Gui/NodeGraphUndoRedo.h" // NodeGuiPtr

#include "Serialization/SerializationFwd.h"

#define NATRON_CACHE_SIZE_TEXT_REFRESH_INTERVAL_MS 1000
#define NATRON_NODES_RENDER_STATE_REFRESH_INTERVAL_MS 300

///These are percentages of the size of the NodeGraph in widget coordinates.
#define NATRON_NAVIGATOR_BASE_HEIGHT 0.2
#define NATRON_NAVIGATOR_BASE_WIDTH 0.2

#define NATRON_SCENE_MAX 1e6
#define NATRON_SCENE_MIN 0

NATRON_NAMESPACE_ENTER

enum EventStateEnum
{
    eEventStateNone,
    eEventStateMovingArea,
    eEventStateZoomingArea,
    eEventStateDraggingArrow,
    eEventStateDraggingNavigator,
    eEventStateDraggingNode,
    eEventStateResizingBackdrop,
    eEventStateSelectionRect,
};

class Navigator
    : public QGraphicsPixmapItem
{
    QGraphicsLineItem* _navLeftEdge;
    QGraphicsLineItem* _navBottomEdge;
    QGraphicsLineItem* _navRightEdge;
    QGraphicsLineItem* _navTopEdge;

public:

    Navigator(QGraphicsItem* parent = 0)
        : QGraphicsPixmapItem(parent)
        , _navLeftEdge(NULL)
        , _navBottomEdge(NULL)
        , _navRightEdge(NULL)
        , _navTopEdge(NULL)
    {
        QPen p;

        p.setBrush( QColor(200, 200, 200) );
        p.setWidth(2);

        _navLeftEdge = new QGraphicsLineItem(this);
        _navLeftEdge->setPen(p);

        _navBottomEdge = new QGraphicsLineItem(this);
        _navBottomEdge->setPen(p);

        _navRightEdge = new QGraphicsLineItem(this);
        _navRightEdge->setPen(p);

        _navTopEdge = new QGraphicsLineItem(this);
        _navTopEdge->setPen(p);
    }

    virtual ~Navigator()
    {
    }

    int getLineWidth() const
    {
        return _navLeftEdge->pen().width();
    }

    void refreshPosition(const QPointF & navTopLeftScene,
                         double width,
                         double height)
    {
        setPos(navTopLeftScene);

        _navLeftEdge->setLine(0,
                              height,
                              0,
                              0);

        _navTopEdge->setLine(0,
                             0,
                             width,
                             0);

        _navRightEdge->setLine(width,
                               0,
                               width,
                               height);

        _navBottomEdge->setLine(width,
                                height,
                                0,
                                height);
    }
};


class NodeGraphPrivate
{
public:
    NodeGraph* _publicInterface;
    NodeCollectionWPtr group;
    QPoint _lastMousePos;
    // Where the last press landed, in view pixels, so that a release can tell a click with a
    // little jitter from a drag.
    QPoint _pressPos;
    QPointF _lastSelectionStartPointScene;
    EventStateEnum _evtState;
    NodeGuiPtr _magnifiedNode;
    double _nodeSelectedScaleBeforeMagnif;
    bool _magnifOn;
    Edge* _arrowSelected;
    mutable QMutex _nodesMutex;
    NodesGuiList _nodes;
    NodesGuiList _nodesTrash;

    /// Enables the create-node shortcut that opens the tab menu.
    /// This is set to true on enterEvent and set back to false on leaveEvent
    bool _nodeCreationShortcutEnabled;
    QGraphicsItem* _root; ///< this is the parent of all items in the graph
    QGraphicsItem* _nodeRoot; ///< this is the parent of all nodes
    QGraphicsSimpleTextItem* _cacheSizeText;
    bool cacheSizeHidden;
    QTimer _refreshCacheTextTimer;
    Navigator* _navigator;
    QMenu* _menu;
    QGraphicsItem *_tL, *_tR, *_bR, *_bL;
    bool _refreshOverlays;

    Edge* _hintInputEdge;
    Edge* _hintOutputEdge;

    // A selection's connections cannot change mid-drag, so its splice plan is built on the first
    // move of a press and reused until release; _splicePlanSelection detects a selection change.
    bool _splicePlanBuilt = false;
    NodesGuiList _splicePlanSelection;
    NodeGuiPtr _spliceEntry;
    int _spliceEntryInput = -1;
    NodeGuiPtr _spliceExit;
    Edge* _splicePipe = nullptr;
    // Set by Escape or focus loss, and cleared by the next press, so that the rest of the drag
    // does not rebuild the plan that was just dismissed.
    bool _spliceCancelled = false;

    // An arrow picked up by a click and following the cursor: an input arrow, or a node's output
    // stub. It is kept apart from _evtState so that panning and zooming still run while it is
    // held, and it is held by node and input number, or by node alone for an output stub, rather
    // than as an Edge*, so deleting the node meanwhile leaves nothing dangling.
    NodeGuiWPtr _pickedUpArrowNode;
    int _pickedUpArrowInput = -1;
    bool _pickedUpArrowIsOutput = false;
    PickUpEndEnum _pickedUpArrowEnd = ePickUpEndSource;

    NodeGuiPtr _backdropResized; //< the backdrop being resized
    UndoGesture _dragGesture; //< from a press to its release, so one resize drag is one undo step
    NodesGuiList _selection;

    //To avoid calling unsetCursor too much
    bool cursorSet;
    std::map<NodeGuiPtr, NodesGuiList> _nodesWithinBDAtPenDown;
    QRectF _selectionRect;
    bool _bendPointsVisible;
    bool _knobLinksVisible;
    double _accumDelta;
    bool _detailsVisible;
    QPointF _deltaSinceMousePress; //< mouse delta since last press
    bool _hasMovedOnce;
    ViewerTab* lastSelectedViewer;
    QPixmap unlockIcon;

    ///True when the graph is rendered from the getFullSceneScreenShot() function
    bool isDoingPreviewRender;
    QTimer autoScrollTimer;
    QTimer refreshRenderStateTimer;


    NodeGraphPrivate(NodeGraph* p,
                     const NodeCollectionPtr& group);

    QPoint getPyPlugUnlockPos() const;

    void resetAllClipboards();

    QRectF calcNodesBoundingRect();

    void copyNodesInternal(const NodesGuiList& selection, NodeClipBoard & clipboard);
    void pasteNodesInternal(const NodeClipBoard & clipboard, const QPointF& scenPos,
                            bool useUndoCommand,
                            std::list<std::pair<std::string, NodeGuiPtr> > *newNodes);

    /**
     * @brief Create a new node given the serialization of another one
     * @param sourceNode[in] The node internalSerialization was made from, if known. A clone is slaved to it.
     * @param offset[in] The offset applied to the new node position relative to the serialized node's position.
     * A serialization without a position is placed automatically and not offset.
     **/
    NodeGuiPtr pasteNode(const Serialization::NodeSerializationPtr& internalSerialization,
                         const NodePtr& sourceNode,
                         const QPointF& offset,
                         const NodeCollectionPtr& group,
                         const std::string& parentName,
                         bool clone,
                         std::map<std::string, std::string>* oldNewScriptNamesMapping);

    /**
     * @brief This is called once all nodes of a clipboard have been pasted to try to restore connections between them
     * WARNING: The 2 lists must be ordered the same: each item in serializations corresponds to the same item in the newNodes
     * list. We're not using 2 lists to avoid a copy from the paste function.
     **/
    void restoreConnections(const std::list<Serialization::NodeSerializationPtr>& serializations,
                            const std::list<std::pair<std::string, NodeGuiPtr>>& newNodes,
                            const std::map<std::string, std::string>& oldNewScriptNamesMap);

    void editSelectionFromSelectionRectangle(bool addToSelection);

    void resetSelection();

    void setNodesBendPointsVisible(bool visible);

    bool rearrangeSelectedNodes();

    void toggleSelectedNodesEnabled();

    void getNodeSet(const NodesGuiList& nodeList, std::set<NodeGuiPtr>& nodeSet);
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphPrivate_h
