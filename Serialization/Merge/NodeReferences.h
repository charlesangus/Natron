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

#ifndef Serialization_Merge_NodeReferences_h
#define Serialization_Merge_NodeReferences_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <string>
#include <string_view>

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Old script name to new script name, for nodes of one group that were renamed.
 **/
typedef std::map<std::string, std::string> NodeRenames;

/**
 * @brief Renames by the group the renamed nodes are children of, keyed by that group's fully
 * qualified name before the renames: script names from the top level down, joined by '.', empty
 * for the top level. Siblings only clash within a group, so a rename never reaches a namesake in
 * another group.
 **/
typedef std::map<std::string, NodeRenames> ScopedNodeRenames;

/**
 * @brief expression, written by a node inside group, with each reference to a renamed node
 * rewritten. A reference is a dotted chain of names, each followed by '.': it starts at a name of
 * group's children, at thisGroup (group itself) or at app or appN (the top level), and each next
 * name is a child of the node before it, as in Grade1.multiply or thisGroup.Group2.Blur1.size. A
 * variable that happens to share a node's name, an attribute after another '.' and text inside a
 * string literal are left alone.
 **/
std::string renameNodesInExpression(std::string_view expression, const ScopedNodeRenames& renames, const std::string& group);

/**
 * @brief Rewrites the node references of the knobs of a node inside group: expressions, and the
 * masters of links, which name the node by script name and by fully qualified name.
 **/
void renameNodeReferences(std::list<KnobSerialization>* knobs, const ScopedNodeRenames& renames, const std::string& group);

/**
 * @brief renameNodeReferences() on the knobs of node, a child of group, and of every node inside
 * it. Group names are taken from the nodes as they are, so call it before renaming them.
 **/
void renameNodeReferencesRecursive(NodeSerialization* node, const ScopedNodeRenames& renames, const std::string& group);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_NodeReferences_h
