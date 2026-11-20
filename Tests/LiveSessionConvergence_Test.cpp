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
#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>

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

} // namespace

class LiveSessionConvergenceTest
    : public BaseTest,
      public ::testing::WithParamInterface<unsigned int> {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        _hostApp = getApp();
        ASSERT_NE(_hostApp, nullptr);
        const int hostId = _hostApp->getAppID();
        for (int i = 0; i < kClientCount; ++i) {
            AppInstancePtr app = appPTR->newAppInstance(CLArgs(), true);
            ASSERT_NE(app, nullptr);
            _clientApps.push_back(app);
        }
        appPTR->setAsTopLevelInstance(hostId);
    }

    void TearDown() override
    {
        _clients.clear();
        _host.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (const AppInstancePtr& app : _clientApps) {
            app->getProject()->reset(false, true);
            appPTR->removeInstance(app->getAppID());
        }
        _clientApps.clear();
        if (_hostApp) {
            appPTR->setAsTopLevelInstance(_hostApp->getAppID());
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
            _clients.push_back(std::make_unique<SessionClient>(_clientApps[i]));
            _clients.back()->connectToHost(QString::fromUtf8("127.0.0.1"), _host->serverPort(), QString::fromUtf8("client %1").arg(i + 1));
        }
        ASSERT_TRUE(runUntil([this]() {
            for (const std::unique_ptr<SessionClient>& client : _clients) {
                if (client->state() != SessionClient::State::eJoined) {
                    return false;
                }
            }

            return _host->peerCount() == kClientCount;
        })) << "the clients did not join";
    }

    static bool runUntil(const std::function<bool()>& done,
                         int timeoutMs = 60000)
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

    void flushAll()
    {
        _host->flushLocalEdits();
        for (const std::unique_ptr<SessionClient>& client : _clients) {
            client->flushLocalEdits();
        }
    }

    // Runs the event loop until every edit made has reached every peer.
    bool quiesce()
    {
        int quietRounds = 0;
        std::uint64_t lastSeq = ~std::uint64_t(0);

        return runUntil([&]() {
            flushAll();
            bool quiet = _host->isIdle();
            for (const std::unique_ptr<SessionClient>& client : _clients) {
                quiet = quiet && client->isIdle() && !client->hasPending() && (client->seq() == _host->seq());
            }
            quietRounds = (quiet && (_host->seq() == lastSeq)) ? quietRounds + 1 : 0;
            lastSeq = _host->seq();

            return quietRounds >= 5;
        });
    }

    void expectConverged()
    {
        const std::string hostYaml = LiveSession::canonicalProjectYaml(_hostApp->getProject());
        for (int i = 0; i < kClientCount; ++i) {
            EXPECT_EQ(LiveSession::canonicalProjectYaml(_clientApps[i]->getProject()), hostYaml) << "client " << (i + 1) << " diverged";
            EXPECT_EQ(_clients[i]->state(), SessionClient::State::eJoined);
        }
    }

    void expectChecksumsMatch()
    {
        std::vector<int> before;
        for (const std::unique_ptr<SessionClient>& client : _clients) {
            before.push_back(client->checksumMismatchCount());
        }
        _host->broadcastChecksum();
        ASSERT_TRUE(quiesce());
        for (int i = 0; i < kClientCount; ++i) {
            EXPECT_EQ(_clients[i]->checksumMismatchCount(), before[i]) << "client " << (i + 1);
        }
    }

    AppInstancePtr peerApp(int peer) const
    {
        return (peer == 0) ? _hostApp : _clientApps[peer - 1];
    }

    NodePtr nodeNamed(int peer,
                      const std::string& name) const
    {
        return peerApp(peer)->getProject()->getNodeByName(name);
    }

    AppInstancePtr _hostApp;
    std::vector<AppInstancePtr> _clientApps;
    std::unique_ptr<SessionHost> _host;
    std::vector<std::unique_ptr<SessionClient>> _clients;
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
    std::vector<std::mt19937> randoms;
    for (int peer = 0; peer <= kClientCount; ++peer) {
        randoms.emplace_back(seed * 7919u + static_cast<unsigned int>(peer));
    }
    std::mt19937 schedule(seed);
    const int kEditsPerPeer = 200;
    for (int round = 0; round < kEditsPerPeer; ++round) {
        for (int peer = 0; peer <= kClientCount; ++peer) {
            randomEdit(peerApp(peer), randoms[peer]);
            peerApp(peer)->getProject()->getGraphEditRecorder()->flush();
        }
        // Peers edit several rounds on what they have before seeing each other's edits.
        const int deliveries = std::uniform_int_distribution<int>(0, 3)(schedule);
        for (int i = 0; i < deliveries; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }

    ASSERT_TRUE(quiesce()) << "the session did not settle";
    expectConverged();
    expectChecksumsMatch();
}

INSTANTIATE_TEST_SUITE_P(Seeds, LiveSessionConvergenceTest, ::testing::Range(1u, 21u));

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
    const std::string gradeName = nodeNamed(0, "Grade1") ? "Grade1" : std::string();
    ASSERT_FALSE(gradeName.empty());
    const int rejectsBefore = _clients[0]->rejectCount();

    nodeNamed(0, gradeName)->destroyNode(true, false);
    _host->flushLocalEdits();
    doubleKnob(nodeNamed(1, gradeName), kGradeParamMultiply)->setValue(3., ViewSpec::all(), 0);
    _clients[0]->flushLocalEdits();

    ASSERT_TRUE(quiesce());
    expectConverged();
    EXPECT_GT(_clients[0]->rejectCount(), rejectsBefore);
    for (int peer = 0; peer <= kClientCount; ++peer) {
        EXPECT_EQ(nodeNamed(peer, gradeName), nullptr) << "peer " << peer;
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
    const std::string blurName = "Blur1";
    ASSERT_NE(nodeNamed(0, blurName), nullptr);

    nodeNamed(1, blurName)->destroyNode(true, false);
    _clients[0]->flushLocalEdits();
    doubleKnob(nodeNamed(0, blurName), kBlurParamSize)->setValue(9., ViewSpec::all(), 0);
    _host->flushLocalEdits();

    ASSERT_TRUE(quiesce());
    expectConverged();
    for (int peer = 0; peer <= kClientCount; ++peer) {
        EXPECT_EQ(nodeNamed(peer, blurName), nullptr) << "peer " << peer;
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
    const std::string constantName = "Constant1";
    ASSERT_NE(nodeNamed(0, constantName), nullptr);

    nodeNamed(1, constantName)->destroyNode(true, false);
    doubleKnob(nodeNamed(2, constantName), kConstantParamColor)->setValue(0.5, ViewSpec::all(), 0);
    nodeNamed(3, constantName)->setScriptName("Renamed");
    _clients[0]->flushLocalEdits();
    _clients[1]->flushLocalEdits();
    _clients[2]->flushLocalEdits();

    ASSERT_TRUE(quiesce());
    expectConverged();
    expectChecksumsMatch();
}
