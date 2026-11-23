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

#include "SessionHost.h"

#include <limits>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "Engine/AppInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/GraphOpApply.h"
#include "Engine/LiveSession/SessionPresence.h"
#include "Engine/LiveSession/SessionRemoteEdits.h"
#include "Engine/LiveSession/SessionShared.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Serialization/Ops/OpsPrivate.h"
#include "Serialization/Session/SessionProtocol.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace Serialization::Session;
using Serialization::NodeUiColor;

const NodeUiColor kPeerColours[] = {
    { 0.90f, 0.35f, 0.30f },
    { 0.30f, 0.65f, 0.95f },
    { 0.40f, 0.80f, 0.40f },
    { 0.95f, 0.75f, 0.25f },
    { 0.75f, 0.45f, 0.90f },
    { 0.30f, 0.85f, 0.80f },
    { 0.95f, 0.55f, 0.75f },
    { 0.65f, 0.65f, 0.65f },
};

const NodeUiColor kHostColour = { 1.0f, 0.55f, 0.10f };

// A client that stops reading must not grow the host's memory without end; a welcome, the
// largest frame, fits twice over.
const qint64 kMaxUnsentBytes = qint64(2) * qint64(kMaxFrameBytes);
const qint64 kClosingTimeoutMs = 10000;
const int kHousekeepingIntervalMs = 1000;

struct Peer {
    QPointer<QTcpSocket> socket;
    SessionFrameDecoder decoder;
    quint32 userId = 0;
    QString name;
    NodeUiColor colour;
    bool joined = false;
    bool closing = false;
    QElapsedTimer closingFor;
    bool wantsResync = false;
    bool wantsSave = false;
};

} // namespace

struct SessionHost::Implementation {
    SessionHost* publicInterface;
    AppInstanceWPtr app;
    QTcpServer server;
    QTimer checksumTimer;
    QTimer housekeepingTimer;
    std::vector<std::unique_ptr<Peer>> peers;
    // Peers of a stopped session, kept until no call that may hold one is running.
    std::vector<std::unique_ptr<Peer>> retired;
    std::uint64_t seq = 0;
    std::uint64_t lastChecksumSeq = std::numeric_limits<std::uint64_t>::max();
    quint32 nextUserId = 1;
    QString displayName;
    int subscription = -1;
    // The project subscribed to, which can outlive the app's last reference while the app is
    // destroyed, and must lose the subscription then.
    ProjectWPtr subscribedProject;
    bool handling = false;
    bool readAgain = false;
    // Calls running that may hold a peer, or this host, across a signal whose slot can stop or
    // delete it: peers are only destroyed, and the host only deleted, once none is.
    int busy = 0;
    bool deleteWhenIdle = false;
    int rejects = 0;
    SessionPresence presence;

    class BusyScope {
    public:
        explicit BusyScope(Implementation* imp)
            : _imp(imp)
        {
            ++_imp->busy;
        }

        ~BusyScope()
        {
            if (--_imp->busy == 0) {
                _imp->settle();
            }
        }

        BusyScope(const BusyScope&) = delete;
        BusyScope& operator=(const BusyScope&) = delete;

    private:
        Implementation* _imp;
    };

    Implementation(SessionHost* publicInterface,
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

    int joinedCount() const
    {
        int count = 0;
        for (const std::unique_ptr<Peer>& peer : peers) {
            if (peer->joined && !peer->closing) {
                ++count;
            }
        }

        return count;
    }

    void settle()
    {
        releaseRetired();
        removeClosedPeers();
        if (deleteWhenIdle) {
            deleteWhenIdle = false;
            publicInterface->deleteLater();
        }
    }

    // A peer whose bytes pile up unsent is not reading: it is cut off at once.
    void write(Peer* peer,
               const std::vector<std::byte>& frame)
    {
        peer->socket->write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size()));
        if (peer->socket && (peer->socket->bytesToWrite() > kMaxUnsentBytes)) {
            const QPointer<QTcpSocket> socket = peer->socket;
            drop(peer, QString());
            if (socket) {
                socket->abort();
            }
        }
    }

    bool send(Peer* peer,
              const SessionMessage& message)
    {
        if (!peer->socket || peer->closing) {
            return false;
        }
        std::vector<std::byte> frame;
        try {
            frame = frameSessionMessage(message);
        } catch (const SessionProtocolError& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8(e.what()));

            return false;
        }
        write(peer, frame);

        return true;
    }

    void sendToJoined(const SessionMessage& message,
                      const Peer* except = nullptr)
    {
        std::vector<std::byte> frame;
        try {
            frame = frameSessionMessage(message);
        } catch (const SessionProtocolError& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8(e.what()));

            return;
        }
        // By index: dropping a peer that does not read emits signals, whose slots may stop the
        // session and empty the list.
        for (std::size_t i = 0; i < peers.size(); ++i) {
            Peer* peer = peers[i].get();
            if ((peer != except) && peer->joined && !peer->closing && peer->socket) {
                write(peer, frame);
            }
        }
    }

    void flushLocalEdits()
    {
        ProjectPtr p = project();
        if (p && p->getGraphEditRecorder()) {
            p->getGraphEditRecorder()->flush();
        }
    }

    void onLocalOps(const std::vector<Serialization::GraphOp>& ops)
    {
        if (ops.empty()) {
            return;
        }
        BusyScope scope(this);
        BroadcastMessage broadcast;
        broadcast.seq = ++seq;
        broadcast.originUser = 0;
        broadcast.clientSeq = 0;
        broadcast.ops = ops;
        sendToJoined(broadcast);
    }

    // Drops the peer when it cannot have the project: a client that joined without it would wait
    // for it forever.
    void sendWelcome(Peer* peer)
    {
        ProjectPtr p = project();
        if (!p) {
            drop(peer, QString::fromUtf8("The host has no project to send."));

            return;
        }
        flushLocalEdits();
        WelcomeMessage welcome;
        welcome.userId = peer->userId;
        welcome.colour = peer->colour;
        welcome.seq = seq;
        welcome.projectName = p->getProjectFilename().toStdString();
        try {
            welcome.snapshot = LiveSession::projectBundleBytes(p);
        } catch (const std::exception& e) {
            drop(peer, QString::fromUtf8("The project could not be sent: %1").arg(QString::fromUtf8(e.what())));

            return;
        }
        if (!send(peer, welcome) && !peer->closing) {
            drop(peer, QString::fromUtf8("The project could not be sent."));
        }
    }

    void drop(Peer* peer,
              const QString& reason)
    {
        if (peer->closing) {
            return;
        }
        if (!reason.isEmpty()) {
            send(peer, ByeMessage { reason.toStdString() });
            if (peer->closing) {
                return;
            }
        }
        const bool wasJoined = peer->joined;
        peer->closing = true;
        peer->closingFor.start();
        peer->joined = false;
        if (peer->socket) {
            peer->socket->disconnectFromHost();
        }
        if (wasJoined) {
            presence.removeUser(peer->userId);
            sendToJoined(PresenceLeftMessage { peer->userId });
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 left the session.").arg(peer->name));
            Q_EMIT publicInterface->peerCountChanged(joinedCount());
        }
    }

    void releaseRetired()
    {
        std::vector<std::unique_ptr<Peer>> leaving;
        leaving.swap(retired);
        for (const std::unique_ptr<Peer>& peer : leaving) {
            if (peer->socket) {
                // flush() can finish the disconnect and emit disconnected() before it returns.
                peer->socket->disconnect(publicInterface);
                peer->socket->flush();
                peer->socket->deleteLater();
                peer->socket = nullptr;
            }
        }
    }

    void removeClosedPeers()
    {
        if (busy > 0) {
            return;
        }
        for (std::vector<std::unique_ptr<Peer>>::iterator it = peers.begin(); it != peers.end();) {
            Peer* peer = it->get();
            const bool gone = !peer->socket || (peer->socket->state() == QAbstractSocket::UnconnectedState);
            if (peer->closing && gone) {
                if (peer->socket) {
                    peer->socket->disconnect(publicInterface);
                    peer->socket->deleteLater();
                }
                it = peers.erase(it);
            } else {
                ++it;
            }
        }
    }

    // A closing peer that never acknowledges the end of the connection is cut off.
    void housekeeping()
    {
        BusyScope scope(this);
        std::vector<QPointer<QTcpSocket>> stuck;
        for (const std::unique_ptr<Peer>& peer : peers) {
            if (peer->closing && peer->socket && (peer->socket->state() != QAbstractSocket::UnconnectedState) && (peer->closingFor.elapsed() > kClosingTimeoutMs)) {
                stuck.push_back(peer->socket);
            }
        }
        for (const QPointer<QTcpSocket>& socket : stuck) {
            if (socket) {
                socket->abort();
            }
        }
        if (!server.isListening() && peers.empty()) {
            housekeepingTimer.stop();
        }
    }

    // Claims each created or renamed node's script name in its group, in order, replacing one
    // already taken by the next free one, so concurrent creations of the same name all succeed.
    std::vector<Serialization::GraphOp> resolveNames(const ProjectPtr& p,
                                                     std::vector<Serialization::GraphOp> ops)
    {
        std::map<std::string, std::set<std::string>> claimed;
        // The group and claimed name of each node named so far in this batch, by node ID: a node
        // created here is not in the project yet, and a node renamed again gives up its claim.
        std::map<std::string, std::pair<std::string, std::string>> named;
        for (Serialization::GraphOp& op : ops) {
            if (Serialization::CreateNodeOp* create = std::get_if<Serialization::CreateNodeOp>(&op)) {
                if (!create->node) {
                    continue;
                }
                std::set<std::string>& taken = claimed[create->parentId];
                const NodeCollectionPtr collection = LiveSession::collectionForParent(p, create->parentId);
                const std::string name = LiveSession::uniqueScriptName(collection, create->node->scriptName, nullptr, taken);
                if (name != create->node->scriptName) {
                    Serialization::NodeSerializationPtr copy = Serialization::OpsDetail::deepCopyNode(*create->node);
                    if (copy->label == copy->scriptName) {
                        copy->label = name;
                    }
                    copy->scriptName = name;
                    create->node = copy;
                }
                taken.insert(name);
                named[create->node->id] = std::make_pair(create->parentId, name);
            } else if (Serialization::RenameOp* rename = std::get_if<Serialization::RenameOp>(&op)) {
                NodePtr node = p->findNodeById(rename->nodeId);
                std::map<std::string, std::pair<std::string, std::string>>::iterator earlier = named.find(rename->nodeId);
                std::string parentId;
                NodeCollectionPtr collection;
                if (node) {
                    parentId = LiveSession::parentIdOf(node);
                    collection = node->getGroup();
                } else if (earlier != named.end()) {
                    parentId = earlier->second.first;
                    collection = LiveSession::collectionForParent(p, parentId);
                } else {
                    continue;
                }
                std::set<std::string>& taken = claimed[parentId];
                if (earlier != named.end()) {
                    taken.erase(earlier->second.second);
                }
                const std::string name = LiveSession::uniqueScriptName(collection, rename->scriptName, node.get(), taken);
                if (name != rename->scriptName) {
                    if (rename->label && (*rename->label == rename->scriptName)) {
                        rename->label = name;
                    }
                    rename->scriptName = name;
                }
                taken.insert(name);
                named[rename->nodeId] = std::make_pair(parentId, name);
            }
        }

        return ops;
    }

    ResyncNodeMessage resyncNode(const ProjectPtr& p,
                                 const std::string& nodeId)
    {
        ResyncNodeMessage message;
        message.nodeId = nodeId;
        NodePtr node = p->findNodeById(nodeId);
        if (!node || !node->isActivated()) {
            message.deleted = true;
        } else {
            message.ops = LiveSession::nodeStateOps(node);
        }

        return message;
    }

    void applyClientOps(Peer* peer,
                        const OpsMessage& message)
    {
        ProjectPtr p = project();
        if (!p) {
            return;
        }
        flushLocalEdits();
        const std::vector<Serialization::GraphOp> resolved = resolveNames(p, message.ops);
        std::vector<Serialization::OpResult> results;
        {
            LiveSession::RemoteApply remote;
            GraphEditRecorder::Suppress suppress;
            results = applyGraphOps(p, resolved);
        }

        BroadcastMessage broadcast;
        broadcast.seq = ++seq;
        broadcast.originUser = peer->userId;
        broadcast.clientSeq = message.clientSeq;
        std::string reasons;
        std::set<std::string> resyncTargets;
        bool fullResync = false;
        for (std::size_t i = 0; i < resolved.size(); ++i) {
            if (i < results.size() && results[i].applied()) {
                broadcast.ops.push_back(resolved[i]);
                continue;
            }
            const std::string why = (i < results.size()) ? results[i].message : std::string("not applied");
            reasons += (reasons.empty() ? "" : "; ") + std::string(Serialization::graphOpName(resolved[i])) + ": " + why;
            if (std::optional<std::string> target = LiveSession::graphOpTargetNode(resolved[i])) {
                resyncTargets.insert(*target);
            } else {
                fullResync = true;
            }
        }
        if (!broadcast.ops.empty()) {
            p->markUnsavedChanges();
        }
        sendToJoined(broadcast);
        notifyRemoteEdits(broadcast.ops);
        if (reasons.empty() || peer->closing) {
            return;
        }
        ++rejects;
        send(peer, RejectMessage { message.clientSeq, reasons });
        // Recreating a node others are linked to, or read by expression, would cut those ties.
        for (const std::string& target : resyncTargets) {
            NodePtr node = p->findNodeById(target);
            if (node && node->isActivated() && LiveSession::hasDependents(node)) {
                fullResync = true;
                break;
            }
        }
        if (fullResync) {
            sendWelcome(peer);

            return;
        }
        for (const std::string& target : resyncTargets) {
            send(peer, resyncNode(p, target));
        }
    }

    void notifyRemoteEdits(const std::vector<Serialization::GraphOp>& ops)
    {
        QStringList deleted;
        QStringList edited;
        LiveSession::remoteEditedNodes(ops, &deleted, &edited);
        if (!deleted.isEmpty() || !edited.isEmpty()) {
            Q_EMIT publicInterface->remoteNodesEdited(deleted, edited);
        }
    }

    PresenceMessage presenceOf(const Peer* peer,
                               const PresenceMessage& sent) const
    {
        PresenceMessage entry = sent;
        entry.userId = peer->userId;
        entry.name = peer->name.toStdString();
        entry.colour = peer->colour;
        entry.host = false;

        return entry;
    }

    void handlePresence(Peer* peer,
                        const PresenceMessage& sent)
    {
        const PresenceMessage entry = presenceOf(peer, sent);
        presence.updateUser(entry);
        sendToJoined(entry, peer);
    }

    // The newcomer learns who is there, and the others learn about it, before any presence of it.
    void introduce(Peer* peer)
    {
        const PresenceMessage entry = presenceOf(peer, PresenceMessage {});
        for (const PresenceMessage& user : presence.users()) {
            if (peer->closing) {
                return;
            }
            send(peer, user);
        }
        presence.updateUser(entry);
        sendToJoined(entry, peer);
    }

    void handleSaveRequest(Peer* peer)
    {
        AppInstancePtr instance = app.lock();
        ProjectPtr p = project();
        const QString name = peer->name;
        const quint32 userId = peer->userId;
        if (!instance || !p) {
            return;
        }
        if (!p->hasProjectBeenSavedByUser()) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 asked to save, but the project has no file yet: save it on the host first.").arg(name));
        } else if (!p->isSaveUpToDate()) {
            try {
                if (!instance->save(std::string())) {
                    Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("Saving for %1 failed.").arg(name));
                }
            } catch (const std::exception& e) {
                Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("Saving for %1 failed: %2").arg(name, QString::fromUtf8(e.what())));
            }
        }
        Q_EMIT publicInterface->saveRequested(userId);
    }

    void handleHello(Peer* peer,
                     const HelloMessage& hello)
    {
        if (hello.protocol != kProtocolVersion) {
            drop(peer, QString::fromUtf8("This session speaks live session protocol %1 (Natron %2) and the joining Natron %3 speaks protocol %4: join with the host's Natron version.").arg(kProtocolVersion).arg(QString::fromUtf8(NATRON_VERSION_STRING), QString::fromUtf8(hello.natronVersion.c_str())).arg(hello.protocol));

            return;
        }
        peer->userId = nextUserId++;
        peer->name = QString::fromUtf8(hello.name.c_str());
        peer->colour = kPeerColours[(peer->userId - 1) % (sizeof(kPeerColours) / sizeof(kPeerColours[0]))];
        peer->decoder.setMaxFrameBytes(kMaxFrameBytes);
        sendWelcome(peer);
        if (peer->closing) {
            return;
        }
        peer->joined = true;
        introduce(peer);
        if (peer->closing) {
            return;
        }
        Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 joined the session.").arg(peer->name));
        if (peer->closing) {
            return;
        }
        Q_EMIT publicInterface->peerCountChanged(joinedCount());
    }

    void handle(Peer* peer,
                const SessionMessage& message)
    {
        if (const HelloMessage* hello = std::get_if<HelloMessage>(&message)) {
            if (peer->joined || peer->userId != 0) {
                drop(peer, QString::fromUtf8("Protocol error: a second hello."));
            } else {
                handleHello(peer, *hello);
            }

            return;
        }
        if (std::holds_alternative<ByeMessage>(message)) {
            drop(peer, QString());

            return;
        }
        if (!peer->joined) {
            drop(peer, QString::fromUtf8("Protocol error: %1 before hello.").arg(QString::fromUtf8(std::string(sessionMessageName(message)).c_str())));

            return;
        }
        if (const OpsMessage* ops = std::get_if<OpsMessage>(&message)) {
            applyClientOps(peer, *ops);
        } else if (std::holds_alternative<ResyncRequestMessage>(message)) {
            peer->wantsResync = true;
        } else if (std::holds_alternative<SaveRequestMessage>(message)) {
            peer->wantsSave = true;
        } else if (const PresenceMessage* sent = std::get_if<PresenceMessage>(&message)) {
            handlePresence(peer, *sent);
        } else {
            drop(peer, QString::fromUtf8("Protocol error: unexpected %1 message.").arg(QString::fromUtf8(std::string(sessionMessageName(message)).c_str())));
        }
    }

    // Requests repeated while the host was busy are answered once, after the edits sent with
    // them.
    void serveRequests(Peer* peer)
    {
        if (peer->wantsResync && !peer->closing) {
            peer->wantsResync = false;
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 diverged and is reloading the project.").arg(peer->name));
            sendWelcome(peer);
        }
        if (peer->wantsSave && !peer->closing) {
            peer->wantsSave = false;
            handleSaveRequest(peer);
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
        BusyScope scope(this);
        handling = true;
        do {
            readAgain = false;
            // By index: a slot of a signal emitted below may stop the session, which empties the
            // list but keeps its peers alive until this returns.
            for (std::size_t i = 0; i < peers.size(); ++i) {
                Peer* peer = peers[i].get();
                if (peer->closing || !peer->socket) {
                    continue;
                }
                const QByteArray bytes = peer->socket->readAll();
                if (!bytes.isEmpty()) {
                    peer->decoder.feed(bytes.constData(), static_cast<std::size_t>(bytes.size()));
                }
                SessionMessage message;
                for (;;) {
                    const SessionFrameDecoder::Status status = peer->decoder.next(&message);
                    if (status == SessionFrameDecoder::Status::eNeedMore) {
                        break;
                    }
                    if (status == SessionFrameDecoder::Status::eError) {
                        drop(peer, QString::fromUtf8(peer->decoder.error().c_str()));
                        break;
                    }
                    handle(peer, message);
                    if (peer->closing) {
                        break;
                    }
                }
                serveRequests(peer);
            }
        } while (readAgain);
        handling = false;
    }

    void onDisconnected(QTcpSocket* socket)
    {
        BusyScope scope(this);
        for (std::size_t i = 0; i < peers.size(); ++i) {
            if (peers[i]->socket == socket) {
                drop(peers[i].get(), QString());
            }
        }
    }

    void onNewConnection()
    {
        BusyScope scope(this);
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            if (!socket) {
                break;
            }
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            std::unique_ptr<Peer> peer = std::make_unique<Peer>();
            peer->socket = socket;
            // Anyone can connect: until a peer has said hello it may send no more than one.
            peer->decoder.setMaxFrameBytes(kMaxFrameBytesBeforeHello);
            peers.push_back(std::move(peer));
            QObject::connect(socket, &QTcpSocket::readyRead, publicInterface, [this]() { pump(); });
            QObject::connect(socket, &QTcpSocket::disconnected, publicInterface, [this, socket]() { onDisconnected(socket); });
        }
    }

    // Nothing changed since the last checksum, so the clients still agree with it.
    void onChecksumTimer()
    {
        BusyScope scope(this);
        flushLocalEdits();
        if (seq != lastChecksumSeq) {
            publicInterface->broadcastChecksum();
        }
    }
};

SessionHost::SessionHost(const AppInstancePtr& app,
                         QObject* parent)
    : QObject(parent)
    , _imp(new Implementation(this, app))
{
    _imp->checksumTimer.setInterval(5000);
    _imp->housekeepingTimer.setInterval(kHousekeepingIntervalMs);
    QObject::connect(&_imp->checksumTimer, &QTimer::timeout, this, [this]() { _imp->onChecksumTimer(); });
    QObject::connect(&_imp->housekeepingTimer, &QTimer::timeout, this, [this]() { _imp->housekeeping(); });
    QObject::connect(&_imp->server, &QTcpServer::newConnection, this, [this]() { _imp->onNewConnection(); });
}

SessionHost::~SessionHost()
{
    _imp->deleteWhenIdle = false;
    stop();
    _imp->releaseRetired();
}

bool
SessionHost::start(const QString& bindAddress,
                   quint16 port,
                   const QString& displayName,
                   QString* error)
{
    if (_imp->server.isListening()) {
        if (error) {
            *error = tr("A session is already being hosted.");
        }

        return false;
    }
    ProjectPtr project = _imp->project();
    if (!project || !project->getGraphEditRecorder()) {
        if (error) {
            *error = tr("There is no project to share.");
        }

        return false;
    }
    QHostAddress address(QHostAddress::Any);
    if (!bindAddress.isEmpty() && !address.setAddress(bindAddress)) {
        if (error) {
            *error = tr("%1 is not an IP address.").arg(bindAddress);
        }

        return false;
    }
    if (!_imp->server.listen(address, port)) {
        if (error) {
            *error = tr("Cannot listen on %1 port %2: %3").arg(address.toString()).arg(port).arg(_imp->server.errorString());
        }

        return false;
    }
    _imp->displayName = displayName;
    _imp->seq = 0;
    _imp->lastChecksumSeq = std::numeric_limits<std::uint64_t>::max();
    _imp->rejects = 0;
    // Edits made before the session started are in the snapshot, not in the sequence.
    project->getGraphEditRecorder()->flush();
    _imp->subscription = project->getGraphEditRecorder()->subscribe([this](const std::vector<Serialization::GraphOp>& ops) {
        _imp->onLocalOps(ops);
    });
    _imp->subscribedProject = project;
    _imp->presence.setSender([this](const PresenceMessage& entry) {
        Implementation::BusyScope scope(_imp.get());
        _imp->sendToJoined(entry);
    });
    _imp->presence.setLocalUser(0, displayName.toStdString(), kHostColour, true);
    if (_imp->checksumTimer.interval() > 0) {
        _imp->checksumTimer.start();
    }
    _imp->housekeepingTimer.start();

    return true;
}

void
SessionHost::stop()
{
    if (!_imp->server.isListening() && _imp->peers.empty()) {
        return;
    }
    Implementation::BusyScope scope(_imp.get());
    _imp->checksumTimer.stop();
    ProjectPtr project = _imp->subscribedProject.lock();
    if (_imp->subscription >= 0) {
        if (project && project->getGraphEditRecorder()) {
            _imp->flushLocalEdits();
            project->getGraphEditRecorder()->unsubscribe(_imp->subscription);
        }
        _imp->subscription = -1;
    }
    _imp->server.close();
    // The peers leave the list at once, so that the calls running keep them but see none, and are
    // released once those calls return.
    const std::size_t first = _imp->retired.size();
    for (std::unique_ptr<Peer>& peer : _imp->peers) {
        _imp->retired.push_back(std::move(peer));
    }
    _imp->peers.clear();
    for (std::size_t i = first; i < _imp->retired.size(); ++i) {
        Peer* peer = _imp->retired[i].get();
        if (peer->socket) {
            peer->socket->disconnect(this);
        }
        _imp->drop(peer, tr("The host closed the session."));
    }
    _imp->presence.clear();
    Q_EMIT peerCountChanged(0);
}

void
SessionHost::scheduleDeletion()
{
    if (_imp->busy > 0) {
        _imp->deleteWhenIdle = true;
    } else {
        deleteLater();
    }
}

bool
SessionHost::isRunning() const
{
    return _imp->server.isListening();
}

quint16
SessionHost::serverPort() const
{
    return _imp->server.serverPort();
}

int
SessionHost::peerCount() const
{
    return _imp->joinedCount();
}

std::uint64_t
SessionHost::seq() const
{
    return _imp->seq;
}

void
SessionHost::setChecksumInterval(int milliseconds)
{
    _imp->checksumTimer.stop();
    _imp->checksumTimer.setInterval(milliseconds);
    if ((milliseconds > 0) && _imp->server.isListening()) {
        _imp->checksumTimer.start();
    }
}

void
SessionHost::broadcastChecksum()
{
    ProjectPtr project = _imp->project();
    if (!project || (_imp->joinedCount() == 0)) {
        return;
    }
    Implementation::BusyScope scope(_imp.get());
    _imp->flushLocalEdits();
    ChecksumMessage checksum;
    checksum.seq = _imp->seq;
    try {
        checksum.hash = LiveSession::projectChecksum(project);
    } catch (const std::exception& e) {
        Q_EMIT sessionMessage(tr("The project checksum could not be computed: %1").arg(QString::fromUtf8(e.what())));

        return;
    }
    _imp->lastChecksumSeq = checksum.seq;
    _imp->sendToJoined(checksum);
}

void
SessionHost::flushLocalEdits()
{
    Implementation::BusyScope scope(_imp.get());
    _imp->flushLocalEdits();
}

bool
SessionHost::isIdle() const
{
    for (const std::unique_ptr<Peer>& peer : _imp->peers) {
        if (peer->closing || !peer->socket) {
            continue;
        }
        if ((peer->socket->bytesToWrite() > 0) || (peer->socket->bytesAvailable() > 0) || (peer->decoder.bufferedBytes() > 0)) {
            return false;
        }
    }

    return true;
}

int
SessionHost::rejectCount() const
{
    return _imp->rejects;
}

SessionPresence*
SessionHost::presence() const
{
    return &_imp->presence;
}

NATRON_NAMESPACE_EXIT
