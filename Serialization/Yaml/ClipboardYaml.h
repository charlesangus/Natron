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

#ifndef Serialization_Yaml_ClipboardYaml_h
#define Serialization_Yaml_ClipboardYaml_h

#include "Global/Macros.h"

#include <list>
#include <string>
#include <string_view>

#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

inline constexpr std::string_view kClipboardYamlFormat = "natron-clipboard";
inline constexpr int kClipboardYamlFormatVersion = 1;

// The MIME type of copied nodes on the system clipboard, next to text/plain with the same text.
inline constexpr std::string_view kClipboardYamlMimeType = "application/vnd.natron.nodes+yaml";

/**
 * @brief Copied nodes. Each node, and each child of a group, carries its own layout.
 **/
struct ClipboardSerialization {
    std::list<NodeSerializationPtr> nodes;
};

/**
 * @brief The clipboard document: `format: natron-clipboard`, `formatVersion` and `nodes` in the
 * project.yaml node schema (so a node's inputs name its siblings by script name, and position,
 * size and colours sit on each node). Heavy roto and tracker tables are inlined as base64.
 **/
std::string encodeClipboardYaml(const ClipboardSerialization& clipboard);

/**
 * @brief Reads the clipboard document. Nodes come back sorted by script name. Throws YamlError for text that is not a Natron clipboard document.
 **/
ClipboardSerialization decodeClipboardYaml(std::string_view text, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_ClipboardYaml_h
