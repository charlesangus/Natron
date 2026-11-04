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

#include <iterator>
#include <list>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::KnobSerialization;
using Serialization::KnobValueSerialization;
using Serialization::NodeSerialization;
using Serialization::NodeSerializationPtr;
using Serialization::ProjectSerialization;
using Serialization::YamlError;
using Serialization::YamlWarnings;

namespace {

// The project.yaml of exampleProject(). The project format reference quotes it as its example,
// so a change here is a change to the documented format.
const char* const kExampleProjectYaml = R"yaml(format: natron-project
formatVersion: 1
savedBy: {version: [2, 6, 0], branch: "main", commit: "5acedc397", os: "Linux", bits: 64}
id: "6f1c2d3e-7a8b-4c5d-9e0f-123456789abc"
knobs:
  frameRange: [1, 240]
  frameRate: 23.976
formats:
  - {name: "Anamorphic", rect: [0, 0, 2048, 858], par: 2}
layers:
  diffuse: {label: "diffuse", channelsLabel: "RGB", channels: ["R", "G", "B"], origin: 2}
  zdepth: {label: "Depth", channelsLabel: "Z", channels: ["Z"], origin: 1}
timeline: {current: 12}
nodes:
  Blur1:
    plugin: "net.sf.cimg.CImgBlur"
    version: [4, 0]
    label: "Soft blur"
    pos: [120, -40]
    knobs:
      size: 3.5
    inputs: {Source: "Read1"}
    pages: ["Controls", "Node"]
  Group1:
    plugin: "fr.inria.built-in.Group"
    version: [1, 0]
    python: {module: "SoftGlow", version: 2}
    inputs: {Input1: "Blur1"}
    userPages:
      Controls:
        group: {label: "Controls"}
        children:
          amount:
            value: 0.5
            user:
              type: "Double"
              dimension: 1
              label: "Amount"
              newLine: true
              evaluates: true
              animates: true
              persistent: true
              default: 0.5
              range: [0, 1, 0, 1]
          advanced:
            group: {label: "Advanced", opened: true}
            children:
              mode:
                value: "add"
                user:
                  type: "Choice"
                  dimension: 1
                  label: "Mode"
                  newLine: false
                  evaluates: true
                  animates: false
                  persistent: true
                  default: 1
                  entries: ["screen", "add"]
                  helps: ["", ""]
    children:
      Blur2:
        plugin: "net.sf.cimg.CImgBlur"
        version: [4, 0]
        pos: [-35.5, 80]
        knobs:
          size: {expr: "thisGroup.amount.get() * 20", hasRet: false}
        inputs: {Source: "Input1"}
      Input1:
        plugin: "fr.inria.built-in.Input"
        version: [1, 0]
      Output1:
        plugin: "fr.inria.built-in.Output"
        version: [1, 0]
        inputs: {Source: "Blur2"}
  Read1:
    plugin: "fr.inria.built-in.Read"
    version: [1, 0]
    knobs:
      filename: "plates/shot010.####.exr"
  Viewer1:
    plugin: "fr.inria.built-in.Viewer"
    version: [1, 0]
    inputs: {"1": "Group1"}
)yaml";

KnobSerialization
valueKnob(const std::string& name,
          const std::string& typeName,
          std::initializer_list<KnobValueSerialization> values)
{
    KnobSerialization knob;
    knob.name = name;
    knob.typeName = typeName;
    knob.dimension = (int)values.size();
    for (const KnobValueSerialization& value : values) {
        Serialization::KnobDimensionSerialization dimension;
        dimension.value = value;
        dimension.defaultValue = value;
        knob.dimensions.push_back(dimension);
    }

    return knob;
}

KnobValueSerialization
d(double value)
{
    return KnobValueSerialization(std::in_place_type<double>, value);
}

KnobValueSerialization
i(int value)
{
    return KnobValueSerialization(std::in_place_type<int>, value);
}

KnobValueSerialization
s(const std::string& value)
{
    return KnobValueSerialization(std::in_place_type<std::string>, value);
}

NodeSerializationPtr
makeNode(const std::string& scriptName,
         const std::string& pluginID,
         int major,
         int minor)
{
    NodeSerializationPtr node = std::make_shared<NodeSerialization>();
    node->scriptName = scriptName;
    node->label = scriptName;
    node->pluginID = pluginID;
    node->pluginMajorVersion = major;
    node->pluginMinorVersion = minor;

    return node;
}

// A project as Engine's toSerialization() fills it, nodes and layers already in sorted order:
// a reader, a blur, a viewer and a group (a PyPlug with a user page) holding its own blur
// between its input and output.
ProjectSerialization
exampleProject()
{
    ProjectSerialization project;
    project.savedByVersion.versionMajor = 2;
    project.savedByVersion.versionMinor = 6;
    project.savedByVersion.versionRevision = 0;
    project.savedByVersion.gitBranch = "main";
    project.savedByVersion.gitCommit = "5acedc397";
    project.savedByVersion.operatingSystem = "Linux";
    project.savedByVersion.bits = 64;
    project.projectId = "6f1c2d3e-7a8b-4c5d-9e0f-123456789abc";
    project.creationDate = 0;

    project.projectKnobs.push_back(valueKnob("frameRange", "Int", { i(1), i(240) }));
    project.projectKnobs.push_back(valueKnob("frameRate", "Double", { d(23.976) }));

    Serialization::FormatSerialization format;
    format.name = "Anamorphic";
    format.x2 = 2048;
    format.y2 = 858;
    format.pixelAspectRatio = 2.;
    project.additionalFormats.push_back(format);

    Serialization::LayerRegistryEntrySerialization diffuse;
    diffuse.layer.layerID = "diffuse";
    diffuse.layer.layerLabel = "diffuse";
    diffuse.layer.channelsLabel = "RGB";
    diffuse.layer.channels = { "R", "G", "B" };
    diffuse.origin = 2;
    Serialization::LayerRegistryEntrySerialization depth;
    depth.layer.layerID = "zdepth";
    depth.layer.layerLabel = "Depth";
    depth.layer.channelsLabel = "Z";
    depth.layer.channels = { "Z" };
    depth.origin = 1;
    project.layers.push_back(diffuse);
    project.layers.push_back(depth);
    project.timelineCurrentTime = 12;

    NodeSerializationPtr blur = makeNode("Blur1", "net.sf.cimg.CImgBlur", 4, 0);
    blur->label = "Soft blur";
    blur->knobs.push_back(valueKnob("size", "Double", { d(3.5), d(3.5) }));
    blur->knobs.back().dimensions.resize(1);
    blur->inputs["Source"] = "Read1";
    blur->pagesOrder = { "Controls", "Node" };
    blur->layout.hasPosition = true;
    blur->layout.x = 120.;
    blur->layout.y = -40.;

    NodeSerializationPtr group = makeNode("Group1", "fr.inria.built-in.Group", 1, 0);
    group->pythonModule = "SoftGlow";
    group->pythonModuleVersion = 2;
    group->inputs["Input1"] = "Blur1";
    Serialization::GroupKnobSerialization page;
    page.name = "Controls";
    page.label = "Controls";
    Serialization::GroupKnobChildSerialization amount;
    amount.knob = valueKnob("amount", "Double", { d(0.5) });
    amount.knob.isUserKnob = true;
    amount.knob.label = "Amount";
    amount.knob.triggerNewLine = true;
    amount.knob.evaluatesOnChange = true;
    amount.knob.animationEnabled = true;
    amount.knob.isPersistent = true;
    Serialization::KnobValueRangeSerialization range;
    range.maximum = 1.;
    range.displayMaximum = 1.;
    amount.knob.extraData = range;
    page.children.push_back(amount);
    Serialization::GroupKnobChildSerialization advanced;
    advanced.isGroup = true;
    advanced.group.name = "advanced";
    advanced.group.label = "Advanced";
    advanced.group.isOpened = true;
    Serialization::GroupKnobChildSerialization mode;
    mode.knob = valueKnob("mode", "Choice", { i(1) });
    mode.knob.isUserKnob = true;
    mode.knob.label = "Mode";
    mode.knob.isPersistent = true;
    mode.knob.evaluatesOnChange = true;
    Serialization::KnobChoiceExtraSerialization modes;
    modes.entries = { "screen", "add" };
    modes.helpStrings = { "", "" };
    mode.knob.extraData = modes;
    mode.knob.choiceOptionID = "add";
    advanced.group.children.push_back(mode);
    page.children.push_back(advanced);
    group->userPages.push_back(page);

    NodeSerializationPtr innerBlur = makeNode("Blur2", "net.sf.cimg.CImgBlur", 4, 0);
    innerBlur->knobs.push_back(valueKnob("size", "Double", { d(10.), d(10.) }));
    innerBlur->knobs.back().dimensions.resize(1);
    innerBlur->knobs.back().dimensions[0].expression = "thisGroup.amount.get() * 20";
    innerBlur->inputs["Source"] = "Input1";
    innerBlur->layout.hasPosition = true;
    innerBlur->layout.x = -35.5;
    innerBlur->layout.y = 80.;
    NodeSerializationPtr input = makeNode("Input1", "fr.inria.built-in.Input", 1, 0);
    NodeSerializationPtr output = makeNode("Output1", "fr.inria.built-in.Output", 1, 0);
    output->inputs["Source"] = "Blur2";
    group->children = { innerBlur, input, output };

    NodeSerializationPtr read = makeNode("Read1", "fr.inria.built-in.Read", 1, 0);
    read->knobs.push_back(valueKnob("filename", "InputFile", { s("plates/shot010.####.exr") }));
    read->knobs.back().stringAnimation.emplace();

    NodeSerializationPtr viewer = makeNode("Viewer1", "fr.inria.built-in.Viewer", 1, 0);
    viewer->inputs["1"] = "Group1";

    project.nodes.nodes = { blur, group, read, viewer };

    return project;
}

void expectNodeListsEqual(const std::list<NodeSerializationPtr>& expected, const std::list<NodeSerializationPtr>& actual);

void
expectNodesEqual(const NodeSerialization& expected,
                 const NodeSerialization& actual)
{
    SCOPED_TRACE("node " + expected.scriptName);
    EXPECT_EQ(expected.scriptName, actual.scriptName);
    EXPECT_EQ(expected.label, actual.label);
    EXPECT_EQ(expected.pluginID, actual.pluginID);
    EXPECT_EQ(expected.pluginMajorVersion, actual.pluginMajorVersion);
    EXPECT_EQ(expected.pluginMinorVersion, actual.pluginMinorVersion);
    EXPECT_EQ(expected.pythonModule, actual.pythonModule);
    EXPECT_EQ(expected.pythonModuleVersion, actual.pythonModuleVersion);
    EXPECT_EQ(expected.layout.hasPosition, actual.layout.hasPosition);
    EXPECT_EQ(expected.layout.x, actual.layout.x);
    EXPECT_EQ(expected.layout.y, actual.layout.y);
    EXPECT_EQ(expected.layout.hasSize, actual.layout.hasSize);
    EXPECT_EQ(expected.layout.width, actual.layout.width);
    EXPECT_EQ(expected.layout.height, actual.layout.height);
    EXPECT_EQ(expected.layout.hasColor, actual.layout.hasColor);
    EXPECT_EQ(expected.layout.colorR, actual.layout.colorR);
    EXPECT_EQ(expected.layout.colorG, actual.layout.colorG);
    EXPECT_EQ(expected.layout.colorB, actual.layout.colorB);
    EXPECT_EQ(expected.layout.hasOverlayColor, actual.layout.hasOverlayColor);
    EXPECT_EQ(expected.layout.overlayR, actual.layout.overlayR);
    EXPECT_EQ(expected.layout.overlayG, actual.layout.overlayG);
    EXPECT_EQ(expected.layout.overlayB, actual.layout.overlayB);
    expectExpectedKnobs(expected.knobs, actual.knobs);
    EXPECT_EQ(expected.inputs, actual.inputs);
    EXPECT_EQ(expected.pagesOrder, actual.pagesOrder);
    ASSERT_EQ(expected.userPages.size(), actual.userPages.size());
    auto page = actual.userPages.begin();
    for (const Serialization::GroupKnobSerialization& expectedPage : expected.userPages) {
        expectGroupKnobsEqual(expectedPage, *page);
        ++page;
    }
    EXPECT_EQ(expected.hasRotoContext, actual.hasRotoContext);
    EXPECT_EQ(expected.hasTrackerContext, actual.hasTrackerContext);
    EXPECT_EQ(0u, actual.knobsAge);
    EXPECT_TRUE(actual.multiInstanceParentName.empty());
    EXPECT_EQ(expected.cacheID, actual.cacheID);
    expectNodeListsEqual(expected.children, actual.children);
}

void
expectNodeListsEqual(const std::list<NodeSerializationPtr>& expected,
                     const std::list<NodeSerializationPtr>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    auto a = actual.begin();
    for (const NodeSerializationPtr& e : expected) {
        expectNodesEqual(*e, **a);
        ++a;
    }
}

void
expectProjectsEqual(const ProjectSerialization& expected,
                    const ProjectSerialization& actual)
{
    EXPECT_EQ(expected.savedByVersion.versionMajor, actual.savedByVersion.versionMajor);
    EXPECT_EQ(expected.savedByVersion.versionMinor, actual.savedByVersion.versionMinor);
    EXPECT_EQ(expected.savedByVersion.versionRevision, actual.savedByVersion.versionRevision);
    EXPECT_EQ(expected.savedByVersion.gitBranch, actual.savedByVersion.gitBranch);
    EXPECT_EQ(expected.savedByVersion.gitCommit, actual.savedByVersion.gitCommit);
    EXPECT_EQ(expected.savedByVersion.operatingSystem, actual.savedByVersion.operatingSystem);
    EXPECT_EQ(expected.savedByVersion.bits, actual.savedByVersion.bits);
    expectExpectedKnobs(expected.projectKnobs, actual.projectKnobs);
    ASSERT_EQ(expected.additionalFormats.size(), actual.additionalFormats.size());
    auto format = actual.additionalFormats.begin();
    for (const Serialization::FormatSerialization& e : expected.additionalFormats) {
        EXPECT_EQ(e.name, format->name);
        EXPECT_EQ(e.x1, format->x1);
        EXPECT_EQ(e.y1, format->y1);
        EXPECT_EQ(e.x2, format->x2);
        EXPECT_EQ(e.y2, format->y2);
        EXPECT_SAME_DOUBLE(e.pixelAspectRatio, format->pixelAspectRatio);
        ++format;
    }
    ASSERT_EQ(expected.layers.size(), actual.layers.size());
    auto layer = actual.layers.begin();
    for (const Serialization::LayerRegistryEntrySerialization& e : expected.layers) {
        EXPECT_EQ(e.layer.layerID, layer->layer.layerID);
        EXPECT_EQ(e.layer.layerLabel, layer->layer.layerLabel);
        EXPECT_EQ(e.layer.channelsLabel, layer->layer.channelsLabel);
        EXPECT_EQ(e.layer.channels, layer->layer.channels);
        EXPECT_EQ(e.origin, layer->origin);
        ++layer;
    }
    EXPECT_EQ(expected.projectId, actual.projectId);
    EXPECT_EQ(expected.timelineCurrentTime, actual.timelineCurrentTime);
    EXPECT_EQ(0, actual.creationDate);
    expectNodeListsEqual(expected.nodes.nodes, actual.nodes.nodes);
}

std::string
replaced(std::string text,
         const std::string& from,
         const std::string& to)
{
    const std::size_t at = text.find(from);
    EXPECT_NE(std::string::npos, at) << from;
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }

    return text;
}

void
expectDecodeError(const std::string& text,
                  const std::string& expectedFragment)
{
    Serialization::NoBlobSource source;
    try {
        Serialization::decodeProjectYaml(text, source);
        ADD_FAILURE() << "decoding did not throw";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find(expectedFragment), std::string::npos) << e.what();
    }
}

} // anonymous namespace

TEST(ProjectYaml, ExampleProjectText)
{
    Serialization::MemoryBlobStore blobs;
    EXPECT_EQ(std::string(kExampleProjectYaml), Serialization::encodeProjectYaml(exampleProject(), blobs));
    EXPECT_TRUE(blobs.members().empty());
}

TEST(ProjectYaml, RoundTripsAndReencodesByteIdentically)
{
    const ProjectSerialization project = exampleProject();
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeProjectYaml(project, blobs);

    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    expectProjectsEqual(project, decoded);
    EXPECT_EQ(text, Serialization::encodeProjectYaml(decoded, blobs));
}

TEST(ProjectYaml, BackdropSizeAndColoursRoundTrip)
{
    ProjectSerialization project = exampleProject();
    NodeSerializationPtr backdrop = makeNode("BackDrop1", "fr.inria.built-in.BackDrop", 1, 0);
    backdrop->layout.hasPosition = true;
    backdrop->layout.x = -300.25;
    backdrop->layout.y = 12.;
    backdrop->layout.hasSize = true;
    backdrop->layout.width = 640.;
    backdrop->layout.height = 480.5;
    backdrop->layout.hasColor = true;
    backdrop->layout.colorR = 0.25f;
    backdrop->layout.colorG = 0.5f;
    backdrop->layout.colorB = 0.125f;
    backdrop->layout.hasOverlayColor = true;
    backdrop->layout.overlayR = 1.f;
    backdrop->layout.overlayG = 0.f;
    backdrop->layout.overlayB = 0.75f;
    project.nodes.nodes.push_front(backdrop);

    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeProjectYaml(project, blobs);
    EXPECT_NE(std::string::npos, text.find("    pos: [-300.25, 12]\n    size: [640, 480.5]\n    color: [0.25, 0.5, 0.125]\n    overlay: [1, 0, 0.75]\n"));

    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    expectProjectsEqual(project, decoded);
    EXPECT_EQ(text, Serialization::encodeProjectYaml(decoded, blobs));
}

TEST(ProjectYaml, RFVGroupFlagRoundTrips)
{
    NodeSerializationPtr setNode = makeNode("Set1", "fr.inria.built-in.Dot", 1, 0);
    Serialization::GroupKnobSerialization page;
    page.name = "Variables";
    page.label = "Variables";
    Serialization::GroupKnobChildSerialization variables;
    variables.isGroup = true;
    variables.group.name = "variables";
    variables.group.label = "Variables";
    variables.group.isRFVGroup = true;
    Serialization::GroupKnobChildSerialization gain;
    gain.knob = valueKnob("gain", "Double", { d(0.25) });
    gain.knob.isUserKnob = true;
    gain.knob.label = "Gain";
    gain.knob.isPersistent = true;
    variables.group.children.push_back(gain);
    page.children.push_back(variables);
    setNode->userPages.push_back(page);
    ProjectSerialization project;
    project.nodes.nodes = { setNode };

    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeProjectYaml(project, blobs);
    EXPECT_NE(std::string::npos, text.find("group: {label: \"Variables\", rfv: true}")) << text;

    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    expectProjectsEqual(project, decoded);
    EXPECT_EQ(text, Serialization::encodeProjectYaml(decoded, blobs));
}

TEST(ProjectYaml, NodeWithoutLayoutWritesNoLayoutKeys)
{
    ProjectSerialization project;
    project.nodes.nodes = { makeNode("Read1", "fr.inria.built-in.Read", 1, 0) };
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeProjectYaml(project, blobs);
    for (const char* key : { "pos:", "size:", "color:", "overlay:" }) {
        EXPECT_EQ(std::string::npos, text.find(key)) << key;
    }

    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    const Serialization::NodeLayoutSerialization& layout = decoded.nodes.nodes.front()->layout;
    EXPECT_FALSE(layout.hasPosition || layout.hasSize || layout.hasColor || layout.hasOverlayColor);
}

TEST(ProjectYaml, ANodeCacheIDRoundTrips)
{
    ProjectSerialization project;
    NodeSerializationPtr read = makeNode("Read1", "fr.inria.built-in.Read", 1, 0);
    read->cacheID = "0b6f1d3c-6a0e-4c43-9f55-2f7e8d1a4b90";
    project.nodes.nodes = { read };
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeProjectYaml(project, blobs);

    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    ASSERT_EQ(1u, decoded.nodes.nodes.size());
    EXPECT_EQ(read->cacheID, decoded.nodes.nodes.front()->cacheID);
}

TEST(ProjectYaml, NodeAndKeyOrderInTheFileIsCosmetic)
{
    std::string shuffled = std::string(kExampleProjectYaml);
    const std::size_t nodesAt = shuffled.find("nodes:\n");
    ASSERT_NE(std::string::npos, nodesAt);
    std::string header = shuffled.substr(0, nodesAt);
    const std::string body = shuffled.substr(nodesAt);
    const std::size_t timelineAt = header.find("timeline:");
    const std::size_t savedByAt = header.find("savedBy:");
    ASSERT_TRUE(timelineAt != std::string::npos && savedByAt != std::string::npos);
    const std::string timelineLine = header.substr(timelineAt, header.find('\n', timelineAt) - timelineAt + 1);
    header.erase(timelineAt, timelineLine.size());
    header.insert(savedByAt, timelineLine);

    // Viewer1 first, Read1 before Blur1, and Output1 before the other nodes of Group1.
    auto block = [&body](const std::string& start, const std::string& end) {
        const std::size_t from = body.find(start);
        const std::size_t to = end.empty() ? body.size() : body.find(end, from);
        EXPECT_TRUE(from != std::string::npos && to != std::string::npos) << start;

        return body.substr(from, to - from);
    };
    const std::string blur = block("  Blur1:\n", "  Group1:\n");
    const std::string groupHead = block("  Group1:\n", "      Blur2:\n");
    const std::string innerBlurAndInput = block("      Blur2:\n", "      Output1:\n");
    const std::string output = block("      Output1:\n", "  Read1:\n");
    const std::string read = block("  Read1:\n", "  Viewer1:\n");
    const std::string viewer = block("  Viewer1:\n", "");
    shuffled = header + "nodes:\n" + viewer + read + blur + groupHead + output + innerBlurAndInput;

    Serialization::NoBlobSource source;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(shuffled, source);
    expectProjectsEqual(exampleProject(), decoded);
    Serialization::MemoryBlobStore blobs;
    EXPECT_EQ(std::string(kExampleProjectYaml), Serialization::encodeProjectYaml(decoded, blobs));
}

TEST(ProjectYaml, DuplicateScriptNameIsAnError)
{
    expectDecodeError(replaced(kExampleProjectYaml, "      Input1:\n", "      Blur2:\n"), "duplicate key \"Blur2\"");

    ProjectSerialization project = exampleProject();
    project.nodes.nodes.push_back(makeNode("Blur1", "net.sf.cimg.CImgBlur", 4, 0));
    Serialization::MemoryBlobStore blobs;
    try {
        Serialization::encodeProjectYaml(project, blobs);
        ADD_FAILURE() << "encoding did not throw";
    } catch (const YamlError& e) {
        EXPECT_EQ(std::string("two nodes are named \"Blur1\" in the project"), e.what());
    }
}

TEST(ProjectYaml, NewerFormatVersionIsRefused)
{
    expectDecodeError(replaced(kExampleProjectYaml, "formatVersion: 1\n", "formatVersion: 99\n"), "This project was saved by a newer Natron (project format version 99; this version reads up to 1)");
    expectDecodeError(replaced(kExampleProjectYaml, "format: natron-project\n", "format: natron-preset\n"), "project.yaml is not a Natron project");
    expectDecodeError("- just\n- a list\n", "project.yaml is not a Natron project");
}

TEST(ProjectYaml, UnknownKeysAreIgnoredWithWarnings)
{
    std::string text = replaced(kExampleProjectYaml, "timeline:", "futureFeature: {enabled: true}\ntimeline:");
    text = replaced(text, "  Read1:\n", "  Read1:\n    colourTag: red\n");
    Serialization::NoBlobSource source;
    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, source, &warnings);
    expectProjectsEqual(exampleProject(), decoded);
    ASSERT_EQ(2u, warnings.size());
    EXPECT_EQ("nodes.Read1: unknown key \"colourTag\" ignored", warnings[0]);
    EXPECT_EQ("project: unknown key \"futureFeature\" ignored", warnings[1]);
}

TEST(ProjectYaml, RotoAndTrackerBlobsAreNamedAfterTheFullyQualifiedNode)
{
    ProjectSerialization project = exampleProject();
    NodeSerializationPtr group = *std::next(project.nodes.nodes.begin());
    ASSERT_EQ("Group1", group->scriptName);

    NodeSerializationPtr rotoPaint = makeNode("RotoPaint1", "fr.inria.built-in.RotoPaint", 1, 0);
    rotoPaint->hasRotoContext = true;
    rotoPaint->rotoContext.baseLayer.name = "Layer1";
    rotoPaint->rotoContext.baseLayer.label = "Layer1";
    rotoPaint->rotoContext.baseLayer.activated = true;
    Serialization::RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeStroke;
    child.stroke.name = "Stroke1";
    child.stroke.label = "Stroke1";
    child.stroke.parentLayerName = "Layer1";
    child.stroke.activated = true;
    child.stroke.strokes.emplace_back();
    for (int i = 0; i < 100; ++i) {
        Serialization::KeyFrameSerialization key;
        key.time = i;
        key.interpolation = 1;
        key.value = i * 0.5;
        child.stroke.strokes.back().xCurve.keyframes.push_back(key);
        key.value = i * 0.25;
        child.stroke.strokes.back().yCurve.keyframes.push_back(key);
        key.value = 1.;
        child.stroke.strokes.back().pressureCurve.keyframes.push_back(key);
    }
    rotoPaint->rotoContext.baseLayer.children.push_back(child);

    NodeSerializationPtr tracker = makeNode("Tracker1", "fr.inria.built-in.Tracker", 1, 0);
    tracker->hasTrackerContext = true;

    group->children.push_back(rotoPaint);
    group->children.push_back(tracker);

    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeProjectYaml(project, blobs);
    ASSERT_EQ(1u, blobs.members().size());
    EXPECT_EQ("blobs/Group1.RotoPaint1/Stroke1.stroke.bin", blobs.members().begin()->first);
    EXPECT_NE(std::string::npos, text.find("      Tracker1:\n        plugin: \"fr.inria.built-in.Tracker\"\n        version: [1, 0]\n        tracker:\n          tracks: []\n")) << text;

    YamlWarnings warnings;
    const ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    expectProjectsEqual(project, decoded);
    const NodeSerializationPtr decodedGroup = *std::next(decoded.nodes.nodes.begin());
    const NodeSerializationPtr decodedRoto = *std::next(decodedGroup->children.begin(), 3);
    ASSERT_EQ("RotoPaint1", decodedRoto->scriptName);
    ASSERT_EQ(1u, decodedRoto->rotoContext.baseLayer.children.size());
    expectCurvesBitIdentical(child.stroke.strokes.back().yCurve, decodedRoto->rotoContext.baseLayer.children.front().stroke.strokes.back().yCurve);
    EXPECT_TRUE((*std::next(decodedGroup->children.begin(), 4))->hasTrackerContext);
}
