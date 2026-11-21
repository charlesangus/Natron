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

#include "Serialization/Yaml/KnobYaml.h"
#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    bool
    hasSideValue(const MergeConflictValue& value)
    {
        return value.knob.has_value() || value.key.has_value();
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
    emitSideValue(YAML::Emitter& em,
                  std::string_view key,
                  const MergeConflictValue& value)
    {
        using namespace YamlDetail;
        if (!hasSideValue(value)) {
            return;
        }
        emitKey(em, key);
        em << YAML::Value << YAML::BeginMap;
        if (value.knob) {
            emitKey(em, "type");
            em << YAML::Value;
            emitString(em, value.knob->typeName);
            emitKey(em, "dimensions");
            em << YAML::Value;
            emitInt(em, value.knob->dimension);
            emitKey(em, "knob");
            em << YAML::Value;
            emitKnobValue(em, *value.knob);
        }
        if (value.key) {
            CurveSerialization curve;
            curve.keyframes.push_back(*value.key);
            emitKey(em, "key");
            em << YAML::Value;
            emitCurve(em, curve);
        }
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
        if (conflict.dimension != -1) {
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
        emitSideValue(em, "base", conflict.base);
        emitSideValue(em, "mine", conflict.mine);
        emitSideValue(em, "theirs", conflict.theirs);
        em << YAML::EndMap;
    }

    MergeConflictValue
    readSideValue(const YAML::Node& node,
                  const std::string& knobName,
                  const std::string& context,
                  YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        MergeConflictValue value;
        MapReader reader(node, context, warnings);
        if (YAML::Node knob = reader.get("knob")) {
            const std::string typeName = readString(reader.get("type"), context + ".type");
            const int dimensions = readInt(reader.get("dimensions"), context + ".dimensions");
            value.knob = readKnob(knobName, knob, context + ".knob", warnings);
            completeKnobSerialization(&(*value.knob), typeName, dimensions);
        }
        if (YAML::Node key = reader.get("key")) {
            const CurveSerialization curve = readCurve(key, context + ".key");
            if (curve.keyframes.size() != 1) {
                fail(key, context + ".key", "expected a curve of one key");
            }
            value.key = curve.keyframes.front();
        }
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
            conflict.dimension = readInt(dimension, context + ".dim");
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
