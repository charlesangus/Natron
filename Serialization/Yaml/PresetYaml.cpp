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

#include "Serialization/Yaml/PresetYaml.h"

#include <set>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

std::string
encodePresetYaml(const std::list<NodeSerializationPtr>& nodes)
{
    using namespace YamlDetail;
    InlineBlobSink sink;
    std::set<std::string> usedPaths;
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitDocumentFormat(em, kPresetYamlFormat, kPresetYamlFormatVersion);
    emitKey(em, "nodes");
    em << YAML::Value << YAML::BeginSeq;
    for (const NodeSerializationPtr& node : nodes) {
        if (node) {
            emitNodeMap(em, std::list<NodeSerializationPtr>(1, node), std::string(), sink, &usedPaths);
        }
    }
    em << YAML::EndSeq;
    em << YAML::EndMap;

    return documentText(em);
}

std::list<NodeSerializationPtr>
decodePresetYaml(std::string_view text,
                 YamlWarnings* warnings)
{
    using namespace YamlDetail;
    YAML::Node document = loadDocument(text, "the preset");
    if (!document.IsMap()) {
        throw YamlError("this is not a Natron preset: it is not a YAML map");
    }
    MapReader reader(document, "preset", warnings);
    checkDocumentFormat(reader, kPresetYamlFormat, kPresetYamlFormatVersion, "a Natron preset");
    YAML::Node nodes = reader.get("nodes");
    if (!nodes || !nodes.IsSequence()) {
        fail(nodes ? nodes : document, "preset.nodes", "expected a list of nodes");
    }
    NoBlobSource source;
    std::list<NodeSerializationPtr> result;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::string context = "preset.nodes[" + std::to_string(i) + "]";
        std::list<NodeSerializationPtr> item = readNodeMap(nodes[i], source, context, warnings);
        if (item.size() != 1) {
            fail(nodes[i], context, "expected one node keyed by its script name");
        }
        result.push_back(item.front());
    }
    reader.finish();
    if (result.empty()) {
        throw YamlError("the preset holds no node");
    }

    return result;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
