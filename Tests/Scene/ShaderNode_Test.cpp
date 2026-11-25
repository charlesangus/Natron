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

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Scene/ShaderManifestTestUtils.h"
#include "Serialization/ProjectBundleTestHelpers.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kPrincipledBsdf = "cycles_principled_bsdf";
const char* const kDwaBaseMaterial = "DwaBaseMaterial";

std::vector<double>
flatNumbers(const QJsonValue& value)
{
    std::vector<double> ret;

    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            const std::vector<double> inner = flatNumbers(item);
            ret.insert(ret.end(), inner.begin(), inner.end());
        }
    } else if (value.isBool()) {
        ret.push_back(value.toBool() ? 1. : 0.);
    } else {
        ret.push_back(value.toDouble());
    }

    return ret;
}

std::string
rowsText(const QJsonValue& value)
{
    std::string ret;

    for (const QJsonValue& row : value.toArray()) {
        std::string line;
        for (double number : flatNumbers(row)) {
            std::ostringstream out;
            out << number;
            line += (line.empty() ? "" : " ") + out.str();
        }
        ret += (ret.empty() ? "" : "\n") + line;
    }

    return ret;
}

void
expectDoubles(const KnobIPtr& knob,
              int dimensions,
              const QJsonValue& value,
              const std::string& name)
{
    KnobDoubleBasePtr k = std::dynamic_pointer_cast<KnobDoubleBase>(knob);

    ASSERT_TRUE(bool(k)) << name;
    ASSERT_EQ(dimensions, k->getDimension()) << name;
    const std::vector<double> expected = flatNumbers(value);
    ASSERT_EQ((std::size_t)dimensions, expected.size()) << name;
    for (int d = 0; d < dimensions; ++d) {
        EXPECT_DOUBLE_EQ(expected[d], k->getDefaultValue(d)) << name << "[" << d << "]";
    }
}

// Every value attribute of the manifest node is a knob of the type its manifest type maps to,
// with the manifest's default; every port-only attribute is an input and has no knob.
void
expectEveryAttributeIsAKnob(const NodePtr& node,
                            const QJsonObject& manifestNode)
{
    const QJsonArray attributes = manifestNode.value(QLatin1String("attributes")).toArray();

    ASSERT_FALSE(attributes.isEmpty());
    for (const QJsonValue& item : attributes) {
        const QJsonObject json = item.toObject();
        const std::string name = json.value(QLatin1String("name")).toString().toStdString();
        const std::string type = json.value(QLatin1String("type")).toString().toStdString();
        const QJsonValue value = json.value(QLatin1String("default"));
        const QJsonArray choices = json.value(QLatin1String("enum")).toArray();
        KnobIPtr knob = node->getKnobByName(NetworkNode::knobNameOf(name));

        if (json.value(QLatin1String("port_only")).toBool()) {
            EXPECT_FALSE(bool(knob)) << name;
            EXPECT_GE(node->getInputNumberFromLabel(name), 0) << name;
            continue;
        }
        if (!knob) {
            ADD_FAILURE() << name << " has no knob";
            continue;
        }
        if ((type == "float") || (type == "Float")) {
            EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(knob))) << name;
            expectDoubles(knob, 1, value, name);
        } else if ((type == "color") || (type == "Rgb")) {
            EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobColor>(knob))) << name;
            expectDoubles(knob, 3, value, name);
        } else if ((type == "vector") || (type == "normal") || (type == "point") || (type == "Vec3f") || (type == "Vec3d")) {
            EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(knob))) << name;
            expectDoubles(knob, 3, value, name);
        } else if (type == "Vec2f") {
            EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(knob))) << name;
            expectDoubles(knob, 2, value, name);
        } else if (type == "transform") {
            expectDoubles(knob, 12, value, name);
        } else if (type == "Mat4d") {
            expectDoubles(knob, 16, value, name);
        } else if ((type == "enum") || ((type == "Int") && !choices.isEmpty())) {
            KnobChoicePtr k = std::dynamic_pointer_cast<KnobChoice>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not a choice";
                continue;
            }
            const std::vector<ChoiceOption> entries = k->getEntries_mt_safe();
            ASSERT_EQ((std::size_t)choices.size(), entries.size()) << name;
            const int index = k->getDefaultValue(0);
            ASSERT_GE(index, 0) << name;
            ASSERT_LT(index, (int)entries.size()) << name;
            std::string expected;
            for (const QJsonValue& option : choices) {
                const QJsonObject o = option.toObject();
                const bool isDefault = (type == "enum") ? (o.value(QLatin1String("name")).toString() == value.toString())
                                                        : (o.value(QLatin1String("value")).toDouble() == value.toDouble());
                if (isDefault) {
                    expected = o.value(QLatin1String("name")).toString().toStdString();
                }
            }
            EXPECT_EQ(expected, entries[index].id) << name;
        } else if ((type == "int") || (type == "Int")) {
            KnobIntPtr k = std::dynamic_pointer_cast<KnobInt>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not an int";
                continue;
            }
            EXPECT_EQ((int)value.toDouble(), k->getDefaultValue(0)) << name;
        } else if ((type == "boolean") || (type == "Bool")) {
            KnobBoolPtr k = std::dynamic_pointer_cast<KnobBool>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not a bool";
                continue;
            }
            EXPECT_EQ(value.toBool(), k->getDefaultValue(0)) << name;
        } else if (((type == "String") && json.contains(QLatin1String("asset")))) {
            KnobFilePtr k = std::dynamic_pointer_cast<KnobFile>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not a file";
                continue;
            }
            EXPECT_EQ(value.toString().toStdString(), k->getDefaultValue(0)) << name;
        } else if ((type == "string") || (type == "String")) {
            KnobStringPtr k = std::dynamic_pointer_cast<KnobString>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not a string";
                continue;
            }
            EXPECT_FALSE(k->isMultiLine()) << name;
            EXPECT_EQ(value.toString().toStdString(), k->getDefaultValue(0)) << name;
        } else if ((type == "array_int") || (type == "FloatVector") || (type == "IntVector") || (type == "RgbVector")) {
            KnobStringPtr k = std::dynamic_pointer_cast<KnobString>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not a table";
                continue;
            }
            EXPECT_TRUE(k->isMultiLine()) << name;
            EXPECT_EQ(rowsText(value), k->getDefaultValue(0)) << name;
        } else if ((type == "array_float") || (type == "array_color") || (type == "array_vector")) {
            KnobParametricPtr k = std::dynamic_pointer_cast<KnobParametric>(knob);
            if (!k) {
                ADD_FAILURE() << name << " is not parametric";
                continue;
            }
            EXPECT_EQ((type == "array_float") ? 1 : 3, k->getDimension()) << name;
        } else {
            ADD_FAILURE() << name << " has the type " << type << ", which this test does not know";
        }
    }
}

class ShaderNodeTest
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

TEST_F(ShaderNodeTest, PrincipledBsdfHasEveryManifestAttribute)
{
    NodePtr node = create(kPrincipledBsdf);

    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(shaderEffect(node));
    expectEveryAttributeIsAKnob(node, shaderManifestNode("cycles", kPrincipledBsdf));

    // A node with key ports starts with those as its inputs, in order.
    const QJsonArray keyPorts = shaderLayoutFile("cycles").value(QLatin1String("nodes")).toObject().value(QLatin1String(kPrincipledBsdf)).toObject().value(QLatin1String("key_ports")).toArray();
    ASSERT_FALSE(keyPorts.isEmpty());
    ASSERT_EQ(keyPorts.size(), node->getNInputs());
    for (int i = 0; i < keyPorts.size(); ++i) {
        EXPECT_EQ(keyPorts[i].toString().toStdString(), node->getInputLabel(i));
    }

    KnobIPtr baseColor = node->getKnobByName("base_color");
    ASSERT_TRUE(bool(baseColor));
    EXPECT_EQ(std::string("Base Color"), baseColor->getLabel());
    KnobPagePtr page = baseColor->getTopLevelPage();
    ASSERT_TRUE(bool(page));
    EXPECT_EQ(std::string("Base"), page->getLabel());
    EXPECT_EQ(std::string("IOR"), node->getKnobByName("ior")->getLabel());

    const std::vector<OutputPortDescriptor> outputs = node->getOutputPorts();
    ASSERT_EQ(1u, outputs.size());
    EXPECT_EQ(std::string("BSDF"), outputs[0].name);
    EXPECT_EQ(PortTypeClass::Closure, outputs[0].typeClass);
}

TEST_F(ShaderNodeTest, DwaBaseMaterialHasEveryManifestAttribute)
{
    NodePtr node = create(kDwaBaseMaterial);

    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(shaderEffect(node));
    expectEveryAttributeIsAKnob(node, shaderManifestNode("moonray", kDwaBaseMaterial));

    const std::vector<OutputPortDescriptor> outputs = node->getOutputPorts();
    ASSERT_EQ(1u, outputs.size());
    EXPECT_EQ(std::string("out"), outputs[0].name);
    EXPECT_EQ(std::string("DwaBaseLayerable"), outputs[0].type);
    EXPECT_EQ(PortTypeClass::Closure, outputs[0].typeClass);
}

TEST_F(ShaderNodeTest, ASaveAndLoadRestoresEditedValues)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr bsdf = create(kPrincipledBsdf);
    NodePtr dwa = create(kDwaBaseMaterial);

    ASSERT_TRUE(bsdf && dwa);
    KnobColorPtr baseColor = std::dynamic_pointer_cast<KnobColor>(bsdf->getKnobByName("base_color"));
    KnobDoublePtr roughness = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("roughness"));
    KnobChoicePtr distribution = std::dynamic_pointer_cast<KnobChoice>(bsdf->getKnobByName("distribution"));
    KnobDoublePtr tangent = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("tangent"));
    ASSERT_TRUE(baseColor && roughness && distribution && tangent);
    baseColor->setValue(0.1, ViewSpec::all(), 0);
    baseColor->setValue(0.2, ViewSpec::all(), 1);
    baseColor->setValue(0.3, ViewSpec::all(), 2);
    roughness->setValue(0.125);
    ASSERT_NE(0, distribution->getDefaultValue(0));
    distribution->setValue(0);
    tangent->setValue(0.75, ViewSpec::all(), 1);

    KnobDoublePtr clearcoatRoughness = std::dynamic_pointer_cast<KnobDouble>(dwa->getKnobByName("clearcoat_roughness"));
    KnobStringPtr label = std::dynamic_pointer_cast<KnobString>(dwa->getKnobByName("label"));
    ASSERT_TRUE(clearcoatRoughness && label);
    clearcoatRoughness->setValue(0.625);
    label->setValue("chrome");

    const std::string bsdfName = bsdf->getScriptName();
    const std::string dwaName = dwa->getScriptName();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("shaders.ntpz"), &yaml));

    baseColor.reset();
    roughness.reset();
    distribution.reset();
    tangent.reset();
    clearcoatRoughness.reset();
    label.reset();
    bsdf.reset();
    dwa.reset();
    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("shaders.ntpz")));
    NodePtr bsdf2 = getApp()->getNodeByFullySpecifiedName(bsdfName);
    NodePtr dwa2 = getApp()->getNodeByFullySpecifiedName(dwaName);
    ASSERT_TRUE(bsdf2 && dwa2);
    KnobColorPtr baseColor2 = std::dynamic_pointer_cast<KnobColor>(bsdf2->getKnobByName("base_color"));
    ASSERT_TRUE(bool(baseColor2));
    EXPECT_DOUBLE_EQ(0.1, baseColor2->getValue(0));
    EXPECT_DOUBLE_EQ(0.2, baseColor2->getValue(1));
    EXPECT_DOUBLE_EQ(0.3, baseColor2->getValue(2));
    EXPECT_DOUBLE_EQ(0.125, std::dynamic_pointer_cast<KnobDouble>(bsdf2->getKnobByName("roughness"))->getValue());
    EXPECT_EQ(0, std::dynamic_pointer_cast<KnobChoice>(bsdf2->getKnobByName("distribution"))->getValue());
    EXPECT_DOUBLE_EQ(0.75, std::dynamic_pointer_cast<KnobDouble>(bsdf2->getKnobByName("tangent"))->getValue(1));
    EXPECT_DOUBLE_EQ(0.625, std::dynamic_pointer_cast<KnobDouble>(dwa2->getKnobByName("clearcoat_roughness"))->getValue());
    EXPECT_EQ(std::string("chrome"), std::dynamic_pointer_cast<KnobString>(dwa2->getKnobByName("label"))->getValue());
}

TEST_F(ShaderNodeTest, AShaderKnobAnimatesAndTakesAnExpression)
{
    NodePtr bsdf = create(kPrincipledBsdf);

    ASSERT_TRUE(bool(bsdf));
    KnobDoublePtr roughness = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("roughness"));
    KnobDoublePtr metallic = std::dynamic_pointer_cast<KnobDouble>(bsdf->getKnobByName("metallic"));
    ASSERT_TRUE(roughness && metallic);
    EXPECT_TRUE(roughness->isAnimationEnabled());

    roughness->setValueAtTime(1, 0.2, ViewSpec::all(), 0);
    roughness->setValueAtTime(11, 0.6, ViewSpec::all(), 0);
    EXPECT_TRUE(roughness->isAnimated(0));
    EXPECT_DOUBLE_EQ(0.2, roughness->getValueAtTime(1));
    EXPECT_DOUBLE_EQ(0.6, roughness->getValueAtTime(11));
    const double middle = roughness->getValueAtTime(6);
    EXPECT_GT(middle, 0.2);
    EXPECT_LT(middle, 0.6);

    ASSERT_NO_THROW(metallic->setExpression(0, "thisNode.roughness.getValue() * 2", false, true));
    EXPECT_FALSE(metallic->getExpression(0).empty());
    EXPECT_NEAR(0.4, metallic->getValueAtTime(1), 1e-9);
    EXPECT_NEAR(1.2, metallic->getValueAtTime(11), 1e-9);
}
