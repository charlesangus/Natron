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

#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "Engine/AppInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/GraphOpApply.h"
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

struct Peer {
    QPointer<QTcpSocket> socket;
    SessionFrameDecoder decoder;
    quint32 userId = 0;
    QString name;
    NodeUiColor colour;
    bool joined = false;
    bool closing = false;
};

} // namespace

struct SessionHost::Implementation {
    SessionHost* publicInterface;
    AppInstanceWPtr app;
    QTcpServer server;
    QTimer checksumTimer;
    std::vector<std::unique_ptr<Peer>> peers;
    std::uint64_t seq = 0;
    quint32 nextUserId = 1;
    QString displayName;
    int subscription = -1;
    bool handling = false;
    bool readAgain = false;
    int rejects = 0;

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

    void send(Peer* peer,
              const SessionMessage& message)
    {
        if (!peer->socket || peer->closing) {
            return;
        }
        try {
            const std::vector<std::byte> frame = frameSessionMessage(message);
            peer->socket->write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size()));
        } catch (const SessionProtocolError& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8(e.what()));
        }
    }

    void sendToJoined(const SessionMessage& message)
    {
        std::vector<std::byte> frame;
        try {
            frame = frameSessionMessage(message);
        } catch (const SessionProtocolError& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8(e.what()));

            return;
        }
        for (const std::unique_ptr<Peer>& peer : peers) {
            if (peer->joined && !peer->closing && peer->socket) {
                peer->socket->write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size()));
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
        BroadcastMessage broadcast;
        broadcast.seq = ++seq;
        broadcast.originUser = 0;
        broadcast.clientSeq = 0;
        broadcast.ops = ops;
        sendToJoined(broadcast);
    }

    void sendWelcome(Peer* peer)
    {
        ProjectPtr p = project();
        if (!p) {
            return;
        }
        flushLocalEdits();
        WelcomeMessage welcome;
        welcome.userId = peer->userId;
        welcome.colour = peer->colour;
        welcome.seq = seq;
        welcome.projectPath = p->getProjectPath().toStdString();
        welcome.projectName = p->getProjectFilename().toStdString();
        try {
            welcome.snapshot = LiveSession::projectBundleBytes(p);
        } catch (const std::exception& e) {
            drop(peer, QString::fromUtf8("The project could not be sent: %1").arg(QString::fromUtf8(e.what())));

            return;
        }
        send(peer, welcome);
    }

    void drop(Peer* peer,
              const QString& reason)
    {
        if (peer->closing) {
            return;
        }
        if (!reason.isEmpty()) {
            send(peer, ByeMessage { reason.toStdString() });
        }
        const bool wasJoined = peer->joined;
        peer->closing = true;
        peer->joined = false;
        if (peer->socket) {
            peer->socket->disconnectFromHost();
        }
        if (wasJoined) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 left the session.").arg(peer->name));
            Q_EMIT publicInterface->peerCountChanged(joinedCount());
        }
    }

    void removeClosedPeers()
    {
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

    // Claims each created or renamed node's script name in its group, in order, replacing one
    // already taken by the next free one, so concurrent creations of the same name all succeed.
    std::vector<Serialization::GraphOp> resolveNames(const ProjectPtr& p,
                                                     std::vector<Serialization::GraphOp> ops)
    {
        std::map<std::string, std::set<std::string>> claimed;
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
            } else if (Serialization::RenameOp* rename = std::get_if<Serialization::RenameOp>(&op)) {
                NodePtr node = p->findNodeById(rename->nodeId);
                if (!node) {
                    continue;
                }
                std::set<std::string>& taken = claimed[LiveSession::parentIdOf(node)];
                const std::string name = LiveSession::uniqueScriptName(node->getGroup(), rename->scriptName, node.get(), taken);
                if (name != rename->scriptName) {
                    if (rename->label && (*rename->label == rename->scriptName)) {
                        rename->label = name;
                    }
                    rename->scriptName = name;
                }
                taken.insert(name);
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
        sendToJoined(broadcast);
        if (reasons.empty()) {
            return;
        }
        ++rejects;
        send(peer, RejectMessage { message.clientSeq, reasons });
        if (fullResync) {
            sendWelcome(peer);

            return;
        }
        for (const std::string& target : resyncTargets) {
            send(peer, resyncNode(p, target));
        }
    }

    void handleSaveRequest(Peer* peer)
    {
        ProjectPtr p = project();
        Q_EMIT publicInterface->saveRequested(peer->userId);
        if (!p) {
            return;
        }
        if (!p->hasProjectBeenSavedByUser()) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 asked to save, but the project has no file yet: save it on the host first.").arg(peer->name));

            return;
        }
        try {
            p->saveProject(p->getProjectPath(), p->getProjectFilename(), nullptr);
        } catch (const std::exception& e) {
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("Saving for %1 failed: %2").arg(peer->name, QString::fromUtf8(e.what())));
        }
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
        sendWelcome(peer);
        if (peer->closing) {
            return;
        }
        peer->joined = true;
        Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 joined the session.").arg(peer->name));
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
            Q_EMIT publicInterface->sessionMessage(QString::fromUtf8("%1 diverged and is reloading the project.").arg(peer->name));
            sendWelcome(peer);
        } else if (std::holds_alternative<SaveRequestMessage>(message)) {
            handleSaveRequest(peer);
        } else {
            drop(peer, QString::fromUtf8("Protocol error: unexpected %1 message.").arg(QString::fromUtf8(std::string(sessionMessageName(message)).c_str())));
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
            }
        } while (readAgain);
        handling = false;
        removeClosedPeers();
    }

    void onDisconnected(QTcpSocket* socket)
    {
        for (const std::unique_ptr<Peer>& peer : peers) {
            if (peer->socket == socket) {
                drop(peer.get(), QString());
            }
        }
        if (!handling) {
            removeClosedPeers();
        }
    }

    void onNewConnection()
    {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            if (!socket) {
                break;
            }
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            std::unique_ptr<Peer> peer = std::make_unique<Peer>();
            peer->socket = socket;
            peers.push_back(std::move(peer));
            QObject::connect(socket, &QTcpSocket::readyRead, publicInterface, [this]() { pump(); });
            QObject::connect(socket, &QTcpSocket::disconnected, publicInterface, [this, socket]() { onDisconnected(socket); });
        }
    }
};

SessionHost::SessionHost(const AppInstancePtr& app,
                         QObject* parent)
    : QObject(parent)
    , _imp(new Implementation(this, app))
{
    _imp->checksumTimer.setInterval(5000);
    QObject::connect(&_imp->checksumTimer, &QTimer::timeout, this, [this]() { broadcastChecksum(); });
    QObject::connect(&_imp->server, &QTcpServer::newConnection, this, [this]() { _imp->onNewConnection(); });
}

SessionHost::~SessionHost()
{
    stop();
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
    _imp->rejects = 0;
    // Edits made before the session started are in the snapshot, not in the sequence.
    project->getGraphEditRecorder()->flush();
    _imp->subscription = project->getGraphEditRecorder()->subscribe([this](const std::vector<Serialization::GraphOp>& ops) {
        _imp->onLocalOps(ops);
    });
    if (_imp->checksumTimer.interval() > 0) {
        _imp->checksumTimer.start();
    }

    return true;
}

void
SessionHost::stop()
{
    if (!_imp->server.isListening() && _imp->peers.empty()) {
        return;
    }
    _imp->checksumTimer.stop();
    ProjectPtr project = _imp->project();
    if (_imp->subscription >= 0) {
        if (project && project->getGraphEditRecorder()) {
            _imp->flushLocalEdits();
            project->getGraphEditRecorder()->unsubscribe(_imp->subscription);
        }
        _imp->subscription = -1;
    }
    _imp->server.close();
    for (const std::unique_ptr<Peer>& peer : _imp->peers) {
        _imp->drop(peer.get(), tr("The host closed the session."));
        if (peer->socket) {
            peer->socket->flush();
            peer->socket->disconnect(this);
            peer->socket->deleteLater();
            peer->socket = nullptr;
        }
    }
    _imp->peers.clear();
    Q_EMIT peerCountChanged(0);
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
    _imp->flushLocalEdits();
    ChecksumMessage checksum;
    checksum.seq = _imp->seq;
    try {
        checksum.hash = LiveSession::projectChecksum(project);
    } catch (const std::exception& e) {
        Q_EMIT sessionMessage(tr("The project checksum could not be computed: %1").arg(QString::fromUtf8(e.what())));

        return;
    }
    _imp->sendToJoined(checksum);
}

void
SessionHost::flushLocalEdits()
{
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

NATRON_NAMESPACE_EXIT
