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

#include <functional>
#include <list>
#include <string>

#include <QFutureWatcher>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QStatusBar>
#include <QUndoCommand>
#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>

#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Plugin.h"
#include "Engine/TimeLine.h"
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

// Sets a node's group pattern knob and the choice of the class it selects, as one edit.
class SetGeoGroupCommand
    : public QUndoCommand {
public:
    SetGeoGroupCommand(const KnobStringPtr& group,
                       const std::string& pattern,
                       const KnobChoicePtr& type,
                       int typeIndex,
                       const QString& text)
        : QUndoCommand(text)
        , _group(group)
        , _oldPattern(group->getValue(0))
        , _newPattern(pattern)
        , _type(type)
        , _oldTypeIndex(type ? type->getValue(0) : -1)
        , _newTypeIndex(typeIndex)
    {
    }

    void undo() override
    {
        set(_oldPattern, _oldTypeIndex);
    }

    void redo() override
    {
        set(_newPattern, _newTypeIndex);
    }

private:
    void set(const std::string& pattern,
             int typeIndex)
    {
        KnobChoicePtr type = _type.lock();
        if (type && (typeIndex >= 0)) {
            ignore_result(type->setValue(typeIndex, ViewSpec::all(), 0, eValueChangedReasonUserEdited, nullptr));
        }
        if (KnobStringPtr group = _group.lock()) {
            ignore_result(group->setValue(pattern, ViewSpec::all(), 0, eValueChangedReasonUserEdited, nullptr));
        }
    }

    KnobStringWPtr _group;
    std::string _oldPattern;
    std::string _newPattern;
    KnobChoiceWPtr _type;
    int _oldTypeIndex;
    int _newTypeIndex;
};

// The first input of node that isn't a mask and can take kind; -1 when it has none.
int
firstInputTaking(const NodePtr& node,
                 DataKindEnum kind)
{
    const EffectInstancePtr effect = node->getEffectInstance();
    const int inputs = node->getNInputs();

    for (int i = 0; i < inputs; ++i) {
        const DataKindEnum inputKind = effect->getInputDataKind(i);
        if (!effect->isInputMask(i) && ((inputKind == kind) || (inputKind == eDataKindPolymorphic))) {
            return i;
        }
    }

    return -1;
}

struct OutputLink {
    NodePtr node;
    int input;
};

// The inputs of viewed's outputs that read viewed and can read created in its place, the first
// one only unless everyOutput. A viewer declares image inputs and takes a scene or geometry
// through an adapter, so the input's declared kind can't decide this.
std::vector<OutputLink>
outputLinks(const NodePtr& viewed,
            const NodePtr& created,
            bool everyOutput)
{
    std::vector<OutputLink> links;

    for (const NodeWPtr& outputW : viewed->getOutputs()) {
        NodePtr candidate = outputW.lock();
        if (!candidate || (candidate == created) || !candidate->getParentMultiInstanceName().empty()) {
            continue;
        }
        const EffectInstancePtr effect = candidate->getEffectInstance();
        const int inputs = candidate->getNInputs();
        for (int i = 0; i < inputs; ++i) {
            if ((candidate->getInput(i) == viewed) && !effect->isInputMask(i) && !candidate->isInputDataKindUnacceptable(created, i)) {
                links.push_back(OutputLink { candidate, i });
                if (!everyOutput) {
                    return links;
                }
            }
        }
    }

    return links;
}

// Puts created below viewed, moving the outputs down when they are in the way.
void
placeBelow(const NodePtr& viewed,
           const NodePtr& created,
           const std::vector<NodePtr>& outputs)
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
    for (const NodePtr& output : outputs) {
        if (NodeGuiPtr outputGui = std::dynamic_pointer_cast<NodeGui>(output->getNodeGui())) {
            outputGui->moveBelowPositionRecursively(QRectF(position, createdSize));
        }
    }
    const QPointF inParent = createdGui->mapToParent(createdGui->mapFromScene(position));
    createdGui->setPosition(inParent.x(), inParent.y());
}

// Connects created after viewed through its first input taking kind, and gives it viewed's first
// output that can read it, or with everyOutput every such output, as an undoable edit. The input
// is connected first so no output is ever left without data, and so that a pass-through created
// resolves to a kind before the outputs are judged against it.
void
insertAfterViewed(const NodePtr& viewed,
                  const NodePtr& created,
                  DataKindEnum kind,
                  bool everyOutput)
{
    const int input = firstInputTaking(created, kind);

    if (input == -1) {
        return;
    }
    const NodePtr previousInput = created->getInput(input);
    if (!created->replaceInput(viewed, input)) {
        return;
    }
    std::vector<ReconnectInputsCommand::InputChange> changes;
    changes.push_back(ReconnectInputsCommand::InputChange { created, input, previousInput, viewed });

    std::vector<NodePtr> moved;
    if (!created->isOutputNode()) {
        for (const OutputLink& link : outputLinks(viewed, created, everyOutput)) {
            if (link.node->replaceInput(created, link.input)) {
                changes.push_back(ReconnectInputsCommand::InputChange { link.node, link.input, viewed, created });
                if (std::find(moved.begin(), moved.end(), link.node) == moved.end()) {
                    moved.push_back(link.node);
                }
            }
        }
    }
    placeBelow(viewed, created, moved);

    NodeCollectionPtr group = viewed->getGroup();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;
    if (graph) {
        UndoScope scope;
        scope.touchNode(created);
        scope.touchNode(viewed);
        for (const NodePtr& output : moved) {
            scope.touchNode(output);
        }
        // The connections are made, so the command's first redo does nothing.
        graph->pushUndoCommand(new ReconnectInputsCommand(graph, changes), scope);
    }
}

GeoGroupClassFlags
groupClassOf(GeoComponentEnum type)
{
    switch (type) {
    case eGeoComponentPoints:
        return eGeoGroupClassPoints;
    case eGeoComponentEdges:
        return eGeoGroupClassEdges;
    case eGeoComponentPrimitives:
        return eGeoGroupClassPrimitives;
    case eGeoComponentObjects:
        break;
    }

    return eGeoGroupClassNone;
}

// The class a selection of type is carried to for a group selecting classes: its own when
// allowed, else the nearest one it converts to. Objects when none is allowed.
GeoComponentEnum
carriedComponentType(GeoComponentEnum type,
                     unsigned classes)
{
    if (classes & groupClassOf(type)) {
        return type;
    }
    GeoComponentEnum order[2] = { eGeoComponentObjects, eGeoComponentObjects };
    if (type == eGeoComponentPoints) {
        order[0] = eGeoComponentPrimitives;
        order[1] = eGeoComponentEdges;
    } else if (type == eGeoComponentPrimitives) {
        order[0] = eGeoComponentPoints;
        order[1] = eGeoComponentEdges;
    } else if (type == eGeoComponentEdges) {
        order[0] = eGeoComponentPoints;
        order[1] = eGeoComponentPrimitives;
    }
    for (GeoComponentEnum candidate : order) {
        if (classes & groupClassOf(candidate)) {
            return candidate;
        }
    }

    return eGeoComponentObjects;
}

const char*
groupClassId(GeoComponentEnum type)
{
    switch (type) {
    case eGeoComponentPoints:
        return "points";
    case eGeoComponentEdges:
        return "edges";
    case eGeoComponentPrimitives:
        return "primitives";
    case eGeoComponentObjects:
        break;
    }

    return "";
}

// Writes selection, carried to a class node's group can select, into that group, and the class
// into its group type choice, as an undoable edit. A node with no declared group, an empty
// selection or no geometry to carry it over leave the group as it is.
void
setGroupFromComponents(Gui* gui,
                       const NodePtr& node,
                       const GeoDetailConstPtr& detail,
                       const GeoComponentSelection& selection)
{
    UndoHistory* history = gui ? gui->getUndoHistory() : nullptr;
    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(node->getEffectInstance().get());
    const NodeSpec* spec = native ? native->nodeSpec() : nullptr;

    if (!history || !spec || spec->geoGroupKnob.empty() || !detail || selection.empty()) {
        return;
    }
    KnobStringPtr group = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(std::string(spec->geoGroupKnob)));
    const GeoComponentEnum type = carriedComponentType(selection.type, spec->geoGroupClasses);
    if (!group || (type == eGeoComponentObjects)) {
        return;
    }
    const GeoComponentSelection carried = SceneViewport::convertedComponentSelection(*detail, selection, type);
    // A selection the class keeps nothing of selects nothing, where an empty pattern would select everything.
    const std::string pattern = carried.empty() ? std::string("^*") : SceneViewport::componentSelectionPattern(*detail, carried);

    KnobChoicePtr typeKnob;
    int typeIndex = -1;
    if (!spec->geoGroupTypeKnob.empty()) {
        typeKnob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(std::string(spec->geoGroupTypeKnob)));
        const std::vector<ChoiceOption> options = typeKnob ? typeKnob->getEntries_mt_safe() : std::vector<ChoiceOption>();
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (options[i].id == groupClassId(type)) {
                typeIndex = int(i);
                break;
            }
        }
        if (typeIndex == -1) {
            typeKnob.reset();
        }
    }
    const QString label = tr("Set %1 of %2").arg(QString::fromStdString(group->getLabel()), QString::fromStdString(node->getLabel()));
    history->push(new SetGeoGroupCommand(group, pattern, typeKnob, typeIndex, label), UndoScope(UndoScope::holderForNode(node)));
}

struct ReselectTarget {
    NodePtr node;
    const NodeSpec* spec = nullptr;
    KnobStringPtr group;
    KnobChoicePtr type;
    NodePtr input;
};

// The node of the graph the viewer is in that Reselect applies to, or why it doesn't apply.
bool
findReselectTarget(const NodePtr& viewerNode,
                   ReselectTarget* target,
                   QString* problem)
{
    NodeCollectionPtr collection = viewerNode ? viewerNode->getGroup() : NodeCollectionPtr();
    NodeGraph* graph = collection ? dynamic_cast<NodeGraph*>(collection->getNodeGraph()) : nullptr;
    if (!graph) {
        *problem = tr("Reselect: there is no node graph to take the node from.");

        return false;
    }
    const std::list<NodeGuiPtr>& selected = graph->getSelectedNodes();
    if (selected.empty()) {
        *problem = tr("Reselect: select a node with a group first.");

        return false;
    }
    if (selected.size() > 1) {
        *problem = tr("Reselect: select only one node.");

        return false;
    }
    target->node = selected.front()->getNode();
    const QString name = QString::fromStdString(target->node->getLabel());
    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(target->node->getEffectInstance().get());
    target->spec = native ? native->nodeSpec() : nullptr;
    if (!target->spec || target->spec->geoGroupKnob.empty()) {
        *problem = tr("Reselect: %1 has no group to reselect.").arg(name);

        return false;
    }
    target->group = std::dynamic_pointer_cast<KnobString>(target->node->getKnobByName(std::string(target->spec->geoGroupKnob)));
    if (!target->group) {
        *problem = tr("Reselect: %1 has no group to reselect.").arg(name);

        return false;
    }
    if (!target->spec->geoGroupTypeKnob.empty()) {
        target->type = std::dynamic_pointer_cast<KnobChoice>(target->node->getKnobByName(std::string(target->spec->geoGroupTypeKnob)));
    }
    const int input = firstInputTaking(target->node, eDataKindGeometry);
    target->input = (input >= 0) ? target->node->getInput(input) : NodePtr();
    if (!target->input) {
        *problem = tr("Reselect: %1 has no geometry connected to its input.").arg(name);

        return false;
    }

    return true;
}

// The class the node's group type and pattern select in detail.
bool
resolveReselectType(const ReselectTarget& target,
                    const GeoDetail& detail,
                    GeoComponentEnum* type,
                    std::string* error)
{
    GeoGroupTypeEnum groupType = eGeoGroupTypeGuess;
    if (target.type) {
        const std::vector<ChoiceOption> options = target.type->getEntries_mt_safe();
        const int index = target.type->getValue(0);
        if ((index >= 0) && (std::size_t(index) < options.size())) {
            const std::string& id = options[std::size_t(index)].id;
            if (id == "points") {
                groupType = eGeoGroupTypePoints;
            } else if (id == "primitives") {
                groupType = eGeoGroupTypePrimitives;
            } else if (id == "edges") {
                groupType = eGeoGroupTypeEdges;
            }
        }
    }
    if (groupType == eGeoGroupTypeGuess) {
        std::vector<GeoGroupTypeEnum> allowed;
        const unsigned classes = target.spec->geoGroupClasses;
        if (classes & eGeoGroupClassPoints) {
            allowed.push_back(eGeoGroupTypePoints);
        }
        if (classes & eGeoGroupClassEdges) {
            allowed.push_back(eGeoGroupTypeEdges);
        }
        if (classes & eGeoGroupClassPrimitives) {
            allowed.push_back(eGeoGroupTypePrimitives);
        }
        if (allowed.empty() || !geoGuessGroupType(detail, target.group->getValue(0), allowed, eGeoGroupTypePrimitives, &groupType, error)) {
            if (allowed.empty() && error) {
                *error = tr("the node selects no point, edge or primitive group").toStdString();
            }

            return false;
        }
    }
    switch (groupType) {
    case eGeoGroupTypePoints:
        *type = eGeoComponentPoints;
        break;
    case eGeoGroupTypeEdges:
        *type = eGeoComponentEdges;
        break;
    default:
        *type = eGeoComponentPrimitives;
        break;
    }

    return true;
}

struct ReselectEvaluation {
    GeoDetailConstPtr detail;
    std::string error;
};

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
            insertAfterViewed(viewedNode, created, eDataKindScene, false);
        }
        if (KnobStringPtr knob = primKnob(created)) {
            pushSet(gui, created, knob, paths);
        }
    };
    TabMenuBridge::launch(graph, context);
}

void
createFromComponents(Gui* gui,
                     const NodePtr& viewed,
                     const GeoDetailConstPtr& detail,
                     const GeoComponentSelection& selection)
{
    NodeCollectionPtr group = viewed ? viewed->getGroup() : NodeCollectionPtr();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;

    if (!gui || !graph) {
        showHint(gui, tr("Create Node from Selection: the viewer shows no geometry."));

        return;
    }
    TabMenuBridge::LaunchContext context;
    context.inputKind = eDataKindGeometry;
    context.decorateArgs = [](CreateNodeArgs& args) {
        // The graph's auto-connect would only connect the new node to the selected nodes.
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    };
    context.created = [gui, detail, selection, viewedWeak = NodeWPtr(viewed)](const NodePtr& created) {
        NodePtr viewedNode = viewedWeak.lock();
        if (viewedNode && viewedNode->isActivated()) {
            insertAfterViewed(viewedNode, created, eDataKindGeometry, true);
        }
        setGroupFromComponents(gui, created, detail, selection);
    };
    TabMenuBridge::launch(graph, context);
}

struct ReselectSession::Implementation {
    Gui* gui;
    ViewerTab* tab;
    QPointer<SceneViewport> view;
    // Evaluating the input's geometry, or showing it, is under way.
    bool pending = false;
    NodeWPtr node;
    GeoDetailConstPtr detail;
    // Puts back what the viewer showed; empty when it was not changed.
    std::function<void()> restoreViewer;

    NodePtr viewerNode() const
    {
        ViewerInstance* viewer = tab ? tab->getInternalNode() : nullptr;

        return viewer ? viewer->getNode() : NodePtr();
    }

    // Makes the viewer's input A show input, when it doesn't; false if it can't.
    bool showInput(const NodePtr& input)
    {
        NodePtr viewerNode = this->viewerNode();
        ViewerInstance* viewer = tab->getInternalNode();
        InspectorNodePtr inspector = std::dynamic_pointer_cast<InspectorNode>(viewerNode);
        int a = -1, b = -1;
        if (!inspector || !viewer) {
            return false;
        }
        viewer->getActiveInputs(a, b);
        if (a < 0) {
            return false;
        }
        if (viewerNode->getInput(a) == input) {
            return true;
        }
        for (int i = 0; i < viewerNode->getNInputs(); ++i) {
            if (viewerNode->getInput(i) == input) {
                inspector->setActiveInputAndRefresh(i, true);
                restoreViewer = [inspector, a]() { inspector->setActiveInputAndRefresh(a, true); };

                return true;
            }
        }
        const NodePtr previous = viewerNode->getInput(a);
        if (!inspector->replaceInput(input, a)) {
            return false;
        }
        restoreViewer = [inspector, previous, a]() {
            if (previous) {
                ignore_result(inspector->replaceInput(previous, a));
            } else {
                ignore_result(inspector->disconnectInput(a));
            }
        };

        return true;
    }

    void finish()
    {
        pending = false;
        node.reset();
        detail.reset();
        if (view) {
            view->endReselect();
        }
        if (restoreViewer) {
            restoreViewer();
            restoreViewer = nullptr;
        }
    }

    void load(const ReselectTarget& target,
              const GeoDetailConstPtr& input)
    {
        pending = false;
        GeoComponentEnum type = eGeoComponentPrimitives;
        std::string error;
        GeoComponentSelection selection;
        if (!input || !resolveReselectType(target, *input, &type, &error)
            || !SceneViewport::componentSelectionFromPattern(*input, type, target.group->getValue(0), &selection, &error)) {
            showHint(gui, tr("Reselect: the group of %1 can't be read: %2").arg(QString::fromStdString(target.node->getLabel()), QString::fromStdString(error)));
            if (restoreViewer) {
                restoreViewer();
                restoreViewer = nullptr;
            }

            return;
        }
        node = target.node;
        detail = input;
        view->beginReselect(selection);
        showHint(gui, tr("Reselecting %1: Enter applies the selection, Escape cancels.").arg(QString::fromStdString(target.node->getLabel())));
    }
};

ReselectSession::ReselectSession(Gui* gui,
                                 ViewerTab* tab,
                                 SceneViewport* view)
    : _imp(new Implementation)
{
    _imp->gui = gui;
    _imp->tab = tab;
    _imp->view = view;
}

ReselectSession::~ReselectSession() = default;

void
ReselectSession::begin()
{
    if (!_imp->view || _imp->pending || _imp->view->isReselecting()) {
        return;
    }
    ReselectTarget target;
    QString problem;
    if (!findReselectTarget(_imp->viewerNode(), &target, &problem)) {
        showHint(_imp->gui, problem);

        return;
    }
    const bool shown = _imp->tab->getSceneViewedNode() == target.input;
    if (!_imp->showInput(target.input)) {
        showHint(_imp->gui, tr("Reselect: the viewer can't show the input of %1.").arg(QString::fromStdString(target.node->getLabel())));

        return;
    }
    if (shown) {
        if (GeoDetailConstPtr detail = _imp->view->shownDetail()) {
            _imp->load(target, detail);

            return;
        }
    }
    _imp->pending = true;
    const EffectInstancePtr effect = target.input->getEffectInstance();
    const double frame = _imp->tab->getTimeLine()->currentFrame();
    auto* watcher = new QFutureWatcher<ReselectEvaluation>(_imp->view);
    QObject::connect(watcher, &QFutureWatcher<ReselectEvaluation>::finished, _imp->view, [this, watcher, target]() {
        const ReselectEvaluation result = watcher->result();
        watcher->deleteLater();
        if (!result.detail) {
            _imp->pending = false;
            showHint(_imp->gui, tr("Reselect: the geometry of the input can't be computed: %1").arg(QString::fromStdString(result.error)));
            if (_imp->restoreViewer) {
                _imp->restoreViewer();
                _imp->restoreViewer = nullptr;
            }

            return;
        }
        _imp->load(target, result.detail);
    });
    watcher->setFuture(QtConcurrent::run([effect, frame]() {
        ReselectEvaluation result;
        if (effect->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(frame, ViewIdx(0)), &result.detail, &result.error) != EffectInstance::eRenderRoIRetCodeOk) {
            result.detail.reset();
        }

        return result;
    }));
}

void
ReselectSession::commit(const GeoComponentSelection& selection)
{
    NodePtr node = _imp->node.lock();
    if (!node || !node->isActivated()) {
        showHint(_imp->gui, tr("Reselect: the node is gone."));
        _imp->finish();

        return;
    }
    if (selection.empty()) {
        showHint(_imp->gui, tr("Reselect: nothing is selected. Select something, or Escape to leave the group as it is."));

        return;
    }
    setGroupFromComponents(_imp->gui, node, _imp->detail, selection);
    _imp->finish();
}

void
ReselectSession::cancel()
{
    _imp->finish();
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
