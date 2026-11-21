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

#ifndef Serialization_Merge_ThreeWayMerge_h
#define Serialization_Merge_ThreeWayMerge_h

#include "Global/Macros.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/Merge/MergeHandler.h"
#include "Serialization/Merge/NodeMatcher.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief What a merge of one template import works from: the import's key, the matches of the
 * template base against the shot (mine) and the new template (theirs), and the shot nodes in scope.
 * An empty key, which no template import has, plans a whole-project merge instead.
 **/
struct MergePlan {
    std::string templateKey;
    NodeCorrespondence baseToMine;
    NodeCorrespondence baseToTheirs;
    std::set<std::string> scope;
};

MergePlan planMerge(const ProjectSerialization& base,
                    const ProjectSerialization& mine,
                    const ProjectSerialization& theirs,
                    const std::string& templateKey);

/**
 * @brief Plans a merge of three versions of one project, such as two branches of a shot and their
 * common ancestor: nodes match by their own IDs, or by structure when a side deleted and recreated
 * one, and every node of mine is in scope. Origins are not followed, so each template import's
 * nodes merge like any other.
 **/
MergePlan planProjectMerge(const ProjectSerialization& base,
                           const ProjectSerialization& mine,
                           const ProjectSerialization& theirs);

struct MergeOptions {
    // Null for MergeHandlerRegistry::defaults().
    const MergeHandlerRegistry* handlers = nullptr;
    // Mints the ID of each node the merge adds; null for random IDs.
    std::function<std::string()> newNodeId;
    // Where a template merge's theirs came from, recorded on the import with theirs as its new
    // base: the template file, the SHA-256 of its project.yaml, and when it was read (ISO 8601,
    // UTC). An empty field leaves the import's as it was.
    std::string templatePath;
    std::string templateContentHash;
    std::string templateImportedAt;
};

struct MergeResult {
    ProjectSerialization merged;
    std::vector<MergeConflict> conflicts;
};

/**
 * @brief Merges the supervisor's new template (theirs) into the shot (mine), from the template the
 * shot imported (base). Nodes outside the plan's scope keep mine's state. Knobs merge per
 * dimension through the handler registry, inputs per label, and a node's position as one (x, y)
 * pair; names, labels, positions, sizes and colours take the side that changed them and never
 * conflict. A script name only has to be free among its siblings, so a node from theirs keeps its
 * name unless a node in the same group already holds it. Every conflict is resolved, mostly by
 * keeping mine, and listed.
 *
 * Expressions and links that name nodes are compared and written in the merged snapshot's names,
 * so a node the artist renamed keeps the supervisor's new expressions that name it. The merged
 * nodes that stand for a template node carry an origin naming theirs, and the import's base
 * becomes theirs, with the options' template file, hash and time, so merging the same theirs again
 * changes no node.
 *
 * A whole-project plan (planProjectMerge()) leaves origins as the sides have them, keeps the ID of
 * a node taken from theirs unless mine uses it, merges the project knobs per knob, with conflicts
 * that name no node, and each template import as a whole: theirs' entry when mine left it as base
 * had it, else mine's. Formats and layers keep mine's.
 **/
MergeResult threeWayMerge(const ProjectSerialization& base,
                          const ProjectSerialization& mine,
                          const ProjectSerialization& theirs,
                          const MergePlan& plan,
                          const MergeOptions& options = MergeOptions());

/**
 * @brief The conflicts to save with result.merged: the conflicts mine saved from earlier merges
 * that are still open, then, for a whole-project plan, those theirs saved, then result's own. An
 * earlier conflict is open while the node it names is in the merged snapshot (or it names no
 * node) and the merge raised no conflict of its kind at the same place; one both sides saved is
 * kept once. For a whole-project plan the saved lists merge three-way by kind, node, knob and
 * place: a conflict base saved that either side no longer has was resolved there and stays
 * resolved.
 **/
std::vector<MergeConflict> conflictsToSave(const ProjectSerialization& base,
                                           const ProjectSerialization& mine,
                                           const ProjectSerialization& theirs,
                                           const MergePlan& plan,
                                           const MergeResult& result);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_ThreeWayMerge_h
