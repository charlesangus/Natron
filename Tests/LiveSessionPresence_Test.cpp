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

#include <Python.h>

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTcpSocket>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/LiveSession/SessionClient.h"
#include "Engine/LiveSession/SessionHost.h"
#include "Engine/LiveSession/SessionPresence.h"
#include "Engine/Project.h"

#include "Serialization/Session/SessionProtocol.h"

NATRON_NAMESPACE_USING

namespace {

namespace Session = Serialization::Session;

// Background apps all have the Python name app, so client B is a child Tests process with its own
// app, driven over its standard input; replies are its "@ " lines. Client A speaks the protocol
// from this process, so that the time its presence leaves is known exactly.
const char* const kPortEnv = "NATRON_TEST_LIVE_SESSION_PRESENCE_PORT";
const char* const kPeerSuite = "LiveSessionPresencePeerProcess";
const int kTimeoutMs = 120000;
const qint64 kPresenceLatencyMs = 200;

bool
runUntil(const std::function<bool()>& done,
         int timeoutMs = kTimeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }

    return true;
}

// The monotonic clock is the same for every process on the machine, so a time taken here can be
// compared with one taken in the child.
qint64
monotonicMs()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return qint64(now.tv_sec) * 1000 + qint64(now.tv_nsec / 1000000);
}

void
replyToParent(const std::string& line)
{
    // A leading newline keeps the reply off the end of a line something else printed.
    std::cout << "\n@ " << line << std::endl;
}

bool
selects(const Session::PresenceMessage& entry,
        const std::string& nodeId)
{
    return std::find(entry.selection.begin(), entry.selection.end(), nodeId) != entry.selection.end();
}

std::string
peerCommand(const AppInstancePtr& app,
            const std::string& line,
            bool* quit)
{
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    SessionPresence* presence = app->getLiveSessionPresence();
    if (verb == "quit") {
        *quit = true;

        return "bye";
    }
    if (!presence) {
        return "no session";
    }
    if (verb == "users") {
        std::size_t count = 0;
        in >> count;
        // The host introduces everyone right after its welcome, which B may still be reading.
        runUntil([presence, count]() { return presence->users().size() >= count; }, 30000);
        std::ostringstream out;
        out << "users";
        for (const Session::PresenceMessage& user : presence->users()) {
            out << ' ' << user.userId << ':' << user.name << ':' << int(user.host);
        }

        return out.str();
    }
    if (verb == "select") {
        std::vector<std::string> ids;
        std::string id;
        while (in >> id) {
            ids.push_back(id);
        }
        presence->setLocalSelection(ids);
        presence->flush();

        return "ok";
    }
    quint32 userId = 0;
    in >> userId;
    if (verb == "waitselect") {
        std::string nodeId;
        qint64 sentAt = 0;
        in >> nodeId >> sentAt;
        const bool seen = runUntil([presence, userId, &nodeId]() {
            const Session::PresenceMessage* user = presence->user(userId);

            return user && selects(*user, nodeId);
        },
                                   30000);
        if (!seen) {
            return "timeout";
        }

        return "seen " + std::to_string(monotonicMs() - sentAt);
    }
    if (verb == "waitgone") {
        const bool gone = runUntil([presence, userId]() { return presence->user(userId) == nullptr; }, 30000);

        return gone ? "gone" : "timeout";
    }

    return "unknown";
}

class Peer {
public:
    explicit Peer(quint16 port)
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromUtf8(kPortEnv), QString::number(port));
        _process.setProcessEnvironment(environment);
        _process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        _process.start(QCoreApplication::applicationFilePath(),
                       QStringList() << QString::fromUtf8("--gtest_filter=%1.*").arg(QString::fromUtf8(kPeerSuite)));
        _started = _process.waitForStarted(30000);
    }

    ~Peer()
    {
        if (_process.state() != QProcess::NotRunning) {
            _process.kill();
            _process.waitForFinished(30000);
        }
    }

    bool started() const
    {
        return _started;
    }

    // The next reply, or "dead" or "timeout". This process's events run meanwhile, so the host
    // keeps serving the session.
    std::string reply()
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            while (_process.canReadLine()) {
                const std::string line = QString::fromUtf8(_process.readLine()).trimmed().toStdString();
                if (line.compare(0, 2, "@ ") == 0) {
                    return line.substr(2);
                }
                if (!line.empty()) {
                    _output += line + "\n";
                    if (_output.size() > 16384) {
                        _output.erase(0, _output.size() - 16384);
                    }
                }
            }
            if (_process.state() == QProcess::NotRunning) {
                return "dead";
            }
            if (timer.elapsed() > kTimeoutMs) {
                return "timeout";
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
    }

    std::string command(const std::string& line)
    {
        const std::string withEnd = line + "\n";
        _process.write(withEnd.data(), static_cast<qint64>(withEnd.size()));
        _process.waitForBytesWritten(30000);

        return reply();
    }

    // True when the child ended cleanly, its own expectations included.
    bool finish()
    {
        if (_process.state() != QProcess::NotRunning) {
            (void)command("quit");
            _process.closeWriteChannel();
        }
        if (!_process.waitForFinished(60000)) {
            _process.kill();
            _process.waitForFinished(30000);

            return false;
        }

        return (_process.exitStatus() == QProcess::NormalExit) && (_process.exitCode() == 0);
    }

    const std::string& output() const
    {
        return _output;
    }

private:
    QProcess _process;
    bool _started = false;
    std::string _output;
};

// One end of a live session connection speaking the protocol directly.
class RawClient {
public:
    RawClient()
    {
        _readyRead = QObject::connect(&_socket, &QTcpSocket::readyRead, [this]() { read(); });
        _closed = QObject::connect(&_socket, &QTcpSocket::disconnected, [this]() {
            read();
            _disconnected = true;
        });
    }

    ~RawClient()
    {
        QObject::disconnect(_readyRead);
        QObject::disconnect(_closed);
        _socket.abort();
    }

    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    bool join(quint16 port,
              const char* name)
    {
        _socket.connectToHost(QString::fromUtf8("127.0.0.1"), port);
        if (!runUntil([this]() { return _socket.state() == QAbstractSocket::ConnectedState; }, 30000)) {
            return false;
        }
        Session::HelloMessage hello;
        hello.name = name;
        hello.natronVersion = NATRON_VERSION_STRING;
        send(hello);

        return runUntil([this]() { return welcome() || _disconnected; }, 30000) && welcome();
    }

    void send(const Session::SessionMessage& message)
    {
        const std::vector<std::byte> frame = Session::frameSessionMessage(message);
        _socket.write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size()));
        _socket.flush();
    }

    void disconnectFromHost()
    {
        _socket.disconnectFromHost();
    }

    const Session::WelcomeMessage* welcome() const
    {
        for (const Session::SessionMessage& message : _messages) {
            if (const Session::WelcomeMessage* found = std::get_if<Session::WelcomeMessage>(&message)) {
                return found;
            }
        }

        return nullptr;
    }

    // The last presence received for userId.
    std::optional<Session::PresenceMessage> presenceOf(quint32 userId) const
    {
        std::optional<Session::PresenceMessage> last;
        for (const Session::SessionMessage& message : _messages) {
            if (const Session::PresenceMessage* entry = std::get_if<Session::PresenceMessage>(&message)) {
                if (entry->userId == userId) {
                    last = *entry;
                }
            }
        }

        return last;
    }

    bool unreadable() const
    {
        return _unreadable;
    }

private:
    void read()
    {
        const QByteArray bytes = _socket.readAll();
        _decoder.feed(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        Session::SessionMessage message;
        for (;;) {
            const Session::SessionFrameDecoder::Status status = _decoder.next(&message);
            if (status == Session::SessionFrameDecoder::Status::eError) {
                _unreadable = true;
            }
            if (status != Session::SessionFrameDecoder::Status::eMessage) {
                break;
            }
            _messages.push_back(message);
        }
    }

    QTcpSocket _socket;
    QMetaObject::Connection _readyRead;
    QMetaObject::Connection _closed;
    Session::SessionFrameDecoder _decoder;
    std::vector<Session::SessionMessage> _messages;
    bool _disconnected = false;
    bool _unreadable = false;
};

} // namespace

// Runs only in a child started by the tests below, as client B of their host.
class LiveSessionPresencePeerProcess
    : public BaseTest {
};

TEST_F(LiveSessionPresencePeerProcess, ServesTheTestThatStartedIt)
{
    const char* port = std::getenv(kPortEnv);
    if (!port) {
        return;
    }
    AppInstancePtr app = getApp();
    ASSERT_NE(app, nullptr);

    QString endReason;
    QObject::connect(app.get(), &AppInstance::liveSessionEnded, [&endReason](const QString& reason) { endReason = reason; });
    app->joinLiveSession(QString::fromUtf8("127.0.0.1"), static_cast<quint16>(std::atoi(port)), QString::fromUtf8("Bea"));
    const bool joined = runUntil([&app]() { return app->getLiveSessionState() != AppInstance::LiveSessionState::eConnecting; })
        && (app->getLiveSessionState() == AppInstance::LiveSessionState::eJoined);
    if (!joined) {
        replyToParent("left " + endReason.toStdString());
        FAIL() << "could not join: " << endReason.toStdString();
    }
    replyToParent("joined " + std::to_string(app->getLiveSessionClient()->userId()));

    bool quit = false;
    std::string input;
    while (!quit) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        pollfd stdinPoll;
        stdinPoll.fd = STDIN_FILENO;
        stdinPoll.events = POLLIN;
        stdinPoll.revents = 0;
        if ((poll(&stdinPoll, 1, 1) > 0) && (stdinPoll.revents & (POLLIN | POLLHUP))) {
            char buffer[4096];
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            input.append(buffer, static_cast<std::size_t>(count));
        }
        std::string::size_type end;
        while (!quit && ((end = input.find('\n')) != std::string::npos)) {
            const std::string line = input.substr(0, end);
            input.erase(0, end + 1);
            replyToParent(peerCommand(app, line, &quit));
        }
    }
    app->leaveLiveSession();
}

class LiveSessionPresenceTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        _app = getApp();
        ASSERT_NE(_app, nullptr);
    }

    void TearDown() override
    {
        if (_peer) {
            EXPECT_TRUE(_peer->finish()) << "client B failed: " << _peer->output();
            _peer.reset();
        }
        if (_app) {
            _app->leaveLiveSession();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            _app->getProject()->reset(false, true);
        }
        BaseTest::TearDown();
    }

    quint16 host()
    {
        QString error;
        EXPECT_TRUE(_app->hostLiveSession(QString::fromUtf8("127.0.0.1"), 0, QString::fromUtf8("Hal"), &error)) << error.toStdString();

        return _app->getLiveSessionHost() ? _app->getLiveSessionHost()->serverPort() : 0;
    }

    AppInstancePtr _app;
    std::unique_ptr<Peer> _peer;
};

TEST_F(LiveSessionPresenceTest, TheLocalEntryIsSentAtMostTenTimesASecond)
{
    SessionPresence presence;
    std::vector<Session::PresenceMessage> sent;
    presence.setSender([&sent](const Session::PresenceMessage& entry) { sent.push_back(entry); });
    presence.setLocalSelection({ "early" });
    EXPECT_TRUE(sent.empty()) << "an entry went out before this user was part of the session";

    presence.setLocalUser(3, "Ann", Serialization::NodeUiColor { 0.1f, 0.2f, 0.3f }, false);
    ASSERT_EQ(sent.size(), std::size_t(1));
    EXPECT_EQ(sent.back().selection, std::vector<std::string>({ "early" }));

    QElapsedTimer timer;
    timer.start();
    presence.setLocalSelection({ "a" });
    presence.setLocalSelection({ "b" });
    presence.setLocalSelection({ "c" });
    EXPECT_EQ(sent.size(), std::size_t(1)) << "changes within the interval were not held back";
    ASSERT_TRUE(runUntil([&sent]() { return sent.size() == 2; }, 5000));
    EXPECT_GE(timer.elapsed() + 5, qint64(SessionPresence::kDefaultThrottleMs));
    EXPECT_EQ(sent.back().selection, std::vector<std::string>({ "c" }));

    presence.setLocalSelection({ "c" });
    runUntil([]() { return false; }, 300);
    EXPECT_EQ(sent.size(), std::size_t(2)) << "an unchanged entry was sent again";

    presence.setLocalOpenPanels({ "c" });
    presence.flush();
    EXPECT_EQ(sent.size(), std::size_t(3));
    EXPECT_EQ(sent.back().openPanels, std::vector<std::string>({ "c" }));

    presence.clear();
    EXPECT_TRUE(presence.users().empty());
    presence.setLocalSelection({ "d" });
    presence.flush();
    EXPECT_EQ(sent.size(), std::size_t(3)) << "an entry went out after the session ended";
}

TEST_F(LiveSessionPresenceTest, ASelectionReachesTheOtherClientQuicklyAndLeavesWithItsUser)
{
    const quint16 port = host();
    ASSERT_NE(port, 0);

    RawClient a;
    ASSERT_TRUE(a.join(port, "Ann"));
    const quint32 aId = a.welcome()->userId;

    _peer = std::make_unique<Peer>(port);
    ASSERT_TRUE(_peer->started());
    const std::string joined = _peer->reply();
    ASSERT_EQ(joined.compare(0, 7, "joined "), 0) << joined;
    const quint32 bId = static_cast<quint32>(std::stoul(joined.substr(7)));
    EXPECT_NE(aId, bId);

    // Everyone is in every table, as the host names and colours them.
    std::ostringstream expectedUsers;
    expectedUsers << "users 0:Hal:1 " << aId << ":Ann:0 " << bId << ":Bea:0";
    EXPECT_EQ(_peer->command("users 3"), expectedUsers.str());
    ASSERT_TRUE(runUntil([&a, bId]() { return a.presenceOf(0) && a.presenceOf(bId); }, 30000));
    EXPECT_TRUE(a.presenceOf(0)->host);
    EXPECT_EQ(a.presenceOf(0)->name, "Hal");
    EXPECT_EQ(a.presenceOf(bId)->name, "Bea");
    EXPECT_FALSE(a.presenceOf(bId)->colour == a.welcome()->colour) << "two users share a colour";

    // A claims another identity; the host keeps the one it gave.
    Session::PresenceMessage selecting;
    selecting.userId = 0;
    selecting.name = "Mallory";
    selecting.host = true;
    selecting.selection = { "node-7" };
    const qint64 sentAt = monotonicMs();
    a.send(selecting);
    const std::string seen = _peer->command("waitselect " + std::to_string(aId) + " node-7 " + std::to_string(sentAt));
    ASSERT_EQ(seen.compare(0, 5, "seen "), 0) << seen;
    EXPECT_LT(std::stoll(seen.substr(5)), kPresenceLatencyMs) << "the selection took too long to reach client B";

    SessionPresence* hostPresence = _app->getLiveSessionPresence();
    ASSERT_NE(hostPresence, nullptr);
    const Session::PresenceMessage* onHost = hostPresence->user(aId);
    ASSERT_NE(onHost, nullptr);
    EXPECT_EQ(onHost->name, "Ann");
    EXPECT_FALSE(onHost->host);
    EXPECT_EQ(onHost->colour, a.welcome()->colour);
    EXPECT_EQ(onHost->selection, std::vector<std::string>({ "node-7" }));
    EXPECT_TRUE(hostPresence->user(0)->host);
    EXPECT_EQ(_peer->command("users 3"), expectedUsers.str());

    // B's own selection reaches A, and A's entry is not sent back to it.
    EXPECT_EQ(_peer->command("select node-3"), "ok");
    ASSERT_TRUE(runUntil([&a, bId]() { return a.presenceOf(bId) && selects(*a.presenceOf(bId), "node-3"); }, 30000));
    EXPECT_FALSE(a.presenceOf(aId).has_value());
    EXPECT_FALSE(a.unreadable());

    a.disconnectFromHost();
    EXPECT_EQ(_peer->command("waitgone " + std::to_string(aId)), "gone");
    EXPECT_TRUE(runUntil([hostPresence, aId]() { return hostPresence->user(aId) == nullptr; }, 30000));
    std::ostringstream remaining;
    remaining << "users 0:Hal:1 " << bId << ":Bea:0";
    EXPECT_EQ(_peer->command("users 2"), remaining.str());
}

TEST_F(LiveSessionPresenceTest, LeavingTheSessionEmptiesTheTable)
{
    const quint16 port = host();
    ASSERT_NE(port, 0);
    RawClient a;
    ASSERT_TRUE(a.join(port, "Ann"));
    SessionPresence* hostPresence = _app->getLiveSessionPresence();
    ASSERT_NE(hostPresence, nullptr);
    ASSERT_TRUE(runUntil([hostPresence]() { return hostPresence->users().size() == 2; }, 30000));

    _app->leaveLiveSession();
    // The host is deleted once events run again, so its table is read before.
    EXPECT_TRUE(hostPresence->users().empty());
    EXPECT_EQ(_app->getLiveSessionPresence(), nullptr);
}
