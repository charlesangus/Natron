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

#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsPixmapItem>
#include <QImage>
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
#define NATRON_NAVIGATOR_RENDER_DELAY_MS 250
// Matches the height below which a node's name text stops being drawn.
#define NATRON_NODEGRAPH_LOW_DETAIL_TEXT_HEIGHT_PX 4

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

// A list of nodes in insertion order that finds and removes a node in constant time. A node is
// in it at most once.
class IndexedNodeGuiList {
public:
    typedef NodesGuiList::iterator iterator;
    typedef NodesGuiList::const_iterator const_iterator;
    typedef NodesGuiList::const_reverse_iterator const_reverse_iterator;

    operator const NodesGuiList&() const { return _list; }
    const NodesGuiList& list() const { return _list; }

    iterator begin() { return _list.begin(); }
    iterator end() { return _list.end(); }
    const_iterator begin() const { return _list.begin(); }
    const_iterator end() const { return _list.end(); }
    const_reverse_iterator rbegin() const { return _list.rbegin(); }
    const_reverse_iterator rend() const { return _list.rend(); }

    bool empty() const { return _list.empty(); }
    std::size_t size() const { return _list.size(); }
    const NodeGuiPtr& front() const { return _list.front(); }
    const NodeGuiPtr& back() const { return _list.back(); }

    bool contains(const NodeGui* node) const
    {
        return _index.find(const_cast<NodeGui*>(node)) != _index.end();
    }

    bool contains(const NodeGuiPtr& node) const { return contains(node.get()); }

    // Returns false, leaving the order alone, if the node is already in the list.
    bool push_back(const NodeGuiPtr& node)
    {
        if (!node || contains(node)) {
            return false;
        }
        _list.push_back(node);
        _index[node.get()] = --_list.end();

        return true;
    }

    iterator erase(iterator it)
    {
        _index.erase(it->get());

        return _list.erase(it);
    }

    // Returns the removed node, or a null pointer if the node was not in the list.
    NodeGuiPtr remove(const NodeGui* node)
    {
        std::unordered_map<NodeGui*, iterator>::iterator found = _index.find(const_cast<NodeGui*>(node));
        if (found == _index.end()) {
            return NodeGuiPtr();
        }
        NodeGuiPtr ret = *found->second;
        _list.erase(found->second);
        _index.erase(found);

        return ret;
    }

    void clear()
    {
        _list.clear();
        _index.clear();
    }

private:
    NodesGuiList _list;
    std::unordered_map<NodeGui*, iterator> _index;
};

// The navigator overlay. It paints its own border and nothing at all while the graph renders its
// thumbnail, so the scene can be rendered without taking the overlay out of it.
class Navigator
    : public QGraphicsPixmapItem
{
    const bool* _hiddenDuringRender;
    QPen _borderPen;
    double _width;
    double _height;

public:
    explicit Navigator(const bool* hiddenDuringRender)
        : QGraphicsPixmapItem(0)
        , _hiddenDuringRender(hiddenDuringRender)
        , _borderPen()
        , _width(0.)
        , _height(0.)
    {
        _borderPen.setBrush(QColor(200, 200, 200));
        _borderPen.setWidth(2);
    }

    virtual ~Navigator()
    {
    }

    void refreshPosition(const QPointF & navTopLeftScene,
                         double width,
                         double height)
    {
        if ((width != _width) || (height != _height)) {
            prepareGeometryChange();
            _width = width;
            _height = height;
        }
        setPos(navTopLeftScene);
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL
    {
        const double m = _borderPen.widthF() / 2.;

        return QGraphicsPixmapItem::boundingRect().united(QRectF(0, 0, _width, _height)).adjusted(-m, -m, m, m);
    }

    virtual QPainterPath shape() const OVERRIDE FINAL
    {
        QPainterPath path;
        path.addRect(boundingRect());

        return path;
    }

    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL
    {
        if (*_hiddenDuringRender) {
            return;
        }
        QGraphicsPixmapItem::paint(painter, option, widget);
        painter->setPen(_borderPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawLine(QPointF(0, _height), QPointF(0, 0));
        painter->drawLine(QPointF(0, 0), QPointF(_width, 0));
        painter->drawLine(QPointF(_width, 0), QPointF(_width, _height));
        painter->drawLine(QPointF(_width, _height), QPointF(0, _height));
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
    IndexedNodeGuiList _nodes;
    IndexedNodeGuiList _nodesTrash;

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
    IndexedNodeGuiList _selection; //< in the order the nodes were selected

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

    /// True while the scene is rendered into the navigator thumbnail
    bool isDoingPreviewRender;
    QTimer autoScrollTimer;
    QTimer refreshRenderStateTimer;

    // The navigator shows a thumbnail of the nodes rendered once per graph change, at the latest
    // after navigatorRenderTimer fires; panning and zooming only recompose it with the view
    // rectangle. Both rectangles are in _root coordinates, so that panning, which moves _root,
    // leaves them valid.
    QImage navigatorContent;
    QRectF navigatorContentRect;
    bool navigatorContentDirty;
    QTimer navigatorRenderTimer;
    QRectF nodesBoundsInRoot;
    bool nodesBoundsDirty;
    bool antiAliasing;
    bool lowDetail;
    int lowDetailFontHeight;

    int stackingBatchDepth;
    bool loadStackingBatchOpen;
    std::unordered_map<NodeGui*, NodeGuiWPtr> frameDependentNodes;
    bool timelineConnected;
    std::vector<NodeGuiWPtr> stackingPending;
    std::unordered_set<NodeGui*> stackingPendingSet;

    // Nodes whose rendering counters or persistent message changed since refreshRenderStateTimer
    // last fired, keyed by address so that a node queued twice is refreshed once.
    std::map<NodeGui*, NodeGuiWPtr> renderStateDirty;

    NodeGraphPrivate(NodeGraph* p,
                     const NodeCollectionPtr& group);

    QPoint getPyPlugUnlockPos() const;

    void resetAllClipboards();

    // In scene coordinates. Cached until a node moves, resizes, shows, hides, or an edge changes.
    QRectF calcNodesBoundingRect();

    // The scene rectangle the navigator maps onto its widget-sized image, and the scale it uses.
    void getNavigatorMapping(QRectF* sceneRect, double* scale, QPointF* offset) const;
    void renderNavigatorContent();

    void copyNodesInternal(const NodesGuiList& selection, NodeClipBoard & clipboard);
    void pasteNodesInternal(const NodeClipBoard & clipboard, const QPointF& scenPos,
                            bool useUndoCommand,
                            std::list<std::pair<std::string, NodeGuiPtr> > *newNodes);

    /**
     * @brief Create a new node given the serialization of another one
     * @param sourceNode[in] The node internalSerialization was made from, if known.
     * @param offset[in] The offset applied to the new node position relative to the serialized node's position.
     * A serialization without a position is placed automatically and not offset.
     * @param linkedNodes[out] If not null, each new node that is to become a Linked Node of its source node,
     * children of a group included, is appended to it with that source node, for the caller to link once
     * every node is created. Dots are copied but not linked.
     **/
    NodeGuiPtr pasteNode(const Serialization::NodeSerializationPtr& internalSerialization,
                         const NodePtr& sourceNode,
                         const QPointF& offset,
                         const NodeCollectionPtr& group,
                         const std::string& parentName,
                         std::list<std::pair<NodePtr, NodePtr>>* linkedNodes,
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

    // Makes nodes, in their order, the selection.
    void replaceSelection(const NodesGuiList& nodes);

    void setNodesBendPointsVisible(bool visible);

    bool rearrangeSelectedNodes();

    void toggleSelectedNodesEnabled();

    void getNodeSet(const NodesGuiList& nodeList, std::set<NodeGuiPtr>& nodeSet);
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphPrivate_h
