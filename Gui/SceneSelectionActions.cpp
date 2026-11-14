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

// Before Qt's OpenGL headers.
#include "Gui/SceneViewport.h"

#include <algorithm>
#include <climits>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>
#include <QStatusBar>
#include <QTimer>
#include <QUndoCommand>
#include <QWidget>
#include <QtCore/QCoreApplication>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/USDExpandHierarchy.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Plugin.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneGraphPanel.h"
#include "Gui/SceneNodeStageSource.h"
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

// While a pick is armed, the keys that end it are taken from the viewer and the Scene Graph panels,
// and the mode is dropped when what it was armed for goes away.
class ArmedPick
    : public QObject {
public:
    ArmedPick(Gui* gui,
              ViewerTab* viewer,
              const NodePtr& node,
              const KnobStringPtr& knob,
              const std::function<void(const QStringList&)>& commit)
        : _gui(gui)
        , _viewer(viewer)
        , _node(node)
        , _knob(knob)
        , _commit(commit)
        , _label(QString::fromStdString(node->getScriptName_mt_safe() + "." + knob->getName()))
        , _timer(new QTimer(this))
    {
        _timer->setInterval(50);
        QObject::connect(_timer, &QTimer::timeout, this, [this]() {
            if (!stillValid()) {
                end();
            }
        });
        _timer->start();
        qApp->installEventFilter(this);
        publish(_label);
    }

    static QString text(const char* source)
    {
        return QCoreApplication::translate("SceneSelectionActions", source);
    }

    const QString& label() const
    {
        return _label;
    }

    // Stops the mode and clears what it showed; the object is deleted once the event loop is back.
    void end();

    bool eventFilter(QObject* watched,
                     QEvent* e) override
    {
        if (_ended || ((e->type() != QEvent::KeyPress) && (e->type() != QEvent::ShortcutOverride))) {
            return false;
        }
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        const bool enter = (k->key() == Qt::Key_Return) || (k->key() == Qt::Key_Enter);
        const bool escape = (k->key() == Qt::Key_Escape);
        if ((!enter && !escape) || (k->modifiers() & ~Qt::KeypadModifier) || !inPickSurface(watched)) {
            return false;
        }
        if (e->type() == QEvent::ShortcutOverride) {
            e->accept();

            return false;
        }
        if (escape) {
            end();
            SceneSelectionActions::showHint(_gui, text("Pick cancelled."));
        } else {
            commitSelection();
        }
        e->accept();

        return true;
    }

private:
    // The viewer's scene view and the Scene Graph panels, but not their text fields, which keep
    // Enter and Esc for themselves.
    static bool inPickSurface(QObject* watched)
    {
        QWidget* widget = qobject_cast<QWidget*>(watched);

        if (!widget || qobject_cast<QLineEdit*>(widget)) {
            return false;
        }
        for (QWidget* w = widget; w; w = w->parentWidget()) {
            if (dynamic_cast<SceneViewport*>(w) || dynamic_cast<SceneGraphPanel*>(w)) {
                return true;
            }
        }

        return false;
    }

    bool stillValid() const
    {
        NodePtr node = _node.lock();
        if (!_gui || !_viewer || !node || !node->isActivated() || !_knob.lock() || (SceneSelectionActions::selectionViewer(_gui) != _viewer.data())) {
            return false;
        }
        for (DockablePanel* dockable : _gui->getVisiblePanels_mt_safe()) {
            NodeSettingsPanel* panel = dynamic_cast<NodeSettingsPanel*>(dockable);
            NodeGuiPtr nodeGui = panel ? panel->getNode() : NodeGuiPtr();
            if (nodeGui && (nodeGui->getNode() == node)) {
                return true;
            }
        }

        return false;
    }

    void commitSelection()
    {
        const QStringList paths = SceneSelectionActions::currentSelection(_gui);

        if (paths.isEmpty()) {
            SceneSelectionActions::showHint(_gui, text("No prims are selected."));

            return;
        }
        const std::function<void(const QStringList&)> commit = _commit;
        QPointer<Gui> gui = _gui;
        end();
        commit(paths);
        if (gui) {
            SceneSelectionActions::showHint(gui, text("Picked %1 prims.").arg(paths.size()));
        }
    }

    void publish(const QString& label)
    {
        if (!_gui) {
            return;
        }
        for (ViewerTab* tab : _gui->getViewersList()) {
            SceneViewport* viewport = tab ? tab->getSceneViewport() : nullptr;
            if (viewport) {
                viewport->setArmedPick((tab == _viewer.data()) ? label : QString());
            }
        }
        for (SceneGraphPanel* panel : _gui->getSceneGraphPanels()) {
            panel->setArmedPick(label);
        }
    }

    QPointer<Gui> _gui;
    QPointer<ViewerTab> _viewer;
    NodeWPtr _node;
    KnobStringWPtr _knob;
    std::function<void(const QStringList&)> _commit;
    QString _label;
    QTimer* _timer;
    bool _ended = false;
};

ArmedPick* g_armedPick = nullptr;

void
ArmedPick::end()
{
    if (_ended) {
        return;
    }
    _ended = true;
    _timer->stop();
    qApp->removeEventFilter(this);
    publish(QString());
    if (g_armedPick == this) {
        g_armedPick = nullptr;
    }
    deleteLater();
}

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

// Pushes the edit of node's prim knob to text; false when there is nothing to change.
bool
pushText(Gui* gui,
         const NodePtr& node,
         const KnobStringPtr& knob,
         const std::string& text)
{
    UndoHistory* history = gui ? gui->getUndoHistory() : nullptr;

    if (!history || (text == knob->getValue(0))) {
        return false;
    }
    const QString label = tr("Set %1 of %2").arg(QString::fromStdString(knob->getLabel()), QString::fromStdString(node->getLabel()));
    history->push(new SetPrimKnobCommand(knob, text, label), UndoScope(UndoScope::holderForNode(node)));

    return true;
}

bool
pushSet(Gui* gui,
        const NodePtr& node,
        const KnobStringPtr& knob,
        const QStringList& paths)
{
    return pushText(gui, node, knob, SceneSelectionActions::primPathsText(knob->getPrimPathHint(), paths));
}

KnobStringPtr
firstPatternKnob(const NodePtr& node)
{
    if (!node) {
        return KnobStringPtr();
    }
    for (const KnobIPtr& knob : node->getKnobs()) {
        KnobStringPtr string = std::dynamic_pointer_cast<KnobString>(knob);
        if (string && (string->getPrimPathHint() == KnobString::ePrimPathHintPattern)) {
            return string;
        }
    }

    return KnobStringPtr();
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

// Whether token is an operator of a path expression, or holds one.
bool
isExpressionToken(const QString& token)
{
    return (token == QLatin1String("-")) || (token == QLatin1String("+")) || (token == QLatin1String("&")) || (token == QLatin1String("~"))
        || token.contains(QLatin1Char('(')) || token.contains(QLatin1Char(')')) || token.contains(QLatin1Char('~')) || token.contains(QLatin1Char('&')) || token.contains(QLatin1Char('%'));
}

bool
isOperatorToken(const QString& token)
{
    return (token == QLatin1String("-")) || (token == QLatin1String("+")) || (token == QLatin1String("&")) || (token == QLatin1String("~"));
}

bool
isWildcardToken(const QString& token)
{
    return token.contains(QLatin1Char('*')) || token.contains(QLatin1Char('?')) || token.contains(QLatin1Char('[')) || token.contains(QLatin1String("//"));
}

// The search Select Matches started last; an older one that ends later is dropped.
int g_matchSearchGeneration = 0;

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

std::string
editPrimPathsText(KnobString::PrimPathHintEnum hint,
                  PrimEditEnum edit,
                  const std::string& current,
                  const QStringList& paths)
{
    if ((hint != KnobString::ePrimPathHintPattern) || (edit == ePrimEditReplace)) {
        return primPathsText(hint, paths);
    }
    const QStringList tokens = QString::fromStdString(current).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    bool isExpression = false;
    bool hasWildcard = false;
    for (const QString& token : tokens) {
        isExpression = isExpression || isExpressionToken(token);
        hasWildcard = hasWildcard || isWildcardToken(token);
    }
    QStringList wanted;
    for (const QString& path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty() && !wanted.contains(trimmed)) {
            wanted << trimmed;
        }
    }
    if (edit == ePrimEditAdd) {
        QStringList missing;
        for (const QString& path : wanted) {
            if (!tokens.contains(path)) {
                missing << path;
            }
        }
        if (missing.isEmpty()) {
            return tokens.join(QLatin1Char(' ')).toStdString();
        }
        const QString base = (isExpression && (tokens.size() > 1)) ? QStringLiteral("(%1)").arg(tokens.join(QLatin1Char(' '))) : tokens.join(QLatin1Char(' '));

        return (base.isEmpty() ? missing.join(QLatin1Char(' ')) : base + QLatin1Char(' ') + missing.join(QLatin1Char(' '))).toStdString();
    }

    QStringList kept = tokens;
    QStringList subtract;
    for (const QString& path : wanted) {
        bool removed = false;
        for (int i = kept.size() - 1; i >= 0; --i) {
            if (kept[i] != path) {
                continue;
            }
            // Dropping an entry beside an operator would leave the rest meaning something else.
            const bool beforeOperator = (i > 0) && isOperatorToken(kept[i - 1]);
            const bool afterOperator = (i + 1 < kept.size()) && isOperatorToken(kept[i + 1]);
            if (!isExpression || (!beforeOperator && !afterOperator)) {
                kept.removeAt(i);
                removed = true;
            }
        }
        if (!removed && (isExpression || hasWildcard)) {
            subtract << path;
        }
    }
    QString text = kept.join(QLatin1Char(' '));
    if (!subtract.isEmpty()) {
        if (text.isEmpty()) {
            return std::string();
        }
        if (kept.size() > 1) {
            text = QStringLiteral("(%1)").arg(text);
        }
        text += QStringLiteral(" - ") + subtract.join(QStringLiteral(" - "));
    }

    return text.toStdString();
}

QStringList
currentSelection(Gui* gui)
{
    ViewerTab* viewer = selectionViewer(gui);
    SceneViewport* viewport = viewer ? viewer->getSceneViewport() : nullptr;
    if (viewport) {
        return viewport->getSelection();
    }
    SceneGraphPanel* panel = gui ? gui->getSceneGraphPanel() : nullptr;

    return panel ? panel->getSelection() : QStringList();
}

namespace {
    constexpr int kMatchStepMs = 8;
    constexpr int kOfferMs = 60000;

    // The "Load and select" report in the status bar, if one is up.
    QPointer<QWidget> g_loadOffer;

    void
    dropLoadOffer()
    {
        QWidget* offer = g_loadOffer.data();
        if (!offer) {
            return;
        }
        if (QStatusBar* bar = qobject_cast<QStatusBar*>(offer->parentWidget())) {
            bar->removeWidget(offer);
        }
        offer->hide();
        offer->deleteLater();
        g_loadOffer.clear();
    }

    bool
    sceneReaches(const NodePtr& from,
                 const NodePtr& target)
    {
        std::vector<NodePtr> pending(1, from);
        std::set<const Node*> seen;
        while (!pending.empty()) {
            NodePtr node = pending.back();
            pending.pop_back();
            if (!node || !seen.insert(node.get()).second) {
                continue;
            }
            if (node == target) {
                return true;
            }
            for (std::size_t i = 0; i < node->getInputs().size(); ++i) {
                pending.push_back(node->getInput(i));
            }
        }

        return false;
    }

    // The viewer whose load rules the node's scene is opened with: the active one if its scene
    // reaches the node, else the first other showing a scene that does. A scene the node's input
    // holds is searched with whatever that viewer has loaded, as the viewer shows it.
    SceneViewport*
    loadRulesViewport(Gui* gui,
                      const NodePtr& node)
    {
        std::vector<ViewerTab*> tabs;
        if (ViewerTab* active = selectionViewer(gui)) {
            tabs.push_back(active);
        }
        for (ViewerTab* tab : gui->getViewersList()) {
            if (tab && (std::find(tabs.begin(), tabs.end(), tab) == tabs.end())) {
                tabs.push_back(tab);
            }
        }
        for (ViewerTab* tab : tabs) {
            if (tab->isShowingScene() && sceneReaches(tab->getSceneViewedNode(), node)) {
                return tab->getSceneViewport();
            }
        }

        return nullptr;
    }

    bool
    underPath(const std::string& path,
              const QStringList& roots)
    {
        for (const QString& root : roots) {
            const std::string prefix = root.toStdString();
            if ((path == prefix) || (path.compare(0, prefix.size() + 1, prefix + "/") == 0)) {
                return true;
            }
        }

        return false;
    }

    void startMatchSearch(Gui* gui, const NodePtr& node, const std::string& expression, const QString& label);

    bool
    isPolicySettled(SceneViewport* viewport)
    {
        return !viewport->isDrawPolicyPending() && !viewport->getRenderer().hasOutstandingStageLoads();
    }

    /**
     * Runs run once the viewer has no load outstanding and no graph policy match pending, unless a
     * newer search starts first. The search copies the viewer's load rules, and a policy still to
     * match or a Full + Load rule matching again after each batch may yet add loads to them.
     **/
    void
    whenPolicySettled(SceneViewport* viewport,
                      int generation,
                      const std::function<void()>& run)
    {
        if (isPolicySettled(viewport)) {
            run();

            return;
        }
        QObject* waiter = new QObject(viewport);
        QObject::connect(viewport, &SceneViewport::drawPolicySettled, waiter, [viewport, generation, run, waiter]() {
            if (generation != g_matchSearchGeneration) {
                waiter->deleteLater();

                return;
            }
            if (!isPolicySettled(viewport)) {
                return;
            }
            // run may lead the viewer to emit again before the deferred delete.
            QObject::disconnect(viewport, nullptr, waiter, nullptr);
            waiter->deleteLater();
            run();
        });
    }

    // Loads paths in the viewer as the Scene Graph panel's Load does, and searches again once the
    // viewer has taken the batches in, the graph policy's own among them. A payload that fails to
    // load ends it with a report.
    void
    loadAndSearch(Gui* gui,
                  const NodePtr& node,
                  const QStringList& paths,
                  const std::string& expression,
                  const QString& label)
    {
        QPointer<SceneViewport> viewport(loadRulesViewport(gui, node));
        if (!viewport) {
            showHint(gui, tr("Select Matches: there is no viewer to load in."));

            return;
        }
        const int generation = ++g_matchSearchGeneration;
        viewport->requestLoad(paths, true);
        showHint(gui, tr("Select Matches: loading %1 unloaded branches.").arg(paths.size()));
        whenPolicySettled(viewport.data(), generation, [gui, node, paths, expression, label, viewport]() {
            for (const std::pair<std::string, std::string>& failure : viewport->getRenderer().getStageLoadFailures()) {
                if (underPath(failure.first, paths)) {
                    showHint(gui, tr("Select Matches: %1 did not load: %2").arg(QString::fromStdString(failure.first), QString::fromStdString(failure.second)));

                    return;
                }
            }
            startMatchSearch(gui, node, expression, label);
        });
    }

    void
    showLoadOffer(Gui* gui,
                  const NodePtr& node,
                  const QString& report,
                  const QStringList& paths,
                  const std::string& expression,
                  const QString& label)
    {
        dropLoadOffer();
        QStatusBar* bar = gui ? gui->statusBar() : nullptr;
        if (!bar) {
            return;
        }
        QWidget* offer = new QWidget(bar);
        offer->setObjectName(QStringLiteral("knobPrimSelectMatchesOffer"));
        offer->setProperty("natronSelectMatchesUnloaded", paths.size());
        QHBoxLayout* layout = new QHBoxLayout(offer);
        layout->setContentsMargins(0, 0, 0, 0);
        QLabel* text = new QLabel(report, offer);
        text->setObjectName(QStringLiteral("knobPrimSelectMatchesReport"));
        QPushButton* button = new QPushButton(tr("Load and select"), offer);
        button->setObjectName(QStringLiteral("knobPrimSelectMatchesLoad"));
        button->setToolTip(tr("Loads these payloads in the viewer, then selects the matches inside them. Unload them in the Scene Graph panel."));
        layout->addWidget(text);
        layout->addWidget(button);
        bar->addPermanentWidget(offer);
        g_loadOffer = offer;
        QTimer::singleShot(kOfferMs, offer, [offer]() {
            if (g_loadOffer.data() == offer) {
                dropLoadOffer();
            }
        });
        QObject::connect(button, &QPushButton::clicked, offer, [gui, node, paths, expression, label]() {
            dropLoadOffer();
            loadAndSearch(gui, node, paths, expression, label);
        });
    }

    // The node's input scene, opened with nothing loaded and then with the viewer's load rules.
    struct MatchSearch {
        SceneGraphPrimWalk walk;
        QStringList found;
        std::shared_ptr<SceneNodeStageSource> source;
        QPointer<SceneViewport> viewport;
        bool offerLoad = false;

        ~MatchSearch()
        {
            if (source) {
                SceneNodeStageSource::reap({ source });
            }
        }
    };

    // The source's callbacks only hold the search weakly, so something must own it while its
    // scene is read on the source's thread. A child of the Gui, so it ends before the app does.
    class MatchSearchOwner
        : public QObject {
    public:
        explicit MatchSearchOwner(QObject* parent)
            : QObject(parent)
        {
        }

        std::shared_ptr<MatchSearch> search;
    };

    QPointer<MatchSearchOwner> g_matchSearchOwner;

    void
    ownMatchSearch(Gui* gui,
                   const std::shared_ptr<MatchSearch>& search)
    {
        delete g_matchSearchOwner.data();
        MatchSearchOwner* owner = new MatchSearchOwner(gui);
        owner->search = search;
        g_matchSearchOwner = owner;
    }

    // Deferred, since a search ends inside its source's callbacks, which the source must outlive.
    void
    releaseMatchSearch(const std::shared_ptr<MatchSearch>& search)
    {
        MatchSearchOwner* owner = g_matchSearchOwner.data();
        if (owner && (owner->search == search)) {
            g_matchSearchOwner.clear();
            owner->deleteLater();
        }
    }

    void
    reportMatches(Gui* gui,
                  const NodePtr& node,
                  const std::shared_ptr<MatchSearch>& search,
                  const std::string& expression,
                  const QString& label)
    {
        const QStringList matches = search->found;
        const QStringList unloaded = search->walk.getUnloadedBranchPaths();
        QString report;
        if (matches.isEmpty()) {
            report = tr("Select Matches: nothing loaded matches the %1.").arg(label);
        } else {
            report = tr("Select Matches: %1 prims selected.").arg(matches.size());
        }
        if (!unloaded.isEmpty()) {
            report += (unloaded.size() == 1) ? tr(" 1 unloaded branch not searched.")
                                             : tr(" %1 unloaded branches not searched.").arg(unloaded.size());
        }
        if (!matches.isEmpty()) {
            SceneViewport* viewport = search->viewport ? search->viewport.data() : nullptr;
            if (!viewport) {
                ViewerTab* viewer = selectionViewer(gui);
                viewport = (viewer && viewer->isShowingScene()) ? viewer->getSceneViewport() : nullptr;
            }
            if (viewport) {
                viewport->setSelection(matches);
            }
        }
        showHint(gui, report);
        releaseMatchSearch(search);
        if (!unloaded.isEmpty() && search->offerLoad) {
            showLoadOffer(gui, node, report, unloaded, expression, label);
        }
    }

    // Walks the stage in short slices under its read lock, so the UI goes on between them.
    void
    stepMatchSearch(Gui* gui,
                    const NodePtr& node,
                    const std::shared_ptr<MatchSearch>& search,
                    const std::string& expression,
                    const QString& label,
                    int generation)
    {
        std::vector<std::pair<std::string, std::string>> failures;
        search->source->readStage([&](const SceneStage* stage) {
            if (stage) {
                for (const SceneStage::LoadFailure& failure : stage->getLoadFailures()) {
                    failures.emplace_back(failure.prim.GetString(), failure.error);
                }
            }
        });
        if (!failures.empty()) {
            showHint(gui, tr("Select Matches: %1 did not load: %2").arg(QString::fromStdString(failures.front().first), QString::fromStdString(failures.front().second)));
            releaseMatchSearch(search);

            return;
        }
        search->walk.start();
        QTimer* timer = new QTimer(gui);
        timer->setInterval(0);
        QObject::connect(timer, &QTimer::timeout, timer, [gui, node, timer, search, generation, expression, label]() {
            if (generation != g_matchSearchGeneration) {
                timer->deleteLater();

                return;
            }
            bool shown = false;
            if (!search->source->tryReadStage([&](const SceneStage* stage) {
                    shown = (stage != nullptr);
                    search->walk.step(stage, kMatchStepMs, INT_MAX, [](const QString&) { return false; }, &search->found);
                })) {
                return;
            }
            if (!shown) {
                timer->deleteLater();
                showHint(gui, tr("Select Matches: the node's input shows no scene."));
                releaseMatchSearch(search);

                return;
            }
            if (!search->walk.isOver()) {
                return;
            }
            timer->deleteLater();
            reportMatches(gui, node, search, expression, label);
        });
        timer->start();
    }

    // Opens the node's input with the load rules the search's viewer has now, and walks it.
    void
    readMatchSearch(Gui* gui,
                    const NodePtr& node,
                    const std::shared_ptr<MatchSearch>& search,
                    const std::string& expression,
                    const QString& label,
                    int generation)
    {
        const NodePtr input = node->getInput(0);
        if (!input) {
            showHint(gui, tr("Select Matches: the node has no scene on its input."));

            return;
        }
        SceneLoadRules rules;
        if (search->viewport) {
            search->viewport->getRenderer().getStageLoadRules(&rules);
        }
        const std::weak_ptr<MatchSearch> weak(search);
        QPointer<Gui> guard(gui);
        search->source = std::make_shared<SceneNodeStageSource>(input, gui, [guard, node, weak, rules, expression, label, generation](bool ok, const QString& error) {
            std::shared_ptr<MatchSearch> current = weak.lock();
            if (!guard || !current || (generation != g_matchSearchGeneration)) {
                return;
            }
            if (!ok) {
                showHint(guard, tr("Select Matches: %1").arg(error));
                releaseMatchSearch(current);

                return;
            }
            if (sceneLoadRequestsFor(rules).empty()) {
                stepMatchSearch(guard, node, current, expression, label, generation);

                return;
            }
            current->source->requestLoadRules(rules); }, false);
        search->source->setLoadsChanged([guard, node, weak, expression, label, generation]() {
            std::shared_ptr<MatchSearch> current = weak.lock();
            if (guard && current && (generation == g_matchSearchGeneration)) {
                stepMatchSearch(guard, node, current, expression, label, generation);
            }
        });
        ownMatchSearch(gui, search);
        const GuiAppInstancePtr app = gui->getApp();
        search->source->request(app ? app->getTimeLine()->currentFrame() : 0.);
        showHint(gui, tr("Select Matches: reading the scene of %1...").arg(QString::fromStdString(input->getLabel())));
    }

    void
    startMatchSearch(Gui* gui,
                     const NodePtr& node,
                     const std::string& expression,
                     const QString& label)
    {
        dropLoadOffer();
        const NodePtr input = node ? node->getInput(0) : NodePtr();
        if (!input) {
            showHint(gui, tr("Select Matches: the node has no scene on its input."));

            return;
        }
        std::shared_ptr<MatchSearch> search(new MatchSearch);
        QString parseError;
        if (!search->walk.setExpression(QString::fromStdString(expression), &parseError)) {
            showHint(gui, tr("Select Matches: %1").arg(parseError));

            return;
        }
        search->viewport = loadRulesViewport(gui, node);
        search->offerLoad = !search->viewport.isNull();
        const int generation = ++g_matchSearchGeneration;
        if (search->viewport) {
            if (!isPolicySettled(search->viewport.data())) {
                showHint(gui, tr("Select Matches: waiting for the viewer's loads..."));
            }
            whenPolicySettled(search->viewport.data(), generation, [gui, node, search, expression, label, generation]() {
                readMatchSearch(gui, node, search, expression, label, generation);
            });

            return;
        }
        readMatchSearch(gui, node, search, expression, label, generation);
    }
} // anonymous namespace

void
selectMatches(Gui* gui,
              const KnobStringPtr& knob)
{
    if (!gui || !knob) {
        return;
    }
    const std::string expression = knob->getValue(0);
    const QString label = QString::fromStdString(knob->getLabel());
    if (PrimSelector::isBlank(expression)) {
        showHint(gui, tr("Select Matches: the %1 is empty.").arg(label));

        return;
    }
    std::string error;
    if (!PrimSelector::validate(expression, &error)) {
        showHint(gui, tr("Select Matches: the %1 is not valid: %2").arg(label, QString::fromStdString(error)));

        return;
    }
    EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
    startMatchSearch(gui, effect ? effect->getNode() : NodePtr(), expression, label);
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
applyPatternBlocker(Gui* gui)
{
    NodePtr node = currentNode(gui);

    if (!node) {
        return tr("Apply To Node: open the settings of the node to apply the expression to.");
    }
    KnobStringPtr knob = firstPatternKnob(node);
    const QString name = QString::fromStdString(node->getLabel());
    if (!knob) {
        return tr("Apply To Node: %1 has no prim pattern.").arg(name);
    }
    if (!knob->isEnabled(0) || knob->isSlave(0)) {
        return tr("Apply To Node: the %1 of %2 can't be edited.").arg(QString::fromStdString(knob->getLabel()), name);
    }

    return QString();
}

bool
applyPatternToCurrentNode(Gui* gui,
                          const std::string& expression)
{
    const QString blocker = applyPatternBlocker(gui);

    if (!blocker.isEmpty()) {
        showHint(gui, blocker);

        return false;
    }
    NodePtr node = currentNode(gui);
    pushText(gui, node, firstPatternKnob(node), expression);

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

bool
armPick(Gui* gui,
        const KnobStringPtr& knob,
        const std::function<void(const QStringList& paths)>& commit)
{
    ViewerTab* viewer = selectionViewer(gui);
    EffectInstance* effect = knob ? dynamic_cast<EffectInstance*>(knob->getHolder()) : nullptr;
    NodePtr node = effect ? effect->getNode() : NodePtr();

    if (!viewer || !viewer->getSceneViewport() || !node) {
        showHint(gui, tr("Pick: open a 3D viewer to select prims in."));

        return false;
    }
    cancelArmedPick();
    g_armedPick = new ArmedPick(gui, viewer, node, knob, commit);
    showHint(gui, tr("Pick for %1: select prims, Enter writes them, Esc cancels.").arg(g_armedPick->label()));

    return true;
}

void
cancelArmedPick()
{
    if (g_armedPick) {
        g_armedPick->end();
    }
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
