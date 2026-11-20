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
            const NodeOrigin& origin = entry.second.origin;
            std::string claimed;
            if (origin.nodeId.empty()) {
                claimed = entry.first;
            } else if (origin.templateKey == templateKey) {
                claimed = origin.nodeId;
            }
            if (!claimed.empty() && base.find(claimed)) {
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

    bool
    sameParent(const MergeGraph::Node& baseNode,
               const MergeGraph::Node& otherNode,
               const NodeCorrespondence& correspondence)
    {
        if (baseNode.parentId.empty()) {
            return otherNode.parentId.empty();
        }
        const std::string mapped = correspondence.otherFor(baseNode.parentId);

        return !mapped.empty() && (mapped == otherNode.parentId);
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

    void
    adoptByStructure(const MergeGraph& base,
                     const MergeGraph& other,
                     NodeCorrespondence* correspondence)
    {
        bool progress = true;
        while (progress) {
            progress = false;
            const std::set<std::string> matchedOther = correspondence->matchedOther();
            std::map<std::string, std::vector<std::string>> candidatesOfBase;
            std::map<std::string, int> claimsOnCandidate;
            for (const std::pair<const std::string, MergeGraph::Node>& baseEntry : base.nodes()) {
                if (correspondence->baseToOther.count(baseEntry.first)) {
                    continue;
                }
                for (const std::pair<const std::string, MergeGraph::Node>& otherEntry : other.nodes()) {
                    const MergeGraph::Node& candidate = otherEntry.second;
                    if (matchedOther.count(otherEntry.first) || !candidate.origin.nodeId.empty() || (candidate.node->pluginID != baseEntry.second.node->pluginID) || !sameParent(baseEntry.second, candidate, *correspondence) || !neighbourAgreement(baseEntry.second, candidate, *correspondence, matchedOther).exact) {
                        continue;
                    }
                    candidatesOfBase[baseEntry.first].push_back(otherEntry.first);
                    ++claimsOnCandidate[otherEntry.first];
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

    std::set<std::string>
    reach(const MergeGraph& graph,
          const std::set<std::string>& seeds,
          bool forward,
          bool backward)
    {
        std::set<std::string> seen(seeds);
        std::deque<std::string> queue(seeds.begin(), seeds.end());
        while (!queue.empty()) {
            const MergeGraph::Node* node = graph.find(queue.front());
            queue.pop_front();
            if (!node) {
                continue;
            }
            if (forward) {
                for (const std::pair<std::string, std::string>& output : node->outputs) {
                    if (seen.insert(output.first).second) {
                        queue.push_back(output.first);
                    }
                }
            }
            if (backward) {
                for (const std::pair<const std::string, std::string>& input : node->inputs) {
                    if (seen.insert(input.second).second) {
                        queue.push_back(input.second);
                    }
                }
            }
        }

        return seen;
    }

    std::set<std::string>
    enclosed(const MergeGraph& graph,
             const std::set<std::string>& seeds)
    {
        const std::set<std::string> downstream = reach(graph, seeds, true, false);
        const std::set<std::string> upstream = reach(graph, seeds, false, true);
        std::set<std::string> result(seeds);
        for (const std::string& id : downstream) {
            if (upstream.count(id)) {
                result.insert(id);
            }
        }

        return result;
    }

} // namespace

MergeGraph::MergeGraph(const ProjectSerialization& project,
                       NodeOrigins origins)
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
    for (std::pair<const std::string, NodeOrigin>& origin : origins) {
        std::map<std::string, Node>::iterator node = _nodes.find(origin.first);
        if (node != _nodes.end()) {
            node->second.origin = std::move(origin.second);
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

std::set<std::string>
NodeCorrespondence::matchedOther() const
{
    std::set<std::string> result;
    for (const std::pair<const std::string, std::string>& entry : baseToOther) {
        result.insert(entry.second);
    }

    return result;
}

NodeMatcher::~NodeMatcher() = default;

NodeCorrespondence
IdOnlyMatcher::match(const MergeGraph& base,
                     const MergeGraph& other,
                     const std::string& templateKey) const
{
    return matchUniqueClaims(collectClaims(base, other, templateKey));
}

NodeCorrespondence
IdPlusAdoptionMatcher::match(const MergeGraph& base,
                             const MergeGraph& other,
                             const std::string& templateKey) const
{
    const std::map<std::string, std::vector<std::string>> claims = collectClaims(base, other, templateKey);
    NodeCorrespondence result = matchUniqueClaims(claims);
    resolveAmbiguousClaims(base, other, claims, &result);
    adoptByStructure(base, other, &result);

    return result;
}

NodeCorrespondence
NameBasedMatcher::match(const MergeGraph& base,
                        const MergeGraph& other,
                        const std::string& /*templateKey*/) const
{
    NodeCorrespondence result;
    for (const std::pair<const std::string, MergeGraph::Node>& entry : base.nodes()) {
        const MergeGraph::Node* found = other.findByPath(entry.second.path);
        if (found) {
            result.baseToOther[entry.first] = found->node->id;
        }
    }

    return result;
}

std::string_view
mergeScopeRuleName(MergeScopeRule rule)
{
    switch (rule) {
    case MergeScopeRule::eOriginOnly:
        return "OriginOnly";
    case MergeScopeRule::eEnclosed:
        return "Enclosed";
    case MergeScopeRule::eMatchedEnclosed:
        return "MatchedEnclosed";
    case MergeScopeRule::eConnected:
        return "Connected";
    }

    return "?";
}

const std::vector<MergeScopeRule>&
allMergeScopeRules()
{
    static const std::vector<MergeScopeRule> rules = {
        MergeScopeRule::eOriginOnly,
        MergeScopeRule::eEnclosed,
        MergeScopeRule::eMatchedEnclosed,
        MergeScopeRule::eConnected,
    };

    return rules;
}

std::set<std::string>
mergeScope(const MergeGraph& base,
           const MergeGraph& mine,
           const std::string& templateKey,
           MergeScopeRule rule)
{
    std::set<std::string> seeds;
    if (rule == MergeScopeRule::eMatchedEnclosed) {
        seeds = IdPlusAdoptionMatcher().match(base, mine, templateKey).matchedOther();
    } else {
        for (const std::pair<const std::string, MergeGraph::Node>& entry : mine.nodes()) {
            if (!entry.second.origin.nodeId.empty() && (entry.second.origin.templateKey == templateKey)) {
                seeds.insert(entry.first);
            }
        }
    }

    std::set<std::string> scope;
    switch (rule) {
    case MergeScopeRule::eOriginOnly:
        scope = seeds;
        break;
    case MergeScopeRule::eEnclosed:
    case MergeScopeRule::eMatchedEnclosed:
        scope = enclosed(mine, seeds);
        break;
    case MergeScopeRule::eConnected:
        scope = reach(mine, seeds, true, true);
        break;
    }

    for (const std::pair<const std::string, MergeGraph::Node>& entry : mine.nodes()) {
        for (const MergeGraph::Node* ancestor = mine.find(entry.second.parentId); ancestor; ancestor = mine.find(ancestor->parentId)) {
            if (scope.count(ancestor->node->id)) {
                scope.insert(entry.first);
                break;
            }
        }
    }

    return scope;
}

std::vector<std::unique_ptr<NodeMatcher>>
prototypeNodeMatchers()
{
    std::vector<std::unique_ptr<NodeMatcher>> matchers;
    matchers.push_back(std::make_unique<IdOnlyMatcher>());
    matchers.push_back(std::make_unique<IdPlusAdoptionMatcher>());
    matchers.push_back(std::make_unique<NameBasedMatcher>());

    return matchers;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
