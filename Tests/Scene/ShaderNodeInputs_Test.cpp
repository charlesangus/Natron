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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "TestBuiltInPlugins.h"

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Plugin.h"

#include "Scene/ShaderNodeTestUtils.h"
#include "Serialization/ProjectBundleTestHelpers.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

class ShaderNodeInputsTest
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
                   bool allPorts = false)
    {
        return createPortTestNode(getApp(), portTestPluginId(kind, allPorts), getApp()->getProject());
    }
};

std::vector<std::string>
withPort(std::vector<std::string> ports,
         std::size_t index,
         const std::string& port)
{
    ports.insert(ports.begin() + (std::ptrdiff_t)std::min(index, ports.size()), port);

    return ports;
}

std::vector<std::string>
withoutPort(std::vector<std::string> ports,
            const std::string& port)
{
    ports.erase(std::remove(ports.begin(), ports.end(), port), ports.end());

    return ports;
}

double
millisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

TEST_F(ShaderNodeInputsTest, TheRuntimeSpecNamesTheShownPorts)
{
    NodePtr bsdf = create("PrincipledBsdf");
    ASSERT_TRUE(bool(bsdf));
    PortTestNode* effect = portTestEffect(bsdf);
    ASSERT_TRUE(effect);

    const std::vector<std::string> ports = effect->getPorts();
    ASSERT_EQ(10u, ports.size());
    EXPECT_EQ(10, bsdf->getNInputs());
    for (std::size_t i = 0; i < ports.size(); ++i) {
        EXPECT_EQ((int)i, bsdf->getInputNumberFromLabel(ports[i])) << ports[i];
        EXPECT_EQ(ports[i], bsdf->getInputLabel((int)i));
    }
    // Every attribute is a knob whether or not it is a port.
    for (const PortTestAttribute& attribute : effect->layout().attributes) {
        EXPECT_TRUE(bool(bsdf->getKnobByName(attribute.name))) << attribute.name;
    }
    EXPECT_EQ(-1, bsdf->getInputNumberFromLabel("color0"));

    NodePtr all = create("PrincipledBsdf", true);
    ASSERT_TRUE(bool(all));
    EXPECT_EQ((int)portTestEffect(all)->layout().attributes.size(), all->getNInputs());
}

TEST_F(ShaderNodeInputsTest, AddingAnInputKeepsExistingConnections)
{
    NodePtr bsdf = create("PrincipledBsdf");
    NodePtr mapA = create("Map");
    NodePtr mapB = create("Map");
    ASSERT_TRUE(bsdf && mapA && mapB);
    PortTestNode* effect = portTestEffect(bsdf);

    ASSERT_TRUE(connectPort(mapA, bsdf, "float2"));
    ASSERT_TRUE(connectPort(mapB, bsdf, "float9"));

    // In front of every port, so every connected input changes number.
    effect->setPorts(withPort(effect->getPorts(), 0, "color0"));
    EXPECT_EQ(11, bsdf->getNInputs());
    EXPECT_EQ(0, bsdf->getInputNumberFromLabel("color0"));
    EXPECT_EQ(3, bsdf->getInputNumberFromLabel("float2"));
    EXPECT_EQ(mapA, portInput(bsdf, "float2"));
    EXPECT_EQ(mapB, portInput(bsdf, "float9"));
    EXPECT_FALSE(bool(portInput(bsdf, "color0")));
    EXPECT_FALSE(bool(portInput(bsdf, "float1")));
    EXPECT_TRUE(isOutputOf(mapA, bsdf));
    EXPECT_EQ(1u, mapA->getOutputs().size());

    // Appended, so no connection moves.
    effect->setPorts(withPort(effect->getPorts(), 100, "vector0"));
    EXPECT_EQ(12, bsdf->getNInputs());
    EXPECT_EQ(mapA, portInput(bsdf, "float2"));
    EXPECT_EQ(mapB, portInput(bsdf, "float9"));

    // The new port takes a connection like any other.
    NodePtr mapC = create("Map");
    ASSERT_TRUE(connectPort(mapC, bsdf, "color0"));
    EXPECT_EQ(mapC, portInput(bsdf, "color0"));

    KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(bsdf->getKnobByName(kPortTestParamPorts));
    ASSERT_TRUE(bool(knob));
    EXPECT_EQ(std::string("color0 float0 float1 float2 float3 float4 float5 float6 float7 float8 float9 vector0"), knob->getValue());
}

TEST_F(ShaderNodeInputsTest, RemovingAConnectedInputDisconnectsItCleanly)
{
    NodePtr bsdf = create("PrincipledBsdf");
    NodePtr mapA = create("Map");
    NodePtr mapB = create("Map");
    ASSERT_TRUE(bsdf && mapA && mapB);
    PortTestNode* effect = portTestEffect(bsdf);

    ASSERT_TRUE(connectPort(mapA, bsdf, "float2"));
    ASSERT_TRUE(connectPort(mapB, bsdf, "float7"));

    effect->setPorts(withoutPort(effect->getPorts(), "float2"));
    EXPECT_EQ(9, bsdf->getNInputs());
    EXPECT_EQ(-1, bsdf->getInputNumberFromLabel("float2"));
    EXPECT_FALSE(isOutputOf(mapA, bsdf));
    EXPECT_TRUE(mapA->getOutputs().empty());
    EXPECT_EQ(mapB, portInput(bsdf, "float7"));
    EXPECT_TRUE(isOutputOf(mapB, bsdf));
    for (int i = 0; i < bsdf->getNInputs(); ++i) {
        EXPECT_NE(mapA, bsdf->getInput(i)) << i;
    }

    // Removing an unconnected port leaves the others where they are.
    effect->setPorts(withoutPort(effect->getPorts(), "float0"));
    EXPECT_EQ(8, bsdf->getNInputs());
    EXPECT_EQ(mapB, portInput(bsdf, "float7"));

    // The removed attribute is still a knob, and comes back as an unconnected port.
    EXPECT_TRUE(bool(bsdf->getKnobByName("float2")));
    effect->setPorts(withPort(effect->getPorts(), 1, "float2"));
    EXPECT_EQ(1, bsdf->getInputNumberFromLabel("float2"));
    EXPECT_FALSE(bool(portInput(bsdf, "float2")));
}

TEST_F(ShaderNodeInputsTest, ReorderingPortsMovesTheirConnections)
{
    NodePtr map = create("Map");
    NodePtr mapA = create("Map");
    NodePtr mapB = create("Map");
    ASSERT_TRUE(map && mapA && mapB);
    PortTestNode* effect = portTestEffect(map);
    ASSERT_EQ((std::vector<std::string> { "color0", "float0", "float1" }), effect->getPorts());

    ASSERT_TRUE(connectPort(mapA, map, "color0"));
    ASSERT_TRUE(connectPort(mapB, map, "float1"));
    effect->setPorts({ "float1", "float0", "color0" });
    EXPECT_EQ(2, map->getInputNumberFromLabel("color0"));
    EXPECT_EQ(mapA, portInput(map, "color0"));
    EXPECT_EQ(mapB, portInput(map, "float1"));
    EXPECT_EQ(1u, mapA->getOutputs().size());
    EXPECT_EQ(1u, mapB->getOutputs().size());

    // Unknown and repeated names are dropped.
    effect->setPorts({ "float1", "nope", "float1", "color0" });
    EXPECT_EQ((std::vector<std::string> { "float1", "color0" }), effect->getPorts());
    EXPECT_EQ(mapA, portInput(map, "color0"));
    EXPECT_EQ(mapB, portInput(map, "float1"));
}

TEST_F(ShaderNodeInputsTest, UndoRestoresPortsAndTheirConnections)
{
    NodePtr bsdf = create("PrincipledBsdf");
    NodePtr mapA = create("Map");
    NodePtr mapB = create("Map");
    ASSERT_TRUE(bsdf && mapA && mapB);
    PortTestNode* effect = portTestEffect(bsdf);
    const std::vector<std::string> before = effect->getPorts();

    ASSERT_TRUE(connectPort(mapA, bsdf, "float2"));
    ASSERT_TRUE(connectPort(mapB, bsdf, "float5"));

    std::vector<std::string> after = withoutPort(before, "float2");
    after = withPort(after, 0, "color1");
    PortTestEditPortsCommand command(bsdf, after);
    command.redo();
    EXPECT_EQ(after, effect->getPorts());
    EXPECT_FALSE(bool(portInput(bsdf, "float2")));
    EXPECT_TRUE(mapA->getOutputs().empty());
    EXPECT_EQ(mapB, portInput(bsdf, "float5"));

    command.undo();
    EXPECT_EQ(before, effect->getPorts());
    EXPECT_EQ(mapA, portInput(bsdf, "float2"));
    EXPECT_EQ(mapB, portInput(bsdf, "float5"));
    EXPECT_EQ(-1, bsdf->getInputNumberFromLabel("color1"));
    EXPECT_EQ(1u, mapA->getOutputs().size());

    command.redo();
    EXPECT_EQ(after, effect->getPorts());
    EXPECT_FALSE(bool(portInput(bsdf, "float2")));
    EXPECT_EQ(mapB, portInput(bsdf, "float5"));
}

// What Python and a knob undo do: they set the ports knob, and the node follows it.
TEST_F(ShaderNodeInputsTest, SettingThePortsKnobEditsTheInputs)
{
    NodePtr bsdf = create("PrincipledBsdf");
    NodePtr map = create("Map");
    ASSERT_TRUE(bsdf && map);
    KnobStringPtr ports = std::dynamic_pointer_cast<KnobString>(bsdf->getKnobByName(kPortTestParamPorts));
    ASSERT_TRUE(bool(ports));
    EXPECT_TRUE(ports->getIsSecret());
    EXPECT_TRUE(ports->getIsPersistent());

    ASSERT_TRUE(connectPort(map, bsdf, "float3"));
    ports->setValue(std::string("color0 float3"));
    EXPECT_EQ(2, bsdf->getNInputs());
    EXPECT_EQ(1, bsdf->getInputNumberFromLabel("float3"));
    EXPECT_EQ(map, portInput(bsdf, "float3"));

    ports->setValue(std::string("color0"));
    EXPECT_EQ(1, bsdf->getNInputs());
    EXPECT_TRUE(map->getOutputs().empty());
}

TEST_F(ShaderNodeInputsTest, CyclesAreRefusedOnShownPorts)
{
    NodePtr mapA = create("Map");
    NodePtr mapB = create("Map");
    ASSERT_TRUE(mapA && mapB);

    ASSERT_TRUE(connectPort(mapA, mapB, "float0"));
    portTestEffect(mapA)->setPorts({ "float1", "color0" });
    const int index = mapA->getInputNumberFromLabel("color0");
    ASSERT_EQ(1, index);
    EXPECT_EQ(Node::eCanConnectInput_graphCycles, mapA->canConnectInput(mapB, index));
    EXPECT_FALSE(mapA->connectInput(mapB, index));
    EXPECT_FALSE(bool(mapA->getInput(index)));
    EXPECT_EQ(mapA, portInput(mapB, "float0"));
}

TEST_F(ShaderNodeInputsTest, SaveAndLoadRestoreShownPortsAndConnections)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr group = createEmptyGroup(getApp());
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = groupCollection(group);
    NodePtr material = createPortTestNode(getApp(), portTestPluginId("DwaBaseMaterial"), collection);
    NodePtr mapA = createPortTestNode(getApp(), portTestPluginId("Map"), collection);
    NodePtr mapB = createPortTestNode(getApp(), portTestPluginId("Map"), collection);
    ASSERT_TRUE(material && mapA && mapB);
    PortTestNode* effect = portTestEffect(material);

    std::vector<std::string> ports = withoutPort(effect->getPorts(), "float4");
    ports = withPort(ports, 0, "color2");
    ports = withPort(ports, 5, "pair0");
    effect->setPorts(ports);
    ASSERT_TRUE(connectPort(mapA, material, "color2"));
    ASSERT_TRUE(connectPort(mapB, material, "float8"));
    portTestEffect(mapB)->setPorts({ "float1" });
    ASSERT_TRUE(connectPort(mapA, mapB, "float1"));

    const std::string materialName = group->getScriptName() + "." + material->getScriptName();
    const std::string mapBName = group->getScriptName() + "." + mapB->getScriptName();
    const std::map<std::string, std::string> connections = portConnections(material);
    ASSERT_EQ(2u, connections.size());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("ports.ntpz"), &yaml));
    EXPECT_NE(std::string::npos, yaml.find("color2")) << yaml;

    material.reset();
    mapA.reset();
    mapB.reset();
    collection.reset();
    group.reset();
    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("ports.ntpz")));
    NodePtr material2 = getApp()->getNodeByFullySpecifiedName(materialName);
    NodePtr mapB2 = getApp()->getNodeByFullySpecifiedName(mapBName);
    ASSERT_TRUE(material2 && mapB2);
    EXPECT_EQ(ports, portTestEffect(material2)->getPorts());
    EXPECT_EQ((int)ports.size(), material2->getNInputs());
    EXPECT_EQ(connections, portConnections(material2));
    EXPECT_EQ(5, material2->getInputNumberFromLabel("pair0"));
    EXPECT_FALSE(bool(portInput(material2, "pair0")));
    EXPECT_EQ((std::vector<std::string> { "float1" }), portTestEffect(mapB2)->getPorts());
    EXPECT_TRUE(bool(portInput(mapB2, "float1")));
}

// One plug-in entry per kind, every one built by one class, against what a further entry costs
// to register and what a port edit costs to republish the spec.
TEST_F(ShaderNodeInputsTest, RegistrationAndSpecCosts)
{
    const int entries = 200;
    const std::chrono::steady_clock::time_point registerStart = std::chrono::steady_clock::now();

    for (int i = 0; i < entries; ++i) {
        const std::string id = std::string(kPortTestPluginIDPrefix) + "RegistrationCost" + std::to_string(i);
        std::map<std::string, void (*)()> functions;
        functions.insert(std::make_pair("BuildEffect", (void (*)())&PortTestNode::BuildEffect));
        const QString label = QString::fromUtf8(("PortRegistrationCost" + std::to_string(i)).c_str());
        Plugin* plugin = appPTR->registerPlugin(QString(), QStringList(QString::fromUtf8(PLUGIN_GROUP_OTHER)), QString::fromUtf8(id.c_str()), label,
                                                QString(), QStringList(), false, false, new LibraryBinary(functions), false, 1, 0, false);
        ASSERT_TRUE(plugin);
        plugin->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(label));
        testRegisteredPluginIds().insert(id);
    }
    const double registerMs = millisecondsSince(registerStart);

    NodePtr material = create("TopEndMaterial");
    ASSERT_TRUE(bool(material));
    PortTestNode* effect = portTestEffect(material);
    const std::vector<std::string> ports = effect->getPorts();
    const int edits = 200;
    const std::chrono::steady_clock::time_point editStart = std::chrono::steady_clock::now();
    for (int i = 0; i < edits; ++i) {
        effect->setPorts((i % 2) ? ports : withPort(ports, 0, "color0"));
    }
    const double editMs = millisecondsSince(editStart);

    std::printf("SHADER_BENCH {\"test\":\"registration\",\"register_entry_us\":%.3f,\"port_edit_us\":%.3f,\"kinds\":%d}\n",
                1000. * registerMs / entries, 1000. * editMs / edits, (int)PortTestNode::layouts().size());
    std::fflush(stdout);
    EXPECT_EQ(ports, effect->getPorts());
}
