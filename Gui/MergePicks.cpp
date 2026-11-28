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

#include "MergePicks.h"

#include <functional>
#include <memory>
#include <set>

#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QUndoCommand>

#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphDiffOverlay.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGui.h"
#include "Gui/UndoHistory.h"

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

QString
tr(const char* text)
{
    return QCoreApplication::translate("MergePicks", text);
}

/**
 * @brief One pick: its graph edit as a child, the merge conflicts before and after it (a restore
 * can add splice choices), and the comparison made again each way.
 **/
class MergePickCommand
    : public QUndoCommand {
public:
    MergePickCommand(Gui* gui,
                     const ProjectPtr& project,
                     const std::shared_ptr<const Serialization::ProjectSerialization>& other,
                     const QString& text)
        : QUndoCommand(text)
        , _gui(gui)
        , _project(project)
        , _other(other)
        , _conflictsBefore(project->getMergeConflicts())
        , _conflictsAfter()
    {
    }

    void captureAfter()
    {
        if (ProjectPtr project = _project.lock()) {
            _conflictsAfter = project->getMergeConflicts();
        }
    }

    bool hasEffect() const
    {
        return (childCount() > 0) || (_conflictsBefore != _conflictsAfter);
    }

    virtual void undo() OVERRIDE FINAL
    {
        QUndoCommand::undo();
        restore(_conflictsBefore);
    }

    virtual void redo() OVERRIDE FINAL
    {
        // The children skip their first redo: the edit is made before the command is pushed.
        QUndoCommand::redo();
        restore(_conflictsAfter);
    }

private:
    void restore(const std::vector<Serialization::MergeConflict>& conflicts)
    {
        ProjectPtr project = _project.lock();
        if (!project) {
            return;
        }
        if (project->getMergeConflicts() != conflicts) {
            project->setMergeConflicts(conflicts);
        }
        _gui->recompareGraphComparison(_other);
    }

    Gui* _gui;
    ProjectWPtr _project;
    std::shared_ptr<const Serialization::ProjectSerialization> _other;
    std::vector<Serialization::MergeConflict> _conflictsBefore;
    std::vector<Serialization::MergeConflict> _conflictsAfter;
};

bool
pick(NodeGraph* graph,
     const QString& text,
     const std::vector<Serialization::GraphOp>& ops,
     const std::vector<ProjectMerge::PendingSplice>& pending,
     QString* error)
{
    Gui* gui = graph->getGui();
    const ProjectMerge::ChangeSummary* summary = gui->getGraphComparison();
    const ProjectPtr project = gui->getApp()->getProject();
    MergePickCommand* command = new MergePickCommand(gui, project, summary->other, text);
    if (!ops.empty()) {
        const std::vector<Serialization::OpResult> results = ApplyGraphOpsCommand::applyAndRecord(graph, project, ops, text, command);
        for (const Serialization::OpResult& result : results) {
            if (!result.applied()) {
                *error = QString::fromStdString(result.message);
                delete command;

                return false;
            }
        }
        if (results.size() != ops.size()) {
            *error = tr("%1 was not applied.").arg(text);
            delete command;

            return false;
        }
    }
    if (!pending.empty()) {
        ProjectMerge::addSpliceChoices(project, pending);
    }
    command->captureAfter();
    if (!command->hasEffect()) {
        delete command;

        return false;
    }
    graph->pushUndoCommand(command, UndoScope());

    return true;
}

const ProjectMerge::ChangeSummary*
comparisonOf(NodeGraph* graph,
             QString* error)
{
    error->clear();
    const ProjectMerge::ChangeSummary* summary = (graph && graph->getGui()) ? graph->getGui()->getGraphComparison() : 0;
    if (!summary || !summary->other) {
        *error = tr("There is no comparison to pick from.");

        return 0;
    }

    return summary;
}

std::vector<std::string>
idsWhere(const ProjectMerge::ChangeSummary& summary,
         const std::vector<std::string>& nodeIds,
         const std::function<bool(const ProjectMerge::NodeChange&)>& applies)
{
    std::vector<std::string> kept;
    for (const std::string& id : nodeIds) {
        const ProjectMerge::NodeChange* change = summary.find(id);
        if (change && applies(*change)) {
            kept.push_back(id);
        }
    }

    return kept;
}

bool
isRemoved(const ProjectMerge::NodeChange& change)
{
    return change.kind == ProjectMerge::NodeChange::eRemoved;
}

bool
isChanged(const ProjectMerge::NodeChange& change)
{
    return (change.kind == ProjectMerge::NodeChange::eChanged) && !NodeGraphDiffOverlay::isOnlyMoved(change);
}

bool
isAdded(const ProjectMerge::NodeChange& change)
{
    return change.kind == ProjectMerge::NodeChange::eAdded;
}

bool
isMoved(const ProjectMerge::NodeChange& change)
{
    return (change.kind == ProjectMerge::NodeChange::eChanged) && change.moved;
}

void
report(NodeGraph* graph,
       const QString& error)
{
    if (!error.isEmpty()) {
        graph->getGui()->getApp()->appendToScriptEditor(error.toStdString());
    }
}

QAction*
addAction(QMenu* menu,
          const QString& text,
          const char* objectName)
{
    QAction* action = menu->addAction(text);
    action->setObjectName(QString::fromUtf8(objectName));

    return action;
}

} // anon namespace

namespace MergePicks {

std::vector<std::string>
selectedNodeIds(NodeGraph* graph)
{
    std::vector<std::string> ids;
    if (NodeGraphDiffOverlay* overlay = graph->getDiffOverlay()) {
        ids.insert(ids.end(), overlay->selectedGhostIds().begin(), overlay->selectedGhostIds().end());
    }
    for (const NodeGuiPtr& node : graph->getSelectedNodes()) {
        const NodePtr internal = node ? node->getNode() : NodePtr();
        if (internal) {
            ids.push_back(internal->getNodeId());
        }
    }

    return ids;
}

bool
restore(NodeGraph* graph,
        const std::vector<std::string>& nodeIds,
        ProjectMerge::SpliceMode mode,
        QString* error)
{
    const ProjectMerge::ChangeSummary* summary = comparisonOf(graph, error);
    if (!summary) {
        return false;
    }
    const std::vector<std::string> ids = idsWhere(*summary, nodeIds, isRemoved);
    if (ids.empty()) {
        return false;
    }
    std::vector<Serialization::GraphOp> ops;
    std::vector<ProjectMerge::PendingSplice> pending;
    if (!ProjectMerge::restoreOperations(graph->getGui()->getApp()->getProject(), *summary, ids, mode, &ops, &pending, error)) {
        return false;
    }
    const QString text = (mode == ProjectMerge::SpliceMode::eKeepMine) ? tr("Restore (Keep My Wiring)")
        : (mode == ProjectMerge::SpliceMode::eEverywhere)              ? tr("Restore (Splice Everywhere)")
                                                                       : tr("Restore");
    const bool picked = pick(graph, text, ops, pending, error);
    if (picked) {
        graph->getDiffOverlay()->clearGhostSelection();
    }

    return picked;
}

bool
revert(NodeGraph* graph,
       const std::vector<std::string>& nodeIds,
       QString* error)
{
    const ProjectMerge::ChangeSummary* summary = comparisonOf(graph, error);
    if (!summary) {
        return false;
    }
    const std::vector<std::string> ids = idsWhere(*summary, nodeIds, isChanged);
    if (ids.empty()) {
        return false;
    }

    return pick(graph, tr("Revert to Other"), ProjectMerge::revertOperations(graph->getGui()->getApp()->getProject(), *summary, ids),
                std::vector<ProjectMerge::PendingSplice>(), error);
}

bool
takeKnob(NodeGraph* graph,
         const std::string& nodeId,
         const std::string& knobName,
         QString* error)
{
    const ProjectMerge::ChangeSummary* summary = comparisonOf(graph, error);
    if (!summary) {
        return false;
    }
    std::vector<Serialization::GraphOp> ops;
    if (!ProjectMerge::takeKnobOperations(graph->getGui()->getApp()->getProject(), *summary, nodeId, knobName, &ops)) {
        *error = tr("%1 is not a changed parameter of that node.").arg(QString::fromStdString(knobName));

        return false;
    }

    return pick(graph, tr("Take %1").arg(QString::fromStdString(knobName)), ops, std::vector<ProjectMerge::PendingSplice>(), error);
}

bool
removeAdded(NodeGraph* graph,
            const std::vector<std::string>& nodeIds,
            QString* error)
{
    const ProjectMerge::ChangeSummary* summary = comparisonOf(graph, error);
    if (!summary) {
        return false;
    }
    const std::vector<std::string> ids = idsWhere(*summary, nodeIds, isAdded);
    if (ids.empty()) {
        return false;
    }

    return pick(graph, tr("Remove"), ProjectMerge::removeAddedOperations(graph->getGui()->getApp()->getProject(), *summary, ids),
                std::vector<ProjectMerge::PendingSplice>(), error);
}

bool
restorePosition(NodeGraph* graph,
                const std::vector<std::string>& nodeIds,
                QString* error)
{
    const ProjectMerge::ChangeSummary* summary = comparisonOf(graph, error);
    if (!summary) {
        return false;
    }
    const std::vector<std::string> ids = idsWhere(*summary, nodeIds, isMoved);
    if (ids.empty()) {
        return false;
    }

    return pick(graph, tr("Restore Position"), ProjectMerge::restorePositionOperations(graph->getGui()->getApp()->getProject(), *summary, ids),
                std::vector<ProjectMerge::PendingSplice>(), error);
}

void
addMenuActions(QMenu* menu,
               NodeGraph* graph)
{
    QString error;
    const ProjectMerge::ChangeSummary* summary = comparisonOf(graph, &error);
    if (!summary) {
        return;
    }
    const std::vector<std::string> ids = selectedNodeIds(graph);
    const bool removed = !idsWhere(*summary, ids, isRemoved).empty();
    const bool changed = !idsWhere(*summary, ids, isChanged).empty();
    const bool added = !idsWhere(*summary, ids, isAdded).empty();
    const bool moved = !idsWhere(*summary, ids, isMoved).empty();
    if (!removed && !changed && !added && !moved) {
        return;
    }
    menu->addSeparator();
    // The picks run after the menu closes, on the selection as it was when it opened.
    const auto bind = [graph, ids](QAction* action, const std::function<bool(NodeGraph*, const std::vector<std::string>&, QString*)>& run) {
        QObject::connect(action, &QAction::triggered, graph, [graph, ids, run]() {
            QString message;
            run(graph, ids, &message);
            report(graph, message);
        });
    };
    if (removed) {
        bind(addAction(menu, tr("Restore"), "actionGraphRestoreGhosts"),
             [](NodeGraph* g, const std::vector<std::string>& n, QString* e) { return restore(g, n, ProjectMerge::SpliceMode::eAsk, e); });
        bind(addAction(menu, tr("Restore (Keep My Wiring)"), "actionGraphRestoreGhostsKeepMyWiring"),
             [](NodeGraph* g, const std::vector<std::string>& n, QString* e) { return restore(g, n, ProjectMerge::SpliceMode::eKeepMine, e); });
        bind(addAction(menu, tr("Restore (Splice Everywhere)"), "actionGraphRestoreGhostsSpliceEverywhere"),
             [](NodeGraph* g, const std::vector<std::string>& n, QString* e) { return restore(g, n, ProjectMerge::SpliceMode::eEverywhere, e); });
    }
    if (changed) {
        bind(addAction(menu, tr("Revert to Other"), "actionGraphRevertToOther"), revert);
    }
    if (added) {
        bind(addAction(menu, tr("Remove"), "actionGraphRemoveAdded"), removeAdded);
    }
    if (moved) {
        bind(addAction(menu, tr("Restore Position"), "actionGraphRestorePosition"), restorePosition);
    }
} // addMenuActions

} // namespace MergePicks

NATRON_NAMESPACE_EXIT
