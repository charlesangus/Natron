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

#ifndef Engine_OverlayPath_h
#define Engine_OverlayPath_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How an overlay node sits in the graph relative to the node a viewer shows.
 * Downstream: the viewed node reads the overlay node. Upstream: the overlay node reads the
 * viewed node. Sibling: neither reads the other but both read a shared ancestor. Unrelated:
 * no shared ancestor.
 **/
enum class OverlayPathRelation {
    eDownstream,
    eUpstream,
    eSibling,
    eUnrelated
};

/**
 * @brief Where an overlay is drawn in a viewer and at which of its node's frames.
 * matrix maps the overlay node's canonical space to the viewed node's canonical space; time is
 * the overlay node's frame matching the viewer's; visible is false when the overlay cannot be
 * placed at this frame (the legs that have to be inverted collapse the plane onto a line or a
 * point, as a Transform scale of 0 does, or a frame hold holds another frame), and then it is
 * neither drawn nor sent events.
 **/
struct OverlayPathResult {
    OverlayPathRelation relation = OverlayPathRelation::eUnrelated;
    Transform::Matrix3x3 matrix;
    double time = 0.;
    bool visible = true;
};

/**
 * @brief Places overlayNode's overlay in a viewer showing viewed at time and view.
 *
 * viewed may be a viewer node. It then shows its active A input, and its B input too while it
 * wipes or compares; the overlay is placed through whichever of the two it is closer to
 * (downstream, then upstream, then sibling), A on a tie, and its other inputs play no part.
 * Every walk tries a node's main input (Node::getPreferredInput, a Merge's B) first, then the
 * others non-optional before optional, last to first, depth first, and takes the first route that
 * order reaches, however long:
 * - downstream when the walk from the viewed node reaches the overlay node;
 * - otherwise upstream when the walk from the overlay node reaches the viewed node;
 * - otherwise sibling, through the first node the walk from the overlay node reaches that the
 *   viewed node also reads, which is the shared ancestor;
 * - otherwise unrelated.
 * The graph path between the two nodes composes each node's getOverlayTransform matrix (draft
 * mode) and maps time through each node's getFramesNeeded: forward along the legs the viewer
 * reads, inverted along the legs that lead from the overlay node down to a shared ancestor. The
 * overlay is drawn in the space of the overlay node's input, so the overlay node's own matrix is
 * never part of the path, though its own retime is. A node whose needed frame is one constant
 * frame is a hold, visible only while the time reaching it is that frame.
 * Disabled nodes are identity, and a Group overlay node resolves through its output node's input.
 * With mapSiblings false a sibling overlay stays in its own canonical space, as an unrelated one
 * always does.
 **/
OverlayPathResult resolveOverlayPath(EffectInstance* viewed,
                                     const NodePtr& overlayNode,
                                     double time,
                                     ViewIdx view,
                                     bool mapSiblings);

/**
 * @brief Remembers the last resolveOverlayPath result per overlay node, so a paint or an event
 * resolves each overlay once. An entry is reused only for the same viewed node, the same nodes it
 * displays, time, view and mapSiblings, and while neither the viewed node's nor the overlay's resolved node's hash mark
 * count has moved: every node a path can cross is upstream of one of the two, and a connection,
 * a node removal or a render knob edit there moves that count. Not thread-safe.
 **/
class OverlayPathCache {
public:
    OverlayPathResult resolve(EffectInstance* viewed,
                              const NodePtr& overlayNode,
                              double time,
                              ViewIdx view,
                              bool mapSiblings);

    void clear();

private:
    struct Entry {
        NodeWPtr overlayNode;
        NodeWPtr resolvedNode;
        EffectInstance* viewed = nullptr;
        std::vector<EffectInstance*> displayed;
        U64 viewedMarks = 0;
        U64 resolvedMarks = 0;
        double time = 0.;
        ViewIdx view;
        bool mapSiblings = true;
        OverlayPathResult result;
    };

    std::map<const Node*, Entry> _entries;
};

/**
 * @brief Maps a position in the viewed node's canonical space into the overlay node's, through
 * the inverse of result.matrix. False when that matrix collapses the plane or the position maps
 * behind the projection; with keepBehind a position behind it is reflected and still mapped, for
 * events such as a pen release that the overlay must receive.
 **/
bool mapToOverlaySpace(const OverlayPathResult& result,
                       double x,
                       double y,
                       bool keepBehind,
                       double* overlayX,
                       double* overlayY);

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayPath_h
