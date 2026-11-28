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

#ifndef Gui_MergeConflictPreview_h
#define Gui_MergeConflictPreview_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QtCore/QObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ProjectMerge.h"

#include "Gui/GuiFwd.h"

#include "Serialization/Merge/MergeConflict.h"

class QGraphicsItem;
class QGraphicsRectItem;

NATRON_NAMESPACE_ENTER

class NodeDiffGhostItem;
class NodeDiffWireItem;

/**
 * @brief Draws, over one node graph, what each side of merge conflicts would give the graph:
 * dashed wires for the side's connections in the side's colour, the base node of a delete-modify
 * conflict as a ghost, and for an insert on the same link the side's nodes outlined and the other
 * side's dimmed. Shown conflicts are drawn faintly, the hovered side in full. Only input,
 * structural insert, reparent, delete-modify and splice choice conflicts have a preview.
 **/
class MergeConflictPreview
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    MergeConflictPreview(NodeGraph* graph,
                         QGraphicsItem* nodeRoot);

    virtual ~MergeConflictPreview() OVERRIDE;

    static bool hasPreview(Serialization::MergeConflictKind kind);

    static QColor sideColor(ProjectMerge::ConflictSide side);

    // The conflicts, by index in the project's list, whose sides are drawn faintly.
    void setShownConflicts(const std::vector<std::size_t>& indices);

    // Draws side of the conflict at index in full, shown or not.
    void hover(std::size_t index,
               ProjectMerge::ConflictSide side);

    void clearHover();

    // Draws nothing.
    void clear();

    // Whether a drawn ghost or wire end lies under scenePos.
    bool isPickAt(const QPointF& scenePos) const;

    /**
     * @brief Resolves the conflict whose drawn ghost or wire end lies under scenePos with the side
     * it was drawn for, as one undo step. Returns false when there is none; error is set when the
     * side could not be taken.
     **/
    bool pickAt(const QPointF& scenePos,
                QString* error);

    // How many ghosts and wire ends are drawn.
    int pickCount() const;

public Q_SLOTS:
    void redraw();

private:
    struct Pick {
        QRectF area;
        bool round;
        // The conflict itself, found again when picked: its index may have shifted by then.
        Serialization::MergeConflict conflict;
        ProjectMerge::ConflictSide side;
    };

    const Pick* pickUnder(const QPointF& scenePos) const;

    NodeGraph* _graph;
    QGraphicsItem* _nodeRoot;
    std::vector<std::size_t> _shown;
    std::optional<std::pair<std::size_t, ProjectMerge::ConflictSide>> _hovered;
    NodeDiffWireItem* _wires;
    std::vector<NodeDiffGhostItem*> _ghosts;
    std::vector<QGraphicsRectItem*> _marks;
    std::vector<Pick> _picks;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_MergeConflictPreview_h
