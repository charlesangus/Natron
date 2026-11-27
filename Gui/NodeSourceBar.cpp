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

#include "Gui/NodeSourceBar.h"

#include <algorithm>

#include <QComboBox>
#include <QHBoxLayout>
#include <QStringList>
#include <QTimer>
#include <QToolButton>

#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

namespace {

// How long the node selection must stay put before it changes the target, so that clicking
// through nodes reads only the last.
const int kSelectionSettleMs = 150;

// Lists the candidates each time it opens.
class CandidateChooser
    : public QComboBox {
public:
    CandidateChooser(Gui* gui,
                     const std::function<bool(const NodePtr&)>& isCandidate,
                     QWidget* parent)
        : QComboBox(parent)
        , _gui(gui)
        , _isCandidate(isCandidate)
    {
    }

    void populate(const NodePtr& current)
    {
        clear();
        addItem(followLabel, QString());
        GuiAppInstancePtr app = _gui ? _gui->getApp() : GuiAppInstancePtr();
        if (!app || !app->getProject()) {
            return;
        }
        NodesList nodes;
        app->getProject()->getNodes_recursive(nodes, true);
        QStringList names;
        for (const NodePtr& node : nodes) {
            if (node && _isCandidate(node)) {
                names << QString::fromStdString(node->getFullyQualifiedName());
            }
        }
        names.sort();
        for (const QString& name : names) {
            addItem(name, name);
        }
        const int index = current ? findData(QString::fromStdString(current->getFullyQualifiedName())) : 0;
        setCurrentIndex(std::max(0, index));
    }

    std::function<NodePtr()> currentNode;

    // The first item's text, which unpins.
    QString followLabel;

    void showPopup() override
    {
        const bool blocked = blockSignals(true);
        populate(currentNode ? currentNode() : NodePtr());
        blockSignals(blocked);
        QComboBox::showPopup();
    }

private:
    Gui* _gui;
    std::function<bool(const NodePtr&)> _isCandidate;
};

QString
followLabelOf(NodeSourceBar::FollowEnum follow)
{
    return (follow == NodeSourceBar::eFollowSelection) ? NodeSourceBar::tr("(Selected Node)") : NodeSourceBar::tr("(Active Viewer)");
}

} // anon namespace

struct NodeSourceBar::Implementation {
    NodeSourceBar* bar;
    Gui* gui;
    std::function<bool(const NodePtr&)> isCandidate;
    std::function<NodePtr()> shownNode;

    QToolButton* pinButton = nullptr;
    QComboBox* followChooser = nullptr;
    CandidateChooser* nodeChooser = nullptr;
    QTimer selectionPoll;

    FollowEnum follow = eFollowViewer;
    bool pinned = false;
    NodeWPtr pinnedNode;
    NodeWPtr selectedNode;

    Implementation(NodeSourceBar* bar,
                   Gui* gui,
                   const std::function<bool(const NodePtr&)>& isCandidate)
        : bar(bar)
        , gui(gui)
        , isCandidate(isCandidate)
    {
    }

    void showPinned(const NodePtr& node)
    {
        {
            const bool blocked = pinButton->blockSignals(true);
            pinButton->setChecked(bool(node));
            pinButton->blockSignals(blocked);
        }
        {
            const bool blocked = nodeChooser->blockSignals(true);
            nodeChooser->populate(node);
            nodeChooser->blockSignals(blocked);
        }
    }

    void onSelectionPoll()
    {
        if ((follow != eFollowSelection) || pinned) {
            Q_EMIT bar->settlingChanged();

            return;
        }
        const NodePtr node = bar->selectedCandidate();
        if (!node || (node == selectedNode.lock())) {
            Q_EMIT bar->settlingChanged();

            return;
        }
        selectedNode = node;
        Q_EMIT bar->targetChanged();
    }
};

NodeSourceBar::NodeSourceBar(Gui* gui,
                             const QString& objectNamePrefix,
                             const std::function<bool(const NodePtr&)>& isCandidate,
                             const ToolTips& toolTips,
                             QWidget* parent)
    : QWidget(parent)
    , _imp(new Implementation(this, gui, isCandidate))
{
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 0);
    layout->setSpacing(4);

    _imp->pinButton = new QToolButton(this);
    _imp->pinButton->setObjectName(objectNamePrefix + QString::fromUtf8("Pin"));
    _imp->pinButton->setText(tr("Pin"));
    _imp->pinButton->setCheckable(true);
    _imp->pinButton->setToolTip(toolTips.pin);
    layout->addWidget(_imp->pinButton);

    _imp->followChooser = new QComboBox(this);
    _imp->followChooser->setObjectName(objectNamePrefix + QString::fromUtf8("Follow"));
    _imp->followChooser->addItem(tr("Viewer"));
    _imp->followChooser->addItem(tr("Node Selection"));
    _imp->followChooser->setToolTip(toolTips.follow);
    layout->addWidget(_imp->followChooser);

    _imp->nodeChooser = new CandidateChooser(gui, isCandidate, this);
    _imp->nodeChooser->setObjectName(objectNamePrefix + QString::fromUtf8("Node"));
    _imp->nodeChooser->setToolTip(toolTips.node);
    _imp->nodeChooser->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _imp->nodeChooser->setMinimumContentsLength(12);
    _imp->nodeChooser->followLabel = followLabelOf(eFollowViewer);
    _imp->nodeChooser->currentNode = [this]() { return getPinnedNode(); };
    _imp->nodeChooser->populate(NodePtr());
    layout->addWidget(_imp->nodeChooser, 1);

    _imp->selectionPoll.setSingleShot(true);
    _imp->selectionPoll.setInterval(kSelectionSettleMs);
    QObject::connect(&_imp->selectionPoll, &QTimer::timeout, this, [this]() { _imp->onSelectionPoll(); });
    QObject::connect(_imp->pinButton, &QToolButton::toggled, this, [this](bool checked) {
        pinNode((checked && _imp->shownNode) ? _imp->shownNode() : NodePtr());
    });
    QObject::connect(_imp->followChooser, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        setFollow((index == (int)eFollowSelection) ? eFollowSelection : eFollowViewer);
    });
    QObject::connect(_imp->nodeChooser, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        const QString name = _imp->nodeChooser->itemData(index).toString();
        GuiAppInstancePtr app = _imp->gui ? _imp->gui->getApp() : GuiAppInstancePtr();

        pinNode((app && !name.isEmpty()) ? app->getNodeByFullySpecifiedName(name.toStdString()) : NodePtr());
    });
    if (gui) {
        QObject::connect(gui, &Gui::nodeSelectionChanged, this, [this]() {
            if ((_imp->follow == eFollowSelection) && !_imp->pinned) {
                _imp->selectionPoll.start();
                Q_EMIT settlingChanged();
            }
        });
    }
}

NodeSourceBar::~NodeSourceBar()
{
}

void
NodeSourceBar::setShownNode(const std::function<NodePtr()>& shownNode)
{
    _imp->shownNode = shownNode;
}

void
NodeSourceBar::pinNode(const NodePtr& node)
{
    _imp->pinned = bool(node);
    _imp->pinnedNode = node;
    if (!node && (_imp->follow == eFollowSelection)) {
        if (NodePtr selected = selectedCandidate()) {
            _imp->selectedNode = selected;
        }
    }
    _imp->showPinned(node);
    Q_EMIT targetChanged();
}

NodePtr
NodeSourceBar::getPinnedNode() const
{
    return _imp->pinned ? _imp->pinnedNode.lock() : NodePtr();
}

bool
NodeSourceBar::isPinned() const
{
    return _imp->pinned;
}

void
NodeSourceBar::setFollow(FollowEnum follow)
{
    if (_imp->follow != follow) {
        _imp->follow = follow;
        const bool blocked = _imp->followChooser->blockSignals(true);
        _imp->followChooser->setCurrentIndex((int)follow);
        _imp->followChooser->blockSignals(blocked);
        _imp->nodeChooser->followLabel = followLabelOf(follow);
        _imp->nodeChooser->setItemText(0, _imp->nodeChooser->followLabel);
    }
    if (follow == eFollowSelection) {
        _imp->selectedNode = selectedCandidate();
        _imp->selectionPoll.start();
    } else {
        _imp->selectedNode.reset();
        _imp->selectionPoll.stop();
    }
    Q_EMIT targetChanged();
}

NodeSourceBar::FollowEnum
NodeSourceBar::getFollow() const
{
    return _imp->follow;
}

QString
NodeSourceBar::followName(FollowEnum follow)
{
    return (follow == eFollowSelection) ? QString::fromUtf8("selection") : QString::fromUtf8("viewer");
}

NodePtr
NodeSourceBar::getTarget() const
{
    if (_imp->pinned) {
        return _imp->pinnedNode.lock();
    }

    return (_imp->follow == eFollowSelection) ? _imp->selectedNode.lock() : NodePtr();
}

NodePtr
NodeSourceBar::selectedCandidate() const
{
    Gui* gui = _imp->gui;
    NodeGraph* graph = gui ? gui->getLastSelectedGraph() : nullptr;

    if (!graph && gui) {
        graph = gui->getNodeGraph();
    }
    if (!graph) {
        return NodePtr();
    }
    const NodesGuiList& selection = graph->getSelectedNodes();
    for (NodesGuiList::const_reverse_iterator it = selection.rbegin(); it != selection.rend(); ++it) {
        NodePtr node = *it ? (*it)->getNode() : NodePtr();
        if (node && _imp->isCandidate(node)) {
            return node;
        }
    }

    return NodePtr();
}

void
NodeSourceBar::release()
{
    _imp->selectedNode.reset();
    if (_imp->pinned) {
        _imp->pinned = false;
        _imp->pinnedNode.reset();
        _imp->showPinned(NodePtr());
    }
}

void
NodeSourceBar::dropTarget()
{
    if (_imp->pinned) {
        pinNode(NodePtr());

        return;
    }
    _imp->selectedNode.reset();
    Q_EMIT targetChanged();
}

bool
NodeSourceBar::isSettling() const
{
    return _imp->selectionPoll.isActive();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_NodeSourceBar.cpp"
