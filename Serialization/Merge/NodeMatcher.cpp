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

#include "Serialization/Merge/NodeMatcher.h"

#include <deque>
#include <vector>

#include "Serialization/Ops/OpsPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    void
    indexNodes(const std::list<NodeSerializationPtr>& nodes,
               const std::string& parentId,
               const std::string& parentPath,
               const OpsDetail::InputRefs& refs,
               std::map<std::string, MergeGraph::Node>* out,
               std::map<std::string, std::string>* idByPath)
    {
        for (const NodeSerializationPtr& node : nodes) {
            const std::string path = parentPath.empty() ? node->scriptName : parentPath + '.' + node->scriptName;
            if (!node->id.empty()) {
                MergeGraph::Node entry;
                entry.node = node;
                entry.parentId = parentId;
                entry.path = path;
                OpsDetail::InputRefs::const_iterator inputs = refs.find(node.get());
                if (inputs != refs.end()) {
                    for (const std::pair<const std::string, OpsDetail::InputRef>& input : inputs->second) {
                        if (!input.second.sourceId.empty()) {
                            entry.inputs[input.first] = input.second.sourceId;
                        }
                    }
                }
                out->emplace(node->id, std::move(entry));
                idByPath->emplace(path, node->id);
            }
            indexNodes(node->children, node->id, path, refs, out, idByPath);
        }
    }

    std::map<std::string, std::vector<std::string>>
    collectClaims(const MergeGraph& base,
                  const MergeGraph& other,
                  const std::string& templateKey)
    {
        std::map<std::string, std::vector<std::string>> claims;
        for (const std::pair<const std::string, MergeGraph::Node>& entry : other.nodes()) {
            const NodeOriginSerialization& origin = entry.second.node->origin;
            const std::string& claimed = (origin.isSet() && (origin.templateKey == templateKey)) ? origin.nodeId : entry.first;
            if (base.find(claimed)) {
                claims[claimed].push_back(entry.first);
            }
        }

        return claims;
    }

    NodeCorrespondence
    matchUniqueClaims(const std::map<std::string, std::vector<std::string>>& claims)
    {
        NodeCorrespondence result;
        for (const std::pair<const std::string, std::vector<std::string>>& claim : claims) {
            if (claim.second.size() == 1) {
                result.baseToOther[claim.first] = claim.second.front();
            }
        }

        return result;
    }

    struct Agreement {
        int agreeing = 0;
        // Every neighbour of the base node is matched and every edge agrees, both ways.
        bool exact = true;
    };

    Agreement
    neighbourAgreement(const MergeGraph::Node& baseNode,
                       const MergeGraph::Node& otherNode,
                       const NodeCorrespondence& correspondence,
                       const std::set<std::string>& matchedOther)
    {
        Agreement agreement;

        std::map<std::string, std::string> upstreamBase;
        for (const std::pair<const std::string, std::string>& input : baseNode.inputs) {
            const std::string mapped = correspondence.otherFor(input.second);
            if (mapped.empty()) {
                agreement.exact = false;
            } else {
                upstreamBase[input.first] = mapped;
            }
        }
        std::map<std::string, std::string> upstreamOther;
        for (const std::pair<const std::string, std::string>& input : otherNode.inputs) {
            if (matchedOther.count(input.second)) {
                upstreamOther[input.first] = input.second;
            }
        }
        for (const std::pair<const std::string, std::string>& input : upstreamBase) {
            std::map<std::string, std::string>::const_iterator found = upstreamOther.find(input.first);
            if ((found != upstreamOther.end()) && (found->second == input.second)) {
                ++agreement.agreeing;
            } else {
                agreement.exact = false;
            }
        }
        for (const std::pair<const std::string, std::string>& input : upstreamOther) {
            if (!upstreamBase.count(input.first)) {
                agreement.exact = false;
            }
        }

        std::set<std::pair<std::string, std::string>> downstreamBase;
        for (const std::pair<std::string, std::string>& output : baseNode.outputs) {
            const std::string mapped = correspondence.otherFor(output.first);
            if (mapped.empty()) {
                agreement.exact = false;
            } else {
                downstreamBase.emplace(mapped, output.second);
            }
        }
        std::set<std::pair<std::string, std::string>> downstreamOther;
        for (const std::pair<std::string, std::string>& output : otherNode.outputs) {
            if (matchedOther.count(output.first)) {
                downstreamOther.insert(output);
            }
        }
        for (const std::pair<std::string, std::string>& output : downstreamBase) {
            if (downstreamOther.count(output)) {
                ++agreement.agreeing;
            } else {
                agreement.exact = false;
            }
        }
        for (const std::pair<std::string, std::string>& output : downstreamOther) {
            if (!downstreamBase.count(output)) {
                agreement.exact = false;
            }
        }

        if (agreement.agreeing == 0) {
            agreement.exact = false;
        }

        return agreement;
    }

    void
    resolveAmbiguousClaims(const MergeGraph& base,
                           const MergeGraph& other,
                           const std::map<std::string, std::vector<std::string>>& claims,
                           NodeCorrespondence* correspondence)
    {
        for (const std::pair<const std::string, std::vector<std::string>>& claim : claims) {
            if (claim.second.size() < 2) {
                continue;
            }
            const std::set<std::string> matchedOther = correspondence->matchedOther();
            int best = 0;
            int bestCount = 0;
            std::string bestId;
            for (const std::string& candidate : claim.second) {
                const int agreeing = neighbourAgreement(*base.find(claim.first), *other.find(candidate), *correspondence, matchedOther).agreeing;
                if (agreeing > best) {
                    best = agreeing;
                    bestCount = 1;
                    bestId = candidate;
                } else if ((agreeing == best) && (agreeing > 0)) {
                    ++bestCount;
                }
            }
            if (bestCount == 1) {
                correspondence->baseToOther[claim.first] = bestId;
            }
        }
    }

    // The matched nodes reached from start against the edges, passing through unmatched nodes.
    struct UpstreamReach {
        std::string direct;
        std::set<std::string> through;
    };

    UpstreamReach
    upstreamReach(const MergeGraph& graph,
                  const std::string& start,
                  const std::set<std::string>& matched)
    {
        UpstreamReach reach;
        if (start.empty()) {
            return reach;
        }
        if (matched.count(start)) {
            reach.direct = start;

            return reach;
        }
        std::set<std::string> seen;
        std::deque<std::string> queue(1, start);
        while (!queue.empty()) {
            const std::string id = queue.front();
            queue.pop_front();
            const MergeGraph::Node* node = graph.find(id);
            if (!node || !seen.insert(id).second) {
                continue;
            }
            for (const std::pair<const std::string, std::string>& input : node->inputs) {
                if (matched.count(input.second)) {
                    reach.through.insert(input.second);
                } else {
                    queue.push_back(input.second);
                }
            }
        }

        return reach;
    }

    // The (consumer, input label) edges into matched nodes, along the edges from node, passing
    // through unmatched consumers.
    struct DownstreamReach {
        std::set<std::pair<std::string, std::string>> direct;
        std::set<std::pair<std::string, std::string>> through;
    };

    DownstreamReach
    downstreamReach(const MergeGraph& graph,
                    const MergeGraph::Node& node,
                    const std::set<std::string>& matched)
    {
        DownstreamReach reach;
        std::set<std::string> seen;
        std::deque<std::string> queue;
        for (const std::pair<std::string, std::string>& output : node.outputs) {
            if (matched.count(output.first)) {
                reach.direct.insert(output);
            } else {
                queue.push_back(output.first);
            }
        }
        while (!queue.empty()) {
            const std::string id = queue.front();
            queue.pop_front();
            const MergeGraph::Node* consumer = graph.find(id);
            if (!consumer || !seen.insert(id).second) {
                continue;
            }
            for (const std::pair<std::string, std::string>& output : consumer->outputs) {
                if (matched.count(output.first)) {
                    reach.through.insert(output);
                } else {
                    queue.push_back(output.first);
                }
            }
        }

        return reach;
    }

    // Whether candidate is wired as baseNode is along every matched neighbour of baseNode, looking
    // through the unmatched nodes between them, with at least one such neighbour, and never directly
    // to a matched node baseNode is not wired to.
    bool
    wiredAsBase(const MergeGraph& other,
                const MergeGraph::Node& baseNode,
                const MergeGraph::Node& candidate,
                const NodeCorrespondence& correspondence,
                const std::set<std::string>& matchedOther)
    {
        int agreeing = 0;

        std::set<std::string> labels;
        for (const std::pair<const std::string, std::string>& input : baseNode.inputs) {
            labels.insert(input.first);
        }
        for (const std::pair<const std::string, std::string>& input : candidate.inputs) {
            labels.insert(input.first);
        }
        for (const std::string& label : labels) {
            std::map<std::string, std::string>::const_iterator baseInput = baseNode.inputs.find(label);
            std::map<std::string, std::string>::const_iterator otherInput = candidate.inputs.find(label);
            const std::string mapped = (baseInput == baseNode.inputs.end()) ? std::string() : correspondence.otherFor(baseInput->second);
            const UpstreamReach reach = upstreamReach(other, (otherInput == candidate.inputs.end()) ? std::string() : otherInput->second, matchedOther);
            if (!mapped.empty()) {
                if ((reach.direct != mapped) && !(reach.direct.empty() && (reach.through == std::set<std::string> { mapped }))) {
                    return false;
                }
                ++agreeing;
            } else if (!reach.direct.empty() || (!reach.through.empty() && (baseInput == baseNode.inputs.end()))) {
                // Reaching past unmatched nodes is only fine where an unmatched base source may stand
                // for them.
                return false;
            }
        }

        std::set<std::pair<std::string, std::string>> baseDownstream;
        bool unmatchedConsumer = false;
        for (const std::pair<std::string, std::string>& output : baseNode.outputs) {
            const std::string mapped = correspondence.otherFor(output.first);
            if (mapped.empty()) {
                unmatchedConsumer = true;
            } else {
                baseDownstream.emplace(mapped, output.second);
            }
        }
        const DownstreamReach reach = downstreamReach(other, candidate, matchedOther);
        for (const std::pair<std::string, std::string>& output : baseDownstream) {
            if (!reach.direct.count(output) && !reach.through.count(output)) {
                return false;
            }
            ++agreeing;
        }
        for (const std::pair<std::string, std::string>& output : reach.direct) {
            if (!baseDownstream.count(output)) {
                return false;
            }
        }
        // Likewise, an unmatched base consumer may stand for the nodes the reach passed through.
        for (const std::pair<std::string, std::string>& output : reach.through) {
            if (!baseDownstream.count(output) && !unmatchedConsumer) {
                return false;
            }
        }

        return agreeing > 0;
    }

    void
    adoptByStructure(const MergeGraph& base,
                     const MergeGraph& other,
                     NodeCorrespondence* correspondence)
    {
        bool progress = true;
        while (progress) {
            progress = false;
            const std::set<std::string> matchedOther = correspondence->matchedOther();
            // (plug-in, parent) to the unmatched nodes without an origin, in ID order.
            std::map<std::pair<std::string, std::string>, std::vector<std::string>> unmatched;
            for (const std::pair<const std::string, MergeGraph::Node>& otherEntry : other.nodes()) {
                if (!matchedOther.count(otherEntry.first) && !otherEntry.second.node->origin.isSet()) {
                    unmatched[std::make_pair(otherEntry.second.node->pluginID, otherEntry.second.parentId)].push_back(otherEntry.first);
                }
            }
            std::map<std::string, std::vector<std::string>> candidatesOfBase;
            std::map<std::string, int> claimsOnCandidate;
            for (const std::pair<const std::string, MergeGraph::Node>& baseEntry : base.nodes()) {
                if (correspondence->baseToOther.count(baseEntry.first)) {
                    continue;
                }
                std::string parent;
                if (!baseEntry.second.parentId.empty()) {
                    parent = correspondence->otherFor(baseEntry.second.parentId);
                    if (parent.empty()) {
                        continue;
                    }
                }
                std::map<std::pair<std::string, std::string>, std::vector<std::string>>::const_iterator bucket = unmatched.find(std::make_pair(baseEntry.second.node->pluginID, parent));
                if (bucket == unmatched.end()) {
                    continue;
                }
                for (const std::string& candidateId : bucket->second) {
                    if (wiredAsBase(other, baseEntry.second, *other.find(candidateId), *correspondence, matchedOther)) {
                        candidatesOfBase[baseEntry.first].push_back(candidateId);
                        ++claimsOnCandidate[candidateId];
                    }
                }
            }
            for (const std::pair<const std::string, std::vector<std::string>>& candidates : candidatesOfBase) {
                if ((candidates.second.size() == 1) && (claimsOnCandidate[candidates.second.front()] == 1)) {
                    correspondence->baseToOther[candidates.first] = candidates.second.front();
                    correspondence->adopted.insert(candidates.second.front());
                    progress = true;
                }
            }
        }
    }

} // namespace

MergeGraph::MergeGraph(const ProjectSerialization& project)
{
    OpsDetail::InputRefs refs;
    OpsDetail::resolveInputs(project.nodes.nodes, &refs);
    indexNodes(project.nodes.nodes, std::string(), std::string(), refs, &_nodes, &_idByPath);
    for (std::pair<const std::string, Node>& entry : _nodes) {
        for (const std::pair<const std::string, std::string>& input : entry.second.inputs) {
            std::map<std::string, Node>::iterator source = _nodes.find(input.second);
            if (source != _nodes.end()) {
                source->second.outputs.emplace(entry.first, input.first);
            }
        }
    }
}

const MergeGraph::Node*
MergeGraph::find(const std::string& id) const
{
    std::map<std::string, Node>::const_iterator found = _nodes.find(id);

    return (found == _nodes.end()) ? nullptr : &found->second;
}

const MergeGraph::Node*
MergeGraph::findByPath(const std::string& path) const
{
    std::map<std::string, std::string>::const_iterator found = _idByPath.find(path);

    return (found == _idByPath.end()) ? nullptr : find(found->second);
}

std::string
NodeCorrespondence::otherFor(const std::string& baseId) const
{
    std::map<std::string, std::string>::const_iterator found = baseToOther.find(baseId);

    return (found == baseToOther.end()) ? std::string() : found->second;
}

std::string
NodeCorrespondence::baseFor(const std::string& otherId) const
{
    for (const std::pair<const std::string, std::string>& entry : baseToOther) {
        if (entry.second == otherId) {
            return entry.first;
        }
    }

    return std::string();
}

std::set<std::string>
NodeCorrespondence::matchedOther() const
{
    std::set<std::string> result;
    for (const std::pair<const std::string, std::string>& entry : baseToOther) {
        result.insert(entry.second);
    }

    return result;
}

NodeCorrespondence
matchNodes(const MergeGraph& base,
           const MergeGraph& other,
           const std::string& templateKey)
{
    const std::map<std::string, std::vector<std::string>> claims = collectClaims(base, other, templateKey);
    NodeCorrespondence result = matchUniqueClaims(claims);
    resolveAmbiguousClaims(base, other, claims, &result);
    adoptByStructure(base, other, &result);

    return result;
}

NodeSerializationPtr
findNodeSerialization(const std::list<NodeSerializationPtr>& nodes,
                      const std::string& id)
{
    for (const NodeSerializationPtr& node : nodes) {
        if (node->id == id) {
            return node;
        }
        NodeSerializationPtr child = findNodeSerialization(node->children, id);
        if (child) {
            return child;
        }
    }

    return NodeSerializationPtr();
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
