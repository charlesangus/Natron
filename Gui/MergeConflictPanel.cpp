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

#include "MergeConflictPanel.h"

#include <map>
#include <set>
#include <string>
#include <variant>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/MergeConflictResolve.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/TabWidget.h"

#include "Serialization/Merge/MergeConflict.h"

NATRON_NAMESPACE_ENTER

namespace {
enum ColumnEnum {
    eColumnConflict = 0,
    eColumnMine,
    eColumnTheirs,
    eColumnBase,
    eColumnCount
};

// On conflict rows: the conflict's index in the project's list. On node rows: the node ID.
const int kConflictIndexRole = Qt::UserRole;
const int kNodeIdRole = Qt::UserRole + 1;

bool
sameConflictPlace(const Serialization::MergeConflict& a,
                  const Serialization::MergeConflict& b)
{
    return (a.kind == b.kind) && (a.templateKey == b.templateKey) && (a.baseNodeId == b.baseNodeId) && (a.nodeId == b.nodeId) && (a.knob == b.knob)
        && (a.dimension == b.dimension) && (a.time == b.time) && (a.item == b.item) && (a.input == b.input);
}

QString
valueText(const Serialization::KnobValueSerialization& value)
{
    return std::visit([](const auto& v) -> QString {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v ? QString::fromUtf8("on") : QString::fromUtf8("off");
        } else if constexpr (std::is_same_v<T, std::string>) {
            return QString::fromStdString(v);
        } else {
            return QString::number(v);
        }
    },
                      value);
}

QString
nodeText(const ProjectPtr& project,
         const std::string& id)
{
    if (id.empty()) {
        return MergeConflictPanel::tr("Top level");
    }
    const NodePtr node = project->findNodeById(id);

    return node ? QString::fromStdString(node->getScriptName()) : QString::fromStdString(id);
}

const Serialization::MergeConflictValue&
sideValue(const Serialization::MergeConflict& conflict,
          ProjectMerge::ConflictSide side)
{
    switch (side) {
    case ProjectMerge::ConflictSide::eMine:
        return conflict.mine;
    case ProjectMerge::ConflictSide::eTheirs:
        return conflict.theirs;
    case ProjectMerge::ConflictSide::eBase:
        break;
    }

    return conflict.base;
}

QString
sideText(const ProjectPtr& project,
         const Serialization::MergeConflict& conflict,
         ProjectMerge::ConflictSide which)
{
    const Serialization::MergeConflictValue& side = sideValue(conflict, which);
    switch (conflict.kind) {
    case Serialization::MergeConflictKind::eInput:
        if (side.connections.empty()) {
            return QString();
        }

        return side.connections.front().sourceId.empty() ? MergeConflictPanel::tr("Disconnected") : nodeText(project, side.connections.front().sourceId);
    case Serialization::MergeConflictKind::eStructuralInsert:
        return side.connections.empty() ? QString() : MergeConflictPanel::tr("Its own chain");
    case Serialization::MergeConflictKind::eReparent:
        return side.parent ? nodeText(project, *side.parent) : QString();
    case Serialization::MergeConflictKind::eAdopted:
        return side.knobs ? MergeConflictPanel::tr("%1 changed knobs").arg(side.knobs->size()) : QString();
    case Serialization::MergeConflictKind::eRotoShape:
        if (conflict.item.empty()) {
            return (side.roto && side.roto->hasSplineWarp) ? MergeConflictPanel::tr("Spline warp") : MergeConflictPanel::tr("No spline warp");
        }

        return side.roto ? MergeConflictPanel::tr("Shape") : MergeConflictPanel::tr("No shape");
    case Serialization::MergeConflictKind::eDeleteModify: {
        if (which == ProjectMerge::ConflictSide::eBase) {
            return MergeConflictPanel::tr("As imported");
        }
        const bool deleter = ((which == ProjectMerge::ConflictSide::eMine) == (conflict.side == Serialization::MergeSide::eMine));

        return deleter ? MergeConflictPanel::tr("Deleted") : MergeConflictPanel::tr("Changed");
    }
    case Serialization::MergeConflictKind::eValue:
    case Serialization::MergeConflictKind::eCurveKey:
        break;
    }
    switch (conflict.kind) {
    case Serialization::MergeConflictKind::eCurveKey:
        if (!side.key) {
            return MergeConflictPanel::tr("No key");
        }

        return QString::number(side.key->value);
    case Serialization::MergeConflictKind::eValue: {
        if (!side.knob) {
            return MergeConflictPanel::tr("Default");
        }
        // A conflict on every dimension of a folded knob shows the one dimension its sides saved.
        const int index = (conflict.dimension == Serialization::kMergeConflictAllDimensions) ? 0 : conflict.dimension;
        if (index < 0) {
            return MergeConflictPanel::tr("Changed");
        }
        if (index >= (int)side.knob->dimensions.size()) {
            return MergeConflictPanel::tr("Default");
        }
        const Serialization::KnobDimensionSerialization& dim = side.knob->dimensions[index];
        if (!dim.expression.empty()) {
            return QString::fromUtf8("= ") + QString::fromStdString(dim.expression);
        }
        if (dim.curve) {
            return MergeConflictPanel::tr("Animated, %1 keys").arg(dim.curve->keyframes.size());
        }
        if (dim.value) {
            return valueText(*dim.value);
        }

        return MergeConflictPanel::tr("Default");
    }
    default:
        return QString();
    }
}

QString
conflictText(const Serialization::MergeConflict& conflict)
{
    const QString knob = QString::fromStdString(conflict.knob);
    const QString item = QString::fromStdString(conflict.item);
    switch (conflict.kind) {
    case Serialization::MergeConflictKind::eValue:
        if (conflict.dimension == Serialization::kMergeConflictAllDimensions) {
            return MergeConflictPanel::tr("%1, all dimensions").arg(knob);
        }
        if (conflict.dimension < 0) {
            return MergeConflictPanel::tr("%1 (animation, options)").arg(knob);
        }

        return MergeConflictPanel::tr("%1, dimension %2").arg(knob).arg(conflict.dimension);
    case Serialization::MergeConflictKind::eCurveKey:
        return MergeConflictPanel::tr("%1, key at %2").arg(knob).arg(conflict.time ? *conflict.time : 0.);
    case Serialization::MergeConflictKind::eInput:
        return MergeConflictPanel::tr("Input %1 connected differently").arg(QString::fromStdString(conflict.input));
    case Serialization::MergeConflictKind::eStructuralInsert:
        return MergeConflictPanel::tr("Nodes inserted on the same link on both sides");
    case Serialization::MergeConflictKind::eDeleteModify: {
        const QString who = (conflict.side == Serialization::MergeSide::eMine) ? MergeConflictPanel::tr("this project") : MergeConflictPanel::tr("the template");
        if (!knob.isEmpty()) {
            return MergeConflictPanel::tr("%1 deleted in %2 and changed in the other").arg(knob, who);
        }
        if (!item.isEmpty()) {
            return MergeConflictPanel::tr("%1 deleted in %2 and changed in the other").arg(item, who);
        }

        return MergeConflictPanel::tr("Node deleted in %1 and changed in the other").arg(who);
    }
    case Serialization::MergeConflictKind::eReparent:
        return MergeConflictPanel::tr("Moved into different groups on both sides");
    case Serialization::MergeConflictKind::eAdopted:
        return MergeConflictPanel::tr("Matched to the template node by structure");
    case Serialization::MergeConflictKind::eRotoShape:
        return item.isEmpty() ? MergeConflictPanel::tr("Spline warp changed on both sides")
                              : MergeConflictPanel::tr("Shape %1 changed on both sides").arg(item);
    }

    return QString();
}

// Conflicts about one node are grouped under its ID in the merged graph, or its base ID when the
// merge left it out; the rest are the project's own.
std::string
groupKeyOf(const Serialization::MergeConflict& conflict)
{
    return conflict.nodeId.empty() ? conflict.baseNodeId : conflict.nodeId;
}

NodeGuiPtr
nodeGuiOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
}
} // anon namespace

struct MergeConflictPanel::Implementation {
    Gui* gui;
    ProjectWPtr project;
    QLabel* summary;
    QTreeWidget* tree;
    QPushButton* mine;
    QPushButton* theirs;
    QPushButton* base;
    QPushButton* accept;
    QPushButton* allMine;
    QPushButton* allTheirs;
    QWidget* optionRow;
    QHBoxLayout* optionLayout;
    std::vector<QPushButton*> optionButtons;
    std::set<std::string> badged;
    // Indexed by the conflict index each row stores.
    std::vector<Serialization::MergeConflict> shown;
    bool refreshing;

    Implementation(Gui* gui)
        : gui(gui)
        , project()
        , summary(0)
        , tree(0)
        , mine(0)
        , theirs(0)
        , base(0)
        , accept(0)
        , allMine(0)
        , allTheirs(0)
        , optionRow(0)
        , optionLayout(0)
        , optionButtons()
        , badged()
        , shown()
        , refreshing(false)
    {
    }

    QPushButton* makeButton(QWidget* parent,
                            QLayout* layout,
                            const char* name,
                            const QString& text,
                            const QString& tooltip)
    {
        QPushButton* b = new QPushButton(text, parent);
        b->setObjectName(QString::fromUtf8(name));
        b->setToolTip(tooltip);
        layout->addWidget(b);

        return b;
    }

    void refreshBadges(const ProjectPtr& p)
    {
        std::map<std::string, std::size_t> counts;
        for (const Serialization::MergeConflict& conflict : p->getMergeConflicts()) {
            if (!conflict.nodeId.empty()) {
                ++counts[conflict.nodeId];
            }
        }
        for (const std::string& id : badged) {
            if (counts.find(id) == counts.end()) {
                if (NodeGuiPtr nodeGui = nodeGuiOf(p->findNodeById(id))) {
                    nodeGui->setMergeConflictCount(0);
                }
            }
        }
        // A node whose NodeGui does not exist yet badges itself when it is made, so it is tracked
        // all the same, for its badge to clear once its conflicts are gone.
        badged.clear();
        for (const auto& entry : counts) {
            if (NodeGuiPtr nodeGui = nodeGuiOf(p->findNodeById(entry.first))) {
                nodeGui->setMergeConflictCount(entry.second);
            }
            badged.insert(entry.first);
        }
    }
};

MergeConflictPanel::MergeConflictPanel(Gui* gui)
    : QWidget(gui)
    , PanelWidget(this, gui)
    , _imp(new Implementation(gui))
{
    setObjectName(QString::fromUtf8("mergeConflicts"));
    setLabel(tr("Merge Conflicts").toStdString());

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(4);

    _imp->summary = new QLabel(this);
    _imp->summary->setObjectName(QString::fromUtf8("mergeConflictSummary"));
    layout->addWidget(_imp->summary);

    _imp->tree = new QTreeWidget(this);
    _imp->tree->setObjectName(QString::fromUtf8("mergeConflictTree"));
    _imp->tree->setColumnCount(eColumnCount);
    _imp->tree->setHeaderLabels(QStringList() << tr("Conflict") << tr("Mine") << tr("Theirs") << tr("Base"));
    _imp->tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _imp->tree->setUniformRowHeights(true);
    _imp->tree->header()->setStretchLastSection(false);
    _imp->tree->header()->setSectionResizeMode(eColumnConflict, QHeaderView::Stretch);
    for (int c = eColumnMine; c < eColumnCount; ++c) {
        _imp->tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    layout->addWidget(_imp->tree, 1);

    QWidget* sideRow = new QWidget(this);
    QHBoxLayout* sideLayout = new QHBoxLayout(sideRow);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(4);
    _imp->mine = _imp->makeButton(sideRow, sideLayout, "mergeConflictMine", tr("Mine"),
                                  tr("Gives the selected conflicts the state this project had before the update."));
    _imp->theirs = _imp->makeButton(sideRow, sideLayout, "mergeConflictTheirs", tr("Theirs"),
                                    tr("Gives the selected conflicts the state of the new template version."));
    _imp->base = _imp->makeButton(sideRow, sideLayout, "mergeConflictBase", tr("Base"),
                                  tr("Gives the selected conflicts the state of the template version imported before."));
    _imp->accept = _imp->makeButton(sideRow, sideLayout, "mergeConflictAccept", tr("Accept"),
                                    tr("Keeps what the update chose for the selected conflicts."));
    sideLayout->addStretch(1);
    layout->addWidget(sideRow);

    _imp->optionRow = new QWidget(this);
    _imp->optionRow->setObjectName(QString::fromUtf8("mergeConflictOptions"));
    _imp->optionLayout = new QHBoxLayout(_imp->optionRow);
    _imp->optionLayout->setContentsMargins(0, 0, 0, 0);
    _imp->optionLayout->setSpacing(4);
    _imp->optionLayout->addStretch(1);
    _imp->optionRow->setVisible(false);
    layout->addWidget(_imp->optionRow);

    QWidget* allRow = new QWidget(this);
    QHBoxLayout* allLayout = new QHBoxLayout(allRow);
    allLayout->setContentsMargins(0, 0, 0, 0);
    allLayout->setSpacing(4);
    _imp->allMine = _imp->makeButton(allRow, allLayout, "mergeConflictAllMine", tr("Resolve All Mine"),
                                     tr("Gives every conflict in the list the state this project had before the update, where it can be put back. To resolve one node, select its row and use Mine."));
    _imp->allTheirs = _imp->makeButton(allRow, allLayout, "mergeConflictAllTheirs", tr("Resolve All Theirs"),
                                       tr("Gives every conflict in the list the state of the new template version, where it can be put back. To resolve one node, select its row and use Theirs."));
    allLayout->addStretch(1);
    layout->addWidget(allRow);

    QObject::connect(_imp->tree, SIGNAL(itemSelectionChanged()), this, SLOT(onSelectionChanged()));
    QObject::connect(_imp->tree, SIGNAL(currentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)), this, SLOT(onCurrentItemChanged()));
    QObject::connect(_imp->mine, SIGNAL(clicked()), this, SLOT(onMineClicked()));
    QObject::connect(_imp->theirs, SIGNAL(clicked()), this, SLOT(onTheirsClicked()));
    QObject::connect(_imp->base, SIGNAL(clicked()), this, SLOT(onBaseClicked()));
    QObject::connect(_imp->accept, SIGNAL(clicked()), this, SLOT(onAcceptClicked()));
    QObject::connect(_imp->allMine, SIGNAL(clicked()), this, SLOT(onAllMineClicked()));
    QObject::connect(_imp->allTheirs, SIGNAL(clicked()), this, SLOT(onAllTheirsClicked()));

    if (gui->getApp()) {
        if (ProjectPtr project = gui->getApp()->getProject()) {
            _imp->project = project;
            QObject::connect(project.get(), SIGNAL(mergeConflictsChanged()), this, SLOT(refresh()));
            QObject::connect(project.get(), SIGNAL(loaded()), this, SLOT(onProjectLoaded()));
        }
    }
    refresh();
}

MergeConflictPanel::~MergeConflictPanel()
{
}

void
MergeConflictPanel::refresh()
{
    ProjectPtr project = _imp->project.lock();
    // The rows are rebuilt from scratch, so the selection is kept by what each conflict is about.
    std::vector<Serialization::MergeConflict> wasSelected;
    for (std::size_t index : getSelectedConflicts()) {
        if (index < _imp->shown.size()) {
            wasSelected.push_back(_imp->shown[index]);
        }
    }
    _imp->refreshing = true;
    _imp->tree->clear();
    const std::vector<Serialization::MergeConflict> conflicts = project ? project->getMergeConflicts() : std::vector<Serialization::MergeConflict>();
    _imp->shown = conflicts;

    std::map<std::string, QTreeWidgetItem*> groups;
    std::vector<std::string> order;
    for (std::size_t i = 0; i < conflicts.size(); ++i) {
        const Serialization::MergeConflict& conflict = conflicts[i];
        const std::string key = groupKeyOf(conflict);
        QTreeWidgetItem*& group = groups[key];
        if (!group) {
            group = new QTreeWidgetItem();
            group->setData(eColumnConflict, kNodeIdRole, QString::fromStdString(key));
            order.push_back(key);
        }
        QTreeWidgetItem* row = new QTreeWidgetItem(group);
        row->setData(eColumnConflict, kConflictIndexRole, (qulonglong)i);
        row->setText(eColumnConflict, conflictText(conflict));
        bool choosable = false;
        const std::pair<int, ProjectMerge::ConflictSide> columns[] = { { eColumnMine, ProjectMerge::ConflictSide::eMine },
                                                                       { eColumnTheirs, ProjectMerge::ConflictSide::eTheirs },
                                                                       { eColumnBase, ProjectMerge::ConflictSide::eBase } };
        for (const std::pair<int, ProjectMerge::ConflictSide>& column : columns) {
            if (project && ProjectMerge::canChooseSide(project, conflict, column.second)) {
                choosable = true;
                row->setText(column.first, sideText(project, conflict, column.second));
            } else {
                row->setText(column.first, tr("Not available"));
            }
        }
        row->setToolTip(eColumnConflict, choosable ? tr("The update chose one side. Choose another, or accept it.") : tr("The update chose for you. Review it in the node graph and accept it."));
    }
    for (const std::string& key : order) {
        QTreeWidgetItem* group = groups[key];
        QString name;
        if (key.empty()) {
            name = tr("Project");
        } else if (NodePtr node = project ? project->findNodeById(key) : NodePtr()) {
            const QString label = QString::fromStdString(node->getLabel());
            const QString path = QString::fromStdString(node->getFullyQualifiedName());
            name = (label == path) ? path : tr("%1 (%2)").arg(label, path);
        } else {
            name = tr("Node not in the project (%1)").arg(QString::fromStdString(key));
        }
        group->setText(eColumnConflict, tr("%1: %2").arg(name).arg(group->childCount()));
        _imp->tree->addTopLevelItem(group);
        group->setExpanded(true);
    }
    std::vector<std::size_t> reselect;
    for (std::size_t i = 0; i < conflicts.size(); ++i) {
        for (const Serialization::MergeConflict& previous : wasSelected) {
            if (sameConflictPlace(conflicts[i], previous)) {
                reselect.push_back(i);
                break;
            }
        }
    }
    selectConflicts(reselect);
    _imp->refreshing = false;

    const int count = (int)conflicts.size();
    const QString title = (count == 0) ? tr("Merge Conflicts") : tr("Merge Conflicts (%1)").arg(count);
    setLabel(title.toStdString());
    if (TabWidget* pane = getParentPane()) {
        pane->setTabLabel(this, title);
    }
    _imp->summary->setText((count == 0) ? tr("No unresolved merge conflicts.")
                                        : tr("%1 unresolved merge conflicts. The update kept this project's values where both sides changed.").arg(count));
    _imp->allMine->setEnabled(count > 0);
    _imp->allTheirs->setEnabled(count > 0);
    onSelectionChanged();
    if (project) {
        _imp->refreshBadges(project);
    }
}

void
MergeConflictPanel::onProjectLoaded()
{
    // The conflict list is set before the nodes are made, so the badges are refreshed now.
    refresh();
    ProjectPtr project = _imp->project.lock();
    if (project && !project->getMergeConflicts().empty()) {
        _imp->gui->ensureMergeConflictPanelVisible();
    }
}

std::vector<std::size_t>
MergeConflictPanel::getSelectedConflicts() const
{
    std::set<std::size_t> indices;
    for (QTreeWidgetItem* item : _imp->tree->selectedItems()) {
        if (item->parent()) {
            indices.insert((std::size_t)item->data(eColumnConflict, kConflictIndexRole).toULongLong());
        } else {
            for (int i = 0; i < item->childCount(); ++i) {
                indices.insert((std::size_t)item->child(i)->data(eColumnConflict, kConflictIndexRole).toULongLong());
            }
        }
    }

    return std::vector<std::size_t>(indices.begin(), indices.end());
}

void
MergeConflictPanel::selectConflicts(const std::vector<std::size_t>& indices)
{
    const std::set<std::size_t> wanted(indices.begin(), indices.end());
    _imp->tree->clearSelection();
    for (int g = 0; g < _imp->tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = _imp->tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem* row = group->child(i);
            if (wanted.count((std::size_t)row->data(eColumnConflict, kConflictIndexRole).toULongLong())) {
                row->setSelected(true);
            }
        }
    }
}

void
MergeConflictPanel::onSelectionChanged()
{
    if (_imp->refreshing) {
        return;
    }
    ProjectPtr project = _imp->project.lock();
    const std::vector<std::size_t> selected = getSelectedConflicts();
    bool choosable[3] = { false, false, false };
    const ProjectMerge::ConflictSide sides[3] = { ProjectMerge::ConflictSide::eMine, ProjectMerge::ConflictSide::eTheirs, ProjectMerge::ConflictSide::eBase };
    std::vector<Serialization::MergeConflict> conflicts;
    if (project) {
        conflicts = project->getMergeConflicts();
        for (std::size_t index : selected) {
            for (int i = 0; i < 3; ++i) {
                if ((index < conflicts.size()) && !choosable[i] && ProjectMerge::canChooseSide(project, conflicts[index], sides[i])) {
                    choosable[i] = true;
                }
            }
        }
    }
    _imp->mine->setEnabled(choosable[0]);
    _imp->theirs->setEnabled(choosable[1]);
    _imp->base->setEnabled(choosable[2]);
    _imp->accept->setEnabled(!selected.empty());

    // The clicked button may be the one whose resolution got here.
    for (QPushButton* button : _imp->optionButtons) {
        button->hide();
        button->deleteLater();
    }
    _imp->optionButtons.clear();
    if ((selected.size() == 1) && (selected.front() < conflicts.size())) {
        const std::size_t index = selected.front();
        for (const Serialization::MergeConflictOption& option : conflicts[index].options) {
            const std::string name = option.name;
            QPushButton* button = new QPushButton(QString::fromStdString(option.label), _imp->optionRow);
            button->setObjectName(QString::fromUtf8("mergeConflictOption_") + QString::fromStdString(name));
            button->setToolTip(tr("Resolves the selected conflict with %1.").arg(QString::fromStdString(option.label)));
            button->setEnabled(ProjectMerge::canChooseOption(project, conflicts[index], name));
            _imp->optionLayout->insertWidget(_imp->optionLayout->count() - 1, button);
            QObject::connect(button, &QPushButton::clicked, this, [this, index, name]() {
                resolveOption(index, name);
            });
            _imp->optionButtons.push_back(button);
        }
    }
    _imp->optionRow->setVisible(!_imp->optionButtons.empty());
}

void
MergeConflictPanel::onCurrentItemChanged()
{
    if (_imp->refreshing) {
        return;
    }
    QTreeWidgetItem* item = _imp->tree->currentItem();
    ProjectPtr project = _imp->project.lock();
    if (!item || !project) {
        return;
    }
    QTreeWidgetItem* group = item->parent() ? item->parent() : item;
    const std::string nodeId = group->data(eColumnConflict, kNodeIdRole).toString().toStdString();
    if (nodeId.empty()) {
        return;
    }
    NodeGuiPtr nodeGui = nodeGuiOf(project->findNodeById(nodeId));
    if (!nodeGui) {
        return;
    }
    if (NodeGraph* graph = nodeGui->getDagGui()) {
        graph->selectNode(nodeGui, false);
        graph->centerOnItem(nodeGui.get());
    }
    nodeGui->setVisibleSettingsPanel(true);
    if (nodeGui->getSettingPanel()) {
        _imp->gui->putSettingsPanelFirst(nodeGui->getSettingPanel());
    }
}

void
MergeConflictPanel::resolveConflicts(const std::vector<std::size_t>& indices,
                                     ProjectMerge::ConflictSide side)
{
    ProjectPtr project = _imp->project.lock();
    if (!project) {
        return;
    }
    // Conflicts that cannot take a side stay for the user to accept, so a mixed selection resolves
    // what it can without failing.
    const std::vector<Serialization::MergeConflict>& conflicts = project->getMergeConflicts();
    std::vector<std::size_t> choosable;
    for (std::size_t index : indices) {
        if ((index < conflicts.size()) && ProjectMerge::canChooseSide(project, conflicts[index], side)) {
            choosable.push_back(index);
        }
    }
    if (choosable.empty()) {
        return;
    }
    QString error;
    if (!MergeConflictResolve::resolve(_imp->gui, choosable, side, &error)) {
        _imp->gui->errorDialog(tr("Merge Conflicts").toStdString(), error.toStdString(), false);
    }
}

void
MergeConflictPanel::resolveOption(std::size_t index,
                                  const std::string& option)
{
    QString error;
    if (!MergeConflictResolve::resolveOption(_imp->gui, index, option, &error)) {
        _imp->gui->errorDialog(tr("Merge Conflicts").toStdString(), error.toStdString(), false);
    }
}

void
MergeConflictPanel::acceptConflicts(const std::vector<std::size_t>& indices)
{
    if (!indices.empty()) {
        MergeConflictResolve::accept(_imp->gui, indices);
    }
}

void
MergeConflictPanel::resolveAll(ProjectMerge::ConflictSide side)
{
    ProjectPtr project = _imp->project.lock();
    if (!project) {
        return;
    }
    std::vector<std::size_t> all(project->getMergeConflicts().size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    resolveConflicts(all, side);
}

void
MergeConflictPanel::onMineClicked()
{
    resolveConflicts(getSelectedConflicts(), ProjectMerge::ConflictSide::eMine);
}

void
MergeConflictPanel::onTheirsClicked()
{
    resolveConflicts(getSelectedConflicts(), ProjectMerge::ConflictSide::eTheirs);
}

void
MergeConflictPanel::onBaseClicked()
{
    resolveConflicts(getSelectedConflicts(), ProjectMerge::ConflictSide::eBase);
}

void
MergeConflictPanel::onAcceptClicked()
{
    acceptConflicts(getSelectedConflicts());
}

void
MergeConflictPanel::onAllMineClicked()
{
    resolveAll(ProjectMerge::ConflictSide::eMine);
}

void
MergeConflictPanel::onAllTheirsClicked()
{
    resolveAll(ProjectMerge::ConflictSide::eTheirs);
}

void
MergeConflictPanel::enterEvent(QEnterEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
MergeConflictPanel::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QWidget::leaveEvent(e);
}

void
MergeConflictPanel::keyPressEvent(QKeyEvent* e)
{
    handleUnCaughtKeyPressEvent(e);
    QWidget::keyPressEvent(e);
}

void
MergeConflictPanel::keyReleaseEvent(QKeyEvent* e)
{
    handleUnCaughtKeyUpEvent(e);
    QWidget::keyReleaseEvent(e);
}

void
MergeConflictPanel::mousePressEvent(QMouseEvent* e)
{
    takeClickFocus();
    QWidget::mousePressEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_MergeConflictPanel.cpp"
