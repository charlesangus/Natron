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
#include <memory>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/ClipboardYaml.h"
#include "Serialization/Yaml/KnobYaml.h"
#include "Serialization/Yaml/YamlScalars.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::ClipboardSerialization;
using Serialization::KnobSerialization;
using Serialization::KnobValueSerialization;
using Serialization::NodeSerialization;
using Serialization::NodeSerializationPtr;
using Serialization::YamlError;
using Serialization::YamlWarnings;

namespace {

NodeSerializationPtr
makeNode(const std::string& scriptName,
         const std::string& pluginID)
{
    NodeSerializationPtr node = std::make_shared<NodeSerialization>();
    node->scriptName = scriptName;
    node->label = scriptName;
    node->pluginID = pluginID;
    node->pluginMajorVersion = 4;
    node->pluginMinorVersion = 0;

    return node;
}

// A two-dimensional knob with both dimensions equal, folded to one value the way the Engine
// saves it.
KnobSerialization
foldedDoubleKnob(const std::string& name,
                 double value)
{
    KnobSerialization knob;
    knob.name = name;
    knob.typeName = "Double";
    knob.dimension = 2;
    knob.dimensions.resize(1);
    knob.dimensions[0].value = KnobValueSerialization(std::in_place_type<double>, value);

    return knob;
}

void
setPosition(const NodeSerializationPtr& node,
            double x,
            double y)
{
    node->layout.hasPosition = true;
    node->layout.x = x;
    node->layout.y = y;
    node->layout.hasSize = true;
    node->layout.width = 104.;
    node->layout.height = 34.;
}

// Blur1 feeds Grade1, whose mask comes from Read1, a node that was not copied.
ClipboardSerialization
blurIntoGrade()
{
    ClipboardSerialization clipboard;
    NodeSerializationPtr grade = makeNode("Grade1", "net.sf.openfx.GradePlugin");
    grade->inputs["Source"] = "Blur1";
    grade->inputs["Mask"] = "Read1";
    NodeSerializationPtr blur = makeNode("Blur1", "net.sf.cimg.CImgBlur");
    blur->knobs.push_back(foldedDoubleKnob("size", 3.5));
    clipboard.nodes = { grade, blur };
    setPosition(grade, 10., 120.);
    setPosition(blur, 10.5, -40.25);

    return clipboard;
}

NodeSerializationPtr
findNode(const ClipboardSerialization& clipboard,
         const std::string& scriptName)
{
    for (const NodeSerializationPtr& node : clipboard.nodes) {
        if (node->scriptName == scriptName) {
            return node;
        }
    }

    return NodeSerializationPtr();
}

void
replaceOnce(std::string* text,
            const std::string& from,
            const std::string& to)
{
    const std::size_t at = text->find(from);
    ASSERT_NE(std::string::npos, at) << "\"" << from << "\" is not in\n"
                                     << *text;
    text->replace(at, from.size(), to);
}

} // anonymous namespace

TEST(ClipboardYaml, AnEditedBlurSizeAndTheBlurToGradeConnectionSurviveTheText)
{
    std::string text = Serialization::encodeClipboardYaml(blurIntoGrade());
    EXPECT_EQ(0u, text.find("format: natron-clipboard\nformatVersion: 1\n")) << text;

    replaceOnce(&text, "size: 3.5", "size: 12.25");

    YamlWarnings warnings;
    const ClipboardSerialization decoded = Serialization::decodeClipboardYaml(text, &warnings);
    EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? std::string() : warnings.front());
    ASSERT_EQ(2u, decoded.nodes.size());

    const NodeSerializationPtr blur = findNode(decoded, "Blur1");
    ASSERT_NE(nullptr, blur);
    ASSERT_TRUE(blur->layout.hasPosition);
    EXPECT_SAME_DOUBLE(10.5, blur->layout.x);
    EXPECT_SAME_DOUBLE(-40.25, blur->layout.y);
    ASSERT_EQ(1u, blur->knobs.size());
    KnobSerialization size = blur->knobs.front();
    Serialization::completeKnobSerialization(&size, "Double", 2);
    ASSERT_FALSE(size.dimensions.empty());
    ASSERT_TRUE(size.dimensions[0].value.has_value());
    ASSERT_TRUE(std::holds_alternative<double>(*size.dimensions[0].value));
    EXPECT_SAME_DOUBLE(12.25, std::get<double>(*size.dimensions[0].value));

    const NodeSerializationPtr grade = findNode(decoded, "Grade1");
    ASSERT_NE(nullptr, grade);
    ASSERT_TRUE(grade->layout.hasPosition);
    EXPECT_SAME_DOUBLE(120., grade->layout.y);
    ASSERT_EQ(1u, grade->inputs.count("Source"));
    EXPECT_EQ("Blur1", grade->inputs.at("Source"));
    ASSERT_EQ(1u, grade->inputs.count("Mask"));
    EXPECT_EQ("Read1", grade->inputs.at("Mask"));
}

TEST(ClipboardYaml, AGroupKeepsThePositionsOfItsChildren)
{
    ClipboardSerialization clipboard;
    NodeSerializationPtr group = makeNode("Group1", "fr.inria.built-in.Group");
    NodeSerializationPtr output = makeNode("Output1", "fr.inria.built-in.Output");
    output->inputs["Source"] = "Blur2";
    NodeSerializationPtr blur = makeNode("Blur2", "net.sf.cimg.CImgBlur");
    NodeSerializationPtr input = makeNode("Input1", "fr.inria.built-in.Input");
    blur->inputs["Source"] = "Input1";
    group->children = { output, blur, input };
    clipboard.nodes = { group };
    setPosition(group, 0., 0.);
    setPosition(input, 1., 2.);
    setPosition(blur, 3., 4.);

    YamlWarnings warnings;
    const ClipboardSerialization decoded = Serialization::decodeClipboardYaml(Serialization::encodeClipboardYaml(clipboard), &warnings);
    EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? std::string() : warnings.front());
    ASSERT_EQ(1u, decoded.nodes.size());
    const NodeSerialization& decodedGroup = *decoded.nodes.front();
    ASSERT_EQ(3u, decodedGroup.children.size());
    for (const NodeSerializationPtr& child : decodedGroup.children) {
        if (child->scriptName == "Input1") {
            ASSERT_TRUE(child->layout.hasPosition);
            EXPECT_SAME_DOUBLE(1., child->layout.x);
            EXPECT_SAME_DOUBLE(2., child->layout.y);
        } else if (child->scriptName == "Blur2") {
            ASSERT_TRUE(child->layout.hasPosition);
            EXPECT_SAME_DOUBLE(3., child->layout.x);
            EXPECT_SAME_DOUBLE(4., child->layout.y);
            EXPECT_EQ("Input1", child->inputs.at("Source"));
        } else {
            EXPECT_FALSE(child->layout.hasPosition);
        }
    }
}

TEST(ClipboardYaml, OtherTextIsRefused)
{
    EXPECT_THROW(Serialization::decodeClipboardYaml("just some text someone copied"), YamlError);
    EXPECT_THROW(Serialization::decodeClipboardYaml(""), YamlError);
    EXPECT_THROW(Serialization::decodeClipboardYaml("key: [unbalanced"), YamlError);
    EXPECT_THROW(Serialization::decodeClipboardYaml("format: natron-preset\nformatVersion: 1\nnodes: []\n"), YamlError);
    EXPECT_THROW(Serialization::decodeClipboardYaml("<?xml version=\"1.0\" ?>\n<!DOCTYPE boost_serialization>\n<boost_serialization><Clipboard/></boost_serialization>\n"), YamlError);
}
