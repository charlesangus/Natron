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

#include "NodeDiffPanel.h"

#include <algorithm>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <QFont>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>

#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kArrow = " \xe2\x86\x92 ";

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
changeRow(const QString& name,
          const QString& before,
          const QString& after)
{
    return name + QString::fromUtf8(": ") + before + QString::fromUtf8(kArrow) + after;
}

// A knob saved folded has one dimension standing for all of them.
const Serialization::KnobDimensionSerialization*
dimensionOf(const std::optional<Serialization::KnobSerialization>& knob,
            std::size_t index)
{
    if (!knob || knob->dimensions.empty()) {
        return nullptr;
    }
    if (knob->dimensions.size() == 1) {
        return &knob->dimensions[0];
    }

    return (index < knob->dimensions.size()) ? &knob->dimensions[index] : nullptr;
}

const Serialization::CurveSerialization*
curveOf(const Serialization::KnobDimensionSerialization* dimension)
{
    return (dimension && dimension->curve) ? &*dimension->curve : nullptr;
}

QString
keysText(const Serialization::CurveSerialization* curve)
{
    const std::size_t count = curve ? curve->keyframes.size() : 0;

    return (count == 1) ? NodeDiffPanel::tr("1 key") : NodeDiffPanel::tr("%1 keys").arg(count);
}

QString
dimensionText(const Serialization::KnobDimensionSerialization* dimension)
{
    if (!dimension) {
        return NodeDiffPanel::tr("default");
    }
    if (!dimension->expression.empty()) {
        return QString::fromUtf8("= ") + QString::fromStdString(dimension->expression);
    }
    if (dimension->master) {
        return NodeDiffPanel::tr("linked");
    }
    if (dimension->curve) {
        return keysText(&*dimension->curve);
    }
    if (dimension->value) {
        return valueText(*dimension->value);
    }

    return NodeDiffPanel::tr("default");
}

QString
timesText(const std::vector<double>& times)
{
    QStringList parts;
    for (double time : times) {
        parts << QString::number(time);
    }

    return parts.join(QString::fromUtf8(", "));
}

bool
sameKey(const Serialization::KeyFrameSerialization& a,
        const Serialization::KeyFrameSerialization& b)
{
    return (a.value == b.value) && (a.interpolation == b.interpolation) && (a.leftDerivative == b.leftDerivative) && (a.rightDerivative == b.rightDerivative)
        && (a.leftWeight == b.leftWeight) && (a.rightWeight == b.rightWeight);
}

// The key times added, removed and changed from before to after, or empty when the keys match.
QString
keyTimesText(const Serialization::CurveSerialization* before,
             const Serialization::CurveSerialization* after)
{
    std::map<double, const Serialization::KeyFrameSerialization*> beforeKeys;
    std::map<double, const Serialization::KeyFrameSerialization*> afterKeys;
    if (before) {
        for (const Serialization::KeyFrameSerialization& key : before->keyframes) {
            beforeKeys[key.time] = &key;
        }
    }
    if (after) {
        for (const Serialization::KeyFrameSerialization& key : after->keyframes) {
            afterKeys[key.time] = &key;
        }
    }
    std::vector<double> added;
    std::vector<double> removed;
    std::vector<double> changed;
    for (const std::pair<const double, const Serialization::KeyFrameSerialization*>& key : afterKeys) {
        std::map<double, const Serialization::KeyFrameSerialization*>::const_iterator old = beforeKeys.find(key.first);
        if (old == beforeKeys.end()) {
            added.push_back(key.first);
        } else if (!sameKey(*old->second, *key.second)) {
            changed.push_back(key.first);
        }
    }
    for (const std::pair<const double, const Serialization::KeyFrameSerialization*>& key : beforeKeys) {
        if (!afterKeys.count(key.first)) {
            removed.push_back(key.first);
        }
    }
    QStringList parts;
    if (!added.empty()) {
        parts << NodeDiffPanel::tr("added at %1").arg(timesText(added));
    }
    if (!removed.empty()) {
        parts << NodeDiffPanel::tr("removed at %1").arg(timesText(removed));
    }
    if (!changed.empty()) {
        parts << NodeDiffPanel::tr("changed at %1").arg(timesText(changed));
    }

    return parts.join(QString::fromUtf8("; "));
}

QString
curveRow(const QString& name,
         const QString& before,
         const QString& after,
         const Serialization::CurveSerialization* beforeCurve,
         const Serialization::CurveSerialization* afterCurve)
{
    QString row = changeRow(name, before, after);
    const QString times = keyTimesText(beforeCurve, afterCurve);
    if (!times.isEmpty()) {
        row += QString::fromUtf8(" (") + times + QString::fromUtf8(")");
    }

    return row;
}

QString
knobText(const std::optional<Serialization::KnobSerialization>& knob,
         std::size_t dimensions)
{
    if (!knob) {
        return NodeDiffPanel::tr("default");
    }
    if (knob->stringAnimation && !knob->stringAnimation->empty()) {
        return NodeDiffPanel::tr("animated");
    }
    QStringList texts;
    for (std::size_t i = 0; i < dimensions; ++i) {
        texts << dimensionText(dimensionOf(knob, i));
    }
    const bool allSame = std::all_of(texts.begin(), texts.end(), [&texts](const QString& text) {
        return text == texts.front();
    });

    return allSame ? texts.front() : texts.join(QString::fromUtf8(", "));
}

void
knobRows(const ProjectMerge::NodeChange::KnobChange& knob,
         QStringList* rows)
{
    const QString name = QString::fromStdString(knob.name);
    const std::size_t beforeCount = knob.before ? knob.before->dimensions.size() : 0;
    const std::size_t afterCount = knob.after ? knob.after->dimensions.size() : 0;
    const std::size_t dimensions = std::max<std::size_t>(1, std::max(beforeCount, afterCount));

    const std::list<Serialization::CurveSerialization> noCurves;
    const std::list<Serialization::CurveSerialization>& beforeParametric = (knob.before && knob.before->parametricCurves) ? *knob.before->parametricCurves : noCurves;
    const std::list<Serialization::CurveSerialization>& afterParametric = (knob.after && knob.after->parametricCurves) ? *knob.after->parametricCurves : noCurves;
    if (!beforeParametric.empty() || !afterParametric.empty()) {
        std::list<Serialization::CurveSerialization>::const_iterator b = beforeParametric.begin();
        std::list<Serialization::CurveSerialization>::const_iterator a = afterParametric.begin();
        for (int index = 0; (b != beforeParametric.end()) || (a != afterParametric.end()); ++index) {
            const Serialization::CurveSerialization* beforeCurve = (b != beforeParametric.end()) ? &*b++ : nullptr;
            const Serialization::CurveSerialization* afterCurve = (a != afterParametric.end()) ? &*a++ : nullptr;
            if (!keyTimesText(beforeCurve, afterCurve).isEmpty()) {
                rows->push_back(curveRow(NodeDiffPanel::tr("%1 curve %2").arg(name).arg(index), keysText(beforeCurve), keysText(afterCurve), beforeCurve, afterCurve));
            }
        }

        return;
    }

    bool animated = false;
    for (std::size_t i = 0; i < dimensions; ++i) {
        animated = animated || curveOf(dimensionOf(knob.before, i)) || curveOf(dimensionOf(knob.after, i));
    }
    if (!animated) {
        const QString before = knobText(knob.before, dimensions);
        const QString after = knobText(knob.after, dimensions);
        rows->push_back((before == after) ? NodeDiffPanel::tr("%1: changed").arg(name) : changeRow(name, before, after));

        return;
    }
    const bool folded = (dimensions == 1);
    const std::size_t rowCount = rows->size();
    for (std::size_t i = 0; i < dimensions; ++i) {
        const Serialization::KnobDimensionSerialization* beforeDimension = dimensionOf(knob.before, i);
        const Serialization::KnobDimensionSerialization* afterDimension = dimensionOf(knob.after, i);
        const QString dimensionName = folded ? name : QString::fromUtf8("%1[%2]").arg(name).arg(i);
        const QString beforeText = dimensionText(beforeDimension);
        const QString afterText = dimensionText(afterDimension);
        const Serialization::CurveSerialization* beforeCurve = curveOf(beforeDimension);
        const Serialization::CurveSerialization* afterCurve = curveOf(afterDimension);
        if (beforeCurve || afterCurve) {
            if ((beforeText != afterText) || !keyTimesText(beforeCurve, afterCurve).isEmpty()) {
                rows->push_back(curveRow(dimensionName, beforeText, afterText, beforeCurve, afterCurve));
            }
        } else if (beforeText != afterText) {
            rows->push_back(changeRow(dimensionName, beforeText, afterText));
        }
    }
    if (rows->size() == rowCount) {
        rows->push_back(NodeDiffPanel::tr("%1: changed").arg(name));
    }
} // knobRows

QString
sourceText(const std::string& source)
{
    return source.empty() ? NodeDiffPanel::tr("none") : QString::fromStdString(source);
}

} // namespace

struct NodeDiffPanel::Implementation {
    Gui* gui;
    QLabel* summary = nullptr;
    QListWidget* list = nullptr;
    QPushButton* clear = nullptr;
    QPointer<NodeGraph> graph;
    bool pending = false;

    explicit Implementation(Gui* g)
        : gui(g)
    {
    }
};

NodeDiffPanel::NodeDiffPanel(Gui* gui)
    : QWidget(gui)
    , PanelWidget(this, gui)
    , _imp(new Implementation(gui))
{
    setObjectName(QString::fromUtf8("nodeDiff"));
    setLabel(tr("Node Diff").toStdString());

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(4);

    _imp->summary = new QLabel(this);
    _imp->summary->setObjectName(QString::fromUtf8("nodeDiffSummary"));
    _imp->summary->setWordWrap(true);
    layout->addWidget(_imp->summary);

    _imp->list = new QListWidget(this);
    _imp->list->setObjectName(QString::fromUtf8("nodeDiffList"));
    _imp->list->setSelectionMode(QAbstractItemView::NoSelection);
    _imp->list->setUniformItemSizes(true);
    layout->addWidget(_imp->list, 1);

    _imp->clear = new QPushButton(tr("Clear Comparison"), this);
    _imp->clear->setObjectName(QString::fromUtf8("nodeDiffClear"));
    _imp->clear->setToolTip(tr("Removes the comparison's colours and removed nodes from the node graphs."));
    layout->addWidget(_imp->clear, 0, Qt::AlignLeft);

    QObject::connect(_imp->clear, SIGNAL(clicked()), this, SLOT(onClearClicked()));
    QObject::connect(gui, SIGNAL(graphComparisonChanged()), this, SLOT(onComparisonChanged()));
    if (gui->getApp()) {
        if (ProjectPtr project = gui->getApp()->getProject()) {
            QObject::connect(project.get(), SIGNAL(loaded()), this, SLOT(onProjectLoaded()));
        }
    }
    refresh();
}

NodeDiffPanel::~NodeDiffPanel()
{
}

void
NodeDiffPanel::scheduleRefresh(NodeGraph* graph)
{
    if (graph) {
        _imp->graph = graph;
    }
    if (_imp->pending) {
        return;
    }
    _imp->pending = true;
    QMetaObject::invokeMethod(this, "refresh", Qt::QueuedConnection);
}

QStringList
NodeDiffPanel::getRows() const
{
    QStringList rows;
    for (int i = 0; i < _imp->list->count(); ++i) {
        rows << _imp->list->item(i)->text();
    }

    return rows;
}

QStringList
NodeDiffPanel::describe(const ProjectMerge::NodeChange& change)
{
    QStringList rows;
    switch (change.kind) {
    case ProjectMerge::NodeChange::eAdded:
        rows << tr("Added");

        return rows;
    case ProjectMerge::NodeChange::eRemoved:
        rows << tr("Removed");

        return rows;
    case ProjectMerge::NodeChange::eChanged:
        break;
    }
    if (change.renamed) {
        rows << changeRow(tr("Renamed"), QString::fromStdString(change.previousScriptName), QString::fromStdString(change.scriptName));
    }
    for (const ProjectMerge::NodeChange::KnobChange& knob : change.knobChanges) {
        knobRows(knob, &rows);
    }
    for (const ProjectMerge::NodeChange::InputChange& input : change.inputChanges) {
        const QString name = input.label.empty() ? tr("Input") : tr("Input %1").arg(QString::fromStdString(input.label));
        rows << changeRow(name, sourceText(input.before), sourceText(input.after));
    }
    if (change.moved) {
        rows << tr("Moved");
    }
    if (change.appearance) {
        rows << tr("Appearance changed");
    }
    if (change.userKnobs) {
        rows << tr("User knobs changed");
    }
    if (change.roto) {
        rows << tr("Roto shapes changed");
    }
    if (change.tracker) {
        rows << tr("Tracks changed");
    }
    if (change.recreated) {
        rows << tr("Plug-in or group changed");
    }

    return rows;
}

void
NodeDiffPanel::refresh()
{
    _imp->pending = false;
    _imp->list->clear();
    const ProjectMerge::ChangeSummary* summary = _imp->gui->getGraphComparison();
    _imp->clear->setEnabled(summary != nullptr);
    if (!summary) {
        _imp->summary->setText(tr("No comparison. File > Compare with File, or Compare with Template Base on a template node, shows one here."));

        return;
    }

    int added = 0;
    int changed = 0;
    int removed = 0;
    for (const ProjectMerge::NodeChange& node : summary->nodes) {
        switch (node.kind) {
        case ProjectMerge::NodeChange::eAdded:
            ++added;
            break;
        case ProjectMerge::NodeChange::eChanged:
            ++changed;
            break;
        case ProjectMerge::NodeChange::eRemoved:
            ++removed;
            break;
        }
    }
    QString text = tr("%1 added, %2 changed, %3 removed.").arg(added).arg(changed).arg(removed);

    NodeGraph* graph = _imp->graph ? _imp->graph.data() : _imp->gui->getNodeGraph();
    QFont heading = _imp->list->font();
    heading.setBold(true);
    if (graph) {
        for (const NodeGuiPtr& gui : graph->getSelectedNodes()) {
            const NodePtr node = gui ? gui->getNode() : NodePtr();
            const ProjectMerge::NodeChange* found = node ? summary->find(node->getNodeId()) : nullptr;
            if (!found) {
                continue;
            }
            QListWidgetItem* title = new QListWidgetItem(QString::fromStdString(node->getScriptName()), _imp->list);
            title->setFont(heading);
            for (const QString& row : describe(*found)) {
                new QListWidgetItem(row, _imp->list);
            }
        }
    }
    if (_imp->list->count() == 0) {
        text += QString::fromUtf8(" ") + tr("Select an added or changed node to list what differs.");
    }
    _imp->summary->setText(text);
} // NodeDiffPanel::refresh

void
NodeDiffPanel::onComparisonChanged()
{
    if (_imp->gui->getGraphComparison()) {
        _imp->gui->ensureNodeDiffPanelVisible();
    }
    refresh();
}

void
NodeDiffPanel::onClearClicked()
{
    _imp->gui->clearGraphComparison();
}

void
NodeDiffPanel::onProjectLoaded()
{
    _imp->gui->clearGraphComparison();
}

void
NodeDiffPanel::enterEvent(QEnterEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
NodeDiffPanel::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QWidget::leaveEvent(e);
}

void
NodeDiffPanel::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Escape) && (e->modifiers() == Qt::NoModifier) && _imp->gui->getGraphComparison()) {
        _imp->gui->clearGraphComparison();
        e->accept();

        return;
    }
    handleUnCaughtKeyPressEvent(e);
    QWidget::keyPressEvent(e);
}

void
NodeDiffPanel::keyReleaseEvent(QKeyEvent* e)
{
    handleUnCaughtKeyUpEvent(e);
    QWidget::keyReleaseEvent(e);
}

void
NodeDiffPanel::mousePressEvent(QMouseEvent* e)
{
    takeClickFocus();
    QWidget::mousePressEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_NodeDiffPanel.cpp"
