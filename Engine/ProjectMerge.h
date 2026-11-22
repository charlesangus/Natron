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

#ifndef Engine_ProjectMerge_h
#define Engine_ProjectMerge_h

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Template updates and comparisons of the open project. Main thread only.
 **/
namespace ProjectMerge {
/**
 * @brief Applies a batch of operations to the open project and returns one result per operation,
 * in order. applyGraphOps() is one; the node graph passes one that does the same inside an undo
 * command.
 **/
typedef std::function<std::vector<Serialization::OpResult>(const std::vector<Serialization::GraphOp>&)> OpApplier;

struct UpdateResult {
    // False when nothing was merged or an operation could not be applied; error says why. The
    // template import and the project's saved conflicts are then left as they were.
    bool ok = false;
    QString error;
    // The conflicts this merge raised. Each is already resolved in the graph, mostly by keeping mine.
    std::vector<Serialization::MergeConflict> conflicts;
    // What was applied to the graph, and how each operation went.
    std::vector<Serialization::GraphOp> ops;
    std::vector<Serialization::OpResult> results;
};

/**
 * @brief Merges the current version of a template into the project: base is the import's stored
 * snapshot, mine is the live project, theirs is the template file newPath, or the path the import
 * recorded when newPath is empty. The operations that turn the live graph into the merged one go
 * through applier (applyGraphOps() when it is empty). When all of them apply, the import's base
 * becomes theirs and the project's saved conflicts become the earlier ones still open plus this
 * merge's; when merging the same file again the graph does not change.
 **/
UpdateResult updateFromTemplate(const ProjectPtr& project,
                                const std::string& templateKey,
                                const QString& newPath,
                                const OpApplier& applier = OpApplier());

struct NodeChange {
    enum Kind {
        eAdded,
        eChanged,
        eRemoved,
    };

    Kind kind = eChanged;
    std::string nodeId;
    // The group the node is in, by ID, or empty at the top level.
    std::string parentId;
    // As in the live project, or as in the other snapshot for a removed node.
    std::string scriptName;
    std::string label;
    std::string pluginID;
    bool hasPosition = false;
    double x = 0.;
    double y = 0.;
    // eChanged: what differs, by knob name and input label.
    std::vector<std::string> knobs;
    std::vector<std::string> inputs;
    bool renamed = false;
    bool appearance = false;
    bool userKnobs = false;
    bool roto = false;
    bool tracker = false;
    // The plug-in, plug-in version, Python module or parent changed.
    bool recreated = false;
};

struct ChangeSummary {
    // Added and changed nodes in the live graph's order, then the removed ones in the other's.
    std::vector<NodeChange> nodes;
    // The project knobs that differ, by name, and whether its formats, layers or variables do.
    std::vector<std::string> projectKnobs;
    bool projectData = false;

    const NodeChange* find(const std::string& nodeId) const;
};

/**
 * @brief How the live project differs from other, node by node, matched by ID: nodes the live
 * project has and other does not are added, the reverse are removed. Changes to the project's
 * own knobs, formats, layers and variables are listed apart. Nothing is changed.
 **/
ChangeSummary compare(const ProjectPtr& project,
                      const Serialization::ProjectSerialization& other);
} // namespace ProjectMerge

NATRON_NAMESPACE_EXIT

#endif // Engine_ProjectMerge_h
