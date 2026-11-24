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

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"

#include "Scene/ShaderNodeTestUtils.h"
#include "Serialization/ProjectBundleTestHelpers.h"
#include "Serialization/Yaml/ClipboardYaml.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

class ShaderNodeOutputsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr create(const std::string& kind,
                   const NodeCollectionPtr& collection)
    {
        return createPortTestNode(getApp(), portTestPluginId(kind), collection);
    }

    // A two-output texture feeding a BSDF from both outputs: its alpha into one port and its
    // colour into another.
    void buildTextureIntoBsdf(const NodeCollectionPtr& collection,
                              NodePtr* texture,
                              NodePtr* bsdf)
    {
        *texture = create("ImageTexture", collection);
        *bsdf = create("PrincipledBsdf", collection);
        ASSERT_TRUE(*texture && *bsdf);
        PortTestNode* downstream = portTestEffect(*bsdf);
        downstream->setPorts({ "float0", "color0", "float1" });
        ASSERT_TRUE(connectPort(*texture, *bsdf, "float0"));
        ASSERT_TRUE(connectPort(*texture, *bsdf, "color0"));
        downstream->setOutputForInput("float0", "Alpha");
        downstream->setOutputForInput("color0", "Color");
    }
};

void
expectTextureIntoBsdf(const NodePtr& texture,
                      const NodePtr& bsdf)
{
    ASSERT_TRUE(texture && bsdf);
    PortTestNode* downstream = portTestEffect(bsdf);
    ASSERT_TRUE(downstream);
    EXPECT_EQ((std::vector<std::string> { "float0", "color0", "float1" }), downstream->getPorts());
    EXPECT_EQ(texture, portInput(bsdf, "float0"));
    EXPECT_EQ(texture, portInput(bsdf, "color0"));
    EXPECT_FALSE(bool(portInput(bsdf, "float1")));
    EXPECT_EQ(std::string("Alpha"), downstream->getOutputForInput("float0"));
    EXPECT_EQ(std::string("Color"), downstream->getOutputForInput("color0"));
    EXPECT_EQ(std::string(), downstream->getOutputForInput("float1"));
}

// What the node graph does after a paste: connects each pasted node's inputs to the pasted nodes
// their serialization names.
void
reconnectPasted(const std::map<std::string, NodePtr>& pastedBySourceName,
                const std::list<Serialization::NodeSerializationPtr>& serializations)
{
    for (const Serialization::NodeSerializationPtr& serialization : serializations) {
        const std::map<std::string, NodePtr>::const_iterator downstream = pastedBySourceName.find(serialization->scriptName);
        ASSERT_TRUE(downstream != pastedBySourceName.end());
        for (const auto& input : serialization->inputs) {
            const std::map<std::string, NodePtr>::const_iterator upstream = pastedBySourceName.find(input.second);
            ASSERT_TRUE(upstream != pastedBySourceName.end()) << input.second;
            EXPECT_TRUE(connectPort(upstream->second, downstream->second, input.first)) << input.first;
        }
    }
}

} // namespace

TEST_F(ShaderNodeOutputsTest, ConnectionsAreCheckedByTargetThenPortType)
{
    NodeCollectionPtr project = getApp()->getProject();
    NodePtr texture = create("ImageTexture", project);
    NodePtr bsdf = create("PrincipledBsdf", project);
    NodePtr material = create("DwaBaseMaterial", project);
    NodePtr map = create("Map", project);
    ASSERT_TRUE(texture && bsdf && material && map);
    const PortTestNode& tex = *portTestEffect(texture);
    const PortTestNode& principled = *portTestEffect(bsdf);
    const PortTestNode& dwa = *portTestEffect(material);
    const PortTestNode& moonrayMap = *portTestEffect(map);

    EXPECT_EQ(PortTestNode::eConnectionOk, PortTestNode::checkConnection(tex, "Alpha", principled, "float0"));
    EXPECT_EQ(PortTestNode::eConnectionOk, PortTestNode::checkConnection(tex, "Color", principled, "float0"));
    EXPECT_EQ(PortTestNode::eConnectionOk, PortTestNode::checkConnection(tex, "", principled, "float0"));
    EXPECT_EQ(PortTestNode::eConnectionNoSuchOutput, PortTestNode::checkConnection(tex, "Normal", principled, "float0"));
    // An attribute that is a knob but not a port takes no connection.
    EXPECT_EQ(PortTestNode::eConnectionNoSuchInput, PortTestNode::checkConnection(tex, "Alpha", principled, "color0"));
    EXPECT_EQ(PortTestNode::eConnectionNoCommonTarget, PortTestNode::checkConnection(tex, "Color", dwa, "float0"));
    EXPECT_EQ(PortTestNode::eConnectionNoCommonTarget, PortTestNode::checkConnection(moonrayMap, "out", principled, "float0"));
    EXPECT_EQ(PortTestNode::eConnectionOk, PortTestNode::checkConnection(moonrayMap, "out", dwa, "float0"));
    // A closure is not a value.
    EXPECT_EQ(PortTestNode::eConnectionTypeMismatch, PortTestNode::checkConnection(dwa, "surface", moonrayMap, "float0"));
    EXPECT_FALSE(PortTestNode::portTypesConvert("closure", "color3"));
    EXPECT_TRUE(PortTestNode::portTypesConvert("closure", "closure"));
}

TEST_F(ShaderNodeOutputsTest, TheOutputNameSurvivesSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr group = createEmptyGroup(getApp());
    ASSERT_TRUE(bool(group));
    NodePtr texture;
    NodePtr bsdf;
    buildTextureIntoBsdf(groupCollection(group), &texture, &bsdf);
    ASSERT_FALSE(HasFatalFailure());
    expectTextureIntoBsdf(texture, bsdf);
    const std::string textureName = group->getScriptName() + "." + texture->getScriptName();
    const std::string bsdfName = group->getScriptName() + "." + bsdf->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("outputs.ntpz"), &yaml));
    EXPECT_NE(std::string::npos, yaml.find("float0=Alpha")) << yaml;

    texture.reset();
    bsdf.reset();
    group.reset();
    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("outputs.ntpz")));
    expectTextureIntoBsdf(getApp()->getNodeByFullySpecifiedName(textureName), getApp()->getNodeByFullySpecifiedName(bsdfName));
}

TEST_F(ShaderNodeOutputsTest, TheOutputNameSurvivesCopyAndPaste)
{
    NodeCollectionPtr project = getApp()->getProject();
    NodePtr texture;
    NodePtr bsdf;
    buildTextureIntoBsdf(project, &texture, &bsdf);
    ASSERT_FALSE(HasFatalFailure());

    Serialization::ClipboardSerialization clipboard;
    for (const NodePtr& node : { texture, bsdf }) {
        Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();
        node->toSerialization(serialization.get());
        clipboard.nodes.push_back(serialization);
    }
    const std::string text = Serialization::encodeClipboardYaml(clipboard);
    EXPECT_NE(std::string::npos, text.find("float0=Alpha")) << text;
    const Serialization::ClipboardSerialization pasted = Serialization::decodeClipboardYaml(text);
    ASSERT_EQ(2u, pasted.nodes.size());

    // Into the same graph, so the pasted nodes are renamed and must not reach the originals.
    std::map<std::string, NodePtr> bySourceName;
    for (const Serialization::NodeSerializationPtr& serialization : pasted.nodes) {
        NodePtr node = pasteSerializedNode(getApp(), serialization, project);
        ASSERT_TRUE(bool(node));
        bySourceName[serialization->scriptName] = node;
    }
    reconnectPasted(bySourceName, pasted.nodes);
    NodePtr texture2 = bySourceName[texture->getScriptName()];
    NodePtr bsdf2 = bySourceName[bsdf->getScriptName()];
    ASSERT_TRUE(texture2 && bsdf2);
    EXPECT_NE(texture, texture2);
    expectTextureIntoBsdf(texture2, bsdf2);
    expectTextureIntoBsdf(texture, bsdf);
}

// A name lives with its port and its upstream node. A port removed and added back, or an input
// given a different upstream, reads that node's first output; an input reconnected to the same
// node, as an undone delete does, reads the same output again.
TEST_F(ShaderNodeOutputsTest, ARemovedPortOrANewUpstreamForgetsTheOutputName)
{
    NodeCollectionPtr project = getApp()->getProject();
    NodePtr texture;
    NodePtr bsdf;
    buildTextureIntoBsdf(project, &texture, &bsdf);
    ASSERT_FALSE(HasFatalFailure());
    NodePtr map = create("Map", project);
    ASSERT_TRUE(bool(map));
    PortTestNode* downstream = portTestEffect(bsdf);

    downstream->setPorts({ "color0", "float1" });
    EXPECT_EQ(std::string(), downstream->getOutputForInput("float0"));
    EXPECT_EQ(std::string("Color"), downstream->getOutputForInput("color0"));
    downstream->setPorts({ "float0", "color0", "float1" });
    EXPECT_FALSE(bool(portInput(bsdf, "float0")));
    EXPECT_EQ(std::string(), downstream->getOutputForInput("float0"));
    ASSERT_TRUE(connectPort(map, bsdf, "float0"));
    EXPECT_EQ(std::string(), downstream->getOutputForInput("float0"));
    EXPECT_TRUE(portTestEffect(map)->findOutput(downstream->getOutputForInput("float0")) != nullptr);

    const int color0 = bsdf->getInputNumberFromLabel("color0");
    ASSERT_GE(color0, 0);
    bsdf->disconnectInput(color0);
    ASSERT_TRUE(connectPort(texture, bsdf, "color0"));
    EXPECT_EQ(std::string("Color"), downstream->getOutputForInput("color0"));
    bsdf->disconnectInput(color0);
    ASSERT_TRUE(connectPort(map, bsdf, "color0"));
    EXPECT_EQ(std::string(), downstream->getOutputForInput("color0"));

    // Moving a port keeps its name.
    downstream->setOutputForInput("color0", "out");
    downstream->setPorts({ "float1", "color0", "float0" });
    EXPECT_EQ(map, portInput(bsdf, "color0"));
    EXPECT_EQ(std::string("out"), downstream->getOutputForInput("color0"));
}

// Python and a Group exported as a PyPlug see the output name as an ordinary string parameter,
// set before the exported script connects the inputs, and an instance of the PyPlug reads the
// same outputs. The script runs as a loaded PyPlug's does: createInstance() into an empty Group.
TEST_F(ShaderNodeOutputsTest, AGroupExportCarriesPortsAndOutputNamesBeforeConnections)
{
    NodePtr group = createEmptyGroup(getApp());
    ASSERT_TRUE(bool(group));
    NodePtr texture;
    NodePtr bsdf;
    buildTextureIntoBsdf(groupCollection(group), &texture, &bsdf);
    ASSERT_FALSE(HasFatalFailure());

    NodeGroupPtr nodeGroup = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_TRUE(bool(nodeGroup));
    QString output;
    nodeGroup->exportGroupToPython(QString::fromUtf8("test.pyplug.ports"), QString::fromUtf8("PortsGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    const int ports = output.indexOf(QString::fromUtf8("float0 color0 float1"));
    const int outputs = output.indexOf(QString::fromUtf8("float0=Alpha"));
    const int connect = output.indexOf(QString::fromUtf8(".connectInput("));
    EXPECT_GE(ports, 0) << output.toStdString();
    EXPECT_GE(outputs, 0) << output.toStdString();
    // Setting the ports prunes the names of ports the node does not have yet.
    EXPECT_LT(ports, outputs) << output.toStdString();
    EXPECT_GT(connect, ports) << output.toStdString();
    EXPECT_GT(connect, outputs) << output.toStdString();

    const std::string textureName = texture->getScriptName();
    const std::string bsdfName = bsdf->getScriptName();
    texture.reset();
    bsdf.reset();
    nodeGroup.reset();
    group.reset();
    getApp()->getProject()->reset(false, true);

    std::string error;
    std::string printed;
    ASSERT_TRUE(interpretPythonScript(output.toStdString(), &error, &printed)) << error;
    CreateNodeArgs containerArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    containerArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr container = getApp()->createNode(containerArgs);
    ASSERT_TRUE(container != nullptr);
    const std::string appVar = getApp()->getAppIDString();
    ASSERT_TRUE(interpretPythonScript("createInstance(" + appVar + ", " + appVar + "." + container->getFullyQualifiedName() + ")\n", &error, &printed)) << error;

    NodeGroupPtr instance = std::dynamic_pointer_cast<NodeGroup>(container->getEffectInstance());
    ASSERT_TRUE(instance != nullptr);
    expectTextureIntoBsdf(instance->getNodeByName(textureName), instance->getNodeByName(bsdfName));
}
