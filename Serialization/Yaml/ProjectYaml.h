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

#ifndef Serialization_Yaml_ProjectYaml_h
#define Serialization_Yaml_ProjectYaml_h

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

inline constexpr std::string_view kProjectYamlFormat = "natron-project";

// Raised when a change to project.yaml would make an older Natron misread it; files with a
// higher version are refused rather than half-loaded.
inline constexpr int kProjectYamlFormatVersion = 1;

/**
 * @brief The project.yaml document: format and formatVersion, the Natron build that saved it,
 * the project knobs that differ from their defaults, the additional formats, the layer registry
 * keyed by layer ID (sorted), the timeline, and the nodes.
 *
 * Nodes are a map keyed by script name, sorted, so their order in the file is cosmetic and a
 * duplicate name is an error. Each node holds `plugin`, `version: [major, minor]`, `label` when
 * it differs from the script name, `python` for an unedited PyPlug, `knobs`, `inputs` (input
 * label to the script name of a sibling node), `pages`, `userPages`, `roto`, `tracker`, and for
 * a group its nodes under `children`; empty members are left out. Heavy roto and tracker data go
 * to sink as blob tables.
 *
 * Not written: the creation date (it belongs to the bundle manifest, so re-saving an unchanged
 * project gives the same text), knobsAge, cacheID, the multi-instance parent and the master node.
 **/
std::string encodeProjectYaml(const ProjectSerialization& project, BlobSink& sink);

/**
 * @brief Reads project.yaml. Throws YamlError for a document that is not a Natron project, one
 * saved by a newer Natron (a higher formatVersion), a duplicate node name, or malformed values;
 * unknown keys and missing blobs are reported in warnings. Nodes and layers come back sorted.
 * Knobs that are not user knobs still need completeKnobSerialization() with the live knob.
 **/
ProjectSerialization decodeProjectYaml(std::string_view text, const BlobSource& source, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_ProjectYaml_h
