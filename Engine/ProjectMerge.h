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
#include <memory>
#include <optional>
#include <set>
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

// Scene units a node may move within its group, past its group's offset, before it counts as moved.
const double kMovedThreshold = 10.;

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
    // Where the node belongs in the live graph: x and y, except for a removed node, whose base
    // position is shifted by the offset of its nearest matched neighbour in the other snapshot's
    // wiring, or by its group's offset when no neighbour is matched. A group's offset is the median
    // of how far its matched nodes moved, so moving a whole template is no change.
    double placedX = 0.;
    double placedY = 0.;
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
    // eChanged: the node moved more than kMovedThreshold within its group, its group's offset
    // taken off.
    bool moved = false;
    // eChanged: the node's size, colour or overlay colour changed.
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
    // The two snapshots compared, in their editable state with knobs in their saved form: the
    // other version, under the IDs of the live nodes its nodes stand for, and the live project.
    std::shared_ptr<const Serialization::ProjectSerialization> other;
    std::shared_ptr<const Serialization::ProjectSerialization> live;
    // For a template version, the live nodes the comparison covers besides those other has; null
    // when it covers every node.
    std::shared_ptr<const std::set<std::string>> scope;

    const NodeChange* find(const std::string& nodeId) const;

    /**
     * @brief The node of the other snapshot with that ID, with its children for a group, or null.
     **/
    const Serialization::NodeSerialization* otherNode(const std::string& nodeId) const;
};

/**
 * @brief The comparison previous made, made again against the same other version and scope, as
 * after an edit of the live project. An empty summary when previous has no other version.
 **/
ChangeSummary recompare(const ProjectPtr& project,
                        const ChangeSummary& previous);

/**
 * @brief How the live project differs from other, node by node, matched by ID: nodes the live
 * project has and other does not are added, the reverse are removed. Changes to the project's
 * own knobs, formats, layers and variables are listed apart. Nothing is changed.
 **/
ChangeSummary compare(const ProjectPtr& project,
                      const Serialization::ProjectSerialization& other);

/**
 * @brief compare() against the project file at path. When the live nodes' origins name the file's
 * nodes more often than the live nodes' own IDs do, the file is a version of that template import
 * and is compared as compareWithTemplateBase() compares the import with its base. Returns false with error set when it cannot be read.
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

/**
 * @brief What restoring a node does to an input that read from it in the other version and that
 * the artist has since wired to something else. An input left empty, or still on what the node
 * read from in the other version, always goes back to the restored node.
 **/
enum class SpliceMode {
    // Leave the input, and report it so the artist can choose.
    eAsk,
    // Leave the input.
    eKeepMine,
    // Connect the input to the restored node.
    eEverywhere,
};

/**
 * @brief An input that a restore left wired as the artist has it, by node IDs: reader's input
 * read from restored in the other version and reads from source (empty when unconnected) now.
 **/
struct PendingSplice {
    std::string readerId;
    std::string input;
    std::string restoredId;
    std::string sourceId;
};

/**
 * @brief The operations that bring back nodes of summary's other version that the live project no
 * longer has, by ID, under their IDs: each at its placed position with its knobs, a group with
 * everything inside it. A node takes a fresh script name when a live node of its group has its
 * name, and the inputs and expressions of the restored nodes that name it follow. An input of a
 * restored node connects to its source when that is live in the same group or restored with it,
 * and stays empty otherwise. Restored nodes that read from each other are wired as in the other
 * version. A live node that read from a restored node in the other version is connected to it
 * again on that input, as mode says when the artist has rewired the input since; under eAsk each
 * such input is added to pending. An ID the live project has, or that the other version does not,
 * is skipped, as is a node inside a group restored with it. Returns false with error set, and no
 * operations, when a node's group is neither live nor restored with it.
 **/
bool restoreOperations(const ProjectPtr& project,
                       const ChangeSummary& summary,
                       const std::vector<std::string>& nodeIds,
                       SpliceMode mode,
                       std::vector<Serialization::GraphOp>* ops,
                       std::vector<PendingSplice>* pending,
                       QString* error);

/**
 * @brief The operations that give changed nodes, by ID, their other version's state of each
 * changed knob, input and appearance (size, colour and overlay colour). An input goes back to its
 * source in the other version when that is live in the node's group, and is disconnected
 * otherwise. Nodes that are not changed in summary are skipped.
 **/
std::vector<Serialization::GraphOp> revertOperations(const ProjectPtr& project,
                                                     const ChangeSummary& summary,
                                                     const std::vector<std::string>& nodeIds);

/**
 * @brief The operation that gives one changed knob of a changed node its other version's state,
 * or false when summary lists no such change.
 **/
bool takeKnobOperations(const ProjectPtr& project,
                        const ChangeSummary& summary,
                        const std::string& nodeId,
                        const std::string& knobName,
                        std::vector<Serialization::GraphOp>* ops);

/**
 * @brief The operations that delete added nodes, by ID. As deleting a node in the graph does, the
 * nodes reading from one with a single connected input are connected to that input's source, and
 * are disconnected otherwise. Nodes that are not added in summary are skipped.
 **/
std::vector<Serialization::GraphOp> removeAddedOperations(const ProjectPtr& project,
                                                          const ChangeSummary& summary,
                                                          const std::vector<std::string>& nodeIds);

/**
 * @brief The operations that move moved nodes, by ID, back to their position in the other
 * version, shifted by their group's offset so a whole template moved stays where it is. Nodes
 * that are not moved in summary are skipped.
 **/
std::vector<Serialization::GraphOp> restorePositionOperations(const ProjectPtr& project,
                                                              const ChangeSummary& summary,
                                                              const std::vector<std::string>& nodeIds);

/**
 * @brief A splice choice for each of pending, appended to the project's merge conflicts in place
 * of any for the same input: Mine (Keep My Wiring) leaves the input as it is, Theirs (Splice In)
 * connects it to the restored node.
 **/
void addSpliceChoices(const ProjectPtr& project,
                      const std::vector<PendingSplice>& pending);

/**
 * @brief The splice choice for one pending input.
 **/
Serialization::MergeConflict spliceChoice(const PendingSplice& splice);

/**
 * @brief What conflict is about, in a line, as the conflict lists show it.
 **/
QString conflictLabel(const Serialization::MergeConflict& conflict);

enum class ConflictSide {
    eMine,
    eTheirs,
    eBase,
};

/**
 * @brief One way to resolve a conflict: a side, or the handler option named option.
 **/
struct ConflictChoice {
    ConflictSide side = ConflictSide::eMine;
    // Empty for a side.
    std::string option;
    QString label;
};

/**
 * @brief The choices to offer for conflict, in Mine, Theirs, Base, options order: each one whose
 * state can be put back in the project, less any whose result equals that of an earlier choice
 * offered. A side without state of its own is not offered (Base for a node only mine has, a group
 * the merge did not keep), nor a Keep Both Keys whose keys share a time, nor Base where it is what
 * Mine or Theirs already gives. A splice choice offers Keep My Wiring (Mine, always) and Splice
 * In (Theirs). Every UI and binding offers these and only these.
 **/
std::vector<ConflictChoice> availableChoices(const ProjectPtr& project,
                                             const Serialization::MergeConflict& conflict);

// "mine", "theirs" or "base" for a side, else the option's name: how scripts name a choice.
std::string choiceName(const ConflictChoice& choice);

/**
 * @brief The choice of availableChoices() that choiceName() calls name. False when it offers none
 * by that name.
 **/
bool findChoice(const ProjectPtr& project,
                const Serialization::MergeConflict& conflict,
                const std::string& name,
                ConflictChoice* choice);

/**
 * @brief Whether availableChoices() offers side: the knob, node, roto item or track it names is
 * still there, the side has a state of its own (a side whose input source or group the merge did
 * not keep has none), and it gives a result no earlier side gives.
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
 * canChooseSide() is false or the state no longer fits the graph. Keep My Wiring of a splice
 * choice gives none.
 **/
bool sideOperations(const ProjectPtr& project,
                    const Serialization::MergeConflict& conflict,
                    ConflictSide side,
                    std::vector<Serialization::GraphOp>* ops,
                    QString* error);

/**
 * @brief Whether availableChoices() offers conflict's handler option named option.
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
