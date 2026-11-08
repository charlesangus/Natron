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

#ifndef Serialization_Ops_ApplyOps_h
#define Serialization_Ops_ApplyOps_h

#include "Global/Macros.h"

#include <vector>

#include "Serialization/Ops/GraphOp.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Applies ops in order to project, each one to the result of those before it, and returns
 * one result per operation. An operation that cannot be applied changes nothing and the next ones
 * are still applied. The nodes of project are replaced by copies first, so another
 * ProjectSerialization copied from project, which shares its nodes, is left as it was.
 *
 * Without the plug-ins, some checks the live graph makes are not made here: any knob name and
 * input label is accepted, and a connection is not checked for cycles. A knob reset to its default
 * is removed from the node's knobs, which only lists the knobs that differ from their default.
 * Inputs naming a node that does not exist are kept as they are.
 **/
std::vector<OpResult> applyOps(ProjectSerialization* project, const std::vector<GraphOp>& ops);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Ops_ApplyOps_h
