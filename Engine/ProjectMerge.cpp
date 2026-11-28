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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ProjectMerge.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <variant>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtCore/QThread>

#include "Engine/GraphOpApply.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/ConflictResolution.h"
#include "Serialization/Merge/MergeHandler.h"
#include "Serialization/Merge/NodeReferences.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/Sha256.h"
#include "Serialization/Yaml/KnobYaml.h"

NATRON_NAMESPACE_ENTER

namespace ProjectMerge {
namespace {

    struct FlatNode {
        const Serialization::NodeSerialization* node = nullptr;
        std::string parentId;
    };

    typedef std::map<std::string, FlatNode> FlatNodes;

    void
    flatten(const std::list<Serialization::NodeSerializationPtr>& nodes,
            const std::string& parentId,
            FlatNodes* out,
            std::vector<std::string>* order)
    {
        for (const Serialization::NodeSerializationPtr& node : nodes) {
            if (!node) {
                continue;
            }
            if (!node->id.empty()) {
                FlatNode flat;
                flat.node = node.get();
                flat.parentId = parentId;
                if (out->emplace(node->id, flat).second) {
                    order->push_back(node->id);
                }
            }
            flatten(node->children, node->id, out, order);
        }
    }

    NodeChange
    describe(const FlatNode& flat,
             NodeChange::Kind kind)
    {
        NodeChange change;
        change.kind = kind;
        change.nodeId = flat.node->id;
        change.parentId = flat.parentId;
        change.scriptName = flat.node->scriptName;
        change.label = flat.node->label;
        change.pluginID = flat.node->pluginID;
        change.hasPosition = flat.node->layout.hasPosition;
        change.x = flat.node->layout.x;
        change.y = flat.node->layout.y;
        change.placedX = change.x;
        change.placedY = change.y;

        return change;
    }

    std::optional<Serialization::KnobSerialization>
    knobNamed(const Serialization::NodeSerialization& node,
              const std::string& name)
    {
        for (const Serialization::KnobSerialization& knob : node.knobs) {
            if (knob.name == name) {
                return knob;
            }
        }

        return std::nullopt;
    }

    std::string
    inputNamed(const Serialization::NodeSerialization& node,
               const std::string& label)
    {
        std::map<std::string, std::string>::const_iterator input = node.inputs.find(label);

        return (input == node.inputs.end()) ? std::string() : input->second;
    }

    // The node of nodes in group parentId with that script name, by ID, or empty.
    std::string
    idOfScriptName(const FlatNodes& nodes,
                   const std::string& parentId,
                   const std::string& scriptName)
    {
        for (const std::pair<const std::string, FlatNode>& entry : nodes) {
            if ((entry.second.parentId == parentId) && (entry.second.node->scriptName == scriptName)) {
                return entry.first;
            }
        }

        return std::string();
    }

    void
    describeChange(const FlatNode& before,
                   const FlatNode& after,
                   NodeChange* change)
    {
        for (const std::string& name : change->knobs) {
            change->knobChanges.push_back(NodeChange::KnobChange { name, knobNamed(*before.node, name), knobNamed(*after.node, name) });
        }
        for (const std::string& label : change->inputs) {
            change->inputChanges.push_back(NodeChange::InputChange { label, inputNamed(*before.node, label), inputNamed(*after.node, label) });
        }
        if (change->renamed) {
            change->previousScriptName = before.node->scriptName;
        }
    }

    void
    describeLinks(const FlatNodes& nodes,
                  const FlatNode& removed,
                  NodeChange* change)
    {
        for (const std::pair<const std::string, std::string>& input : removed.node->inputs) {
            const std::string source = idOfScriptName(nodes, removed.parentId, input.second);
            if (!source.empty()) {
                change->sources.push_back(NodeChange::Link { input.first, source });
            }
        }
        for (const std::pair<const std::string, FlatNode>& entry : nodes) {
            if (entry.second.parentId != removed.parentId) {
                continue;
            }
            for (const std::pair<const std::string, std::string>& input : entry.second.node->inputs) {
                if (input.second == removed.node->scriptName) {
                    change->readers.push_back(NodeChange::Link { input.first, entry.first });
                }
            }
        }
    }

    // The merge leaves out a knob at its defaults, but some knobs are saved even then (a channel
    // set always is). Without mine's copy back, the diff resets every such knob on every update.
    void
    keepMineDefaultKnobs(const std::list<Serialization::KnobSerialization>& mine,
                         std::list<Serialization::KnobSerialization>* merged)
    {
        for (const Serialization::KnobSerialization& knob : mine) {
            if (!Serialization::isDefaultKnob(knob)) {
                continue;
            }
            const bool kept = std::any_of(merged->begin(), merged->end(), [&knob](const Serialization::KnobSerialization& other) {
                return other.name == knob.name;
            });
            if (!kept) {
                merged->push_back(knob);
            }
        }
    }

    void
    keepMineDefaultKnobs(const FlatNodes& mine,
                         const std::list<Serialization::NodeSerializationPtr>& merged)
    {
        for (const Serialization::NodeSerializationPtr& node : merged) {
            if (!node) {
                continue;
            }
            FlatNodes::const_iterator found = mine.find(node->id);
            if (!node->id.empty() && (found != mine.end())) {
                keepMineDefaultKnobs(found->second.node->knobs, &node->knobs);
            }
            keepMineDefaultKnobs(mine, node->children);
        }
    }

    // A project read from a file carries only what the file saves of a plug-in knob (no type,
    // default, label or menu, a choice by its option ID), a live one everything; both are put in
    // the saved form so a knob compares by what it holds.
    void
    toSavedKnobs(std::list<Serialization::KnobSerialization>* knobs)
    {
        try {
            *knobs = Serialization::decodeKnobsYaml(Serialization::encodeKnobsYaml(*knobs));
        } catch (const std::exception&) {
        }
    }

    void
    toSavedKnobs(const std::list<Serialization::NodeSerializationPtr>& nodes)
    {
        for (const Serialization::NodeSerializationPtr& node : nodes) {
            if (node) {
                toSavedKnobs(&node->knobs);
                toSavedKnobs(node->children);
            }
        }
    }

    void
    toSavedKnobs(Serialization::ProjectSerialization* project)
    {
        toSavedKnobs(&project->projectKnobs);
        toSavedKnobs(project->nodes.nodes);
    }

    // The ID of the node an input of node names, or the name itself when it names none.
    std::string
    inputSourceOf(const FlatNodes& nodes,
                  const FlatNode& node,
                  const std::string& label)
    {
        const std::string name = inputNamed(*node.node, label);
        if (name.empty()) {
            return name;
        }
        const std::string id = idOfScriptName(nodes, node.parentId, name);

        return id.empty() ? name : id;
    }

    void
    addUnique(std::vector<std::string>* list,
              const std::string& name)
    {
        if (std::find(list->begin(), list->end(), name) == list->end()) {
            list->push_back(name);
        }
    }

    bool
    readSnapshot(const QString& path,
                 Serialization::ProjectSerialization* snapshot,
                 QString* error)
    {
        try {
            Serialization::ProjectBundleReader bundle(path.toStdString());
            Serialization::YamlWarnings warnings;
            *snapshot = bundle.readProject(&warnings);
        } catch (const std::exception& e) {
            if (error) {
                *error = QCoreApplication::translate("ProjectMerge", "%1 cannot be read: %2").arg(path).arg(QString::fromUtf8(e.what()));
            }

            return false;
        }

        return true;
    }

    void
    remapNodeIds(const std::list<Serialization::NodeSerializationPtr>& nodes,
                 const Serialization::NodeCorrespondence& correspondence)
    {
        for (const Serialization::NodeSerializationPtr& node : nodes) {
            if (!node) {
                continue;
            }
            const std::string mapped = correspondence.otherFor(node->id);
            if (!mapped.empty()) {
                node->id = mapped;
            }
            remapNodeIds(node->children, correspondence);
        }
    }

    const Serialization::MergeConflictValue&
    valueOf(const Serialization::MergeConflict& conflict,
            ConflictSide side)
    {
        switch (side) {
        case ConflictSide::eMine:
            return conflict.mine;
        case ConflictSide::eTheirs:
            return conflict.theirs;
        case ConflictSide::eBase:
            break;
        }

        return conflict.base;
    }

    KnobIPtr
    conflictKnob(const ProjectPtr& project,
                 const Serialization::MergeConflict& conflict)
    {
        if (conflict.knob.empty() || !conflict.item.empty()) {
            return KnobIPtr();
        }
        if (!conflict.nodeId.empty()) {
            NodePtr node = project->findNodeById(conflict.nodeId);

            return node ? node->getKnobByName(conflict.knob) : KnobIPtr();
        }
        if (!conflict.baseNodeId.empty()) {
            return KnobIPtr();
        }

        return project->getKnobByName(conflict.knob);
    }

    bool
    failWith(QString* error,
             const QString& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    bool
    failWith(QString* error,
             const std::string& message)
    {
        return failWith(error, QString::fromStdString(message));
    }

    Serialization::ResolutionSide
    resolutionSide(ConflictSide side)
    {
        switch (side) {
        case ConflictSide::eMine:
            return Serialization::ResolutionSide::eMine;
        case ConflictSide::eTheirs:
            return Serialization::ResolutionSide::eTheirs;
        case ConflictSide::eBase:
            break;
        }

        return Serialization::ResolutionSide::eBase;
    }

    // A user knob of the project that one side deleted and the other changed.
    bool
    projectKnobDeleted(const Serialization::MergeConflict& conflict)
    {
        return (conflict.kind == Serialization::MergeConflictKind::eDeleteModify) && conflict.nodeId.empty() && conflict.baseNodeId.empty() && !conflict.knob.empty();
    }

    // The side, or with no side the handler option, applied to snapshot.
    typedef std::function<bool(Serialization::ProjectSerialization* snapshot, std::string* error)> SnapshotEdit;

    // The operations giving the project value's state for conflict: one knob set for a knob of a
    // node or of the project, an edit of a snapshot of the project for the rest.
    bool
    stateOperations(const ProjectPtr& project,
                    const Serialization::MergeConflict& conflict,
                    const Serialization::MergeConflictValue& value,
                    const SnapshotEdit& edit,
                    std::vector<Serialization::GraphOp>* ops,
                    QString* error)
    {
        const bool knobValue = ((conflict.kind == Serialization::MergeConflictKind::eValue) || (conflict.kind == Serialization::MergeConflictKind::eCurveKey)) && conflict.item.empty();
        if (knobValue) {
            KnobIPtr knob = conflictKnob(project, conflict);
            if (!knob) {
                return failWith(error, QCoreApplication::translate("ProjectMerge", "The knob of this conflict is no longer in the project"));
            }
            Serialization::KnobSerialization state;
            knob->toSerialization(&state);
            // A side read from a project file holds values typed only by their YAML form (4 is an
            // int), which the typed live state cannot take until the live knob types them.
            Serialization::MergeConflictValue typed = value;
            if (typed.knob) {
                typed.knob = completeKnobSerializationForKnob(*knob, *typed.knob);
            }
            std::string message;
            if (!Serialization::applyConflictKnobValue(conflict, typed, &state, &message)) {
                return failWith(error, message);
            }
            Serialization::SetKnobOp set;
            if (!conflict.nodeId.empty()) {
                set.nodeId = conflict.nodeId;
            }
            set.knobName = conflict.knob;
            if (!Serialization::isDefaultKnob(state)) {
                set.value = std::move(state);
            }
            ops->assign(1, Serialization::GraphOp(std::move(set)));

            return true;
        }
        if (conflict.nodeId.empty() && !projectKnobDeleted(conflict)) {
            return failWith(error, QCoreApplication::translate("ProjectMerge", "This conflict has no side to choose: the merge's choice can only be accepted"));
        }

        Serialization::ProjectSerialization live;
        project->toSerialization(&live);
        const Serialization::ProjectSerialization before = Serialization::editableState(live);
        Serialization::ProjectSerialization after = Serialization::editableState(live);
        std::string message;
        if (!edit(&after, &message)) {
            return failWith(error, message);
        }
        *ops = Serialization::diffSnapshots(before, after);

        return true;
    }

    // What a side's state needs to be put back, without building it.
    bool
    sideAvailable(const ProjectPtr& project,
                  const Serialization::MergeConflict& conflict,
                  const Serialization::MergeConflictValue& value,
                  bool baseSide)
    {
        using Serialization::MergeConflictKind;
        if ((conflict.kind == MergeConflictKind::eValue) || (conflict.kind == MergeConflictKind::eCurveKey)) {
            if ((conflict.kind == MergeConflictKind::eCurveKey) && !conflict.time) {
                return false;
            }
            if (conflict.item.empty()) {
                return bool(conflictKnob(project, conflict));
            }
        }
        if (projectKnobDeleted(conflict)) {
            return true;
        }
        if (conflict.nodeId.empty()) {
            return false;
        }
        const NodePtr node = project->findNodeById(conflict.nodeId);
        if (!node) {
            return false;
        }
        switch (conflict.kind) {
        case MergeConflictKind::eInput:
        case MergeConflictKind::eStructuralInsert:
        case MergeConflictKind::eSpliceChoice:
            for (const Serialization::MergeConflictConnection& connection : value.connections) {
                if (!project->findNodeById(connection.nodeId) || (!connection.sourceId.empty() && !project->findNodeById(connection.sourceId))) {
                    return false;
                }
            }

            return !value.connections.empty();
        case MergeConflictKind::eReparent:
            return value.parent && (value.parent->empty() || project->findNodeById(*value.parent));
        case MergeConflictKind::eAdopted:
            return value.knobs.has_value();
        case MergeConflictKind::eDeleteModify:
            if (conflict.item.empty() && conflict.knob.empty() && baseSide) {
                return value.knobs.has_value();
            }

            return true;
        case MergeConflictKind::eRotoShape:
        case MergeConflictKind::eValue:
        case MergeConflictKind::eCurveKey:
            break;
        }

        return true;
    }

    bool
    allApplied(const std::vector<Serialization::GraphOp>& ops,
               const std::vector<Serialization::OpResult>& results)
    {
        if (results.size() != ops.size()) {
            return false;
        }

        return std::all_of(results.begin(), results.end(), [](const Serialization::OpResult& result) {
            return result.applied();
        });
    }

    // Puts the project back to state, an editable state taken earlier. False when an operation
    // doing so fails.
    bool
    restoreState(const ProjectPtr& project,
                 const Serialization::ProjectSerialization& state)
    {
        Serialization::ProjectSerialization live;
        project->toSerialization(&live);
        const std::vector<Serialization::GraphOp> back = Serialization::diffSnapshots(Serialization::editableState(live), state);
        if (back.empty()) {
            return true;
        }

        return allApplied(back, applyGraphOps(project, back));
    }

    ConflictSide
    conflictSide(Serialization::ResolutionSide side)
    {
        switch (side) {
        case Serialization::ResolutionSide::eMine:
            return ConflictSide::eMine;
        case Serialization::ResolutionSide::eTheirs:
            return ConflictSide::eTheirs;
        case Serialization::ResolutionSide::eBase:
            break;
        }

        return ConflictSide::eBase;
    }

    bool
    keepsWiring(const Serialization::MergeConflict& conflict,
                const Serialization::MergeConflictChoice& choice)
    {
        return (conflict.kind == Serialization::MergeConflictKind::eSpliceChoice) && choice.option.empty() && (choice.side == Serialization::ResolutionSide::eMine);
    }

    // A node's base state, put back on a node another side kept, may be what the node already
    // holds; only the live graph tells.
    bool
    baseChangesNothing(const ProjectPtr& project,
                       const Serialization::MergeConflict& conflict)
    {
        if ((conflict.kind != Serialization::MergeConflictKind::eDeleteModify) || !conflict.item.empty() || !conflict.knob.empty()) {
            return false;
        }
        const SnapshotEdit edit = [&conflict](Serialization::ProjectSerialization* snapshot, std::string* message) {
            return Serialization::applyConflictSide(conflict, Serialization::ResolutionSide::eBase, snapshot, message);
        };
        std::vector<Serialization::GraphOp> ops;

        return stateOperations(project, conflict, conflict.base, edit, &ops, nullptr) && ops.empty();
    }

    std::vector<Serialization::MergeConflictChoice>
    offeredChoices(const ProjectPtr& project,
                   const Serialization::MergeConflict& conflict)
    {
        const Serialization::ChoiceFilter applicable = [&project, &conflict](const Serialization::MergeConflictChoice& choice) {
            if (keepsWiring(conflict, choice)) {
                return true;
            }
            const bool base = choice.option.empty() && (choice.side == Serialization::ResolutionSide::eBase);

            return sideAvailable(project, conflict, Serialization::choiceState(conflict, choice), base) && !(base && baseChangesNothing(project, conflict));
        };

        return Serialization::availableChoices(conflict, applicable);
    }

} // namespace

std::vector<Serialization::OpResult>
applyAllOrNothing(const ProjectPtr& project,
                  const std::vector<Serialization::GraphOp>& ops,
                  bool* restored)
{
    assert(QThread::currentThread() == qApp->thread());
    if (restored) {
        *restored = true;
    }
    Serialization::ProjectSerialization live;
    project->toSerialization(&live);
    const Serialization::ProjectSerialization before = Serialization::editableState(live);
    std::vector<Serialization::OpResult> results = applyGraphOps(project, ops);
    if (!allApplied(ops, results)) {
        const bool back = restoreState(project, before);
        if (restored) {
            *restored = back;
        }
    }

    return results;
}

const Serialization::NodeSerialization*
ChangeSummary::otherNode(const std::string& nodeId) const
{
    if (!other || nodeId.empty()) {
        return nullptr;
    }
    std::vector<const std::list<Serialization::NodeSerializationPtr>*> pending(1, &other->nodes.nodes);
    while (!pending.empty()) {
        const std::list<Serialization::NodeSerializationPtr>* nodes = pending.back();
        pending.pop_back();
        for (const Serialization::NodeSerializationPtr& node : *nodes) {
            if (!node) {
                continue;
            }
            if (node->id == nodeId) {
                return node.get();
            }
            pending.push_back(&node->children);
        }
    }

    return nullptr;
}

const NodeChange*
ChangeSummary::find(const std::string& nodeId) const
{
    for (const NodeChange& change : nodes) {
        if (change.nodeId == nodeId) {
            return &change;
        }
    }

    return nullptr;
}

namespace {
    typedef std::pair<double, double> Offset;

    // How far the nodes of one snapshot moved in another, by node ID and by group ID. A node
    // counts when it has the same ID and group on both sides and a position on both.
    struct LayoutShift {
        std::map<std::string, Offset> nodes;
        std::map<std::string, Offset> groups;

        Offset groupOffset(const std::string& groupId) const
        {
            std::map<std::string, Offset>::const_iterator found = groups.find(groupId);

            return (found == groups.end()) ? Offset(0., 0.) : found->second;
        }
    };

    double
    median(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        const std::size_t middle = values.size() / 2;

        return (values.size() % 2) ? values[middle] : 0.5 * (values[middle - 1] + values[middle]);
    }

    // The median keeps a few nodes the artist moved within the template from dragging the offset
    // of the whole template.
    LayoutShift
    layoutShift(const FlatNodes& from,
                const FlatNodes& to)
    {
        LayoutShift shift;
        std::map<std::string, std::pair<std::vector<double>, std::vector<double>>> byGroup;
        for (const std::pair<const std::string, FlatNode>& entry : from) {
            FlatNodes::const_iterator other = to.find(entry.first);
            if ((other == to.end()) || (other->second.parentId != entry.second.parentId)) {
                continue;
            }
            const Serialization::NodeLayoutSerialization& before = entry.second.node->layout;
            const Serialization::NodeLayoutSerialization& after = other->second.node->layout;
            if (!before.hasPosition || !after.hasPosition) {
                continue;
            }
            const Offset offset(after.x - before.x, after.y - before.y);
            shift.nodes[entry.first] = offset;
            std::pair<std::vector<double>, std::vector<double>>& values = byGroup[entry.second.parentId];
            values.first.push_back(offset.first);
            values.second.push_back(offset.second);
        }
        for (const std::pair<const std::string, std::pair<std::vector<double>, std::vector<double>>>& group : byGroup) {
            shift.groups[group.first] = Offset(median(group.second.first), median(group.second.second));
        }

        return shift;
    }

    // Each node's neighbours by wiring, sources and readers alike, within its own group.
    typedef std::map<std::string, std::vector<std::string>> Wiring;

    Wiring
    wiringOf(const FlatNodes& nodes)
    {
        std::map<std::pair<std::string, std::string>, std::string> byName;
        for (const std::pair<const std::string, FlatNode>& entry : nodes) {
            byName.emplace(std::make_pair(entry.second.parentId, entry.second.node->scriptName), entry.first);
        }
        Wiring wiring;
        for (const std::pair<const std::string, FlatNode>& entry : nodes) {
            for (const std::pair<const std::string, std::string>& input : entry.second.node->inputs) {
                std::map<std::pair<std::string, std::string>, std::string>::const_iterator source = byName.find(std::make_pair(entry.second.parentId, input.second));
                if ((source != byName.end()) && (source->second != entry.first)) {
                    wiring[entry.first].push_back(source->second);
                    wiring[source->second].push_back(entry.first);
                }
            }
        }

        return wiring;
    }

    // The offset of the shifted node nearest to id by wiring, looking through unshifted ones, or
    // the offset of id's group when none is reachable.
    Offset
    placementOffset(const FlatNodes& nodes,
                    const Wiring& wiring,
                    const LayoutShift& shift,
                    const std::string& id)
    {
        std::set<std::string> visited;
        std::deque<std::string> pending;
        visited.insert(id);
        pending.push_back(id);
        while (!pending.empty()) {
            const std::string current = pending.front();
            pending.pop_front();
            std::map<std::string, Offset>::const_iterator shifted = shift.nodes.find(current);
            if (shifted != shift.nodes.end()) {
                return shifted->second;
            }
            Wiring::const_iterator next = wiring.find(current);
            if (next == wiring.end()) {
                continue;
            }
            for (const std::string& neighbour : next->second) {
                if (visited.insert(neighbour).second) {
                    pending.push_back(neighbour);
                }
            }
        }
        FlatNodes::const_iterator found = nodes.find(id);

        return shift.groupOffset((found == nodes.end()) ? std::string() : found->second.parentId);
    }

    // A node without a shift, such as one moved to another group or given or stripped of a
    // position, moved by definition.
    bool
    movedWithinGroup(const LayoutShift& shift,
                     const FlatNodes& to,
                     const std::string& id)
    {
        std::map<std::string, Offset>::const_iterator shifted = shift.nodes.find(id);
        FlatNodes::const_iterator node = to.find(id);
        if ((shifted == shift.nodes.end()) || (node == to.end())) {
            return true;
        }
        const Offset group = shift.groupOffset(node->second.parentId);

        return std::hypot(shifted->second.first - group.first, shifted->second.second - group.second) > kMovedThreshold;
    }

    void
    placeNodesIn(const std::list<Serialization::NodeSerializationPtr>& nodes,
                 const std::function<void(Serialization::NodeSerialization*)>& place)
    {
        for (const Serialization::NodeSerializationPtr& node : nodes) {
            if (node) {
                place(node.get());
                placeNodesIn(node->children, place);
            }
        }
    }

    // The merge gives a node theirs adds theirs' position, which is left behind wherever the
    // artist moved the template; it follows its nearest matched neighbour from theirs to where the
    // merge puts that neighbour instead.
    void
    placeAddedNodes(const Serialization::ProjectSerialization& theirs,
                    const Serialization::MergePlan& plan,
                    const FlatNodes& mineNodes,
                    Serialization::ProjectSerialization* merged)
    {
        Serialization::NodeCorrespondence theirsToMine;
        for (const std::pair<const std::string, std::string>& entry : plan.baseToTheirs.baseToOther) {
            const std::string mineId = plan.baseToMine.otherFor(entry.first);
            if (!mineId.empty()) {
                theirsToMine.baseToOther[entry.second] = mineId;
            }
        }
        Serialization::ProjectSerialization mapped = Serialization::editableState(theirs);
        remapNodeIds(mapped.nodes.nodes, theirsToMine);
        FlatNodes theirsNodes;
        std::vector<std::string> theirsOrder;
        flatten(mapped.nodes.nodes, std::string(), &theirsNodes, &theirsOrder);
        FlatNodes mergedNodes;
        std::vector<std::string> mergedOrder;
        flatten(merged->nodes.nodes, std::string(), &mergedNodes, &mergedOrder);
        const LayoutShift shift = layoutShift(theirsNodes, mergedNodes);
        const Wiring wiring = wiringOf(theirsNodes);

        placeNodesIn(merged->nodes.nodes, [&](Serialization::NodeSerialization* node) {
            if (node->id.empty() || (mineNodes.find(node->id) != mineNodes.end()) || !node->layout.hasPosition) {
                return;
            }
            if (!node->origin.isSet() || (node->origin.templateKey != plan.templateKey)) {
                return;
            }
            const std::string& theirsId = node->origin.nodeId;
            FlatNodes::const_iterator source = theirsNodes.find(theirsId);
            if ((source == theirsNodes.end()) || !source->second.node->layout.hasPosition) {
                return;
            }
            const Offset offset = placementOffset(theirsNodes, wiring, shift, theirsId);
            node->layout.x = source->second.node->layout.x + offset.first;
            node->layout.y = source->second.node->layout.y + offset.second;
        });
    }
} // anon namespace

UpdateResult
updateFromTemplate(const ProjectPtr& project,
                   const std::string& templateKey,
                   const QString& newPath,
                   const OpApplier& applier)
{
    assert(QThread::currentThread() == qApp->thread());
    UpdateResult result;

    const Serialization::TemplateImportSerialization* found = nullptr;
    const std::list<Serialization::TemplateImportSerialization> imports = project->getTemplateImports();
    for (const Serialization::TemplateImportSerialization& entry : imports) {
        if (entry.key == templateKey) {
            found = &entry;
            break;
        }
    }
    if (!found) {
        result.error = QCoreApplication::translate("ProjectMerge", "The project has no template import named %1").arg(QString::fromStdString(templateKey));

        return result;
    }
    if (!found->base) {
        result.error = QCoreApplication::translate("ProjectMerge", "The template import %1 has no stored base to merge from").arg(QString::fromStdString(templateKey));

        return result;
    }
    const Serialization::TemplateImportSerialization entry = *found;
    const std::string path = newPath.isEmpty() ? entry.path : newPath.toStdString();
    if (path.empty()) {
        result.error = QCoreApplication::translate("ProjectMerge", "The template import %1 has no file to update from").arg(QString::fromStdString(templateKey));

        return result;
    }

    Serialization::ProjectSerialization theirs;
    Serialization::MergeOptions options;
    try {
        Serialization::ProjectBundleReader bundle(path);
        Serialization::YamlWarnings warnings;
        theirs = bundle.readProject(&warnings);
        options.templateContentHash = Serialization::sha256Hex(bundle.projectYaml());
    } catch (const std::exception& e) {
        result.error = QCoreApplication::translate("ProjectMerge", "%1 cannot be read as a template: %2").arg(QString::fromStdString(path)).arg(QString::fromUtf8(e.what()));

        return result;
    }
    theirs.templates.clear();
    theirs.mergeConflicts.clear();
    options.templatePath = QFileInfo(QString::fromStdString(path)).absoluteFilePath().toStdString();
    options.templateImportedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();

    Serialization::ProjectSerialization mine;
    project->toSerialization(&mine);
    const Serialization::ProjectSerialization& base = *entry.base;
    const Serialization::MergePlan plan = Serialization::planMerge(base, mine, theirs, templateKey);
    Serialization::MergeResult merged = Serialization::threeWayMerge(base, mine, theirs, plan, options);
    const std::vector<Serialization::MergeConflict> toSave = Serialization::conflictsToSave(base, mine, theirs, plan, merged);

    const Serialization::TemplateImportSerialization* updated = nullptr;
    for (const Serialization::TemplateImportSerialization& candidate : merged.merged.templates) {
        if (candidate.key == templateKey) {
            updated = &candidate;
            break;
        }
    }
    if (!updated) {
        result.error = QCoreApplication::translate("ProjectMerge", "The merge dropped the template import %1").arg(QString::fromStdString(templateKey));

        return result;
    }

    const Serialization::ProjectSerialization before = Serialization::editableState(mine);
    Serialization::ProjectSerialization after = Serialization::editableState(merged.merged);
    FlatNodes mineNodes;
    std::vector<std::string> mineOrder;
    flatten(before.nodes.nodes, std::string(), &mineNodes, &mineOrder);
    keepMineDefaultKnobs(mineNodes, after.nodes.nodes);
    keepMineDefaultKnobs(before.projectKnobs, &after.projectKnobs);
    placeAddedNodes(theirs, plan, mineNodes, &after);
    result.ops = Serialization::diffSnapshots(before, after);
    if (!result.ops.empty()) {
        result.results = applier ? applier(result.ops) : applyAllOrNothing(project, result.ops);
    }
    QStringList problems;
    for (const Serialization::OpResult& op : result.results) {
        if (!op.applied()) {
            problems.push_back(QString::fromStdString(op.message));
        }
    }
    if (result.results.size() != result.ops.size()) {
        problems.push_back(QCoreApplication::translate("ProjectMerge", "the operations were not all applied"));
    }
    if (!problems.isEmpty()) {
        // An applier may stop partway; the update is all or nothing.
        if (!restoreState(project, before)) {
            problems.push_back(QCoreApplication::translate("ProjectMerge", "the operations that did apply could not all be undone"));
        }
        result.error = QCoreApplication::translate("ProjectMerge", "The update could not be applied: %1").arg(problems.join(QString::fromUtf8("; ")));

        return result;
    }

    project->setTemplateImport(*updated);
    project->setMergeConflicts(toSave);
    result.conflicts = std::move(merged.conflicts);
    result.ok = true;

    return result;
}

ChangeSummary
compare(const ProjectPtr& project,
        const Serialization::ProjectSerialization& other)
{
    assert(QThread::currentThread() == qApp->thread());
    Serialization::ProjectSerialization live;
    project->toSerialization(&live);
    Serialization::ProjectSerialization before = Serialization::editableState(other);
    Serialization::ProjectSerialization after = Serialization::editableState(live);
    toSavedKnobs(&before);
    toSavedKnobs(&after);

    FlatNodes beforeNodes;
    FlatNodes afterNodes;
    std::vector<std::string> beforeOrder;
    std::vector<std::string> afterOrder;
    flatten(before.nodes.nodes, std::string(), &beforeNodes, &beforeOrder);
    flatten(after.nodes.nodes, std::string(), &afterNodes, &afterOrder);
    const LayoutShift shift = layoutShift(beforeNodes, afterNodes);

    std::map<std::string, NodeChange> changed;
    ChangeSummary summary;
    auto changeFor = [&](const std::string& id) -> NodeChange* {
        if ((beforeNodes.find(id) == beforeNodes.end()) || (afterNodes.find(id) == afterNodes.end())) {
            return nullptr;
        }
        auto it = changed.find(id);
        if (it == changed.end()) {
            it = changed.emplace(id, describe(afterNodes.at(id), NodeChange::eChanged)).first;
        }

        return &it->second;
    };

    for (const Serialization::GraphOp& op : Serialization::diffSnapshots(before, after)) {
        if (const Serialization::SetKnobOp* knob = std::get_if<Serialization::SetKnobOp>(&op)) {
            if (!knob->nodeId) {
                addUnique(&summary.projectKnobs, knob->knobName);
            } else if (NodeChange* change = changeFor(*knob->nodeId)) {
                addUnique(&change->knobs, knob->knobName);
            }
        } else if (const Serialization::SetInputOp* input = std::get_if<Serialization::SetInputOp>(&op)) {
            if (NodeChange* change = changeFor(input->nodeId)) {
                addUnique(&change->inputs, input->inputLabel);
            }
        } else if (const Serialization::RenameOp* rename = std::get_if<Serialization::RenameOp>(&op)) {
            if (NodeChange* change = changeFor(rename->nodeId)) {
                change->renamed = true;
            }
        } else if (const Serialization::SetNodeUiOp* ui = std::get_if<Serialization::SetNodeUiOp>(&op)) {
            const bool appearance = ui->size || ui->color || ui->overlayColor;
            const bool moved = ui->position && movedWithinGroup(shift, afterNodes, ui->nodeId);
            if (appearance || moved) {
                if (NodeChange* change = changeFor(ui->nodeId)) {
                    change->appearance = change->appearance || appearance;
                    change->moved = change->moved || moved;
                }
            }
        } else if (const Serialization::SetUserKnobsOp* userKnobs = std::get_if<Serialization::SetUserKnobsOp>(&op)) {
            if (NodeChange* change = changeFor(userKnobs->nodeId)) {
                change->userKnobs = true;
            }
        } else if (const Serialization::SetRotoDataOp* roto = std::get_if<Serialization::SetRotoDataOp>(&op)) {
            if (NodeChange* change = changeFor(roto->nodeId)) {
                change->roto = true;
            }
        } else if (const Serialization::SetTrackerDataOp* tracker = std::get_if<Serialization::SetTrackerDataOp>(&op)) {
            if (NodeChange* change = changeFor(tracker->nodeId)) {
                change->tracker = true;
            }
        } else if (const Serialization::DeleteNodeOp* deleted = std::get_if<Serialization::DeleteNodeOp>(&op)) {
            if (NodeChange* change = changeFor(deleted->nodeId)) {
                change->recreated = true;
            }
        } else if (const Serialization::CreateNodeOp* created = std::get_if<Serialization::CreateNodeOp>(&op)) {
            std::vector<const Serialization::NodeSerialization*> pending(1, created->node.get());
            while (!pending.empty()) {
                const Serialization::NodeSerialization* node = pending.back();
                pending.pop_back();
                if (!node) {
                    continue;
                }
                if (NodeChange* change = changeFor(node->id)) {
                    change->recreated = true;
                }
                for (const Serialization::NodeSerializationPtr& child : node->children) {
                    pending.push_back(child.get());
                }
            }
        } else if (std::holds_alternative<Serialization::SetProjectDataOp>(op)) {
            summary.projectData = true;
        }
    }

    // The diff leaves out an input whose source is deleted, since deleting the source
    // disconnects it; the node still lost that input.
    for (const std::pair<const std::string, FlatNode>& entry : afterNodes) {
        FlatNodes::const_iterator old = beforeNodes.find(entry.first);
        if (old == beforeNodes.end()) {
            continue;
        }
        std::set<std::string> labels;
        for (const FlatNode* flat : { &old->second, &entry.second }) {
            for (const std::pair<const std::string, std::string>& input : flat->node->inputs) {
                labels.insert(input.first);
            }
        }
        for (const std::string& label : labels) {
            if (inputSourceOf(beforeNodes, old->second, label) != inputSourceOf(afterNodes, entry.second, label)) {
                if (NodeChange* change = changeFor(entry.first)) {
                    addUnique(&change->inputs, label);
                }
            }
        }
    }

    for (const std::string& id : afterOrder) {
        if (beforeNodes.find(id) == beforeNodes.end()) {
            summary.nodes.push_back(describe(afterNodes.at(id), NodeChange::eAdded));
        } else if (auto it = changed.find(id); it != changed.end()) {
            describeChange(beforeNodes.at(id), afterNodes.at(id), &it->second);
            summary.nodes.push_back(it->second);
        }
    }
    const Wiring beforeWiring = wiringOf(beforeNodes);
    for (const std::string& id : beforeOrder) {
        if (afterNodes.find(id) == afterNodes.end()) {
            NodeChange removed = describe(beforeNodes.at(id), NodeChange::eRemoved);
            describeLinks(beforeNodes, beforeNodes.at(id), &removed);
            const Offset offset = placementOffset(beforeNodes, beforeWiring, shift, id);
            removed.placedX = removed.x + offset.first;
            removed.placedY = removed.y + offset.second;
            summary.nodes.push_back(std::move(removed));
        }
    }
    summary.other = std::make_shared<const Serialization::ProjectSerialization>(std::move(before));
    summary.live = std::make_shared<const Serialization::ProjectSerialization>(std::move(after));

    return summary;
}

namespace {
    void
    collectNodeIds(const std::list<Serialization::NodeSerializationPtr>& nodes,
                   std::set<std::string>* ids)
    {
        for (const Serialization::NodeSerializationPtr& node : nodes) {
            if (node) {
                ids->insert(node->id);
                collectNodeIds(node->children, ids);
            }
        }
    }

    void
    countMatches(const std::list<Serialization::NodeSerializationPtr>& nodes,
                 const std::set<std::string>& otherIds,
                 int* byId,
                 std::map<std::string, int>* byOrigin)
    {
        for (const Serialization::NodeSerializationPtr& node : nodes) {
            if (!node) {
                continue;
            }
            if (otherIds.count(node->id)) {
                ++*byId;
            }
            if (node->origin.isSet() && otherIds.count(node->origin.nodeId)) {
                ++(*byOrigin)[node->origin.templateKey];
            }
            countMatches(node->children, otherIds, byId, byOrigin);
        }
    }

    // An import's nodes get fresh IDs, so a version of the template file shares none with them;
    // they name the file's nodes through their origins instead.
    std::string
    templateVersionKey(const Serialization::ProjectSerialization& mine,
                       const Serialization::ProjectSerialization& other,
                       const std::list<Serialization::TemplateImportSerialization>& imports)
    {
        std::set<std::string> otherIds;
        collectNodeIds(other.nodes.nodes, &otherIds);
        int byId = 0;
        std::map<std::string, int> byOrigin;
        countMatches(mine.nodes.nodes, otherIds, &byId, &byOrigin);
        std::string best;
        int bestCount = byId;
        for (const Serialization::TemplateImportSerialization& entry : imports) {
            std::map<std::string, int>::const_iterator found = byOrigin.find(entry.key);
            if ((found != byOrigin.end()) && (found->second > bestCount)) {
                best = entry.key;
                bestCount = found->second;
            }
        }

        return best;
    }

    ChangeSummary
    compareWithTemplateVersion(const ProjectPtr& project,
                               const Serialization::ProjectSerialization& mine,
                               const Serialization::ProjectSerialization& version,
                               const std::string& templateKey)
    {
        const Serialization::MergePlan plan = Serialization::planMerge(version, mine, version, templateKey);
        Serialization::ProjectSerialization other = Serialization::editableState(version);
        remapNodeIds(other.nodes.nodes, plan.baseToMine);

        ChangeSummary all = compare(project, other);
        ChangeSummary summary;
        summary.other = all.other;
        summary.live = all.live;
        summary.scope = std::make_shared<const std::set<std::string>>(plan.scope);
        for (NodeChange& change : all.nodes) {
            if ((change.kind == NodeChange::eRemoved) || (plan.scope.find(change.nodeId) != plan.scope.end())) {
                summary.nodes.push_back(std::move(change));
            }
        }

        return summary;
    }
} // anon namespace

ChangeSummary
recompare(const ProjectPtr& project,
          const ChangeSummary& previous)
{
    if (!previous.other) {
        return ChangeSummary();
    }
    ChangeSummary all = compare(project, *previous.other);
    if (!previous.scope) {
        return all;
    }
    ChangeSummary summary;
    summary.other = all.other;
    summary.live = all.live;
    summary.scope = previous.scope;
    // A node restored since is in other under its own ID, so it is matched and stays listed.
    for (NodeChange& change : all.nodes) {
        if ((change.kind != NodeChange::eAdded) || previous.scope->count(change.nodeId)) {
            summary.nodes.push_back(std::move(change));
        }
    }

    return summary;
}

bool
compareWithFile(const ProjectPtr& project,
                const QString& path,
                ChangeSummary* summary,
                QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    Serialization::ProjectSerialization other;
    if (!readSnapshot(path, &other, error)) {
        return false;
    }
    Serialization::ProjectSerialization mine;
    project->toSerialization(&mine);
    const std::string templateKey = templateVersionKey(mine, other, project->getTemplateImports());
    if (templateKey.empty()) {
        *summary = compare(project, other);
    } else {
        other.templates.clear();
        *summary = compareWithTemplateVersion(project, mine, other, templateKey);
    }

    return true;
}

bool
compareWithTemplateBase(const ProjectPtr& project,
                        const std::string& templateKey,
                        ChangeSummary* summary,
                        QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    const std::list<Serialization::TemplateImportSerialization> imports = project->getTemplateImports();
    const Serialization::TemplateImportSerialization* found = nullptr;
    for (const Serialization::TemplateImportSerialization& entry : imports) {
        if (entry.key == templateKey) {
            found = &entry;
            break;
        }
    }
    if (!found || !found->base) {
        if (error) {
            *error = QCoreApplication::translate("ProjectMerge", "The project has no stored base for the template import %1").arg(QString::fromStdString(templateKey));
        }

        return false;
    }

    Serialization::ProjectSerialization mine;
    project->toSerialization(&mine);
    *summary = compareWithTemplateVersion(project, mine, *found->base, templateKey);

    return true;
}

QString
conflictLabel(const Serialization::MergeConflict& conflict)
{
    const QString knob = QString::fromStdString(conflict.knob);
    const QString item = QString::fromStdString(conflict.item);
    switch (conflict.kind) {
    case Serialization::MergeConflictKind::eValue:
        if (conflict.dimension == Serialization::kMergeConflictAllDimensions) {
            return QCoreApplication::translate("ProjectMerge", "%1, all dimensions").arg(knob);
        }
        if (conflict.dimension < 0) {
            return QCoreApplication::translate("ProjectMerge", "%1 (animation, options)").arg(knob);
        }

        return QCoreApplication::translate("ProjectMerge", "%1, dimension %2").arg(knob).arg(conflict.dimension);
    case Serialization::MergeConflictKind::eCurveKey:
        return QCoreApplication::translate("ProjectMerge", "%1, key at %2").arg(knob).arg(conflict.time ? *conflict.time : 0.);
    case Serialization::MergeConflictKind::eInput:
        return QCoreApplication::translate("ProjectMerge", "Input %1 connected differently").arg(QString::fromStdString(conflict.input));
    case Serialization::MergeConflictKind::eSpliceChoice:
        return QCoreApplication::translate("ProjectMerge", "Input %1 rewired since the restored node was removed").arg(QString::fromStdString(conflict.input));
    case Serialization::MergeConflictKind::eStructuralInsert:
        return QCoreApplication::translate("ProjectMerge", "Nodes inserted on the same link on both sides");
    case Serialization::MergeConflictKind::eDeleteModify: {
        const QString who = (conflict.side == Serialization::MergeSide::eMine) ? QCoreApplication::translate("ProjectMerge", "this project") : QCoreApplication::translate("ProjectMerge", "the template");
        if (!knob.isEmpty()) {
            return QCoreApplication::translate("ProjectMerge", "%1 deleted in %2 and changed in the other").arg(knob, who);
        }
        if (!item.isEmpty()) {
            return QCoreApplication::translate("ProjectMerge", "%1 deleted in %2 and changed in the other").arg(item, who);
        }

        return QCoreApplication::translate("ProjectMerge", "Node deleted in %1 and changed in the other").arg(who);
    }
    case Serialization::MergeConflictKind::eReparent:
        return QCoreApplication::translate("ProjectMerge", "Moved into different groups on both sides");
    case Serialization::MergeConflictKind::eAdopted:
        return QCoreApplication::translate("ProjectMerge", "Matched to the template node by structure");
    case Serialization::MergeConflictKind::eRotoShape:
        return item.isEmpty() ? QCoreApplication::translate("ProjectMerge", "Spline warp changed on both sides")
                              : QCoreApplication::translate("ProjectMerge", "Shape %1 changed on both sides").arg(item);
    }

    return QString();
}

std::vector<ConflictChoice>
availableChoices(const ProjectPtr& project,
                 const Serialization::MergeConflict& conflict)
{
    std::vector<ConflictChoice> choices;
    for (const Serialization::MergeConflictChoice& offered : offeredChoices(project, conflict)) {
        ConflictChoice choice;
        choice.side = conflictSide(offered.side);
        choice.option = offered.option;
        if (!offered.option.empty()) {
            choice.label = QString::fromStdString(offered.label);
        } else if (conflict.kind == Serialization::MergeConflictKind::eSpliceChoice) {
            choice.label = (choice.side == ConflictSide::eMine) ? QCoreApplication::translate("ProjectMerge", "Keep My Wiring") : QCoreApplication::translate("ProjectMerge", "Splice In");
        } else if (choice.side == ConflictSide::eMine) {
            choice.label = QCoreApplication::translate("ProjectMerge", "Mine");
        } else if (choice.side == ConflictSide::eTheirs) {
            choice.label = QCoreApplication::translate("ProjectMerge", "Theirs");
        } else {
            choice.label = QCoreApplication::translate("ProjectMerge", "Base");
        }
        choices.push_back(std::move(choice));
    }

    return choices;
}

std::string
choiceName(const ConflictChoice& choice)
{
    if (!choice.option.empty()) {
        return choice.option;
    }
    switch (choice.side) {
    case ConflictSide::eMine:
        return "mine";
    case ConflictSide::eTheirs:
        return "theirs";
    case ConflictSide::eBase:
        break;
    }

    return "base";
}

bool
findChoice(const ProjectPtr& project,
           const Serialization::MergeConflict& conflict,
           const std::string& name,
           ConflictChoice* choice)
{
    for (const ConflictChoice& offered : availableChoices(project, conflict)) {
        if (choiceName(offered) == name) {
            *choice = offered;

            return true;
        }
    }

    return false;
}

bool
canChooseSide(const ProjectPtr& project,
              const Serialization::MergeConflict& conflict,
              ConflictSide side)
{
    const std::vector<Serialization::MergeConflictChoice> offered = offeredChoices(project, conflict);

    return std::any_of(offered.begin(), offered.end(), [side](const Serialization::MergeConflictChoice& choice) {
        return choice.option.empty() && (conflictSide(choice.side) == side);
    });
}

bool
sideOperations(const ProjectPtr& project,
               const Serialization::MergeConflict& conflict,
               ConflictSide side,
               std::vector<Serialization::GraphOp>* ops,
               QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    ops->clear();
    if (!canChooseSide(project, conflict, side)) {
        return failWith(error, QCoreApplication::translate("ProjectMerge", "This side of the conflict cannot be put back: the merge's choice can only be accepted"));
    }

    const SnapshotEdit edit = [&conflict, side](Serialization::ProjectSerialization* snapshot, std::string* message) {
        return Serialization::applyConflictSide(conflict, resolutionSide(side), snapshot, message);
    };

    return stateOperations(project, conflict, valueOf(conflict, side), edit, ops, error);
}

bool
canChooseOption(const ProjectPtr& project,
                const Serialization::MergeConflict& conflict,
                const std::string& option)
{
    const std::vector<Serialization::MergeConflictChoice> offered = offeredChoices(project, conflict);

    return !option.empty() && std::any_of(offered.begin(), offered.end(), [&option](const Serialization::MergeConflictChoice& choice) {
        return choice.option == option;
    });
}

bool
optionOperations(const ProjectPtr& project,
                 const Serialization::MergeConflict& conflict,
                 const std::string& option,
                 std::vector<Serialization::GraphOp>* ops,
                 QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    ops->clear();
    if (!canChooseOption(project, conflict, option)) {
        return failWith(error, QCoreApplication::translate("ProjectMerge", "This conflict has no choice %1 that can be applied").arg(QString::fromStdString(option)));
    }

    const SnapshotEdit edit = [&conflict, &option](Serialization::ProjectSerialization* snapshot, std::string* message) {
        return Serialization::applyConflictOption(conflict, option, snapshot, message);
    };

    return stateOperations(project, conflict, Serialization::conflictOption(conflict, option)->state, edit, ops, error);
}

namespace {
    typedef std::list<Serialization::NodeSerializationPtr> NodeList;

    struct EditableNode {
        Serialization::NodeSerializationPtr node;
        NodeList* siblings = nullptr;
    };

    bool
    findEditable(NodeList* nodes,
                 const std::string& id,
                 EditableNode* found)
    {
        for (const Serialization::NodeSerializationPtr& node : *nodes) {
            if (!node) {
                continue;
            }
            if (node->id == id) {
                found->node = node;
                found->siblings = nodes;

                return true;
            }
            if (findEditable(&node->children, id, found)) {
                return true;
            }
        }

        return false;
    }

    // The list holding the nodes of group parentId: the top level, or the group's children.
    NodeList*
    nodesOfGroup(Serialization::ProjectSerialization* snapshot,
                 const std::string& parentId)
    {
        if (parentId.empty()) {
            return &snapshot->nodes.nodes;
        }
        EditableNode group;

        return findEditable(&snapshot->nodes.nodes, parentId, &group) ? &group.node->children : nullptr;
    }

    Serialization::NodeSerializationPtr
    copyNode(const Serialization::NodeSerialization& node)
    {
        Serialization::NodeSerializationPtr copy = std::make_shared<Serialization::NodeSerialization>(node);
        for (Serialization::NodeSerializationPtr& child : copy->children) {
            if (child) {
                child = copyNode(*child);
            }
        }

        return copy;
    }

    // The group's script names from the top level down, joined by '.', as node references name it.
    std::string
    qualifiedGroupName(const FlatNodes& nodes,
                       const std::string& groupId)
    {
        std::string name;
        for (std::string id = groupId; !id.empty();) {
            FlatNodes::const_iterator found = nodes.find(id);
            if (found == nodes.end()) {
                break;
            }
            name = name.empty() ? found->second.node->scriptName : (found->second.node->scriptName + "." + name);
            id = found->second.parentId;
        }

        return name;
    }

    std::string
    freshScriptName(const std::string& name,
                    const std::set<std::string>& taken)
    {
        if (!taken.count(name)) {
            return name;
        }
        std::string stem = name;
        while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) {
            stem.pop_back();
        }
        if (stem.empty()) {
            stem = name + "_";
        }
        for (int index = 1;; ++index) {
            const std::string candidate = stem + std::to_string(index);
            if (!taken.count(candidate)) {
                return candidate;
            }
        }
    }

    // The live nodes that id read from in the other snapshot, looking through the sources that are
    // not live: where deleting id and the nodes between left its readers connected.
    std::set<std::string>
    liveUpstream(const FlatNodes& other,
                 const FlatNodes& live,
                 const std::string& id)
    {
        std::set<std::string> found;
        std::set<std::string> visited;
        std::deque<std::string> pending(1, id);
        visited.insert(id);
        while (!pending.empty()) {
            const FlatNode& node = other.at(pending.front());
            pending.pop_front();
            for (const std::pair<const std::string, std::string>& input : node.node->inputs) {
                const std::string source = idOfScriptName(other, node.parentId, input.second);
                if (source.empty() || !visited.insert(source).second) {
                    continue;
                }
                if (live.count(source)) {
                    found.insert(source);
                } else {
                    pending.push_back(source);
                }
            }
        }

        return found;
    }

    // The live node of group parentId with that ID, or null.
    const Serialization::NodeSerialization*
    liveSibling(const FlatNodes& live,
                const std::string& parentId,
                const std::string& id)
    {
        FlatNodes::const_iterator found = id.empty() ? live.end() : live.find(id);

        return ((found != live.end()) && (found->second.parentId == parentId)) ? found->second.node : nullptr;
    }

    Serialization::ProjectSerialization
    liveEditableState(const ProjectPtr& project)
    {
        Serialization::ProjectSerialization live;
        project->toSerialization(&live);

        return Serialization::editableState(live);
    }

    bool
    isKind(const ChangeSummary& summary,
           const std::string& nodeId,
           NodeChange::Kind kind)
    {
        const NodeChange* change = summary.find(nodeId);

        return change && (change->kind == kind);
    }

    Serialization::SetKnobOp
    knobOperation(const ProjectPtr& project,
                  const std::string& nodeId,
                  const NodeChange::KnobChange& change)
    {
        Serialization::SetKnobOp set;
        set.nodeId = nodeId;
        set.knobName = change.name;
        if (change.before) {
            // A knob saved in a file holds values typed only by their YAML form until the live
            // knob types them.
            const NodePtr node = project->findNodeById(nodeId);
            const KnobIPtr knob = node ? node->getKnobByName(change.name) : KnobIPtr();
            set.value = knob ? completeKnobSerializationForKnob(*knob, *change.before) : *change.before;
        }

        return set;
    }
} // anon namespace

bool
restoreOperations(const ProjectPtr& project,
                  const ChangeSummary& summary,
                  const std::vector<std::string>& nodeIds,
                  SpliceMode mode,
                  std::vector<Serialization::GraphOp>* ops,
                  std::vector<PendingSplice>* pending,
                  QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    ops->clear();
    pending->clear();
    if (!summary.other) {
        return failWith(error, QCoreApplication::translate("ProjectMerge", "The comparison holds no other version to restore from"));
    }
    const Serialization::ProjectSerialization before = liveEditableState(project);
    Serialization::ProjectSerialization after = Serialization::editableState(before);
    FlatNodes other;
    FlatNodes live;
    std::vector<std::string> otherOrder;
    std::vector<std::string> liveOrder;
    flatten(summary.other->nodes.nodes, std::string(), &other, &otherOrder);
    flatten(after.nodes.nodes, std::string(), &live, &liveOrder);

    const std::set<std::string> wanted(nodeIds.begin(), nodeIds.end());
    const auto inWanted = [&](const std::string& id) {
        return wanted.count(id) && other.count(id) && !live.count(id);
    };
    std::vector<std::string> roots;
    std::set<std::string> restored;
    for (const std::string& id : otherOrder) {
        if (!inWanted(id)) {
            continue;
        }
        bool insideRestored = false;
        for (std::string parent = other.at(id).parentId; !parent.empty(); parent = other.count(parent) ? other.at(parent).parentId : std::string()) {
            if (inWanted(parent)) {
                insideRestored = true;
                break;
            }
        }
        if (insideRestored) {
            continue;
        }
        const std::string& parentId = other.at(id).parentId;
        if (!parentId.empty() && !live.count(parentId)) {
            return failWith(error, QCoreApplication::translate("ProjectMerge", "%1 cannot be restored without the group it is in").arg(QString::fromStdString(other.at(id).node->scriptName)));
        }
        std::vector<const Serialization::NodeSerialization*> below(1, other.at(id).node);
        while (!below.empty()) {
            const Serialization::NodeSerialization* node = below.back();
            below.pop_back();
            if (!node->id.empty() && live.count(node->id)) {
                return failWith(error, QCoreApplication::translate("ProjectMerge", "%1 cannot be restored: %2 is already in the project").arg(QString::fromStdString(other.at(id).node->scriptName)).arg(QString::fromStdString(node->scriptName)));
            }
            if (!node->id.empty()) {
                restored.insert(node->id);
            }
            for (const Serialization::NodeSerializationPtr& child : node->children) {
                if (child) {
                    below.push_back(child.get());
                }
            }
        }
        roots.push_back(id);
    }

    // Names are given in two passes so a fresh name never takes the name another restored node
    // keeps.
    std::map<std::string, std::set<std::string>> taken;
    for (const std::pair<const std::string, FlatNode>& entry : live) {
        taken[entry.second.parentId].insert(entry.second.node->scriptName);
    }
    std::map<std::string, std::string> names;
    for (const std::string& id : roots) {
        const FlatNode& node = other.at(id);
        if (taken[node.parentId].insert(node.node->scriptName).second) {
            names[id] = node.node->scriptName;
        }
    }
    Serialization::ScopedNodeRenames renames;
    for (const std::string& id : roots) {
        const FlatNode& node = other.at(id);
        if (names.count(id)) {
            continue;
        }
        const std::string fresh = freshScriptName(node.node->scriptName, taken[node.parentId]);
        taken[node.parentId].insert(fresh);
        names[id] = fresh;
        renames[qualifiedGroupName(other, node.parentId)][node.node->scriptName] = fresh;
    }

    const auto nameOfSource = [&](const std::string& parentId, const std::string& sourceName) -> std::string {
        const std::string sourceId = idOfScriptName(other, parentId, sourceName);
        if (sourceId.empty()) {
            return std::string();
        }
        std::map<std::string, std::string>::const_iterator restoredName = names.find(sourceId);
        if (restoredName != names.end()) {
            return restoredName->second;
        }
        const Serialization::NodeSerialization* source = liveSibling(live, parentId, sourceId);

        return source ? source->scriptName : std::string();
    };

    for (const std::string& id : roots) {
        const FlatNode& node = other.at(id);
        Serialization::NodeSerializationPtr copy = copyNode(*node.node);
        if (!renames.empty()) {
            Serialization::renameNodeReferencesRecursive(copy.get(), renames, qualifiedGroupName(other, node.parentId));
        }
        copy->scriptName = names.at(id);
        copy->inputs.clear();
        for (const std::pair<const std::string, std::string>& input : node.node->inputs) {
            const std::string source = nameOfSource(node.parentId, input.second);
            if (!source.empty()) {
                copy->inputs[input.first] = source;
            }
        }
        if (const NodeChange* change = summary.find(id)) {
            copy->layout.hasPosition = true;
            copy->layout.x = change->placedX;
            copy->layout.y = change->placedY;
        }
        NodeList* siblings = nodesOfGroup(&after, node.parentId);
        if (!siblings) {
            return failWith(error, QCoreApplication::translate("ProjectMerge", "%1 cannot be restored without the group it is in").arg(QString::fromStdString(node.node->scriptName)));
        }
        siblings->push_back(copy);
    }

    for (const std::string& id : roots) {
        const FlatNode& node = other.at(id);
        const std::set<std::string> upstream = liveUpstream(other, live, id);
        for (const std::pair<const std::string, FlatNode>& entry : other) {
            if ((entry.second.parentId != node.parentId) || restored.count(entry.first)) {
                continue;
            }
            for (const std::pair<const std::string, std::string>& input : entry.second.node->inputs) {
                if (input.second != node.node->scriptName) {
                    continue;
                }
                EditableNode reader;
                if (!liveSibling(live, node.parentId, entry.first) || !findEditable(&after.nodes.nodes, entry.first, &reader)) {
                    continue;
                }
                std::map<std::string, std::string>::const_iterator current = reader.node->inputs.find(input.first);
                const std::string sourceName = (current == reader.node->inputs.end()) ? std::string() : current->second;
                const std::string sourceId = sourceName.empty() ? std::string() : idOfScriptName(live, node.parentId, sourceName);
                const bool rewired = !sourceName.empty() && !upstream.count(sourceId);
                if (!rewired || (mode == SpliceMode::eEverywhere)) {
                    reader.node->inputs[input.first] = names.at(id);
                } else if (mode == SpliceMode::eAsk) {
                    pending->push_back(PendingSplice { entry.first, input.first, id, sourceId });
                }
            }
        }
    }

    *ops = Serialization::diffSnapshots(before, after);

    return true;
}

std::vector<Serialization::GraphOp>
revertOperations(const ProjectPtr& project,
                 const ChangeSummary& summary,
                 const std::vector<std::string>& nodeIds)
{
    assert(QThread::currentThread() == qApp->thread());
    std::vector<Serialization::GraphOp> ops;
    if (!summary.other) {
        return ops;
    }
    const Serialization::ProjectSerialization before = liveEditableState(project);
    Serialization::ProjectSerialization after = Serialization::editableState(before);
    FlatNodes other;
    FlatNodes live;
    std::vector<std::string> order;
    flatten(summary.other->nodes.nodes, std::string(), &other, &order);
    flatten(after.nodes.nodes, std::string(), &live, &order);
    for (const std::string& id : nodeIds) {
        const NodeChange* change = summary.find(id);
        EditableNode node;
        if (!change || (change->kind != NodeChange::eChanged) || !other.count(id) || !live.count(id) || !findEditable(&after.nodes.nodes, id, &node)) {
            continue;
        }
        for (const NodeChange::KnobChange& knob : change->knobChanges) {
            ops.push_back(knobOperation(project, id, knob));
        }
        const FlatNode& was = other.at(id);
        const std::string& parentId = live.at(id).parentId;
        for (const std::string& label : change->inputs) {
            const Serialization::NodeSerialization* source = liveSibling(live, parentId, idOfScriptName(other, was.parentId, inputNamed(*was.node, label)));
            if (source) {
                node.node->inputs[label] = source->scriptName;
            } else {
                node.node->inputs.erase(label);
            }
        }
        if (change->appearance) {
            const Serialization::NodeLayoutSerialization& layout = was.node->layout;
            node.node->layout.hasSize = layout.hasSize;
            node.node->layout.width = layout.width;
            node.node->layout.height = layout.height;
            node.node->layout.hasColor = layout.hasColor;
            node.node->layout.colorR = layout.colorR;
            node.node->layout.colorG = layout.colorG;
            node.node->layout.colorB = layout.colorB;
            node.node->layout.hasOverlayColor = layout.hasOverlayColor;
            node.node->layout.overlayR = layout.overlayR;
            node.node->layout.overlayG = layout.overlayG;
            node.node->layout.overlayB = layout.overlayB;
        }
    }
    for (Serialization::GraphOp& op : Serialization::diffSnapshots(before, after)) {
        ops.push_back(std::move(op));
    }

    return ops;
}

bool
takeKnobOperations(const ProjectPtr& project,
                   const ChangeSummary& summary,
                   const std::string& nodeId,
                   const std::string& knobName,
                   std::vector<Serialization::GraphOp>* ops)
{
    ops->clear();
    const NodeChange* change = summary.find(nodeId);
    if (!change || (change->kind != NodeChange::eChanged)) {
        return false;
    }
    for (const NodeChange::KnobChange& knob : change->knobChanges) {
        if (knob.name == knobName) {
            ops->push_back(knobOperation(project, nodeId, knob));

            return true;
        }
    }

    return false;
}

std::vector<Serialization::GraphOp>
removeAddedOperations(const ProjectPtr& project,
                      const ChangeSummary& summary,
                      const std::vector<std::string>& nodeIds)
{
    assert(QThread::currentThread() == qApp->thread());
    const Serialization::ProjectSerialization before = liveEditableState(project);
    Serialization::ProjectSerialization after = Serialization::editableState(before);
    for (const std::string& id : nodeIds) {
        EditableNode node;
        if (!isKind(summary, id, NodeChange::eAdded) || !findEditable(&after.nodes.nodes, id, &node)) {
            continue;
        }
        std::string bridge;
        int connected = 0;
        for (const std::pair<const std::string, std::string>& input : node.node->inputs) {
            if (!input.second.empty()) {
                bridge = input.second;
                ++connected;
            }
        }
        if (connected != 1) {
            bridge.clear();
        }
        node.siblings->remove(node.node);
        for (const Serialization::NodeSerializationPtr& reader : *node.siblings) {
            if (!reader) {
                continue;
            }
            for (std::map<std::string, std::string>::iterator input = reader->inputs.begin(); input != reader->inputs.end();) {
                if (input->second != node.node->scriptName) {
                    ++input;
                } else if (bridge.empty()) {
                    input = reader->inputs.erase(input);
                } else {
                    input->second = bridge;
                    ++input;
                }
            }
        }
    }

    return Serialization::diffSnapshots(before, after);
}

std::vector<Serialization::GraphOp>
restorePositionOperations(const ProjectPtr& project,
                          const ChangeSummary& summary,
                          const std::vector<std::string>& nodeIds)
{
    assert(QThread::currentThread() == qApp->thread());
    std::vector<Serialization::GraphOp> ops;
    if (!summary.other || !summary.live) {
        return ops;
    }
    FlatNodes other;
    FlatNodes compared;
    std::vector<std::string> order;
    flatten(summary.other->nodes.nodes, std::string(), &other, &order);
    flatten(summary.live->nodes.nodes, std::string(), &compared, &order);
    const LayoutShift shift = layoutShift(other, compared);
    for (const std::string& id : nodeIds) {
        const NodeChange* change = summary.find(id);
        if (!change || (change->kind != NodeChange::eChanged) || !change->moved || !other.count(id) || !project->findNodeById(id)) {
            continue;
        }
        const Serialization::NodeLayoutSerialization& layout = other.at(id).node->layout;
        const Offset offset = shift.groupOffset(change->parentId);
        Serialization::SetNodeUiOp move;
        move.nodeId = id;
        move.position = layout.hasPosition ? std::optional<std::pair<double, double>>(std::make_pair(layout.x + offset.first, layout.y + offset.second)) : std::optional<std::pair<double, double>>();
        ops.push_back(Serialization::GraphOp(std::move(move)));
    }

    return ops;
}

Serialization::MergeConflict
spliceChoice(const PendingSplice& splice)
{
    Serialization::MergeConflict conflict;
    conflict.kind = Serialization::MergeConflictKind::eSpliceChoice;
    conflict.nodeId = splice.readerId;
    conflict.input = splice.input;
    conflict.mine.connections.push_back(Serialization::MergeConflictConnection { splice.readerId, splice.input, splice.sourceId });
    conflict.theirs.connections.push_back(Serialization::MergeConflictConnection { splice.readerId, splice.input, splice.restoredId });

    return conflict;
}

void
addSpliceChoices(const ProjectPtr& project,
                 const std::vector<PendingSplice>& pending)
{
    if (pending.empty()) {
        return;
    }
    std::vector<Serialization::MergeConflict> conflicts = project->getMergeConflicts();
    for (const PendingSplice& splice : pending) {
        conflicts.erase(std::remove_if(conflicts.begin(), conflicts.end(), [&splice](const Serialization::MergeConflict& conflict) {
                            return (conflict.kind == Serialization::MergeConflictKind::eSpliceChoice) && (conflict.nodeId == splice.readerId) && (conflict.input == splice.input);
                        }),
                        conflicts.end());
        conflicts.push_back(spliceChoice(splice));
    }
    project->setMergeConflicts(conflicts);
}

} // namespace ProjectMerge

NATRON_NAMESPACE_EXIT
