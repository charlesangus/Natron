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

#ifndef Serialization_Merge_MergePrivate_h
#define Serialization_Merge_MergePrivate_h

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Serialization/Merge/NodeReferences.h"
#include "Serialization/Merge/ThreeWayMerge.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace MergeDetail {

    // A node of the merged snapshot while it is being built.
    struct OutNode {
        // Without children; its inputs are written from inputIds when the tree is assembled.
        NodeSerializationPtr node;
        // Empty at the top level.
        std::string parent;
        // Input label to the merged ID of the source.
        std::map<std::string, std::string> inputIds;
        // Mine's nodes first, in mine's order, then the nodes taken from theirs, in theirs' order.
        std::pair<int, int> order;
        bool fromMine = true;
        // The base node it stands for, or empty.
        std::string baseId;
        // The theirs node it stands for, or empty.
        std::string theirsId;
    };

    struct MergeContext {
        MergeContext(const ProjectSerialization& baseProject,
                     const ProjectSerialization& mineProject,
                     const ProjectSerialization& theirsProject,
                     const MergePlan& plan,
                     const MergeOptions& options);

        const ProjectSerialization& mineProject;
        const ProjectSerialization& theirsProject;
        MergeGraph base;
        MergeGraph mine;
        MergeGraph theirs;
        const MergePlan& plan;
        const MergeHandlerRegistry& handlers;
        std::function<std::string()> newNodeId;
        std::map<std::string, std::string> mineToBase;
        std::map<std::string, std::string> theirsToBase;
        // Position in a depth-first walk of each side's tree.
        std::map<std::string, int> mineOrder;
        std::map<std::string, int> theirsOrder;

        std::map<std::string, OutNode> out;
        std::map<std::string, std::string> baseToOut;
        std::map<std::string, std::string> theirsToOut;
        // The script names in use in each group of the merged snapshot, by the group's merged ID,
        // empty for the top level. A name only has to be free among its siblings.
        std::map<std::string, std::set<std::string>> names;
        // Mine's names the merge changed, old to new, by mine's group.
        ScopedNodeRenames mineRenames;
        std::vector<MergeConflict> conflicts;
        // A merged node's reparent conflict, as an index into conflicts.
        std::map<std::string, std::size_t> reparentConflicts;
        // The parent each removed mine node had, so its orphans move up to a node that remains.
        std::map<std::string, std::string> removedParents;

        // The base node a side's node stands for, or empty.
        std::string baseOfMine(const std::string& mineId) const;
        std::string baseOfTheirs(const std::string& theirsId) const;

        // A script name not yet in use in group, from wanted.
        std::string uniqueName(const std::string& group,
                               const std::string& wanted) const;

        // Copies theirs' node into the merged snapshot under a fresh ID and a name free in its group.
        std::string addFromTheirs(const std::string& theirsId);

        void addConflict(MergeConflictKind kind,
                         const std::string& baseId,
                         MergeSide side = MergeSide::eMine);
    };

    // A mine or theirs node ID made comparable with base IDs: the base node it stands for, else a
    // tagged ID that no base node has.
    std::string inBaseTerms(const MergeContext& context,
                            const std::string& id,
                            bool fromTheirs);

    // The fully qualified name of the group a node is in, empty at the top level.
    std::string groupPath(const MergeGraph& graph,
                          const MergeGraph::Node& node);

    // A side's script name to its name on another side, for the base nodes both have.
    ScopedNodeRenames sideNames(const MergeGraph& from,
                                const MergeGraph& to,
                                const std::map<std::string, std::string>& fromToTo);

    // A node's knobs with references renamed.
    std::list<KnobSerialization> renamedKnobs(const MergeGraph& graph,
                                              const MergeGraph::Node& node,
                                              const ScopedNodeRenames& renames);

    // The script-name renames that take one side's names to the merged names.
    ScopedNodeRenames baseNamesToOut(const MergeContext& context);
    ScopedNodeRenames theirsNamesToOut(const MergeContext& context);

    // Node existence: deletes, restores, additions and identical additions.
    void mergeExistence(MergeContext* context);
    // Parents of the nodes both sides kept, and of the nodes whose group the merge removed.
    void mergeParents(MergeContext* context);
    // Renames every node whose name a sibling took first, now that each node has its group.
    void settleNames(MergeContext* context);
    // Script names, labels and layout of the nodes both sides kept.
    void mergeAppearance(MergeContext* context);
    // Knobs of the nodes both sides kept, and the adopted-node conflicts.
    void mergeKnobs(MergeContext* context);
    // Inputs of the nodes both sides kept.
    void mergeConnections(MergeContext* context);
    // The merged snapshot.
    ProjectSerialization assemble(MergeContext* context);
    // The side states of the node, input and group conflicts, once the merged snapshot is assembled.
    void recordConflictSides(MergeContext* context);

} // namespace MergeDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_MergePrivate_h
