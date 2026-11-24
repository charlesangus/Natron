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

#include "NodeGraph.h"

#include <memory>
#include <string>

#include <QStatusBar>
#include <QUndoCommand>

#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/NodeConvert.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/IVariantRegistry.h"
#include "Engine/UndoCommand.h"

#include "Gui/Gui.h"
#include "Gui/NodeGui.h"

#define kMakeINodeWarningTimeoutMs 10000

NATRON_NAMESPACE_ENTER

namespace {

class EngineUndoCommand
    : public QUndoCommand {
    UndoCommandPtr _command;

public:
    explicit EngineUndoCommand(const UndoCommandPtr& command)
        : QUndoCommand()
        , _command(command)
    {
        setText(QString::fromUtf8(command->getText().c_str()));
    }

    virtual void redo() OVERRIDE FINAL
    {
        _command->redo();
        if (_command->isObsolete()) {
            setObsolete(true);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        _command->undo();
    }
};

NodesList
toNodes(const NodesGuiList& guis)
{
    NodesList ret;

    for (NodesGuiList::const_iterator it = guis.begin(); it != guis.end(); ++it) {
        NodePtr node = *it ? (*it)->getNode() : NodePtr();
        if (node) {
            ret.push_back(node);
        }
    }

    return ret;
}

NodeGuiPtr
guiOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
}

} // anonymous namespace

bool
NodeGraph::canMakeINodeFromSelection(QString* reason) const
{
    const NodesList nodes = toNodes(getSelectedNodes());

    if (nodes.size() == 1) {
        if (exactIVariantOf(nodes.front())) {
            return true;
        }
        if (IVariantRegistry::builtin().forIVariant(nodes.front()->getPluginID())) {
            if (reason) {
                *reason = tr("The selected node is already an I-node.");
            }

            return false;
        }
    }
    std::string why;
    if (LoopWrapCommand::canWrap(nodes, &why)) {
        return true;
    }
    if (reason) {
        *reason = QString::fromUtf8(why.c_str());
    }

    return false;
}

bool
NodeGraph::canMakeBaseNodeFromSelection(QString* reason) const
{
    const NodesList nodes = toNodes(getSelectedNodes());

    if ((nodes.size() == 1) && baseVariantOf(nodes.front())) {
        return true;
    }
    if (reason) {
        *reason = (nodes.size() == 1) ? tr("The selected node is not an I-node.") : tr("Select a single I-node.");
    }

    return false;
}

void
NodeGraph::makeINodeFromSelection()
{
    const NodesGuiList selection = getSelectedNodes();
    const NodesList nodes = toNodes(selection);
    std::string error;

    if ((nodes.size() == 1) && exactIVariantOf(nodes.front())) {
        std::shared_ptr<NodeConvertCommand> command = NodeConvertCommand::toIVariant(nodes.front(), &error);
        if (!command) {
            getGui()->statusBar()->showMessage(QString::fromUtf8(error.c_str()), kMakeINodeWarningTimeoutMs);

            return;
        }
        pushUndoCommand(new EngineUndoCommand(command), selection);
        const NodeGuiPtr converted = guiOf(command->getConverted());
        if (converted) {
            selectNode(converted, false);
        }

        return;
    }

    std::shared_ptr<LoopWrapCommand> command = LoopWrapCommand::create(nodes, &error);
    if (!command) {
        getGui()->statusBar()->showMessage(QString::fromUtf8(error.c_str()), kMakeINodeWarningTimeoutMs);

        return;
    }
    const std::string warning = LoopWrapCommand::warningFor(nodes);
    pushUndoCommand(new EngineUndoCommand(command), selection);

    const NodeGuiPtr loopEnd = guiOf(command->getLoopEnd());
    if (loopEnd) {
        // The driven-knob table starts empty: wrapping drives nothing, the user picks the knobs.
        selectNode(loopEnd, false);
        showSelectedNodeSettingsPanel();
    }
    if (!warning.empty()) {
        getGui()->statusBar()->showMessage(QString::fromUtf8(warning.c_str()), kMakeINodeWarningTimeoutMs);
    }
}

void
NodeGraph::makeBaseNodeFromSelection()
{
    const NodesGuiList selection = getSelectedNodes();
    const NodesList nodes = toNodes(selection);

    if (nodes.size() != 1) {
        return;
    }
    const NodePtr node = nodes.front();
    std::string error;
    std::shared_ptr<NodeConvertCommand> command = NodeConvertCommand::toBaseNode(node, &error);
    if (!command) {
        getGui()->statusBar()->showMessage(QString::fromUtf8(error.c_str()), kMakeINodeWarningTimeoutMs);

        return;
    }
    if (hasConnectedInput(node, kMapInputLabel)) {
        const QString message = tr("%1 has no Map input: making %2 its base node drops the Map connection. Continue?")
                                    .arg(QString::fromUtf8(IVariantRegistry::builtin().forIVariant(node->getPluginID())->baseId.c_str()))
                                    .arg(QString::fromUtf8(node->getLabel_mt_safe().c_str()));
        if (Dialogs::questionDialog(tr("Make Base Node").toStdString(), message.toStdString(), false) != eStandardButtonYes) {
            return;
        }
    }
    pushUndoCommand(new EngineUndoCommand(command), selection);
    const NodeGuiPtr converted = guiOf(command->getConverted());
    if (converted) {
        selectNode(converted, false);
    }
}

NATRON_NAMESPACE_EXIT
