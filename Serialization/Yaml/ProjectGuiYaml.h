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

#ifndef Serialization_Yaml_ProjectGuiYaml_h
#define Serialization_Yaml_ProjectGuiYaml_h

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

inline constexpr std::string_view kGuiYamlFormat = "natron-gui";
inline constexpr std::string_view kLayoutYamlFormat = "natron-layout";
inline constexpr int kGuiYamlFormatVersion = 1;

/**
 * @brief The gui.yaml document: the node graph state of every node keyed by its fully
 * qualified name (a group's nodes follow it), the viewers keyed by script name, the opened
 * histograms, settings panels and Python panels, the script editor input and the window layout.
 * Not written: the class versions and the backdrops of files that predate backdrop nodes.
 **/
std::string encodeProjectGuiYaml(const ProjectGuiSerialization& gui);
ProjectGuiSerialization decodeProjectGuiYaml(std::string_view text, YamlWarnings* warnings = nullptr);

/**
 * @brief A window layout as a document of its own, for layout files: `format: natron-layout`,
 * `formatVersion` and `windows`, the same windows gui.yaml holds under `layout`.
 **/
std::string encodeGuiLayoutYaml(const GuiLayoutSerialization& layout);
GuiLayoutSerialization decodeGuiLayoutYaml(std::string_view text, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_ProjectGuiYaml_h
