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

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsObject>
#include <QLineF>
#include <QRectF>
#include <QString>
#include <QtCore/QObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The removed nodes of a graph comparison, drawn where they belong among the live nodes,
 * with dashed wires to the nodes they were connected to. It takes no mouse button, so nothing of it can be selected.
 **/
class NodeDiffGhostItem
    : public QGraphicsObject {
public:
    struct Ghost {
        QRectF rect;
        QString label;
    };

    explicit NodeDiffGhostItem(QGraphicsItem* parent);

    void setShapes(std::vector<Ghost> ghosts, std::vector<QLineF> wires);

    std::size_t ghostCount() const
    {
        return _ghosts.size();
    }

    const std::vector<Ghost>& ghosts() const
    {
        return _ghosts;
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL;
    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    std::vector<Ghost> _ghosts;
    std::vector<QLineF> _wires;
    QRectF _bounds;
};

/**
 * @brief Shows the Gui's graph comparison over one node graph: added nodes tinted green, changed
 * ones amber, nodes only moved within their group outlined in amber, and removed ones as ghosts. Nothing of it is saved with the project.
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

    // The ID of the group whose nodes the graph shows, empty for the top level.
    static std::string groupIdOf(const NodeGraph* graph);

public Q_SLOTS:
    void refresh();

private:
    void fitTint(NodeGui* gui);

    NodeGraph* _graph;
    NodeDiffGhostItem* _ghosts;
    std::unordered_map<const NodeGui*, TintEnum> _tints;
    // The live nodes a ghost has a wire to.
    std::unordered_set<const NodeGui*> _neighbours;
    bool _pending;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphDiffOverlay_h
