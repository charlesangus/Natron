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

#include "NodeConvert.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include <QtCore/QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GroupInput.h"
#include "Engine/GroupOutput.h"
#include "Engine/Knob.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/IVariantRegistry.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Project.h"

// How far above and below the nodes they wrap the loop nodes are placed.
#define kNodeConvertLoopNodeSpacing 100.

NATRON_NAMESPACE_ENTER

namespace {

bool
isPluginInstalled(const std::string& pluginId)
{
    try {
        return appPTR->getPluginBinary(QString::fromUtf8(pluginId.c_str()), -1, -1, false) != nullptr;
    } catch (const std::exception&) {
        return false;
    }
}

int
inputWithLabel(const NodePtr& node,
               const std::string& label)
{
    const int n = node->getNInputs();
    for (int i = 0; i < n; ++i) {
        if (node->getInputLabel(i) == label) {
            return i;
        }
    }

    return -1;
}

void
setGuiInput(const NodePtr& node,
            int inputNb,
            const NodePtr& input)
{
    const NodePtr current = node->getGuiInput(inputNb);
    if (current == input) {
        return;
    }
    if (!input) {
        ignore_result(node->disconnectInput(inputNb));
    } else if (current) {
        ignore_result(node->replaceInput(input, inputNb));
    } else {
        ignore_result(node->connectInput(input, inputNb));
    }
}

// An edge from an input of a node: node's input inputNb reads source.
struct Edge {
    NodeWPtr node;
    int inputNb;
    NodeWPtr source;
};

// The connections of the other nodes that read node.
std::vector<Edge>
readersOf(const NodePtr& node)
{
    std::vector<Edge> ret;
    const NodesWList& outputs = node->getGuiOutputs();
    std::set<Node*> seen;

    for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        NodePtr output = it->lock();
        if (!output || !seen.insert(output.get()).second) {
            continue;
        }
        const int n = output->getNInputs();
        for (int i = 0; i < n; ++i) {
            if (output->getGuiInput(i) == node) {
                ret.push_back(Edge { output, i, node });
            }
        }
    }

    return ret;
}

void
deactivateNode(const NodePtr& node)
{
    if (node && node->isActivated()) {
        node->deactivate(NodesList(), true, false, true, false, true);
    }
}

void
activateNode(const NodePtr& node)
{
    if (node && !node->isActivated()) {
        node->activate(NodesList(), true, true);
    }
}

void
recheckExpressionsNaming(const NodePtr& node)
{
    AppInstancePtr app = node->getApp();
    if (app) {
        std::set<std::string> names;
        names.insert(node->getScriptName_mt_safe());
        app->recheckInvalidExpressionsReferencing(names);
    }
}

NodePtr
createNodeIn(const NodeCollectionPtr& group,
             const std::string& pluginId,
             const std::string& name,
             double x,
             double y)
{
    AppInstancePtr app = group ? group->getApplication() : AppInstancePtr();
    if (!app) {
        return NodePtr();
    }
    CreateNodeArgs args(pluginId, group);
    if (!name.empty()) {
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
    }
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, x, 0);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, y, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    return app->createNode(args);
}

void
destroyIfDeactivated(const NodeWPtr& weak)
{
    NodePtr node = weak.lock();
    if (node && !node->isActivated()) {
        node->destroyNode(false, false);
    }
}

} // anonymous namespace

const IVariant*
exactIVariantOf(const NodePtr& node)
{
    if (!node) {
        return nullptr;
    }
    const IVariant* variant = IVariantRegistry::builtin().forBase(node->getPluginID());

    return (variant && isPluginInstalled(variant->iId)) ? variant : nullptr;
}

const IVariant*
baseVariantOf(const NodePtr& node)
{
    if (!node) {
        return nullptr;
    }
    const IVariant* variant = IVariantRegistry::builtin().forIVariant(node->getPluginID());

    return (variant && isPluginInstalled(variant->baseId)) ? variant : nullptr;
}

bool
hasConnectedInput(const NodePtr& node,
                  const std::string& label)
{
    if (!node) {
        return false;
    }
    const int inputNb = inputWithLabel(node, label);

    return (inputNb >= 0) && bool(node->getGuiInput(inputNb));
}

//================================================================

struct NodeConvertCommand::Implementation {
    NodeWPtr original;
    NodeWPtr converted;
    IVariant variant;
    bool toIVariant = true;
    bool created = false;
    bool done = false;
    // The knobs whose link groups the node out of the graph leaves, and the replacement's knobs
    // that joined them.
    std::list<KnobIWPtr> originalLinked;
    std::list<KnobIWPtr> convertedLinked;

    std::string targetKnobName(const std::string& name) const
    {
        return toIVariant ? variant.iKnobFor(name) : variant.baseKnobFor(name);
    }

    void copyKnobs(const NodePtr& from,
                   const NodePtr& to)
    {
        EffectInstancePtr fromEffect = from->getEffectInstance();
        EffectInstancePtr toEffect = to->getEffectInstance();
        if (!fromEffect || !toEffect) {
            return;
        }
        const KnobsVec knobs = fromEffect->getKnobs_mt_safe();
        toEffect->beginChanges();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            const KnobIPtr& src = *it;
            if (!src || !src->getIsPersistent() || src->isUserKnob()) {
                continue;
            }
            KnobIPtr dst = toEffect->getKnobByName(targetKnobName(src->getName()));
            if (!dst || (dst->typeName() != src->typeName())) {
                continue;
            }
            const int dims = std::min(src->getDimension(), dst->getDimension());
            if (dims == src->getDimension() && dims == dst->getDimension()) {
                dst->clone(src);
            } else {
                for (int d = 0; d < dims; ++d) {
                    dst->clone(src, d, d);
                }
            }

            KnobHelper* srcHelper = dynamic_cast<KnobHelper*>(src.get());
            KnobHelper* dstHelper = dynamic_cast<KnobHelper*>(dst.get());
            if (srcHelper && dstHelper && srcHelper->hasDrivers() && dstHelper->canHaveDriver()) {
                for (int d = 0; d < dims; ++d) {
                    KnobDriverPtr driver = srcHelper->getDriver(d);
                    if (driver) {
                        ignore_result(dstHelper->setDriver(d, driver));
                    }
                }
            }

            // Links to knobs of other nodes move to the replacement. The replacement joins each
            // group before the original leaves it, so that no group drops to a single member.
            bool linked = false;
            const std::vector<KnobLinkedDimension> links = src->getLinkedKnobs();
            for (std::size_t i = 0; i < links.size(); ++i) {
                const KnobLinkedDimension& link = links[i];
                if (!link.knob || (link.knob->getHolder() == fromEffect.get()) || (link.dimension >= dims) || src->isLinkPermanent(link.dimension)) {
                    continue;
                }
                if (dst->linkTo(link.dimension, link.knob, link.otherDimension)) {
                    linked = true;
                }
            }
            if (linked) {
                convertedLinked.push_back(dst);
                if (src->unlinkFromGroups()) {
                    originalLinked.push_back(src);
                }
            }
        }
        toEffect->endChanges();
    }

    static void relink(const std::list<KnobIWPtr>& knobs)
    {
        for (std::list<KnobIWPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            KnobIPtr knob = it->lock();
            if (knob && knob->hasRelinkableGroups()) {
                ignore_result(knob->relinkToGroups());
            }
        }
    }

    static void unlink(const std::list<KnobIWPtr>& knobs)
    {
        for (std::list<KnobIWPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            KnobIPtr knob = it->lock();
            if (knob) {
                ignore_result(knob->unlinkFromGroups());
            }
        }
    }

    void create(const NodePtr& node)
    {
        const std::string name = node->getScriptName_mt_safe();
        const std::string label = node->getLabel_mt_safe();
        double x = 0., y = 0.;
        node->getPosition(&x, &y);

        std::vector<std::pair<std::string, NodePtr>> inputs;
        const int n = node->getNInputs();
        for (int i = 0; i < n; ++i) {
            NodePtr input = node->getGuiInput(i);
            if (input) {
                inputs.push_back(std::make_pair(node->getInputLabel(i), input));
            }
        }
        const std::vector<Edge> readers = readersOf(node);

        // Deactivating first frees the name for the replacement and marks the expressions that
        // read the node missing, to be rechecked against the replacement.
        deactivateNode(node);

        const std::string pluginId = toIVariant ? variant.iId : variant.baseId;
        NodePtr replacement = createNodeIn(node->getGroup(), pluginId, name, x, y);
        if (!replacement) {
            activateNode(node);

            return;
        }
        if (replacement->getScriptName_mt_safe() != name) {
            replacement->setScriptName_no_error_check(name);
        }
        if (label != name) {
            replacement->setLabel(label);
        }
        converted = replacement;

        copyKnobs(node, replacement);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const int inputNb = inputWithLabel(replacement, inputs[i].first);
            if (inputNb >= 0) {
                setGuiInput(replacement, inputNb, inputs[i].second);
            }
        }
        for (std::size_t i = 0; i < readers.size(); ++i) {
            NodePtr reader = readers[i].node.lock();
            if (reader) {
                setGuiInput(reader, readers[i].inputNb, replacement);
            }
        }
        recheckExpressionsNaming(replacement);
    }
};

NodeConvertCommand::NodeConvertCommand(const NodePtr& node,
                                       const IVariant& variant,
                                       bool toIVariant)
    : UndoCommand()
    , _imp(new Implementation)
{
    _imp->original = node;
    _imp->variant = variant;
    _imp->toIVariant = toIVariant;
    setText(toIVariant ? ("Make I-Node " + node->getLabel_mt_safe()) : ("Make Base Node " + node->getLabel_mt_safe()));
}

std::shared_ptr<NodeConvertCommand>
NodeConvertCommand::toIVariant(const NodePtr& node,
                               std::string* error)
{
    const IVariant* variant = exactIVariantOf(node);
    if (!variant || !node->isActivated()) {
        if (error) {
            *error = node ? (node->getLabel_mt_safe() + " has no I-variant.") : std::string("No node to convert.");
        }

        return std::shared_ptr<NodeConvertCommand>();
    }

    return std::make_shared<NodeConvertCommand>(node, *variant, true);
}

std::shared_ptr<NodeConvertCommand>
NodeConvertCommand::toBaseNode(const NodePtr& node,
                               std::string* error)
{
    const IVariant* variant = baseVariantOf(node);
    if (!variant || !node->isActivated()) {
        if (error) {
            *error = node ? (node->getLabel_mt_safe() + " is not an I-node.") : std::string("No node to convert.");
        }

        return std::shared_ptr<NodeConvertCommand>();
    }

    return std::make_shared<NodeConvertCommand>(node, *variant, false);
}

NodeConvertCommand::~NodeConvertCommand()
{
    // Whichever node is out of the graph can no longer come back.
    destroyIfDeactivated(_imp->done ? _imp->original : _imp->converted);
}

void
NodeConvertCommand::redo()
{
    NodePtr original = _imp->original.lock();
    if (!original || _imp->done) {
        return;
    }
    if (!_imp->created) {
        _imp->created = true;
        _imp->create(original);
        _imp->done = bool(_imp->converted.lock());

        return;
    }
    NodePtr converted = _imp->converted.lock();
    if (!converted) {
        return;
    }
    Implementation::relink(_imp->convertedLinked);
    Implementation::unlink(_imp->originalLinked);
    deactivateNode(original);
    activateNode(converted);
    _imp->done = true;
}

void
NodeConvertCommand::undo()
{
    NodePtr original = _imp->original.lock();
    NodePtr converted = _imp->converted.lock();
    if (!original || !converted || !_imp->done) {
        return;
    }
    Implementation::relink(_imp->originalLinked);
    Implementation::unlink(_imp->convertedLinked);
    deactivateNode(converted);
    activateNode(original);
    _imp->done = false;
}

NodePtr
NodeConvertCommand::getOriginal() const
{
    return _imp->original.lock();
}

NodePtr
NodeConvertCommand::getConverted() const
{
    return _imp->converted.lock();
}

//================================================================

namespace {

// What wrapping a selection needs: the edges that enter it, grouped by the node outside they
// read, and its single output.
struct WrapPlan {
    NodeCollectionPtr group;
    std::vector<std::vector<Edge>> entries;
    // The input a LoopBegin with nothing upstream feeds when nothing enters the selection.
    Edge openInput { NodeWPtr(), -1, NodeWPtr() };
    NodePtr output;
    std::vector<Edge> readers;
};

bool
isWrappable(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();

    return effect && !node->isEffectViewer() && !node->isOutputNode() && !dynamic_cast<GroupOutput*>(effect.get()) && !dynamic_cast<GroupInput*>(effect.get());
}

bool
planWrap(const NodesList& nodes,
         WrapPlan* plan,
         std::string* reason)
{
    const auto refuse = [reason](const std::string& why) {
        if (reason) {
            *reason = why;
        }

        return false;
    };

    if (nodes.empty()) {
        return refuse("Select the nodes to wrap in a loop.");
    }
    std::set<Node*> selected;
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!*it || !(*it)->isActivated()) {
            return refuse("A selected node is not in the graph.");
        }
        if (!isWrappable(*it)) {
            return refuse((*it)->getLabel_mt_safe() + " cannot be inside a loop.");
        }
        if (!plan->group) {
            plan->group = (*it)->getGroup();
        } else if ((*it)->getGroup() != plan->group) {
            return refuse("The selected nodes are not all in the same group.");
        }
        selected.insert(it->get());
    }

    std::vector<NodePtr> ends;
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        bool readInside = false;
        bool readOutside = false;
        const std::vector<Edge> readers = readersOf(*it);
        for (std::size_t i = 0; i < readers.size(); ++i) {
            NodePtr reader = readers[i].node.lock();
            if (reader && selected.count(reader.get())) {
                readInside = true;
            } else if (reader) {
                readOutside = true;
            }
        }
        if (!readInside || readOutside) {
            ends.push_back(*it);
        }
    }
    if (ends.size() != 1) {
        return refuse("A loop needs a selection with a single output.");
    }
    plan->output = ends.front();
    plan->readers = readersOf(plan->output);

    std::vector<NodePtr> sources;
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        const int n = (*it)->getNInputs();
        for (int i = 0; i < n; ++i) {
            NodePtr input = (*it)->getGuiInput(i);
            if (!input || selected.count(input.get())) {
                continue;
            }
            std::vector<NodePtr>::const_iterator found = std::find(sources.begin(), sources.end(), input);
            const std::size_t index = found - sources.begin();
            if (found == sources.end()) {
                sources.push_back(input);
                plan->entries.push_back(std::vector<Edge>());
            }
            plan->entries[index].push_back(Edge { *it, i, input });
        }
    }

    if (plan->entries.empty()) {
        // The first required input left open on a node that reads nothing in the selection.
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end() && !plan->openInput.node.lock(); ++it) {
            EffectInstancePtr effect = (*it)->getEffectInstance();
            const int n = (*it)->getNInputs();
            bool readsSelection = false;
            for (int i = 0; i < n; ++i) {
                NodePtr input = (*it)->getGuiInput(i);
                if (input && selected.count(input.get())) {
                    readsSelection = true;
                }
            }
            if (readsSelection) {
                continue;
            }
            for (int i = 0; i < n; ++i) {
                if (!(*it)->getGuiInput(i) && !effect->isInputOptional(i)) {
                    plan->openInput = Edge { *it, i, NodeWPtr() };
                    break;
                }
            }
        }
        if (!plan->openInput.node.lock()) {
            return refuse("Nothing enters the selection, and it has no open input for a LoopBegin.");
        }
    }

    return true;
}

} // anonymous namespace

struct LoopWrapCommand::Implementation {
    NodesList nodes;
    WrapPlan plan;
    bool created = false;
    bool done = false;
    NodeWPtr end;
    // One per entry of the plan, in its order, or the single one feeding its open input.
    std::vector<NodeWPtr> begins;

    void connect()
    {
        NodePtr loopEnd = end.lock();
        if (!loopEnd) {
            return;
        }
        if (plan.entries.empty()) {
            NodePtr node = plan.openInput.node.lock();
            NodePtr begin = begins.empty() ? NodePtr() : begins.front().lock();
            if (node && begin) {
                setGuiInput(node, plan.openInput.inputNb, begin);
            }
        }
        for (std::size_t e = 0; e < plan.entries.size() && e < begins.size(); ++e) {
            NodePtr begin = begins[e].lock();
            if (!begin) {
                continue;
            }
            for (std::size_t i = 0; i < plan.entries[e].size(); ++i) {
                const Edge& edge = plan.entries[e][i];
                NodePtr node = edge.node.lock();
                NodePtr source = edge.source.lock();
                if (node && source) {
                    setGuiInput(begin, 0, source);
                    setGuiInput(node, edge.inputNb, begin);
                }
            }
        }
        setGuiInput(loopEnd, kLoopEndInputSource, plan.output);
        for (std::size_t i = 0; i < plan.readers.size(); ++i) {
            NodePtr reader = plan.readers[i].node.lock();
            if (reader) {
                setGuiInput(reader, plan.readers[i].inputNb, loopEnd);
            }
        }
    }

    void disconnect()
    {
        for (std::size_t i = 0; i < plan.readers.size(); ++i) {
            NodePtr reader = plan.readers[i].node.lock();
            if (reader) {
                setGuiInput(reader, plan.readers[i].inputNb, plan.output);
            }
        }
        for (std::size_t e = 0; e < plan.entries.size(); ++e) {
            for (std::size_t i = 0; i < plan.entries[e].size(); ++i) {
                const Edge& edge = plan.entries[e][i];
                NodePtr node = edge.node.lock();
                if (node) {
                    setGuiInput(node, edge.inputNb, edge.source.lock());
                }
            }
        }
        if (plan.entries.empty()) {
            NodePtr node = plan.openInput.node.lock();
            if (node) {
                setGuiInput(node, plan.openInput.inputNb, NodePtr());
            }
        }
    }

    void create()
    {
        double outX = 0., outY = 0.;
        plan.output->getPosition(&outX, &outY);
        NodePtr loopEnd = createNodeIn(plan.group, PLUGINID_NATRON_LOOPEND, std::string(), outX, outY + kNodeConvertLoopNodeSpacing);
        if (!loopEnd) {
            return;
        }
        end = loopEnd;
        EffectInstancePtr effect = loopEnd->getEffectInstance();
        KnobChoicePtr mode = std::dynamic_pointer_cast<KnobChoice>(effect->getKnobByName(kLoopEndParamMode));
        if (mode) {
            ignore_result(mode->setValueFromID(kLoopEndParamModeMapBlend, 0));
        }
        KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(effect->getKnobByName(kLoopEndParamIterations));
        if (iterations) {
            ignore_result(iterations->setValue(kNodeConvertLoopIterations));
        }

        std::vector<NodePtr> consumers;
        if (plan.entries.empty()) {
            consumers.push_back(plan.openInput.node.lock());
        }
        for (std::size_t e = 0; e < plan.entries.size(); ++e) {
            consumers.push_back(plan.entries[e].front().node.lock());
        }
        for (std::size_t i = 0; i < consumers.size(); ++i) {
            double x = 0., y = 0.;
            if (consumers[i]) {
                consumers[i]->getPosition(&x, &y);
            }
            NodePtr begin = createNodeIn(plan.group, PLUGINID_NATRON_LOOPBEGIN, std::string(), x, y - kNodeConvertLoopNodeSpacing);
            begins.push_back(begin);
        }
        connect();
    }
};

LoopWrapCommand::LoopWrapCommand(const NodesList& nodes)
    : UndoCommand("Wrap in Loop")
    , _imp(new Implementation)
{
    _imp->nodes = nodes;
}

bool
LoopWrapCommand::canWrap(const NodesList& nodes,
                         std::string* reason)
{
    WrapPlan plan;

    return planWrap(nodes, &plan, reason);
}

std::shared_ptr<LoopWrapCommand>
LoopWrapCommand::create(const NodesList& nodes,
                        std::string* error)
{
    WrapPlan plan;
    if (!planWrap(nodes, &plan, error)) {
        return std::shared_ptr<LoopWrapCommand>();
    }
    std::shared_ptr<LoopWrapCommand> command = std::make_shared<LoopWrapCommand>(nodes);
    command->_imp->plan = plan;
    if (nodes.size() == 1) {
        command->setText("Make I-Node " + nodes.front()->getLabel_mt_safe());
    }

    return command;
}

std::string
LoopWrapCommand::warningFor(const NodesList& nodes)
{
    std::string ret;

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!*it) {
            continue;
        }
        EffectInstancePtr effect = (*it)->getEffectInstance();
        const bool transforms = effect && effect->getCanTransform();
        const bool defocus = (*it)->getPluginID() == PLUGINID_NATRON_DEFOCUS;
        if (!transforms && !defocus) {
            continue;
        }
        if (!ret.empty()) {
            ret += "\n";
        }
        ret += (*it)->getLabel_mt_safe();
        ret += transforms ? " passes its transform on to the nodes below it, which a loop stops: they resample its output instead."
                          : " blends poorly between loop iterations.";
        const IVariant* variant = exactIVariantOf(*it);
        if (variant) {
            ret += " Make I-Node on it alone converts it to " + variant->iId + " instead.";
        }
    }

    return ret;
}

LoopWrapCommand::~LoopWrapCommand()
{
    if (!_imp->done) {
        destroyIfDeactivated(_imp->end);
        for (std::size_t i = 0; i < _imp->begins.size(); ++i) {
            destroyIfDeactivated(_imp->begins[i]);
        }
    }
}

void
LoopWrapCommand::redo()
{
    if (_imp->done) {
        return;
    }
    if (!_imp->created) {
        _imp->created = true;
        _imp->create();
        _imp->done = bool(_imp->end.lock());

        return;
    }
    activateNode(_imp->end.lock());
    for (std::size_t i = 0; i < _imp->begins.size(); ++i) {
        activateNode(_imp->begins[i].lock());
    }
    _imp->connect();
    _imp->done = true;
}

void
LoopWrapCommand::undo()
{
    if (!_imp->done) {
        return;
    }
    _imp->disconnect();
    deactivateNode(_imp->end.lock());
    for (std::size_t i = 0; i < _imp->begins.size(); ++i) {
        deactivateNode(_imp->begins[i].lock());
    }
    _imp->done = false;
}

NodePtr
LoopWrapCommand::getLoopEnd() const
{
    return _imp->end.lock();
}

NodesList
LoopWrapCommand::getLoopBegins() const
{
    NodesList ret;

    for (std::size_t i = 0; i < _imp->begins.size(); ++i) {
        NodePtr begin = _imp->begins[i].lock();
        if (begin) {
            ret.push_back(begin);
        }
    }

    return ret;
}

NATRON_NAMESPACE_EXIT
