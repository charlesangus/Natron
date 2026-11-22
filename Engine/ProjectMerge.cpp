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
#include "Serialization/Merge/MergeHandler.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/Sha256.h"

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
        if (conflict.knob.empty()) {
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

    Serialization::KnobDimensionSerialization
    defaultDimensionLike(const Serialization::KnobDimensionSerialization& live)
    {
        Serialization::KnobDimensionSerialization dimension;
        dimension.defaultValue = live.defaultValue;
        dimension.value = live.defaultValue;

        return dimension;
    }

    // Times are compared as the curve merge stores them: copied, never computed.
    void
    setKeyAt(Serialization::CurveSerialization* curve,
             double time,
             const std::optional<Serialization::KeyFrameSerialization>& key)
    {
        std::vector<Serialization::KeyFrameSerialization>& keys = curve->keyframes;
        keys.erase(std::remove_if(keys.begin(), keys.end(), [time](const Serialization::KeyFrameSerialization& k) {
                       return k.time == time;
                   }),
                   keys.end());
        if (key) {
            Serialization::KeyFrameSerialization placed = *key;
            placed.time = time;
            keys.insert(std::upper_bound(keys.begin(), keys.end(), placed, [](const Serialization::KeyFrameSerialization& a, const Serialization::KeyFrameSerialization& b) {
                            return a.time < b.time;
                        }),
                        placed);
        }
    }

} // namespace

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
        result.results = applier ? applier(result.ops) : applyGraphOps(project, result.ops);
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
    const Serialization::ProjectSerialization before = Serialization::editableState(other);
    const Serialization::ProjectSerialization after = Serialization::editableState(live);

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

    for (const std::string& id : afterOrder) {
        if (beforeNodes.find(id) == beforeNodes.end()) {
            summary.nodes.push_back(describe(afterNodes.at(id), NodeChange::eAdded));
        } else if (auto it = changed.find(id); it != changed.end()) {
            summary.nodes.push_back(it->second);
        }
    }
    for (const std::string& id : beforeOrder) {
        if (afterNodes.find(id) == afterNodes.end()) {
            summary.nodes.push_back(describe(beforeNodes.at(id), NodeChange::eRemoved));
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
              const Serialization::MergeConflict& conflict)
{
    if ((conflict.kind != Serialization::MergeConflictKind::eValue) && (conflict.kind != Serialization::MergeConflictKind::eCurveKey)) {
        return false;
    }
    if ((conflict.kind == Serialization::MergeConflictKind::eCurveKey) && !conflict.time) {
        return false;
    }

    return bool(conflictKnob(project, conflict));
}

bool
sideOperation(const ProjectPtr& project,
              const Serialization::MergeConflict& conflict,
              ConflictSide side,
              Serialization::GraphOp* op,
              QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }

        return false;
    };
    if (!canChooseSide(project, conflict)) {
        return fail(QCoreApplication::translate("ProjectMerge", "This conflict has no side to choose: the merge's choice can only be accepted"));
    }
    KnobIPtr knob = conflictKnob(project, conflict);
    Serialization::KnobSerialization state;
    knob->toSerialization(&state);
    const Serialization::MergeConflictValue& value = valueOf(conflict, side);
    const int dimension = conflict.dimension;

    if (conflict.kind == Serialization::MergeConflictKind::eValue) {
        if (dimension < 0) {
            state.parametricCurves = value.knob ? value.knob->parametricCurves : std::nullopt;
            state.stringAnimation = value.knob ? value.knob->stringAnimation : std::nullopt;
            state.choiceOptionID = value.knob ? value.knob->choiceOptionID : std::nullopt;
        } else {
            if (dimension >= int(state.dimensions.size())) {
                return fail(QCoreApplication::translate("ProjectMerge", "%1 has no dimension %2").arg(QString::fromStdString(conflict.knob)).arg(dimension));
            }
            if (value.knob && (dimension < int(value.knob->dimensions.size()))) {
                state.dimensions[dimension] = value.knob->dimensions[dimension];
            } else {
                state.dimensions[dimension] = defaultDimensionLike(state.dimensions[dimension]);
            }
        }
    } else {
        const double time = *conflict.time;
        if (state.parametricCurves) {
            if ((dimension < 0) || (dimension >= int(state.parametricCurves->size()))) {
                return fail(QCoreApplication::translate("ProjectMerge", "%1 has no curve %2").arg(QString::fromStdString(conflict.knob)).arg(dimension));
            }
            std::list<Serialization::CurveSerialization>::iterator curve = state.parametricCurves->begin();
            std::advance(curve, dimension);
            setKeyAt(&*curve, time, value.key);
        } else {
            if ((dimension < 0) || (dimension >= int(state.dimensions.size()))) {
                return fail(QCoreApplication::translate("ProjectMerge", "%1 has no dimension %2").arg(QString::fromStdString(conflict.knob)).arg(dimension));
            }
            Serialization::KnobDimensionSerialization& target = state.dimensions[dimension];
            if (!target.curve) {
                target.curve = Serialization::CurveSerialization();
            }
            setKeyAt(&*target.curve, time, value.key);
            if (target.curve->keyframes.empty()) {
                target.curve.reset();
            }
        }
    }

    Serialization::SetKnobOp set;
    if (!conflict.nodeId.empty()) {
        set.nodeId = conflict.nodeId;
    }
    set.knobName = conflict.knob;
    if (!Serialization::isDefaultKnob(state)) {
        set.value = std::move(state);
    }
    *op = std::move(set);

    return true;
}

} // namespace ProjectMerge

NATRON_NAMESPACE_EXIT
