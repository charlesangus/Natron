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

#include "Serialization/Yaml/KnobYaml.h"

#include <cmath>
#include <limits>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace YamlDetail {

    namespace {

        // A map holding any of these is a knob with knob-level data; any other map is the form of a
        // single dimension.
        const char* const kKnobLevelKeys[] = { "secret", "alias", "option", "strings", "curves", "user" };

        const char* const kCurveInTable = "table";

        bool
        isKnobLevelMap(const YAML::Node& node)
        {
            if (!node.IsMap()) {
                return false;
            }
            for (const char* key : kKnobLevelKeys) {
                if (node[key]) {
                    return true;
                }
            }

            return false;
        }

        bool
        hasExpression(const KnobDimensionSerialization& dimension)
        {
            return !dimension.expression.empty() || dimension.expressionHasReturnVariable;
        }

        bool
        isDimensionEmpty(const KnobDimensionSerialization& dimension)
        {
            return !dimension.value && !dimension.curve && !hasExpression(dimension) && !dimension.master && !dimension.linkGroup && dimension.enabled;
        }

        bool
        isDimensionPlainValue(const KnobDimensionSerialization& dimension)
        {
            return dimension.value && !dimension.curve && !hasExpression(dimension) && !dimension.master && !dimension.linkGroup && dimension.enabled;
        }

        void
        emitValue(YAML::Emitter& em,
                  const KnobValueSerialization& value)
        {
            if (const bool* b = std::get_if<bool>(&value)) {
                emitBool(em, *b);
            } else if (const int* i = std::get_if<int>(&value)) {
                emitInt(em, *i);
            } else if (const double* d = std::get_if<double>(&value)) {
                emitDouble(em, *d);
            } else if (const std::string* s = std::get_if<std::string>(&value)) {
                emitString(em, *s);
            }
        }

        // A value's type is only known from its YAML form: quoted text is a string, true and false
        // are booleans, an integer that fits is an int and any other number a double.
        KnobValueSerialization
        readValue(const YAML::Node& node,
                  const std::string& context)
        {
            if (!node.IsScalar()) {
                fail(node, context, "expected a value");
            }
            const std::string& text = node.Scalar();
            if (!isPlainScalar(node)) {
                return KnobValueSerialization(std::in_place_type<std::string>, text);
            }
            if ((text == "true") || (text == "false")) {
                return KnobValueSerialization(std::in_place_type<bool>, text == "true");
            }
            std::optional<std::int64_t> integer = parseYamlInteger(text);
            const bool negativeZero = integer && (*integer == 0) && (text.front() == '-');
            if (integer && !negativeZero) {
                if ((*integer >= std::numeric_limits<int>::min()) && (*integer <= std::numeric_limits<int>::max())) {
                    return KnobValueSerialization(std::in_place_type<int>, (int)*integer);
                }
            }
            if (std::optional<double> number = parseYamlDouble(text)) {
                return KnobValueSerialization(std::in_place_type<double>, *number);
            }

            return KnobValueSerialization(std::in_place_type<std::string>, text);
        }

        std::string
        lastNameComponent(const std::string& fullName)
        {
            std::size_t dot = fullName.rfind('.');

            return (dot == std::string::npos) ? fullName : fullName.substr(dot + 1);
        }

        std::string
        linkText(const KnobMasterSerialization& master)
        {
            if (master.masterNodeNameFull.empty() && master.masterKnobName.empty() && (master.masterDimension < 0)) {
                return std::string();
            }
            std::string text = master.masterNodeNameFull + "." + master.masterKnobName;
            if (master.masterDimension >= 0) {
                text += "." + std::to_string(master.masterDimension);
            }

            return text;
        }

        KnobMasterSerialization
        parseLink(const YAML::Node& node,
                  const std::string& context)
        {
            KnobMasterSerialization master;
            std::string text = readString(node, context);
            if (text.empty()) {
                return master;
            }
            std::size_t dot = text.rfind('.');
            if (dot == std::string::npos) {
                fail(node, context, "a link is \"<node>.<knob>[.<dimension>]\", found \"" + text + "\"");
            }
            std::string last = text.substr(dot + 1);
            if (std::optional<std::int64_t> dimension = parseYamlInteger(last)) {
                if ((*dimension < 0) || (*dimension > std::numeric_limits<int>::max()) || (last.front() == '+')) {
                    fail(node, context, "invalid link dimension in \"" + text + "\"");
                }
                master.masterDimension = (int)*dimension;
                text.resize(dot);
                dot = text.rfind('.');
                if (dot == std::string::npos) {
                    fail(node, context, "a link is \"<node>.<knob>[.<dimension>]\", found \"" + readString(node, context) + "\"");
                }
            }
            master.masterKnobName = text.substr(dot + 1);
            master.masterNodeNameFull = text.substr(0, dot);
            master.masterNodeName = lastNameComponent(master.masterNodeNameFull);

            return master;
        }

        // A string knob's curve is the keyframe index of its strings with constant interpolation;
        // when it is exactly that, the strings alone say everything. Derivatives are not compared:
        // a live curve gives its end keys nonzero ones even when constant, and the format drops
        // them for every interpolation that does not store them.
        bool
        isStringCurveImplied(const KnobSerialization& knob)
        {
            if (!knob.stringAnimation || knob.stringAnimation->empty() || (knob.dimensions.size() != 1) || !knob.dimensions[0].curve) {
                return false;
            }
            const std::vector<KeyFrameSerialization>& keys = knob.dimensions[0].curve->keyframes;
            if (keys.size() != knob.stringAnimation->size()) {
                return false;
            }
            std::size_t i = 0;
            for (std::map<int, std::string>::const_iterator it = knob.stringAnimation->begin(); it != knob.stringAnimation->end(); ++it, ++i) {
                const KeyFrameSerialization& key = keys[i];
                if ((key.time != (double)it->first) || (key.value != (double)i) || (key.interpolation != kYamlKeyframeConstant)) {
                    return false;
                }
            }

            return true;
        }

        void
        emitStringAnimation(YAML::Emitter& em,
                            const std::map<int, std::string>& strings)
        {
            em << YAML::Flow << YAML::BeginSeq;
            for (std::map<int, std::string>::const_iterator it = strings.begin(); it != strings.end(); ++it) {
                emitInt(em, it->first);
                emitString(em, it->second);
            }
            em << YAML::EndSeq;
        }

        std::map<int, std::string>
        readStringAnimation(const YAML::Node& node,
                            const std::string& context)
        {
            std::map<int, std::string> strings;
            if (!node.IsSequence() || ((node.size() % 2) != 0)) {
                fail(node, context, "expected a string animation [time, \"string\", ...]");
            }
            for (std::size_t i = 0; i < node.size(); i += 2) {
                const int time = readInt(node[i], context);
                if (!node[i + 1].IsScalar()) {
                    fail(node[i + 1], context, "expected the string of the keyframe at time " + std::to_string(time));
                }
                if (!strings.emplace(time, node[i + 1].Scalar()).second) {
                    fail(node[i], context, "two strings at time " + std::to_string(time));
                }
            }

            return strings;
        }

        bool
        isStringAnimationForm(const YAML::Node& node)
        {
            if (!node.IsSequence()) {
                return false;
            }
            for (std::size_t i = 0; i < node.size(); ++i) {
                if (node[i].IsScalar() && !isPlainScalar(node[i])) {
                    return true;
                }
            }

            return false;
        }

        CurveSerialization
        impliedStringCurve(const std::map<int, std::string>& strings)
        {
            CurveSerialization curve;
            double index = 0.;
            for (std::map<int, std::string>::const_iterator it = strings.begin(); it != strings.end(); ++it) {
                KeyFrameSerialization key;
                key.time = it->first;
                key.value = index;
                key.interpolation = kYamlKeyframeConstant;
                curve.keyframes.push_back(key);
                index += 1.;
            }

            return curve;
        }

        const KnobChoiceExtraSerialization*
        choiceExtra(const KnobSerialization& knob)
        {
            return std::get_if<KnobChoiceExtraSerialization>(&knob.extraData);
        }

        bool
        isChoiceShortForm(const KnobSerialization& knob)
        {
            if (!knob.choiceOptionID || knob.choiceOptionID->empty() || (knob.dimensions.size() != 1) || !isDimensionPlainValue(knob.dimensions[0])) {
                return false;
            }
            const int* index = std::get_if<int>(&(*knob.dimensions[0].value));
            const KnobChoiceExtraSerialization* extra = choiceExtra(knob);
            if (!index || !extra || (*index < 0) || (*index >= (int)extra->entries.size())) {
                return false;
            }

            return extra->entries[*index] == *knob.choiceOptionID;
        }

        struct KnobForm {
            bool choiceShortForm;
            bool stringCurveImplied;
        };

        void
        emitDimension(YAML::Emitter& em,
                      const KnobSerialization& knob,
                      std::size_t index,
                      const KnobForm& form,
                      const KnobEmitOptions& options)
        {
            const KnobDimensionSerialization& dimension = knob.dimensions[index];
            if (form.choiceShortForm) {
                emitString(em, *knob.choiceOptionID);

                return;
            }
            if (isDimensionPlainValue(dimension)) {
                emitValue(em, *dimension.value);

                return;
            }
            if (isDimensionEmpty(dimension)) {
                em << YAML::Null;

                return;
            }
            em << YAML::Flow << YAML::BeginMap;
            if (dimension.curve) {
                emitKey(em, "curve");
                em << YAML::Value;
                if (options.curveInTable && options.curveInTable(knob, index)) {
                    em << kCurveInTable;
                } else if (form.stringCurveImplied) {
                    emitStringAnimation(em, *knob.stringAnimation);
                } else {
                    emitCurve(em, *dimension.curve);
                }
            }
            if (hasExpression(dimension)) {
                emitKey(em, "expr");
                em << YAML::Value;
                emitString(em, dimension.expression);
                emitKey(em, "hasRet");
                em << YAML::Value;
                emitBool(em, dimension.expressionHasReturnVariable);
            }
            if (dimension.master) {
                const KnobMasterSerialization& master = *dimension.master;
                emitKey(em, "link");
                em << YAML::Value;
                emitString(em, linkText(master));
                if (!master.masterTrackName.empty()) {
                    emitKey(em, "track");
                    em << YAML::Value;
                    emitString(em, master.masterTrackName);
                }
                if (master.masterNodeName != lastNameComponent(master.masterNodeNameFull)) {
                    emitKey(em, "nodeName");
                    em << YAML::Value;
                    emitString(em, master.masterNodeName);
                }
            }
            if (dimension.linkGroup) {
                const KnobLinkGroupSerialization& group = *dimension.linkGroup;
                emitKey(em, "linkGroup");
                em << YAML::Value;
                emitString(em, group.id);
                if (!group.linked) {
                    emitKey(em, "unlinked");
                    em << YAML::Value;
                    emitBool(em, true);
                }
                if (group.nodeLink) {
                    emitKey(em, "nodeLink");
                    em << YAML::Value;
                    emitBool(em, true);
                }
            }
            if (dimension.value && !dimension.curve && !hasExpression(dimension) && !dimension.master) {
                emitKey(em, "value");
                em << YAML::Value;
                emitValue(em, *dimension.value);
            }
            if (!dimension.enabled) {
                emitKey(em, "enabled");
                em << YAML::Value;
                emitBool(em, false);
            }
            em << YAML::EndMap;
        }

        void
        emitDimensions(YAML::Emitter& em,
                       const KnobSerialization& knob,
                       const KnobForm& form,
                       const KnobEmitOptions& options)
        {
            if (knob.dimensions.size() == 1) {
                emitDimension(em, knob, 0, form, options);

                return;
            }
            em << YAML::Flow << YAML::BeginSeq;
            for (std::size_t i = 0; i < knob.dimensions.size(); ++i) {
                emitDimension(em, knob, i, form, options);
            }
            em << YAML::EndSeq;
        }

        void
        emitUserDefinition(YAML::Emitter& em,
                           const KnobSerialization& knob)
        {
            em << YAML::BeginMap;
            emitKey(em, "type");
            em << YAML::Value;
            emitString(em, knob.typeName);
            emitKey(em, "dimension");
            em << YAML::Value;
            emitInt(em, knob.dimension);
            emitKey(em, "label");
            em << YAML::Value;
            emitString(em, knob.label);
            if (!knob.tooltip.empty()) {
                emitKey(em, "tooltip");
                em << YAML::Value;
                emitString(em, knob.tooltip);
            }
            emitKey(em, "newLine");
            em << YAML::Value;
            emitBool(em, knob.triggerNewLine);
            emitKey(em, "evaluates");
            em << YAML::Value;
            emitBool(em, knob.evaluatesOnChange);
            emitKey(em, "animates");
            em << YAML::Value;
            emitBool(em, knob.animationEnabled);
            emitKey(em, "persistent");
            em << YAML::Value;
            emitBool(em, knob.isPersistent);

            bool hasDefault = false;
            for (const KnobDimensionSerialization& dimension : knob.dimensions) {
                hasDefault = hasDefault || dimension.defaultValue.has_value();
            }
            if (hasDefault) {
                emitKey(em, "default");
                em << YAML::Value;
                if (knob.dimensions.size() != 1) {
                    em << YAML::Flow << YAML::BeginSeq;
                }
                for (const KnobDimensionSerialization& dimension : knob.dimensions) {
                    if (dimension.defaultValue) {
                        emitValue(em, *dimension.defaultValue);
                    } else {
                        em << YAML::Null;
                    }
                }
                if (knob.dimensions.size() != 1) {
                    em << YAML::EndSeq;
                }
            }

            if (const KnobChoiceExtraSerialization* choice = std::get_if<KnobChoiceExtraSerialization>(&knob.extraData)) {
                emitKey(em, "entries");
                em << YAML::Value;
                emitStringList(em, choice->entries);
                emitKey(em, "helps");
                em << YAML::Value;
                emitStringList(em, choice->helpStrings);
            } else if (const KnobValueRangeSerialization* range = std::get_if<KnobValueRangeSerialization>(&knob.extraData)) {
                emitKey(em, "range");
                em << YAML::Value << YAML::Flow << YAML::BeginSeq;
                emitDouble(em, range->minimum);
                emitDouble(em, range->maximum);
                emitDouble(em, range->displayMinimum);
                emitDouble(em, range->displayMaximum);
                em << YAML::EndSeq;
            } else if (const KnobFileExtraSerialization* file = std::get_if<KnobFileExtraSerialization>(&knob.extraData)) {
                emitKey(em, "sequences");
                em << YAML::Value;
                emitBool(em, file->useSequences);
            } else if (const KnobPathExtraSerialization* path = std::get_if<KnobPathExtraSerialization>(&knob.extraData)) {
                emitKey(em, "multiPath");
                em << YAML::Value;
                emitBool(em, path->multiPath);
            } else if (const KnobLayerSelectExtraSerialization* layerSelect = std::get_if<KnobLayerSelectExtraSerialization>(&knob.extraData)) {
                emitKey(em, "channelButtons");
                em << YAML::Value;
                emitBool(em, layerSelect->withChannelButtons);
            } else if (const KnobTextExtraSerialization* text = std::get_if<KnobTextExtraSerialization>(&knob.extraData)) {
                emitKey(em, "text");
                em << YAML::Value << YAML::Flow << YAML::BeginMap;
                emitKey(em, "label");
                em << YAML::Value;
                emitBool(em, text->isLabel);
                emitKey(em, "multiLine");
                em << YAML::Value;
                emitBool(em, text->isMultiLine);
                emitKey(em, "richText");
                em << YAML::Value;
                emitBool(em, text->usesRichText);
                em << YAML::EndMap;
            }
            if (knob.useHostOverlayHandle) {
                emitKey(em, "overlayHandle");
                em << YAML::Value;
                emitBool(em, *knob.useHostOverlayHandle);
            }
            em << YAML::EndMap;
        }

        void
        readUserDefinition(const YAML::Node& node,
                           KnobSerialization* knob,
                           const std::string& context,
                           YamlWarnings* warnings)
        {
            MapReader reader(node, context, warnings);
            knob->isUserKnob = true;
            knob->typeName = readString(reader.get("type"), context + ".type");
            knob->dimension = readInt(reader.get("dimension"), context + ".dimension");
            if (knob->dimension < 1) {
                fail(reader.get("dimension"), context, "a knob has at least one dimension");
            }
            knob->label = readString(reader.get("label"), context + ".label");
            if (YAML::Node tooltip = reader.get("tooltip")) {
                knob->tooltip = readString(tooltip, context + ".tooltip");
            }
            knob->triggerNewLine = readBool(reader.get("newLine"), context + ".newLine");
            knob->evaluatesOnChange = readBool(reader.get("evaluates"), context + ".evaluates");
            knob->animationEnabled = readBool(reader.get("animates"), context + ".animates");
            knob->isPersistent = readBool(reader.get("persistent"), context + ".persistent");

            if (YAML::Node defaults = reader.get("default")) {
                std::vector<YAML::Node> values;
                if (defaults.IsSequence()) {
                    for (std::size_t i = 0; i < defaults.size(); ++i) {
                        values.push_back(defaults[i]);
                    }
                } else {
                    values.push_back(defaults);
                }
                if (knob->dimensions.size() < values.size()) {
                    knob->dimensions.resize(values.size());
                }
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (!values[i].IsNull()) {
                        knob->dimensions[i].defaultValue = readValue(values[i], context + ".default");
                    }
                }
            }

            if (reader.has("entries") || reader.has("helps")) {
                KnobChoiceExtraSerialization choice;
                if (YAML::Node entries = reader.get("entries")) {
                    choice.entries = readStringVector(entries, context + ".entries");
                }
                if (YAML::Node helps = reader.get("helps")) {
                    choice.helpStrings = readStringVector(helps, context + ".helps");
                }
                knob->extraData = choice;
            }
            if (YAML::Node rangeNode = reader.get("range")) {
                std::vector<double> values = readDoubleVector(rangeNode, context + ".range", 4);
                KnobValueRangeSerialization range;
                range.minimum = values[0];
                range.maximum = values[1];
                range.displayMinimum = values[2];
                range.displayMaximum = values[3];
                knob->extraData = range;
            }
            if (YAML::Node sequences = reader.get("sequences")) {
                KnobFileExtraSerialization file;
                file.useSequences = readBool(sequences, context + ".sequences");
                knob->extraData = file;
            }
            if (YAML::Node multiPath = reader.get("multiPath")) {
                KnobPathExtraSerialization path;
                path.multiPath = readBool(multiPath, context + ".multiPath");
                knob->extraData = path;
            }
            if (YAML::Node channelButtons = reader.get("channelButtons")) {
                KnobLayerSelectExtraSerialization layerSelect;
                layerSelect.withChannelButtons = readBool(channelButtons, context + ".channelButtons");
                knob->extraData = layerSelect;
            }
            if (YAML::Node textNode = reader.get("text")) {
                MapReader textReader(textNode, context + ".text", warnings);
                KnobTextExtraSerialization text;
                text.isLabel = readBool(textReader.get("label"), context + ".text.label");
                text.isMultiLine = readBool(textReader.get("multiLine"), context + ".text.multiLine");
                text.usesRichText = readBool(textReader.get("richText"), context + ".text.richText");
                textReader.finish();
                knob->extraData = text;
            }
            if (YAML::Node overlayHandle = reader.get("overlayHandle")) {
                knob->useHostOverlayHandle = readBool(overlayHandle, context + ".overlayHandle");
            }
            reader.finish();
        }

        void
        readDimension(const YAML::Node& node,
                      KnobSerialization* knob,
                      std::size_t index,
                      const std::string& context,
                      YamlWarnings* warnings,
                      std::vector<TabledCurveRef>* tabledCurves)
        {
            KnobDimensionSerialization& dimension = knob->dimensions[index];
            if (node.IsNull()) {
                return;
            }
            if (node.IsScalar()) {
                dimension.value = readValue(node, context);

                return;
            }
            if (!node.IsMap()) {
                fail(node, context, "expected a value, ~ or a map");
            }
            MapReader reader(node, context, warnings);
            if (YAML::Node curve = reader.get("curve")) {
                if (curve.IsScalar() && (curve.Scalar() == kCurveInTable)) {
                    dimension.curve.emplace();
                    if (tabledCurves) {
                        tabledCurves->push_back(TabledCurveRef { knob->name, index });
                    } else {
                        warn(warnings, context + ": the curve is said to be in a table, but there is none here");
                    }
                } else if (isStringAnimationForm(curve)) {
                    std::map<int, std::string> strings = readStringAnimation(curve, context + ".curve");
                    dimension.curve = impliedStringCurve(strings);
                    knob->stringAnimation = std::move(strings);
                } else {
                    dimension.curve = readCurve(curve, context + ".curve");
                }
            }
            if (YAML::Node expression = reader.get("expr")) {
                dimension.expression = readString(expression, context + ".expr");
            }
            if (YAML::Node hasRet = reader.get("hasRet")) {
                dimension.expressionHasReturnVariable = readBool(hasRet, context + ".hasRet");
            }
            if (YAML::Node link = reader.get("link")) {
                dimension.master = parseLink(link, context + ".link");
                if (YAML::Node track = reader.get("track")) {
                    dimension.master->masterTrackName = readString(track, context + ".track");
                }
                if (YAML::Node nodeName = reader.get("nodeName")) {
                    dimension.master->masterNodeName = readString(nodeName, context + ".nodeName");
                }
            }
            if (YAML::Node linkGroup = reader.get("linkGroup")) {
                dimension.linkGroup.emplace();
                dimension.linkGroup->id = readString(linkGroup, context + ".linkGroup");
                if (YAML::Node unlinked = reader.get("unlinked")) {
                    dimension.linkGroup->linked = !readBool(unlinked, context + ".unlinked");
                }
                if (YAML::Node nodeLink = reader.get("nodeLink")) {
                    dimension.linkGroup->nodeLink = readBool(nodeLink, context + ".nodeLink");
                }
            }
            if (YAML::Node value = reader.get("value")) {
                if (!value.IsNull()) {
                    dimension.value = readValue(value, context + ".value");
                }
            }
            if (YAML::Node enabled = reader.get("enabled")) {
                dimension.enabled = readBool(enabled, context + ".enabled");
            }
            reader.finish();
        }

        bool
        coerceValue(KnobValueSerialization* value,
                    KnobStorageTypeEnum storage)
        {
            switch (storage) {
            case eKnobStorageTypeBool:
                if (const int* i = std::get_if<int>(value)) {
                    if ((*i == 0) || (*i == 1)) {
                        *value = KnobValueSerialization(std::in_place_type<bool>, *i == 1);
                    }
                }

                return std::holds_alternative<bool>(*value);
            case eKnobStorageTypeInt:
                if (const double* d = std::get_if<double>(value)) {
                    if ((std::trunc(*d) == *d) && (*d >= std::numeric_limits<int>::min()) && (*d <= std::numeric_limits<int>::max())) {
                        *value = KnobValueSerialization(std::in_place_type<int>, (int)*d);
                    }
                } else if (const bool* b = std::get_if<bool>(value)) {
                    *value = KnobValueSerialization(std::in_place_type<int>, *b ? 1 : 0);
                }

                return std::holds_alternative<int>(*value);
            case eKnobStorageTypeDouble:
                if (const int* i = std::get_if<int>(value)) {
                    *value = KnobValueSerialization(std::in_place_type<double>, (double)*i);
                }

                return std::holds_alternative<double>(*value);
            case eKnobStorageTypeString:
                if (const int* i = std::get_if<int>(value)) {
                    *value = KnobValueSerialization(std::in_place_type<std::string>, std::to_string(*i));
                } else if (const double* d = std::get_if<double>(value)) {
                    *value = KnobValueSerialization(std::in_place_type<std::string>, formatYamlDouble(*d));
                } else if (const bool* b = std::get_if<bool>(value)) {
                    *value = KnobValueSerialization(std::in_place_type<std::string>, *b ? "true" : "false");
                }

                return std::holds_alternative<std::string>(*value);
            case eKnobStorageTypeNone:
                break;
            }

            return true;
        }

    } // namespace

    void
    emitKnobValue(YAML::Emitter& em,
                  const KnobSerialization& knob,
                  const KnobEmitOptions& options)
    {
        KnobForm form;
        form.choiceShortForm = isChoiceShortForm(knob);
        form.stringCurveImplied = isStringCurveImplied(knob);

        const bool writesOption = knob.choiceOptionID && !knob.choiceOptionID->empty() && !form.choiceShortForm;
        const bool writesStrings = knob.stringAnimation && !knob.stringAnimation->empty() && !form.stringCurveImplied;
        const bool writesParametric = knob.parametricCurves && !knob.parametricCurves->empty();
        const bool hasKnobLevelData = knob.secret || knob.masterIsAlias || writesOption || writesStrings || writesParametric || knob.isUserKnob;

        if (!hasKnobLevelData) {
            emitDimensions(em, knob, form, options);

            return;
        }

        bool allDimensionsEmpty = true;
        for (const KnobDimensionSerialization& dimension : knob.dimensions) {
            allDimensionsEmpty = allDimensionsEmpty && isDimensionEmpty(dimension);
        }

        if (knob.isUserKnob) {
            em << YAML::BeginMap;
        } else {
            em << YAML::Flow << YAML::BeginMap;
        }
        if (!allDimensionsEmpty) {
            emitKey(em, "value");
            em << YAML::Value;
            emitDimensions(em, knob, form, options);
        }
        if (knob.secret) {
            emitKey(em, "secret");
            em << YAML::Value;
            emitBool(em, true);
        }
        if (knob.masterIsAlias) {
            emitKey(em, "alias");
            em << YAML::Value;
            emitBool(em, true);
        }
        if (writesOption) {
            emitKey(em, "option");
            em << YAML::Value;
            emitString(em, *knob.choiceOptionID);
        }
        if (writesStrings) {
            emitKey(em, "strings");
            em << YAML::Value;
            emitStringAnimation(em, *knob.stringAnimation);
        }
        if (writesParametric) {
            emitKey(em, "curves");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const CurveSerialization& curve : *knob.parametricCurves) {
                emitCurve(em, curve);
            }
            em << YAML::EndSeq;
        }
        if (knob.isUserKnob) {
            emitKey(em, "user");
            em << YAML::Value;
            emitUserDefinition(em, knob);
        }
        em << YAML::EndMap;
    }

    void
    emitKnobMap(YAML::Emitter& em,
                const std::list<KnobSerialization>& knobs,
                const KnobEmitOptions& options)
    {
        em << YAML::BeginMap;
        for (const KnobSerialization& knob : knobs) {
            emitKey(em, knob.name);
            em << YAML::Value;
            emitKnobValue(em, knob, options);
        }
        em << YAML::EndMap;
    }

    KnobSerialization
    readKnob(const std::string& name,
             const YAML::Node& node,
             const std::string& context,
             YamlWarnings* warnings,
             std::vector<TabledCurveRef>* tabledCurves)
    {
        KnobSerialization knob;
        knob.name = name;
        knob.typeName.clear();
        knob.dimension = 0;

        YAML::Node dimensionsNode = node;
        if (isKnobLevelMap(node)) {
            MapReader reader(node, context, warnings);
            dimensionsNode = reader.get("value");
            if (YAML::Node secret = reader.get("secret")) {
                knob.secret = readBool(secret, context + ".secret");
            }
            if (YAML::Node alias = reader.get("alias")) {
                knob.masterIsAlias = readBool(alias, context + ".alias");
            }
            if (YAML::Node option = reader.get("option")) {
                knob.choiceOptionID = readString(option, context + ".option");
            }
            if (YAML::Node strings = reader.get("strings")) {
                knob.stringAnimation = readStringAnimation(strings, context + ".strings");
            }
            if (YAML::Node curves = reader.get("curves")) {
                if (!curves.IsSequence()) {
                    fail(curves, context + ".curves", "expected a list of curves");
                }
                knob.parametricCurves.emplace();
                for (std::size_t i = 0; i < curves.size(); ++i) {
                    knob.parametricCurves->push_back(readCurve(curves[i], context + ".curves[" + std::to_string(i) + "]"));
                }
            }
            if (YAML::Node user = reader.get("user")) {
                readUserDefinition(user, &knob, context + ".user", warnings);
            }
            reader.finish();
        }

        if (dimensionsNode) {
            std::vector<YAML::Node> dimensionNodes;
            if (dimensionsNode.IsSequence()) {
                for (std::size_t i = 0; i < dimensionsNode.size(); ++i) {
                    dimensionNodes.push_back(dimensionsNode[i]);
                }
            } else {
                dimensionNodes.push_back(dimensionsNode);
            }
            if (knob.dimensions.size() < dimensionNodes.size()) {
                knob.dimensions.resize(dimensionNodes.size());
            }
            for (std::size_t i = 0; i < dimensionNodes.size(); ++i) {
                const std::string dimensionContext = (dimensionNodes.size() == 1) ? context : context + "[" + std::to_string(i) + "]";
                readDimension(dimensionNodes[i], &knob, i, dimensionContext, warnings, tabledCurves);
            }
        }

        if (knob.isUserKnob) {
            const KnobChoiceExtraSerialization* choice = choiceExtra(knob);
            completeKnobSerialization(&knob, knob.typeName, knob.dimension, choice ? &choice->entries : nullptr);
        }

        return knob;
    }

    std::list<KnobSerialization>
    readKnobMap(const YAML::Node& node,
                const std::string& context,
                YamlWarnings* warnings,
                std::vector<TabledCurveRef>* tabledCurves)
    {
        std::list<KnobSerialization> knobs;
        MapReader reader(node, context, warnings);
        for (const std::pair<std::string, YAML::Node>& entry : reader.entries()) {
            knobs.push_back(readKnob(entry.first, entry.second, context + "." + entry.first, warnings, tabledCurves));
        }

        return knobs;
    }

    namespace {

        void
        emitGroupDefinition(YAML::Emitter& em,
                            const GroupKnobSerialization& group)
        {
            em << YAML::BeginMap;
            emitKey(em, "group");
            em << YAML::Value << YAML::Flow << YAML::BeginMap;
            emitKey(em, "label");
            em << YAML::Value;
            emitString(em, group.label);
            if (group.secret) {
                emitKey(em, "secret");
                em << YAML::Value;
                emitBool(em, true);
            }
            if (group.isSetAsTab) {
                emitKey(em, "tab");
                em << YAML::Value;
                emitBool(em, true);
            }
            if (group.isOpened) {
                emitKey(em, "opened");
                em << YAML::Value;
                emitBool(em, true);
            }
            if (group.isRFVGroup) {
                emitKey(em, "rfv");
                em << YAML::Value;
                emitBool(em, true);
            }
            em << YAML::EndMap;
            if (!group.children.empty()) {
                emitKey(em, "children");
                em << YAML::Value;
                emitGroupKnobChildren(em, group.children);
            }
            em << YAML::EndMap;
        }

        GroupKnobSerialization
        readGroupDefinition(const std::string& name,
                            const YAML::Node& node,
                            const std::string& context,
                            YamlWarnings* warnings);

        std::list<GroupKnobChildSerialization>
        readGroupKnobChildren(const YAML::Node& node,
                              const std::string& context,
                              YamlWarnings* warnings)
        {
            std::list<GroupKnobChildSerialization> children;
            MapReader reader(node, context, warnings);
            for (const std::pair<std::string, YAML::Node>& entry : reader.entries()) {
                children.push_back(GroupKnobChildSerialization());
                GroupKnobChildSerialization& child = children.back();
                const std::string childContext = context + "." + entry.first;
                if (entry.second.IsMap() && entry.second["group"]) {
                    child.isGroup = true;
                    child.group = readGroupDefinition(entry.first, entry.second, childContext, warnings);
                } else {
                    child.knob = readKnob(entry.first, entry.second, childContext, warnings);
                }
            }

            return children;
        }

        GroupKnobSerialization
        readGroupDefinition(const std::string& name,
                            const YAML::Node& node,
                            const std::string& context,
                            YamlWarnings* warnings)
        {
            GroupKnobSerialization group;
            group.name = name;
            MapReader reader(node, context, warnings);
            MapReader definition(reader.get("group"), context + ".group", warnings);
            group.label = readString(definition.get("label"), context + ".group.label");
            if (YAML::Node secret = definition.get("secret")) {
                group.secret = readBool(secret, context + ".group.secret");
            }
            if (YAML::Node tab = definition.get("tab")) {
                group.isSetAsTab = readBool(tab, context + ".group.tab");
            }
            if (YAML::Node opened = definition.get("opened")) {
                group.isOpened = readBool(opened, context + ".group.opened");
            }
            if (YAML::Node rfv = definition.get("rfv")) {
                group.isRFVGroup = readBool(rfv, context + ".group.rfv");
            }
            definition.finish();
            if (YAML::Node children = reader.get("children")) {
                group.children = readGroupKnobChildren(children, context + ".children", warnings);
            }
            reader.finish();

            return group;
        }

    } // namespace

    void
    emitGroupKnobChildren(YAML::Emitter& em,
                          const std::list<GroupKnobChildSerialization>& children)
    {
        em << YAML::BeginMap;
        for (const GroupKnobChildSerialization& child : children) {
            if (child.isGroup) {
                emitKey(em, child.group.name);
                em << YAML::Value;
                emitGroupDefinition(em, child.group);
            } else {
                emitKey(em, child.knob.name);
                em << YAML::Value;
                emitKnobValue(em, child.knob);
            }
        }
        em << YAML::EndMap;
    }

    void
    emitUserPages(YAML::Emitter& em,
                  const std::list<GroupKnobSerialization>& pages)
    {
        em << YAML::BeginMap;
        for (const GroupKnobSerialization& page : pages) {
            emitKey(em, page.name);
            em << YAML::Value;
            emitGroupDefinition(em, page);
        }
        em << YAML::EndMap;
    }

    std::list<GroupKnobSerialization>
    readUserPages(const YAML::Node& node,
                  const std::string& context,
                  YamlWarnings* warnings)
    {
        std::list<GroupKnobSerialization> pages;
        MapReader reader(node, context, warnings);
        for (const std::pair<std::string, YAML::Node>& entry : reader.entries()) {
            pages.push_back(readGroupDefinition(entry.first, entry.second, context + "." + entry.first, warnings));
        }

        return pages;
    }

} // namespace YamlDetail

std::string
encodeKnobsYaml(const std::list<KnobSerialization>& knobs)
{
    YAML::Emitter em;
    if (knobs.empty()) {
        em << YAML::Flow << YAML::BeginMap << YAML::EndMap;
    } else {
        YamlDetail::emitKnobMap(em, knobs);
    }

    return YamlDetail::documentText(em);
}

std::list<KnobSerialization>
decodeKnobsYaml(std::string_view text,
                YamlWarnings* warnings)
{
    YAML::Node node = YamlDetail::loadDocument(text, "the knobs");

    return YamlDetail::readKnobMap(node, "knobs", warnings);
}

void
completeKnobSerialization(KnobSerialization* knob,
                          const std::string& typeName,
                          int dimension,
                          const std::vector<std::string>* choiceEntries)
{
    if (knob->typeName.empty()) {
        knob->typeName = typeName;
    }
    if (knob->dimension <= 0) {
        knob->dimension = dimension;
    }
    if (knob->dimensions.empty() && (knob->dimension > 0)) {
        knob->dimensions.resize(knob->dimension);
    }
    const KnobTypeTraits traits = getKnobTypeTraits(knob->typeName);
    if (!traits.isKnown) {
        return;
    }

    if (traits.isChoice) {
        if (!choiceEntries) {
            if (const KnobChoiceExtraSerialization* choice = std::get_if<KnobChoiceExtraSerialization>(&knob->extraData)) {
                choiceEntries = &choice->entries;
            }
        }
        for (KnobDimensionSerialization& dimensionData : knob->dimensions) {
            const std::string* optionID = dimensionData.value ? std::get_if<std::string>(&(*dimensionData.value)) : nullptr;
            if (!optionID) {
                continue;
            }
            int index = 0;
            if (choiceEntries) {
                for (std::size_t i = 0; i < choiceEntries->size(); ++i) {
                    if ((*choiceEntries)[i] == *optionID) {
                        index = (int)i;
                        break;
                    }
                }
            }
            if (!knob->choiceOptionID) {
                knob->choiceOptionID = *optionID;
            }
            dimensionData.value = KnobValueSerialization(std::in_place_type<int>, index);
        }
        if (!knob->choiceOptionID) {
            knob->choiceOptionID = std::string();
        }
    }

    if (traits.hasValue) {
        for (KnobDimensionSerialization& dimensionData : knob->dimensions) {
            if (dimensionData.value) {
                YamlDetail::coerceValue(&(*dimensionData.value), traits.storageType);
            }
            if (dimensionData.defaultValue) {
                YamlDetail::coerceValue(&(*dimensionData.defaultValue), traits.storageType);
            }
        }
    }
    if (traits.isParametric && !knob->parametricCurves) {
        knob->parametricCurves.emplace();
    }
    if (traits.hasStringAnimation && !knob->stringAnimation) {
        knob->stringAnimation.emplace();
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
