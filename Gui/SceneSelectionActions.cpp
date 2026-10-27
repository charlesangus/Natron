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

#include "SceneSelectionActions.h"

#include <list>
#include <string>

#include <QStatusBar>
#include <QUndoCommand>
#include <QtCore/QCoreApplication>

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ViewerInstance.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/TabMenuBridge.h"
#include "Gui/UndoHistory.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

QString
tr(const char* text)
{
    return QCoreApplication::translate("SceneSelectionActions", text);
}

class SetPrimKnobCommand
    : public QUndoCommand {
public:
    SetPrimKnobCommand(const KnobStringPtr& knob,
                       const std::string& newValue,
                       const QString& text)
        : QUndoCommand(text)
        , _knob(knob)
        , _oldValue(knob->getValue(0))
        , _newValue(newValue)
    {
    }

    void undo() override
    {
        set(_oldValue);
    }

    void redo() override
    {
        set(_newValue);
    }

private:
    void set(const std::string& value)
    {
        if (KnobStringPtr knob = _knob.lock()) {
            knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, nullptr);
        }
    }

    KnobStringWPtr _knob;
    std::string _oldValue;
    std::string _newValue;
};

std::string
knobText(const KnobStringPtr& knob,
         const QStringList& paths)
{
    if (paths.isEmpty()) {
        return std::string();
    }
    const QString text = (knob->getPrimPathHint() == KnobString::ePrimPathHintPath) ? paths.front() : paths.join(QLatin1Char(' '));

    return text.trimmed().toStdString();
}

// Pushes the edit of node's prim knob; false when there is nothing to change.
bool
pushSet(Gui* gui,
        const NodePtr& node,
        const KnobStringPtr& knob,
        const QStringList& paths)
{
    UndoHistory* history = gui ? gui->getUndoHistory() : nullptr;
    const std::string text = knobText(knob, paths);

    if (!history || (text == knob->getValue(0))) {
        return false;
    }
    const QString label = tr("Set %1 of %2").arg(QString::fromStdString(knob->getLabel()), QString::fromStdString(node->getLabel()));
    history->push(new SetPrimKnobCommand(knob, text, label), UndoScope(UndoScope::holderForNode(node)));

    return true;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

namespace SceneSelectionActions {
ViewerTab*
selectionViewer(Gui* gui)
{
    if (!gui) {
        return nullptr;
    }
    if (ViewerTab* active = gui->getActiveViewer()) {
        return active;
    }
    // A removed viewer's tab can stay listed while its node is deactivated.
    for (ViewerTab* tab : gui->getViewersList()) {
        NodePtr node = (tab && tab->getInternalNode()) ? tab->getInternalNode()->getNode() : NodePtr();
        if (node && node->isActivated()) {
            return tab;
        }
    }

    return nullptr;
}

NodePtr
currentNode(Gui* gui)
{
    if (!gui) {
        return NodePtr();
    }
    const std::list<DockablePanel*> panels = gui->getVisiblePanels_mt_safe();
    for (DockablePanel* dockable : panels) {
        NodeSettingsPanel* panel = dynamic_cast<NodeSettingsPanel*>(dockable);
        NodeGuiPtr nodeGui = panel ? panel->getNode() : NodeGuiPtr();
        NodePtr node = nodeGui ? nodeGui->getNode() : NodePtr();
        if (node && node->isActivated()) {
            return node;
        }
    }

    return NodePtr();
}

KnobStringPtr
primKnob(const NodePtr& node)
{
    if (!node) {
        return KnobStringPtr();
    }
    KnobStringPtr path;
    for (const KnobIPtr& knob : node->getKnobs()) {
        KnobStringPtr string = std::dynamic_pointer_cast<KnobString>(knob);
        if (!string) {
            continue;
        }
        if (string->getPrimPathHint() == KnobString::ePrimPathHintPattern) {
            return string;
        }
        if (!path && (string->getPrimPathHint() == KnobString::ePrimPathHintPath)) {
            path = string;
        }
    }

    return path;
}

QString
setOnNodeBlocker(Gui* gui,
                 const QStringList& paths)
{
    NodePtr node = currentNode(gui);

    if (!node) {
        return tr("Set Selection on Current Node: open the settings of the node to set it on.");
    }
    KnobStringPtr knob = primKnob(node);
    const QString name = QString::fromStdString(node->getLabel());
    if (!knob) {
        return tr("Set Selection on Current Node: %1 has no prim path or pattern.").arg(name);
    }
    if (!knob->isEnabled(0) || knob->isSlave(0)) {
        return tr("Set Selection on Current Node: the %1 of %2 can't be edited.").arg(QString::fromStdString(knob->getLabel()), name);
    }
    if (paths.isEmpty()) {
        return tr("Set Selection on Current Node: no prims are selected.");
    }

    return QString();
}

bool
setOnCurrentNode(Gui* gui,
                 const QStringList& paths)
{
    const QString blocker = setOnNodeBlocker(gui, paths);

    if (!blocker.isEmpty()) {
        showHint(gui, blocker);

        return false;
    }
    NodePtr node = currentNode(gui);
    pushSet(gui, node, primKnob(node), paths);

    return true;
}

QString
createBlocker(Gui* gui,
              const NodePtr& viewed)
{
    NodeCollectionPtr group = viewed ? viewed->getGroup() : NodeCollectionPtr();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;

    if (!gui || !viewed || !graph) {
        return tr("Create Node from Selection: the viewer shows no scene.");
    }

    return QString();
}

void
createFromSelection(Gui* gui,
                    const NodePtr& viewed,
                    const QStringList& paths)
{
    const QString blocker = createBlocker(gui, viewed);

    if (!blocker.isEmpty()) {
        showHint(gui, blocker);

        return;
    }
    NodeGraph* graph = dynamic_cast<NodeGraph*>(viewed->getGroup()->getNodeGraph());
    TabMenuBridge::LaunchContext context;
    context.inputKind = eDataKindScene;
    context.needsPrimPathKnob = true;
    context.insertAfter = viewed;
    context.created = [gui, paths](const NodePtr& created) {
        if (KnobStringPtr knob = primKnob(created)) {
            pushSet(gui, created, knob, paths);
        }
    };
    TabMenuBridge::launch(graph, context);
}

void
showHint(Gui* gui,
         const QString& text)
{
    if (gui && gui->statusBar()) {
        gui->statusBar()->showMessage(text, 5000);
    }
}
} // namespace SceneSelectionActions

NATRON_NAMESPACE_EXIT
