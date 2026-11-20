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

#include "Serialization/Merge/MergeScope.h"

#include <deque>

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    std::set<std::string>
    reach(const MergeGraph& graph,
          const std::set<std::string>& seeds,
          bool forward)
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
            } else {
                for (const std::pair<const std::string, std::string>& input : node->inputs) {
                    if (seen.insert(input.second).second) {
                        queue.push_back(input.second);
                    }
                }
            }
        }

        return seen;
    }

} // namespace

std::set<std::string>
mergeScope(const MergeGraph& mine,
           const NodeCorrespondence& baseToMine)
{
    const std::set<std::string> seeds = baseToMine.matchedOther();
    const std::set<std::string> downstream = reach(mine, seeds, true);
    const std::set<std::string> upstream = reach(mine, seeds, false);
    std::set<std::string> scope(seeds);
    for (const std::string& id : downstream) {
        if (upstream.count(id)) {
            scope.insert(id);
        }
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

} // namespace Serialization
NATRON_NAMESPACE_EXIT
