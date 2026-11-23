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
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CLArgs.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LiveSession/SessionClient.h"
#include "Engine/LiveSession/SessionHost.h"
#include "Engine/LiveSession/SessionShared.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kHotNodeName = "HotGrade";
const int kClientCount = 3;

NodePtr
createIn(const AppInstancePtr& app,
         const char* pluginID)
{
    CreateNodeArgs args(pluginID, app->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);

    return app->createNode(args);
}

KnobDoubleBasePtr
doubleKnob(const NodePtr& node,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDoubleBase>(node->getKnobByName(name));
}

// The knob an edit of node sets, by its plug-in.
const char*
editableKnob(const NodePtr& node)
{
    const std::string pluginID = node->getPluginID();
    if (pluginID == PLUGINID_NATRON_GRADE) {
        return kGradeParamMultiply;
    }
    if (pluginID == PLUGINID_NATRON_BLUR) {
        return kBlurParamSize;
    }
    if (pluginID == PLUGINID_NATRON_CONSTANT) {
        return kConstantParamColor;
    }

    return nullptr;
}

NodesList
activeNodes(const AppInstancePtr& app)
{
    NodesList nodes;
    app->getProject()->getActiveNodes(&nodes);

    return nodes;
}

NodePtr
pick(const NodesList& nodes,
     std::mt19937& random,
     const std::function<bool(const NodePtr&)>& accept)
{
    std::vector<NodePtr> candidates;
    for (const NodePtr& node : nodes) {
        if (accept(node)) {
            candidates.push_back(node);
        }
    }
    if (candidates.empty()) {
        return NodePtr();
    }

    return candidates[std::uniform_int_distribution<std::size_t>(0, candidates.size() - 1)(random)];
}

bool
isHot(const NodePtr& node)
{
    return node->getScriptName() == kHotNodeName;
}

double
randomValue(std::mt19937& random)
{
    return std::uniform_int_distribution<int>(1, 64)(random) / 8.;
}

// One edit of the kinds users make together: knob sets, a race on one knob, creations,
// deletions, rewiring and renames to names other peers want too.
void
randomEdit(const AppInstancePtr& app,
           std::mt19937& random)
{
    const NodesList nodes = activeNodes(app);
    const int kind = std::uniform_int_distribution<int>(0, 9)(random);
    if (kind <= 2) {
        NodePtr node = pick(nodes, random, [](const NodePtr& n) { return editableKnob(n) != nullptr; });
        if (node) {
            doubleKnob(node, editableKnob(node))->setValue(randomValue(random), ViewSpec::all(), 0);
        }
    } else if (kind <= 4) {
        NodePtr hot = pick(nodes, random, isHot);
        if (hot) {
            doubleKnob(hot, kGradeParamMultiply)->setValue(randomValue(random), ViewSpec::all(), 0);
        }
    } else if (kind == 5) {
        const char* const plugins[] = { PLUGINID_NATRON_BLUR, PLUGINID_NATRON_GRADE, PLUGINID_NATRON_CONSTANT, PLUGINID_NATRON_DOT };
        createIn(app, plugins[std::uniform_int_distribution<int>(0, 3)(random)]);
    } else if (kind == 6) {
        if (nodes.size() > 4) {
            NodePtr node = pick(nodes, random, [](const NodePtr& n) { return !isHot(n); });
            if (node) {
                node->destroyNode(true, false);
            }
        }
    } else if (kind <= 8) {
        NodePtr target = pick(nodes, random, [](const NodePtr& n) { return n->getNInputs() > 0; });
        if (!target) {
            return;
        }
        const int input = std::uniform_int_distribution<int>(0, target->getNInputs() - 1)(random);
        if (std::uniform_int_distribution<int>(0, 4)(random) == 0) {
            target->disconnectInput(input);

            return;
        }
        NodePtr source = pick(nodes, random, [&target, input](const NodePtr& n) {
            return (n != target) && (target->canConnectInput(n, input) == Node::eCanConnectInput_ok);
        });
        if (source) {
            if (target->getRealGuiInput(input)) {
                target->disconnectInput(input);
            }
            NodeCollection::connectNodes(input, source, target, false);
        }
    } else {
        NodePtr node = pick(nodes, random, [](const NodePtr& n) { return !isHot(n); });
        if (node) {
            try {
                node->setScriptName("Shared" + std::to_string(std::uniform_int_distribution<int>(1, 4)(random)));
            } catch (const std::exception&) {
            }
        }
    }
}

void
flushRecorder(const AppInstancePtr& app)
{
    app->getProject()->getGraphEditRecorder()->flush();
}

// Python names every node of a background app app.<name>, whichever app it belongs to, so a
// second app in one process cannot name its copy of the host's nodes. Each client is therefore a
// child Tests process with one app, driven over its standard input.
const char* const kPeerPortEnv = "NATRON_TEST_LIVE_SESSION_PEER_PORT";
const char* const kPeerNameEnv = "NATRON_TEST_LIVE_SESSION_PEER_NAME";
const char* const kPeerSuite = "LiveSessionPeerProcess";
const int kTimeoutMs = 120000;

void
replyToParent(const std::string& line)
{
    // A leading newline keeps the reply off the end of a line something else printed.
    std::cout << "\n@ " << line << std::endl;
}

std::string
peerCommand(const AppInstancePtr& app,
            SessionClient& client,
            bool& held,
            const std::string& line,
            bool* quit)
{
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "state") {
        client.flushLocalEdits();
        std::ostringstream out;
        out << "state " << client.seq() << ' ' << int(client.hasPending()) << ' ' << int(client.isIdle()) << ' '
            << int(client.state() == SessionClient::State::eJoined) << ' ' << client.checksumMismatchCount() << ' '
            << client.rejectCount();

        return out.str();
    }
    if (verb == "run") {
        unsigned long seed = 0;
        int rounds = 0;
        in >> seed >> rounds;
        std::mt19937 random(static_cast<unsigned int>(seed));
        std::mt19937 schedule(static_cast<unsigned int>(seed) + 104729u);
        for (int round = 0; round < rounds; ++round) {
            randomEdit(app, random);
            flushRecorder(app);
            // Peers edit several rounds on what they have before seeing each other's edits.
            const int deliveries = std::uniform_int_distribution<int>(0, 3)(schedule);
            for (int i = 0; (i < deliveries) && !held; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
        }

        return "ran";
    }
    if (verb == "hold") {
        held = true;

        return "ok";
    }
    if (verb == "release") {
        held = false;

        return "ok";
    }
    if (verb == "quit") {
        *quit = true;

        return "bye";
    }
    if (verb == "yaml") {
        std::string path;
        std::getline(in >> std::ws, path);
        QFile file(QString::fromStdString(path));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return "failed";
        }
        const std::string yaml = LiveSession::canonicalProjectYaml(app->getProject());
        const bool written = file.write(yaml.data(), static_cast<qint64>(yaml.size())) == static_cast<qint64>(yaml.size());

        return written ? "ok" : "failed";
    }

    std::string name;
    in >> name;
    if (verb == "named") {
        NodePtr created = createIn(app, PLUGINID_NATRON_GRADE);
        if (!created) {
            return "failed";
        }
        try {
            created->setScriptName(name);
        } catch (const std::exception&) {
            return "failed";
        }
        client.flushLocalEdits();

        return "ok";
    }
    NodePtr node = app->getProject()->getNodeByName(name);
    if (verb == "has") {
        return node ? "yes" : "no";
    }
    if (!node) {
        return "missing";
    }
    if (verb == "set") {
        std::string knobName;
        double value = 0.;
        in >> knobName >> value;
        KnobDoubleBasePtr knob = doubleKnob(node, knobName.c_str());
        if (!knob) {
            return "missing";
        }
        knob->setValue(value, ViewSpec::all(), 0);
    } else if (verb == "mask") {
        std::string inputLabel;
        int on = 0;
        in >> inputLabel >> on;
        KnobBoolPtr maskSwitch = node->getMaskEnabledKnob(node->getInputNumberFromLabel(inputLabel));
        if (!maskSwitch) {
            return "missing";
        }
        maskSwitch->setValue(on != 0);
    } else if (verb == "connect") {
        std::string inputLabel;
        std::string sourceName;
        in >> inputLabel >> sourceName;
        const int inputNb = node->getInputNumberFromLabel(inputLabel);
        NodePtr source = app->getProject()->getNodeByName(sourceName);
        if ((inputNb < 0) || !source) {
            return "missing";
        }
        if (!node->replaceInput(source, inputNb)) {
            return "failed";
        }
    } else if (verb == "destroy") {
        node->destroyNode(true, false);
    } else if (verb == "rename") {
        std::string newName;
        in >> newName;
        try {
            node->setScriptName(newName);
        } catch (const std::exception&) {
            return "failed";
        }
    } else {
        return "unknown";
    }
    client.flushLocalEdits();

    return "ok";
}

struct PeerState {
    bool ok = false;
    std::uint64_t seq = 0;
    bool pending = false;
    bool idle = false;
    bool joined = false;
    int mismatches = 0;
    int rejects = 0;
};

// A client in a child Tests process. Replies are the child's "@ " lines.
class Peer {
public:
    Peer(quint16 port,
         int index)
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromUtf8(kPeerPortEnv), QString::number(port));
        environment.insert(QString::fromUtf8(kPeerNameEnv), QString::fromUtf8("client %1").arg(index));
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

    void send(const std::string& line)
    {
        const std::string withEnd = line + "\n";
        _process.write(withEnd.data(), static_cast<qint64>(withEnd.size()));
        _process.waitForBytesWritten(30000);
    }

    // The next reply, or "dead" or "timeout". While pumping, this process's events run too,
    // so the host keeps serving the session; otherwise the host sees nothing until later.
    std::string reply(bool pump)
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
            if (pump) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            } else {
                _process.waitForReadyRead(100);
            }
        }
    }

    std::string command(const std::string& line)
    {
        send(line);

        return reply(false);
    }

    PeerState state()
    {
        PeerState state;
        std::istringstream in(command("state"));
        std::string word;
        int pending = 0;
        int idle = 0;
        int joined = 0;
        in >> word >> state.seq >> pending >> idle >> joined >> state.mismatches >> state.rejects;
        state.ok = (word == "state") && !in.fail();
        state.pending = pending != 0;
        state.idle = idle != 0;
        state.joined = joined != 0;

        return state;
    }

    void quit()
    {
        if (_process.state() != QProcess::NotRunning) {
            send("quit");
            (void)reply(false);
            _process.closeWriteChannel();
        }
    }

    // True when the child ended cleanly, its own expectations included.
    bool finish()
    {
        if (!_process.waitForFinished(60000)) {
            _process.kill();
            _process.waitForFinished(30000);

            return false;
        }
        (void)reply(false);

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
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    return true;
}

} // namespace

// Runs only in a child started by the tests below, as one client of their host.
class LiveSessionPeerProcess
    : public BaseTest {
};

TEST_F(LiveSessionPeerProcess, ServesTheTestThatStartedIt)
{
    const char* port = std::getenv(kPeerPortEnv);
    if (!port) {
        return;
    }
    const char* name = std::getenv(kPeerNameEnv);
    AppInstancePtr app = getApp();
    ASSERT_NE(app, nullptr);

    std::unique_ptr<SessionClient> client = std::make_unique<SessionClient>(app);
    QString leftReason;
    QObject::connect(client.get(), &SessionClient::left, [&leftReason](const QString& reason) { leftReason = reason; });
    client->connectToHost(QString::fromUtf8("127.0.0.1"), static_cast<quint16>(std::atoi(port)), QString::fromUtf8(name ? name : "client"));
    const bool joined = runUntil([&client]() {
                            return (client->state() == SessionClient::State::eJoined) || (client->state() == SessionClient::State::eDisconnected);
                        })
        && (client->state() == SessionClient::State::eJoined);
    if (!joined) {
        replyToParent("left " + leftReason.toStdString());
        FAIL() << "could not join: " << leftReason.toStdString();
    }
    replyToParent("joined");

    bool held = false;
    bool quit = false;
    std::string input;
    while (!quit) {
        if (!held) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        pollfd stdinPoll;
        stdinPoll.fd = STDIN_FILENO;
        stdinPoll.events = POLLIN;
        stdinPoll.revents = 0;
        if ((poll(&stdinPoll, 1, held ? 10 : 1) > 0) && (stdinPoll.revents & (POLLIN | POLLHUP))) {
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
            replyToParent(peerCommand(app, *client, held, line, &quit));
        }
    }
    client.reset();
}

class LiveSessionConvergenceTest
    : public BaseTest,
      public ::testing::WithParamInterface<unsigned int> {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        _hostApp = getApp();
        ASSERT_NE(_hostApp, nullptr);
        ASSERT_TRUE(_dir.isValid());
    }

    void TearDown() override
    {
        for (const std::unique_ptr<Peer>& peer : _peers) {
            peer->quit();
        }
        for (const std::unique_ptr<Peer>& peer : _peers) {
            EXPECT_TRUE(peer->finish()) << "a client process failed: " << peer->output();
        }
        _peers.clear();
        _host.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (_hostApp) {
            _hostApp->getProject()->reset(false, true);
        }
        BaseTest::TearDown();
    }

    // The starting comp: a few connected nodes and the node every peer races on.
    void buildComp()
    {
        NodePtr constant = createIn(_hostApp, PLUGINID_NATRON_CONSTANT);
        NodePtr grade = createIn(_hostApp, PLUGINID_NATRON_GRADE);
        NodePtr blur = createIn(_hostApp, PLUGINID_NATRON_BLUR);
        NodePtr merge = createIn(_hostApp, PLUGINID_NATRON_NATIVE_MERGE);
        NodePtr hot = createIn(_hostApp, PLUGINID_NATRON_GRADE);
        ASSERT_TRUE(constant && grade && blur && merge && hot);
        hot->setScriptName(kHotNodeName);
        ASSERT_TRUE(NodeCollection::connectNodes(0, constant, grade, false));
        ASSERT_TRUE(NodeCollection::connectNodes(0, grade, blur, false));
        ASSERT_TRUE(NodeCollection::connectNodes(0, blur, merge, false));
        ASSERT_TRUE(NodeCollection::connectNodes(1, constant, merge, false));
        ASSERT_TRUE(NodeCollection::connectNodes(0, constant, hot, false));
    }

    void startSession()
    {
        _host = std::make_unique<SessionHost>(_hostApp);
        _host->setChecksumInterval(0);
        QString error;
        ASSERT_TRUE(_host->start(QString::fromUtf8("127.0.0.1"), 0, QString::fromUtf8("host"), &error)) << error.toStdString();
        for (int i = 0; i < kClientCount; ++i) {
            _peers.push_back(std::make_unique<Peer>(_host->serverPort(), i + 1));
            ASSERT_TRUE(_peers.back()->started()) << "client " << (i + 1) << " did not start";
        }
        for (int i = 0; i < kClientCount; ++i) {
            ASSERT_EQ(_peers[i]->reply(true), "joined") << "client " << (i + 1) << ": " << _peers[i]->output();
        }
        ASSERT_TRUE(runUntil([this]() { return _host->peerCount() == kClientCount; })) << "the host did not see every client join";
    }

    // Runs the session until every edit made has reached every peer.
    bool quiesce()
    {
        int quietRounds = 0;
        std::uint64_t lastSeq = ~std::uint64_t(0);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kTimeoutMs) {
            _host->flushLocalEdits();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            bool quiet = _host->isIdle();
            for (const std::unique_ptr<Peer>& peer : _peers) {
                const PeerState state = peer->state();
                if (!state.ok) {
                    ADD_FAILURE() << "a client stopped answering: " << peer->output();

                    return false;
                }
                quiet = quiet && !state.pending && state.idle && (state.seq == _host->seq());
            }
            quietRounds = (quiet && (_host->seq() == lastSeq)) ? quietRounds + 1 : 0;
            lastSeq = _host->seq();
            if (quietRounds >= 5) {
                return true;
            }
        }

        return false;
    }

    std::string peerYaml(int client)
    {
        const QString path = _dir.filePath(QString::fromUtf8("client%1.yaml").arg(client + 1));
        if (_peers[client]->command("yaml " + path.toStdString()) != "ok") {
            return std::string();
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return std::string();
        }

        return file.readAll().toStdString();
    }

    void expectConverged()
    {
        const std::string hostYaml = LiveSession::canonicalProjectYaml(_hostApp->getProject());
        for (int i = 0; i < kClientCount; ++i) {
            EXPECT_EQ(peerYaml(i), hostYaml) << "client " << (i + 1) << " diverged";
            EXPECT_TRUE(_peers[i]->state().joined) << "client " << (i + 1);
        }
    }

    void expectChecksumsMatch()
    {
        std::vector<int> before;
        for (const std::unique_ptr<Peer>& peer : _peers) {
            before.push_back(peer->state().mismatches);
        }
        _host->broadcastChecksum();
        ASSERT_TRUE(quiesce());
        for (int i = 0; i < kClientCount; ++i) {
            EXPECT_EQ(_peers[i]->state().mismatches, before[i]) << "client " << (i + 1);
        }
    }

    NodePtr hostNode(const std::string& name) const
    {
        return _hostApp->getProject()->getNodeByName(name);
    }

    // Peer 0 is the host, peer n is client n.
    bool hasNode(int peer,
                 const std::string& name)
    {
        return (peer == 0) ? (hostNode(name) != nullptr) : (_peers[peer - 1]->command("has " + name) == "yes");
    }

    AppInstancePtr _hostApp;
    std::unique_ptr<SessionHost> _host;
    std::vector<std::unique_ptr<Peer>> _peers;
    QTemporaryDir _dir;
};

TEST_P(LiveSessionConvergenceTest, ConcurrentRandomEditsConverge)
{
    buildComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    expectConverged();

    const unsigned int seed = GetParam();
    const int kEditsPerPeer = 200;
    for (int i = 0; i < kClientCount; ++i) {
        _peers[i]->send("run " + std::to_string(seed * 7919u + static_cast<unsigned int>(i + 1)) + " " + std::to_string(kEditsPerPeer));
    }
    std::mt19937 random(seed * 7919u);
    std::mt19937 schedule(seed);
    for (int round = 0; round < kEditsPerPeer; ++round) {
        randomEdit(_hostApp, random);
        flushRecorder(_hostApp);
        const int deliveries = std::uniform_int_distribution<int>(0, 3)(schedule);
        for (int i = 0; i < deliveries; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }
    for (int i = 0; i < kClientCount; ++i) {
        ASSERT_EQ(_peers[i]->reply(true), "ran") << "client " << (i + 1) << ": " << _peers[i]->output();
    }

    ASSERT_TRUE(quiesce()) << "the session did not settle";
    expectConverged();
    expectChecksumsMatch();
}

INSTANTIATE_TEST_CASE_P(Seeds, LiveSessionConvergenceTest, ::testing::Range(1u, 21u));

// The host reads nothing while a step waits on a client's reply, so each step below reaches
// the host only once the session is run, after the steps before it.
class LiveSessionDeleteVersusEditTest
    : public LiveSessionConvergenceTest {
};

TEST_F(LiveSessionDeleteVersusEditTest, AnEditOfANodeTheHostDeletedIsRejectedAndResynced)
{
    buildComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_NE(hostNode("Grade1"), nullptr);
    const int rejectsBefore = _peers[0]->state().rejects;

    // Held, client 1 cannot read the deletion before it edits the node.
    ASSERT_EQ(_peers[0]->command("hold"), "ok");
    hostNode("Grade1")->destroyNode(true, false);
    _host->flushLocalEdits();
    ASSERT_EQ(_peers[0]->command(std::string("set Grade1 ") + kGradeParamMultiply + " 3"), "ok");
    ASSERT_EQ(_peers[0]->command("release"), "ok");

    ASSERT_TRUE(quiesce());
    expectConverged();
    EXPECT_GT(_peers[0]->state().rejects, rejectsBefore);
    for (int peer = 0; peer <= kClientCount; ++peer) {
        EXPECT_FALSE(hasNode(peer, "Grade1")) << "peer " << peer;
    }
    expectChecksumsMatch();
}

TEST_F(LiveSessionDeleteVersusEditTest, AClientDeletionWinsOverAnEarlierHostEdit)
{
    buildComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_NE(hostNode("Blur1"), nullptr);

    ASSERT_EQ(_peers[0]->command("destroy Blur1"), "ok");
    doubleKnob(hostNode("Blur1"), kBlurParamSize)->setValue(9., ViewSpec::all(), 0);
    _host->flushLocalEdits();

    ASSERT_TRUE(quiesce());
    expectConverged();
    for (int peer = 0; peer <= kClientCount; ++peer) {
        EXPECT_FALSE(hasNode(peer, "Blur1")) << "peer " << peer;
    }
    expectChecksumsMatch();
}

TEST_F(LiveSessionDeleteVersusEditTest, AnEditOfANodeAnotherClientDeletedConverges)
{
    buildComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_NE(hostNode("Constant1"), nullptr);

    ASSERT_EQ(_peers[0]->command("destroy Constant1"), "ok");
    ASSERT_EQ(_peers[1]->command(std::string("set Constant1 ") + kConstantParamColor + " 0.5"), "ok");
    ASSERT_EQ(_peers[2]->command("rename Constant1 Renamed"), "ok");

    ASSERT_TRUE(quiesce());
    expectConverged();
    expectChecksumsMatch();
}

// The creation and the rename reach the host in one edit, which the host acknowledges with
// both: the client keeps the name it gave last.
TEST_F(LiveSessionDeleteVersusEditTest, ANodeCreatedAndRenamedInOneEditKeepsItsNewName)
{
    buildComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }

    ASSERT_EQ(_peers[0]->command("named ClientGrade"), "ok");
    ASSERT_TRUE(quiesce());
    EXPECT_TRUE(hasNode(0, "ClientGrade"));
    EXPECT_TRUE(hasNode(1, "ClientGrade"));
    expectConverged();
    expectChecksumsMatch();
}

// A mask switch is set by hand and also follows its input. As in the cases above, each client
// step reaches the host only after the host's own steps that follow it here.
class LiveSessionMaskSwitchTest
    : public LiveSessionConvergenceTest {
protected:
    // The hot node's mask is connected, and a spare node is there to connect it to instead.
    void buildMaskedComp()
    {
        buildComp();
        _spare = createIn(_hostApp, PLUGINID_NATRON_CONSTANT);
        _hot = hostNode(kHotNodeName);
        ASSERT_TRUE(_spare && _hot);
        _spare->setScriptName("Spare");
        _mask = _hot->getInputNumberFromLabel("Mask");
        ASSERT_GE(_mask, 0);
        ASSERT_NE(_hot->getMaskEnabledKnob(_mask), nullptr);
        ASSERT_TRUE(NodeCollection::connectNodes(_mask, hostNode("Constant1"), _hot, false));
        ASSERT_TRUE(_hot->isMaskEnabled(_mask));
    }

    void TearDown() override
    {
        _hot.reset();
        _spare.reset();
        LiveSessionConvergenceTest::TearDown();
    }

    NodePtr _hot;
    NodePtr _spare;
    int _mask = -1;
};

TEST_F(LiveSessionMaskSwitchTest, AClientSwitchWinsOverAnEarlierHostInputChange)
{
    buildMaskedComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }

    ASSERT_EQ(_peers[0]->command(std::string("mask ") + kHotNodeName + " Mask 0"), "ok");
    ASSERT_TRUE(_hot->replaceInput(_spare, _mask));
    _host->flushLocalEdits();

    ASSERT_TRUE(quiesce());
    EXPECT_FALSE(_hot->isMaskEnabled(_mask));
    expectConverged();
    expectChecksumsMatch();
}

TEST_F(LiveSessionMaskSwitchTest, AClientInputChangeWinsOverAnEarlierHostSwitch)
{
    buildMaskedComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }

    ASSERT_EQ(_peers[0]->command(std::string("connect ") + kHotNodeName + " Mask Spare"), "ok");
    _hot->getMaskEnabledKnob(_mask)->setValue(false);
    _host->flushLocalEdits();

    ASSERT_TRUE(quiesce());
    EXPECT_TRUE(_hot->isMaskEnabled(_mask));
    expectConverged();
    expectChecksumsMatch();
}

TEST_F(LiveSessionMaskSwitchTest, AJoinAndAResyncKeepASwitchTurnedOffOverAConnectedMask)
{
    buildMaskedComp();
    if (HasFatalFailure()) {
        return;
    }
    _hot->getMaskEnabledKnob(_mask)->setValue(false);
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    expectConverged();

    // The host refuses the connection to the node it deleted and resyncs the hot node.
    const int rejectsBefore = _peers[0]->state().rejects;
    ASSERT_EQ(_peers[0]->command("hold"), "ok");
    _spare->destroyNode(true, false);
    _host->flushLocalEdits();
    ASSERT_EQ(_peers[0]->command(std::string("connect ") + kHotNodeName + " Source Spare"), "ok");
    ASSERT_EQ(_peers[0]->command("release"), "ok");

    ASSERT_TRUE(quiesce());
    EXPECT_GT(_peers[0]->state().rejects, rejectsBefore);
    EXPECT_FALSE(_hot->isMaskEnabled(_mask));
    expectConverged();
    expectChecksumsMatch();
}

// The resync of the hot node is built before the host reads the client's deletion of the mask
// source, so it carries the mask connection and a switch turned on, which the deletion turns off.
TEST_F(LiveSessionMaskSwitchTest, AResyncKeepsTheSwitchOffUnderAMaskSourceTheClientDeletes)
{
    buildMaskedComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    expectConverged();

    const int rejectsBefore = _peers[0]->state().rejects;
    ASSERT_EQ(_peers[0]->command("hold"), "ok");
    _spare->destroyNode(true, false);
    _host->flushLocalEdits();
    ASSERT_EQ(_peers[0]->command(std::string("connect ") + kHotNodeName + " Source Spare"), "ok");
    ASSERT_EQ(_peers[0]->command("destroy Constant1"), "ok");
    ASSERT_EQ(_peers[0]->command("release"), "ok");

    ASSERT_TRUE(quiesce());
    EXPECT_GT(_peers[0]->state().rejects, rejectsBefore);
    EXPECT_FALSE(_hot->isMaskEnabled(_mask));
    expectConverged();
    expectChecksumsMatch();
}
