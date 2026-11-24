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
#include <optional>
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
 * in order. applyAllOrNothing() is one; the node graph passes one that does the same inside an
 * undo command.
 **/
typedef std::function<std::vector<Serialization::OpResult>(const std::vector<Serialization::GraphOp>&)> OpApplier;

/**
 * @brief applyGraphOps(), except that when any operation fails the ones that applied are undone,
 * leaving the project as it was. restored, when given, is false when undoing them failed too.
 **/
std::vector<Serialization::OpResult> applyAllOrNothing(const ProjectPtr& project,
                                                       const std::vector<Serialization::GraphOp>& ops,
                                                       bool* restored = nullptr);

struct UpdateResult {
    // False when nothing was merged or an operation could not be applied; error says why. The
    // graph, the template import and the project's saved conflicts are then left as they were.
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
 * through applier (applyAllOrNothing() when it is empty). When one fails, the graph is put back as
 * it was before the update. When all of them apply, the import's base becomes theirs and the
 * project's saved conflicts become the earlier ones still open plus this merge's; when merging the
 * same file again the graph does not change.
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

    struct KnobChange {
        std::string name;
        // Absent when the knob is at its default on that side.
        std::optional<Serialization::KnobSerialization> before;
        std::optional<Serialization::KnobSerialization> after;
    };

    struct InputChange {
        std::string label;
        // The source's script name, empty when unconnected.
        std::string before;
        std::string after;
    };

    // eChanged: knobs and inputs again, in the same order, with each side's state.
    std::vector<KnobChange> knobChanges;
    std::vector<InputChange> inputChanges;
    // eChanged with renamed: the script name in the other snapshot.
    std::string previousScriptName;

    struct Link {
        std::string input;
        std::string nodeId;
    };

    // eRemoved: in the other snapshot, the node's sources by its input label, and the nodes in its
    // group that read from it by their input label.
    std::vector<Link> sources;
    std::vector<Link> readers;
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

/**
 * @brief compare() against the project file at path. Returns false with error set when it cannot
 * be read.
 **/
bool compareWithFile(const ProjectPtr& project,
                     const QString& path,
                     ChangeSummary* summary,
                     QString* error);

/**
 * @brief How the nodes of a template import differ from the import's stored base: the base's
 * nodes stand for the live nodes the merge would match them with, and only those nodes and the
 * nodes added inside the import's groups are listed. Returns false with error set when the project
 * has no such import or it has no base.
 **/
bool compareWithTemplateBase(const ProjectPtr& project,
                             const std::string& templateKey,
                             ChangeSummary* summary,
                             QString* error);

enum class ConflictSide {
    eMine,
    eTheirs,
    eBase,
};

/**
 * @brief Whether the state side recorded for conflict can be put back in the project: the knob,
 * node, roto item or track it names is still there, and the side has a state of its own (a side
 * whose input source or group the merge did not keep has none).
 *
 * The state is, by kind: a value or curve key on a knob of a node, of a roto item or track, or of
 * the project; an input's source; an inserted chain's connections; a node's group and inputs
 * there; the knobs an adopted node left at their default; a roto item, track or spline warp. For
 * a delete-modify conflict the side that deleted deletes, the side that kept keeps the graph as
 * it is, and the base gives a node the knobs it had in the base.
 **/
bool canChooseSide(const ProjectPtr& project,
                   const Serialization::MergeConflict& conflict,
                   ConflictSide side);

/**
 * @brief The operations that give the project side's state for conflict, leaving the rest of the
 * graph as it is now, possibly none when it already has it. A node moved back into a group keeps
 * its ID; the nodes that read from it in the group it left are disconnected. A roto item or
 * track the node lost goes back at the end of its list. Returns false with error set when
 * canChooseSide() is false or the state no longer fits the graph.
 **/
bool sideOperations(const ProjectPtr& project,
                    const Serialization::MergeConflict& conflict,
                    ConflictSide side,
                    std::vector<Serialization::GraphOp>* ops,
                    QString* error);

/**
 * @brief Whether conflict offers the handler option named option and its state can be applied.
 **/
bool canChooseOption(const ProjectPtr& project,
                     const Serialization::MergeConflict& conflict,
                     const std::string& option);

/**
 * @brief sideOperations() for the state of conflict's handler option named option.
 **/
bool optionOperations(const ProjectPtr& project,
                      const Serialization::MergeConflict& conflict,
                      const std::string& option,
                      std::vector<Serialization::GraphOp>* ops,
                      QString* error);

} // namespace ProjectMerge

NATRON_NAMESPACE_EXIT

#endif // Engine_ProjectMerge_h
