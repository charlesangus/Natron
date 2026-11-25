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

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "TestBuiltInPlugins.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Network/NetworkNode.h"
#include "Engine/Project.h"

#include "Serialization/ProjectBundleTestHelpers.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kPortDescriptorTestPluginID = "test.natron.network.PortDescriptorNode";

NetworkAttribute
makeAttribute(const std::string& name,
              const std::string& label,
              const std::string& page,
              NetworkKnobKind knob,
              int dimensions,
              const std::string& type,
              PortTypeClass typeClass,
              bool linkable,
              bool portOnly,
              bool array)
{
    NetworkAttribute attribute;

    attribute.name = name;
    attribute.label = label;
    attribute.page = page;
    attribute.knob = knob;
    attribute.dimensions = dimensions;
    attribute.defaults.assign(dimensions, 0.5);
    attribute.linkable = linkable;
    attribute.portOnly = portOnly;
    attribute.port.attribute = name;
    attribute.port.type = type;
    attribute.port.typeClass = typeClass;
    attribute.port.page = page;
    attribute.port.array = array;
    attribute.port.varying = linkable && !portOnly;

    return attribute;
}

NetworkNodeLayout
portDescriptorTestLayout()
{
    NetworkNodeLayout layout;

    layout.pluginId = kPortDescriptorTestPluginID;
    layout.label = "Test Port Descriptor Node";
    layout.grouping = "Test/Network";
    layout.targets = { "test" };
    layout.pages = { "Surface", "Ramp" };
    layout.attributes = {
        makeAttribute("base_color", "Base Color", "Surface", NetworkKnobKind::Color, 3, "color", PortTypeClass::Color, true, false, false),
        makeAttribute("normal", "Normal", "Surface", NetworkKnobKind::None, 1, "normal", PortTypeClass::Normal, true, true, false),
        makeAttribute("ramp", "Ramp", "Ramp", NetworkKnobKind::Parametric, 3, "array_color", PortTypeClass::Color, true, false, true),
        makeAttribute("samples", "Samples", "Ramp", NetworkKnobKind::Int, 1, "int", PortTypeClass::Int, false, false, false),
    };
    OutputPortDescriptor bsdf;
    bsdf.name = "BSDF";
    bsdf.type = "closure";
    bsdf.typeClass = PortTypeClass::Closure;
    OutputPortDescriptor alpha;
    alpha.name = "Alpha";
    alpha.type = "float";
    alpha.typeClass = PortTypeClass::Float;
    layout.outputs = { bsdf, alpha };
    layout.defaultPorts = { "base_color" };

    return layout;
}

void
registerPortDescriptorTestPluginOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        const NetworkNodeLayout* layout = NetworkNode::addLayout(portDescriptorTestLayout());
        testRegisteredPluginIds().insert(layout->pluginId);
        (void)NetworkNode::registerPlugin(*layout);
    });
}

NetworkNode*
networkEffect(const NodePtr& node)
{
    return node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : nullptr;
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class PortDescriptorTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerPortDescriptorTestPluginOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createNetworkNode()
    {
        CreateNodeArgs args(kPortDescriptorTestPluginID, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

        return getApp()->createNode(args);
    }
};

TEST_F(PortDescriptorTest, ANetworkNodeDescribesEachInputAndOutput)
{
    NodePtr node = createNetworkNode();

    ASSERT_TRUE(bool(node));
    ASSERT_EQ(2, node->getNInputs());

    const std::optional<PortDescriptor> color = node->getInputPortDescriptor(0);
    ASSERT_TRUE(color.has_value());
    EXPECT_EQ(std::string("base_color"), color->attribute);
    EXPECT_EQ(std::string("color"), color->type);
    EXPECT_EQ(PortTypeClass::Color, color->typeClass);
    EXPECT_EQ(std::string("Surface"), color->page);
    EXPECT_TRUE(color->promoted);
    EXPECT_FALSE(color->array);
    EXPECT_TRUE(color->varying);
    EXPECT_EQ(std::string("base_color"), node->getInputLabel(0));

    // A port-only attribute is always an input, after the promoted ones.
    const std::optional<PortDescriptor> normal = node->getInputPortDescriptor(1);
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(std::string("normal"), normal->attribute);
    EXPECT_EQ(PortTypeClass::Normal, normal->typeClass);
    EXPECT_EQ(std::string("Surface"), normal->page);
    EXPECT_TRUE(normal->promoted);
    EXPECT_FALSE(normal->array);
    EXPECT_FALSE(normal->varying);

    EXPECT_FALSE(node->getInputPortDescriptor(2).has_value());
    EXPECT_FALSE(node->getInputPortDescriptor(-1).has_value());

    const std::vector<OutputPortDescriptor> outputs = node->getOutputPorts();
    ASSERT_EQ(2u, outputs.size());
    EXPECT_EQ(std::string("BSDF"), outputs[0].name);
    EXPECT_EQ(PortTypeClass::Closure, outputs[0].typeClass);
    EXPECT_EQ(std::string("Alpha"), outputs[1].name);
    EXPECT_EQ(PortTypeClass::Float, outputs[1].typeClass);

    // The array attribute is described once promoted.
    NetworkNode* effect = networkEffect(node);
    ASSERT_TRUE(effect);
    ASSERT_TRUE(effect->promotePort("ramp"));
    ASSERT_EQ(3, node->getNInputs());
    const std::optional<PortDescriptor> ramp = node->getInputPortDescriptor(2);
    ASSERT_TRUE(ramp.has_value());
    EXPECT_EQ(std::string("ramp"), ramp->attribute);
    EXPECT_EQ(std::string("Ramp"), ramp->page);
    EXPECT_TRUE(ramp->array);
    EXPECT_TRUE(ramp->varying);

    // Every linkable attribute is described, promoted or not; a knob-only one is not.
    effect->setPorts({});
    const std::vector<PortDescriptor> all = effect->getPortDescriptors();
    ASSERT_EQ(3u, all.size());
    EXPECT_FALSE(all[0].promoted);
    EXPECT_TRUE(all[1].promoted);
    EXPECT_FALSE(all[2].promoted);
}

TEST_F(PortDescriptorTest, AClassicNodeDescribesNoPorts)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(node));
    EXPECT_FALSE(node->getInputPortDescriptor(0).has_value());
    EXPECT_TRUE(node->getOutputPorts().empty());

    NodePtr write = createNode(_writePluginID);
    ASSERT_TRUE(bool(write));
    ASSERT_GT(write->getNInputs(), 0);
    for (int i = 0; i < write->getNInputs(); ++i) {
        EXPECT_FALSE(write->getInputPortDescriptor(i).has_value()) << i;
    }
    EXPECT_TRUE(write->getOutputPorts().empty());
}

TEST_F(PortDescriptorTest, PromotedPortsAndOutputNamesSurviveASaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr upstream = createNetworkNode();
    NodePtr downstream = createNetworkNode();

    ASSERT_TRUE(upstream && downstream);
    NetworkNode* effect = networkEffect(downstream);
    ASSERT_TRUE(effect);
    ASSERT_TRUE(effect->promotePort("ramp"));
    const int rampInput = downstream->getInputNumberFromLabel("ramp");
    ASSERT_EQ(2, rampInput);
    ASSERT_TRUE(downstream->connectInput(upstream, rampInput));
    effect->setOutputForInput("ramp", "Alpha");
    ASSERT_EQ(std::string("Alpha"), effect->getOutputForInput("ramp"));
    EXPECT_EQ(NetworkNode::eConnectionOk, NetworkNode::checkConnection(*networkEffect(upstream), "Alpha", *effect, "ramp"));

    const std::string upstreamName = upstream->getScriptName();
    const std::string downstreamName = downstream->getScriptName();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("ports.ntpz"), &yaml));

    effect = nullptr;
    upstream.reset();
    downstream.reset();
    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("ports.ntpz")));
    NodePtr upstream2 = getApp()->getNodeByFullySpecifiedName(upstreamName);
    NodePtr downstream2 = getApp()->getNodeByFullySpecifiedName(downstreamName);
    ASSERT_TRUE(upstream2 && downstream2);
    NetworkNode* loaded = networkEffect(downstream2);
    ASSERT_TRUE(loaded);
    EXPECT_EQ((std::vector<std::string> { "base_color", "normal", "ramp" }), loaded->getPorts());
    ASSERT_EQ(3, downstream2->getNInputs());
    EXPECT_EQ(upstream2, downstream2->getInput(2));
    const std::optional<PortDescriptor> ramp = downstream2->getInputPortDescriptor(2);
    ASSERT_TRUE(ramp.has_value());
    EXPECT_TRUE(ramp->promoted);
    EXPECT_EQ(std::string("ramp"), ramp->attribute);
    EXPECT_EQ(std::string("Alpha"), loaded->getOutputForInput("ramp"));
}
