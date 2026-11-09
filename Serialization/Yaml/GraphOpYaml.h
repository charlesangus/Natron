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

#ifndef Serialization_Yaml_GraphOpYaml_h
#define Serialization_Yaml_GraphOpYaml_h

#include "Global/Macros.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief One operation as a YAML map whose `op` key names it (see graphOpName()), the others being
 * its fields:
 * - createNode: `parent` (absent for the top level), `node`, a one-entry node map as a project
 *   writes it, IDs included;
 * - deleteNode: `node`;
 * - setKnob: `node` or `project: true`, `knob`, and either `value` (a knob value as
 *   encodeKnobsYaml writes it) or `default: true`;
 * - setInput: `node`, `input`, `source` (`~` to disconnect);
 * - rename: `node`, `name`, `label` when it changes;
 * - setNodeUi: `node` and any of `pos`, `size`, `color`, `overlay`, a colour being `~` to clear it;
 * - setUserKnobs: `node`, `pages`, `userPages`;
 * - setRotoData / setTrackerData: `node`, `roto` / `tracker` (`~` to remove everything);
 * - setProjectData: `formats` and/or `layers`.
 * Node references are IDs. Blobs are inlined, so the text stands alone.
 **/
std::string encodeGraphOpYaml(const GraphOp& op);

/**
 * @brief A sequence of operations, in order.
 **/
std::string encodeGraphOpsYaml(const std::vector<GraphOp>& ops);

/**
 * @brief One entry of a decoded sequence: the operation, or why it could not be read.
 **/
struct DecodedGraphOp {
    std::optional<GraphOp> op;
    std::string error;
};

/**
 * @brief Reads one operation written by encodeGraphOpYaml. Throws YamlError when it is malformed or
 * names an unknown operation; unknown keys are skipped and listed in warnings.
 **/
GraphOp decodeGraphOpYaml(std::string_view text, YamlWarnings* warnings = nullptr);

/**
 * @brief Reads a sequence written by encodeGraphOpsYaml. An entry that is malformed or names an
 * unknown operation becomes an error entry and the others are still read, so a newer writer's
 * operations do not hide the rest. Throws YamlError only when the text is not a sequence.
 **/
std::vector<DecodedGraphOp> decodeGraphOpsYaml(std::string_view text, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_GraphOpYaml_h
