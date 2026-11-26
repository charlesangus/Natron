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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"

#include "Scene/ShaderManifestTestUtils.h"
#include "Serialization/ProjectBundleTestHelpers.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const char* const kPrincipledBsdf = "cycles_principled_bsdf";
const char* const kImageTexture = "cycles_image_texture";
const char* const kMixClosure = "cycles_mix_closure";

bool
hasPort(const NodePtr& node,
        const std::string& attribute)
{
    NetworkNode* effect = shaderEffect(node);

    if (!effect) {
        return false;
    }
    const std::vector<std::string> ports = effect->getPorts();

    return std::find(ports.begin(), ports.end(), attribute) != ports.end();
}

// Keeps what a step records, the way the window's history would.
class CollectingRecorder
    : public UndoRecorderI {
public:
    CollectingRecorder()
        : UndoRecorderI()
        , depth(0)
        , serial(0)
        , commands()
    {
    }

    virtual void openStep(const std::string& /*text*/) OVERRIDE FINAL
    {
        if (depth++ == 0) {
            ++serial;
        }
    }

    virtual void closeStep() OVERRIDE FINAL
    {
        if (depth > 0) {
            --depth;
        }
    }

    virtual bool isCapturing() const OVERRIDE FINAL
    {
        return depth > 0;
    }

    virtual std::uint64_t getCaptureSerial() const OVERRIDE FINAL
    {
        return serial;
    }

    virtual void record(const UndoCommandPtr& command,
                        const UndoTouched& /*touched*/) OVERRIDE FINAL
    {
        commands.push_back(command);
    }

    int depth;
    std::uint64_t serial;
    std::vector<UndoCommandPtr> commands;
};

class ShaderPromoteTest
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

    NodePtr create(const std::string& id)
    {
        return createShaderNode(getApp(), id, getApp()->getProject());
    }
};

} // namespace

TEST_F(ShaderPromoteTest, APromotedConnectedPortSurvivesASaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(bsdf && texture);
    ASSERT_FALSE(hasPort(bsdf, "specular_tint"));
    ASSERT_TRUE(promoteNetworkPort(bsdf, "specular_tint"));
    ASSERT_TRUE(hasPort(bsdf, "specular_tint"));
    const int input = bsdf->getInputNumberFromLabel("specular_tint");
    ASSERT_GE(input, 0);
    ASSERT_TRUE(bsdf->connectInput(texture, input));
    shaderEffect(bsdf)->setOutputForInput("specular_tint", "alpha");

    KnobIPtr knob = bsdf->getKnobByName("specular_tint");
    ASSERT_TRUE(bool(knob));
    EXPECT_FALSE(knob->isEnabled(0));

    const std::string bsdfName = bsdf->getScriptName();
    const std::string textureName = texture->getScriptName();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("promote.ntpz"), &yaml));

    knob.reset();
    bsdf.reset();
    texture.reset();
    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("promote.ntpz")));
    NodePtr bsdf2 = getApp()->getNodeByFullySpecifiedName(bsdfName);
    NodePtr texture2 = getApp()->getNodeByFullySpecifiedName(textureName);
    ASSERT_TRUE(bsdf2 && texture2);
    EXPECT_TRUE(hasPort(bsdf2, "specular_tint"));
    const int input2 = bsdf2->getInputNumberFromLabel("specular_tint");
    ASSERT_GE(input2, 0);
    EXPECT_EQ(texture2, bsdf2->getInput(input2));
    EXPECT_EQ(std::string("alpha"), shaderEffect(bsdf2)->getOutputForInput("specular_tint"));
    KnobIPtr knob2 = bsdf2->getKnobByName("specular_tint");
    ASSERT_TRUE(bool(knob2));
    EXPECT_FALSE(knob2->isEnabled(0));

    bsdf2->disconnectInput(input2);
    for (int i = 0; i < knob2->getDimension(); ++i) {
        EXPECT_TRUE(knob2->isEnabled(i)) << i;
    }
}

TEST_F(ShaderPromoteTest, ALinkedKnobStaysDisabledWhenItsPortIsDisconnected)
{
    NodePtr master = create(kPrincipledBsdf);
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(master && bsdf && texture);
    const int input = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(input, 0);
    KnobIPtr knob = bsdf->getKnobByName("base_color");
    KnobIPtr masterKnob = master->getKnobByName("base_color");
    ASSERT_TRUE(knob && masterKnob);

    // Connected before the link is made, so the port disabled the knob first.
    ASSERT_TRUE(bsdf->connectInput(texture, input));
    for (int i = 0; i < knob->getDimension(); ++i) {
        ASSERT_TRUE(knob->slaveTo(i, masterKnob, i));
    }
    bsdf->disconnectInput(input);
    for (int i = 0; i < knob->getDimension(); ++i) {
        EXPECT_FALSE(knob->isEnabled(i)) << i;
    }
}

TEST_F(ShaderPromoteTest, DemotingAConnectedPortDisconnectsItAndEnablesItsKnob)
{
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(bsdf && texture);
    ASSERT_TRUE(hasPort(bsdf, "base_color"));
    const int input = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(input, 0);
    ASSERT_TRUE(bsdf->connectInput(texture, input));
    KnobIPtr knob = bsdf->getKnobByName("base_color");
    ASSERT_TRUE(bool(knob));
    EXPECT_FALSE(knob->isEnabled(0));

    const int inputsBefore = bsdf->getNInputs();
    ASSERT_TRUE(demoteNetworkPort(bsdf, "base_color"));
    EXPECT_FALSE(hasPort(bsdf, "base_color"));
    EXPECT_EQ(-1, bsdf->getInputNumberFromLabel("base_color"));
    EXPECT_EQ(inputsBefore - 1, bsdf->getNInputs());
    for (int i = 0; i < bsdf->getNInputs(); ++i) {
        EXPECT_NE(texture, bsdf->getInput(i)) << i;
    }
    EXPECT_TRUE(knob->isEnabled(0));
}

TEST_F(ShaderPromoteTest, KnobOnlyAttributesRefusePromotionAndPortOnlyOnesRefuseDemotion)
{
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr mix = create(kMixClosure);

    ASSERT_TRUE(bsdf && mix);
    const std::vector<std::string> bsdfPorts = shaderEffect(bsdf)->getPorts();
    EXPECT_FALSE(promoteNetworkPort(bsdf, "distribution"));
    EXPECT_FALSE(promoteNetworkPort(bsdf, "no_such_attribute"));
    EXPECT_EQ(bsdfPorts, shaderEffect(bsdf)->getPorts());
    EXPECT_TRUE(bool(bsdf->getKnobByName("distribution")));

    const std::vector<std::string> mixPorts = shaderEffect(mix)->getPorts();
    EXPECT_FALSE(demoteNetworkPort(mix, "closure1"));
    EXPECT_FALSE(demoteNetworkPort(mix, "closure2"));
    EXPECT_EQ(mixPorts, shaderEffect(mix)->getPorts());
    EXPECT_GE(mix->getInputNumberFromLabel("closure1"), 0);
    EXPECT_GE(mix->getInputNumberFromLabel("closure2"), 0);
}

TEST_F(ShaderPromoteTest, UndoRestoresPortsConnectionsAndOutputs)
{
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(bsdf && texture);
    NetworkNode* effect = shaderEffect(bsdf);
    ASSERT_TRUE(effect);
    const int input = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(input, 0);
    ASSERT_TRUE(bsdf->connectInput(texture, input));
    effect->setOutputForInput("base_color", "alpha");
    const std::vector<std::string> before = effect->getPorts();

    std::vector<std::string> after = before;
    after.erase(std::find(after.begin(), after.end(), std::string("base_color")));
    after.push_back("specular_tint");
    NetworkEditPortsCommand command(bsdf, after, "Edit Ports");

    command.redo();
    EXPECT_FALSE(hasPort(bsdf, "base_color"));
    EXPECT_TRUE(hasPort(bsdf, "specular_tint"));
    EXPECT_TRUE(bsdf->getKnobByName("base_color")->isEnabled(0));

    command.undo();
    EXPECT_EQ(before, effect->getPorts());
    const int restored = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(restored, 0);
    EXPECT_EQ(texture, bsdf->getInput(restored));
    EXPECT_EQ(std::string("alpha"), effect->getOutputForInput("base_color"));
    EXPECT_FALSE(bsdf->getKnobByName("base_color")->isEnabled(0));
    EXPECT_FALSE(hasPort(bsdf, "specular_tint"));

    command.redo();
    EXPECT_EQ(after, effect->getPorts());
}

TEST_F(ShaderPromoteTest, PythonPromotesAndDemotesByScriptName)
{
    NodePtr bsdf = create(kPrincipledBsdf);

    ASSERT_TRUE(bool(bsdf));
    const std::string node = getApp()->getAppIDString() + "." + bsdf->getScriptName();
    std::string script = "node = " + node + "\n"
                                            "if 'specular_tint' in node.getPorts():\n"
                                            "    raise RuntimeError('already a port')\n"
                                            "if 'specular_tint' not in node.getPortAttributes():\n"
                                            "    raise RuntimeError('not a port attribute')\n"
                                            "if not node.promotePort('specular_tint'):\n"
                                            "    raise RuntimeError('promote refused')\n"
                                            "if node.getPorts()[-1] != 'specular_tint':\n"
                                            "    raise RuntimeError('not appended')\n"
                                            "if node.promotePort('distribution'):\n"
                                            "    raise RuntimeError('knob-only attribute promoted')\n"
                                            "if not node.setOutputForInput('specular_tint', 'alpha'):\n"
                                            "    raise RuntimeError('output refused')\n"
                                            "if node.getOutputForInput('specular_tint') != 'alpha':\n"
                                            "    raise RuntimeError('output not kept')\n"
                                            "if not node.demotePort('base_color'):\n"
                                            "    raise RuntimeError('demote refused')\n"
                                            "if 'base_color' in node.getPorts():\n"
                                            "    raise RuntimeError('still a port')\n";
    std::string error;
    std::string output;

    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(hasPort(bsdf, "specular_tint"));
    EXPECT_FALSE(hasPort(bsdf, "base_color"));
}

// Promoting and demoting through Python, with the demoted port connected, records one step whose
// undo brings back the ports, the connection and the output the input read, and whose redo
// removes them again.
TEST_F(ShaderPromoteTest, PythonPortEditsUndoThroughTheUndoHistory)
{
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(bsdf && texture);
    NetworkNode* effect = shaderEffect(bsdf);
    ASSERT_TRUE(effect);
    const int input = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(input, 0);
    ASSERT_TRUE(bsdf->connectInput(texture, input));
    effect->setOutputForInput("base_color", "alpha");
    const std::vector<std::string> before = effect->getPorts();

    std::shared_ptr<CollectingRecorder> recorder = std::make_shared<CollectingRecorder>();
    getApp()->setUndoRecorder(recorder);
    const std::string node = getApp()->getAppIDString() + "." + bsdf->getScriptName();
    std::string error;
    std::string output;
    {
        UndoStepGuard step(getApp(), "test");
        ASSERT_TRUE(interpretPythonScript("node = " + node + "\n"
                                                             "if not node.promotePort('specular_tint'):\n"
                                                             "    raise RuntimeError('promote refused')\n"
                                                             "if not node.demotePort('base_color'):\n"
                                                             "    raise RuntimeError('demote refused')\n",
                                          &error, &output))
            << error;
    }
    EXPECT_TRUE(error.empty()) << error;
    ASSERT_EQ(2u, recorder->commands.size());
    const std::vector<std::string> after = effect->getPorts();
    EXPECT_FALSE(hasPort(bsdf, "base_color"));
    EXPECT_TRUE(hasPort(bsdf, "specular_tint"));
    EXPECT_TRUE(texture->getOutputs().empty());
    EXPECT_EQ(std::string(), effect->getOutputForInput("base_color"));

    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::reverse_iterator it = recorder->commands.rbegin(); it != recorder->commands.rend(); ++it) {
            (*it)->undo();
        }
    }
    EXPECT_EQ(before, effect->getPorts());
    const int restored = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(restored, 0);
    EXPECT_EQ(texture, bsdf->getInput(restored));
    EXPECT_EQ(std::string("alpha"), effect->getOutputForInput("base_color"));
    EXPECT_FALSE(bsdf->getKnobByName("base_color")->isEnabled(0));
    EXPECT_FALSE(hasPort(bsdf, "specular_tint"));

    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::iterator it = recorder->commands.begin(); it != recorder->commands.end(); ++it) {
            (*it)->redo();
        }
    }
    EXPECT_EQ(after, effect->getPorts());
    EXPECT_TRUE(texture->getOutputs().empty());
    EXPECT_TRUE(bsdf->getKnobByName("base_color")->isEnabled(0));

    recorder->commands.clear();
    getApp()->setUndoRecorder(UndoRecorderPtr());
}

// What promoteNetworkPort() and demoteNetworkPort() push on the node; with no window there is no
// history, so the test undoes the pushed command itself.
TEST_F(ShaderPromoteTest, AConnectedDemoteAndPromoteUndoThroughTheirCommand)
{
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(bsdf && texture);
    NetworkNode* effect = shaderEffect(bsdf);
    ASSERT_TRUE(effect);
    ASSERT_TRUE(bsdf->connectInput(texture, bsdf->getInputNumberFromLabel("base_color")));
    effect->setOutputForInput("base_color", "alpha");
    const std::vector<std::string> before = effect->getPorts();

    UndoCommandPtr demote;
    ASSERT_TRUE(makeNetworkPortCommand(bsdf, "base_color", false, &demote));
    ASSERT_TRUE(bool(demote));
    bsdf->pushUndoCommand(demote);
    EXPECT_FALSE(hasPort(bsdf, "base_color"));
    EXPECT_TRUE(texture->getOutputs().empty());
    EXPECT_TRUE(bsdf->getKnobByName("base_color")->isEnabled(0));
    demote->undo();
    EXPECT_EQ(before, effect->getPorts());
    EXPECT_EQ(texture, bsdf->getInput(bsdf->getInputNumberFromLabel("base_color")));
    EXPECT_EQ(std::string("alpha"), effect->getOutputForInput("base_color"));
    EXPECT_FALSE(bsdf->getKnobByName("base_color")->isEnabled(0));

    UndoCommandPtr promote;
    ASSERT_TRUE(makeNetworkPortCommand(bsdf, "specular_tint", true, &promote));
    ASSERT_TRUE(bool(promote));
    bsdf->pushUndoCommand(promote);
    const int specularTint = bsdf->getInputNumberFromLabel("specular_tint");
    ASSERT_GE(specularTint, 0);
    ASSERT_TRUE(bsdf->connectInput(texture, specularTint));
    EXPECT_FALSE(bsdf->getKnobByName("specular_tint")->isEnabled(0));
    promote->undo();
    EXPECT_EQ(before, effect->getPorts());
    EXPECT_EQ(-1, bsdf->getInputNumberFromLabel("specular_tint"));
    EXPECT_TRUE(bsdf->getKnobByName("specular_tint")->isEnabled(0));
    EXPECT_EQ(texture, bsdf->getInput(bsdf->getInputNumberFromLabel("base_color")));
    EXPECT_EQ(std::string("alpha"), effect->getOutputForInput("base_color"));

    UndoCommandPtr none;
    EXPECT_TRUE(makeNetworkPortCommand(bsdf, "base_color", true, &none));
    EXPECT_FALSE(bool(none));
    EXPECT_FALSE(makeNetworkPortCommand(bsdf, "distribution", true, &none));
}

// Python refuses an output name the connected node lacks or whose type does not convert, and a
// label that is not a port, changing nothing.
TEST_F(ShaderPromoteTest, PythonRefusesABadOutputOrInput)
{
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr texture = create(kImageTexture);

    ASSERT_TRUE(bsdf && texture);
    ASSERT_TRUE(bsdf->connectInput(texture, bsdf->getInputNumberFromLabel("base_color")));
    const std::string node = getApp()->getAppIDString() + "." + bsdf->getScriptName();
    std::string error;
    std::string output;
    ASSERT_TRUE(interpretPythonScript("node = " + node + "\n"
                                                         "if node.setOutputForInput('base_color', 'no_such_output'):\n"
                                                         "    raise RuntimeError('unknown output taken')\n"
                                                         "if node.setOutputForInput('distribution', ''):\n"
                                                         "    raise RuntimeError('knob-only attribute taken')\n"
                                                         "if node.setOutputForInput('no_such_port', 'alpha'):\n"
                                                         "    raise RuntimeError('unknown input taken')\n"
                                                         "if not node.setOutputForInput('base_color', 'alpha'):\n"
                                                         "    raise RuntimeError('alpha refused')\n",
                                      &error, &output))
        << error;
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(std::string("alpha"), shaderEffect(bsdf)->getOutputForInput("base_color"));
}

// A closure does not convert to a colour, so the connection is refused, from C++ and from Python,
// with the reason.
TEST_F(ShaderPromoteTest, AConnectionWhoseTypesDoNotConvertIsRefused)
{
    NodePtr upstream = create(kPrincipledBsdf);
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr mix = create(kMixClosure);

    ASSERT_TRUE(upstream && bsdf && mix);
    const int baseColor = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(baseColor, 0);
    EXPECT_EQ(Node::eCanConnectInput_incompatiblePorts, bsdf->canConnectInput(upstream, baseColor));
    EXPECT_EQ(NetworkNode::eConnectionTypeMismatch, NetworkNode::checkInputConnection(upstream, *bsdf, baseColor));
    EXPECT_FALSE(NetworkNode::connectionCheckReason(NetworkNode::eConnectionTypeMismatch).empty());
    EXPECT_FALSE(NodeCollection::connectNodes(baseColor, upstream, bsdf));
    EXPECT_FALSE(bool(bsdf->getInput(baseColor)));
    EXPECT_EQ(Node::eCanConnectInput_ok, mix->canConnectInput(upstream, mix->getInputNumberFromLabel("closure1")));

    const std::string script = "up = " + getApp()->getAppIDString() + "." + upstream->getScriptName() + "\n"
                                                                                                        "down = "
        + getApp()->getAppIDString() + "." + bsdf->getScriptName() + "\n"
                                                                     "import warnings\n"
                                                                     "with warnings.catch_warnings(record=True) as caught:\n"
                                                                     "    warnings.simplefilter('always')\n"
                                                                     "    if down.connectInput("
        + std::to_string(baseColor) + ", up):\n"
                                      "        raise RuntimeError('closure connected to a colour')\n"
                                      "    if not [w for w in caught if 'convert' in str(w.message)]:\n"
                                      "        raise RuntimeError('no reason given')\n";
    std::string error;
    std::string output;
    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(bool(bsdf->getInput(baseColor)));
}

TEST_F(ShaderPromoteTest, TheTypeCheckSeesThroughDots)
{
    NodePtr upstream = create(kPrincipledBsdf);
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr dot = create(PLUGINID_NATRON_DOT);
    NodePtr secondDot = create(PLUGINID_NATRON_DOT);

    ASSERT_TRUE(upstream && bsdf && dot && secondDot);
    ASSERT_TRUE(dot->connectInput(upstream, 0));
    ASSERT_TRUE(secondDot->connectInput(dot, 0));
    const int baseColor = bsdf->getInputNumberFromLabel("base_color");
    ASSERT_GE(baseColor, 0);
    EXPECT_EQ(NetworkNode::eConnectionTypeMismatch, NetworkNode::checkInputConnection(secondDot, *bsdf, baseColor));
    EXPECT_EQ(Node::eCanConnectInput_incompatiblePorts, bsdf->canConnectInput(secondDot, baseColor));
    EXPECT_FALSE(NodeCollection::connectNodes(baseColor, secondDot, bsdf));
    EXPECT_FALSE(bool(bsdf->getInput(baseColor)));
}
