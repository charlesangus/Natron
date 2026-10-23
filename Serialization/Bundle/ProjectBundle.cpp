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
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    // The local header of a stored member is 30 bytes and `mimetype` is 8 bytes long, so in a
    // bundle the mime type always starts at offset 38.
    constexpr std::size_t kMimeTypeOffset = 38;

    // Large enough for the whole mime type entry, and for the XML declaration or doctype of a
    // legacy project that may follow a short prologue.
    constexpr std::size_t kLegacyXmlSniffSize = 64;
    constexpr std::size_t kSniffSize = std::max(kLegacyXmlSniffSize, kMimeTypeOffset + kNatronProjectMimeType.size());
    static_assert(kSniffSize >= kMimeTypeOffset + kNatronProjectMimeType.size());

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
    const std::string_view text = bytesText(head.first(std::min(head.size(), kSniffSize)));
    if (text.starts_with("PK\x03\x04") && (head.size() >= kMimeTypeOffset + kNatronProjectMimeType.size()) && (bytesText(head.subspan(30, 8)) == "mimetype") && (bytesText(head.subspan(kMimeTypeOffset, kNatronProjectMimeType.size())) == kNatronProjectMimeType)) {
        return ProjectFileKind::eBundle;
    }
    if (contains(text, "<?xml") || contains(text, "<!DOCTYPE boost_serialization")) {
        return ProjectFileKind::eLegacyXml;
    }

    return ProjectFileKind::eUnknown;
}

ProjectFileKind
detectProjectFileKind(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return ProjectFileKind::eUnknown;
    }
    std::byte head[kSniffSize];
    const std::size_t size = std::fread(head, 1, sizeof(head), file);
    std::fclose(file);

    return detectProjectFileKind(std::span<const std::byte>(head, size));
}

ProjectBundleWriter::ProjectBundleWriter() = default;

void
ProjectBundleWriter::setProject(const ProjectSerialization& project)
{
    MemoryBlobStore blobs;
    std::string text = encodeProjectYaml(project, blobs);
    _projectYaml = std::move(text);
    _blobs = std::move(blobs);
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
    ZipWriter writer(output);
    writeMembers(writer);
}

void
ProjectBundleWriter::write(const std::string& path) const
{
    ZipWriter writer(path);
    writeMembers(writer);
}

ProjectBundleReader::ProjectBundleReader(const std::string& path)
    : _archive(openArchive(path))
{
    validate();
}

ProjectBundleReader::ProjectBundleReader(std::span<const std::byte> bytes)
    : _archive(openArchive(bytes))
{
    validate();
}

void
ProjectBundleReader::validate()
{
    const std::vector<ZipMember>& members = _archive.members();
    if (members.empty() || (members.front().name != "mimetype") || (bytesText(_archive.data(members.front())) != kNatronProjectMimeType)) {
        throw ProjectBundleError("Unrecognized or damaged project file: it is a zip archive but not a " NATRON_APPLICATION_NAME " project");
    }
    try {
        _archive.verify();
    } catch (const ZipError& e) {
        throw ProjectBundleError(std::string("Unrecognized or damaged project file: ") + e.what());
    }
    const ZipMember* manifest = _archive.find(kBundleManifestMember);
    if (!manifest) {
        throw ProjectBundleError("Unrecognized or damaged project file: manifest.yaml is missing");
    }
    try {
        _manifest = decodeManifestYaml(bytesText(_archive.data(*manifest)));
    } catch (const YamlError& e) {
        throw ProjectBundleError(e.what());
    }
    if (!_archive.contains(kBundleProjectMember)) {
        throw ProjectBundleError("Unrecognized or damaged project file: project.yaml is missing");
    }
}

std::string_view
ProjectBundleReader::projectYaml() const
{
    return bytesText(_archive.data(kBundleProjectMember));
}

ProjectSerialization
ProjectBundleReader::readProject(YamlWarnings* warnings) const
{
    return decodeProjectYaml(projectYaml(), *this, warnings);
}

std::optional<std::span<const std::byte>>
ProjectBundleReader::findMember(std::string_view path) const
{
    const ZipMember* member = _archive.find(path);
    if (!member) {
        return std::nullopt;
    }

    return _archive.data(*member);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
