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

#include "Serialization/Yaml/ClipboardYaml.h"

#include <set>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

std::string
encodeClipboardYaml(const ClipboardSerialization& clipboard)
{
    using namespace YamlDetail;

    InlineBlobSink sink;
    std::set<std::string> usedPaths;
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitDocumentFormat(em, kClipboardYamlFormat, kClipboardYamlFormatVersion);
    if (!clipboard.sourceProject.empty()) {
        emitKey(em, "sourceProject");
        em << YAML::Value;
        emitString(em, clipboard.sourceProject);
    }
    emitKey(em, "nodes");
    em << YAML::Value;
    emitNodeMap(em, clipboard.nodes, std::string(), sink, &usedPaths);

    em << YAML::EndMap;

    return documentText(em);
}

ClipboardSerialization
decodeClipboardYaml(std::string_view text,
                    YamlWarnings* warnings)
{
    using namespace YamlDetail;

    YAML::Node document = loadDocument(text, "the clipboard");
    if (!document.IsMap()) {
        throw YamlError("this is not a Natron clipboard: it is not a YAML map");
    }
    MapReader reader(document, "clipboard", warnings);
    checkDocumentFormat(reader, kClipboardYamlFormat, kClipboardYamlFormatVersion, "a Natron clipboard");

    ClipboardSerialization clipboard;
    NoBlobSource source;
    if (YAML::Node sourceProject = reader.get("sourceProject")) {
        clipboard.sourceProject = readString(sourceProject, "clipboard.sourceProject");
    }
    if (YAML::Node nodes = reader.get("nodes")) {
        clipboard.nodes = readNodeMap(nodes, source, "clipboard.nodes", warnings);
    }
    reader.finish();

    return clipboard;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
