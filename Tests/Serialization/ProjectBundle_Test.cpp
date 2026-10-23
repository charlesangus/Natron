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

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/ManifestYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_USING
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

} // anonymous namespace

TEST(ProjectBundle, MembersAreWrittenInTheirFixedOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("order.ntpz")).toStdString();
    makeWriter("2026-10-22T10:00:00Z").write(path);

    ZipReader archive(path);
    std::vector<std::string> names;
    for (const Serialization::ZipMember& member : archive.members()) {
        names.push_back(member.name);
    }
    const std::vector<std::string> expected = { "mimetype", "manifest.yaml", "project.yaml", kStrokeBlob, kTrackBlob };
    EXPECT_EQ(expected, names);
    EXPECT_EQ(std::string(Serialization::kNatronProjectMimeType), asText(archive.data("mimetype")));

    const BundleManifest manifest = Serialization::decodeManifestYaml(asText(archive.data("manifest.yaml")));
    ASSERT_EQ(3u, manifest.members.size());
    for (const Serialization::BundleManifestMember& listed : manifest.members) {
        const Serialization::ZipMember* member = archive.find(listed.name);
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
    EXPECT_TRUE(reader.archive().contains(kStrokeBlob));
    EXPECT_TRUE(reader.archive().contains(kTrackBlob));
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
        ZipWriter copy(damaged);
        for (const Serialization::ZipMember& member : source.members()) {
            if ((member.name != "mimetype") && (member.name != kStrokeBlob)) {
                copy.addMember(member.name, source.data(member));
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
        ASSERT_EQ(ma.name, mb.name);
        if (ma.name == "manifest.yaml") {
            EXPECT_NE(asText(a.data(ma)), asText(b.data(mb)));
        } else {
            EXPECT_EQ(asText(a.data(ma)), asText(b.data(mb))) << ma.name;
        }
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

    const std::string noManifest = dir.filePath(QString::fromUtf8("nomanifest.ntpz")).toStdString();
    {
        ZipWriter writer(noManifest);
        writer.addMember("project.yaml", asBytes("format: natron-project\nformatVersion: 1\n"));
        writer.finish();
    }
    EXPECT_THROW(ProjectBundleReader reader(noManifest), ProjectBundleError);

    const std::string newer = dir.filePath(QString::fromUtf8("newer.ntpz")).toStdString();
    {
        ZipWriter writer(newer);
        writer.addMember("manifest.yaml", asBytes("format: natron-bundle\nformatVersion: 99\n"));
        writer.addMember("project.yaml", asBytes("format: natron-project\nformatVersion: 1\n"));
        writer.finish();
    }
    try {
        ProjectBundleReader reader(newer);
        ADD_FAILURE() << "a bundle from a newer Natron was read";
    } catch (const ProjectBundleError& e) {
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
    EXPECT_EQ(ProjectFileKind::eUnknown, Serialization::detectProjectFileKind(otherZip));

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
