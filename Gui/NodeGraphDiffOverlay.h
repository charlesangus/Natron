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

#ifndef Gui_NodeGraphDiffOverlay_h
#define Gui_NodeGraphDiffOverlay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGraphicsObject>
#include <QLineF>
#include <QRectF>
#include <QString>
#include <QtCore/QObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ProjectMerge.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A node that is not in the live graph, drawn where it was: a removed node of a graph
 * comparison, or a node one side of a merge conflict has. The graph selects it apart from the
 * nodes, so it takes no mouse button itself.
 **/
class NodeDiffGhostItem
    : public QGraphicsObject {
public:
    NodeDiffGhostItem(QGraphicsItem* parent,
                      const std::string& nodeId,
                      const QRectF& rect,
                      const QString& label,
                      const QColor& color);

    const std::string& nodeId() const
    {
        return _nodeId;
    }

    const QRectF& rect() const
    {
        return _rect;
    }

    void setGhostSelected(bool selected);

    bool isGhostSelected() const
    {
        return _selected;
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL;
    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    std::string _nodeId;
    QRectF _rect;
    QString _label;
    QColor _color;
    bool _selected;
};

/**
 * @brief Dashed wires between ghosts and the nodes they were connected to, each in its own colour.
 **/
class NodeDiffWireItem
    : public QGraphicsObject {
public:
    struct Wire {
        QLineF line;
        QColor color;
    };

    explicit NodeDiffWireItem(QGraphicsItem* parent);

    void setWires(std::vector<Wire> wires);

    const std::vector<Wire>& wires() const
    {
        return _wires;
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL;
    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    std::vector<Wire> _wires;
    QRectF _bounds;
};

/**
 * @brief Shows the Gui's graph comparison over one node graph: added nodes tinted green, changed
 * ones amber with a change marker, nodes only moved within their group outlined in amber, and
 * nodes only the compared version has drawn as ghosts the user can select. None of it is saved
 * with the project.
 **/
class NodeGraphDiffOverlay
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum TintEnum {
        eTintNone,
        eTintAdded,
        eTintChanged,
        eTintMoved
    };

    NodeGraphDiffOverlay(NodeGraph* graph,
                         QGraphicsItem* nodeRoot);

    virtual ~NodeGraphDiffOverlay() OVERRIDE;

    // Draws the comparison again once control returns to the event loop.
    void schedule();

    // Call when node may have moved or changed shape.
    void noteNodeChanged(NodeGui* node);

    TintEnum tintOf(const NodeGui* node) const;

    int ghostCount() const;

    // The rectangle of the ghost at index, in the coordinates of node positions, or an empty one
    // past the last.
    QRectF ghostRect(int index) const;

    // The node IDs of the ghosts, in the order of ghostRect().
    std::vector<std::string> ghostIds() const;

    // The rectangle of the ghost of nodeId, empty when it has none.
    QRectF ghostRectOf(const std::string& nodeId) const;

    // The ghost under scenePos, topmost first; empty when there is none.
    std::string ghostIdAt(const QPointF& scenePos) const;

    std::vector<std::string> ghostIdsWithin(const QRectF& sceneRect) const;

    const std::set<std::string>& selectedGhostIds() const
    {
        return _selectedGhosts;
    }

    bool isGhostSelected(const std::string& nodeId) const
    {
        return _selectedGhosts.count(nodeId) != 0;
    }

    // Ignores IDs without a ghost.
    void setGhostSelection(const std::set<std::string>& nodeIds);

    void toggleGhostSelected(const std::string& nodeId);

    void clearGhostSelection();

    // The ID of the group whose nodes the graph shows, empty for the top level.
    static std::string groupIdOf(const NodeGraph* graph);

    // Whether the change is a position within the group and nothing else.
    static bool isOnlyMoved(const ProjectMerge::NodeChange& change);

    static QColor tintColor(TintEnum tint);

    static QColor removedColor();

    // Where the segment between the centres of source and reader leaves each of them.
    static QLineF wireBetween(const QRectF& source, const QRectF& reader);

Q_SIGNALS:
    void ghostSelectionChanged();

public Q_SLOTS:
    void refresh();

private:
    void fitTint(NodeGui* gui);
    void applyGhostSelection();

    NodeGraph* _graph;
    QGraphicsItem* _nodeRoot;
    std::vector<NodeDiffGhostItem*> _ghosts;
    NodeDiffWireItem* _wires;
    std::set<std::string> _selectedGhosts;
    std::unordered_map<const NodeGui*, TintEnum> _tints;
    // The live nodes a ghost has a wire to.
    std::unordered_set<const NodeGui*> _neighbours;
    bool _pending;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphDiffOverlay_h
