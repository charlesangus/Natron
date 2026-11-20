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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"

#include "Scene/ShaderNodeTestUtils.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::chrono::steady_clock Clock;

double
millisecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double
median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.;
    }
    std::sort(values.begin(), values.end());

    return values[values.size() / 2];
}

// Ports, connections and output names of one network node, by script name.
struct NodeWiring {
    std::vector<std::string> ports;
    std::map<std::string, std::string> connections;
    std::map<std::string, std::string> outputs;

    bool operator==(const NodeWiring& other) const
    {
        return (ports == other.ports) && (connections == other.connections) && (outputs == other.outputs);
    }
};

std::map<std::string, NodeWiring>
networkWiring(const NodeCollectionPtr& collection)
{
    std::map<std::string, NodeWiring> ret;

    for (const NodePtr& node : collection->getNodes()) {
        PortTestNode* effect = portTestEffect(node);
        if (!effect) {
            continue;
        }
        NodeWiring& wiring = ret[node->getScriptName()];
        wiring.ports = effect->getPorts();
        wiring.connections = portConnections(node);
        for (const auto& connection : wiring.connections) {
            const std::string output = effect->getOutputForInput(connection.first);
            if (!output.empty()) {
                wiring.outputs[connection.first] = output;
            }
        }
    }

    return ret;
}

// The value ports of a node, the ones a texture or map can feed, in port order.
std::vector<std::string>
valuePorts(const PortTestNode& node)
{
    std::vector<std::string> ret;

    for (const std::string& port : node.getPorts()) {
        const PortTestAttribute* attribute = node.findAttribute(port);
        if (attribute && attribute->bindable) {
            ret.push_back(port);
        }
    }

    return ret;
}

/**
 * The bench network: 5 large materials, 10 BSDFs, 5 two-output textures and 30 small maps, the
 * maps feeding the materials and each texture feeding two BSDFs, one from each output. Maps and
 * materials share only the MoonRay target, textures and BSDFs only Cycles, so no edge crosses.
 **/
class Network {
public:
    NodePtr group;
    std::vector<NodePtr> materials, bsdfs, textures, maps;
    int edges = 0;

    void build(const AppInstancePtr& app,
               bool allPorts)
    {
        group = createEmptyGroup(app);
        ASSERT_TRUE(bool(group));
        const NodeCollectionPtr collection = groupCollection(group);
        const auto make = [&](const char* kind, int count, std::vector<NodePtr>* nodes) {
            for (int i = 0; i < count; ++i) {
                NodePtr node = createPortTestNode(app, portTestPluginId(kind, allPorts), collection);
                ASSERT_TRUE(bool(node)) << kind;
                nodes->push_back(node);
            }
        };
        make("DwaBaseMaterial", 5, &materials);
        make("PrincipledBsdf", 10, &bsdfs);
        make("ImageTexture", 5, &textures);
        make("Map", 30, &maps);

        std::map<NodePtr, std::size_t> nextSlot;
        const auto wire = [&](const NodePtr& upstream, const std::string& output, const NodePtr& downstream) {
            PortTestNode* effect = portTestEffect(downstream);
            const std::vector<std::string> ports = valuePorts(*effect);
            std::size_t& slot = nextSlot[downstream];
            ASSERT_LT(slot, ports.size());
            const std::string port = ports[slot++];
            ASSERT_EQ(PortTestNode::eConnectionOk, PortTestNode::checkConnection(*portTestEffect(upstream), output, *effect, port)) << port;
            ASSERT_TRUE(connectPort(upstream, downstream, port)) << port;
            if (!output.empty()) {
                effect->setOutputForInput(port, output);
            }
            ++edges;
        };
        for (std::size_t i = 0; i < maps.size(); ++i) {
            wire(maps[i], std::string(), materials[i % materials.size()]);
        }
        for (std::size_t i = 0; i < textures.size(); ++i) {
            wire(textures[i], "Alpha", bsdfs[i]);
            wire(textures[i], "Color", bsdfs[i + 5]);
        }
    }
};

PXR_NS::SdfValueTypeName
usdType(const std::string& type)
{
    if (type == "float") {
        return PXR_NS::SdfValueTypeNames->Float;
    } else if (type == "color3") {
        return PXR_NS::SdfValueTypeNames->Color3f;
    } else if (type == "vector3") {
        return PXR_NS::SdfValueTypeNames->Vector3f;
    } else if (type == "float2") {
        return PXR_NS::SdfValueTypeNames->Float2;
    } else if (type == "int") {
        return PXR_NS::SdfValueTypeNames->Int;
    } else if (type == "bool") {
        return PXR_NS::SdfValueTypeNames->Bool;
    } else if (type == "token" || type == "closure") {
        return PXR_NS::SdfValueTypeNames->Token;
    } else if (type == "float[]") {
        return PXR_NS::SdfValueTypeNames->FloatArray;
    } else if (type == "color3[]") {
        return PXR_NS::SdfValueTypeNames->Color3fArray;
    } else if (type == "int[]") {
        return PXR_NS::SdfValueTypeNames->IntArray;
    }

    return PXR_NS::SdfValueTypeNames->String;
}

PXR_NS::VtValue
knobValue(const PortTestAttribute& attribute,
          const KnobIPtr& knob)
{
    if (KnobChoicePtr choice = std::dynamic_pointer_cast<KnobChoice>(knob)) {
        return PXR_NS::VtValue(PXR_NS::TfToken("option" + std::to_string(choice->getValue())));
    } else if (KnobBoolPtr toggle = std::dynamic_pointer_cast<KnobBool>(knob)) {
        return PXR_NS::VtValue(toggle->getValue());
    } else if (KnobIntPtr integer = std::dynamic_pointer_cast<KnobInt>(knob)) {
        return PXR_NS::VtValue(integer->getValue());
    } else if (std::shared_ptr<KnobDoubleBase> number = std::dynamic_pointer_cast<KnobDoubleBase>(knob)) {
        if (attribute.type == "color3") {
            return PXR_NS::VtValue(PXR_NS::GfVec3f((float)number->getValue(0), (float)number->getValue(1), (float)number->getValue(2)));
        } else if (attribute.type == "vector3") {
            return PXR_NS::VtValue(PXR_NS::GfVec3f((float)number->getValue(0), (float)number->getValue(1), (float)number->getValue(2)));
        } else if (attribute.type == "float2") {
            return PXR_NS::VtValue(PXR_NS::GfVec2f((float)number->getValue(0), (float)number->getValue(1)));
        }

        return PXR_NS::VtValue((float)number->getValue());
    } else if (KnobStringPtr text = std::dynamic_pointer_cast<KnobString>(knob)) {
        if (attribute.type == "float[]") {
            return PXR_NS::VtValue(PXR_NS::VtFloatArray());
        } else if (attribute.type == "color3[]") {
            return PXR_NS::VtValue(PXR_NS::VtVec3fArray());
        } else if (attribute.type == "int[]") {
            return PXR_NS::VtValue(PXR_NS::VtIntArray());
        }

        return PXR_NS::VtValue(text->getValue());
    }

    return PXR_NS::VtValue();
}

// Authors the whole network as UsdShade shaders: every attribute's value, and every connected
// port as a connection to the output it reads. The re-author a knob edit costs at worst.
bool
authorNetwork(const NodeCollectionPtr& collection,
              std::string* error)
{
    SceneLayerEdit edit;
    edit.layer = PXR_NS::SdfLayer::CreateAnonymous();
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
    if (!stage) {
        return false;
    }
    const PXR_NS::SdfPath root("/Network");
    const NodesList nodes = collection->getNodes();
    std::map<const Node*, PXR_NS::UsdShadeShader> shaders;

    for (const NodePtr& node : nodes) {
        PortTestNode* effect = portTestEffect(node);
        if (!effect) {
            continue;
        }
        PXR_NS::UsdShadeShader shader = PXR_NS::UsdShadeShader::Define(stage, root.AppendChild(PXR_NS::TfToken(node->getScriptName())));
        shader.SetShaderId(PXR_NS::TfToken(effect->layout().label));
        for (const PortTestOutput& output : effect->layout().outputs) {
            shader.CreateOutput(PXR_NS::TfToken(output.name), usdType(output.type));
        }
        std::map<std::string, KnobIPtr> knobs;
        for (const KnobIPtr& knob : node->getKnobs()) {
            knobs[knob->getName()] = knob;
        }
        for (const PortTestAttribute& attribute : effect->layout().attributes) {
            const std::map<std::string, KnobIPtr>::const_iterator knob = knobs.find(attribute.name);
            if (knob == knobs.end()) {
                *error = "no knob " + attribute.name;

                return false;
            }
            shader.CreateInput(PXR_NS::TfToken(attribute.name), usdType(attribute.type)).Set(knobValue(attribute, knob->second));
        }
        shaders[node.get()] = shader;
    }
    for (const NodePtr& node : nodes) {
        PortTestNode* effect = portTestEffect(node);
        if (!effect) {
            continue;
        }
        for (const std::string& port : effect->getPorts()) {
            NodePtr upstream = portInput(node, port);
            PortTestNode* source = portTestEffect(upstream);
            if (!source) {
                continue;
            }
            const PortTestOutput* output = source->findOutput(effect->getOutputForInput(port));
            if (!output) {
                *error = "no output for " + port;

                return false;
            }
            shaders[node.get()].GetInput(PXR_NS::TfToken(port)).ConnectToSource(shaders[upstream.get()].GetOutput(PXR_NS::TfToken(output->name)));
        }
    }

    return true;
}

class ShaderNodeCostTest
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

    std::vector<NodePtr> createGradeChain(int count)
    {
        std::vector<NodePtr> ret;

        for (int i = 0; i < count; ++i) {
            CreateNodeArgs args(PLUGINID_NATRON_GRADE, getApp()->getProject());
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
            args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
            NodePtr grade = getApp()->createNode(args);
            if (!grade) {
                ADD_FAILURE() << "cannot create " << PLUGINID_NATRON_GRADE;

                return ret;
            }
            if (!ret.empty()) {
                grade->connectInput(ret.back(), 0);
            }
            ret.push_back(grade);
        }

        return ret;
    }

    // Comp graph edits: a knob change and a reconnection per round, the edits a compositor makes
    // most while a network sits in the project.
    double timeCompEdits(const std::vector<NodePtr>& chain,
                         int rounds)
    {
        const Clock::time_point start = Clock::now();

        for (int i = 0; i < rounds; ++i) {
            const NodePtr& grade = chain[1 + (i % (chain.size() - 1))];
            std::shared_ptr<KnobDoubleBase> multiply = std::dynamic_pointer_cast<KnobDoubleBase>(grade->getKnobByName(kGradeParamMultiply));
            if (multiply) {
                multiply->setValue(1. + 0.001 * i);
            }
            NodePtr upstream = grade->getInput(0);
            grade->disconnectInput(0);
            grade->connectInput(upstream, 0);
        }

        return millisecondsSince(start);
    }

    void runCostBench(bool allPorts)
    {
        ProjectPtr project = getApp()->getProject();
        const std::vector<NodePtr> chain = createGradeChain(20);
        ASSERT_EQ(20u, chain.size());
        const int rounds = 200;
        timeCompEdits(chain, rounds / 10);
        const double compWithoutMs = timeCompEdits(chain, rounds);

        const Clock::time_point createStart = Clock::now();
        Network network;
        network.build(getApp(), allPorts);
        ASSERT_FALSE(HasFatalFailure());
        const double createMs = millisecondsSince(createStart);
        ASSERT_EQ(40, network.edges);
        const std::string groupName = network.group->getScriptName();
        const std::map<std::string, NodeWiring> wiring = networkWiring(groupCollection(network.group));
        ASSERT_EQ(50u, wiring.size());

        timeCompEdits(chain, rounds / 10);
        const double compWithMs = timeCompEdits(chain, rounds);

        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString directory = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("network.ntpz");
        QString savedPath;
        const Clock::time_point saveStart = Clock::now();
        ASSERT_TRUE(project->saveProject(directory, fileName, &savedPath));
        const double saveMs = millisecondsSince(saveStart);

        network = Network();
        project->reset(false, true);
        const Clock::time_point openStart = Clock::now();
        ASSERT_TRUE(project->loadProject(directory, fileName));
        const double openMs = millisecondsSince(openStart);

        NodePtr group = getApp()->getNodeByFullySpecifiedName(groupName);
        ASSERT_TRUE(bool(group));
        const NodeCollectionPtr collection = groupCollection(group);
        const std::map<std::string, NodeWiring> loaded = networkWiring(collection);
        ASSERT_EQ(wiring.size(), loaded.size());
        for (const auto& node : wiring) {
            const std::map<std::string, NodeWiring>::const_iterator it = loaded.find(node.first);
            ASSERT_TRUE(it != loaded.end()) << node.first;
            EXPECT_TRUE(node.second == it->second) << node.first;
        }

        NodePtr material;
        for (const NodePtr& node : collection->getNodes()) {
            PortTestNode* effect = portTestEffect(node);
            if (effect && (effect->layout().attributes.size() > 100)) {
                material = node;
                break;
            }
        }
        ASSERT_TRUE(bool(material));
        std::shared_ptr<KnobDoubleBase> knob = std::dynamic_pointer_cast<KnobDoubleBase>(material->getKnobByName("float20"));
        ASSERT_TRUE(bool(knob));
        std::vector<double> knobEditMs;
        for (int i = 0; i < 11; ++i) {
            const Clock::time_point editStart = Clock::now();
            knob->setValue(0.5 + 0.01 * i);
            std::string error;
            ASSERT_TRUE(authorNetwork(collection, &error)) << error;
            knobEditMs.push_back(millisecondsSince(editStart));
        }

        int inputs = 0;
        int knobs = 0;
        for (const NodePtr& node : collection->getNodes()) {
            inputs += node->getNInputs();
            knobs += (int)node->getKnobs().size();
        }
        std::printf("SHADER_BENCH {\"test\":\"network\",\"variant\":\"%s\",\"nodes\":%d,\"edges\":%d,\"inputs\":%d,\"knobs\":%d,"
                    "\"create_ms\":%.3f,\"save_ms\":%.3f,\"open_ms\":%.3f,\"knob_edit_ms\":%.3f,\"knob_edit_max_ms\":%.3f,"
                    "\"comp_edit_without_ms\":%.3f,\"comp_edit_with_ms\":%.3f,\"comp_rounds\":%d}\n",
                    allPorts ? "every-attribute-a-port" : "knob-first", (int)wiring.size(), 40, inputs, knobs,
                    createMs, saveMs, openMs, median(knobEditMs), *std::max_element(knobEditMs.begin(), knobEditMs.end()),
                    compWithoutMs, compWithMs, rounds);
        std::fflush(stdout);
    }
};

} // namespace

TEST_F(ShaderNodeCostTest, KnobFirstNetwork)
{
    runCostBench(false);
}

TEST_F(ShaderNodeCostTest, EveryAttributeAPortNetwork)
{
    runCostBench(true);
}
