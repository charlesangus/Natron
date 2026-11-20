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
 * @brief Old script name to new script name, for nodes that were renamed.
 **/
typedef std::map<std::string, std::string> NodeRenames;

/**
 * @brief expression with each reference to a renamed node rewritten. A reference is an identifier
 * followed by '.', as in Grade1.multiply or thisGroup.Grade1.multiply, so a variable that happens to
 * share a node's name is left alone.
 **/
std::string renameNodesInExpression(std::string_view expression, const NodeRenames& renames);

/**
 * @brief Rewrites the node references of knobs: expressions, and the masters of links, which name
 * the node by script name and by fully qualified name.
 **/
void renameNodeReferences(std::list<KnobSerialization>* knobs, const NodeRenames& renames);

/**
 * @brief renameNodeReferences() on the knobs of node and of every node inside it.
 **/
void renameNodeReferencesRecursive(NodeSerialization* node, const NodeRenames& renames);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_NodeReferences_h
