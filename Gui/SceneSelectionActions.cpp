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

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QPointF>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>
#include <QStatusBar>
#include <QTimer>
#include <QUndoCommand>
#include <QtCore/QCoreApplication>

#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Plugin.h"
#include "Engine/ViewerInstance.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/SceneGraphPanel.h"
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

void
selectMatches(Gui* gui,
              const KnobStringPtr& knob)
{
    if (!gui || !knob) {
        return;
    }
    const std::string expression = knob->getValue(0);
    const QString label = QString::fromStdString(knob->getLabel());
    EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
    NodePtr node = effect ? effect->getNode() : NodePtr();
    if (PrimSelector::isBlank(expression)) {
        showHint(gui, tr("Select Matches: the %1 is empty.").arg(label));

        return;
    }
    std::string error;
    if (!PrimSelector::validate(expression, &error)) {
        showHint(gui, tr("Select Matches: the %1 is not valid: %2").arg(label, QString::fromStdString(error)));

        return;
    }
    if (!node || !node->getInput(0)) {
        showHint(gui, tr("Select Matches: the node has no scene on its input."));

        return;
    }
    const int generation = ++g_matchSearchGeneration;
    std::shared_ptr<SceneOps::UnloadedSearch> search = SceneOps::UnloadedSearch::start(node->getEffectInstance(), 0, expression);
    QTimer* timer = new QTimer(gui);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, timer, [gui, timer, search, generation, label]() {
        if (generation != g_matchSearchGeneration) {
            timer->deleteLater();

            return;
        }
        const SceneOps::UnloadedSearch::StatusEnum status = search->getStatus();
        if (status == SceneOps::UnloadedSearch::eStatusRunning) {
            return;
        }
        timer->deleteLater();
        if (status == SceneOps::UnloadedSearch::eStatusFailed) {
            showHint(gui, tr("Select Matches: %1").arg(QString::fromStdString(search->getError())));

            return;
        }
        if (status != SceneOps::UnloadedSearch::eStatusDone) {
            return;
        }
        QStringList matches;
        for (const std::string& path : search->getMatches()) {
            matches << QString::fromStdString(path);
        }
        if (matches.isEmpty()) {
            showHint(gui, tr("Select Matches: nothing matches the %1.").arg(label));

            return;
        }
        ViewerTab* viewer = selectionViewer(gui);
        SceneViewport* viewport = viewer ? viewer->getSceneViewport() : nullptr;
        if (viewport) {
            viewport->setSelection(matches);
        } else {
            for (SceneGraphPanel* panel : gui->getSceneGraphPanels()) {
                panel->showSelection(matches);
            }
        }
        showHint(gui, tr("Select Matches: %1 prims selected.").arg(matches.size()));
    });
    timer->start();
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
