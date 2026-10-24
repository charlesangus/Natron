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

#include "Serialization/Yaml/ManifestYaml.h"

#include <charconv>
#include <cstdio>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    std::string
    formatCrc(std::uint32_t crc)
    {
        char text[9];
        std::snprintf(text, sizeof(text), "%08x", crc);

        return std::string(text, 8);
    }

    std::uint32_t
    readCrc(const YAML::Node& node,
            const std::string& context)
    {
        using namespace YamlDetail;
        const std::string text = readString(node, context);
        std::uint32_t crc = 0;
        const char* end = text.data() + text.size();
        std::from_chars_result result = std::from_chars(text.data(), end, crc, 16);
        if ((text.size() != 8) || (result.ec != std::errc()) || (result.ptr != end)) {
            fail(node, context, "expected 8 hexadecimal digits, found \"" + text + "\"");
        }

        return crc;
    }

} // anon namespace

std::string
encodeManifestYaml(const BundleManifest& manifest)
{
    using namespace YamlDetail;
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitKey(em, "format");
    em << YAML::Value << std::string(kManifestYamlFormat);
    emitKey(em, "formatVersion");
    em << YAML::Value;
    emitInt(em, manifest.formatVersion);
    emitKey(em, "app");
    em << YAML::Value << YAML::Flow << YAML::BeginMap;
    emitKey(em, "version");
    em << YAML::Value;
    emitString(em, manifest.appVersion);
    emitKey(em, "commit");
    em << YAML::Value;
    emitString(em, manifest.appCommit);
    em << YAML::EndMap;
    emitKey(em, "saved");
    em << YAML::Value;
    emitString(em, manifest.saveDate);
    emitKey(em, "author");
    em << YAML::Value;
    emitString(em, manifest.author);
    emitKey(em, "created");
    em << YAML::Value;
    emitInt(em, manifest.creationDate);
    emitKey(em, "members");
    em << YAML::Value << YAML::BeginSeq;
    for (const BundleManifestMember& member : manifest.members) {
        em << YAML::Flow << YAML::BeginMap;
        emitKey(em, "name");
        em << YAML::Value;
        emitString(em, member.name);
        emitKey(em, "size");
        em << YAML::Value;
        emitInt(em, member.size);
        emitKey(em, "crc32");
        em << YAML::Value;
        emitString(em, formatCrc(member.crc32));
        em << YAML::EndMap;
    }
    em << YAML::EndSeq;
    em << YAML::EndMap;

    return documentText(em);
}

BundleManifest
decodeManifestYaml(std::string_view text,
                   YamlWarnings* warnings)
{
    using namespace YamlDetail;
    YAML::Node document = loadDocument(text, "manifest.yaml");
    if (!document.IsMap()) {
        throw YamlError("manifest.yaml is not a Natron bundle manifest: it is not a YAML map");
    }
    MapReader reader(document, "manifest", warnings);
    YAML::Node format = reader.get("format");
    if (!format || !format.IsScalar() || (format.Scalar() != kManifestYamlFormat)) {
        throw YamlError("manifest.yaml is not a Natron bundle manifest: \"format: " + std::string(kManifestYamlFormat) + "\" is missing");
    }
    BundleManifest manifest;
    manifest.formatVersion = readInt(reader.get("formatVersion"), "manifest.formatVersion");
    if (manifest.formatVersion < 1) {
        fail(reader.get("formatVersion"), "manifest.formatVersion", "invalid format version " + std::to_string(manifest.formatVersion));
    }
    // project.yaml carries the version a reader must refuse; the manifest only informs, so a
    // newer one still gives its author and dates, but its member list may mean something else.
    const bool newer = manifest.formatVersion > kBundleFormatVersion;
    if (newer && warnings) {
        warnings->push_back("manifest.yaml was written by a newer Natron (bundle format version " + std::to_string(manifest.formatVersion) + "; this version reads up to " + std::to_string(kBundleFormatVersion) + "): only its author and dates are read");
    }
    YamlWarnings* const keyWarnings = newer ? nullptr : warnings;
    if (YAML::Node app = reader.get("app")) {
        MapReader appReader(app, "manifest.app", keyWarnings);
        if (YAML::Node version = appReader.get("version")) {
            manifest.appVersion = readString(version, "manifest.app.version");
        }
        if (YAML::Node commit = appReader.get("commit")) {
            manifest.appCommit = readString(commit, "manifest.app.commit");
        }
        appReader.finish();
    }
    if (YAML::Node saved = reader.get("saved")) {
        manifest.saveDate = readString(saved, "manifest.saved");
    }
    if (YAML::Node author = reader.get("author")) {
        manifest.author = readString(author, "manifest.author");
    }
    if (YAML::Node created = reader.get("created")) {
        manifest.creationDate = readInt64(created, "manifest.created");
    }
    if (newer) {
        return manifest;
    }
    if (YAML::Node members = reader.get("members")) {
        if (!members.IsSequence()) {
            fail(members, "manifest.members", "expected a list of members");
        }
        for (std::size_t i = 0; i < members.size(); ++i) {
            const std::string context = "manifest.members[" + std::to_string(i) + "]";
            MapReader memberReader(members[i], context, warnings);
            BundleManifestMember member;
            member.name = readString(memberReader.get("name"), context + ".name");
            member.size = readUInt(memberReader.get("size"), context + ".size");
            member.crc32 = readCrc(memberReader.get("crc32"), context + ".crc32");
            memberReader.finish();
            manifest.members.push_back(member);
        }
    }
    reader.finish();

    return manifest;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
