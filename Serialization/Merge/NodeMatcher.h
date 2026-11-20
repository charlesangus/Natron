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
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Where a node brought in by a template import came from: the import's key, and the ID
 * the node has in that template.
 **/
struct NodeOrigin {
    std::string templateKey;
    std::string nodeId;
    bool operator==(const NodeOrigin&) const = default;
};

/**
 * @brief Node ID to origin, for the nodes of one snapshot that carry one.
 **/
typedef std::map<std::string, NodeOrigin> NodeOrigins;

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
        // Empty when the node carries no origin.
        NodeOrigin origin;
    };

    MergeGraph(const ProjectSerialization& project, NodeOrigins origins = NodeOrigins());

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
    // Every other-side node that stands for a base node.
    std::set<std::string> matchedOther() const;
};

/**
 * @brief A policy for matching the nodes of a template base against another snapshot of it:
 * the supervisor's new version, whose nodes keep their template IDs, or a shot the template was
 * imported into, whose nodes have fresh IDs and carry an origin. Matching is deterministic, and
 * no other-side node stands for two base nodes.
 **/
class NodeMatcher {
public:
    virtual ~NodeMatcher();

    virtual std::string_view name() const = 0;

    /**
     * @brief templateKey selects the import being merged: an origin naming another import never
     * matches.
     **/
    virtual NodeCorrespondence match(const MergeGraph& base,
                                     const MergeGraph& other,
                                     const std::string& templateKey) const
        = 0;
};

/**
 * @brief A node matches the base node its origin names, or, without an origin, the base node with
 * its own ID. A base node claimed by more than one node is left unmatched.
 **/
class IdOnlyMatcher
    : public NodeMatcher {
public:
    std::string_view name() const override
    {
        return "IdOnly";
    }

    NodeCorrespondence match(const MergeGraph& base, const MergeGraph& other, const std::string& templateKey) const override;
};

/**
 * @brief IdOnly, then two structural steps. A base node claimed by several nodes goes to the one
 * whose neighbours agree with it best, when one does. Then an unmatched base node adopts an
 * unmatched node without an origin that has the same plug-in, the same parent, and the same
 * matched upstream and downstream neighbours on the same input labels, when each is the other's
 * only candidate. Adoption repeats until nothing more is adopted.
 **/
class IdPlusAdoptionMatcher
    : public NodeMatcher {
public:
    std::string_view name() const override
    {
        return "IdPlusAdoption";
    }

    NodeCorrespondence match(const MergeGraph& base, const MergeGraph& other, const std::string& templateKey) const override;
};

/**
 * @brief Matches nodes by path, ignoring IDs and origins.
 **/
class NameBasedMatcher
    : public NodeMatcher {
public:
    std::string_view name() const override
    {
        return "NameBased";
    }

    NodeCorrespondence match(const MergeGraph& base, const MergeGraph& other, const std::string& templateKey) const override;
};

/**
 * @brief Candidate rules for which nodes of a shot a merge of one template import may touch.
 **/
enum class MergeScopeRule {
    // The nodes whose origin names the import.
    eOriginOnly,
    // Those, plus the nodes without one that lie on a path from one of them to another.
    eEnclosed,
    // The nodes that stand for a base node under IdPlusAdoption, plus the nodes that lie on a path
    // from one of them to another.
    eMatchedEnclosed,
    // The nodes whose origin names the import, plus everything wired to them, directly or not.
    eConnected,
};

std::string_view mergeScopeRuleName(MergeScopeRule rule);

const std::vector<MergeScopeRule>& allMergeScopeRules();

/**
 * @brief The IDs of the shot nodes in scope for the import templateKey. base is that import's
 * template; only eMatchedEnclosed reads it. The children of an in-scope group are in scope.
 **/
std::set<std::string> mergeScope(const MergeGraph& base,
                                 const MergeGraph& mine,
                                 const std::string& templateKey,
                                 MergeScopeRule rule);

/**
 * @brief The three strategies, in the order the scoring table shows them.
 **/
std::vector<std::unique_ptr<NodeMatcher>> prototypeNodeMatchers();

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_NodeMatcher_h
