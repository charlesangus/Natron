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

#include "Serialization/Ops/OpsPrivate.h"

#include <set>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace OpsDetail {

    NodeSerializationPtr
    deepCopyNode(const NodeSerialization& node)
    {
        NodeSerializationPtr copy = std::make_shared<NodeSerialization>(node);
        for (NodeSerializationPtr& child : copy->children) {
            child = deepCopyNode(*child);
        }

        return copy;
    }

    void
    stripInputsRecursive(NodeSerialization* node)
    {
        node->inputs.clear();
        for (const NodeSerializationPtr& child : node->children) {
            stripInputsRecursive(child.get());
        }
    }

    bool
    indexNodes(const std::list<NodeSerializationPtr>& nodes,
               const NodeSerializationPtr& parent,
               NodeIdIndex* index)
    {
        bool unique = true;
        for (const NodeSerializationPtr& node : nodes) {
            if (!node->id.empty() && !index->emplace(node->id, IndexedNode { node, parent }).second) {
                unique = false;
            }
            if (!indexNodes(node->children, node, index)) {
                unique = false;
            }
        }

        return unique;
    }

    std::list<NodeSerializationPtr>&
    siblingsOf(std::list<NodeSerializationPtr>& topLevel,
               const NodeSerializationPtr& parent)
    {
        return parent ? parent->children : topLevel;
    }

    namespace {

        void
        resolveInScopes(const std::list<NodeSerializationPtr>& nodes,
                        std::vector<const std::list<NodeSerializationPtr>*>* scopes,
                        InputRefs* refs)
        {
            scopes->push_back(&nodes);
            for (const NodeSerializationPtr& node : nodes) {
                NodeInputRefs& inputs = (*refs)[node.get()];
                for (const std::pair<const std::string, std::string>& input : node->inputs) {
                    InputRef ref { std::string(), input.second };
                    bool found = false;
                    for (auto scope = scopes->rbegin(); !found && (scope != scopes->rend()); ++scope) {
                        for (const NodeSerializationPtr& candidate : **scope) {
                            if (candidate->scriptName == input.second) {
                                ref.sourceId = candidate->id;
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!ref.sourceId.empty()) {
                        ref.sourceName.clear();
                    }
                    inputs[input.first] = ref;
                }
                resolveInScopes(node->children, scopes, refs);
            }
            scopes->pop_back();
        }

    } // namespace

    void
    resolveInputs(const std::list<NodeSerializationPtr>& topLevel,
                  InputRefs* refs)
    {
        std::vector<const std::list<NodeSerializationPtr>*> scopes;
        resolveInScopes(topLevel, &scopes, refs);
    }

} // namespace OpsDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT
