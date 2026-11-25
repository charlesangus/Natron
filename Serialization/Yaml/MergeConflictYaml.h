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

#ifndef Serialization_Yaml_MergeConflictYaml_h
#define Serialization_Yaml_MergeConflictYaml_h

#include "Global/Macros.h"

#include <string>
#include <string_view>
#include <vector>

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

inline constexpr std::string_view kMergeConflictsYamlFormat = "natron-merge-conflicts";
inline constexpr int kMergeConflictsYamlFormatVersion = 1;

/**
 * @brief The bundle member, relative to the bundle root, holding a project's unresolved merge
 * conflicts. A project without any has no such member.
 **/
inline constexpr std::string_view kMergeConflictsMember = "merge/conflicts.yaml";

/**
 * @brief Writes conflicts in their order. A side's knob is written as project.yaml writes it,
 * with its type and dimension count beside it; a side's key as a one-key curve, its other keys
 * as a curve; its connections as node, input and source IDs; its knobs as a list, each with its
 * name, type and dimension count; its roto and tracker data as project.yaml writes them, with
 * every blob inline. A handler option is written with its name, label and the same state keys.
 **/
std::string encodeMergeConflictsYaml(const std::vector<MergeConflict>& conflicts);

/**
 * @brief Reads conflicts written by encodeMergeConflictsYaml. Throws YamlError for a document that
 * is not a conflict list or holds a malformed conflict, YamlNewerFormatError for one from a newer
 * Natron; unknown keys are reported in warnings.
 *
 * A side's knob is completed with its recorded type, so a choice knob carries its option ID and
 * index 0 until it is matched against the live knob's entries.
 **/
std::vector<MergeConflict> decodeMergeConflictsYaml(std::string_view text, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_MergeConflictYaml_h
