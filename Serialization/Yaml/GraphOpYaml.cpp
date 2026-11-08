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

#include "Serialization/Yaml/GraphOpYaml.h"

#include <type_traits>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    using namespace YamlDetail;

    void
    emitStringEntry(YAML::Emitter& em,
                    std::string_view key,
                    const std::string& value)
    {
        emitKey(em, key);
        em << YAML::Value;
        emitString(em, value);
    }

    void
    emitNullEntry(YAML::Emitter& em,
                  std::string_view key)
    {
        emitKey(em, key);
        em << YAML::Value << YAML::Null;
    }

    void
    emitColorEntry(YAML::Emitter& em,
                   std::string_view key,
                   const std::optional<std::optional<NodeUiColor>>& color)
    {
        if (!color) {
            return;
        }
        if (*color) {
            emitFloatTriple(em, key, (*color)->r, (*color)->g, (*color)->b);
        } else {
            emitNullEntry(em, key);
        }
    }

    void
    emitOp(YAML::Emitter& em,
           const GraphOp& graphOp,
           BlobSink& sink,
           std::set<std::string>* usedPaths)
    {
        em << YAML::BeginMap;
        emitStringEntry(em, "op", std::string(graphOpName(graphOp)));
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, CreateNodeOp>) {
                if (!op.node) {
                    throw YamlError("a createNode operation needs a node");
                }
                if (!op.parentId.empty()) {
                    emitStringEntry(em, "parent", op.parentId);
                }
                emitKey(em, "node");
                em << YAML::Value;
                emitNodeMap(em, std::list<NodeSerializationPtr>(1, op.node), std::string(), sink, usedPaths, true);
            } else if constexpr (std::is_same_v<T, DeleteNodeOp>) {
                emitStringEntry(em, "node", op.nodeId);
            } else if constexpr (std::is_same_v<T, SetKnobOp>) {
                if (op.nodeId) {
                    emitStringEntry(em, "node", *op.nodeId);
                } else {
                    emitKey(em, "project");
                    em << YAML::Value;
                    emitBool(em, true);
                }
                emitStringEntry(em, "knob", op.knobName);
                if (op.value) {
                    emitKey(em, "value");
                    em << YAML::Value;
                    emitKnobValue(em, *op.value);
                } else {
                    emitKey(em, "default");
                    em << YAML::Value;
                    emitBool(em, true);
                }
            } else if constexpr (std::is_same_v<T, SetInputOp>) {
                emitStringEntry(em, "node", op.nodeId);
                emitStringEntry(em, "input", op.inputLabel);
                if (op.sourceId.empty()) {
                    emitNullEntry(em, "source");
                } else {
                    emitStringEntry(em, "source", op.sourceId);
                }
            } else if constexpr (std::is_same_v<T, RenameOp>) {
                emitStringEntry(em, "node", op.nodeId);
                emitStringEntry(em, "name", op.scriptName);
                if (op.label) {
                    emitStringEntry(em, "label", *op.label);
                }
            } else if constexpr (std::is_same_v<T, SetNodeUiOp>) {
                emitStringEntry(em, "node", op.nodeId);
                if (op.position) {
                    emitDoublePair(em, "pos", op.position->first, op.position->second);
                }
                if (op.size) {
                    emitDoublePair(em, "size", op.size->first, op.size->second);
                }
                emitColorEntry(em, "color", op.color);
                emitColorEntry(em, "overlay", op.overlayColor);
            } else if constexpr (std::is_same_v<T, SetUserKnobsOp>) {
                emitStringEntry(em, "node", op.nodeId);
                emitKey(em, "pages");
                em << YAML::Value;
                emitStringList(em, op.pagesOrder);
                emitKey(em, "userPages");
                em << YAML::Value;
                emitUserPages(em, op.userPages);
            } else if constexpr (std::is_same_v<T, SetRotoDataOp>) {
                emitStringEntry(em, "node", op.nodeId);
                if (op.roto) {
                    emitKey(em, "roto");
                    em << YAML::Value;
                    emitRotoContext(em, *op.roto, BlobEncodeContext { &sink, op.nodeId, usedPaths });
                } else {
                    emitNullEntry(em, "roto");
                }
            } else if constexpr (std::is_same_v<T, SetTrackerDataOp>) {
                emitStringEntry(em, "node", op.nodeId);
                if (op.tracker) {
                    emitKey(em, "tracker");
                    em << YAML::Value;
                    emitTrackerContext(em, *op.tracker, BlobEncodeContext { &sink, op.nodeId, usedPaths });
                } else {
                    emitNullEntry(em, "tracker");
                }
            } else {
                static_assert(std::is_same_v<T, SetProjectDataOp>);
                if (op.formats) {
                    emitKey(em, "formats");
                    em << YAML::Value;
                    emitFormats(em, *op.formats);
                }
                if (op.layers) {
                    emitKey(em, "layers");
                    em << YAML::Value;
                    emitLayers(em, *op.layers);
                }
            }
        },
                   graphOp);
        em << YAML::EndMap;
    }

    YAML::Node
    required(MapReader& reader,
             const YAML::Node& map,
             std::string_view key)
    {
        YAML::Node value = reader.get(key);
        if (!value) {
            fail(map, reader.context(), "missing \"" + std::string(key) + "\"");
        }

        return value;
    }

    std::string
    requiredString(MapReader& reader,
                   const YAML::Node& map,
                   std::string_view key)
    {
        return readString(required(reader, map, key), reader.context() + "." + std::string(key));
    }

    std::string
    requiredId(MapReader& reader,
               const YAML::Node& map,
               std::string_view key)
    {
        std::string id = requiredString(reader, map, key);
        if (id.empty()) {
            fail(map, reader.context() + "." + std::string(key), "expected a node ID");
        }

        return id;
    }

    std::optional<std::optional<NodeUiColor>>
    readColorEntry(MapReader& reader,
                   std::string_view key)
    {
        std::optional<std::optional<NodeUiColor>> color;
        YAML::Node value = reader.get(key);
        if (!value) {
            return color;
        }
        if (value.IsNull()) {
            color.emplace(std::nullopt);
        } else {
            const std::vector<float> rgb = readFloatTriple(value, reader.context() + "." + std::string(key));
            color.emplace(NodeUiColor { rgb[0], rgb[1], rgb[2] });
        }

        return color;
    }

    GraphOp
    readOp(const YAML::Node& map,
           const std::string& context,
           YamlWarnings* warnings)
    {
        if (!map || !map.IsMap()) {
            fail(map, context, "expected an operation map");
        }
        MapReader reader(map, context, warnings);
        const std::string name = requiredString(reader, map, "op");
        GraphOp result;
        if (name == "createNode") {
            CreateNodeOp op;
            if (YAML::Node parent = reader.get("parent")) {
                op.parentId = readString(parent, context + ".parent");
            }
            YAML::Node nodeMap = required(reader, map, "node");
            std::list<NodeSerializationPtr> nodes = readNodeMap(nodeMap, NoBlobSource(), context + ".node", warnings);
            if (nodes.size() != 1) {
                fail(nodeMap, context + ".node", "expected exactly one node");
            }
            op.node = nodes.front();
            if (op.node->id.empty()) {
                fail(nodeMap, context + ".node", "a created node needs an id");
            }
            result = std::move(op);
        } else if (name == "deleteNode") {
            result = DeleteNodeOp { requiredId(reader, map, "node") };
        } else if (name == "setKnob") {
            SetKnobOp op;
            YAML::Node project = reader.get("project");
            YAML::Node node = reader.get("node");
            if (project && node) {
                fail(map, context, "a knob belongs to a node or to the project, not both");
            }
            if (project) {
                if (!readBool(project, context + ".project")) {
                    fail(project, context + ".project", "expected true");
                }
            } else {
                op.nodeId = requiredId(reader, map, "node");
            }
            op.knobName = requiredString(reader, map, "knob");
            if (op.knobName.empty()) {
                fail(map, context + ".knob", "expected a knob name");
            }
            YAML::Node value = reader.get("value");
            YAML::Node reset = reader.get("default");
            if (value && reset) {
                fail(map, context, "expected either a value or default, not both");
            }
            if (reset) {
                if (!readBool(reset, context + ".default")) {
                    fail(reset, context + ".default", "expected true");
                }
            } else if (value) {
                op.value = readKnob(op.knobName, value, context + ".value", warnings);
            } else {
                fail(map, context, "missing \"value\" or \"default\"");
            }
            result = std::move(op);
        } else if (name == "setInput") {
            SetInputOp op;
            op.nodeId = requiredId(reader, map, "node");
            op.inputLabel = requiredString(reader, map, "input");
            op.sourceId = requiredString(reader, map, "source");
            result = std::move(op);
        } else if (name == "rename") {
            RenameOp op;
            op.nodeId = requiredId(reader, map, "node");
            op.scriptName = requiredString(reader, map, "name");
            if (YAML::Node label = reader.get("label")) {
                op.label = readString(label, context + ".label");
            }
            result = std::move(op);
        } else if (name == "setNodeUi") {
            SetNodeUiOp op;
            op.nodeId = requiredId(reader, map, "node");
            if (YAML::Node pos = reader.get("pos")) {
                const std::vector<double> values = readDoubleVector(pos, context + ".pos", 2);
                op.position = std::make_pair(values[0], values[1]);
            }
            if (YAML::Node size = reader.get("size")) {
                const std::vector<double> values = readDoubleVector(size, context + ".size", 2);
                op.size = std::make_pair(values[0], values[1]);
            }
            op.color = readColorEntry(reader, "color");
            op.overlayColor = readColorEntry(reader, "overlay");
            result = std::move(op);
        } else if (name == "setUserKnobs") {
            SetUserKnobsOp op;
            op.nodeId = requiredId(reader, map, "node");
            op.pagesOrder = readStringList(required(reader, map, "pages"), context + ".pages");
            op.userPages = readUserPages(required(reader, map, "userPages"), context + ".userPages", warnings);
            result = std::move(op);
        } else if (name == "setRotoData") {
            SetRotoDataOp op;
            op.nodeId = requiredId(reader, map, "node");
            YAML::Node roto = required(reader, map, "roto");
            if (!roto.IsNull()) {
                op.roto = readRotoContext(roto, NoBlobSource(), context + ".roto", warnings);
            }
            result = std::move(op);
        } else if (name == "setTrackerData") {
            SetTrackerDataOp op;
            op.nodeId = requiredId(reader, map, "node");
            YAML::Node tracker = required(reader, map, "tracker");
            if (!tracker.IsNull()) {
                op.tracker = readTrackerContext(tracker, NoBlobSource(), context + ".tracker", warnings);
            }
            result = std::move(op);
        } else if (name == "setProjectData") {
            SetProjectDataOp op;
            if (YAML::Node formats = reader.get("formats")) {
                op.formats = readFormats(formats, warnings);
            }
            if (YAML::Node layers = reader.get("layers")) {
                op.layers = readLayers(layers, warnings);
            }
            result = std::move(op);
        } else {
            fail(map, context + ".op", "unknown operation \"" + name + "\"");
        }
        reader.finish();

        return result;
    }

} // namespace

std::string
encodeGraphOpYaml(const GraphOp& op)
{
    YAML::Emitter em;
    InlineBlobSink sink;
    std::set<std::string> usedPaths;
    emitOp(em, op, sink, &usedPaths);

    return documentText(em);
}

std::string
encodeGraphOpsYaml(const std::vector<GraphOp>& ops)
{
    YAML::Emitter em;
    InlineBlobSink sink;
    std::set<std::string> usedPaths;
    if (ops.empty()) {
        em << YAML::Flow << YAML::BeginSeq << YAML::EndSeq;
    } else {
        em << YAML::BeginSeq;
        for (const GraphOp& op : ops) {
            emitOp(em, op, sink, &usedPaths);
        }
        em << YAML::EndSeq;
    }

    return documentText(em);
}

GraphOp
decodeGraphOpYaml(std::string_view text,
                  YamlWarnings* warnings)
{
    const YAML::Node root = loadDocument(text, "a graph operation");

    return readOp(root, "op", warnings);
}

std::vector<DecodedGraphOp>
decodeGraphOpsYaml(std::string_view text,
                   YamlWarnings* warnings)
{
    const YAML::Node root = loadDocument(text, "graph operations");
    std::vector<DecodedGraphOp> result;
    if (!root || root.IsNull()) {
        return result;
    }
    if (!root.IsSequence()) {
        fail(root, "ops", "expected a list of operations");
    }
    for (std::size_t i = 0; i < root.size(); ++i) {
        DecodedGraphOp entry;
        try {
            entry.op = readOp(root[i], "ops[" + std::to_string(i) + "]", warnings);
        } catch (const YamlError& e) {
            entry.error = e.what();
        } catch (const YAML::Exception& e) {
            entry.error = e.what();
        }
        result.push_back(std::move(entry));
    }

    return result;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
