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

#include "Serialization/Yaml/MergeConflictYaml.h"

#include <set>

#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/KnobYaml.h"
#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    bool
    hasSideValue(const MergeConflictValue& value)
    {
        return value.knob.has_value() || value.key.has_value() || !value.otherKeys.empty() || !value.connections.empty() || value.parent.has_value() || value.knobs.has_value() || value.node || value.roto.has_value() || value.tracker.has_value() || !value.itemsBefore.empty();
    }

    void
    emitTypedKnob(YAML::Emitter& em,
                  const KnobSerialization& knob)
    {
        using namespace YamlDetail;
        emitKey(em, "type");
        em << YAML::Value;
        emitString(em, knob.typeName);
        emitKey(em, "dimensions");
        em << YAML::Value;
        emitInt(em, knob.dimension);
        emitKey(em, "knob");
        em << YAML::Value;
        emitKnobValue(em, knob);
    }

    KnobSerialization
    readTypedKnob(YamlDetail::MapReader& reader,
                  const YAML::Node& knob,
                  const std::string& knobName,
                  const std::string& context,
                  YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        const std::string typeName = readString(reader.get("type"), context + ".type");
        const int dimensions = readInt(reader.get("dimensions"), context + ".dimensions");
        KnobSerialization result = readKnob(knobName, knob, context + ".knob", warnings);
        completeKnobSerialization(&result, typeName, dimensions);

        return result;
    }

    bool
    namesSide(const MergeConflict& conflict)
    {
        return (conflict.kind == MergeConflictKind::eDeleteModify) || (conflict.kind == MergeConflictKind::eAdopted) || (conflict.side != MergeSide::eMine);
    }

    void
    emitOptionalString(YAML::Emitter& em,
                       std::string_view key,
                       const std::string& value)
    {
        using namespace YamlDetail;
        if (!value.empty()) {
            emitKey(em, key);
            em << YAML::Value;
            emitString(em, value);
        }
    }

    void
    emitStateEntries(YAML::Emitter& em,
                     const MergeConflictValue& value,
                     const std::string& nodeId)
    {
        using namespace YamlDetail;
        if (value.knob) {
            emitTypedKnob(em, *value.knob);
        }
        if (value.key) {
            CurveSerialization curve;
            curve.keyframes.push_back(*value.key);
            emitKey(em, "key");
            em << YAML::Value;
            emitCurve(em, curve);
        }
        if (!value.otherKeys.empty()) {
            CurveSerialization curve;
            curve.keyframes.assign(value.otherKeys.begin(), value.otherKeys.end());
            emitKey(em, "otherKeys");
            em << YAML::Value;
            emitCurve(em, curve);
        }
        if (!value.connections.empty()) {
            emitKey(em, "connections");
            em << YAML::Value << YAML::BeginSeq;
            for (const MergeConflictConnection& connection : value.connections) {
                em << YAML::BeginMap;
                emitKey(em, "node");
                em << YAML::Value;
                emitString(em, connection.nodeId);
                emitKey(em, "input");
                em << YAML::Value;
                emitString(em, connection.input);
                emitKey(em, "source");
                em << YAML::Value;
                emitString(em, connection.sourceId);
                em << YAML::EndMap;
            }
            em << YAML::EndSeq;
        }
        if (value.parent) {
            emitKey(em, "parent");
            em << YAML::Value;
            emitString(em, *value.parent);
        }
        if (value.knobs) {
            emitKey(em, "knobs");
            em << YAML::Value << YAML::BeginSeq;
            for (const KnobSerialization& knob : *value.knobs) {
                em << YAML::BeginMap;
                emitKey(em, "name");
                em << YAML::Value;
                emitString(em, knob.name);
                emitTypedKnob(em, knob);
                em << YAML::EndMap;
            }
            em << YAML::EndSeq;
        }
        InlineBlobSink sink;
        std::set<std::string> usedPaths;
        if (value.node) {
            emitKey(em, "node");
            em << YAML::Value;
            emitNodeMap(em, std::list<NodeSerializationPtr>(1, value.node), std::string(), sink, &usedPaths, true, true);
        }
        if (value.roto) {
            emitKey(em, "roto");
            em << YAML::Value;
            emitRotoContext(em, *value.roto, BlobEncodeContext { &sink, nodeId, &usedPaths });
        }
        if (value.tracker) {
            emitKey(em, "tracker");
            em << YAML::Value;
            emitTrackerContext(em, *value.tracker, BlobEncodeContext { &sink, nodeId, &usedPaths });
        }
        if (!value.itemsBefore.empty()) {
            emitKey(em, "itemsBefore");
            em << YAML::Value;
            emitStringList(em, value.itemsBefore);
        }
    }

    void
    emitSideValue(YAML::Emitter& em,
                  std::string_view key,
                  const MergeConflictValue& value,
                  const std::string& nodeId)
    {
        using namespace YamlDetail;
        if (!hasSideValue(value)) {
            return;
        }
        emitKey(em, key);
        em << YAML::Value << YAML::BeginMap;
        emitStateEntries(em, value, nodeId);
        em << YAML::EndMap;
    }

    void
    emitConflict(YAML::Emitter& em,
                 const MergeConflict& conflict)
    {
        using namespace YamlDetail;
        em << YAML::BeginMap;
        emitOptionalString(em, "template", conflict.templateKey);
        emitKey(em, "kind");
        em << YAML::Value << std::string(mergeConflictKindName(conflict.kind));
        emitOptionalString(em, "baseNode", conflict.baseNodeId);
        emitOptionalString(em, "node", conflict.nodeId);
        emitOptionalString(em, "knob", conflict.knob);
        if (conflict.dimension == kMergeConflictAllDimensions) {
            emitKey(em, "dim");
            em << YAML::Value << std::string("all");
        } else if (conflict.dimension != -1) {
            emitKey(em, "dim");
            em << YAML::Value;
            emitInt(em, conflict.dimension);
        }
        if (conflict.time) {
            emitKey(em, "time");
            em << YAML::Value;
            emitDouble(em, *conflict.time);
        }
        emitOptionalString(em, "item", conflict.item);
        emitOptionalString(em, "input", conflict.input);
        if (!conflict.inputs.empty()) {
            emitKey(em, "inputs");
            em << YAML::Value;
            emitStringList(em, conflict.inputs);
        }
        if (namesSide(conflict)) {
            emitKey(em, "side");
            em << YAML::Value << std::string((conflict.side == MergeSide::eMine) ? "mine" : "theirs");
        }
        if (!conflict.defaultedKnobs.empty()) {
            emitKey(em, "defaulted");
            em << YAML::Value;
            emitStringList(em, conflict.defaultedKnobs);
        }
        emitSideValue(em, "base", conflict.base, conflict.nodeId);
        emitSideValue(em, "mine", conflict.mine, conflict.nodeId);
        emitSideValue(em, "theirs", conflict.theirs, conflict.nodeId);
        if (!conflict.options.empty()) {
            emitKey(em, "options");
            em << YAML::Value << YAML::BeginSeq;
            for (const MergeConflictOption& option : conflict.options) {
                em << YAML::BeginMap;
                emitKey(em, "name");
                em << YAML::Value;
                emitString(em, option.name);
                emitKey(em, "label");
                em << YAML::Value;
                emitString(em, option.label);
                emitStateEntries(em, option.state, conflict.nodeId);
                em << YAML::EndMap;
            }
            em << YAML::EndSeq;
        }
        em << YAML::EndMap;
    }

    MergeConflictValue
    readStateEntries(YamlDetail::MapReader& reader,
                     const std::string& knobName,
                     const std::string& context,
                     YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        MergeConflictValue value;
        if (YAML::Node knob = reader.get("knob")) {
            value.knob = readTypedKnob(reader, knob, knobName, context, warnings);
        }
        if (YAML::Node key = reader.get("key")) {
            const CurveSerialization curve = readCurve(key, context + ".key");
            if (curve.keyframes.size() != 1) {
                fail(key, context + ".key", "expected a curve of one key");
            }
            value.key = curve.keyframes.front();
        }
        if (YAML::Node otherKeys = reader.get("otherKeys")) {
            const CurveSerialization curve = readCurve(otherKeys, context + ".otherKeys");
            value.otherKeys.assign(curve.keyframes.begin(), curve.keyframes.end());
        }
        if (YAML::Node connections = reader.get("connections")) {
            if (!connections.IsSequence()) {
                fail(connections, context + ".connections", "expected a list of connections");
            }
            for (std::size_t i = 0; i < connections.size(); ++i) {
                const std::string where = context + ".connections[" + std::to_string(i) + "]";
                if (!connections[i].IsMap()) {
                    fail(connections[i], where, "expected a map");
                }
                MapReader connectionReader(connections[i], where, warnings);
                MergeConflictConnection connection;
                connection.nodeId = readString(connectionReader.get("node"), where + ".node");
                connection.input = readString(connectionReader.get("input"), where + ".input");
                connection.sourceId = readString(connectionReader.get("source"), where + ".source");
                connectionReader.finish();
                value.connections.push_back(std::move(connection));
            }
        }
        if (YAML::Node parent = reader.get("parent")) {
            value.parent = readString(parent, context + ".parent");
        }
        if (YAML::Node knobs = reader.get("knobs")) {
            if (!knobs.IsSequence()) {
                fail(knobs, context + ".knobs", "expected a list of knobs");
            }
            value.knobs.emplace();
            for (std::size_t i = 0; i < knobs.size(); ++i) {
                const std::string where = context + ".knobs[" + std::to_string(i) + "]";
                if (!knobs[i].IsMap()) {
                    fail(knobs[i], where, "expected a map");
                }
                MapReader knobReader(knobs[i], where, warnings);
                const std::string name = readString(knobReader.get("name"), where + ".name");
                const YAML::Node knob = knobReader.get("knob");
                if (!knob) {
                    fail(knobs[i], where, "expected a knob");
                }
                value.knobs->push_back(readTypedKnob(knobReader, knob, name, where, warnings));
                knobReader.finish();
            }
        }
        if (YAML::Node node = reader.get("node")) {
            std::list<NodeSerializationPtr> nodes = readNodeMap(node, NoBlobSource(), context + ".node", warnings);
            if ((nodes.size() != 1) || !nodes.front() || nodes.front()->id.empty()) {
                fail(node, context + ".node", "expected exactly one node, with its id");
            }
            value.node = nodes.front();
        }
        if (YAML::Node roto = reader.get("roto")) {
            value.roto = readRotoContext(roto, NoBlobSource(), context + ".roto", warnings);
        }
        if (YAML::Node tracker = reader.get("tracker")) {
            value.tracker = readTrackerContext(tracker, NoBlobSource(), context + ".tracker", warnings);
        }
        if (YAML::Node itemsBefore = reader.get("itemsBefore")) {
            value.itemsBefore = readStringVector(itemsBefore, context + ".itemsBefore");
        }

        return value;
    }

    MergeConflictValue
    readSideValue(const YAML::Node& node,
                  const std::string& knobName,
                  const std::string& context,
                  YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        if (!node.IsMap()) {
            fail(node, context, "expected a map");
        }
        MapReader reader(node, context, warnings);
        MergeConflictValue value = readStateEntries(reader, knobName, context, warnings);
        reader.finish();

        return value;
    }

    MergeConflict
    readConflict(const YAML::Node& node,
                 const std::string& context,
                 YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        if (!node.IsMap()) {
            fail(node, context, "expected a map");
        }
        MergeConflict conflict;
        MapReader reader(node, context, warnings);
        const YAML::Node kindNode = reader.get("kind");
        const std::string kindName = readString(kindNode, context + ".kind");
        const std::optional<MergeConflictKind> kind = mergeConflictKindFromName(kindName);
        if (!kind) {
            fail(kindNode, context + ".kind", "unknown conflict kind \"" + kindName + "\"");
        }
        conflict.kind = *kind;
        if (YAML::Node templateKey = reader.get("template")) {
            conflict.templateKey = readString(templateKey, context + ".template");
        }
        if (YAML::Node baseNode = reader.get("baseNode")) {
            conflict.baseNodeId = readString(baseNode, context + ".baseNode");
        }
        if (YAML::Node nodeId = reader.get("node")) {
            conflict.nodeId = readString(nodeId, context + ".node");
        }
        if (YAML::Node knob = reader.get("knob")) {
            conflict.knob = readString(knob, context + ".knob");
        }
        if (YAML::Node dimension = reader.get("dim")) {
            if (dimension.IsScalar() && (dimension.Scalar() == "all")) {
                conflict.dimension = kMergeConflictAllDimensions;
            } else {
                conflict.dimension = readInt(dimension, context + ".dim");
            }
        }
        if (YAML::Node time = reader.get("time")) {
            conflict.time = readDouble(time, context + ".time");
        }
        if (YAML::Node item = reader.get("item")) {
            conflict.item = readString(item, context + ".item");
        }
        if (YAML::Node input = reader.get("input")) {
            conflict.input = readString(input, context + ".input");
        }
        if (YAML::Node inputs = reader.get("inputs")) {
            conflict.inputs = readStringVector(inputs, context + ".inputs");
        }
        if (YAML::Node side = reader.get("side")) {
            const std::string sideName = readString(side, context + ".side");
            if (sideName == "mine") {
                conflict.side = MergeSide::eMine;
            } else if (sideName == "theirs") {
                conflict.side = MergeSide::eTheirs;
            } else {
                fail(side, context + ".side", "expected mine or theirs");
            }
        }
        if (YAML::Node defaulted = reader.get("defaulted")) {
            conflict.defaultedKnobs = readStringVector(defaulted, context + ".defaulted");
        }
        if (YAML::Node base = reader.get("base")) {
            conflict.base = readSideValue(base, conflict.knob, context + ".base", warnings);
        }
        if (YAML::Node mine = reader.get("mine")) {
            conflict.mine = readSideValue(mine, conflict.knob, context + ".mine", warnings);
        }
        if (YAML::Node theirs = reader.get("theirs")) {
            conflict.theirs = readSideValue(theirs, conflict.knob, context + ".theirs", warnings);
        }
        if (YAML::Node options = reader.get("options")) {
            if (!options.IsSequence()) {
                fail(options, context + ".options", "expected a list of options");
            }
            for (std::size_t i = 0; i < options.size(); ++i) {
                const std::string where = context + ".options[" + std::to_string(i) + "]";
                if (!options[i].IsMap()) {
                    fail(options[i], where, "expected a map");
                }
                MapReader optionReader(options[i], where, warnings);
                MergeConflictOption option;
                option.name = readString(optionReader.get("name"), where + ".name");
                option.label = readString(optionReader.get("label"), where + ".label");
                option.state = readStateEntries(optionReader, conflict.knob, where, warnings);
                optionReader.finish();
                conflict.options.push_back(std::move(option));
            }
        }
        reader.finish();

        return conflict;
    }

} // anon namespace

std::string
encodeMergeConflictsYaml(const std::vector<MergeConflict>& conflicts)
{
    using namespace YamlDetail;
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitDocumentFormat(em, kMergeConflictsYamlFormat, kMergeConflictsYamlFormatVersion);
    emitKey(em, "conflicts");
    em << YAML::Value << YAML::BeginSeq;
    for (const MergeConflict& conflict : conflicts) {
        emitConflict(em, conflict);
    }
    em << YAML::EndSeq;
    em << YAML::EndMap;

    return documentText(em);
}

std::vector<MergeConflict>
decodeMergeConflictsYaml(std::string_view text,
                         YamlWarnings* warnings)
{
    using namespace YamlDetail;
    YAML::Node document = loadDocument(text, std::string(kMergeConflictsMember));
    if (!document.IsMap()) {
        throw YamlError("this is not a Natron merge conflict list: it is not a YAML map");
    }
    MapReader reader(document, "conflicts", warnings);
    checkDocumentFormat(reader, kMergeConflictsYamlFormat, kMergeConflictsYamlFormatVersion, "a Natron merge conflict list");
    YAML::Node list = reader.get("conflicts");
    if (!list || !list.IsSequence()) {
        fail(list ? list : document, "conflicts", "expected a list of conflicts");
    }
    std::vector<MergeConflict> conflicts;
    for (std::size_t i = 0; i < list.size(); ++i) {
        conflicts.push_back(readConflict(list[i], "conflicts[" + std::to_string(i) + "]", warnings));
    }
    reader.finish();

    return conflicts;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
