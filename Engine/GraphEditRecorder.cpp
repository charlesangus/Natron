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

#include "GraphEditRecorder.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <QtCore/QMetaObject>
#include <QtCore/QObject>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

NATRON_NAMESPACE_ENTER

namespace {

thread_local int suppressDepth = 0;

enum EntryKindEnum {
    eEntryKindOp,
    eEntryKindNodeKnob,
    eEntryKindProjectKnob,
    eEntryKindNodeUi,
    eEntryKindUserKnobs,
    eEntryKindRoto,
    eEntryKindTracker,
    eEntryKindProjectData
};

struct Entry {
    EntryKindEnum kind = eEntryKindOp;
    std::optional<Serialization::GraphOp> op;
    // The node an operation is about, compared by address only: creation scopes drop what was
    // recorded about the nodes they create.
    const Node* subject = nullptr;
    NodeWPtr node;
    KnobIWPtr knob;
};

// Every entry has the key (sequence number, 0). A creation scope reserves a sequence number
// when it begins, and puts its operations at (that number, 1...) when it ends, before what was
// recorded while it existed.
typedef std::pair<std::uint64_t, std::uint32_t> EntryKey;

struct CreationState {
    int depth = 0;
    GraphEditRecorder* recorder = nullptr;
    std::uint64_t startSeq = 0;
    NodesList noted;
};

thread_local CreationState creation;

// The project-level knobs every save sets, which a project document leaves out.
bool
isSavedEachTime(const KnobI& knob)
{
    const std::string& name = knob.getName();

    return name == "softwareVersion" || name == "lastAuthor" || name == "lastSaveDate";
}

bool
isProjectVariable(const KnobI& knob)
{
    KnobGroupPtr parent = std::dynamic_pointer_cast<KnobGroup>(knob.getParentKnob());

    return parent && parent->isRFVGroup();
}

NodePtr
enclosingGroupNode(const Node& node)
{
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node.getGroup());

    return group ? group->getNode() : NodePtr();
}

bool
isRecordableProject(const Project& project)
{
    return !project.isLoadingProject() && !project.isLoadingProjectInternal() && !project.isProjectClosing();
}

bool
isRecordableNode(const Node& node)
{
    if (!node.isPartOfProject() || node.getParentMultiInstance() || !node.isActivated() || node.getNodeId().empty()) {
        return false;
    }
    for (NodePtr group = enclosingGroupNode(node); group; group = enclosingGroupNode(*group)) {
        if (!group->isActivated()) {
            return false;
        }
    }

    return true;
}

Serialization::NodeUiColor
toUiColor(double r,
          double g,
          double b)
{
    Serialization::NodeUiColor color;
    color.r = (float)r;
    color.g = (float)g;
    color.b = (float)b;

    return color;
}

Serialization::SetNodeUiOp
nodeUiOp(const Node& node)
{
    Serialization::SetNodeUiOp op;
    op.nodeId = node.getNodeId();
    double x, y;
    node.getPosition(&x, &y);
    op.position = std::make_pair(x, y);
    if (node.hasSize()) {
        double w, h;
        node.getSize(&w, &h);
        op.size = std::make_pair(w, h);
    }
    double r, g, b;
    if (node.hasUserColor()) {
        node.getUserColor(&r, &g, &b);
        op.color = std::optional<Serialization::NodeUiColor>(toUiColor(r, g, b));
    } else {
        op.color = std::optional<Serialization::NodeUiColor>();
    }
    if (node.hasOverlayColor()) {
        node.getStoredOverlayColor(&r, &g, &b);
        op.overlayColor = std::optional<Serialization::NodeUiColor>(toUiColor(r, g, b));
    } else {
        op.overlayColor = std::optional<Serialization::NodeUiColor>();
    }

    return op;
}

Serialization::SetProjectDataOp
projectDataOp(const Project& project)
{
    Serialization::SetProjectDataOp op;
    std::list<Format> formats;
    project.getAdditionalFormats(&formats);
    op.formats.emplace();
    for (const Format& format : formats) {
        Serialization::FormatSerialization serialization;
        serialization.x1 = format.x1;
        serialization.y1 = format.y1;
        serialization.x2 = format.x2;
        serialization.y2 = format.y2;
        serialization.pixelAspectRatio = format.getPixelAspectRatio();
        serialization.name = format.getName();
        op.formats->push_back(serialization);
    }
    op.layers.emplace();
    std::shared_ptr<const std::vector<LayerRegistryEntry>> layers = project.getLayerRegistrySnapshot();
    for (const LayerRegistryEntry& layer : *layers) {
        if (layer.origin == LayerRegistryEntry::eOriginBuiltin) {
            continue;
        }
        Serialization::LayerRegistryEntrySerialization entry;
        entry.layer.layerID = layer.desc.getLayerID();
        entry.layer.layerLabel = layer.desc.getLayerLabel();
        entry.layer.channelsLabel = layer.desc.getChannelsLabel();
        entry.layer.channels = layer.desc.getChannels();
        entry.origin = (int)layer.origin;
        op.layers->push_back(entry);
    }

    return op;
}

// What the project document holds for knob: Project::toSerialization's rule.
std::optional<Serialization::KnobSerialization>
projectKnobValue(KnobI& knob)
{
    bool saved = isProjectVariable(knob);
    for (int d = 0; !saved && d < knob.getDimension(); ++d) {
        saved = (bool)knob.getLinkGroup(d);
    }
    if (!saved && !knob.hasModificationsForSerialization()) {
        return std::nullopt;
    }
    Serialization::KnobSerialization value;
    knob.toSerialization(&value);

    return value;
}

} // anon namespace

std::atomic<int> GraphEditRecorder::_subscriptions(0);

struct GraphEditRecorder::Implementation {
    Project* project;
    QObject flushContext;

    std::mutex lock;
    std::map<int, Subscriber> subscribers;
    int nextSubscription = 1;
    std::map<EntryKey, Entry> pending;
    std::map<std::pair<int, const void*>, EntryKey> coalesced;
    std::set<const Node*> deleted;
    std::uint64_t nextSeq = 1;
    bool flushScheduled = false;

    explicit Implementation(Project* project_)
        : project(project_)
    {
    }

    bool hasSubscribers()
    {
        std::lock_guard<std::mutex> l(lock);

        return !subscribers.empty();
    }

    // Called with lock held.
    void scheduleFlush(GraphEditRecorder* recorder)
    {
        if (flushScheduled) {
            return;
        }
        flushScheduled = true;
        QMetaObject::invokeMethod(&flushContext, [recorder]() { recorder->flush(); }, Qt::QueuedConnection);
    }

    void add(GraphEditRecorder* recorder,
             Entry entry)
    {
        std::lock_guard<std::mutex> l(lock);
        pending.emplace(EntryKey(nextSeq++, 0), std::move(entry));
        scheduleFlush(recorder);
    }

    // Records one entry for (kind, target) until the next delivery, and for this one target only:
    // an entry left by a target that has died at the same address is not reused.
    void addCoalesced(GraphEditRecorder* recorder,
                      EntryKindEnum kind,
                      const void* target,
                      Entry entry)
    {
        std::lock_guard<std::mutex> l(lock);
        const std::pair<int, const void*> key((int)kind, target);
        std::map<std::pair<int, const void*>, EntryKey>::iterator found = coalesced.find(key);
        if (found != coalesced.end()) {
            std::map<EntryKey, Entry>::iterator existing = pending.find(found->second);
            if (existing != pending.end()) {
                const bool sameNode = existing->second.node.lock() == entry.node.lock();
                const bool sameKnob = existing->second.knob.lock() == entry.knob.lock();
                if (sameNode && sameKnob) {
                    return;
                }
            }
        }
        entry.kind = kind;
        const EntryKey entryKey(nextSeq++, 0);
        pending.emplace(entryKey, std::move(entry));
        coalesced[key] = entryKey;
        scheduleFlush(recorder);
    }

    bool isDeleted(const Node* node)
    {
        std::lock_guard<std::mutex> l(lock);

        return deleted.count(node) > 0;
    }

    bool isLiveNode(const NodePtr& node)
    {
        return node && isRecordableNode(*node) && project->findNodeById(node->getNodeId()) == node && !isDeleted(node.get());
    }

    std::optional<Serialization::GraphOp> build(const Entry& entry)
    {
        if (entry.kind == eEntryKindOp) {
            return entry.op;
        }
        if (entry.kind == eEntryKindProjectData) {
            return Serialization::GraphOp(projectDataOp(*project));
        }
        if (entry.kind == eEntryKindProjectKnob) {
            KnobIPtr knob = entry.knob.lock();
            if (!knob) {
                return std::nullopt;
            }
            Serialization::SetKnobOp op;
            op.knobName = knob->getName();
            op.value = projectKnobValue(*knob);

            return Serialization::GraphOp(op);
        }

        NodePtr node = entry.node.lock();
        if (!isLiveNode(node)) {
            return std::nullopt;
        }
        switch (entry.kind) {
        case eEntryKindNodeKnob: {
            KnobIPtr knob = entry.knob.lock();
            if (!knob) {
                return std::nullopt;
            }
            Serialization::SetKnobOp op;
            op.nodeId = node->getNodeId();
            op.knobName = knob->getName();
            Serialization::KnobSerialization value;
            if (knob->toSerialization(&value)) {
                op.value = value;
            }

            return Serialization::GraphOp(op);
        }
        case eEntryKindNodeUi:

            return Serialization::GraphOp(nodeUiOp(*node));
        case eEntryKindUserKnobs: {
            Serialization::NodeSerialization serialization;
            node->toSerialization(&serialization, false);
            Serialization::SetUserKnobsOp op;
            op.nodeId = node->getNodeId();
            op.userPages = serialization.userPages;
            op.pagesOrder = serialization.pagesOrder;

            return Serialization::GraphOp(op);
        }
        case eEntryKindRoto: {
            RotoCurveContextPtr roto = node->getCurveContext();
            if (!roto) {
                return std::nullopt;
            }
            Serialization::SetRotoDataOp op;
            op.nodeId = node->getNodeId();
            if (!roto->isEmpty()) {
                op.roto.emplace();
                roto->toSerialization(&*op.roto);
            }

            return Serialization::GraphOp(op);
        }
        case eEntryKindTracker: {
            TrackerContextPtr tracker = node->getTrackerContext();
            if (!tracker) {
                return std::nullopt;
            }
            Serialization::SetTrackerDataOp op;
            op.nodeId = node->getNodeId();
            op.tracker.emplace();
            tracker->toSerialization(&*op.tracker);

            return Serialization::GraphOp(op);
        }
        default:

            return std::nullopt;
        }
    }

    void appendInputs(const Node& node,
                      std::vector<Serialization::GraphOp>* ops)
    {
        const int nInputs = node.getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            NodePtr source = node.getRealGuiInput(i);
            if (source) {
                Serialization::SetInputOp op;
                op.nodeId = node.getNodeId();
                op.inputLabel = node.getInputLabel(i);
                op.sourceId = source->getNodeId();
                ops->push_back(Serialization::GraphOp(op));
            }
        }
    }

    void endCreation(std::uint64_t startSeq,
                     const NodesList& noted)
    {
        std::set<const Node*> notedSet;
        std::set<const Node*> survivors;
        for (const NodePtr& node : noted) {
            notedSet.insert(node.get());
            if (isLiveNode(node)) {
                survivors.insert(node.get());
            }
        }

        std::vector<Serialization::GraphOp> ops;
        {
            Suppress suppress;
            std::set<const Node*> done;
            for (const NodePtr& node : noted) {
                if (!survivors.count(node.get()) || !done.insert(node.get()).second) {
                    continue;
                }
                bool insideSurvivor = false;
                for (NodePtr group = enclosingGroupNode(*node); group && !insideSurvivor; group = enclosingGroupNode(*group)) {
                    insideSurvivor = survivors.count(group.get()) > 0;
                }
                if (insideSurvivor) {
                    continue;
                }
                Serialization::CreateNodeOp create;
                NodePtr parent = enclosingGroupNode(*node);
                create.parentId = parent ? parent->getNodeId() : std::string();
                create.node = std::make_shared<Serialization::NodeSerialization>();
                node->toSerialization(create.node.get());
                ops.push_back(Serialization::GraphOp(create));
            }
            for (const NodePtr& node : noted) {
                if (!survivors.count(node.get())) {
                    continue;
                }
                appendInputs(*node, &ops);
            }
        }

        std::lock_guard<std::mutex> l(lock);
        for (std::map<EntryKey, Entry>::iterator it = pending.lower_bound(EntryKey(startSeq, 0)); it != pending.end();) {
            if (it->second.subject && notedSet.count(it->second.subject)) {
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        std::uint32_t index = 1;
        for (Serialization::GraphOp& op : ops) {
            Entry entry;
            entry.op = std::move(op);
            pending.emplace(EntryKey(startSeq, index++), std::move(entry));
        }
    }
};

GraphEditRecorder::GraphEditRecorder(Project* project)
    : _imp(new Implementation(project))
{
}

GraphEditRecorder::~GraphEditRecorder()
{
    std::lock_guard<std::mutex> l(_imp->lock);
    _subscriptions.fetch_sub((int)_imp->subscribers.size());
}

int
GraphEditRecorder::subscribe(const Subscriber& subscriber)
{
    std::lock_guard<std::mutex> l(_imp->lock);
    const int id = _imp->nextSubscription++;
    _imp->subscribers[id] = subscriber;
    _subscriptions.fetch_add(1);

    return id;
}

void
GraphEditRecorder::unsubscribe(int subscription)
{
    std::lock_guard<std::mutex> l(_imp->lock);
    if (_imp->subscribers.erase(subscription) > 0) {
        _subscriptions.fetch_sub(1);
    }
    if (_imp->subscribers.empty()) {
        _imp->pending.clear();
        _imp->coalesced.clear();
        _imp->deleted.clear();
    }
}

void
GraphEditRecorder::flush()
{
    std::map<EntryKey, Entry> entries;
    std::vector<Subscriber> subscribers;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->flushScheduled = false;
        // Nodes being made are not finished: their operations are delivered after them.
        if ((creation.depth > 0) && (creation.recorder == this)) {
            return;
        }
        entries.swap(_imp->pending);
        _imp->coalesced.clear();
        for (std::map<int, Subscriber>::const_iterator it = _imp->subscribers.begin(); it != _imp->subscribers.end(); ++it) {
            subscribers.push_back(it->second);
        }
    }
    if (entries.empty() || subscribers.empty() || !isRecordableProject(*_imp->project)) {
        return;
    }

    std::vector<Serialization::GraphOp> ops;
    {
        // Serializing a node can sync knobs from its plug-in.
        Suppress suppress;
        for (std::map<EntryKey, Entry>::const_iterator it = entries.begin(); it != entries.end(); ++it) {
            std::optional<Serialization::GraphOp> op = _imp->build(it->second);
            if (op) {
                ops.push_back(std::move(*op));
            }
        }
    }
    if (ops.empty()) {
        return;
    }
    for (const Subscriber& subscriber : subscribers) {
        subscriber(ops);
    }
}

GraphEditRecorder::Suppress::Suppress()
{
    ++suppressDepth;
}

GraphEditRecorder::Suppress::~Suppress()
{
    --suppressDepth;
}

bool
GraphEditRecorder::isSuppressed()
{
    return suppressDepth > 0;
}

GraphEditRecorder*
GraphEditRecorder::recorderFor(const AppInstance* app)
{
    if (!app || isSuppressed()) {
        return nullptr;
    }
    ProjectPtr project = app->getProject();
    if (!project || !isRecordableProject(*project)) {
        return nullptr;
    }
    GraphEditRecorder* recorder = project->getGraphEditRecorder();
    if (!recorder || !recorder->_imp->hasSubscribers()) {
        return nullptr;
    }

    return recorder;
}

GraphEditRecorder::CreationScope::CreationScope(const AppInstance* app)
{
    if (!isActive()) {
        return;
    }
    begin(app);
}

GraphEditRecorder::CreationScope::CreationScope(Node* activatedNode)
{
    if (!isActive() || !activatedNode) {
        return;
    }
    AppInstancePtr app = activatedNode->getApp();
    begin(app.get());
    if (creation.depth > 0) {
        NodePtr node = activatedNode->shared_from_this();
        creation.noted.push_back(node);
        std::lock_guard<std::mutex> l(creation.recorder->_imp->lock);
        creation.recorder->_imp->deleted.erase(activatedNode);
    }
}

void
GraphEditRecorder::CreationScope::begin(const AppInstance* app)
{
    if (creation.depth > 0) {
        ++creation.depth;
        _owner = true;

        return;
    }
    GraphEditRecorder* recorder = recorderFor(app);
    if (!recorder) {
        return;
    }
    {
        std::lock_guard<std::mutex> l(recorder->_imp->lock);
        creation.startSeq = recorder->_imp->nextSeq++;
    }
    creation.recorder = recorder;
    creation.noted.clear();
    creation.depth = 1;
    _owner = true;
}

GraphEditRecorder::CreationScope::~CreationScope()
{
    if (!_owner || (--creation.depth > 0)) {
        return;
    }
    GraphEditRecorder* recorder = creation.recorder;
    NodesList noted;
    noted.swap(creation.noted);
    creation.recorder = nullptr;
    if (!recorder->_imp->hasSubscribers()) {
        return;
    }
    recorder->_imp->endCreation(creation.startSeq, noted);
    std::lock_guard<std::mutex> l(recorder->_imp->lock);
    if (!recorder->_imp->pending.empty()) {
        recorder->_imp->scheduleFlush(recorder);
    }
}

void
GraphEditRecorder::onNodeAdded(const NodePtr& node)
{
    if (!node) {
        return;
    }
    GraphEditRecorder* recorder = creation.recorder;
    if (creation.depth > 0) {
        creation.noted.push_back(node);
    } else {
        recorder = recorderFor(node->getApp().get());
    }
    if (!recorder) {
        return;
    }
    // A new node may have the address of one deleted earlier.
    std::lock_guard<std::mutex> l(recorder->_imp->lock);
    recorder->_imp->deleted.erase(node.get());
}

void
GraphEditRecorder::onNodeDeactivating(Node* node)
{
    AppInstancePtr app = node->getApp();
    GraphEditRecorder* recorder = recorderFor(app.get());
    if (!recorder || !isRecordableNode(*node) || recorder->_imp->isDeleted(node)) {
        return;
    }
    Entry entry;
    entry.op = Serialization::GraphOp(Serialization::DeleteNodeOp { node->getNodeId() });
    entry.subject = node;
    recorder->_imp->add(recorder, std::move(entry));
    std::lock_guard<std::mutex> l(recorder->_imp->lock);
    recorder->_imp->deleted.insert(node);
}

void
GraphEditRecorder::onNodeRenamed(Node* node)
{
    AppInstancePtr app = node->getApp();
    GraphEditRecorder* recorder = recorderFor(app.get());
    if (!recorder || !isRecordableNode(*node) || recorder->_imp->isDeleted(node)) {
        return;
    }
    Serialization::RenameOp op;
    op.nodeId = node->getNodeId();
    op.scriptName = node->getScriptName_mt_safe();
    op.label = node->getLabel_mt_safe();
    Entry entry;
    entry.op = Serialization::GraphOp(op);
    entry.subject = node;
    recorder->_imp->add(recorder, std::move(entry));
}

void
GraphEditRecorder::onNodeLayoutChanged(Node* node)
{
    AppInstancePtr app = node->getApp();
    GraphEditRecorder* recorder = recorderFor(app.get());
    if (!recorder || !isRecordableNode(*node)) {
        return;
    }
    Entry entry;
    entry.subject = node;
    entry.node = node->weak_from_this();
    recorder->_imp->addCoalesced(recorder, eEntryKindNodeUi, node, std::move(entry));
}

void
GraphEditRecorder::onInputChanged(Node* node,
                                  int inputNb)
{
    AppInstancePtr app = node->getApp();
    GraphEditRecorder* recorder = recorderFor(app.get());
    if (!recorder || !isRecordableNode(*node) || recorder->_imp->isDeleted(node) || (inputNb < 0) || (inputNb >= node->getNInputs())) {
        return;
    }
    Serialization::SetInputOp op;
    op.nodeId = node->getNodeId();
    op.inputLabel = node->getInputLabel(inputNb);
    NodePtr source = node->getRealGuiInput(inputNb);
    if (source) {
        op.sourceId = source->getNodeId();
    }
    Entry entry;
    entry.op = Serialization::GraphOp(op);
    entry.subject = node;
    recorder->_imp->add(recorder, std::move(entry));
}

void
GraphEditRecorder::onKnobContentChanged(KnobI* knob)
{
    if (isSuppressed()) {
        return;
    }
    KnobHolder* holder = knob->getHolder();
    if (!holder) {
        return;
    }
    if (TrackMarker* marker = dynamic_cast<TrackMarker*>(holder)) {
        TrackerContextPtr context = marker->getContext();
        if (context) {
            onTrackerChanged(context->getNode());
        }

        return;
    }
    if (!knob->getIsPersistent() || dynamic_cast<KnobPage*>(knob) || dynamic_cast<KnobButton*>(knob)) {
        return;
    }
    if (Project* project = dynamic_cast<Project*>(holder)) {
        if (dynamic_cast<KnobGroup*>(knob) || isSavedEachTime(*knob) || (knob->isUserKnob() && !isProjectVariable(*knob))) {
            return;
        }
        GraphEditRecorder* recorder = recorderFor(project->getApp().get());
        if (!recorder) {
            return;
        }
        Entry entry;
        entry.knob = knob->weak_from_this();
        recorder->_imp->addCoalesced(recorder, eEntryKindProjectKnob, knob, std::move(entry));

        return;
    }
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
    if (!effect) {
        return;
    }
    NodePtr node = effect->getNode();
    if (!node) {
        return;
    }
    GraphEditRecorder* recorder = recorderFor(node->getApp().get());
    if (!recorder || !isRecordableNode(*node)) {
        return;
    }
    Entry entry;
    entry.subject = node.get();
    entry.node = node;
    if (knob->isUserKnob()) {
        recorder->_imp->addCoalesced(recorder, eEntryKindUserKnobs, node.get(), std::move(entry));

        return;
    }
    if (dynamic_cast<KnobGroup*>(knob)) {
        return;
    }
    entry.knob = knob->weak_from_this();
    recorder->_imp->addCoalesced(recorder, eEntryKindNodeKnob, knob, std::move(entry));
}

void
GraphEditRecorder::onKnobListChanged(KnobHolder* holder)
{
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
    if (!effect) {
        return;
    }
    NodePtr node = effect->getNode();
    if (!node) {
        return;
    }
    GraphEditRecorder* recorder = recorderFor(node->getApp().get());
    if (!recorder || !isRecordableNode(*node)) {
        return;
    }
    Entry entry;
    entry.subject = node.get();
    entry.node = node;
    recorder->_imp->addCoalesced(recorder, eEntryKindUserKnobs, node.get(), std::move(entry));
}

void
GraphEditRecorder::onRotoChanged(const NodePtr& node)
{
    if (!node) {
        return;
    }
    GraphEditRecorder* recorder = recorderFor(node->getApp().get());
    if (!recorder || !isRecordableNode(*node)) {
        return;
    }
    Entry entry;
    entry.subject = node.get();
    entry.node = node;
    recorder->_imp->addCoalesced(recorder, eEntryKindRoto, node.get(), std::move(entry));
}

void
GraphEditRecorder::onTrackerChanged(const NodePtr& node)
{
    if (!node) {
        return;
    }
    GraphEditRecorder* recorder = recorderFor(node->getApp().get());
    if (!recorder || !isRecordableNode(*node)) {
        return;
    }
    Entry entry;
    entry.subject = node.get();
    entry.node = node;
    recorder->_imp->addCoalesced(recorder, eEntryKindTracker, node.get(), std::move(entry));
}

void
GraphEditRecorder::onProjectDataChanged(Project* project)
{
    GraphEditRecorder* recorder = recorderFor(project->getApp().get());
    if (!recorder) {
        return;
    }
    recorder->_imp->addCoalesced(recorder, eEntryKindProjectData, project, Entry());
}

NATRON_NAMESPACE_EXIT
