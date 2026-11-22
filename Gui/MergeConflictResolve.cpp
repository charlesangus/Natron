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
#include <set>

#include <QCoreApplication>

#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_ENTER

namespace {
// An update replaces an import's base snapshot rather than editing it, so comparing the pointers
// is enough.
bool
sameImports(const std::list<Serialization::TemplateImportSerialization>& a,
            const std::list<Serialization::TemplateImportSerialization>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const Serialization::TemplateImportSerialization& x, const Serialization::TemplateImportSerialization& y) {
                          return (x.key == y.key) && (x.path == y.path) && (x.contentHash == y.contentHash) && (x.importedAt == y.importedAt) && (x.base == y.base);
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
    const NodesList created = project->importTemplate(path, group, error);

    Serialization::ProjectSerialization after;
    project->toSerialization(&after);
    ApplyGraphOpsCommand::recordChange(graph, project, before, after, command->text(), command);
    push(gui, command);

    const std::list<Serialization::TemplateImportSerialization>& imports = project->getTemplateImports();
    if (created.empty() || (imports.size() <= importsBefore)) {
        return std::string();
    }

    return imports.back().key;
}

ProjectMerge::UpdateResult
updateFromTemplate(Gui* gui,
                   const std::string& templateKey,
                   const QString& path)
{
    const ProjectPtr project = gui->getApp()->getProject();
    NodeGraph* graph = graphOf(gui);
    MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Update from Template"));
    const QString text = command->text();
    const ProjectMerge::OpApplier applier = [graph, project, text, command](const std::vector<Serialization::GraphOp>& ops) {
        return ApplyGraphOpsCommand::applyAndRecord(graph, project, ops, text, command);
    };
    ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project, templateKey, path, applier);
    push(gui, command);
    if (!result.conflicts.empty()) {
        gui->ensureMergeConflictPanelVisible();
    }

    return result;
}

bool
resolve(Gui* gui,
        const std::vector<std::size_t>& indices,
        ProjectMerge::ConflictSide side,
        QString* error)
{
    const ProjectPtr project = gui->getApp()->getProject();
    const std::vector<Serialization::MergeConflict> conflicts = project->getMergeConflicts();
    std::vector<Serialization::GraphOp> ops;
    std::set<std::size_t> resolved;
    bool ok = true;
    for (std::size_t index : indices) {
        if ((index >= conflicts.size()) || (resolved.find(index) != resolved.end())) {
            continue;
        }
        Serialization::GraphOp op;
        QString opError;
        if (!ProjectMerge::sideOperation(project, conflicts[index], side, &op, &opError)) {
            if (ok && error) {
                *error = opError;
            }
            ok = false;
            continue;
        }
        ops.push_back(op);
        resolved.insert(index);
    }
    if (resolved.empty()) {
        return ok;
    }

    NodeGraph* graph = graphOf(gui);
    MergeStateCommand* command = new MergeStateCommand(project, QCoreApplication::translate("MergeConflictResolve", "Resolve Merge Conflict"));
    const std::vector<Serialization::OpResult> results = ApplyGraphOpsCommand::applyAndRecord(graph, project, ops, command->text(), command);
    std::set<std::size_t> applied;
    std::size_t opIndex = 0;
    for (std::size_t index : resolved) {
        if ((opIndex < results.size()) && results[opIndex].applied()) {
            applied.insert(index);
        } else {
            if (ok && error) {
                *error = (opIndex < results.size()) ? QString::fromStdString(results[opIndex].message)
                                                    : QCoreApplication::translate("MergeConflictResolve", "The resolution was not applied.");
            }
            ok = false;
        }
        ++opIndex;
    }
    removeConflicts(project, applied);
    push(gui, command);

    return ok;
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
