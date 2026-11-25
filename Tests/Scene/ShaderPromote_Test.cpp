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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

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
