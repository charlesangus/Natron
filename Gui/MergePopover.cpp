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

#include "MergePopover.h"

#include <algorithm>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/MergeConflictPreview.h"
#include "Gui/MergeConflictResolve.h"
#include "Gui/MergePicks.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphDiffOverlay.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kIndexProperty = "mergeConflictIndex";
const char* const kSideProperty = "mergeConflictSide";

NodeGuiPtr
nodeGuiOf(const ProjectPtr& project,
          const std::string& nodeId)
{
    const NodePtr node = (project && !nodeId.empty()) ? project->findNodeById(nodeId) : NodePtr();

    return node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
}

QPushButton*
addButton(QLayout* layout,
          QWidget* parent,
          const QString& text,
          const QString& objectName,
          const QString& toolTip)
{
    QPushButton* button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setToolTip(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button);

    return button;
}

QLabel*
addLabel(QLayout* layout,
         QWidget* parent,
         const QString& text,
         bool bold)
{
    QLabel* label = new QLabel(text, parent);
    label->setWordWrap(true);
    if (bold) {
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
    }
    layout->addWidget(label);

    return label;
}

} // anon namespace

MergePopover::MergePopover(NodeGraph* graph)
    : QFrame(graph->viewport())
    , _graph(graph)
    , _nodeId()
    , _layout(0)
    , _content(0)
    , _error(0)
{
    setObjectName(QString::fromUtf8("MergePopover"));
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(8, 6, 8, 6);
    _layout->setSpacing(4);
    _error = new QLabel(this);
    _error->setObjectName(QString::fromUtf8("mergePopoverError"));
    _error->setWordWrap(true);
    _error->setStyleSheet(QString::fromUtf8("color: rgb(230, 90, 80);"));
    _error->hide();
    _layout->addWidget(_error);
    hide();

    Gui* gui = graph->getGui();
    if (gui) {
        // Queued: a choice made from one of the popover's buttons rebuilds it, deleting the button.
        QObject::connect(gui, SIGNAL(graphComparisonChanged()), this, SLOT(rebuild()), Qt::QueuedConnection);
        const ProjectPtr project = gui->getApp()->getProject();
        if (project) {
            QObject::connect(project.get(), SIGNAL(mergeConflictsChanged()), this, SLOT(rebuild()), Qt::QueuedConnection);
        }
    }
}

MergePopover::~MergePopover()
{
    qApp->removeEventFilter(this);
}

std::vector<std::size_t>
MergePopover::conflictsOf(const ProjectPtr& project,
                          const std::string& nodeId)
{
    std::vector<std::size_t> indices;
    if (!project || nodeId.empty()) {
        return indices;
    }
    const std::vector<Serialization::MergeConflict>& conflicts = project->getMergeConflicts();
    for (std::size_t i = 0; i < conflicts.size(); ++i) {
        if (conflicts[i].nodeId == nodeId) {
            indices.push_back(i);
        }
    }

    return indices;
}

bool
MergePopover::resolveSide(Gui* gui,
                          const std::vector<std::size_t>& indices,
                          ProjectMerge::ConflictSide side,
                          QString* error)
{
    error->clear();
    const ProjectPtr project = gui->getApp()->getProject();
    const std::vector<Serialization::MergeConflict>& conflicts = project->getMergeConflicts();
    // Conflicts that cannot take the side stay, as the Merge Conflicts panel leaves them.
    std::vector<std::size_t> choosable;
    for (std::size_t index : indices) {
        if ((index < conflicts.size()) && ProjectMerge::canChooseSide(project, conflicts[index], side)) {
            choosable.push_back(index);
        }
    }

    return choosable.empty() || MergeConflictResolve::resolve(gui, choosable, side, error);
}

bool
MergePopover::isOpen() const
{
    return !_nodeId.empty();
}

std::string
MergePopover::nodeId() const
{
    return _nodeId;
}

bool
MergePopover::openFor(const NodeGuiPtr& node)
{
    const NodePtr internal = node ? node->getNode() : NodePtr();
    if (!internal) {
        return false;
    }
    if (!isOpen()) {
        qApp->installEventFilter(this);
    }
    _nodeId = internal->getNodeId();
    _error->hide();
    rebuild();
    if (isOpen()) {
        setFocus(Qt::PopupFocusReason);
    }

    return isOpen();
}

void
MergePopover::dismiss()
{
    if (!isOpen()) {
        return;
    }
    _nodeId.clear();
    qApp->removeEventFilter(this);
    hide();
    if (MergeConflictPreview* preview = _graph->getMergePreview()) {
        preview->clear();
    }
    if (hasFocus() || isAncestorOf(QApplication::focusWidget())) {
        _graph->setFocus(Qt::OtherFocusReason);
    }
}

void
MergePopover::showError(const QString& error)
{
    _error->setText(error);
    _error->setVisible(!error.isEmpty());
    adjustSize();
    reposition();
}

void
MergePopover::rebuild()
{
    if (!isOpen()) {
        return;
    }
    Gui* gui = _graph->getGui();
    const ProjectPtr project = gui ? gui->getApp()->getProject() : ProjectPtr();
    const NodeGuiPtr node = nodeGuiOf(project, _nodeId);
    if (!node || (node->getDagGui() != _graph)) {
        dismiss();

        return;
    }
    const ProjectMerge::ChangeSummary* summary = gui->getGraphComparison();
    // A comparison hides the conflicts until it is cleared.
    const std::vector<std::size_t> indices = summary ? std::vector<std::size_t>() : conflictsOf(project, _nodeId);
    const ProjectMerge::NodeChange* change = summary ? summary->find(_nodeId) : 0;
    if (change && ((change->kind != ProjectMerge::NodeChange::eChanged) || NodeGraphDiffOverlay::isOnlyMoved(*change))) {
        change = 0;
    }
    if (indices.empty() && !change) {
        dismiss();

        return;
    }

    if (_content) {
        _content->hide();
        _content->deleteLater();
    }
    _content = new QWidget(this);
    QVBoxLayout* box = new QVBoxLayout(_content);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);
    addLabel(box, _content, QString::fromStdString(node->getNode()->getLabel()), true);

    const std::vector<Serialization::MergeConflict>& conflicts = project->getMergeConflicts();
    for (std::size_t k = 0; k < indices.size(); ++k) {
        const std::size_t index = indices[k];
        const Serialization::MergeConflict& conflict = conflicts[index];
        const QString label = ProjectMerge::conflictLabel(conflict);
        addLabel(box, _content, label, false);
        QHBoxLayout* row = new QHBoxLayout;
        row->setSpacing(4);
        const std::vector<ProjectMerge::ConflictChoice> choices = ProjectMerge::availableChoices(project, conflict);
        for (std::size_t n = 0; n < choices.size(); ++n) {
            const ProjectMerge::ConflictChoice choice = choices[n];
            QPushButton* button = addButton(row, _content, choice.label, QString::fromUtf8("mergePopoverChoice_%1_%2").arg(k).arg(n),
                                            tr("%1: %2").arg(label, choice.label));
            if (choice.option.empty() && MergeConflictPreview::hasPreview(conflict.kind)) {
                button->setProperty(kIndexProperty, (qulonglong)index);
                button->setProperty(kSideProperty, (int)choice.side);
                button->installEventFilter(this);
            }
            QObject::connect(button, &QPushButton::clicked, this, [this, conflict, choice]() {
                Gui* g = _graph->getGui();
                const std::vector<std::size_t> now = MergeConflictResolve::indicesOf(g, std::vector<Serialization::MergeConflict>(1, conflict));
                if (now.empty()) {
                    return;
                }
                QString error;
                const bool done = choice.option.empty() ? MergeConflictResolve::resolve(g, now, choice.side, &error)
                                                        : MergeConflictResolve::resolveOption(g, now.front(), choice.option, &error);
                showError(done ? QString() : error);
            });
        }
        row->addStretch();
        box->addLayout(row);
    }
    if (!indices.empty()) {
        QHBoxLayout* row = new QHBoxLayout;
        row->setSpacing(4);
        QPushButton* allMine = addButton(row, _content, tr("All Mine"), QString::fromUtf8("mergePopoverAllMine"),
                                         tr("Give every conflict of this node the state this project had."));
        QPushButton* allTheirs = addButton(row, _content, tr("All Theirs"), QString::fromUtf8("mergePopoverAllTheirs"),
                                           tr("Give every conflict of this node the state the template had."));
        row->addStretch();
        box->addLayout(row);
        std::vector<Serialization::MergeConflict> mine;
        for (std::size_t index : indices) {
            mine.push_back(conflicts[index]);
        }
        const auto bindAll = [this, mine](QPushButton* button, ProjectMerge::ConflictSide side) {
            QObject::connect(button, &QPushButton::clicked, this, [this, mine, side]() {
                QString error;
                Gui* g = _graph->getGui();
                showError(resolveSide(g, MergeConflictResolve::indicesOf(g, mine), side, &error) ? QString() : error);
            });
        };
        bindAll(allMine, ProjectMerge::ConflictSide::eMine);
        bindAll(allTheirs, ProjectMerge::ConflictSide::eTheirs);
    }
    if (MergeConflictPreview* preview = _graph->getMergePreview()) {
        // Indices shift as conflicts are resolved, so a hover from before may name another one.
        preview->clearHover();
        preview->setShownConflicts(indices);
    }

    if (change) {
        addLabel(box, _content, tr("Changes"), true);
        const std::string nodeId = _nodeId;
        for (const ProjectMerge::NodeChange::KnobChange& knob : change->knobChanges) {
            QHBoxLayout* row = new QHBoxLayout;
            row->setSpacing(4);
            addLabel(row, _content, QString::fromStdString(knob.name), false);
            row->addStretch();
            QPushButton* take = addButton(row, _content, tr("Take"), QString::fromUtf8("mergePopoverTake_%1").arg(QString::fromStdString(knob.name)),
                                          tr("Give %1 the value the compared version has.").arg(QString::fromStdString(knob.name)));
            const std::string knobName = knob.name;
            QObject::connect(take, &QPushButton::clicked, this, [this, nodeId, knobName]() {
                QString error;
                showError(MergePicks::takeKnob(_graph, nodeId, knobName, &error) ? QString() : error);
            });
            box->addLayout(row);
        }
        QHBoxLayout* row = new QHBoxLayout;
        QPushButton* revert = addButton(row, _content, tr("Revert Node"), QString::fromUtf8("mergePopoverRevertNode"),
                                        tr("Give the node the state the compared version has."));
        row->addStretch();
        box->addLayout(row);
        QObject::connect(revert, &QPushButton::clicked, this, [this, nodeId]() {
            QString error;
            showError(MergePicks::revert(_graph, std::vector<std::string>(1, nodeId), &error) ? QString() : error);
        });
    }

    _layout->insertWidget(0, _content);
    adjustSize();
    show();
    raise();
    reposition();
} // MergePopover::rebuild

void
MergePopover::reposition()
{
    if (!isOpen()) {
        return;
    }
    Gui* gui = _graph->getGui();
    const NodeGuiPtr node = nodeGuiOf(gui ? gui->getApp()->getProject() : ProjectPtr(), _nodeId);
    if (!node) {
        return;
    }
    const QRectF scene = node->mapToScene(node->boundingRect()).boundingRect();
    const QPoint right = _graph->mapFromScene(scene.topRight());
    const QPoint left = _graph->mapFromScene(scene.topLeft());
    const QSize area = _graph->viewport()->size();
    const int gap = 8;
    int x = right.x() + gap;
    if ((x + width() > area.width()) && (left.x() - gap - width() >= 0)) {
        x = left.x() - gap - width();
    }
    x = std::max(0, std::min(x, area.width() - width()));
    const int y = std::max(0, std::min(right.y(), area.height() - height()));
    move(x, y);
}

void
MergePopover::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Escape) && (e->modifiers() == Qt::NoModifier)) {
        dismiss();
        e->accept();

        return;
    }
    QFrame::keyPressEvent(e);
}

bool
MergePopover::eventFilter(QObject* watched,
                          QEvent* e)
{
    if ((e->type() == QEvent::Enter) || (e->type() == QEvent::Leave)) {
        const QVariant index = watched->property(kIndexProperty);
        MergeConflictPreview* preview = _graph->getMergePreview();
        if (index.isValid() && preview) {
            if (e->type() == QEvent::Enter) {
                preview->hover((std::size_t)index.toULongLong(), (ProjectMerge::ConflictSide)watched->property(kSideProperty).toInt());
            } else {
                preview->clearHover();
            }
        }
    } else if (isOpen() && (e->type() == QEvent::MouseButtonPress)) {
        QWidget* widget = qobject_cast<QWidget*>(watched);
        // The graph handles its own clicks: one on a preview ghost picks and leaves the popover open.
        if (widget && (widget != this) && !isAncestorOf(widget) && (widget != _graph) && !_graph->isAncestorOf(widget)) {
            dismiss();
        }
    }

    return QFrame::eventFilter(watched, e);
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_MergePopover.cpp"
