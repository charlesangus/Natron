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

#include "Global/Macros.h"

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/DataKindTestEffect.h"
#include "Tests/Scene/ShaderManifestTestUtils.h"
#include "Tests/TestBuiltInPlugins.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/ViewerInstance.h"

#define kNetworkKindTestNodeID "test.natron.built-in.NetworkKindNode"
#define kNetworkKindTestSceneSinkID "test.natron.built-in.NetworkKindSceneSink"

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<int> networkRenders(0);

// A network node: network in, network out, counting every render it is asked for.
class NetworkKindTestNode
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new NetworkKindTestNode(n);
    }

    explicit NetworkKindTestNode(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "in", .kind = eDataKindNetwork, .role = InputRole::Aux, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kNetworkKindTestNodeID,
        .label = "Test Network Kind Node",
        .outputKind = eDataKindNetwork,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual StatusEnum render(const RenderActionArgs& /*args*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++networkRenders;

        return eStatusFailed;
    }
};

class NetworkKindTestSceneSink
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new NetworkKindTestSceneSink(n);
    }

    explicit NetworkKindTestSceneSink(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kNetworkKindTestSceneSinkID,
        .label = "Test Network Kind Scene Sink",
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

template <typename PLUGIN>
void
registerNetworkKindTestPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    testRegisteredPluginIds().insert(node->getPluginID());
    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);
    const QString label = QString::fromUtf8(node->getPluginLabel().c_str());
    Plugin* p = appPTR->registerPlugin(QString(), QStringList(QString::fromUtf8(PLUGIN_GROUP_OTHER)), QString::fromUtf8(node->getPluginID().c_str()), label,
                                       QString(), QStringList(), false, false, binary, false, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), which assigns the label script names are made from.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(label));
}

void
registerNetworkKindTestPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNetworkKindTestPlugin<NetworkKindTestNode>();
        registerNetworkKindTestPlugin<NetworkKindTestSceneSink>();
    });
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class NetworkKindTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerNetworkKindTestPluginsOnce();
        BaseTest::SetUp();
        networkRenders = 0;
    }

    NodePtr createNetworkNode()
    {
        return createNode(QString::fromUtf8(kNetworkKindTestNodeID));
    }
};

TEST_F(NetworkKindTest, ANetworkNodeOutputsTheNetworkKind)
{
    NodePtr node = createNetworkNode();

    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindNetwork, node->getEffectiveOutputDataKind());
}

TEST_F(NetworkKindTest, NetworkJoinsNetworkOnly)
{
    NodePtr upstream = createNetworkNode();
    NodePtr downstream = createNetworkNode();
    NodePtr imageSource = createNode(_generatorPluginID);
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));

    ASSERT_TRUE(upstream && downstream && imageSource && deepSource);
    EXPECT_EQ(Node::eCanConnectInput_ok, downstream->canConnectInput(upstream, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, downstream->canConnectInput(imageSource, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, downstream->canConnectInput(deepSource, 0));
}

TEST_F(NetworkKindTest, ImageAndSceneInputsRefuseANetworkNode)
{
    NodePtr network = createNetworkNode();
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));
    NodePtr sceneSink = createNode(QString::fromUtf8(kNetworkKindTestSceneSinkID));
    NodePtr deepAdapterSink = createNode(QString::fromUtf8(kTestPluginIDDataKindAdapterSink));

    ASSERT_TRUE(network && imageSink && sceneSink && deepAdapterSink);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, imageSink->canConnectInput(network, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, sceneSink->canConnectInput(network, 0));
    // An image input that takes deep through the adapter still refuses a network.
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, deepAdapterSink->canConnectInput(network, 0));
}

TEST_F(NetworkKindTest, AViewerRefusesANetworkNode)
{
    NodePtr network = createNetworkNode();
    NodePtr viewer = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));

    ASSERT_TRUE(network && viewer);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, viewer->canConnectInput(network, 0));
}

// A Dot carries the network kind through, so a Dot between them changes nothing.
TEST_F(NetworkKindTest, ADotPassesTheNetworkKindThrough)
{
    NodePtr network = createNetworkNode();
    NodePtr downstream = createNetworkNode();
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(network && downstream && dot && imageSink);
    ASSERT_TRUE(dot->connectInput(network, 0));
    EXPECT_EQ(eDataKindNetwork, dot->getEffectiveOutputDataKind());
    EXPECT_EQ(Node::eCanConnectInput_ok, downstream->canConnectInput(dot, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, imageSink->canConnectInput(dot, 0));
}

TEST_F(NetworkKindTest, ANetworkNodeIsNeverRendered)
{
    NodePtr upstream = createNetworkNode();
    NodePtr network = createNetworkNode();

    ASSERT_TRUE(upstream && network);
    ASSERT_TRUE(network->connectInput(upstream, 0));

    int width = 64;
    int height = 64;
    std::vector<unsigned int> buffer(width * height, 0);
    EXPECT_FALSE(network->makePreviewImage(1, &width, &height, buffer.data()));
    float rgba[4] = { 0.f, 0.f, 0.f, 0.f };
    EXPECT_FALSE(sampleNodeColor(network, 1., ViewIdx(0), 0, RectI(0, 0, 4, 4), ImageLayerDesc::getRGBAComponents(), rgba));
    EXPECT_EQ(0, networkRenders.load());
}

// A real shader node, wired into a Viewer as a loaded project can wire it (connecting refuses it),
// renders nothing and reaches no render: no image, no tiles, and the kind-mismatch message on the
// Viewer alone.
TEST_F(NetworkKindTest, AViewerWiredToAShaderShowsNothing)
{
    NodePtr shader = createShaderNode(getApp(), "cycles_principled_bsdf", getApp()->getProject());
    CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
    viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    NodePtr viewerNode = getApp()->createNode(viewerArgs);

    ASSERT_TRUE(shader && viewerNode);
    ViewerInstance* viewer = viewerNode->isEffectViewer();
    ASSERT_TRUE(viewer != NULL);
    EXPECT_EQ(eDataKindNetwork, shader->getEffectiveOutputDataKind());
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, viewerNode->canConnectInput(shader, 0));
    EXPECT_FALSE(NodeCollection::connectNodes(0, shader, viewerNode));
    ASSERT_TRUE(viewerNode->connectInput(shader, 0));

    const SequenceTime time = (SequenceTime)getApp()->getTimeLine()->currentFrame();
    ViewerArgs args;
    const ViewerInstance::ViewerRenderRetCode ret = viewer->getRenderViewerArgsAndCheckCache_public(time, false, ViewIdx(0), 0, viewer->getHash(), true, NodePtr(), RenderStatsPtr(), &args);
    EXPECT_EQ(ViewerInstance::eViewerRenderRetCodeFail, ret);
    EXPECT_FALSE(args.params && !args.params->tiles.empty());
    EXPECT_FALSE(shader->hasPersistentMessage());
    EXPECT_TRUE(viewerNode->hasPersistentMessage());

    int width = 64;
    int height = 64;
    std::vector<unsigned int> buffer(width * height, 0);
    EXPECT_FALSE(shader->makePreviewImage(1, &width, &height, buffer.data()));
    float rgba[4] = { 0.f, 0.f, 0.f, 0.f };
    EXPECT_FALSE(sampleNodeColor(shader, 1., ViewIdx(0), 0, RectI(0, 0, 4, 4), ImageLayerDesc::getRGBAComponents(), rgba));
    EXPECT_FALSE(shader->hasPersistentMessage());
}
