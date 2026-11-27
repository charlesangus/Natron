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

#ifndef Gui_NodeSourceBar_h
#define Gui_NodeSourceBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>

#include <QWidget>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The row at the top of a panel that shows one node's output, choosing which node: the one
 * the active viewer shows, the candidate last selected in the node graph, or a pinned node. A Pin
 * button pins the node shown now, a Follow chooser picks Viewer or Node Selection, and a node
 * chooser lists the candidates by name, its first item unpinning.
 *
 * The bar holds the choice, not the node's output: the panel reads its target, the pinned node,
 * else the selected node when following the selection, else null for the viewer's node, whenever
 * targetChanged is emitted. Following the selection, a node becomes the target once the selection
 * has stayed put a moment, so that clicking through nodes switches once; selecting no candidate
 * keeps the target.
 *
 * Scripts find its widgets by object name: the prefix given followed by Pin, Follow and Node.
 **/
class NodeSourceBar
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum FollowEnum {
        eFollowViewer = 0,
        eFollowSelection
    };

    struct ToolTips {
        QString pin;
        QString follow;
        QString node;
    };

    NodeSourceBar(Gui* gui,
                  const QString& objectNamePrefix,
                  const std::function<bool(const NodePtr&)>& isCandidate,
                  const ToolTips& toolTips,
                  QWidget* parent);

    ~NodeSourceBar() override;

    // The node the panel shows now, which the Pin button pins.
    void setShownNode(const std::function<NodePtr()>& shownNode);

    // Shows node whichever viewer is active and whatever is selected; null follows again.
    void pinNode(const NodePtr& node);

    NodePtr getPinnedNode() const;

    // Whether a node is pinned, true still when the pinned node has been destroyed.
    bool isPinned() const;

    void setFollow(FollowEnum follow);

    FollowEnum getFollow() const;

    // "viewer" or "selection", as panels publish it to scripts.
    static QString followName(FollowEnum follow);

    // The pinned node, else the selected node when following the selection, else null.
    NodePtr getTarget() const;

    // The candidate last selected in the node graph, null when none is selected.
    NodePtr selectedCandidate() const;

    // Unpins and forgets the selected node without emitting targetChanged.
    void release();

    // The target was deleted or deactivated: unpins, or forgets the selected node.
    void dropTarget();

    // Whether a selection change waits to be read.
    bool isSettling() const;

Q_SIGNALS:

    void targetChanged();

    // The selection started or stopped waiting to be read, without changing the target.
    void settlingChanged();

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeSourceBar_h
