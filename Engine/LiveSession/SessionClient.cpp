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

#include "SessionClient.h"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QtCore/QByteArray>
#include <QtNetwork/QTcpSocket>

#include "Engine/AppInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/GraphOpApply.h"
#include "Engine/KnobTypes.h"
#include "Engine/LiveSession/SessionShared.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Ops/OpsPrivate.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Session/SessionProtocol.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace Serialization::Session;

struct PendingEdit {
    std::uint64_t clientSeq = 0;
    std::vector<Serialization::GraphOp> ops;
    std::vector<std::string> keys;
};

} // namespace

struct SessionClient::Implementation {
    SessionClient* publicInterface;
    AppInstanceWPtr app;
    QTcpSocket* socket = nullptr;
    SessionFrameDecoder decoder;
    State state = State::eDisconnected;
    QString displayName;
    quint32 userId = 0;
    std::uint64_t seq = 0;
    std::uint64_t nextClientSeq = 1;
    std::deque<PendingEdit> pending;
    std::map<std::string, int> pendingKeys;
    std::vector<Serialization::GraphOp> deferred;
    int subscription = -1;
    // The project subscribed to, which can outlive the app's last reference while the app is
    // destroyed, and must lose the subscription then.
    ProjectWPtr subscribedProject;
    bool handling = false;
    bool readAgain = false;
    bool awaitingProject = false;
    bool ended = false;
    bool deleteWhenIdle = false;
    int mismatches = 0;
    int rejects = 0;
    int loads = 0;

    Implementation(SessionClient* publicInterface,
                   const AppInstancePtr& app)
        : publicInterface(publicInterface)
        , app(app)
    {
    }

    ProjectPtr project() const
    {
        AppInstancePtr instance = app.lock();

        return instance ? instance->getProject() : ProjectPtr();
    }

    GraphEditRecorder* recorder() const
    {
        ProjectPtr p = project();

        return p ? p->getGraphEditRecorder() : nullptr;
    }

    void send(const SessionMessage& message)
    {
        if (!socket || (socket->state() != QAbstractSocket::ConnectedState)) {
            return;
        }
        try {
            const std::vector<std::byte> frame = frameSessionMessage(message);
            socket->write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size()));
        } catch (const SessionProtocolError& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8(e.what()));
        }
    }

    void flushLocalEdits()
    {
        if (GraphEditRecorder* r = recorder()) {
            r->flush();
        }
    }

    bool isPendingKey(const std::string& key) const
    {
        return pendingKeys.count(key) > 0;
    }

    void addPending(PendingEdit edit)
    {
        for (const std::string& key : edit.keys) {
            ++pendingKeys[key];
        }
        pending.push_back(std::move(edit));
    }

    void popPending()
    {
        for (const std::string& key : pending.front().keys) {
            std::map<std::string, int>::iterator it = pendingKeys.find(key);
            if ((it != pendingKeys.end()) && (--it->second <= 0)) {
                pendingKeys.erase(it);
            }
        }
        pending.pop_front();
    }

    void clearPending()
    {
        pending.clear();
        pendingKeys.clear();
        deferred.clear();
    }

    void onLocalOps(const std::vector<Serialization::GraphOp>& ops)
    {
        if ((state != State::eJoined) || ops.empty()) {
            return;
        }
        PendingEdit edit;
        edit.clientSeq = nextClientSeq++;
        edit.ops = ops;
        for (const Serialization::GraphOp& op : ops) {
            const std::vector<std::string> keys = LiveSession::graphOpKeys(op);
            edit.keys.insert(edit.keys.end(), keys.begin(), keys.end());
        }
        send(OpsMessage { edit.clientSeq, ops });
        // A deferred host edit came before this one on the host, which applies this one over it.
        // Retried here once this one is acknowledged, and so no longer pending, it would undo it.
        for (const Serialization::GraphOp& op : ops) {
            supersedeDeferred(op);
        }
        addPending(std::move(edit));
    }

    bool mustSkip(const Serialization::GraphOp& op) const
    {
        for (const std::string& key : LiveSession::graphOpKeys(op)) {
            if (isPendingKey(key)) {
                return true;
            }
        }
        for (const std::string& id : LiveSession::graphOpNodeIds(op)) {
            if (isPendingKey(LiveSession::nodeExistenceKey(id))) {
                return true;
            }
        }

        return false;
    }

    // What a host edit does here, or nothing when an edit still pending here overrides it. A
    // connection from a node a pending edit deletes is a disconnection: the host connects it and
    // then deletes its source, which empties the input, while skipping it would leave this
    // copy's old source in place.
    std::optional<Serialization::GraphOp> asAppliedHere(const ProjectPtr& p,
                                                        const Serialization::GraphOp& op) const
    {
        if (const Serialization::SetInputOp* input = std::get_if<Serialization::SetInputOp>(&op)) {
            if (!input->sourceId.empty() && isPendingKey(LiveSession::nodeExistenceKey(input->sourceId)) && !p->findNodeById(input->sourceId)) {
                Serialization::SetInputOp disconnect = *input;
                disconnect.sourceId.clear();
                if (mustSkip(Serialization::GraphOp(disconnect))) {
                    return std::nullopt;
                }

                return Serialization::GraphOp(disconnect);
            }
        }
        if (mustSkip(op)) {
            return std::nullopt;
        }

        return op;
    }

    // A deferred host edit is out of date once a later edit, the host's or this user's, writes the
    // same thing or deletes its node. When that later edit deletes its source, the host's input
    // ends up empty.
    void supersedeDeferred(const Serialization::GraphOp& op)
    {
        if (deferred.empty()) {
            return;
        }
        const std::vector<std::string> keys = LiveSession::graphOpKeys(op);
        const Serialization::DeleteNodeOp* deletion = std::get_if<Serialization::DeleteNodeOp>(&op);
        for (std::vector<Serialization::GraphOp>::iterator it = deferred.begin(); it != deferred.end();) {
            const std::vector<std::string> waiting = LiveSession::graphOpKeys(*it);
            bool superseded = std::find_first_of(waiting.begin(), waiting.end(), keys.begin(), keys.end()) != waiting.end();
            if (!superseded && deletion) {
                superseded = (LiveSession::graphOpTargetNode(*it) == deletion->nodeId);
                Serialization::SetInputOp* input = std::get_if<Serialization::SetInputOp>(&*it);
                if (!superseded && input && (input->sourceId == deletion->nodeId)) {
                    input->sourceId.clear();
                }
            }
            if (superseded) {
                it = deferred.erase(it);
            } else {
                ++it;
            }
        }
    }

    void dropDeferredOf(const std::string& nodeId)
    {
        for (std::vector<Serialization::GraphOp>::iterator it = deferred.begin(); it != deferred.end();) {
            const std::vector<std::string> ids = LiveSession::graphOpNodeIds(*it);
            if (std::find(ids.begin(), ids.end(), nodeId) != ids.end()) {
                it = deferred.erase(it);
            } else {
                ++it;
            }
        }
    }

    // A host edit may fail here only because of this user's pending edits, a connection that
    // closes a cycle with a pending one for instance. The host applies those after it and refuses
    // or keeps them, so the edit waits until they are settled and is applied then. They are not
    // settled at their acknowledgement: the host's refusal and its resync of what it refused
    // follow it.
    void applyOrDefer(const ProjectPtr& p,
                      const Serialization::GraphOp& op,
                      bool arriving)
    {
        const std::vector<Serialization::OpResult> results = applyGraphOps(p, { op });
        if (results.empty() || results.front().applied()) {
            return;
        }
        if (arriving && pending.empty()) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("A host edit (%1) could not be applied here: %2")
                                                       .arg(QString::fromUtf8(std::string(Serialization::graphOpName(op)).c_str()),
                                                            QString::fromUtf8(results.front().message.c_str())));
        } else {
            deferred.push_back(op);
        }
    }

    void retryDeferred(const ProjectPtr& p)
    {
        if (deferred.empty()) {
            return;
        }
        std::vector<Serialization::GraphOp> waiting;
        waiting.swap(deferred);
        {
            GraphEditRecorder::Suppress suppress;
            for (const Serialization::GraphOp& op : waiting) {
                if (std::optional<Serialization::GraphOp> here = asAppliedHere(p, op)) {
                    applyOrDefer(p, *here, false);
                }
            }
        }
        restorePendingMaskSwitches(p);
    }

    // A mask switch is set by hand, and also follows its input whenever the input changes. A host
    // edit of an input, or a deletion that empties one, sets the switch here although a pending
    // edit of the switch, or of the input under it, comes after it on the host and decides the
    // switch there. The last such pending edit decides it here too.
    void restorePendingMaskSwitches(const ProjectPtr& p)
    {
        if (pending.empty()) {
            return;
        }
        std::map<std::pair<std::string, int>, const Serialization::GraphOp*> decisive;
        for (const PendingEdit& edit : pending) {
            for (const Serialization::GraphOp& op : edit.ops) {
                if (const Serialization::SetKnobOp* set = std::get_if<Serialization::SetKnobOp>(&op)) {
                    NodePtr node = set->nodeId ? p->findNodeById(*set->nodeId) : NodePtr();
                    if (!node) {
                        continue;
                    }
                    const int nInputs = node->getNInputs();
                    for (int i = 0; i < nInputs; ++i) {
                        KnobBoolPtr maskSwitch = node->getMaskEnabledKnob(i);
                        if (maskSwitch && (maskSwitch->getName() == set->knobName)) {
                            decisive[std::make_pair(*set->nodeId, i)] = &op;
                            break;
                        }
                    }
                } else if (const Serialization::SetInputOp* input = std::get_if<Serialization::SetInputOp>(&op)) {
                    NodePtr node = p->findNodeById(input->nodeId);
                    const int inputNb = node ? node->getInputNumberFromLabel(input->inputLabel) : -1;
                    if ((inputNb >= 0) && node->getMaskEnabledKnob(inputNb)) {
                        decisive[std::make_pair(input->nodeId, inputNb)] = &op;
                    }
                }
            }
        }
        GraphEditRecorder::Suppress suppress;
        for (const std::pair<const std::pair<std::string, int>, const Serialization::GraphOp*>& entry : decisive) {
            if (std::holds_alternative<Serialization::SetKnobOp>(*entry.second)) {
                applyGraphOps(p, { *entry.second });
            } else if (NodePtr node = p->findNodeById(entry.first.first)) {
                node->refreshMaskEnabledNess(entry.first.second);
            }
        }
    }

    // A pending creation or rename here may hold the name the host gave another node. That node
    // takes the name, and the holder gets a free one until the host's name for it arrives.
    void freeName(const ProjectPtr& p,
                  const NodeCollectionPtr& collection,
                  const std::string& name,
                  const std::string& nodeId)
    {
        if (!collection || name.empty()) {
            return;
        }
        NodePtr holder = collection->getNodeByName(name);
        if (!holder || (holder->getNodeId() == nodeId)) {
            return;
        }
        const std::string aside = LiveSession::uniqueScriptName(collection, name + "_", holder.get(), { name });
        applyGraphOps(p, { Serialization::GraphOp(Serialization::RenameOp { holder->getNodeId(), aside, std::nullopt }) });
    }

    void freeNameFor(const ProjectPtr& p,
                     const Serialization::GraphOp& op)
    {
        if (const Serialization::CreateNodeOp* create = std::get_if<Serialization::CreateNodeOp>(&op)) {
            if (create->node) {
                freeName(p, LiveSession::collectionForParent(p, create->parentId), create->node->scriptName, create->node->id);
            }
        } else if (const Serialization::RenameOp* rename = std::get_if<Serialization::RenameOp>(&op)) {
            NodePtr node = p->findNodeById(rename->nodeId);
            if (node) {
                freeName(p, node->getGroup(), rename->scriptName, rename->nodeId);
            }
        }
    }

    void applyHostOps(const ProjectPtr& p,
                      const std::vector<Serialization::GraphOp>& ops)
    {
        GraphEditRecorder::Suppress suppress;
        for (const Serialization::GraphOp& op : ops) {
            supersedeDeferred(op);
            std::optional<Serialization::GraphOp> here = asAppliedHere(p, op);
            if (!here) {
                continue;
            }
            freeNameFor(p, *here);
            applyOrDefer(p, *here, true);
        }
        restorePendingMaskSwitches(p);
    }

    // The host may have given this user's creations and renames other names than asked for.
    void adoptHostNames(const ProjectPtr& p,
                        const std::vector<Serialization::GraphOp>& acknowledged)
    {
        std::vector<Serialization::GraphOp> renames;
        for (const Serialization::GraphOp& op : acknowledged) {
            std::string nodeId;
            std::string name;
            std::optional<std::string> label;
            if (const Serialization::CreateNodeOp* create = std::get_if<Serialization::CreateNodeOp>(&op)) {
                if (!create->node) {
                    continue;
                }
                nodeId = create->node->id;
                name = create->node->scriptName;
                label = create->node->label;
            } else if (const Serialization::RenameOp* rename = std::get_if<Serialization::RenameOp>(&op)) {
                nodeId = rename->nodeId;
                name = rename->scriptName;
                label = rename->label;
            } else {
                continue;
            }
            if (isPendingKey("name:" + nodeId) || isPendingKey(LiveSession::nodeExistenceKey(nodeId))) {
                continue;
            }
            NodePtr node = p->findNodeById(nodeId);
            if (!node || ((node->getScriptName() == name) && (!label || (node->getLabel() == *label)))) {
                continue;
            }
            renames.push_back(Serialization::GraphOp(Serialization::RenameOp { nodeId, name, label }));
        }
        if (!renames.empty()) {
            applyHostOps(p, renames);
        }
    }

    // The project was just loaded from the host, so a node already holding the name a pending
    // creation or rename asks for holds it on the host too: the pending node waits under a free
    // name until the host's name for it arrives.
    Serialization::GraphOp withUntakenName(const ProjectPtr& p,
                                           const Serialization::GraphOp& op) const
    {
        if (const Serialization::CreateNodeOp* create = std::get_if<Serialization::CreateNodeOp>(&op)) {
            if (!create->node) {
                return op;
            }
            const NodeCollectionPtr collection = LiveSession::collectionForParent(p, create->parentId);
            const std::string& name = create->node->scriptName;
            NodePtr holder = collection ? collection->getNodeByName(name) : NodePtr();
            if (!holder || (holder->getNodeId() == create->node->id)) {
                return op;
            }
            Serialization::CreateNodeOp aside = *create;
            aside.node = Serialization::OpsDetail::deepCopyNode(*create->node);
            aside.node->scriptName = LiveSession::uniqueScriptName(collection, name + "_", nullptr, { name });

            return Serialization::GraphOp(aside);
        }
        if (const Serialization::RenameOp* rename = std::get_if<Serialization::RenameOp>(&op)) {
            NodePtr node = p->findNodeById(rename->nodeId);
            const NodeCollectionPtr collection = node ? node->getGroup() : NodeCollectionPtr();
            if (!collection || rename->scriptName.empty()) {
                return op;
            }
            NodePtr holder = collection->getNodeByName(rename->scriptName);
            if (!holder || (holder == node)) {
                return op;
            }
            Serialization::RenameOp aside = *rename;
            aside.scriptName = LiveSession::uniqueScriptName(collection, rename->scriptName + "_", node.get(), { rename->scriptName });

            return Serialization::GraphOp(aside);
        }

        return op;
    }

    void reapplyPending(const ProjectPtr& p,
                        const std::optional<std::string>& onlyNode)
    {
        std::vector<Serialization::GraphOp> ops;
        for (const PendingEdit& edit : pending) {
            for (const Serialization::GraphOp& op : edit.ops) {
                if (onlyNode) {
                    const std::vector<std::string> ids = LiveSession::graphOpNodeIds(op);
                    if (std::find(ids.begin(), ids.end(), *onlyNode) == ids.end()) {
                        continue;
                    }
                }
                ops.push_back(op);
            }
        }
        GraphEditRecorder::Suppress suppress;
        for (const Serialization::GraphOp& op : ops) {
            applyGraphOps(p, { withUntakenName(p, op) });
        }
    }

    void handleBroadcast(const BroadcastMessage& broadcast)
    {
        ProjectPtr p = project();
        if (!p) {
            return;
        }
        flushLocalEdits();
        if ((broadcast.originUser == userId) && (broadcast.originUser != 0)) {
            while (!pending.empty() && (pending.front().clientSeq <= broadcast.clientSeq)) {
                popPending();
            }
            adoptHostNames(p, broadcast.ops);
            retryDeferred(p);
        } else if (broadcast.seq > seq) {
            applyHostOps(p, broadcast.ops);
        }
        if (broadcast.seq > seq) {
            seq = broadcast.seq;
        }
    }

    void handleResyncNode(const ResyncNodeMessage& message)
    {
        ProjectPtr p = project();
        if (!p) {
            return;
        }
        flushLocalEdits();
        dropDeferredOf(message.nodeId);
        {
            GraphEditRecorder::Suppress suppress;
            if (p->findNodeById(message.nodeId)) {
                applyGraphOps(p, { Serialization::GraphOp(Serialization::DeleteNodeOp { message.nodeId }) });
            }
        }
        // The node's state is the host's before this user's pending edits, which win over it
        // here as they will on the host: the ones of this node are applied again below, and the
        // others are still in place.
        if (!message.deleted) {
            applyHostOps(p, message.ops);
        }
        reapplyPending(p, message.nodeId);
        retryDeferred(p);
        restorePendingMaskSwitches(p);
    }

    bool loadProject(const WelcomeMessage& welcome)
    {
        ProjectPtr p = project();
        if (!p) {
            return false;
        }
        flushLocalEdits();
        QString error;
        bool ok = false;
        {
            GraphEditRecorder::Suppress suppress;
            ok = p->loadProjectFromBundleBytes(welcome.snapshot, QString(), QString::fromUtf8(welcome.projectName.c_str()), &error);
        }
        // Loading runs the event loop, where the session may have ended.
        if (ended) {
            return false;
        }
        if (!ok) {
            end(QString::fromUtf8("The host's project could not be loaded: %1").arg(error));

            return false;
        }
        ++loads;
        seq = welcome.seq;
        awaitingProject = false;
        deferred.clear();
        reapplyPending(p, std::nullopt);

        return true;
    }

    void handleWelcome(const WelcomeMessage& welcome)
    {
        const bool joining = (state == State::eJoining);
        if (!loadProject(welcome) || ended) {
            return;
        }
        if (!joining) {
            return;
        }
        userId = welcome.userId;
        if (GraphEditRecorder* r = recorder()) {
            r->flush();
            subscription = r->subscribe([this](const std::vector<Serialization::GraphOp>& ops) { onLocalOps(ops); });
            subscribedProject = project();
        }
        state = State::eJoined;
        Q_EMIT publicInterface->joined();
    }

    void handleChecksum(const ChecksumMessage& checksum)
    {
        ProjectPtr p = project();
        flushLocalEdits();
        if (!p || awaitingProject || !pending.empty() || (checksum.seq != seq)) {
            return;
        }
        std::string mine;
        try {
            mine = LiveSession::projectChecksum(p);
        } catch (const std::exception& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("The project checksum could not be computed: %1").arg(QString::fromUtf8(e.what())));

            return;
        }
        if (mine == checksum.hash) {
            return;
        }
        ++mismatches;
        awaitingProject = true;
        Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("The project differs from the host's at edit %1: reloading it from the host.").arg(checksum.seq));
        send(ResyncRequestMessage {});
    }

    void handle(const SessionMessage& message)
    {
        if (const ByeMessage* bye = std::get_if<ByeMessage>(&message)) {
            end(QString::fromUtf8(bye->reason.c_str()));

            return;
        }
        if (const WelcomeMessage* welcome = std::get_if<WelcomeMessage>(&message)) {
            handleWelcome(*welcome);

            return;
        }
        if (state != State::eJoined) {
            end(QString::fromUtf8("Protocol error: %1 before welcome.").arg(QString::fromUtf8(std::string(sessionMessageName(message)).c_str())));

            return;
        }
        if (const BroadcastMessage* broadcast = std::get_if<BroadcastMessage>(&message)) {
            handleBroadcast(*broadcast);
        } else if (const RejectMessage* reject = std::get_if<RejectMessage>(&message)) {
            ++rejects;
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("The host refused an edit: %1").arg(QString::fromUtf8(reject->reason.c_str())));
        } else if (const ResyncNodeMessage* resync = std::get_if<ResyncNodeMessage>(&message)) {
            handleResyncNode(*resync);
        } else if (const ChecksumMessage* checksum = std::get_if<ChecksumMessage>(&message)) {
            handleChecksum(*checksum);
        } else {
            end(QString::fromUtf8("Protocol error: unexpected %1 message.").arg(QString::fromUtf8(std::string(sessionMessageName(message)).c_str())));
        }
    }

    // Reading is not reentrant: applying operations or loading may run the event loop, and the
    // bytes that arrive meanwhile are read once the message being handled is done.
    void pump()
    {
        if (handling) {
            readAgain = true;

            return;
        }
        handling = true;
        do {
            readAgain = false;
            if (!socket || ended) {
                break;
            }
            const QByteArray bytes = socket->readAll();
            if (!bytes.isEmpty()) {
                decoder.feed(bytes.constData(), static_cast<std::size_t>(bytes.size()));
            }
            SessionMessage message;
            while (!ended) {
                const SessionFrameDecoder::Status status = decoder.next(&message);
                if (status == SessionFrameDecoder::Status::eNeedMore) {
                    break;
                }
                if (status == SessionFrameDecoder::Status::eError) {
                    end(QString::fromUtf8("The host sent unreadable data: %1").arg(QString::fromUtf8(decoder.error().c_str())));
                    break;
                }
                handle(message);
            }
        } while (readAgain);
        handling = false;
        if (deleteWhenIdle) {
            deleteWhenIdle = false;
            publicInterface->deleteLater();
        }
    }

    void end(const QString& reason)
    {
        if (ended || (state == State::eDisconnected)) {
            return;
        }
        ended = true;
        if (subscription >= 0) {
            if (ProjectPtr subscribed = subscribedProject.lock()) {
                if (GraphEditRecorder* r = subscribed->getGraphEditRecorder()) {
                    r->unsubscribe(subscription);
                }
            }
            subscription = -1;
        }
        state = State::eDisconnected;
        clearPending();
        if (socket) {
            socket->disconnect(publicInterface);
            socket->disconnectFromHost();
            socket->deleteLater();
            socket = nullptr;
        }
        Q_EMIT publicInterface->left(reason);
    }

    void start(const QString& host,
               quint16 port,
               const QString& name)
    {
        displayName = name;
        userId = 0;
        seq = 0;
        nextClientSeq = 1;
        clearPending();
        decoder = SessionFrameDecoder();
        awaitingProject = false;
        ended = false;
        mismatches = 0;
        rejects = 0;
        loads = 0;
        state = State::eConnecting;
        socket = new QTcpSocket(publicInterface);
        QObject::connect(socket, &QTcpSocket::connected, publicInterface, [this]() {
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            state = State::eJoining;
            HelloMessage hello;
            hello.protocol = kProtocolVersion;
            hello.name = displayName.toStdString();
            hello.natronVersion = NATRON_VERSION_STRING;
            send(hello);
        });
        QObject::connect(socket, &QTcpSocket::readyRead, publicInterface, [this]() { pump(); });
        QObject::connect(socket, &QTcpSocket::disconnected, publicInterface, [this]() {
            if (handling) {
                readAgain = true;
            } else {
                pump();
            }
            end(QString::fromUtf8("The connection to the host was lost. The project is kept here, unsaved."));
        });
        QObject::connect(socket, &QTcpSocket::errorOccurred, publicInterface, [this](QAbstractSocket::SocketError) {
            if (state == State::eConnecting) {
                end(QString::fromUtf8("Cannot connect to the host: %1").arg(socket->errorString()));
            }
        });
        socket->connectToHost(host, port);
    }
};

SessionClient::SessionClient(const AppInstancePtr& app,
                             QObject* parent)
    : QObject(parent)
    , _imp(new Implementation(this, app))
{
}

SessionClient::~SessionClient()
{
    _imp->deleteWhenIdle = false;
    if (_imp->state != State::eDisconnected) {
        _imp->send(ByeMessage { std::string() });
        if (_imp->socket) {
            _imp->socket->flush();
        }
        _imp->end(QString());
    }
}

void
SessionClient::connectToHost(const QString& host,
                             quint16 port,
                             const QString& displayName)
{
    if (_imp->state != State::eDisconnected) {
        leave();
    }
    _imp->start(host, port, displayName);
}

void
SessionClient::leave(const QString& reason)
{
    if (_imp->state == State::eDisconnected) {
        return;
    }
    _imp->flushLocalEdits();
    _imp->send(ByeMessage { std::string() });
    if (_imp->socket) {
        _imp->socket->flush();
    }
    _imp->end(reason.isEmpty() ? tr("You left the session. The project is kept here, unsaved.") : reason);
}

void
SessionClient::scheduleDeletion()
{
    if (_imp->handling) {
        _imp->deleteWhenIdle = true;
    } else {
        deleteLater();
    }
}

void
SessionClient::requestSave()
{
    if (_imp->state == State::eJoined) {
        _imp->flushLocalEdits();
        _imp->send(SaveRequestMessage {});
    }
}

SessionClient::State
SessionClient::state() const
{
    return _imp->state;
}

quint32
SessionClient::userId() const
{
    return _imp->userId;
}

std::uint64_t
SessionClient::seq() const
{
    return _imp->seq;
}

void
SessionClient::flushLocalEdits()
{
    _imp->flushLocalEdits();
}

bool
SessionClient::hasPending() const
{
    return !_imp->pending.empty() || _imp->awaitingProject;
}

bool
SessionClient::isIdle() const
{
    if (!_imp->socket) {
        return true;
    }

    return (_imp->socket->bytesToWrite() == 0) && (_imp->socket->bytesAvailable() == 0) && (_imp->decoder.bufferedBytes() == 0);
}

int
SessionClient::checksumMismatchCount() const
{
    return _imp->mismatches;
}

int
SessionClient::rejectCount() const
{
    return _imp->rejects;
}

int
SessionClient::projectLoadCount() const
{
    return _imp->loads;
}

NATRON_NAMESPACE_EXIT
