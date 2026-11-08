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
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Yaml/GraphOpYaml.h"
#include "Serialization/Yaml/KnobYaml.h"

NATRON_NAMESPACE_USING
using namespace Serialization;

namespace {

KnobSerialization
knobFromYaml(const std::string& text)
{
    std::list<KnobSerialization> knobs = decodeKnobsYaml(text);
    EXPECT_EQ(knobs.size(), 1u);

    return knobs.front();
}

// Encoding the decoded operation gives the same text again, and the text names the operation.
GraphOp
roundTrip(const GraphOp& op)
{
    const std::string text = encodeGraphOpYaml(op);
    EXPECT_NE(text.find(std::string(graphOpName(op))), std::string::npos) << text;
    YamlWarnings warnings;
    GraphOp decoded = decodeGraphOpYaml(text, &warnings);
    EXPECT_TRUE(warnings.empty()) << text;
    EXPECT_EQ(op.index(), decoded.index());
    EXPECT_EQ(text, encodeGraphOpYaml(decoded));

    return decoded;
}

NodeSerializationPtr
makeNode(const std::string& id,
         const std::string& scriptName,
         const std::string& pluginID)
{
    NodeSerializationPtr node = std::make_shared<NodeSerialization>();
    node->id = id;
    node->scriptName = scriptName;
    node->label = scriptName;
    node->pluginID = pluginID;
    node->pluginMajorVersion = 1;
    node->pluginMinorVersion = 0;

    return node;
}

} // namespace

TEST(GraphOpYaml, CreateNodeKeepsIdsAndChildren)
{
    NodeSerializationPtr group = makeNode("aaaa0001", "Group1", "fr.inria.built-in.Group");
    group->layout.hasPosition = true;
    group->layout.x = 10.;
    group->layout.y = -20.5;
    group->knobs.push_back(knobFromYaml("size: 3.5"));
    NodeSerializationPtr child = makeNode("aaaa0002", "Blur1", "net.sf.cimg.CImgBlur");
    child->inputs["Source"] = "Input1";
    group->children.push_back(child);

    const GraphOp decoded = roundTrip(CreateNodeOp { "parent01", group });
    const CreateNodeOp& op = std::get<CreateNodeOp>(decoded);
    EXPECT_EQ(op.parentId, "parent01");
    ASSERT_NE(op.node, nullptr);
    EXPECT_EQ(op.node->id, "aaaa0001");
    EXPECT_EQ(op.node->scriptName, "Group1");
    EXPECT_TRUE(op.node->layout.hasPosition);
    EXPECT_EQ(op.node->layout.y, -20.5);
    ASSERT_EQ(op.node->children.size(), 1u);
    EXPECT_EQ(op.node->children.front()->id, "aaaa0002");
    EXPECT_EQ(op.node->children.front()->inputs.at("Source"), "Input1");

    const GraphOp atRoot = roundTrip(CreateNodeOp { std::string(), child });
    EXPECT_TRUE(std::get<CreateNodeOp>(atRoot).parentId.empty());
}

TEST(GraphOpYaml, CreateNodeWithoutIdIsRefused)
{
    const std::string text = encodeGraphOpYaml(CreateNodeOp { std::string(), makeNode(std::string(), "Blur1", "net.sf.cimg.CImgBlur") });
    EXPECT_THROW(decodeGraphOpYaml(text), YamlError);
}

TEST(GraphOpYaml, DeleteRenameAndSetInput)
{
    EXPECT_EQ(std::get<DeleteNodeOp>(roundTrip(DeleteNodeOp { "id000001" })).nodeId, "id000001");

    const RenameOp rename = std::get<RenameOp>(roundTrip(RenameOp { "id000001", "Sharpen2", std::nullopt }));
    EXPECT_EQ(rename.scriptName, "Sharpen2");
    EXPECT_FALSE(rename.label.has_value());
    const RenameOp relabel = std::get<RenameOp>(roundTrip(RenameOp { "id000001", "Sharpen2", std::string("Edge sharpen") }));
    ASSERT_TRUE(relabel.label.has_value());
    EXPECT_EQ(*relabel.label, "Edge sharpen");

    const SetInputOp connect = std::get<SetInputOp>(roundTrip(SetInputOp { "id000001", "Source", "id000002" }));
    EXPECT_EQ(connect.inputLabel, "Source");
    EXPECT_EQ(connect.sourceId, "id000002");
    const std::string disconnectText = encodeGraphOpYaml(SetInputOp { "id000001", "Mask", std::string() });
    EXPECT_NE(disconnectText.find("source: ~"), std::string::npos) << disconnectText;
    EXPECT_TRUE(std::get<SetInputOp>(roundTrip(SetInputOp { "id000001", "Mask", std::string() })).sourceId.empty());
}

TEST(GraphOpYaml, SetKnobValueDefaultAndProject)
{
    const SetKnobOp value = std::get<SetKnobOp>(roundTrip(SetKnobOp { std::string("id000001"), "size", knobFromYaml("size: {expr: \"frame * 2\", hasRet: false}") }));
    ASSERT_TRUE(value.nodeId.has_value());
    EXPECT_EQ(*value.nodeId, "id000001");
    EXPECT_EQ(value.knobName, "size");
    ASSERT_TRUE(value.value.has_value());
    EXPECT_EQ(value.value->name, "size");
    ASSERT_EQ(value.value->dimensions.size(), 1u);
    EXPECT_EQ(value.value->dimensions[0].expression, "frame * 2");

    const SetKnobOp reset = std::get<SetKnobOp>(roundTrip(SetKnobOp { std::string("id000001"), "size", std::nullopt }));
    EXPECT_FALSE(reset.value.has_value());

    const SetKnobOp project = std::get<SetKnobOp>(roundTrip(SetKnobOp { std::nullopt, "frameRate", knobFromYaml("frameRate: 25") }));
    EXPECT_FALSE(project.nodeId.has_value());
    EXPECT_EQ(project.knobName, "frameRate");
}

TEST(GraphOpYaml, SetNodeUiTellsUnchangedFromCleared)
{
    SetNodeUiOp ui;
    ui.nodeId = "id000001";
    ui.position = std::make_pair(1.5, -2.);
    ui.color.emplace(NodeUiColor { 0.25f, 0.5f, 1.f });
    ui.overlayColor.emplace(std::nullopt);
    const SetNodeUiOp decoded = std::get<SetNodeUiOp>(roundTrip(ui));
    ASSERT_TRUE(decoded.position.has_value());
    EXPECT_EQ(decoded.position->first, 1.5);
    EXPECT_FALSE(decoded.size.has_value());
    ASSERT_TRUE(decoded.color.has_value() && decoded.color->has_value());
    EXPECT_EQ(**decoded.color, (NodeUiColor { 0.25f, 0.5f, 1.f }));
    ASSERT_TRUE(decoded.overlayColor.has_value());
    EXPECT_FALSE(decoded.overlayColor->has_value());
}

TEST(GraphOpYaml, SetUserKnobs)
{
    SetUserKnobsOp op;
    op.nodeId = "id000001";
    op.pagesOrder = { "Controls", "Node", "Extra" };
    GroupKnobSerialization page;
    page.name = "Extra";
    page.label = "Extra";
    GroupKnobChildSerialization child;
    child.knob = knobFromYaml("amount: {value: 0.5, user: {type: \"Double\", dimension: 1, label: \"Amount\", newLine: true, evaluates: true, animates: true, persistent: true, default: 0.5, range: [0, 1, 0, 1]}}");
    page.children.push_back(child);
    op.userPages.push_back(page);
    const SetUserKnobsOp decoded = std::get<SetUserKnobsOp>(roundTrip(op));
    EXPECT_EQ(decoded.pagesOrder, op.pagesOrder);
    ASSERT_EQ(decoded.userPages.size(), 1u);
    EXPECT_EQ(decoded.userPages.front(), op.userPages.front());

    SetUserKnobsOp none;
    none.nodeId = "id000001";
    EXPECT_TRUE(std::get<SetUserKnobsOp>(roundTrip(none)).userPages.empty());
}

TEST(GraphOpYaml, SetRotoAndTrackerData)
{
    RotoContextSerialization roto;
    roto.baseLayer.name = "Layer1";
    roto.baseLayer.label = "Layer1";
    roto.baseLayer.activated = true;
    roto.autoKeying = true;
    const SetRotoDataOp withRoto = std::get<SetRotoDataOp>(roundTrip(SetRotoDataOp { "id000001", roto }));
    ASSERT_TRUE(withRoto.roto.has_value());
    EXPECT_TRUE(withRoto.roto->autoKeying);
    EXPECT_FALSE(std::get<SetRotoDataOp>(roundTrip(SetRotoDataOp { "id000001", std::nullopt })).roto.has_value());

    TrackerContextSerialization tracker;
    TrackSerialization track;
    track.scriptName = "track1";
    track.label = "track1";
    track.userKeys = { 1, 12 };
    tracker.tracks.push_back(track);
    const SetTrackerDataOp withTracker = std::get<SetTrackerDataOp>(roundTrip(SetTrackerDataOp { "id000001", tracker }));
    ASSERT_TRUE(withTracker.tracker.has_value());
    ASSERT_EQ(withTracker.tracker->tracks.size(), 1u);
    EXPECT_EQ(withTracker.tracker->tracks.front().userKeys, track.userKeys);
    EXPECT_FALSE(std::get<SetTrackerDataOp>(roundTrip(SetTrackerDataOp { "id000001", std::nullopt })).tracker.has_value());
}

TEST(GraphOpYaml, SetProjectData)
{
    SetProjectDataOp op;
    FormatSerialization format;
    format.name = "Anamorphic";
    format.x2 = 2048;
    format.y2 = 858;
    format.pixelAspectRatio = 2.;
    op.formats = std::list<FormatSerialization> { format };
    const SetProjectDataOp decoded = std::get<SetProjectDataOp>(roundTrip(op));
    ASSERT_TRUE(decoded.formats.has_value());
    ASSERT_EQ(decoded.formats->size(), 1u);
    EXPECT_EQ(decoded.formats->front().name, "Anamorphic");
    EXPECT_EQ(decoded.formats->front().pixelAspectRatio, 2.);
    EXPECT_FALSE(decoded.layers.has_value());

    SetProjectDataOp layers;
    LayerRegistryEntrySerialization entry;
    entry.layer.layerID = "zdepth";
    entry.layer.layerLabel = "Depth";
    entry.layer.channelsLabel = "Z";
    entry.layer.channels = { "Z" };
    entry.origin = 1;
    layers.layers = std::list<LayerRegistryEntrySerialization> { entry };
    layers.formats = std::list<FormatSerialization>();
    const SetProjectDataOp decodedLayers = std::get<SetProjectDataOp>(roundTrip(layers));
    ASSERT_TRUE(decodedLayers.formats.has_value());
    EXPECT_TRUE(decodedLayers.formats->empty());
    ASSERT_TRUE(decodedLayers.layers.has_value());
    EXPECT_EQ(decodedLayers.layers->front().layer.channels, entry.layer.channels);
}

TEST(GraphOpYaml, BatchKeepsOrder)
{
    const std::vector<GraphOp> ops {
        DeleteNodeOp { "id000003" },
        SetInputOp { "id000001", "Source", "id000002" },
        RenameOp { "id000002", "Grade2", std::nullopt },
    };
    const std::string text = encodeGraphOpsYaml(ops);
    const std::vector<DecodedGraphOp> decoded = decodeGraphOpsYaml(text);
    ASSERT_EQ(decoded.size(), 3u);
    std::vector<GraphOp> again;
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        ASSERT_TRUE(decoded[i].op.has_value()) << decoded[i].error;
        EXPECT_EQ(decoded[i].op->index(), ops[i].index());
        again.push_back(*decoded[i].op);
    }
    EXPECT_EQ(text, encodeGraphOpsYaml(again));

    EXPECT_TRUE(decodeGraphOpsYaml(encodeGraphOpsYaml({})).empty());
}

TEST(GraphOpYaml, UnknownOrMalformedEntryDoesNotHideTheRest)
{
    const std::string text = R"yaml(- {op: "deleteNode", node: "id000001"}
- {op: "transmogrify", node: "id000002"}
- {op: "setInput", node: "id000003"}
- {op: "rename", node: "id000004", name: "Blur9"}
)yaml";
    const std::vector<DecodedGraphOp> decoded = decodeGraphOpsYaml(text);
    ASSERT_EQ(decoded.size(), 4u);
    ASSERT_TRUE(decoded[0].op.has_value());
    EXPECT_EQ(std::get<DeleteNodeOp>(*decoded[0].op).nodeId, "id000001");
    EXPECT_FALSE(decoded[1].op.has_value());
    EXPECT_NE(decoded[1].error.find("transmogrify"), std::string::npos) << decoded[1].error;
    EXPECT_FALSE(decoded[2].op.has_value());
    EXPECT_FALSE(decoded[2].error.empty());
    ASSERT_TRUE(decoded[3].op.has_value());
    EXPECT_EQ(std::get<RenameOp>(*decoded[3].op).scriptName, "Blur9");

    EXPECT_THROW(decodeGraphOpYaml("{op: \"transmogrify\"}"), YamlError);
    EXPECT_THROW(decodeGraphOpsYaml("op: \"deleteNode\""), YamlError);
}

TEST(GraphOpYaml, UnknownKeysAreWarnings)
{
    YamlWarnings warnings;
    const GraphOp op = decodeGraphOpYaml("{op: \"deleteNode\", node: \"id000001\", reason: \"cleanup\"}", &warnings);
    EXPECT_EQ(std::get<DeleteNodeOp>(op).nodeId, "id000001");
    EXPECT_EQ(warnings.size(), 1u);
}
