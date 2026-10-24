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

#include "Serialization/Bundle/ProjectBundle.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    namespace fs = std::filesystem;

    constexpr std::size_t kLegacyXmlSniffSize = 64;
    constexpr std::size_t kListedNamesMax = 10;
    constexpr const char* kNoProjectYaml = "Not a " NATRON_APPLICATION_NAME " project (no project.yaml)";
    constexpr std::string_view kMimeTypeName = "mimetype";
    constexpr std::string_view kDefaultRootFolder = "project";

    std::string_view
    trimTrailingSpace(std::string_view text)
    {
        while (!text.empty() && ((text.back() == '\n') || (text.back() == '\r') || (text.back() == ' ') || (text.back() == '\t'))) {
            text.remove_suffix(1);
        }

        return text;
    }

    std::string
    listNames(const std::vector<std::string>& names)
    {
        std::string list;
        for (std::size_t i = 0; (i < names.size()) && (i < kListedNamesMax); ++i) {
            if (i > 0) {
                list += ", ";
            }
            list += names[i];
        }
        if (names.size() > kListedNamesMax) {
            list += " and " + std::to_string(names.size() - kListedNamesMax) + " more";
        }

        return list;
    }

    std::span<const std::byte>
    textBytes(std::string_view text)
    {
        return std::as_bytes(std::span<const char>(text.data(), text.size()));
    }

    std::string_view
    bytesText(std::span<const std::byte> bytes)
    {
        return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    bool
    contains(std::string_view haystack,
             std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    ZipReader
    openArchive(const std::string& path)
    {
        try {
            return ZipReader(path);
        } catch (const ZipError& e) {
            throw ProjectBundleError(std::string("Unrecognized or damaged project file: ") + e.what());
        }
    }

    ZipReader
    openArchive(std::span<const std::byte> bytes)
    {
        try {
            return ZipReader(bytes);
        } catch (const ZipError& e) {
            throw ProjectBundleError(std::string("Unrecognized or damaged project file: ") + e.what());
        }
    }

} // anon namespace

ProjectFileKind
detectProjectFileKind(std::span<const std::byte> head)
{
    if (head.size() >= 4) {
        const std::string_view magic = bytesText(head.first(4));
        if ((magic == "PK\x03\x04") || (magic == "PK\x05\x06")) {
            return ProjectFileKind::eBundle;
        }
    }
    const std::string_view text = bytesText(head.first(std::min(head.size(), kLegacyXmlSniffSize)));
    if (contains(text, "<?xml") || contains(text, "<!DOCTYPE boost_serialization")) {
        return ProjectFileKind::eLegacyXml;
    }

    return ProjectFileKind::eUnknown;
}

ProjectFileKind
detectProjectFileKind(const std::string& path)
{
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return ProjectFileKind::eDirectory;
    }
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return ProjectFileKind::eUnknown;
    }
    std::byte head[kLegacyXmlSniffSize];
    const std::size_t size = std::fread(head, 1, sizeof(head), file);
    std::fclose(file);
    const ProjectFileKind kind = detectProjectFileKind(std::span<const std::byte>(head, size));
    if ((kind == ProjectFileKind::eUnknown) && (fs::path(path).filename() == kBundleProjectMember)) {
        return ProjectFileKind::eProjectYaml;
    }

    return kind;
}

ProjectBundleWriter::ProjectBundleWriter()
    : _rootFolder(kDefaultRootFolder)
{
}

void
ProjectBundleWriter::setProject(const ProjectSerialization& project)
{
    MemoryBlobStore blobs;
    std::string text = encodeProjectYaml(project, blobs);
    _projectYaml = std::move(text);
    _blobs = std::move(blobs);
}

void
ProjectBundleWriter::setRootFolder(std::string_view folder)
{
    const bool usable = !folder.empty() && (folder.find('/') == std::string_view::npos) && zipMemberNameProblem(folder).empty();
    _rootFolder = usable ? std::string(folder) : std::string(kDefaultRootFolder);
}

void
ProjectBundleWriter::setManifest(const BundleManifest& manifest)
{
    _manifest = manifest;
}

BundleManifest
ProjectBundleWriter::manifest() const
{
    BundleManifest manifest = _manifest;
    manifest.members.clear();
    auto list = [&manifest](std::string_view name, std::span<const std::byte> data) {
        BundleManifestMember member;
        member.name = std::string(name);
        member.size = static_cast<std::uint32_t>(data.size());
        member.crc32 = zipCrc32(data);
        manifest.members.push_back(std::move(member));
    };
    list(kBundleProjectMember, textBytes(_projectYaml));
    for (const auto& blob : _blobs.members()) {
        list(blob.first, blob.second);
    }

    return manifest;
}

void
ProjectBundleWriter::writeMembers(ZipWriter& writer) const
{
    const std::string manifestYaml = encodeManifestYaml(manifest());
    writer.addMember(kBundleManifestMember, textBytes(manifestYaml));
    writer.addMember(kBundleProjectMember, textBytes(_projectYaml));
    for (const auto& blob : _blobs.members()) {
        writer.addMember(blob.first, blob.second);
    }
    writer.finish();
}

void
ProjectBundleWriter::write(ZipOutput& output) const
{
    ZipWriter writer(output, kNatronProjectMimeType, _rootFolder);
    writeMembers(writer);
}

void
ProjectBundleWriter::write(const std::string& path) const
{
    ZipWriter writer(path, kNatronProjectMimeType, _rootFolder);
    writeMembers(writer);
}

ProjectBundleReader::ProjectBundleReader(const std::string& path)
{
    const ProjectFileKind kind = detectProjectFileKind(path);
    if ((kind == ProjectFileKind::eDirectory) || (kind == ProjectFileKind::eProjectYaml)) {
        const std::string folder = (kind == ProjectFileKind::eDirectory) ? path : fs::path(path).parent_path().string();
        try {
            _directory = std::make_unique<DirectorySource>(folder.empty() ? std::string(".") : folder);
        } catch (const DirectorySourceError& e) {
            throw ProjectBundleError(std::string("Unrecognized or damaged project folder: ") + e.what());
        }
    } else {
        _archive.emplace(openArchive(path));
    }
    validate();
}

ProjectBundleReader::ProjectBundleReader(std::span<const std::byte> bytes)
    : _archive(openArchive(bytes))
{
    validate();
}

std::optional<std::span<const std::byte>>
ProjectBundleReader::rawMember(std::string_view name) const
{
    if (_directory) {
        return _directory->findMember(name);
    }
    const ZipMember* member = _archive->find(name);
    if (!member) {
        return std::nullopt;
    }
    try {
        return _archive->data(*member);
    } catch (const ZipError& e) {
        throw ProjectBundleError(std::string("Unrecognized or damaged project file: ") + e.what());
    }
}

void
ProjectBundleReader::validate()
{
    // A folder is its own root: a project.yaml below it would be someone else's project.
    if (_directory) {
        _root.clear();
        if (!_directory->findMember(kBundleProjectMember)) {
            throw ProjectBundleError(kNoProjectYaml);
        }
    } else {
        validateArchive();
    }

    std::vector<std::string> mimeTypes = { _root + std::string(kMimeTypeName) };
    if (!_root.empty()) {
        mimeTypes.emplace_back(kMimeTypeName);
    }
    for (const std::string& name : mimeTypes) {
        if (const std::optional<std::span<const std::byte>> bytes = rawMember(name)) {
            if (trimTrailingSpace(bytesText(*bytes)) != kNatronProjectMimeType) {
                throw ProjectBundleError("Not a " NATRON_APPLICATION_NAME " project: its mimetype is not " + std::string(kNatronProjectMimeType));
            }
        }
    }

    readManifest();
}

void
ProjectBundleReader::validateArchive()
{
    std::vector<std::string> roots;
    for (const ZipMember& member : _archive->members()) {
        const std::string& name = member.name;
        if (name == kBundleProjectMember) {
            roots.emplace_back();
        } else if (name.ends_with(kBundleProjectMember) && (name.size() > kBundleProjectMember.size() + 1)) {
            const std::size_t slash = name.size() - kBundleProjectMember.size() - 1;
            if ((name[slash] == '/') && (name.find('/') == slash)) {
                roots.push_back(name.substr(0, slash + 1));
            }
        }
    }
    if (roots.empty()) {
        throw ProjectBundleError(kNoProjectYaml);
    }
    if (roots.size() > 1) {
        std::vector<std::string> found;
        for (const std::string& root : roots) {
            found.push_back(root + std::string(kBundleProjectMember));
        }
        throw ProjectBundleError("Ambiguous project archive: several project.yaml (" + listNames(found) + ")");
    }
    _root = roots.front();

    std::vector<std::string> outside;
    for (const ZipMember& member : _archive->members()) {
        const bool inRoot = member.name.starts_with(_root);
        if (!inRoot && (member.name != kMimeTypeName)) {
            outside.push_back(member.name);
            continue;
        }
        try {
            _archive->verify(member.name);
        } catch (const ZipError& e) {
            throw ProjectBundleError(std::string("Unrecognized or damaged project file: ") + e.what());
        }
    }
    if (!outside.empty()) {
        _warnings.push_back("Ignored " + std::to_string(outside.size()) + " archive member(s) outside the project folder \"" + _root + "\": " + listNames(outside));
    }
}

void
ProjectBundleReader::readManifest()
{
    const std::optional<std::span<const std::byte>> bytes = rawMember(_root + std::string(kBundleManifestMember));
    if (!bytes) {
        return;
    }
    YamlWarnings decodeWarnings;
    try {
        _manifest = decodeManifestYaml(bytesText(*bytes), &decodeWarnings);
    } catch (const YamlError& e) {
        _warnings.push_back(std::string("manifest.yaml was ignored: ") + e.what());
        return;
    }
    _hasManifest = true;
    _warnings.insert(_warnings.end(), decodeWarnings.begin(), decodeWarnings.end());

    std::vector<std::string> mismatches;
    for (const BundleManifestMember& listed : _manifest.members) {
        const std::string name = _root + listed.name;
        std::uint32_t size = 0;
        std::uint32_t crc = 0;
        if (_directory) {
            const std::optional<std::span<const std::byte>> data = _directory->findMember(name);
            if (!data) {
                mismatches.push_back(listed.name + " is missing");
                continue;
            }
            size = static_cast<std::uint32_t>(std::min<std::size_t>(data->size(), UINT32_MAX));
            crc = zipCrc32(*data);
        } else {
            const ZipMember* member = _archive->find(name);
            if (!member) {
                mismatches.push_back(listed.name + " is missing");
                continue;
            }
            size = member->size;
            crc = member->crc32;
        }
        if ((size != listed.size) || (crc != listed.crc32)) {
            mismatches.push_back(listed.name + " differs");
        }
    }
    if (!mismatches.empty()) {
        _warnings.push_back("manifest.yaml does not match the project's members, so it is probably out of date: " + listNames(mismatches));
    }
}

std::string_view
ProjectBundleReader::projectYaml() const
{
    const std::optional<std::span<const std::byte>> bytes = rawMember(_root + std::string(kBundleProjectMember));

    return bytes ? bytesText(*bytes) : std::string_view();
}

ProjectSerialization
ProjectBundleReader::readProject(YamlWarnings* warnings) const
{
    if (warnings) {
        warnings->insert(warnings->end(), _warnings.begin(), _warnings.end());
    }

    return decodeProjectYaml(projectYaml(), *this, warnings);
}

std::optional<std::span<const std::byte>>
ProjectBundleReader::findMember(std::string_view path) const
{
    return rawMember(_root + std::string(path));
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
