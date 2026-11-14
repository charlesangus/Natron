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

#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QStatusBar>
#include <QUndoCommand>
#include <QtCore/QCoreApplication>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/USDExpandHierarchy.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Plugin.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/ViewerInstance.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/SceneViewport.h"
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

// Gives the prim knob of the node args create paths as its initial value, which no paths leave
// empty so the knob never evaluates with its normal default. A USDXformPrim wraps a selected prim
// rather than defining one there.
void
addInitialPrims(CreateNodeArgs& args,
                const QStringList& paths)
{
    const QString pluginID = QString::fromStdString(args.getProperty<std::string>(kCreateNodeArgsPropPluginID));
    const Plugin* plugin = appPTR->getPluginBinary(pluginID, args.getProperty<int>(kCreateNodeArgsPropPluginVersion, 0), args.getProperty<int>(kCreateNodeArgsPropPluginVersion, 1), false);

    if (!plugin || plugin->getPrimPathKnob().empty()) {
        return;
    }
    args.addParamDefaultValue<std::string>(plugin->getPrimPathKnob(), SceneSelectionActions::primPathsText(plugin->getPrimPathKnobTakesPattern() ? KnobString::ePrimPathHintPattern : KnobString::ePrimPathHintPath, paths));
    if (!paths.isEmpty() && (pluginID == QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM))) {
        args.addParamDefaultValue<int>(kSceneReparentParamMode, (int)eSceneReparentModeWrap);
    }
}

// Pushes the edit of node's prim knob; false when there is nothing to change.
bool
pushSet(Gui* gui,
        const NodePtr& node,
        const KnobStringPtr& knob,
        const QStringList& paths)
{
    UndoHistory* history = gui ? gui->getUndoHistory() : nullptr;
    const std::string text = SceneSelectionActions::primPathsText(knob->getPrimPathHint(), paths);

    if (!history || (text == knob->getValue(0))) {
        return false;
    }
    const QString label = tr("Set %1 of %2").arg(QString::fromStdString(knob->getLabel()), QString::fromStdString(node->getLabel()));
    history->push(new SetPrimKnobCommand(knob, text, label), UndoScope(UndoScope::holderForNode(node)));

    return true;
}

// The first input of node that isn't a mask and can take a scene; -1 when it has none.
int
sceneInput(const NodePtr& node)
{
    const EffectInstancePtr effect = node->getEffectInstance();
    const int inputs = node->getNInputs();

    for (int i = 0; i < inputs; ++i) {
        const DataKindEnum kind = effect->getInputDataKind(i);
        if (!effect->isInputMask(i) && ((kind == eDataKindScene) || (kind == eDataKindPolymorphic))) {
            return i;
        }
    }

    return -1;
}

// Finds the first output of viewed that can read created in place of viewed, and which of its
// inputs reads viewed. A viewer declares image inputs and takes a scene through an adapter, so
// the input's declared kind can't decide this.
bool
firstSceneOutput(const NodePtr& viewed,
                 const NodePtr& created,
                 NodePtr* output,
                 int* outputInput)
{
    for (const NodeWPtr& outputW : viewed->getOutputs()) {
        NodePtr candidate = outputW.lock();
        if (!candidate || (candidate == created) || !candidate->getParentMultiInstanceName().empty()) {
            continue;
        }
        const EffectInstancePtr effect = candidate->getEffectInstance();
        const int inputs = candidate->getNInputs();
        for (int i = 0; i < inputs; ++i) {
            if ((candidate->getInput(i) == viewed) && !effect->isInputMask(i) && !candidate->isInputDataKindUnacceptable(created, i)) {
                *output = candidate;
                *outputInput = i;

                return true;
            }
        }
    }

    return false;
}

// Puts created below viewed, moving output down when it is in the way.
void
placeBelow(const NodePtr& viewed,
           const NodePtr& created,
           const NodePtr& output)
{
    NodeGuiPtr viewedGui = std::dynamic_pointer_cast<NodeGui>(viewed->getNodeGui());
    NodeGuiPtr createdGui = std::dynamic_pointer_cast<NodeGui>(created->getNodeGui());

    if (!viewedGui || !createdGui) {
        return;
    }
    const QSize viewedSize = viewedGui->getSize();
    const QSize createdSize = createdGui->getSize();
    const QPointF viewedPos = viewedGui->scenePos();
    const QPointF position(viewedPos.x() + (viewedSize.width() - createdSize.width()) / 2.,
                           viewedPos.y() + viewedSize.height() + NodeGui::DEFAULT_OFFSET_BETWEEN_NODES);
    if (NodeGuiPtr outputGui = output ? std::dynamic_pointer_cast<NodeGui>(output->getNodeGui()) : NodeGuiPtr()) {
        outputGui->moveBelowPositionRecursively(QRectF(position, createdSize));
    }
    const QPointF inParent = createdGui->mapToParent(createdGui->mapFromScene(position));
    createdGui->setPosition(inParent.x(), inParent.y());
}

// Connects created after viewed and gives it viewed's first scene output only, as an undoable edit.
// The input is connected first so the output is never left without a scene, and so that a
// pass-through created resolves to a scene before the output is judged against it.
void
insertAfterViewed(const NodePtr& viewed,
                  const NodePtr& created)
{
    const int input = sceneInput(created);

    if (input == -1) {
        return;
    }
    const NodePtr previousInput = created->getInput(input);
    if (!created->replaceInput(viewed, input)) {
        return;
    }
    std::vector<ReconnectInputsCommand::InputChange> changes;
    changes.push_back(ReconnectInputsCommand::InputChange { created, input, previousInput, viewed });

    NodePtr output;
    int outputInput = -1;
    if (!created->isOutputNode() && firstSceneOutput(viewed, created, &output, &outputInput)) {
        if (output->replaceInput(created, outputInput)) {
            changes.push_back(ReconnectInputsCommand::InputChange { output, outputInput, viewed, created });
        } else {
            output.reset();
        }
    }
    placeBelow(viewed, created, output);

    NodeCollectionPtr group = viewed->getGroup();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;
    if (graph) {
        UndoScope scope;
        scope.touchNode(created);
        scope.touchNode(viewed);
        if (output) {
            scope.touchNode(output);
        }
        // The connections are made, so the command's first redo does nothing.
        graph->pushUndoCommand(new ReconnectInputsCommand(graph, changes), scope);
    }
}

typedef std::vector<std::pair<std::string, SceneDrawStates::Stored>> SetDrawStates;

// Clears the draw states a bake turned into rules. Undo puts them back over whatever the viewer
// set since, and leaves the states set elsewhere since as they are.
class BakeDrawStatesCommand
    : public QUndoCommand {
public:
    BakeDrawStatesCommand(SceneViewport* viewport,
                          const SetDrawStates& baked,
                          const QString& text)
        : QUndoCommand(text)
        , _viewport(viewport)
        , _baked(baked)
    {
        _paths.reserve(baked.size());
        for (const SetDrawStates::value_type& state : baked) {
            _paths.push_back(state.first);
        }
    }

    void undo() override
    {
        if (_viewport) {
            SceneViewport* viewport = _viewport;
            const SetDrawStates baked = _baked;
            _viewport->editWithGraphPolicy([viewport, baked]() { viewport->mergeSetDrawStates(baked); });
        }
    }

    void redo() override
    {
        if (_viewport) {
            // The rules stand in for the states, so the states go with the rules' policy: no frame
            // shows neither.
            SceneViewport* viewport = _viewport;
            const std::vector<std::string> paths = _paths;
            _viewport->editWithGraphPolicy([viewport, paths]() { viewport->clearSetDrawStates(paths); });
        }
    }

private:
    QPointer<SceneViewport> _viewport;
    SetDrawStates _baked;
    std::vector<std::string> _paths;
};

class StepScope {
public:
    StepScope(UndoHistory* history,
              const QString& text)
        : _history(history)
    {
        _history->openStep(text, true);
    }

    ~StepScope()
    {
        _history->closeStep();
    }

    StepScope(const StepScope&) = delete;
    StepScope& operator=(const StepScope&) = delete;

private:
    UndoHistory* _history;
};

// A disabled node's rules reach no viewer, so baking into one would change what is drawn.
bool
isExpandHierarchy(const NodePtr& node)
{
    return node && node->isActivated() && !node->isNodeDisabled() && dynamic_cast<USDExpandHierarchy*>(node->getEffectInstance().get());
}

NodePtr
expandHierarchyBelow(const NodePtr& viewed)
{
    for (const NodeWPtr& outputW : viewed->getOutputs()) {
        NodePtr output = outputW.lock();
        if (isExpandHierarchy(output) && (output->getInput(0) == viewed)) {
            return output;
        }
    }

    return NodePtr();
}

// Creates a USDExpandHierarchy reading viewed that takes viewed's place in every input viewed fed
// and that can read it, the viewer's included, as an undoable edit.
NodePtr
createExpandHierarchyAfter(const NodePtr& viewed,
                           NodeGraph* graph)
{
    AppInstancePtr app = viewed->getApp();
    if (!app) {
        return NodePtr();
    }
    CreateNodeArgs args(PLUGINID_NATRON_USDEXPANDHIERARCHY, viewed->getGroup());
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    // Its command goes in only once it reads viewed: a node that cannot is removed unseen.
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    NodePtr created = app->createNode(args);
    if (!created) {
        return NodePtr();
    }

    std::vector<ReconnectInputsCommand::InputChange> outputs;
    for (const NodeWPtr& outputW : viewed->getOutputs()) {
        NodePtr output = outputW.lock();
        if (!output || (output == created) || !output->getParentMultiInstanceName().empty()) {
            continue;
        }
        const EffectInstancePtr effect = output->getEffectInstance();
        const int inputs = output->getNInputs();
        for (int i = 0; i < inputs; ++i) {
            if ((output->getInput(i) == viewed) && !effect->isInputMask(i) && !output->isInputDataKindUnacceptable(created, i)) {
                outputs.push_back(ReconnectInputsCommand::InputChange { output, i, viewed, created });
            }
        }
    }

    const NodePtr previousInput = created->getInput(0);
    const NodeGuiPtr createdGui = std::dynamic_pointer_cast<NodeGui>(created->getNodeGui());
    if (!createdGui || !created->replaceInput(viewed, 0)) {
        created->destroyNode(false, false);

        return NodePtr();
    }
    AddMultipleNodesCommand* added = new AddMultipleNodesCommand(graph, createdGui);
    added->setCreationAutoSaves();
    graph->pushUndoCommand(added, NodesGuiList(1, createdGui));
    std::vector<ReconnectInputsCommand::InputChange> changes;
    changes.push_back(ReconnectInputsCommand::InputChange { created, 0, previousInput, viewed });
    UndoScope scope;
    scope.touchNode(created);
    scope.touchNode(viewed);
    NodePtr below;
    for (const ReconnectInputsCommand::InputChange& change : outputs) {
        NodePtr output = change.node.lock();
        if (output && output->replaceInput(created, change.inputNb)) {
            changes.push_back(change);
            scope.touchNode(output);
            if (!below) {
                below = output;
            }
        }
    }
    placeBelow(viewed, created, below);
    // The connections are made, so the command's first redo does nothing.
    graph->pushUndoCommand(new ReconnectInputsCommand(graph, changes), scope);

    return created;
} // createExpandHierarchyAfter

// Connects viewer's active input A to node, as an undoable edit, unless it reads node already.
void
viewNode(ViewerTab* viewer,
         const NodePtr& node)
{
    ViewerInstance* instance = viewer->getInternalNode();
    NodePtr viewerNode = instance ? instance->getNode() : NodePtr();
    NodeCollectionPtr group = viewerNode ? viewerNode->getGroup() : NodeCollectionPtr();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;
    if (!graph) {
        return;
    }
    int a = -1, b = -1;
    instance->getActiveInputs(a, b);
    if (a < 0) {
        return;
    }
    const NodePtr before = viewerNode->getInput(a);
    if ((before == node) || !viewerNode->replaceInput(node, a)) {
        return;
    }
    UndoScope scope;
    scope.touchNode(viewerNode);
    scope.touchNode(node);
    std::vector<ReconnectInputsCommand::InputChange> changes;
    changes.push_back(ReconnectInputsCommand::InputChange { viewerNode, a, before, node });
    // The connection is made, so the command's first redo does nothing.
    graph->pushUndoCommand(new ReconnectInputsCommand(graph, changes), scope);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

namespace SceneSelectionActions {
std::string
primPathsText(KnobString::PrimPathHintEnum hint,
              const QStringList& paths)
{
    if (paths.isEmpty()) {
        return std::string();
    }
    const QString text = (hint == KnobString::ePrimPathHintPath) ? paths.front() : paths.join(QLatin1Char(' '));

    return text.trimmed().toStdString();
}

QStringList
keepSelectionOrder(const QStringList& order,
                   const QStringList& selected)
{
    const QSet<QString> selectedSet(selected.begin(), selected.end());
    QStringList kept;
    QSet<QString> keptSet;

    for (const QString& path : order) {
        if (selectedSet.contains(path) && !keptSet.contains(path)) {
            kept << path;
            keptSet.insert(path);
        }
    }
    for (const QString& path : selected) {
        if (!keptSet.contains(path)) {
            kept << path;
            keptSet.insert(path);
        }
    }

    return kept;
}

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
    context.decorateArgs = [paths](CreateNodeArgs& args) {
        // The graph's auto-connect would give the new node every output of the viewed node.
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        addInitialPrims(args, paths);
    };
    // A node type the decorator could not reach, such as a PyPlug's, still gets the prims.
    context.created = [gui, paths, viewedWeak = NodeWPtr(viewed)](const NodePtr& created) {
        NodePtr viewedNode = viewedWeak.lock();
        if (viewedNode && viewedNode->isActivated()) {
            insertAfterViewed(viewedNode, created);
        }
        if (KnobStringPtr knob = primKnob(created)) {
            pushSet(gui, created, knob, paths);
        }
    };
    TabMenuBridge::launch(graph, context);
}

QString
bakeBlocker(ViewerTab* viewer)
{
    SceneViewport* viewport = viewer ? viewer->getSceneViewport() : nullptr;
    const NodePtr viewed = viewer ? viewer->getSceneViewedNode() : NodePtr();
    NodeCollectionPtr group = viewed ? viewed->getGroup() : NodeCollectionPtr();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;

    if (!viewport || !viewed || !graph) {
        return tr("Bake to Node: the viewer shows no scene.");
    }
    if (!viewport->hasSetDrawStates()) {
        return tr("Bake to Node: this viewer sets no draw state.");
    }

    return QString();
}

bool
bakeToNode(Gui* gui,
           ViewerTab* viewer)
{
    UndoHistory* history = gui ? gui->getUndoHistory() : nullptr;
    QString blocker = bakeBlocker(viewer);

    if (blocker.isEmpty() && !history) {
        blocker = tr("Bake to Node: the viewer shows no scene.");
    }
    if (!blocker.isEmpty()) {
        showHint(gui, blocker);

        return false;
    }
    SceneViewport* viewport = viewer->getSceneViewport();
    const NodePtr viewed = viewer->getSceneViewedNode();
    NodeGraph* graph = dynamic_cast<NodeGraph*>(viewed->getGroup()->getNodeGraph());
    const SetDrawStates states = viewport->getSetDrawStates();

    if (states.empty()) {
        showHint(gui, tr("Bake to Node: this viewer sets no draw state."));

        return false;
    }
    std::vector<std::pair<std::string, SceneDrawStateEnum>> stateOf;
    stateOf.reserve(states.size());
    for (const SetDrawStates::value_type& state : states) {
        stateOf.push_back(std::make_pair(state.first, state.second.state));
    }
    const std::vector<USDExpandHierarchy::Rule> rules = USDExpandHierarchy::getRulesForStates(stateOf);

    NodePtr target = isExpandHierarchy(viewed) ? viewed : expandHierarchyBelow(viewed);
    USDExpandHierarchy* expand = target ? dynamic_cast<USDExpandHierarchy*>(target->getEffectInstance().get()) : nullptr;
    const int room = expand ? expand->getFreeRuleCount() : (int)kUSDExpandHierarchyMaxRules;
    if ((int)rules.size() > room) {
        if (target) {
            showHint(gui, tr("Bake to Node: %1 has room for %2 more rules, and the draw states of this viewer take %3.").arg(QString::fromStdString(target->getLabel())).arg(room).arg(rules.size()));
        } else {
            showHint(gui, tr("Bake to Node: a USDExpandHierarchy holds at most %1 rules, and the draw states of this viewer take %2.").arg(room).arg(rules.size()));
        }

        return false;
    }

    StepScope step(history, tr("Bake to Node"));
    if (!target) {
        target = createExpandHierarchyAfter(viewed, graph);
        if (!target) {
            showHint(gui, tr("Bake to Node: the USDExpandHierarchy could not be created."));

            return false;
        }
    }
    if (!expand) {
        expand = dynamic_cast<USDExpandHierarchy*>(target->getEffectInstance().get());
    }
    if (!expand) {
        showHint(gui, tr("Bake to Node: the USDExpandHierarchy could not be created."));

        return false;
    }
    viewNode(viewer, target);
    expand->appendRules(rules);
    history->push(new BakeDrawStatesCommand(viewport, states, tr("Clear Draw States")), UndoScope());

    return true;
} // bakeToNode

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
