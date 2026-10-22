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

#ifndef Serialization_Yaml_ManifestYaml_h
#define Serialization_Yaml_ManifestYaml_h

#include "Global/Macros.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

inline constexpr std::string_view kManifestYamlFormat = "natron-bundle";

// Raised when a change to the bundle layout would make an older Natron misread it.
inline constexpr int kBundleFormatVersion = 1;

/**
 * @brief One archive member as the manifest lists it.
 **/
struct BundleManifestMember {
    std::string name;
    std::uint32_t size = 0;
    std::uint32_t crc32 = 0;

    bool operator==(const BundleManifestMember&) const = default;
};

/**
 * @brief manifest.yaml: what changes on every save, kept out of project.yaml so that re-saving an
 * unchanged project leaves the graph document byte-identical.
 *
 * appVersion and appCommit name the build that saved the bundle, saveDate is the save time as
 * ISO 8601 UTC, author the user and host that saved it, and creationDate the project's creation
 * time in milliseconds since the epoch. members lists every member after the manifest itself,
 * in archive order.
 **/
struct BundleManifest {
    int formatVersion = kBundleFormatVersion;
    std::string appVersion;
    std::string appCommit;
    std::string saveDate;
    std::string author;
    long long creationDate = 0;
    std::vector<BundleManifestMember> members;

    bool operator==(const BundleManifest&) const = default;
};

std::string encodeManifestYaml(const BundleManifest& manifest);

/**
 * @brief Throws YamlError for a document that is not a bundle manifest or one written by a newer
 * Natron (a higher formatVersion); unknown keys are reported in warnings.
 **/
BundleManifest decodeManifestYaml(std::string_view text, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_ManifestYaml_h
