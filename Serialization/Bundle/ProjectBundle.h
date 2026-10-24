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
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Serialization/Bundle/DirectorySource.h"
#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ManifestYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A project bundle (.ntpz): a ZipStore archive whose members all sit under one folder
 * named after the project file, in this order: `<stem>/mimetype`, `<stem>/manifest.yaml`,
 * `<stem>/project.yaml`, then the blob members of project.yaml sorted by path. Writing the same
 * project twice gives the same bytes except for manifest.yaml.
 *
 * A reader is lenient so that a bundle unzipped, edited and re-zipped by any tool still opens:
 * - any zip archive is accepted, whatever its member order or compression; directory entries
 *   are ignored;
 * - the bundle's root is wherever its one `project.yaml` is, at the top of the archive or one
 *   folder down, whatever that folder is called; none is "not a project", several are refused
 *   as ambiguous;
 * - `mimetype` is optional, at the root or at the top of the archive; one that names another
 *   type is refused;
 * - a member whose CRC-32 does not match is refused;
 * - manifest.yaml is optional and only informs: a manifest that is missing or unreadable gives
 *   no author or dates, and a member list that does not match the members gives one warning;
 * - members outside the root folder, other than a top-level `mimetype`, are ignored with a
 *   warning; members under the root that a project does not name are ignored silently.
 * The same members read from a folder (an unzipped bundle) are a bundle too.
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
    eDirectory,
    eProjectYaml,
    eUnknown,
};

/**
 * @brief What a project path is: a folder (possibly an unzipped bundle); a bundle, which is any
 * zip archive (`PK\3\4`, or `PK\5\6` for an empty one, at offset 0), a project in the old
 * boost XML format (`<?xml` or `<!DOCTYPE boost_serialization` within the first 64 bytes), a
 * file named `project.yaml` (the main member of an unzipped bundle), or anything else, which
 * includes a path that cannot be read. The span overload only looks at bytes, so it never
 * answers eDirectory or eProjectYaml.
 *
 * Whether a zip archive or folder really holds a project is for ProjectBundleReader to say.
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
     * @brief The folder every member is written under, normally the project file name without
     * `.ntpz`. An empty name, one with a '/' or one that is not a valid member name falls back
     * to `project`. Manifest member names are relative to this folder.
     **/
    void setRootFolder(std::string_view folder);

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

    std::string _rootFolder;
    std::string _projectYaml;
    MemoryBlobStore _blobs;
    BundleManifest _manifest;
};

class ProjectBundleReader
    : public BlobSource {
public:
    /**
     * @brief Opens the bundle at path and checks it as described above, throwing
     * ProjectBundleError when it is not a readable project. path is a zip archive, a folder
     * holding project.yaml, or a file named project.yaml, whose folder is read.
     *
     * An archive stays mapped while the reader lives, so the file must not be rewritten in
     * place meanwhile; saving writes a new file and renames it over the old one. A folder is
     * read whole on construction.
     **/
    explicit ProjectBundleReader(const std::string& path);

    /**
     * @brief The same for a zip archive in memory; the bytes must outlive the reader.
     **/
    explicit ProjectBundleReader(std::span<const std::byte> bytes);

    /**
     * @brief The manifest, or a default one (no author, dates or members) when the bundle has
     * none or it could not be read.
     **/
    const BundleManifest& manifest() const
    {
        return _manifest;
    }

    bool hasManifest() const
    {
        return _hasManifest;
    }

    /**
     * @brief The folder of the archive the members are read from, with a trailing '/', or empty
     * when they sit at the top of the archive or the bundle is a folder.
     **/
    const std::string& root() const
    {
        return _root;
    }

    /**
     * @brief What opening the bundle found wrong without refusing it: an unreadable or stale
     * manifest, members ignored outside the root.
     **/
    const YamlWarnings& warnings() const
    {
        return _warnings;
    }

    std::string_view projectYaml() const;

    /**
     * @brief Decodes project.yaml. warnings receives warnings() first, then what decoding
     * reports: a blob member project.yaml names that the bundle lacks leaves that item's data
     * empty. A project.yaml from a newer Natron throws YamlNewerFormatError; anything else that
     * does not decode throws YamlError.
     **/
    ProjectSerialization readProject(YamlWarnings* warnings = nullptr) const;

    /**
     * @brief A member by its name relative to the root.
     **/
    std::optional<std::span<const std::byte>> findMember(std::string_view path) const override;

private:
    void validate();
    void validateArchive();
    void readManifest();
    std::optional<std::span<const std::byte>> rawMember(std::string_view name) const;

    std::optional<ZipReader> _archive;
    std::unique_ptr<DirectorySource> _directory;
    std::string _root;
    BundleManifest _manifest;
    bool _hasManifest = false;
    YamlWarnings _warnings;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Bundle_ProjectBundle_h
