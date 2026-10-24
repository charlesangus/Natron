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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/TLSHolder.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ManifestYaml.h"
#include "Serialization/Yaml/ProjectYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING
using Serialization::BundleManifest;
using Serialization::ProjectBundleError;
using Serialization::ProjectBundleReader;
using Serialization::ProjectBundleWriter;
using Serialization::ProjectFileKind;
using Serialization::ProjectSerialization;
using Serialization::YamlWarnings;
using Serialization::ZipReader;
using Serialization::ZipWriter;

namespace {

const char* const kStrokeBlob = "blobs/RotoPaint1/Stroke1.stroke.bin";
const char* const kTrackBlob = "blobs/Tracker1/track1.track.bin";

Serialization::KeyFrameSerialization
key(double time,
    double value)
{
    Serialization::KeyFrameSerialization result;
    result.time = time;
    result.value = value;
    result.interpolation = Serialization::kYamlKeyframeLinear;

    return result;
}

Serialization::NodeSerializationPtr
makeRotoNode()
{
    Serialization::NodeSerializationPtr node = std::make_shared<Serialization::NodeSerialization>();
    node->scriptName = "RotoPaint1";
    node->label = "RotoPaint1";
    node->pluginID = "fr.inria.built-in.RotoPaint";
    node->pluginMajorVersion = 1;
    node->pluginMinorVersion = 0;
    node->hasRotoContext = true;

    Serialization::RotoStrokeItemSerialization stroke;
    stroke.name = "Stroke1";
    stroke.label = "Stroke1";
    stroke.activated = true;
    stroke.parentLayerName = "Layer1";
    stroke.brushType = 3;
    Serialization::RotoStrokeCurvesSerialization curves;
    for (int i = 0; i < 2000; ++i) {
        const double time = 1. + i * 0.01;
        curves.xCurve.keyframes.push_back(key(time, std::sin(i * 0.1) * 960. + 960.));
        curves.yCurve.keyframes.push_back(key(time, std::cos(i * 0.1) * 540. + 540.));
        curves.pressureCurve.keyframes.push_back(key(time, (i % 100) / 100.));
    }
    stroke.strokes.push_back(curves);

    Serialization::RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeStroke;
    child.stroke = stroke;
    node->rotoContext.baseLayer.name = "Layer1";
    node->rotoContext.baseLayer.label = "Layer1";
    node->rotoContext.baseLayer.activated = true;
    node->rotoContext.baseLayer.children.push_back(child);

    return node;
}

Serialization::NodeSerializationPtr
makeTrackerNode()
{
    Serialization::NodeSerializationPtr node = std::make_shared<Serialization::NodeSerialization>();
    node->scriptName = "Tracker1";
    node->label = "Tracker1";
    node->pluginID = "fr.inria.built-in.Tracker";
    node->pluginMajorVersion = 1;
    node->pluginMinorVersion = 0;
    node->inputs["Source"] = "RotoPaint1";
    node->hasTrackerContext = true;

    Serialization::TrackSerialization track;
    track.scriptName = "track1";
    track.label = "track1";
    Serialization::KnobSerialization center;
    center.name = "center";
    center.typeName = "Double";
    center.dimension = 2;
    for (int d = 0; d < 2; ++d) {
        Serialization::KnobDimensionSerialization dimension;
        Serialization::CurveSerialization curve;
        for (int frame = 1; frame <= 500; ++frame) {
            curve.keyframes.push_back(key(frame, std::sin(frame * 0.017 + d) * 512.));
        }
        dimension.curve = curve;
        dimension.value = Serialization::KnobValueSerialization(std::in_place_type<double>, curve.keyframes.back().value);
        center.dimensions.push_back(dimension);
    }
    track.knobs.push_back(center);
    node->trackerContext.tracks.push_back(track);

    return node;
}

ProjectSerialization
makeProject()
{
    ProjectSerialization project;
    project.savedByVersion.versionMajor = 2;
    project.savedByVersion.versionMinor = 6;
    project.savedByVersion.versionRevision = 0;
    project.savedByVersion.gitBranch = "main";
    project.savedByVersion.gitCommit = "0123456789abcdef";
    project.savedByVersion.operatingSystem = "Linux";
    project.savedByVersion.bits = 64;
    project.timelineCurrentTime = 12;
    project.nodes.nodes.push_back(makeRotoNode());
    project.nodes.nodes.push_back(makeTrackerNode());

    return project;
}

BundleManifest
makeManifest(const std::string& saveDate)
{
    BundleManifest manifest;
    manifest.appVersion = "2.6.0";
    manifest.appCommit = "0123456789abcdef";
    manifest.saveDate = saveDate;
    manifest.author = "user@host";
    manifest.creationDate = 1700000000000LL;

    return manifest;
}

ProjectBundleWriter
makeWriter(const std::string& saveDate)
{
    ProjectBundleWriter writer;
    writer.setProject(makeProject());
    writer.setManifest(makeManifest(saveDate));

    return writer;
}

std::vector<std::byte>
readFileBytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        bytes[i] = static_cast<std::byte>(chars[i]);
    }

    return bytes;
}

void
writeFileText(const std::string& path,
              const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
}

std::span<const std::byte>
asBytes(std::string_view text)
{
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string
asText(std::span<const std::byte> bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const Serialization::RotoStrokeItemSerialization&
strokeOf(const ProjectSerialization& project)
{
    return project.nodes.nodes.front()->rotoContext.baseLayer.children.front().stroke;
}

const Serialization::KnobSerialization&
centerOf(const ProjectSerialization& project)
{
    return std::next(project.nodes.nodes.begin())->get()->trackerContext.tracks.front().knobs.front();
}

struct Entry {
    std::string name;
    std::vector<std::byte> data;
    std::optional<std::uint32_t> crc = std::nullopt;
};

std::vector<std::byte>
toBytes(std::string_view text)
{
    const std::span<const std::byte> bytes = asBytes(text);

    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

void
writeFileBytes(const std::string& path,
               const std::vector<std::byte>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void
put16(std::vector<std::byte>& out,
      std::uint32_t v)
{
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}

void
put32(std::vector<std::byte>& out,
      std::uint32_t v)
{
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

void
putText(std::vector<std::byte>& out,
        const std::string& text)
{
    const std::span<const std::byte> bytes = asBytes(text);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// ZipWriter always leads with a mimetype member, so archives laid out as other zip tools lay
// them out are built by hand: stored members, in the given order.
std::vector<std::byte>
buildStoredZip(const std::vector<Entry>& entries)
{
    std::vector<std::byte> out;
    std::vector<std::uint32_t> offsets;
    for (const Entry& e : entries) {
        offsets.push_back(static_cast<std::uint32_t>(out.size()));
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 33);
        put32(out, e.crc.value_or(Serialization::zipCrc32(e.data)));
        put32(out, static_cast<std::uint32_t>(e.data.size()));
        put32(out, static_cast<std::uint32_t>(e.data.size()));
        put16(out, static_cast<std::uint32_t>(e.name.size()));
        put16(out, 0);
        putText(out, e.name);
        out.insert(out.end(), e.data.begin(), e.data.end());
    }
    const std::uint32_t directoryOffset = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        put32(out, 0x02014b50);
        put16(out, 20);
        put16(out, 20);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 33);
        put32(out, e.crc.value_or(Serialization::zipCrc32(e.data)));
        put32(out, static_cast<std::uint32_t>(e.data.size()));
        put32(out, static_cast<std::uint32_t>(e.data.size()));
        put16(out, static_cast<std::uint32_t>(e.name.size()));
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, offsets[i]);
        putText(out, e.name);
    }
    const std::uint32_t directorySize = static_cast<std::uint32_t>(out.size()) - directoryOffset;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, directorySize);
    put32(out, directoryOffset);
    put16(out, 0);

    return out;
}

/**
 * @brief The members a writer would write, named under prefix ("" or "folder/"); the mimetype
 * and the manifest only when asked for.
 **/
std::vector<Entry>
bundleEntries(const ProjectBundleWriter& writer,
              const std::string& prefix,
              bool withMimeType,
              bool withManifest)
{
    std::vector<Entry> entries;
    if (withMimeType) {
        entries.push_back({ prefix + "mimetype", toBytes(Serialization::kNatronProjectMimeType) });
    }
    if (withManifest) {
        entries.push_back({ prefix + "manifest.yaml", toBytes(Serialization::encodeManifestYaml(writer.manifest())) });
    }
    entries.push_back({ prefix + "project.yaml", toBytes(writer.projectYaml()) });
    for (const auto& blob : writer.blobs().members()) {
        entries.push_back({ prefix + blob.first, blob.second });
    }

    return entries;
}

void
writeFolder(const std::filesystem::path& folder,
            const std::vector<Entry>& entries)
{
    for (const Entry& e : entries) {
        const std::filesystem::path file = folder / e.name;
        std::filesystem::create_directories(file.parent_path());
        writeFileBytes(file.string(), e.data);
    }
}

/**
 * @brief The project as project.yaml text and blob members, so two loads of the same graph
 * compare equal exactly when they would save the same.
 **/
std::pair<std::string, std::map<std::string, std::vector<std::byte>, std::less<>>>
reencode(const ProjectSerialization& project)
{
    Serialization::MemoryBlobStore blobs;
    std::string text = Serialization::encodeProjectYaml(project, blobs);

    return { std::move(text), blobs.members() };
}

bool
containsText(const std::string& haystack,
             std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // anonymous namespace

TEST(ProjectBundle, MembersAreWrittenInTheirFixedOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("order.ntpz")).toStdString();
    ProjectBundleWriter orderWriter = makeWriter("2026-10-22T10:00:00Z");
    orderWriter.setRootFolder("order");
    orderWriter.write(path);

    ZipReader archive(path);
    std::vector<std::string> names;
    for (const Serialization::ZipMember& member : archive.members()) {
        names.push_back(member.name);
    }
    const std::vector<std::string> expected = { "order/mimetype", "order/manifest.yaml", "order/project.yaml", std::string("order/") + kStrokeBlob, std::string("order/") + kTrackBlob };
    EXPECT_EQ(expected, names);
    EXPECT_EQ(std::string(Serialization::kNatronProjectMimeType), asText(archive.data("order/mimetype")));

    const BundleManifest manifest = Serialization::decodeManifestYaml(asText(archive.data("order/manifest.yaml")));
    ASSERT_EQ(3u, manifest.members.size());
    for (const Serialization::BundleManifestMember& listed : manifest.members) {
        const Serialization::ZipMember* member = archive.find("order/" + listed.name);
        ASSERT_TRUE(member) << listed.name;
        EXPECT_EQ(member->size, listed.size) << listed.name;
        EXPECT_EQ(member->crc32, listed.crc32) << listed.name;
    }
    EXPECT_EQ("user@host", manifest.author);
    EXPECT_EQ("2026-10-22T10:00:00Z", manifest.saveDate);
    EXPECT_EQ(1700000000000LL, manifest.creationDate);
}

TEST(ProjectBundle, RotoAndTrackerBlobsReadBack)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("blobs.ntpz")).toStdString();
    const ProjectSerialization original = makeProject();
    makeWriter("2026-10-22T10:00:00Z").write(path);

    ProjectBundleReader reader(path);
    EXPECT_EQ("project/", reader.root());
    EXPECT_TRUE(reader.findMember(kStrokeBlob).has_value());
    EXPECT_TRUE(reader.findMember(kTrackBlob).has_value());
    EXPECT_EQ(std::string::npos, std::string(reader.projectYaml()).find("1.01")) << "stroke samples belong in the blob";

    YamlWarnings warnings;
    const ProjectSerialization loaded = reader.readProject(&warnings);
    EXPECT_TRUE(warnings.empty());
    ASSERT_EQ(2u, loaded.nodes.nodes.size());
    EXPECT_EQ(12, loaded.timelineCurrentTime);

    ASSERT_EQ(1u, strokeOf(loaded).strokes.size());
    EXPECT_EQ(strokeOf(original).strokes.front().xCurve, strokeOf(loaded).strokes.front().xCurve);
    EXPECT_EQ(strokeOf(original).strokes.front().yCurve, strokeOf(loaded).strokes.front().yCurve);
    EXPECT_EQ(strokeOf(original).strokes.front().pressureCurve, strokeOf(loaded).strokes.front().pressureCurve);

    ASSERT_EQ(2u, centerOf(loaded).dimensions.size());
    for (int d = 0; d < 2; ++d) {
        ASSERT_TRUE(centerOf(loaded).dimensions[d].curve.has_value());
        EXPECT_EQ(*centerOf(original).dimensions[d].curve, *centerOf(loaded).dimensions[d].curve);
    }
    EXPECT_EQ("RotoPaint1", std::next(loaded.nodes.nodes.begin())->get()->inputs.at("Source"));
}

TEST(ProjectBundle, AMissingBlobWarnsAndLeavesItsItemEmpty)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string complete = dir.filePath(QString::fromUtf8("complete.ntpz")).toStdString();
    const std::string damaged = dir.filePath(QString::fromUtf8("damaged.ntpz")).toStdString();
    makeWriter("2026-10-22T10:00:00Z").write(complete);
    {
        ZipReader source(complete);
        ZipWriter copy(damaged, Serialization::kNatronProjectMimeType, "damaged");
        for (const Serialization::ZipMember& member : source.members()) {
            const std::string relative = member.name.substr(member.name.find('/') + 1);
            if ((relative != "mimetype") && (relative != kStrokeBlob)) {
                copy.addMember(relative, source.data(member));
            }
        }
        copy.finish();
    }

    ProjectBundleReader reader(damaged);
    YamlWarnings warnings;
    const ProjectSerialization loaded = reader.readProject(&warnings);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_NE(std::string::npos, warnings.front().find(kStrokeBlob)) << warnings.front();
    EXPECT_NE(std::string::npos, warnings.front().find("is missing")) << warnings.front();
    EXPECT_EQ("Stroke1", strokeOf(loaded).name);
    EXPECT_TRUE(strokeOf(loaded).strokes.empty());
    EXPECT_EQ(500u, centerOf(loaded).dimensions[0].curve->keyframes.size());
}

TEST(ProjectBundle, TwoSavesOfTheSameProjectDifferOnlyInTheManifest)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string first = dir.filePath(QString::fromUtf8("first.ntpz")).toStdString();
    const std::string second = dir.filePath(QString::fromUtf8("second.ntpz")).toStdString();
    makeWriter("2026-10-22T10:00:00Z").write(first);
    makeWriter("2026-10-23T08:30:00Z").write(second);

    ZipReader a(first);
    ZipReader b(second);
    ASSERT_EQ(a.members().size(), b.members().size());
    for (std::size_t i = 0; i < a.members().size(); ++i) {
        const Serialization::ZipMember& ma = a.members()[i];
        const Serialization::ZipMember& mb = b.members()[i];
        ASSERT_EQ(ma.name.substr(ma.name.find('/')), mb.name.substr(mb.name.find('/')));
        if (ma.name.ends_with("/manifest.yaml")) {
            EXPECT_NE(asText(a.data(ma)), asText(b.data(mb)));
        } else {
            EXPECT_EQ(asText(a.data(ma)), asText(b.data(mb))) << ma.name;
        }
    }
}

TEST(ProjectBundle, EveryMemberSitsUnderTheStemAlignedAndSavesAreByteIdentical)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string first = dir.filePath(QString::fromUtf8("first.ntpz")).toStdString();
    const std::string second = dir.filePath(QString::fromUtf8("second.ntpz")).toStdString();
    ProjectBundleWriter a = makeWriter("2026-10-22T10:00:00Z");
    a.setRootFolder("test");
    a.write(first);
    ProjectBundleWriter b = makeWriter("2026-10-22T10:00:00Z");
    b.setRootFolder("test");
    b.write(second);
    EXPECT_EQ(readFileBytes(first), readFileBytes(second));

    ZipReader archive(first);
    ASSERT_FALSE(archive.members().empty());
    for (const Serialization::ZipMember& member : archive.members()) {
        EXPECT_TRUE(member.name.starts_with("test/")) << member.name;
        if (member.name != "test/mimetype") {
            EXPECT_EQ(0u, member.dataOffset % Serialization::kZipMemberAlignment) << member.name;
        }
    }
    EXPECT_EQ("test/mimetype", archive.members().front().name);
    EXPECT_EQ(Serialization::ProjectFileKind::eBundle, Serialization::detectProjectFileKind(first));
    ProjectBundleReader reader(first);
    EXPECT_EQ(12, reader.readProject().timelineCurrentTime);
}

TEST(ProjectBundle, AnUnusableStemFallsBackToProject)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::vector<std::string> stems = { "", "..", "a/b", "a\\b", ".", std::string("a\0b", 3) };
    for (const std::string& stem : stems) {
        const std::string path = dir.filePath(QString::fromUtf8("bad.ntpz")).toStdString();
        ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
        writer.setRootFolder(stem);
        writer.write(path);
        ZipReader archive(path);
        for (const Serialization::ZipMember& member : archive.members()) {
            EXPECT_TRUE(member.name.starts_with("project/")) << member.name;
        }
        EXPECT_NO_THROW(ProjectBundleReader(path).readProject());
    }
}

TEST(ProjectBundle, TheReaderRefusesWhatIsNotANatronBundle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const std::string otherZip = dir.filePath(QString::fromUtf8("other.ntpz")).toStdString();
    {
        ZipWriter writer(otherZip, "application/zip");
        writer.addMember("project.yaml", asBytes("format: natron-project\n"));
        writer.finish();
    }
    EXPECT_THROW(ProjectBundleReader reader(otherZip), ProjectBundleError);

    const std::string newer = dir.filePath(QString::fromUtf8("newer.ntpz")).toStdString();
    {
        ZipWriter writer(newer);
        writer.addMember("manifest.yaml", asBytes("format: natron-bundle\nformatVersion: 1\n"));
        writer.addMember("project.yaml", asBytes("format: natron-project\nformatVersion: 99\n"));
        writer.finish();
    }
    try {
        ProjectBundleReader(newer).readProject();
        ADD_FAILURE() << "a project from a newer Natron was read";
    } catch (const Serialization::YamlNewerFormatError& e) {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("newer Natron")) << e.what();
    }

    const std::string truncated = dir.filePath(QString::fromUtf8("truncated.ntpz")).toStdString();
    makeWriter("2026-10-22T10:00:00Z").write(truncated);
    std::vector<std::byte> bytes = readFileBytes(truncated);
    bytes.resize(bytes.size() - 10);
    try {
        ProjectBundleReader reader(std::span<const std::byte>(bytes.data(), bytes.size()));
        ADD_FAILURE() << "a truncated bundle was read";
    } catch (const ProjectBundleError& e) {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("damaged")) << e.what();
    }
}

TEST(ProjectBundle, DetectProjectFileKindClassifiesEachKind)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const std::string bundle = dir.filePath(QString::fromUtf8("bundle.ntpz")).toStdString();
    makeWriter("2026-10-22T10:00:00Z").write(bundle);
    EXPECT_EQ(ProjectFileKind::eBundle, Serialization::detectProjectFileKind(bundle));

    const std::string xml = dir.filePath(QString::fromUtf8("old.ntp")).toStdString();
    writeFileText(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n<!DOCTYPE boost_serialization>\n");
    EXPECT_EQ(ProjectFileKind::eLegacyXml, Serialization::detectProjectFileKind(xml));

    const std::string doctype = dir.filePath(QString::fromUtf8("doctype.ntp")).toStdString();
    writeFileText(doctype, "\n\n<!DOCTYPE boost_serialization>\n<boost_serialization signature=\"serialization::archive\">\n");
    EXPECT_EQ(ProjectFileKind::eLegacyXml, Serialization::detectProjectFileKind(doctype));

    const std::string otherZip = dir.filePath(QString::fromUtf8("other.zip")).toStdString();
    {
        ZipWriter writer(otherZip, "application/zip");
        writer.finish();
    }
    EXPECT_EQ(ProjectFileKind::eBundle, Serialization::detectProjectFileKind(otherZip));

    const std::string emptyZip = dir.filePath(QString::fromUtf8("empty.zip")).toStdString();
    writeFileBytes(emptyZip, buildStoredZip({}));
    EXPECT_EQ(ProjectFileKind::eBundle, Serialization::detectProjectFileKind(emptyZip));

    EXPECT_EQ(ProjectFileKind::eDirectory, Serialization::detectProjectFileKind(dir.path().toStdString()));
    const std::string yaml = dir.filePath(QString::fromUtf8("project.yaml")).toStdString();
    writeFileText(yaml, "format: natron-project\nformatVersion: 1\n");
    EXPECT_EQ(ProjectFileKind::eProjectYaml, Serialization::detectProjectFileKind(yaml));

    const std::string garbage = dir.filePath(QString::fromUtf8("garbage.ntpz")).toStdString();
    writeFileText(garbage, "this is not a project at all");
    EXPECT_EQ(ProjectFileKind::eUnknown, Serialization::detectProjectFileKind(garbage));

    const std::string empty = dir.filePath(QString::fromUtf8("empty.ntpz")).toStdString();
    writeFileText(empty, "");
    EXPECT_EQ(ProjectFileKind::eUnknown, Serialization::detectProjectFileKind(empty));

    EXPECT_EQ(ProjectFileKind::eUnknown, Serialization::detectProjectFileKind(dir.filePath(QString::fromUtf8("absent.ntpz")).toStdString()));
}

TEST(ProjectBundle, AFailedSaveLeavesThePreviousFileIntact)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("project.ntpz"));

    const ProjectBundleWriter previous = makeWriter("2026-10-22T10:00:00Z");
    Project::writeProjectFileAtomically(path, [&previous](Serialization::ZipOutput& output) {
        previous.write(output);
    });
    const std::vector<std::byte> before = readFileBytes(path.toStdString());
    ASSERT_FALSE(before.empty());

    ProjectSerialization changed = makeProject();
    changed.timelineCurrentTime = 99;
    ProjectBundleWriter next;
    next.setProject(changed);
    bool replaced = false;
    EXPECT_THROW(Project::writeProjectFileAtomically(path, [&next](Serialization::ZipOutput& output) {
        ZipWriter writer(output);
        writer.addMember("manifest.yaml", asBytes(Serialization::encodeManifestYaml(next.manifest())));
        writer.addMember("project.yaml", asBytes(next.projectYaml()));
        throw std::runtime_error("disk full"); }, [&replaced]() { replaced = true; }), std::runtime_error);
    EXPECT_FALSE(replaced);

    EXPECT_EQ(before, readFileBytes(path.toStdString()));
    ProjectBundleReader reader(path.toStdString());
    EXPECT_EQ(12, reader.readProject().timelineCurrentTime);

    QDir listing(dir.path());
    EXPECT_EQ(QStringList() << QString::fromUtf8("project.ntpz"), listing.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot));
}

TEST(ProjectBundle, BackupRotationLeavesTheTargetInPlaceUntilTheNewFileIsCommitted)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("project.ntpz"));
    const QString backup1 = path + QString::fromUtf8(".~1~");
    const QString backup2 = path + QString::fromUtf8(".~2~");

    const ProjectBundleWriter previous = makeWriter("2026-10-22T10:00:00Z");
    Project::writeProjectFileAtomically(path, [&previous](Serialization::ZipOutput& output) {
        previous.write(output);
    });
    const std::vector<std::byte> before = readFileBytes(path.toStdString());
    ASSERT_FALSE(before.empty());
    writeFileText(backup1.toStdString(), "older backup");

    ProjectSerialization changed = makeProject();
    changed.timelineCurrentTime = 99;
    ProjectBundleWriter next;
    next.setProject(changed);

    // Throwing after the rotation stands for a commit that fails once the backups have moved:
    // either way the new file is discarded without replacing the target.
    const auto failAfterRotating = [&path, &next](int saveVersions) {
        EXPECT_THROW(Project::writeProjectFileAtomically(path, [&next](Serialization::ZipOutput& output) { next.write(output); }, [&path, saveVersions]() {
            Project::rotateBackups(path, saveVersions);
            throw std::runtime_error("commit failed"); }), std::runtime_error);
    };

    failAfterRotating(2);
    EXPECT_EQ(before, readFileBytes(path.toStdString()));
    EXPECT_EQ(before, readFileBytes(backup1.toStdString()));
    const std::span<const std::byte> older = asBytes("older backup");
    EXPECT_EQ(std::vector<std::byte>(older.begin(), older.end()), readFileBytes(backup2.toStdString()));

    Project::writeProjectFileAtomically(path, [&next](Serialization::ZipOutput& output) { next.write(output); }, [&path]() { Project::rotateBackups(path, 2); });
    const std::vector<std::byte> after = readFileBytes(path.toStdString());
    EXPECT_EQ(99, ProjectBundleReader(path.toStdString()).readProject().timelineCurrentTime);
    EXPECT_EQ(before, readFileBytes(backup1.toStdString()));
    EXPECT_EQ(before, readFileBytes(backup2.toStdString()));
    EXPECT_FALSE(QFile::exists(path + QString::fromUtf8(".~3~")));

    // Keeping no versions drops every backup, still never the target.
    failAfterRotating(0);
    EXPECT_EQ(after, readFileBytes(path.toStdString()));
    EXPECT_FALSE(QFile::exists(backup1));
    EXPECT_FALSE(QFile::exists(backup2));
}

TEST(ProjectBundle, SavesStartedTogetherNeverShareATemporaryFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    constexpr int kSavers = 32;
    constexpr int kRounds = 10;
    const QString shared = dir.filePath(QString::fromUtf8("shared.ntpz"));

    std::vector<ProjectBundleWriter> writers(kSavers);
    QStringList expectedFiles;
    expectedFiles << QString::fromUtf8("shared.ntpz");
    for (int i = 0; i < kSavers; ++i) {
        ProjectSerialization project = makeProject();
        project.timelineCurrentTime = 100 + i;
        writers[i].setProject(project);
        writers[i].setManifest(makeManifest("2026-10-22T10:00:00Z"));
        expectedFiles << QString::fromUtf8("project%1.ntpz").arg(i);
    }
    expectedFiles.sort();

    for (int round = 0; round < kRounds; ++round) {
        // Released together, the savers start within the same millisecond, which is what made a
        // clock-derived temporary name collide.
        std::barrier start(kSavers);
        std::vector<std::string> failures(kSavers);
        std::vector<std::thread> savers;
        for (int i = 0; i < kSavers; ++i) {
            savers.emplace_back([&, i]() {
                const QString own = dir.filePath(QString::fromUtf8("project%1.ntpz").arg(i));
                const auto write = [&writers, i](Serialization::ZipOutput& output) { writers[i].write(output); };
                start.arrive_and_wait();
                try {
                    Project::writeProjectFileAtomically(own, write);
                    Project::writeProjectFileAtomically(shared, write);
                } catch (const std::exception& e) {
                    failures[i] = e.what();
                }
            });
        }
        for (std::thread& saver : savers) {
            saver.join();
        }

        for (int i = 0; i < kSavers; ++i) {
            EXPECT_TRUE(failures[i].empty()) << "round " << round << ", saver " << i << ": " << failures[i];
            const QString own = dir.filePath(QString::fromUtf8("project%1.ntpz").arg(i));
            EXPECT_EQ(100 + i, ProjectBundleReader(own.toStdString()).readProject().timelineCurrentTime) << "round " << round;
        }
        // The last commit wins the shared target, and it is always one saver's complete project.
        const int sharedTime = ProjectBundleReader(shared.toStdString()).readProject().timelineCurrentTime;
        EXPECT_TRUE(sharedTime >= 100 && sharedTime < 100 + kSavers) << "round " << round << ": " << sharedTime;

        QDir listing(dir.path());
        EXPECT_EQ(expectedFiles, listing.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) << "round " << round;
    }
}

TEST(ProjectBundle, AnUnwritableDirectoryFailsTheSaveAndKeepsTheTarget)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("project.ntpz"));
    const ProjectBundleWriter previous = makeWriter("2026-10-22T10:00:00Z");
    Project::writeProjectFileAtomically(path, [&previous](Serialization::ZipOutput& output) {
        previous.write(output);
    });
    const std::vector<std::byte> before = readFileBytes(path.toStdString());
    ASSERT_FALSE(before.empty());

    const QFileDevice::Permissions writable = QFile::permissions(dir.path());
    ASSERT_TRUE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QFile probe(dir.filePath(QString::fromUtf8("probe")));
    const bool permissionsApply = !probe.open(QIODevice::WriteOnly);
    probe.close();
    if (permissionsApply) {
        ProjectSerialization changed = makeProject();
        changed.timelineCurrentTime = 99;
        ProjectBundleWriter next;
        next.setProject(changed);
        bool replaced = false;
        EXPECT_THROW(Project::writeProjectFileAtomically(path, [&next](Serialization::ZipOutput& output) { next.write(output); }, [&path, &replaced]() {
            replaced = true;
            Project::rotateBackups(path, 1); }), std::runtime_error);
        EXPECT_FALSE(replaced);
    } else {
        // Root ignores directory permissions, so there is nothing to check here.
        std::cerr << "Skipping: the directory stays writable for this user." << std::endl;
        QFile::remove(probe.fileName());
    }
    ASSERT_TRUE(QFile::setPermissions(dir.path(), writable));

    EXPECT_EQ(before, readFileBytes(path.toStdString()));
    QDir listing(dir.path());
    EXPECT_EQ(QStringList() << QString::fromUtf8("project.ntpz"), listing.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot));
}

TEST(ProjectBundle, AnAutoSaveBeingWrittenIsNotTakenForAnAutoSave)
{
    EXPECT_EQ(9, Project::autoSaveProjectNameLength(QString::fromUtf8("Shot.ntpz.autosave")));
    EXPECT_EQ(9, Project::autoSaveProjectNameLength(QString::fromUtf8("Shot.ntpz.autosave.1234567890")));
    EXPECT_EQ(-1, Project::autoSaveProjectNameLength(QString::fromUtf8("Shot.ntpz.autosave.1234567890.aB3dE9")));
    EXPECT_EQ(-1, Project::autoSaveProjectNameLength(QString::fromUtf8("Shot.ntpz.autosave.aB3dE9")));
    EXPECT_EQ(-1, Project::autoSaveProjectNameLength(QString::fromUtf8("RENDER_SAVE.ntpz.autosave")));
    EXPECT_EQ(-1, Project::autoSaveProjectNameLength(QString::fromUtf8("Shot.ntpz")));

    // Whatever QSaveFile names its file while the auto-save is open (nothing at all where the
    // file system supports unnamed temporary files), a scan for auto-saves must skip it.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString autoSave = dir.filePath(QString::fromUtf8("Shot.ntpz.autosave.1234567890"));
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    QStringList seenWhileWriting;
    Project::writeProjectFileAtomically(autoSave, [&writer](Serialization::ZipOutput& output) { writer.write(output); }, [&dir, &seenWhileWriting]() { seenWhileWriting = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot); });
    for (const QString& entry : seenWhileWriting) {
        EXPECT_EQ(-1, Project::autoSaveProjectNameLength(entry)) << entry.toStdString();
    }
    EXPECT_EQ(9, Project::autoSaveProjectNameLength(QFileInfo(autoSave).fileName()));
}

TEST(ProjectBundle, BackupRotationKeepsTwelveVersionsInOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("project.ntpz"));
    constexpr int kSaves = 12;

    // Past nine backups, an alphabetical order puts .~10~ before .~2~ and the rotation renames
    // backups over each other.
    for (int save = 0; save < kSaves; ++save) {
        ProjectSerialization project = makeProject();
        project.timelineCurrentTime = save;
        ProjectBundleWriter writer;
        writer.setProject(project);
        Project::writeProjectFileAtomically(path, [&writer](Serialization::ZipOutput& output) { writer.write(output); }, [&path]() { Project::rotateBackups(path, 32); });
    }

    EXPECT_EQ(kSaves - 1, ProjectBundleReader(path.toStdString()).readProject().timelineCurrentTime);
    for (int backup = 1; backup < kSaves; ++backup) {
        const QString backupPath = path + QString::fromUtf8(".~%1~").arg(backup);
        ASSERT_TRUE(QFile::exists(backupPath)) << backupPath.toStdString();
        EXPECT_EQ(kSaves - 1 - backup, ProjectBundleReader(backupPath.toStdString()).readProject().timelineCurrentTime) << backupPath.toStdString();
    }
    EXPECT_FALSE(QFile::exists(path + QString::fromUtf8(".~%1~").arg(kSaves)));
    EXPECT_EQ(kSaves, QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size());
}

namespace {

const int kHeldSaveGuardSeconds = 30;

class ProjectSaveTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

} // anonymous namespace

TEST_F(ProjectSaveTest, TwoRenderSavesGetTheirOwnFiles)
{
    ASSERT_TRUE(QDir().mkpath(Project::autoSavesDir()));
    ProjectPtr project = getApp()->getProject();

    QString first, second;
    ASSERT_TRUE(project->saveForRenderProcess(&first));
    ASSERT_TRUE(project->saveForRenderProcess(&second));

    EXPECT_NE(first, second);
    for (const QString& renderSave : { first, second }) {
        EXPECT_TRUE(QFile::exists(renderSave)) << renderSave.toStdString();
        EXPECT_TRUE(Project::isRenderSaveFile(renderSave)) << renderSave.toStdString();
        // An auto-save scan must never offer a render save as a project to restore.
        EXPECT_EQ(-1, Project::autoSaveProjectNameLength(QFileInfo(renderSave).fileName())) << renderSave.toStdString();
        QFile::remove(renderSave);
    }
    EXPECT_FALSE(Project::isRenderSaveFile(QDir::temp().filePath(QFileInfo(first).fileName())));
}

TEST_F(ProjectSaveTest, ASaveAskedForDuringAnAutoSaveRunsAfterIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString startedPath = dir.filePath(QString::fromUtf8("started"));
    const QString goPath = dir.filePath(QString::fromUtf8("go"));
    const QString logPath = dir.filePath(QString::fromUtf8("log"));
    ProjectPtr project = getApp()->getProject();

    // The save callback holds the auto-save open until the main thread lets it go, and logs the
    // order in which the two saves reach it.
    const std::string define = "import os, time\n"
                               "def holdAutoSave(filename, app, autoSave):\n"
                               "    log = open('"
        + logPath.toStdString() + "', 'a')\n"
                                  "    if autoSave:\n"
                                  "        open('"
        + startedPath.toStdString() + "', 'w').close()\n"
                                      "        deadline = time.monotonic() + "
        + std::to_string(kHeldSaveGuardSeconds) + "\n"
                                                  "        while not os.path.exists('"
        + goPath.toStdString() + "') and time.monotonic() < deadline:\n"
                                 "            time.sleep(0.005)\n"
                                 "        log.write('auto-save released\\n' if os.path.exists('"
        + goPath.toStdString() + "') else 'auto-save timed out\\n')\n"
                                 "    else:\n"
                                 "        log.write('save\\n')\n"
                                 "    log.close()\n"
                                 "    return filename\n";
    std::string error, output;
    ASSERT_TRUE(interpretPythonScript(define, &error, &output)) << error;
    KnobString* onSave = dynamic_cast<KnobString*>(project->getKnobByName("beforeProjectSave").get());
    ASSERT_TRUE(onSave);
    onSave->setValue(std::string("holdAutoSave"));

    bool autoSaved = false;
    std::thread autoSaver([&project, &dir, &autoSaved]() {
        autoSaved = project->saveProject_imp(dir.path(), QString::fromUtf8("held.ntpz"), true, false);
        appPTR->getAppTLS()->cleanupTLSForThread();
    });
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kHeldSaveGuardSeconds);
    while (!QFile::exists(startedPath) && std::chrono::steady_clock::now() < deadline) {
        QThread::msleep(1);
    }
    const bool autoSaveStarted = QFile::exists(startedPath);

    // Only fires if the main thread keeps running its events while the save waits.
    QTimer::singleShot(100, [goPath]() {
        QFile go(goPath);
        go.open(QIODevice::WriteOnly);
    });
    QString savedPath;
    const bool saved = project->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("manual.ntpz"), &savedPath);
    autoSaver.join();
    onSave->setValue(std::string());

    ASSERT_TRUE(autoSaveStarted);
    EXPECT_TRUE(autoSaved);
    EXPECT_TRUE(saved);
    EXPECT_TRUE(QFile::exists(dir.filePath(QString::fromUtf8("held.ntpz.autosave"))));
    EXPECT_TRUE(QFile::exists(dir.filePath(QString::fromUtf8("manual.ntpz"))));
    QFile log(logPath);
    ASSERT_TRUE(log.open(QIODevice::ReadOnly));
    EXPECT_EQ(std::string("auto-save released\nsave\n"), QString::fromUtf8(log.readAll()).toStdString());
}

TEST(ProjectBundle, TheMembersMayBeAtTheTopOrInAnyOneFolder)
{
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    for (const std::string& prefix : { std::string(), std::string("anything/") }) {
        const std::vector<std::byte> zip = buildStoredZip(bundleEntries(writer, prefix, true, true));
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        EXPECT_EQ(prefix, reader.root());
        EXPECT_TRUE(reader.hasManifest());
        EXPECT_EQ("user@host", reader.manifest().author);
        YamlWarnings warnings;
        const ProjectSerialization loaded = reader.readProject(&warnings);
        EXPECT_TRUE(warnings.empty()) << prefix << ": " << warnings.front();
        EXPECT_EQ(12, loaded.timelineCurrentTime);
        ASSERT_EQ(1u, strokeOf(loaded).strokes.size());
    }
}

TEST(ProjectBundle, ARenamedFileStillOpens)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string saved = dir.filePath(QString::fromUtf8("test.ntpz")).toStdString();
    const std::string renamed = dir.filePath(QString::fromUtf8("renamed.ntpz")).toStdString();
    ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    writer.setRootFolder("test");
    writer.write(saved);
    std::filesystem::rename(saved, renamed);

    ProjectBundleReader reader(renamed);
    EXPECT_EQ("test/", reader.root());
    EXPECT_TRUE(reader.warnings().empty());
    EXPECT_EQ(12, reader.readProject().timelineCurrentTime);
}

TEST(ProjectBundle, TheMimeTypeIsOptionalButMustNotNameAnotherType)
{
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    {
        const std::vector<std::byte> zip = buildStoredZip(bundleEntries(writer, "test/", false, true));
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        YamlWarnings warnings;
        EXPECT_EQ(12, reader.readProject(&warnings).timelineCurrentTime);
        EXPECT_TRUE(warnings.empty());
    }
    {
        std::vector<Entry> entries = bundleEntries(writer, "test/", false, true);
        entries.push_back({ "mimetype", toBytes(std::string(Serialization::kNatronProjectMimeType) + "\n") });
        const std::vector<std::byte> zip = buildStoredZip(entries);
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        EXPECT_TRUE(reader.warnings().empty()) << "a top-level mimetype is not outside the project";
    }

    std::vector<Entry> inFolder = bundleEntries(writer, "test/", false, true);
    inFolder.insert(inFolder.begin(), { "test/mimetype", toBytes("application/zip") });
    std::vector<Entry> atTop = bundleEntries(writer, "test/", false, true);
    atTop.insert(atTop.begin(), { "mimetype", toBytes("application/epub+zip") });
    for (const std::vector<Entry>& entries : { inFolder, atTop }) {
        const std::vector<std::byte> zip = buildStoredZip(entries);
        try {
            ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
            ADD_FAILURE() << "an archive of another type was read as a project";
        } catch (const ProjectBundleError& e) {
            EXPECT_TRUE(containsText(e.what(), "Not a " NATRON_APPLICATION_NAME " project")) << e.what();
        }
    }
}

TEST(ProjectBundle, TheRootIsTheOneProjectYamlAtDepthZeroOrOne)
{
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");

    std::vector<Entry> twoFolders = bundleEntries(writer, "a/", true, true);
    for (const Entry& e : bundleEntries(writer, "b/", true, true)) {
        twoFolders.push_back(e);
    }
    std::vector<Entry> topAndFolder = bundleEntries(writer, "", false, true);
    for (const Entry& e : bundleEntries(writer, "copy/", false, false)) {
        topAndFolder.push_back(e);
    }
    for (const std::vector<Entry>& entries : { twoFolders, topAndFolder }) {
        const std::vector<std::byte> zip = buildStoredZip(entries);
        try {
            ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
            ADD_FAILURE() << "an archive with two projects was read";
        } catch (const ProjectBundleError& e) {
            EXPECT_TRUE(containsText(e.what(), "Ambiguous project archive")) << e.what();
        }
    }

    const std::vector<std::vector<Entry>> withoutProject = {
        {},
        { { "notes.txt", toBytes("hello") } },
        bundleEntries(writer, "deep/er/", true, true),
    };
    for (const std::vector<Entry>& entries : withoutProject) {
        const std::vector<std::byte> zip = buildStoredZip(entries);
        try {
            ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
            ADD_FAILURE() << "an archive without a project was read";
        } catch (const ProjectBundleError& e) {
            EXPECT_TRUE(containsText(e.what(), "no project.yaml")) << e.what();
        }
    }
}

TEST(ProjectBundle, AStaleManifestLoadsWithOneWarning)
{
    ProjectSerialization changed = makeProject();
    changed.timelineCurrentTime = 99;
    ProjectBundleWriter current;
    current.setProject(changed);
    const ProjectBundleWriter previous = makeWriter("2026-10-22T10:00:00Z");
    BundleManifest stale = previous.manifest();
    stale.members.push_back({ "blobs/gone.bin", 3, 0x12345678 });

    std::vector<Entry> entries = bundleEntries(current, "test/", true, false);
    entries.push_back({ "test/manifest.yaml", toBytes(Serialization::encodeManifestYaml(stale)) });
    const std::vector<std::byte> zip = buildStoredZip(entries);
    ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
    EXPECT_TRUE(reader.hasManifest());
    EXPECT_EQ("user@host", reader.manifest().author);

    YamlWarnings warnings;
    EXPECT_EQ(99, reader.readProject(&warnings).timelineCurrentTime);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_TRUE(containsText(warnings.front(), "manifest.yaml")) << warnings.front();
    EXPECT_TRUE(containsText(warnings.front(), "project.yaml differs")) << warnings.front();
    EXPECT_TRUE(containsText(warnings.front(), "blobs/gone.bin is missing")) << warnings.front();
}

TEST(ProjectBundle, AMissingOrUnreadableManifestGivesDefaults)
{
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    {
        const std::vector<std::byte> zip = buildStoredZip(bundleEntries(writer, "test/", true, false));
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        EXPECT_FALSE(reader.hasManifest());
        EXPECT_EQ(BundleManifest(), reader.manifest());
        YamlWarnings warnings;
        EXPECT_EQ(12, reader.readProject(&warnings).timelineCurrentTime);
        EXPECT_TRUE(warnings.empty());
    }
    {
        std::vector<Entry> entries = bundleEntries(writer, "test/", true, false);
        entries.push_back({ "test/manifest.yaml", toBytes("[not, a, manifest") });
        const std::vector<std::byte> zip = buildStoredZip(entries);
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        EXPECT_FALSE(reader.hasManifest());
        EXPECT_EQ(BundleManifest(), reader.manifest());
        YamlWarnings warnings;
        EXPECT_EQ(12, reader.readProject(&warnings).timelineCurrentTime);
        ASSERT_EQ(1u, warnings.size());
        EXPECT_TRUE(containsText(warnings.front(), "manifest.yaml")) << warnings.front();
    }
    {
        std::vector<Entry> entries = bundleEntries(writer, "test/", true, false);
        entries.push_back({ "test/manifest.yaml", toBytes("format: natron-bundle\nformatVersion: 99\nauthor: someone\nmembers: something new\nfuture: 1\n") });
        const std::vector<std::byte> zip = buildStoredZip(entries);
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        EXPECT_TRUE(reader.hasManifest());
        EXPECT_EQ("someone", reader.manifest().author);
        EXPECT_TRUE(reader.manifest().members.empty());
        YamlWarnings warnings;
        EXPECT_EQ(12, reader.readProject(&warnings).timelineCurrentTime);
        ASSERT_EQ(1u, warnings.size());
        EXPECT_TRUE(containsText(warnings.front(), "newer")) << warnings.front();
    }
}

TEST(ProjectBundle, MembersOutsideTheRootAreIgnoredWithAWarning)
{
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    std::vector<Entry> entries = bundleEntries(writer, "test/", true, true);
    entries.insert(entries.begin(), { "mimetype", toBytes(Serialization::kNatronProjectMimeType) });
    entries.push_back({ "notes.txt", toBytes("hello") });
    entries.push_back({ "other/readme", toBytes("hi") });
    entries.push_back({ "test/unused.txt", toBytes("ignored silently") });
    const std::vector<std::byte> zip = buildStoredZip(entries);
    ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));

    YamlWarnings warnings;
    EXPECT_EQ(12, reader.readProject(&warnings).timelineCurrentTime);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_TRUE(containsText(warnings.front(), "notes.txt")) << warnings.front();
    EXPECT_TRUE(containsText(warnings.front(), "other/readme")) << warnings.front();
    EXPECT_FALSE(containsText(warnings.front(), "mimetype")) << warnings.front();
    EXPECT_FALSE(containsText(warnings.front(), "unused.txt")) << warnings.front();
}

TEST(ProjectBundle, AMemberWithAWrongCrcIsRefused)
{
    const ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    std::vector<Entry> entries = bundleEntries(writer, "test/", true, false);
    for (Entry& e : entries) {
        if (e.name == std::string("test/") + kStrokeBlob) {
            e.crc = Serialization::zipCrc32(e.data) ^ 1u;
        }
    }
    const std::vector<std::byte> zip = buildStoredZip(entries);
    try {
        ProjectBundleReader reader(std::span<const std::byte>(zip.data(), zip.size()));
        ADD_FAILURE() << "a member with a wrong CRC was read";
    } catch (const ProjectBundleError& e) {
        EXPECT_TRUE(containsText(e.what(), "damaged")) << e.what();
    }
}

TEST(ProjectBundle, AnUnzippedFolderLoadsTheSameGraphAsTheArchive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    const std::string archive = (root / "test.ntpz").string();
    ProjectBundleWriter writer = makeWriter("2026-10-22T10:00:00Z");
    writer.setRootFolder("test");
    writer.write(archive);
    {
        ZipReader zip(archive);
        std::vector<Entry> entries;
        for (const Serialization::ZipMember& member : zip.members()) {
            const std::span<const std::byte> data = zip.data(member);
            entries.push_back({ member.name, std::vector<std::byte>(data.begin(), data.end()) });
        }
        writeFolder(root, entries);
    }
    const std::filesystem::path folder = root / "test";

    ProjectBundleReader fromZip(archive);
    const auto expected = reencode(fromZip.readProject());
    for (const std::filesystem::path& path : { folder, folder / "project.yaml" }) {
        ProjectBundleReader fromFolder(path.string());
        EXPECT_EQ("", fromFolder.root());
        EXPECT_TRUE(fromFolder.hasManifest());
        EXPECT_EQ(fromZip.manifest(), fromFolder.manifest());
        const std::optional<std::span<const std::byte>> stroke = fromFolder.findMember(kStrokeBlob);
        ASSERT_TRUE(stroke.has_value());
        EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(stroke->data()) % Serialization::kZipMemberAlignment);
        YamlWarnings warnings;
        const ProjectSerialization loaded = fromFolder.readProject(&warnings);
        EXPECT_TRUE(warnings.empty()) << path.string();
        const auto actual = reencode(loaded);
        EXPECT_EQ(expected.first, actual.first) << path.string();
        EXPECT_TRUE(expected.second == actual.second) << path.string();
    }
    EXPECT_EQ(ProjectFileKind::eDirectory, Serialization::detectProjectFileKind(folder.string()));
    EXPECT_EQ(ProjectFileKind::eProjectYaml, Serialization::detectProjectFileKind((folder / "project.yaml").string()));

    std::filesystem::remove(folder / "mimetype");
    std::filesystem::remove(folder / "manifest.yaml");
    ProjectBundleReader bare(folder.string());
    EXPECT_FALSE(bare.hasManifest());
    EXPECT_EQ(12, bare.readProject().timelineCurrentTime);

    writeFileText((folder / "mimetype").string(), "application/zip");
    EXPECT_THROW(ProjectBundleReader reader(folder.string()), ProjectBundleError);

    const std::filesystem::path empty = root / "empty";
    std::filesystem::create_directory(empty);
    try {
        ProjectBundleReader reader(empty.string());
        ADD_FAILURE() << "a folder without a project was read";
    } catch (const ProjectBundleError& e) {
        EXPECT_TRUE(containsText(e.what(), "no project.yaml")) << e.what();
    }
}
