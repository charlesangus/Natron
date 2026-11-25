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
#include <list>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Scene/ShaderManifestTestUtils.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kPrincipledBsdf = "cycles_principled_bsdf";
const char* const kImageTexture = "cycles_image_texture";
const char* const kMixClosure = "cycles_mix_closure";
const char* const kRemovedShader = "cycles_removed_shader";
const double kRoughness = 0.375;

Serialization::NodeSerializationPtr
findSerializedNodeByName(const std::list<Serialization::NodeSerializationPtr>& nodes,
                         const std::string& scriptName)
{
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if (node->scriptName == scriptName) {
            return node;
        }
    }

    return Serialization::NodeSerializationPtr();
}

} // anonymous namespace

class ShaderMissingTest
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

    // texture -> Principled BSDF (base_color and the promoted specular_tint, reading alpha) ->
    // Mix Closure's closure1, with an edited roughness.
    void buildGraph()
    {
        ProjectPtr project = getApp()->getProject();
        NodePtr texture = createShaderNode(getApp(), kImageTexture, project);
        NodePtr bsdf = createShaderNode(getApp(), kPrincipledBsdf, project);
        NodePtr mix = createShaderNode(getApp(), kMixClosure, project);
        ASSERT_TRUE(texture && bsdf && mix);

        NetworkNode* effect = shaderEffect(bsdf);
        ASSERT_TRUE(effect);
        ASSERT_TRUE(effect->promotePort("specular_tint"));
        const int baseColor = bsdf->getInputNumberFromLabel("base_color");
        const int specularTint = bsdf->getInputNumberFromLabel("specular_tint");
        const int closure1 = mix->getInputNumberFromLabel("closure1");
        ASSERT_TRUE(baseColor >= 0 && specularTint >= 0 && closure1 >= 0);
        ASSERT_TRUE(bsdf->connectInput(texture, baseColor));
        ASSERT_TRUE(bsdf->connectInput(texture, specularTint));
        effect->setOutputForInput("specular_tint", "alpha");
        ASSERT_TRUE(mix->connectInput(bsdf, closure1));

        KnobDoublePtr roughness = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("roughness"));
        ASSERT_TRUE(bool(roughness));
        roughness->setValue(kRoughness);

        _textureName = texture->getScriptName_mt_safe();
        _bsdfName = bsdf->getScriptName_mt_safe();
        _mixName = mix->getScriptName_mt_safe();
    }

    std::string _textureName;
    std::string _bsdfName;
    std::string _mixName;
};

TEST_F(ShaderMissingTest, AnUnknownShaderRoundTripsWithItsConnections)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    ProjectPtr project = getApp()->getProject();
    Serialization::ProjectSerialization saved;
    project->toSerialization(&saved);
    Serialization::NodeSerializationPtr original = findSerializedNode(saved.nodes.nodes, kPrincipledBsdf);
    ASSERT_TRUE(bool(original));
    original->pluginID = kRemovedShader;
    ASSERT_EQ(2u, original->inputs.size());
    ASSERT_FALSE(original->knobs.empty());

    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("missing-shader.ntpz"), saved));

    NodePtr placeholder = project->getNodeByName(_bsdfName);
    NodePtr texture = project->getNodeByName(_textureName);
    NodePtr mix = project->getNodeByName(_mixName);
    ASSERT_TRUE(placeholder && texture && mix);
    ASSERT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());
    for (const char* label : { "base_color", "specular_tint" }) {
        const int input = placeholder->getInputNumberFromLabel(label);
        ASSERT_GE(input, 0) << label;
        EXPECT_EQ(texture, placeholder->getInput(input)) << label;
    }
    const int closure1 = mix->getInputNumberFromLabel("closure1");
    ASSERT_GE(closure1, 0);
    EXPECT_EQ(placeholder, mix->getInput(closure1));

    Serialization::ProjectSerialization resaved;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("missing-shader-resaved.ntpz"), nullptr, &resaved));
    Serialization::NodeSerializationPtr resavedMissing = findSerializedNode(resaved.nodes.nodes, kRemovedShader);
    ASSERT_TRUE(bool(resavedMissing));
    EXPECT_TRUE(findSerializedNode(resaved.nodes.nodes, PLUGINID_NATRON_MISSING_PLUGIN) == Serialization::NodeSerializationPtr());
    EXPECT_EQ(nodeYaml(*original), nodeYaml(*resavedMissing));

    Serialization::NodeSerializationPtr resavedMix = findSerializedNodeByName(resaved.nodes.nodes, _mixName);
    ASSERT_TRUE(bool(resavedMix));
    const std::map<std::string, std::string>::const_iterator mixInput = resavedMix->inputs.find("closure1");
    ASSERT_TRUE(mixInput != resavedMix->inputs.end());
    EXPECT_EQ(_bsdfName, mixInput->second);

    // With the shader back, the file loads as if it had never been missing.
    resavedMissing->pluginID = kPrincipledBsdf;
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("shader-back.ntpz"), resaved));

    NodePtr bsdf = project->getNodeByName(_bsdfName);
    texture = project->getNodeByName(_textureName);
    mix = project->getNodeByName(_mixName);
    ASSERT_TRUE(bsdf && texture && mix);
    NetworkNode* effect = shaderEffect(bsdf);
    ASSERT_TRUE(effect);
    const std::vector<std::string> ports = effect->getPorts();
    EXPECT_TRUE(std::find(ports.begin(), ports.end(), std::string("specular_tint")) != ports.end());
    for (const char* label : { "base_color", "specular_tint" }) {
        const int input = bsdf->getInputNumberFromLabel(label);
        ASSERT_GE(input, 0) << label;
        EXPECT_EQ(texture, bsdf->getInput(input)) << label;
    }
    EXPECT_EQ(std::string("alpha"), effect->getOutputForInput("specular_tint"));
    EXPECT_EQ(bsdf, mix->getInput(mix->getInputNumberFromLabel("closure1")));
    KnobDoublePtr roughness = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("roughness"));
    ASSERT_TRUE(bool(roughness));
    EXPECT_DOUBLE_EQ(kRoughness, roughness->getValue());
}

TEST_F(ShaderMissingTest, ASavedAttributeTheShaderLacksLoadsWithAWarningNamingIt)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    ProjectPtr project = getApp()->getProject();
    Serialization::ProjectSerialization saved;
    project->toSerialization(&saved);
    Serialization::NodeSerializationPtr original = findSerializedNode(saved.nodes.nodes, kPrincipledBsdf);
    ASSERT_TRUE(bool(original));
    std::list<Serialization::KnobSerialization>::const_iterator roughness = original->knobs.begin();
    while ((roughness != original->knobs.end()) && (roughness->name != "roughness")) {
        ++roughness;
    }
    ASSERT_TRUE(roughness != original->knobs.end());
    Serialization::KnobSerialization retired = *roughness;
    retired.name = "retired_attribute";
    original->knobs.push_back(retired);

    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("retired-attribute.ntpz"), saved));

    const QString bsdfName = QString::fromUtf8(_bsdfName.c_str());
    std::vector<std::string> nodeWarnings;
    for (const QString& warning : project->getLoadWarnings()) {
        if (warning.contains(bsdfName)) {
            nodeWarnings.push_back(warning.toStdString());
        }
    }
    ASSERT_EQ(1u, nodeWarnings.size());
    EXPECT_NE(std::string::npos, nodeWarnings[0].find("retired_attribute")) << nodeWarnings[0];

    NodePtr bsdf = project->getNodeByName(_bsdfName);
    ASSERT_TRUE(bool(bsdf));
    EXPECT_EQ(std::string(kPrincipledBsdf), bsdf->getPluginID());
    KnobDoublePtr loadedRoughness = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("roughness"));
    ASSERT_TRUE(bool(loadedRoughness));
    EXPECT_DOUBLE_EQ(kRoughness, loadedRoughness->getValue());
}
