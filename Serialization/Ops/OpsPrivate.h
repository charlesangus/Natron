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

#ifndef Serialization_Ops_OpsPrivate_h
#define Serialization_Ops_OpsPrivate_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <string>

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace OpsDetail {

    /**
     * @brief A copy of node that shares no child with it.
     **/
    NodeSerializationPtr deepCopyNode(const NodeSerialization& node);

    void stripInputsRecursive(NodeSerialization* node);

    struct IndexedNode {
        NodeSerializationPtr node;
        // Null at the project's top level.
        NodeSerializationPtr parent;
    };

    typedef std::map<std::string, IndexedNode> NodeIdIndex;

    /**
     * @brief Adds nodes and everything below them to index by ID. Nodes without an ID are left out.
     * Returns false when an ID is already in the index or appears twice.
     **/
    bool indexNodes(const std::list<NodeSerializationPtr>& nodes, const NodeSerializationPtr& parent, NodeIdIndex* index);

    std::list<NodeSerializationPtr>& siblingsOf(std::list<NodeSerializationPtr>& topLevel, const NodeSerializationPtr& parent);

    /**
     * @brief An input's source: its ID when it has one, its script name otherwise.
     **/
    struct InputRef {
        std::string sourceId;
        std::string sourceName;

        bool operator==(const InputRef&) const = default;
    };

    typedef std::map<std::string, InputRef> NodeInputRefs;
    typedef std::map<NodeSerialization*, NodeInputRefs> InputRefs;

    /**
     * @brief The inputs of every node below topLevel, each source name looked up among the node's
     * siblings first, then in the enclosing groups: a multi-instance child repeats its parent's
     * inputs, which name the parent's siblings.
     **/
    void resolveInputs(const std::list<NodeSerializationPtr>& topLevel, InputRefs* refs);

} // namespace OpsDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Ops_OpsPrivate_h
