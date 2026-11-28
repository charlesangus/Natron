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

#include "MergeConflictResolve.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

#include <QCoreApplication>

#include "Engine/GraphOpApply.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/ProjectTemplate.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/UndoHistory.h"

#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {
// An update or a publish replaces an import's base snapshot rather than editing it, so comparing
// the pointers is enough.
bool
sameImports(const std::list<Serialization::TemplateImportSerialization>& a,
            const std::list<Serialization::TemplateImportSerialization>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const Serialization::TemplateImportSerialization& x, const Serialization::TemplateImportSerialization& y) {
                          return (x.key == y.key) && (x.templateId == y.templateId) && (x.name == y.name) && (x.version == y.version) && (x.publishDir == y.publishDir) && (x.path == y.path) && (x.contentHash == y.contentHash) && (x.importedAt == y.importedAt) && (x.excluded == y.excluded) && (x.base == y.base);
                      });
}
} // anon namespace

MergeStateCommand::MergeStateCommand(const ProjectPtr& project,
                                     const QString& text)
    : QUndoCommand(text)
    , _project(project)
    , _importsBefore(project->getTemplateImports())
    , _importsAfter(_importsBefore)
    , _conflictsBefore(project->getMergeConflicts())
    , _conflictsAfter(_conflictsBefore)
{
}

void
MergeStateCommand::captureAfter()
{
    ProjectPtr project = _project.lock();
    if (!project) {
        return;
    }
    _importsAfter = project->getTemplateImports();
    _conflictsAfter = project->getMergeConflicts();
}

bool
MergeStateCommand::hasEffect() const
{
    return (childCount() > 0) || !sameImports(_importsAfter, _importsBefore) || (_conflictsAfter != _conflictsBefore);
}

void
MergeStateCommand::restore(const std::list<Serialization::TemplateImportSerialization>& imports,
                           const std::vector<Serialization::MergeConflict>& conflicts)
{
    ProjectPtr project = _project.lock();
    if (!project) {
        return;
    }
    project->setTemplateImports(imports);
    project->setMergeConflicts(conflicts);
}

// The graph is restored before the conflicts in both directions so that whatever reacts to the
// conflict list finds the nodes it names.
void
MergeStateCommand::undo()
{
    QUndoCommand::undo();
    restore(_importsBefore, _conflictsBefore);
}

void
MergeStateCommand::redo()
{
    QUndoCommand::redo();
    restore(_importsAfter, _conflictsAfter);
}

namespace MergeConflictResolve {
namespace {
    NodeGraph*
    graphOf(Gui* gui)
    {
        NodeGraph* graph = gui->getLastSelectedGraph();

        return graph ? graph : gui->getNodeGraph();
    }

    void
    push(Gui* gui,
         MergeStateCommand* command)
    {
        command->captureAfter();
        if (!command->hasEffect()) {
            delete command;

            return;
        }
        graphOf(gui)->pushUndoCommand(command, UndoScope());
    }

    void
    removeConflicts(const ProjectPtr& project,
                    const std::set<std::size_t>& indices)
    {
        const std::vector<Serialization::MergeConflict>& conflicts = project->getMergeConflicts();
        std::vector<Serialization::MergeConflict> kept;
        kept.reserve(conflicts.size());
        for (std::size_t i = 0; i < conflicts.size(); ++i) {
            if (indices.find(i) == indices.end()) {
                kept.push_back(conflicts[i]);
            }
        }
        project->setMergeConflicts(kept);
    }
} // anon namespace

std::string
importTemplate(Gui* gui,
               const QString& path,
               QString* error)
{
    const ProjectPtr project = gui->getApp()->getProject();
    NodeGraph* graph = graphOf(gui);
    NodeCollectionPtr group = graph->getGroup();
    if (!group) {
        group = project;
    }

    MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Import Template"));
    Serialization::ProjectSerialization before;
    project->toSerialization(&before);
    const std::size_t importsBefore = project->getTemplateImports().size();
    error->clear();
    ignore_result(project->importTemplate(path, group, error));

    Serialization::ProjectSerialization after;
    project->toSerialization(&after);
    ApplyGraphOpsCommand::recordChange(graph, project, before, after, command->text(), command);
    push(gui, command);

    const std::list<Serialization::TemplateImportSerialization> imports = project->getTemplateImports();
    if (imports.size() <= importsBefore) {
        return std::string();
    }

    return imports.back().key;
}

bool
runTemplateEdit(Gui* gui,
                NodeGraph* graph,
                const QString& text,
                const std::function<bool(TemplateEdit*)>& edit)
{
    const ProjectPtr project = gui->getApp()->getProject();
    NodeGraph* target = graph ? graph : graphOf(gui);
    MergeStateCommand* command = new MergeStateCommand(project, text);
    TemplateEdit templateEdit;
    templateEdit.applier = [target, project, text, command](const std::vector<Serialization::GraphOp>& ops) {
        return ApplyGraphOpsCommand::applyAndRecord(target, project, ops, text, command);
    };
    const std::vector<Serialization::MergeConflict> conflictsBefore = project->getMergeConflicts();
    const bool ok = edit(&templateEdit);
    const bool raisedConflicts = project->getMergeConflicts().size() > conflictsBefore.size();
    command->captureAfter();
    if (command->hasEffect()) {
        target->pushUndoCommand(command, UndoScope());
    } else {
        delete command;
    }
    if (raisedConflicts) {
        gui->ensureMergeConflictPanelVisible();
    }

    return ok;
}

bool
deleteNodesAndTemplates(Gui* gui,
                        NodeGraph* graph,
                        const std::vector<std::string>& templateKeys,
                        const NodesGuiList& nodes,
                        QString* error)
{
    const ProjectPtr project = gui->getApp()->getProject();
    MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Delete"));
    const QString text = command->text();
    bool ok = true;
    error->clear();
    for (const std::string& key : templateKeys) {
        TemplateEdit edit;
        edit.applier = [graph, project, text, command](const std::vector<Serialization::GraphOp>& ops) {
            return ApplyGraphOpsCommand::applyAndRecord(graph, project, ops, text, command);
        };
        if (!project->detachTemplate(key, &edit) && ok) {
            ok = false;
            *error = edit.error;
        }
    }
    command->captureAfter();
    // A child, so the push removes the nodes after the detaches the command already holds.
    if (!nodes.empty()) {
        new RemoveMultipleNodesCommand(graph, nodes, command);
    }
    if (command->hasEffect()) {
        graph->pushUndoCommand(command, UndoScope());
    } else {
        delete command;
    }

    return ok;
}

ProjectMerge::UpdateResult
updateFromTemplate(Gui* gui,
                   const std::string& templateKey,
                   const QString& path,
                   bool allowMismatch)
{
    const ProjectPtr project = gui->getApp()->getProject();
    QString file = path;
    if (file.isEmpty()) {
        for (const Serialization::TemplateImportSerialization& entry : project->getTemplateImports()) {
            if (entry.key == templateKey) {
                file = QString::fromStdString(entry.path);
            }
        }
    }
    NodeGraph* graph = graphOf(gui);
    MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Update from Template"));
    const QString text = command->text();
    const ProjectMerge::OpApplier applier = [graph, project, text, command](const std::vector<Serialization::GraphOp>& ops) {
        return ApplyGraphOpsCommand::applyAndRecord(graph, project, ops, text, command);
    };
    ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project, templateKey, file, applier, allowMismatch);
    push(gui, command);
    if (!result.conflicts.empty()) {
        gui->ensureMergeConflictPanelVisible();
    }

    return result;
}

namespace {
    bool
    allApplied(const std::vector<Serialization::GraphOp>& ops,
               const std::vector<Serialization::OpResult>& results,
               QString* message)
    {
        for (const Serialization::OpResult& result : results) {
            if (!result.applied()) {
                *message = QString::fromStdString(result.message);

                return false;
            }
        }
        if (results.size() != ops.size()) {
            *message = QCoreApplication::translate("MergeConflictResolve", "The resolution was not applied.");

            return false;
        }

        return true;
    }

    // Puts the graph back to state, then applies replay.
    bool
    restoreAndReplay(const ProjectPtr& project,
                     const Serialization::ProjectSerialization& state,
                     const std::vector<Serialization::GraphOp>& replay)
    {
        Serialization::ProjectSerialization live;
        project->toSerialization(&live);
        const std::vector<Serialization::GraphOp> back = Serialization::diffSnapshots(Serialization::editableState(live), state);
        QString message;
        if (!back.empty() && !allApplied(back, applyGraphOps(project, back), &message)) {
            return false;
        }

        return replay.empty() || allApplied(replay, applyGraphOps(project, replay), &message);
    }

    // Each conflict's operations are built from the graph as the ones before it left it, so that
    // conflicts on the same node compose. The whole batch is one change of the graph, recorded from
    // one snapshot taken before it and one after it.
    bool
    resolveWith(Gui* gui,
                const std::vector<std::size_t>& indices,
                const std::function<bool(const Serialization::MergeConflict&, std::vector<Serialization::GraphOp>*, QString*)>& operations,
                QString* error)
    {
        const ProjectPtr project = gui->getApp()->getProject();
        const std::vector<Serialization::MergeConflict> conflicts = project->getMergeConflicts();
        NodeGraph* graph = graphOf(gui);
        MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Resolve Merge Conflict"));
        std::set<std::size_t> seen;
        std::set<std::size_t> applied;
        bool ok = true;
        const auto failed = [&ok, error](const QString& message) {
            if (ok && error) {
                *error = message;
            }
            ok = false;
        };
        std::set<std::string> present;
        for (std::size_t index : indices) {
            if ((index < conflicts.size()) && !conflicts[index].nodeId.empty() && project->findNodeById(conflicts[index].nodeId)) {
                present.insert(conflicts[index].nodeId);
            }
        }

        Serialization::ProjectSerialization before;
        project->toSerialization(&before);
        std::optional<Serialization::ProjectSerialization> beforeState;
        // The operations of the resolutions applied so far, in order.
        std::vector<Serialization::GraphOp> kept;
        for (std::size_t index : indices) {
            if ((index >= conflicts.size()) || !seen.insert(index).second) {
                continue;
            }
            // A conflict about a node an earlier resolution of the batch deleted has nothing left to resolve.
            if (present.count(conflicts[index].nodeId) && !project->findNodeById(conflicts[index].nodeId)) {
                applied.insert(index);
                continue;
            }
            std::vector<Serialization::GraphOp> ops;
            QString opError;
            if (!operations(conflicts[index], &ops, &opError)) {
                failed(opError);
                continue;
            }
            if (!ops.empty()) {
                QString message;
                if (!allApplied(ops, applyGraphOps(project, ops), &message)) {
                    failed(message);
                    // Only this resolution is dropped: the graph goes back to what the ones before it made.
                    if (!beforeState) {
                        beforeState = Serialization::editableState(before);
                    }
                    if (!restoreAndReplay(project, *beforeState, kept)) {
                        ignore_result(restoreAndReplay(project, *beforeState, std::vector<Serialization::GraphOp>()));
                        kept.clear();
                        applied.clear();
                        break;
                    }
                    continue;
                }
                kept.insert(kept.end(), ops.begin(), ops.end());
            }
            applied.insert(index);
        }
        if (!kept.empty()) {
            Serialization::ProjectSerialization after;
            project->toSerialization(&after);
            ApplyGraphOpsCommand::recordChange(graph, project, before, after, command->text(), command);
        }
        removeConflicts(project, applied);
        push(gui, command);

        return ok;
    }
} // anon namespace

bool
resolve(Gui* gui,
        const std::vector<std::size_t>& indices,
        ProjectMerge::ConflictSide side,
        QString* error)
{
    const ProjectPtr project = gui->getApp()->getProject();

    return resolveWith(gui, indices, [&project, side](const Serialization::MergeConflict& conflict, std::vector<Serialization::GraphOp>* ops, QString* opError) { return ProjectMerge::sideOperations(project, conflict, side, ops, opError); }, error);
}

bool
resolveOption(Gui* gui,
              std::size_t index,
              const std::string& option,
              QString* error)
{
    const ProjectPtr project = gui->getApp()->getProject();

    return resolveWith(gui, std::vector<std::size_t>(1, index), [&project, &option](const Serialization::MergeConflict& conflict, std::vector<Serialization::GraphOp>* ops, QString* opError) { return ProjectMerge::optionOperations(project, conflict, option, ops, opError); }, error);
}

void
accept(Gui* gui,
       const std::vector<std::size_t>& indices)
{
    const ProjectPtr project = gui->getApp()->getProject();
    MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Accept Merge Conflict"));
    removeConflicts(project, std::set<std::size_t>(indices.begin(), indices.end()));
    push(gui, command);
}
} // namespace MergeConflictResolve

NATRON_NAMESPACE_EXIT
