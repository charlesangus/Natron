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
#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/Yaml/ClipboardYaml.h"
#include "Serialization/Yaml/KnobYaml.h"
#include "Serialization/Yaml/YamlScalars.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::ClipboardSerialization;
using Serialization::KnobSerialization;
using Serialization::KnobValueSerialization;
using Serialization::NodeGuiSerialization;
using Serialization::NodeGuiSerializationPtr;
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

NodeGuiSerializationPtr
makeGui(const std::string& nodeName,
        double x,
        double y)
{
    NodeGuiSerializationPtr gui = std::make_shared<NodeGuiSerialization>();
    gui->nodeName = nodeName;
    gui->positionX = x;
    gui->positionY = y;
    gui->width = 104.;
    gui->height = 34.;

    return gui;
}

// Blur1 feeds Grade1, whose mask comes from Read1, a node that was not copied. The source nodes
// sat in Group1, so their graph states carry fully qualified names.
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
    clipboard.nodesUI = { makeGui("Group1.Grade1", 10., 120.), makeGui("Group1.Blur1", 10.5, -40.25) };

    return clipboard;
}

NodeSerializationPtr
findNode(const ClipboardSerialization& clipboard,
         const std::string& scriptName,
         NodeGuiSerializationPtr* gui)
{
    std::list<NodeGuiSerializationPtr>::const_iterator itGui = clipboard.nodesUI.begin();
    for (const NodeSerializationPtr& node : clipboard.nodes) {
        if (node->scriptName == scriptName) {
            *gui = (itGui != clipboard.nodesUI.end()) ? *itGui : NodeGuiSerializationPtr();

            return node;
        }
        if (itGui != clipboard.nodesUI.end()) {
            ++itGui;
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
    ASSERT_EQ(2u, decoded.nodesUI.size());

    NodeGuiSerializationPtr blurGui;
    const NodeSerializationPtr blur = findNode(decoded, "Blur1", &blurGui);
    ASSERT_NE(nullptr, blur);
    ASSERT_NE(nullptr, blurGui);
    EXPECT_EQ("Blur1", blurGui->nodeName);
    EXPECT_SAME_DOUBLE(10.5, blurGui->positionX);
    EXPECT_SAME_DOUBLE(-40.25, blurGui->positionY);
    ASSERT_EQ(1u, blur->knobs.size());
    KnobSerialization size = blur->knobs.front();
    Serialization::completeKnobSerialization(&size, "Double", 2);
    ASSERT_FALSE(size.dimensions.empty());
    ASSERT_TRUE(size.dimensions[0].value.has_value());
    ASSERT_TRUE(std::holds_alternative<double>(*size.dimensions[0].value));
    EXPECT_SAME_DOUBLE(12.25, std::get<double>(*size.dimensions[0].value));

    NodeGuiSerializationPtr gradeGui;
    const NodeSerializationPtr grade = findNode(decoded, "Grade1", &gradeGui);
    ASSERT_NE(nullptr, grade);
    ASSERT_NE(nullptr, gradeGui);
    EXPECT_SAME_DOUBLE(120., gradeGui->positionY);
    ASSERT_EQ(1u, grade->inputs.count("Source"));
    EXPECT_EQ("Blur1", grade->inputs.at("Source"));
    ASSERT_EQ(1u, grade->inputs.count("Mask"));
    EXPECT_EQ("Read1", grade->inputs.at("Mask"));
}

TEST(ClipboardYaml, AGroupKeepsTheGraphStatesOfItsNodesAlignedWithThem)
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
    NodeGuiSerializationPtr groupGui = makeGui("Comp.Group1", 0., 0.);
    groupGui->children = { makeGui("Comp.Group1.Input1", 1., 2.), makeGui("Comp.Group1.Blur2", 3., 4.) };
    clipboard.nodesUI = { groupGui };

    YamlWarnings warnings;
    const ClipboardSerialization decoded = Serialization::decodeClipboardYaml(Serialization::encodeClipboardYaml(clipboard), &warnings);
    EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? std::string() : warnings.front());
    ASSERT_EQ(1u, decoded.nodes.size());
    ASSERT_EQ(1u, decoded.nodesUI.size());
    const NodeSerialization& decodedGroup = *decoded.nodes.front();
    const NodeGuiSerialization& decodedGroupGui = *decoded.nodesUI.front();
    EXPECT_EQ("Group1", decodedGroupGui.nodeName);
    ASSERT_EQ(3u, decodedGroup.children.size());
    ASSERT_EQ(decodedGroup.children.size(), decodedGroupGui.children.size());
    std::list<NodeGuiSerializationPtr>::const_iterator itGui = decodedGroupGui.children.begin();
    for (const NodeSerializationPtr& child : decodedGroup.children) {
        const NodeGuiSerialization& childGui = **itGui++;
        EXPECT_EQ("Group1." + child->scriptName, childGui.nodeName);
        if (child->scriptName == "Input1") {
            EXPECT_SAME_DOUBLE(2., childGui.positionY);
        } else if (child->scriptName == "Blur2") {
            EXPECT_SAME_DOUBLE(4., childGui.positionY);
            EXPECT_EQ("Input1", child->inputs.at("Source"));
        } else {
            EXPECT_SAME_DOUBLE(0., childGui.positionY);
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
