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

UpdateResult
updateFromTemplate(const ProjectPtr& project,
                   const std::string& templateKey,
                   const QString& newPath,
                   const OpApplier& applier)
{
    assert(QThread::currentThread() == qApp->thread());
    UpdateResult result;

    const Serialization::TemplateImportSerialization* found = nullptr;
    const std::list<Serialization::TemplateImportSerialization>& imports = project->getTemplateImports();
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
            if (NodeChange* change = changeFor(ui->nodeId)) {
                change->appearance = true;
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
    for (const std::string& id : beforeOrder) {
        if (afterNodes.find(id) == afterNodes.end()) {
            NodeChange removed = describe(beforeNodes.at(id), NodeChange::eRemoved);
            describeLinks(beforeNodes, beforeNodes.at(id), &removed);
            summary.nodes.push_back(std::move(removed));
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
    Serialization::ProjectSerialization other;
    if (!readSnapshot(path, &other, error)) {
        return false;
    }
    *summary = compare(project, other);

    return true;
}

bool
compareWithTemplateBase(const ProjectPtr& project,
                        const std::string& templateKey,
                        ChangeSummary* summary,
                        QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    const Serialization::TemplateImportSerialization* found = nullptr;
    for (const Serialization::TemplateImportSerialization& entry : project->getTemplateImports()) {
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
    const Serialization::ProjectSerialization& base = *found->base;
    const Serialization::MergePlan plan = Serialization::planMerge(base, mine, base, templateKey);
    Serialization::ProjectSerialization other = Serialization::editableState(base);
    remapNodeIds(other.nodes.nodes, plan.baseToMine);

    ChangeSummary all = compare(project, other);
    summary->nodes.clear();
    summary->projectKnobs.clear();
    summary->projectData = false;
    for (NodeChange& change : all.nodes) {
        if ((change.kind == NodeChange::eRemoved) || (plan.scope.find(change.nodeId) != plan.scope.end())) {
            summary->nodes.push_back(std::move(change));
        }
    }

    return true;
}

bool
canChooseSide(const ProjectPtr& project,
              const Serialization::MergeConflict& conflict,
              ConflictSide side)
{
    return sideAvailable(project, conflict, valueOf(conflict, side), side == ConflictSide::eBase);
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
    const Serialization::MergeConflictOption* found = Serialization::conflictOption(conflict, option);

    return found && sideAvailable(project, conflict, found->state, false);
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

} // namespace ProjectMerge

NATRON_NAMESPACE_EXIT
