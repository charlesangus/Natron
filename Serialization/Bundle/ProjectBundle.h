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

#ifndef Serialization_Bundle_ProjectBundle_h
#define Serialization_Bundle_ProjectBundle_h

#include "Global/Macros.h"

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ManifestYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A project bundle (.ntpz): a ZipStore archive whose members are, in this order,
 * `mimetype`, `manifest.yaml`, `project.yaml`, then the blob members of project.yaml sorted by
 * path. Any other member a reader finds is ignored. Writing the same project twice gives the same bytes
 * except for manifest.yaml.
 **/

inline constexpr std::string_view kBundleManifestMember = "manifest.yaml";
inline constexpr std::string_view kBundleProjectMember = "project.yaml";

/**
 * @brief Thrown when a file is not a readable project bundle. The message is meant for the user.
 **/
class ProjectBundleError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ProjectFileKind {
    eBundle,
    eLegacyXml,
    eUnknown,
};

/**
 * @brief What a project file is, from its first bytes: a bundle (a zip archive whose first
 * member is the Natron `mimetype`), a project in the old boost XML format (`<?xml` or
 * `<!DOCTYPE boost_serialization` within the first 64 bytes), or anything else, which includes
 * a file that cannot be read.
 **/
ProjectFileKind detectProjectFileKind(const std::string& path);
ProjectFileKind detectProjectFileKind(std::span<const std::byte> head);

class ProjectBundleWriter {
public:
    ProjectBundleWriter();

    /**
     * @brief Encodes project.yaml, keeping its heavy roto and tracker data as blob members.
     * Throws YamlError when the project cannot be encoded.
     **/
    void setProject(const ProjectSerialization& project);

    /**
     * @brief The manifest to write. Its member list is ignored: write() fills in the real one.
     **/
    void setManifest(const BundleManifest& manifest);

    const std::string& projectYaml() const
    {
        return _projectYaml;
    }

    const MemoryBlobStore& blobs() const
    {
        return _blobs;
    }

    /**
     * @brief The manifest as write() writes it, with its member list.
     **/
    BundleManifest manifest() const;

    /**
     * @brief Writes the whole archive to output. Throws ZipError, or whatever output throws,
     * when it cannot.
     **/
    void write(ZipOutput& output) const;

    /**
     * @brief Writes the whole archive to path, replacing whatever is there. Not atomic: a save
     * that must leave the previous file intact on failure writes to an output that commits
     * only once the archive is complete.
     **/
    void write(const std::string& path) const;

private:
    void writeMembers(ZipWriter& writer) const;

    std::string _projectYaml;
    MemoryBlobStore _blobs;
    BundleManifest _manifest;
};

class ProjectBundleReader
    : public BlobSource {
public:
    /**
     * @brief Maps the bundle at path and checks it: a zip archive, the Natron mimetype first,
     * a readable manifest.yaml from a Natron that is not newer, a project.yaml, and the CRC of
     * every member. Throws ProjectBundleError otherwise.
     *
     * The archive stays mapped while the reader lives, so the file must not be rewritten in
     * place meanwhile; saving writes a new file and renames it over the old one.
     **/
    explicit ProjectBundleReader(const std::string& path);

    /**
     * @brief The same for a bundle in memory; the bytes must outlive the reader.
     **/
    explicit ProjectBundleReader(std::span<const std::byte> bytes);

    const BundleManifest& manifest() const
    {
        return _manifest;
    }

    std::string_view projectYaml() const;

    /**
     * @brief Decodes project.yaml. A blob member it names that the bundle lacks is reported in
     * warnings and leaves that item's data empty; anything else that does not decode throws
     * YamlError.
     **/
    ProjectSerialization readProject(YamlWarnings* warnings = nullptr) const;

    std::optional<std::span<const std::byte>> findMember(std::string_view path) const override;

    const ZipReader& archive() const
    {
        return _archive;
    }

private:
    void validate();

    ZipReader _archive;
    BundleManifest _manifest;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Bundle_ProjectBundle_h
