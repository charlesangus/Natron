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

#include "NodeGraphMergeBar.h"

#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/MergePicks.h"
#include "Gui/MergePopover.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphDiffOverlay.h"

NATRON_NAMESPACE_ENTER

namespace {

QPushButton*
addButton(QHBoxLayout* layout,
          QWidget* parent,
          const QString& text,
          const char* objectName,
          const QString& toolTip)
{
    QPushButton* button = new QPushButton(text, parent);
    button->setObjectName(QString::fromUtf8(objectName));
    button->setToolTip(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button);

    return button;
}

QLabel*
addLabel(QHBoxLayout* layout,
         QWidget* parent,
         const char* objectName)
{
    QLabel* label = new QLabel(parent);
    label->setObjectName(QString::fromUtf8(objectName));
    // Labels give way first, so the buttons stay whole in a narrow graph.
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setMinimumWidth(40);
    layout->addWidget(label, 1);

    return label;
}

std::vector<std::size_t>
allConflicts(Gui* gui)
{
    std::vector<std::size_t> indices;
    const std::size_t count = gui->getApp()->getProject()->getMergeConflicts().size();
    for (std::size_t i = 0; i < count; ++i) {
        indices.push_back(i);
    }

    return indices;
}

} // anon namespace

NodeGraphMergeBar::NodeGraphMergeBar(NodeGraph* graph)
    : QFrame(graph)
    , _graph(graph)
    , _mode(eModeNone)
    , _comparison(0)
    , _compared(0)
    , _counts(0)
    , _restoreSelected(0)
    , _revertSelected(0)
    , _conflicts(0)
    , _conflictCount(0)
    , _error(0)
{
    setObjectName(QString::fromUtf8("NodeGraphMergeBar"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(4);

    _comparison = new QWidget(this);
    QHBoxLayout* comparison = new QHBoxLayout(_comparison);
    comparison->setContentsMargins(0, 0, 0, 0);
    comparison->setSpacing(4);
    _compared = addLabel(comparison, _comparison, "mergeBarCompared");
    _counts = addLabel(comparison, _comparison, "mergeBarCounts");
    QPushButton* previousChange = addButton(comparison, _comparison, tr("Previous Change"), "mergeBarPreviousChange", tr("Frame the previous changed node."));
    QPushButton* nextChange = addButton(comparison, _comparison, tr("Next Change"), "mergeBarNextChange", tr("Frame the next changed node."));
    _restoreSelected = addButton(comparison, _comparison, tr("Restore Selected"), "mergeBarRestoreSelected", tr("Bring the selected removed nodes back."));
    _revertSelected = addButton(comparison, _comparison, tr("Revert Selected"), "mergeBarRevertSelected", tr("Give the selected changed nodes the state the compared version has."));
    QPushButton* clear = addButton(comparison, _comparison, tr("Clear Comparison"), "mergeBarClearComparison", tr("Stop showing the comparison."));
    layout->addWidget(_comparison, 1);

    _conflicts = new QWidget(this);
    QHBoxLayout* conflicts = new QHBoxLayout(_conflicts);
    conflicts->setContentsMargins(0, 0, 0, 0);
    conflicts->setSpacing(4);
    _conflictCount = addLabel(conflicts, _conflicts, "mergeBarConflictCount");
    QPushButton* previousConflict = addButton(conflicts, _conflicts, tr("Previous Conflict"), "mergeBarPreviousConflict", tr("Frame the previous node with a merge conflict."));
    QPushButton* nextConflict = addButton(conflicts, _conflicts, tr("Next Conflict"), "mergeBarNextConflict", tr("Frame the next node with a merge conflict."));
    QPushButton* allMine = addButton(conflicts, _conflicts, tr("All Mine"), "mergeBarAllMine", tr("Give every merge conflict the state this project had."));
    QPushButton* allTheirs = addButton(conflicts, _conflicts, tr("All Theirs"), "mergeBarAllTheirs", tr("Give every merge conflict the state the template had."));
    QPushButton* openPanel = addButton(conflicts, _conflicts, tr("Open Panel"), "mergeBarOpenPanel", tr("Show the Merge Conflicts panel."));
    layout->addWidget(_conflicts, 1);

    _error = new QLabel(this);
    _error->setObjectName(QString::fromUtf8("mergeBarError"));
    _error->setStyleSheet(QString::fromUtf8("color: rgb(230, 90, 80);"));
    _error->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _error->hide();
    layout->addWidget(_error, 1);

    QObject::connect(previousChange, &QPushButton::clicked, graph, &NodeGraph::goToPreviousComparisonChange);
    QObject::connect(nextChange, &QPushButton::clicked, graph, &NodeGraph::goToNextComparisonChange);
    QObject::connect(_restoreSelected, &QPushButton::clicked, this, [this]() {
        QString error;
        MergePicks::restore(_graph, MergePicks::selectedNodeIds(_graph), ProjectMerge::SpliceMode::eAsk, &error);
        showError(error);
    });
    QObject::connect(_revertSelected, &QPushButton::clicked, this, [this]() {
        QString error;
        MergePicks::revert(_graph, MergePicks::selectedNodeIds(_graph), &error);
        showError(error);
    });
    QObject::connect(clear, &QPushButton::clicked, this, [this]() {
        _graph->getGui()->clearGraphComparison();
    });
    QObject::connect(previousConflict, &QPushButton::clicked, graph, &NodeGraph::goToPreviousMergeConflict);
    QObject::connect(nextConflict, &QPushButton::clicked, graph, &NodeGraph::goToNextMergeConflict);
    const auto bindAll = [this](QPushButton* button, ProjectMerge::ConflictSide side) {
        QObject::connect(button, &QPushButton::clicked, this, [this, side]() {
            QString error;
            MergePopover::resolveSide(_graph->getGui(), allConflicts(_graph->getGui()), side, &error);
            showError(error);
        });
    };
    bindAll(allMine, ProjectMerge::ConflictSide::eMine);
    bindAll(allTheirs, ProjectMerge::ConflictSide::eTheirs);
    QObject::connect(openPanel, &QPushButton::clicked, this, [this]() {
        _graph->getGui()->ensureMergeConflictPanelVisible();
    });

    Gui* gui = graph->getGui();
    if (gui) {
        QObject::connect(gui, SIGNAL(graphComparisonChanged()), this, SLOT(refresh()));
        const ProjectPtr project = gui->getApp()->getProject();
        if (project) {
            QObject::connect(project.get(), SIGNAL(mergeConflictsChanged()), this, SLOT(refresh()));
        }
    }
    QObject::connect(graph, SIGNAL(selectionChanged()), this, SLOT(refresh()));
    if (NodeGraphDiffOverlay* overlay = graph->getDiffOverlay()) {
        QObject::connect(overlay, SIGNAL(ghostSelectionChanged()), this, SLOT(refresh()));
    }
    hide();
}

NodeGraphMergeBar::~NodeGraphMergeBar()
{
}

NodeGraphMergeBar::ModeEnum
NodeGraphMergeBar::modeOf(Gui* gui)
{
    if (!gui) {
        return eModeNone;
    }
    if (gui->getGraphComparison()) {
        return eModeComparison;
    }
    const ProjectPtr project = gui->getApp() ? gui->getApp()->getProject() : ProjectPtr();

    return (project && !project->getMergeConflicts().empty()) ? eModeConflicts : eModeNone;
}

QString
NodeGraphMergeBar::modeName(ModeEnum mode)
{
    switch (mode) {
    case eModeComparison:
        return QString::fromUtf8("comparison");
    case eModeConflicts:
        return QString::fromUtf8("conflicts");
    case eModeNone:
        break;
    }

    return QString::fromUtf8("none");
}

void
NodeGraphMergeBar::showError(const QString& error)
{
    _error->setText(error);
    _error->setToolTip(error);
    _error->setVisible(!error.isEmpty());
}

void
NodeGraphMergeBar::refresh()
{
    Gui* gui = _graph->getGui();
    const ModeEnum mode = modeOf(gui);
    if (mode != _mode) {
        showError(QString());
    }
    _comparison->setVisible(mode == eModeComparison);
    _conflicts->setVisible(mode == eModeConflicts);
    if (mode == eModeComparison) {
        const ProjectMerge::ChangeSummary* summary = gui->getGraphComparison();
        int added = 0;
        int changed = 0;
        int removed = 0;
        int moved = 0;
        for (const ProjectMerge::NodeChange& change : summary->nodes) {
            if (change.kind == ProjectMerge::NodeChange::eAdded) {
                ++added;
            } else if (change.kind == ProjectMerge::NodeChange::eRemoved) {
                ++removed;
            } else if (NodeGraphDiffOverlay::isOnlyMoved(change)) {
                ++moved;
            } else {
                ++changed;
            }
        }
        const QString description = gui->getGraphComparisonDescription();
        _compared->setText(description.isEmpty() ? tr("Comparing") : tr("Comparing with %1").arg(description));
        _compared->setToolTip(_compared->text());
        _counts->setText(tr("%1 added, %2 changed, %3 removed, %4 moved").arg(added).arg(changed).arg(removed).arg(moved));
        _counts->setToolTip(_counts->text());
        bool anyRemoved = false;
        bool anyChanged = false;
        for (const std::string& id : MergePicks::selectedNodeIds(_graph)) {
            const ProjectMerge::NodeChange* change = summary->find(id);
            if (!change) {
                continue;
            }
            anyRemoved |= (change->kind == ProjectMerge::NodeChange::eRemoved);
            anyChanged |= (change->kind == ProjectMerge::NodeChange::eChanged) && !NodeGraphDiffOverlay::isOnlyMoved(*change);
        }
        _restoreSelected->setEnabled(anyRemoved);
        _revertSelected->setEnabled(anyChanged);
    } else if (mode == eModeConflicts) {
        const std::size_t count = gui->getApp()->getProject()->getMergeConflicts().size();
        _conflictCount->setText((count == 1) ? tr("1 merge conflict") : tr("%1 merge conflicts").arg(count));
    }
    if (mode != _mode) {
        _mode = mode;
        _graph->layoutMergeBar();
    }
} // NodeGraphMergeBar::refresh

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_NodeGraphMergeBar.cpp"
