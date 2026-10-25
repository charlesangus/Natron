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

#include "PythonUndoCommands.h"

#include <algorithm>
#include <list>
#include <map>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TrackMarker.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoKnobRef.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

UndoRecorderPtr
recorderFor(const AppInstancePtr& app)
{
    return app ? app->getRecordingUndoRecorder() : UndoRecorderPtr();
}

UndoRecorderPtr
recorderForNode(const NodePtr& node)
{
    return node ? recorderFor(node->getApp()) : UndoRecorderPtr();
}

UndoTouched
touchedNode(const NodePtr& node)
{
    UndoTouched touched;

    touched.nodes.push_back(node);

    return touched;
}

void
collectViewers(const NodePtr& node,
               std::list<ViewerInstance*>* viewers)
{
    std::list<ViewerInstance*> connected;

    node->hasViewersConnected(&connected);
    for (std::list<ViewerInstance*>::iterator it = connected.begin(); it != connected.end(); ++it) {
        if (std::find(viewers->begin(), viewers->end(), *it) == viewers->end()) {
            viewers->push_back(*it);
        }
    }
}

// Graph edits made through undo pass no render request of their own, like the node graph's.
void
refreshAfterGraphEdit(const NodePtr& node,
                      std::list<ViewerInstance*> viewers)
{
    collectViewers(node, &viewers);
    for (std::list<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }
    AppInstancePtr app = node->getApp();
    if (app) {
        app->triggerAutoSave();
        app->redrawAllViewers();
    }
}

//================================================================

// A dimension in a link group that is not permanent, and a linked member of the group that it can
// rejoin the group through.
struct KnobGroupLink {
    int dimension;
    bool linked;
    bool nodeLink;
    UndoKnobRef partner;
    int partnerDimension;
    bool partnerNodeLink;
};

struct KnobState {
    // Holds the values, animation and defaults; expressions and links are kept apart since a
    // detached knob cannot hold them.
    KnobIPtr values;
    std::vector<std::string> expressions;
    std::vector<bool> expressionsHaveRet;
    std::vector<std::pair<int, UndoKnobRef>> masters;
    std::vector<KnobGroupLink> groupLinks;
    UndoKnobRef aliasMaster;
    bool hasAliasMaster;

    KnobState()
        : values()
        , expressions()
        , expressionsHaveRet()
        , masters()
        , groupLinks()
        , aliasMaster()
        , hasAliasMaster(false)
    {
    }
};

void
captureGroupLinks(const KnobIPtr& knob,
                  std::vector<KnobGroupLink>* links)
{
    for (int i = 0; i < knob->getDimension(); ++i) {
        KnobLinkGroupPtr group = knob->getLinkGroup(i);
        if (!group || group->isPermanent()) {
            continue;
        }
        std::vector<KnobLinkGroup::Member> members = group->getMembers();
        for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
            KnobIPtr partner = it->knob.lock();
            if (!partner || !it->linked || ((partner == knob) && (it->dimension == i))) {
                continue;
            }
            KnobGroupLink link;
            link.dimension = i;
            link.linked = group->isMemberLinked(knob.get(), i);
            link.nodeLink = group->isMemberNodeLink(knob.get(), i);
            link.partner = UndoKnobRef(partner);
            link.partnerDimension = it->dimension;
            link.partnerNodeLink = it->nodeLink;
            links->push_back(link);
            break;
        }
    }
}

// Adds the nodes of the knobs in a link group with knob to nodes.
void
collectGroupLinkNodes(const KnobIPtr& knob,
                      NodesList* nodes)
{
    for (int i = 0; i < knob->getDimension(); ++i) {
        KnobLinkGroupPtr group = knob->getLinkGroup(i);
        if (!group) {
            continue;
        }
        std::vector<KnobLinkGroup::Member> members = group->getMembers();
        for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
            KnobIPtr member = it->knob.lock();
            EffectInstance* effect = member ? dynamic_cast<EffectInstance*>(member->getHolder()) : 0;
            NodePtr node = effect ? effect->getNode() : NodePtr();
            if (node && (std::find(nodes->begin(), nodes->end(), node) == nodes->end())) {
                nodes->push_back(node);
            }
        }
    }
}

// Emits linkChanged() for every live member of the group.
void
notifyLinkGroupMembers(const KnobLinkGroupPtr& group)
{
    std::vector<KnobLinkGroup::Member> members = group->getMembers();
    for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
        KnobIPtr member = it->knob.lock();
        KnobSignalSlotHandlerPtr handler = member ? member->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (handler) {
            handler->s_linkChanged();
        }
    }
}

KnobState
captureKnobState(const KnobIPtr& knob)
{
    KnobState state;
    Serialization::KnobSerialization serialization;

    knob->toSerialization(&serialization);
    state.values = createDetachedKnobFromSerialization(completeKnobSerializationForKnob(*knob, serialization));

    KnobIPtr aliasMaster = knob->getAliasMaster();
    state.hasAliasMaster = bool(aliasMaster);
    state.aliasMaster = UndoKnobRef(aliasMaster);
    for (int i = 0; i < knob->getDimension(); ++i) {
        state.expressions.push_back(knob->getExpression(i));
        state.expressionsHaveRet.push_back(knob->isExpressionUsingRetVariable(i));
        std::pair<int, KnobIPtr> master = knob->isSlave(i) ? knob->getMaster(i) : std::make_pair(-1, KnobIPtr());
        state.masters.push_back(std::make_pair(master.first, UndoKnobRef(master.second)));
    }
    captureGroupLinks(knob, &state.groupLinks);

    return state;
}

void
applyKnobExpressions(const KnobIPtr& knob,
                     const KnobState& state,
                     int dimension = -1)
{
    for (int i = 0; i < knob->getDimension(); ++i) {
        if ((dimension != -1) && (i != dimension)) {
            continue;
        }
        knob->clearExpression(i, true);
        if ((i < (int)state.expressions.size()) && !state.expressions[i].empty()) {
            try {
                knob->setExpression(i, state.expressions[i], state.expressionsHaveRet[i], false);
            } catch (...) {
                // The expression was valid when recorded; what it names may since be gone.
            }
        }
    }
}

void
applyKnobState(const KnobIPtr& knob,
               const KnobState& state)
{
    KnobIPtr aliasMaster = state.hasAliasMaster ? state.aliasMaster.lock() : KnobIPtr();
    KnobIPtr currentAliasMaster = knob->getAliasMaster();
    // An alias follows its master entirely; re-making an unchanged one would only churn its links.
    const bool keepAlias = currentAliasMaster && (currentAliasMaster == aliasMaster);

    // The nodes the knob shares a group with now, whose link lines change if it leaves.
    NodesList previousLinkNodes;
    collectGroupLinkNodes(knob, &previousLinkNodes);
    bool linksChanged = !previousLinkNodes.empty();

    knob->beginChanges();
    if (currentAliasMaster && !keepAlias) {
        knob->setKnobAsAliasOfThis(currentAliasMaster, false);
    }
    if (!keepAlias) {
        for (int i = 0; i < knob->getDimension(); ++i) {
            if (knob->isSlave(i)) {
                knob->unSlave(i, false);
            }
            if (knob->getLinkGroup(i) && !knob->isLinkPermanent(i)) {
                knob->leaveLinkGroups(i);
            }
        }
    }
    // An alias kept as it was is linked: its recorded state goes back to its master through the group.
    if (state.values) {
        knob->clone(state.values);
    }
    applyKnobExpressions(knob, state);
    if (!keepAlias) {
        if (aliasMaster) {
            knob->setKnobAsAliasOfThis(aliasMaster, true);
        } else {
            for (int i = 0; i < knob->getDimension() && i < (int)state.masters.size(); ++i) {
                KnobIPtr master = state.masters[i].second.lock();
                if (master) {
                    knob->slaveTo(i, master, state.masters[i].first);
                }
            }
        }
        // Each dimension rejoins a group with the member it shared it with. Rejoining adopts the group's
        // current state, so the recorded state is put back afterwards: through the group to every member
        // for a dimension that was linked, as the knob's own for one that had unlinked.
        for (std::vector<KnobGroupLink>::const_iterator it = state.groupLinks.begin(); it != state.groupLinks.end(); ++it) {
            KnobIPtr partner = it->partner.lock();
            if (!partner || !knob->linkTo(it->dimension, partner, it->partnerDimension)) {
                continue;
            }
            linksChanged = true;
            KnobLinkGroupPtr group = knob->getLinkGroup(it->dimension);
            if (!group) {
                continue;
            }
            group->setMemberNodeLink(knob.get(), it->dimension, it->nodeLink);
            group->setMemberNodeLink(partner.get(), it->partnerDimension, it->partnerNodeLink);
            if (!it->linked) {
                group->setMemberLinked(knob.get(), it->dimension, false);
                notifyLinkGroupMembers(group);
            }
            if (state.values) {
                knob->clone(state.values.get(), it->dimension, std::min(it->dimension, state.values->getDimension() - 1));
            }
            applyKnobExpressions(knob, state, it->dimension);
        }
    }
    knob->endChanges();
    if (linksChanged) {
        EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
        NodePtr node = effect ? effect->getNode() : NodePtr();
        if (node) {
            node->emitLinksChanged(previousLinkNodes);
        }
    }
    knob->evaluateValueChange(-1, knob->getCurrentTime(), ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
}

/*
 * Restores a knob to the state it had before a step's first Python edit of it. The state to redo
 * is taken when undoing: the history only undoes an entry once every later one is undone, so the
 * knob then holds exactly what the step left it with.
 */
class KnobStateCommand
    : public UndoCommand {
public:
    explicit KnobStateCommand(const KnobIPtr& knob)
        : UndoCommand("Edit " + knob->getLabel())
        , _knob(knob)
        , _before(captureKnobState(knob))
        , _after()
        , _hasAfter(false)
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        KnobIPtr knob = _knob.lock();
        if (!knob) {
            return;
        }
        _after = captureKnobState(knob);
        _hasAfter = true;
        applyKnobState(knob, _before);
    }

    virtual void redo() OVERRIDE FINAL
    {
        KnobIPtr knob = _knob.lock();
        if (!knob || !_hasAfter) {
            return;
        }
        applyKnobState(knob, _after);
    }

    // Successive records of one knob keep the oldest state: it is what undoing them all returns to.
    virtual bool mergeWith(const UndoCommandPtr& other) OVERRIDE FINAL
    {
        const KnobStateCommand* isKnobState = dynamic_cast<const KnobStateCommand*>(other.get());
        if (!isKnobState) {
            return false;
        }
        KnobIPtr knob = _knob.lock();

        return knob && (isKnobState->_knob.lock() == knob);
    }

private:
    UndoKnobRef _knob;
    KnobState _before;
    KnobState _after;
    bool _hasAfter;
};

//================================================================

class NodeCreatedCommand
    : public UndoCommand {
public:
    explicit NodeCreatedCommand(const NodePtr& node)
        : UndoCommand("Create " + node->getLabel_mt_safe())
        , _node(node)
        , _undone(false)
    {
    }

    virtual ~NodeCreatedCommand()
    {
        NodePtr node = _node.lock();
        if (_undone && node) {
            node->destroyNode(false, false);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (!node) {
            return;
        }
        std::list<ViewerInstance*> viewers;
        collectViewers(node, &viewers);
        node->deactivate(NodesList(), true, true, true, false);
        _undone = true;
        refreshAfterGraphEdit(node, viewers);
    }

    virtual void redo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (!node) {
            return;
        }
        node->activate(NodesList(), true, false);
        _undone = false;
        refreshAfterGraphEdit(node, std::list<ViewerInstance*>());
    }

private:
    NodeWPtr _node;
    bool _undone;
};

//================================================================

class NodeRemovedCommand
    : public UndoCommand {
public:
    NodeRemovedCommand(const NodePtr& node,
                       bool autoReconnect)
        : UndoCommand("Remove " + node->getLabel_mt_safe())
        , _node(node)
        , _outputs()
        , _autoReconnect(autoReconnect)
        , _removed(false)
    {
        const NodesWList& outputs = node->getGuiOutputs();
        for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
            if (it->lock()) {
                _outputs.push_back(*it);
            }
        }
    }

    // The node stays in its group while removed, for undo; once undo can no longer bring it back
    // it is destroyed as deleting it without undo would have.
    virtual ~NodeRemovedCommand()
    {
        NodePtr node = _node.lock();
        if (_removed && node) {
            node->destroyNode(false, false);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (!node) {
            return;
        }
        node->activate(outputs(), false, false);
        _removed = false;
        refreshAfterGraphEdit(node, std::list<ViewerInstance*>());
    }

    virtual void redo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (!node) {
            return;
        }
        std::list<ViewerInstance*> viewers;
        collectViewers(node, &viewers);
        node->deactivate(outputs(), false, _autoReconnect, true, false);
        _removed = true;
        refreshAfterGraphEdit(node, viewers);
    }

private:
    NodesList outputs() const
    {
        NodesList ret;
        for (NodesWList::const_iterator it = _outputs.begin(); it != _outputs.end(); ++it) {
            NodePtr output = it->lock();
            if (output) {
                ret.push_back(output);
            }
        }

        return ret;
    }

    NodeWPtr _node;
    NodesWList _outputs;
    bool _autoReconnect;
    bool _removed;
};

//================================================================

class InputCommand
    : public UndoCommand {
public:
    InputCommand(const NodePtr& node,
                 int inputNb,
                 const NodePtr& before,
                 const NodePtr& after)
        : UndoCommand("Connect " + node->getLabel_mt_safe())
        , _node(node)
        , _inputNb(inputNb)
        , _before(before)
        , _after(after)
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        setInput(_before);
    }

    virtual void redo() OVERRIDE FINAL
    {
        setInput(_after);
    }

private:
    void setInput(const NodeWPtr& input)
    {
        NodePtr node = _node.lock();
        if (!node) {
            return;
        }
        NodePtr inputNode = input.lock();
        if (inputNode) {
            node->replaceInput(inputNode, _inputNb);
        } else {
            node->disconnectInput(_inputNb);
        }
        refreshAfterGraphEdit(node, std::list<ViewerInstance*>());
    }

    NodeWPtr _node;
    int _inputNb;
    NodeWPtr _before;
    NodeWPtr _after;
};

//================================================================

class NodeNameCommand
    : public UndoCommand {
public:
    NodeNameCommand(const NodePtr& node,
                    const std::string& scriptNameBefore,
                    const std::string& labelBefore)
        : UndoCommand("Rename " + node->getLabel_mt_safe())
        , _node(node)
        , _scriptNameBefore(scriptNameBefore)
        , _labelBefore(labelBefore)
        , _scriptNameAfter(node->getScriptName_mt_safe())
        , _labelAfter(node->getLabel_mt_safe())
    {
    }

    bool changes() const
    {
        return (_scriptNameBefore != _scriptNameAfter) || (_labelBefore != _labelAfter);
    }

    virtual void undo() OVERRIDE FINAL
    {
        apply(_scriptNameBefore, _labelBefore);
    }

    virtual void redo() OVERRIDE FINAL
    {
        apply(_scriptNameAfter, _labelAfter);
    }

private:
    void apply(const std::string& scriptName,
               const std::string& label)
    {
        NodePtr node = _node.lock();
        if (!node) {
            return;
        }
        if (node->getScriptName_mt_safe() != scriptName) {
            try {
                node->setScriptName(scriptName);
            } catch (...) {
                // Another node took the name since; the node keeps its current one.
            }
        }
        if (node->getLabel_mt_safe() != label) {
            node->setLabel(label);
        }
    }

    NodeWPtr _node;
    std::string _scriptNameBefore;
    std::string _labelBefore;
    std::string _scriptNameAfter;
    std::string _labelAfter;
};

} // anon namespace

//================================================================

struct PythonNodeLayout {
    double x, y;
    bool hasSize;
    double w, h;
    bool hasColor;
    double r, g, b;

    explicit PythonNodeLayout(const Node& node)
        : x(0)
        , y(0)
        , hasSize(node.hasSize())
        , w(0)
        , h(0)
        , hasColor(node.hasUserColor())
        , r(0)
        , g(0)
        , b(0)
    {
        node.getPosition(&x, &y);
        node.getSize(&w, &h);
        node.getUserColor(&r, &g, &b);
    }

    bool operator==(const PythonNodeLayout& other) const
    {
        return (x == other.x) && (y == other.y) && (hasSize == other.hasSize) && (w == other.w) && (h == other.h) && (hasColor == other.hasColor) && (!hasColor || ((r == other.r) && (g == other.g) && (b == other.b)));
    }

    void apply(Node& node) const
    {
        node.setPosition(x, y);
        if (hasSize) {
            node.setSize(w, h);
        }
        if (hasColor) {
            node.setColor(r, g, b);
        } else if (node.hasUserColor()) {
            node.resetColor();
        }
    }
};

namespace {

class NodeLayoutCommand
    : public UndoCommand {
public:
    NodeLayoutCommand(const NodePtr& node,
                      const std::shared_ptr<PythonNodeLayout>& before)
        : UndoCommand("Move " + node->getLabel_mt_safe())
        , _node(node)
        , _before(before)
        , _after(std::make_shared<PythonNodeLayout>(*node))
    {
    }

    bool changes() const
    {
        return !(*_before == *_after);
    }

    virtual void undo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (node) {
            _before->apply(*node);
        }
    }

    virtual void redo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (node) {
            _after->apply(*node);
        }
    }

private:
    NodeWPtr _node;
    std::shared_ptr<PythonNodeLayout> _before;
    std::shared_ptr<PythonNodeLayout> _after;
};

} // anon namespace

//================================================================

struct PythonUserKnobs {
    Serialization::NodeSerialization serialization;

    explicit PythonUserKnobs(const Node& node)
        : serialization()
    {
        Serialization::NodeSerialization full;
        node.toSerialization(&full, false);
        // Only the user pages, so that restoring them leaves the plug-in's own knobs alone.
        serialization.userPages = full.userPages;
        serialization.pagesOrder = full.pagesOrder;
    }

    void apply(Node& node) const
    {
        std::list<KnobIWPtr> userKnobs;
        const KnobsVec& knobs = node.getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            if ((*it)->isUserKnob()) {
                userKnobs.push_back(*it);
            }
        }
        node.deleteUserKnobs(userKnobs);
        node.restoreUserKnobs(serialization);

        const std::map<std::string, std::string> noRenames;
        node.storeKnobsLinks(serialization, noRenames);
        NodesList allNodes;
        AppInstancePtr app = node.getApp();
        if (app) {
            app->getProject()->getActiveNodesExpandGroups(&allNodes);
        }
        // A link to a node that is gone stays unresolved rather than abort the restore.
        node.restoreKnobsLinks(allNodes, noRenames, false);
        KnobHelper::restoreLinkGroups(NodesList(1, node.shared_from_this()));
        node.getEffectInstance()->recreateUserKnobs(true);
        // recreateUserKnobs() only declares the knobs to Python when a settings panel exists.
        node.declarePythonFields();
    }
};

namespace {

/*
 * Rebuilds a node's user knobs from a snapshot rather than replaying each creation or removal:
 * a removed knob takes its expressions, links and Python attribute with it.
 */
class UserKnobsCommand
    : public UndoCommand {
public:
    UserKnobsCommand(const NodePtr& node,
                     const std::shared_ptr<PythonUserKnobs>& before)
        : UndoCommand("Edit parameters of " + node->getLabel_mt_safe())
        , _node(node)
        , _before(before)
        , _after(std::make_shared<PythonUserKnobs>(*node))
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (node) {
            _before->apply(*node);
        }
    }

    virtual void redo() OVERRIDE FINAL
    {
        NodePtr node = _node.lock();
        if (node) {
            _after->apply(*node);
        }
    }

private:
    NodeWPtr _node;
    std::shared_ptr<PythonUserKnobs> _before;
    std::shared_ptr<PythonUserKnobs> _after;
};

// Knobs whose holder is part of the project: a node, a node's track, or the project itself.
bool
isRecordedHolder(KnobHolder* holder)
{
    return dynamic_cast<EffectInstance*>(holder) || dynamic_cast<TrackMarker*>(holder) || dynamic_cast<Project*>(holder);
}

NodePtr
nodeOfHolder(KnobHolder* holder)
{
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);

    return effect ? effect->getNode() : NodePtr();
}

} // anon namespace

//================================================================

void
recordPythonKnobEdit(const KnobIPtr& knob)
{
    if (!knob) {
        return;
    }
    KnobHolder* holder = knob->getHolder();
    if (!holder || !isRecordedHolder(holder)) {
        return;
    }
    UndoRecorderPtr recorder = recorderFor(holder->getApp());
    if (!recorder || !recorder->claimKnob(knob)) {
        return;
    }
    UndoTouched touched;
    touched.knobs.push_back(knob);
    recorder->record(std::make_shared<KnobStateCommand>(knob), touched);
}

void
recordPythonNodeCreated(const NodePtr& node)
{
    UndoRecorderPtr recorder = recorderForNode(node);

    if (recorder) {
        recorder->record(std::make_shared<NodeCreatedCommand>(node), touchedNode(node));
    }
}

void
destroyNodeFromPython(const NodePtr& node,
                      bool autoReconnect)
{
    if (!node) {
        return;
    }
    UndoRecorderPtr recorder = recorderForNode(node);
    if (!recorder) {
        node->destroyNode(false, autoReconnect);

        return;
    }
    UndoCommandPtr command = std::make_shared<NodeRemovedCommand>(node, autoReconnect);
    command->redo();
    recorder->record(command, touchedNode(node));
}

//================================================================

PythonInputUndoGuard::PythonInputUndoGuard(const NodePtr& node,
                                           int inputNb)
    : _recorder(recorderForNode(node))
    , _node(node)
    , _inputNb(inputNb)
    , _before()
{
    if (_recorder) {
        _before = node->getGuiInput(inputNb);
    }
}

PythonInputUndoGuard::~PythonInputUndoGuard()
{
    if (!_recorder) {
        return;
    }
    NodePtr before = _before.lock();
    NodePtr after = _node->getGuiInput(_inputNb);
    if (before == after) {
        return;
    }
    UndoTouched touched = touchedNode(_node);
    _recorder->record(std::make_shared<InputCommand>(_node, _inputNb, before, after), touched);
}

PythonNodeNameUndoGuard::PythonNodeNameUndoGuard(const NodePtr& node)
    : _recorder(recorderForNode(node))
    , _node(node)
    , _scriptName()
    , _label()
{
    if (_recorder) {
        _scriptName = node->getScriptName_mt_safe();
        _label = node->getLabel_mt_safe();
    }
}

PythonNodeNameUndoGuard::~PythonNodeNameUndoGuard()
{
    if (!_recorder) {
        return;
    }
    std::shared_ptr<NodeNameCommand> command = std::make_shared<NodeNameCommand>(_node, _scriptName, _label);
    if (command->changes()) {
        _recorder->record(command, touchedNode(_node));
    }
}

PythonNodeLayoutUndoGuard::PythonNodeLayoutUndoGuard(const NodePtr& node)
    : _recorder(recorderForNode(node))
    , _node(node)
    , _before()
{
    if (_recorder) {
        _before = std::make_shared<PythonNodeLayout>(*node);
    }
}

PythonNodeLayoutUndoGuard::~PythonNodeLayoutUndoGuard()
{
    if (!_recorder) {
        return;
    }
    std::shared_ptr<NodeLayoutCommand> command = std::make_shared<NodeLayoutCommand>(_node, _before);
    if (command->changes()) {
        _recorder->record(command, touchedNode(_node));
    }
}

PythonUserKnobsUndoGuard::PythonUserKnobsUndoGuard(KnobHolder* holder)
    : _recorder()
    , _node(nodeOfHolder(holder))
    , _before()
{
    _recorder = recorderForNode(_node);
    if (_recorder) {
        _before = std::make_shared<PythonUserKnobs>(*_node);
    }
}

PythonUserKnobsUndoGuard::~PythonUserKnobsUndoGuard()
{
    if (_recorder) {
        _recorder->record(std::make_shared<UserKnobsCommand>(_node, _before), touchedNode(_node));
    }
}

NATRON_NAMESPACE_EXIT
