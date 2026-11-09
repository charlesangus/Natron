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

#ifndef Engine_GraphOpApply_h
#define Engine_GraphOpApply_h

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Serialization/Ops/GraphOp.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Applies ops in order to the open project, through the calls the node graph and project
 * loading make, and returns one result per operation. An operation that fails changes nothing
 * and does not stop the ones after it. Nodes are found by their persistent ID, and a created node
 * and its children take the IDs the operation gives them. The edits are not recorded as graph
 * edits. Main thread only. Knob links are made after the last operation, so that a link may
 * name a node or project variable that a later operation in ops makes.
 *
 * Differences from Serialization::applyOps(): the plug-ins are known here, so an unknown plug-in,
 * knob or input label and a connection the node refuses (a cycle) are invalid. Clearing an
 * overlay colour is not shown by a node graph already displaying it. The project's additional
 * formats can only be appended to, and a layer that a node uses cannot be removed. A node always
 * has a position, so clearing it keeps the one it has.
 **/
std::vector<Serialization::OpResult> applyGraphOps(const ProjectPtr& project,
                                                   const std::vector<Serialization::GraphOp>& ops);

NATRON_NAMESPACE_EXIT

#endif // Engine_GraphOpApply_h
