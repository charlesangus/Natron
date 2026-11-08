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

#ifndef Serialization_Ops_SnapshotDiff_h
#define Serialization_Ops_SnapshotDiff_h

#include "Global/Macros.h"

#include <vector>

#include "Serialization/Ops/GraphOp.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The operations that turn before into after: editableState(applyOps(before, ops)) equals
 * editableState(after), up to the order of knobs (see applyOps()). Identical snapshots give no
 * operation, and the same pair always gives the same operations.
 *
 * Nodes are matched by ID; a node without an ID is not compared. A node whose plug-in, plug-in
 * version, Python module or parent changed is deleted and created again with its ID. Operations
 * come grouped, in this order: deletions; renames, through a temporary name when two nodes swap
 * names; creations, a group before what it holds; per node, its user knobs, knobs, appearance,
 * roto and tracker data; the project's knobs, then its formats, layers and variables; connections.
 * A multi-instance child gets no connection of its own: it takes its main instance's.
 **/
std::vector<GraphOp> diffSnapshots(const ProjectSerialization& before, const ProjectSerialization& after);

/**
 * @brief project without what graph operations do not carry, which is not part of an edit:
 * the saving version, project ID and creation date describe the file; the timeline position is
 * where each session's playhead is, and moves on every frame of playback; cache IDs are issued
 * again on each open; knob ages count changes within one session. Inputs whose source is not a
 * node with an ID are dropped too: an operation names a source by its ID, and a live graph cannot
 * hold a connection to a node that does not exist. The nodes are copies, sharing nothing with
 * project.
 **/
ProjectSerialization editableState(const ProjectSerialization& project);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Ops_SnapshotDiff_h
