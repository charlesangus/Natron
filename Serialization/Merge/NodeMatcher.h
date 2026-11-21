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

#ifndef Serialization_Merge_NodeMatcher_h
#define Serialization_Merge_NodeMatcher_h

#include "Global/Macros.h"

#include <map>
#include <set>
#include <string>
#include <utility>

#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief One snapshot of a three-way merge, indexed for matching. Nodes without an ID are left
 * out. The project must outlive the graph.
 **/
class MergeGraph {
public:
    struct Node {
        NodeSerializationPtr node;
        // Empty at the project's top level.
        std::string parentId;
        // Script names from the top level down, joined by '.'.
        std::string path;
        // Input label to source node ID, for the inputs whose source has an ID.
        std::map<std::string, std::string> inputs;
        // (consumer node ID, input label) for every input this node feeds.
        std::set<std::pair<std::string, std::string>> outputs;
    };

    explicit MergeGraph(const ProjectSerialization& project);

    const Node* find(const std::string& id) const;
    const Node* findByPath(const std::string& path) const;
    const std::map<std::string, Node>& nodes() const
    {
        return _nodes;
    }

private:
    std::map<std::string, Node> _nodes;
    std::map<std::string, std::string> _idByPath;
};

/**
 * @brief Which node of another snapshot stands for each node of the base.
 **/
struct NodeCorrespondence {
    // Base node ID to the other side's node ID. A base node absent here has no counterpart.
    std::map<std::string, std::string> baseToOther;
    // The other-side nodes matched by structure rather than by identity.
    std::set<std::string> adopted;

    // The other side's node for a base node, or empty.
    std::string otherFor(const std::string& baseId) const;
    // The base node an other-side node stands for, or empty.
    std::string baseFor(const std::string& otherId) const;
    // Every other-side node that stands for a base node.
    std::set<std::string> matchedOther() const;
};

/**
 * @brief Matches the nodes of a base against another snapshot of it. With templateKey empty, other
 * is a version of base itself, whose nodes keep base's IDs: the template as the supervisor revised
 * it, or another version of the same project. Otherwise other is a shot the template was imported
 * into as templateKey, whose nodes have fresh IDs and carry an origin. Matching is deterministic,
 * works across the whole tree (a node moved into or out of a group is still the same node), and no
 * other-side node stands for two base nodes.
 *
 * In a shot, a node whose origin names templateKey claims the base node the origin names; any other
 * node claims the base node with its own ID. A base node claimed by several nodes goes to the one
 * whose neighbours agree with it best, when one does. Then an unmatched base node adopts an
 * unmatched node without an origin that has the same plug-in, the same parent, and the same matched
 * upstream and downstream neighbours on the same input labels, when each is the other's only
 * candidate. In a shot the adopted node must also lie in the merge scope of the nodes matched so
 * far, so a node of the artist's own branch next to the template is never adopted. Adoption repeats
 * until nothing more is adopted. A merge writes an origin onto each adopted node it keeps, so the
 * next match finds it by identity.
 **/
NodeCorrespondence matchNodes(const MergeGraph& base,
                              const MergeGraph& other,
                              const std::string& templateKey);

/**
 * @brief The node with the given ID anywhere in the tree, or null.
 **/
NodeSerializationPtr findNodeSerialization(const std::list<NodeSerializationPtr>& nodes,
                                           const std::string& id);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_NodeMatcher_h
