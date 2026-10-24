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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectEquivalence.h"

#include "Engine/AppInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/Yaml/ManifestYaml.h"

NATRON_NAMESPACE_USING

namespace {

struct RawEntry {
    std::string name;
    std::vector<std::byte> data;
    bool deflated = false;
};

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
putString(std::vector<std::byte>& out,
          const std::string& s)
{
    const std::byte* bytes = reinterpret_cast<const std::byte*>(s.data());
    out.insert(out.end(), bytes, bytes + s.size());
}

std::vector<std::byte>
rawDeflate(const std::vector<std::byte>& data)
{
    z_stream strm {};
    EXPECT_EQ(deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
    std::vector<std::byte> out(deflateBound(&strm, static_cast<uLong>(data.size())) + 16);
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(data.data()));
    strm.avail_in = static_cast<uInt>(data.size());
    strm.next_out = reinterpret_cast<Bytef*>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    EXPECT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
    out.resize(strm.total_out);
    deflateEnd(&strm);

    return out;
}

// A zip archive with the entries in the given order, as a general-purpose tool writes one.
// An entry whose name ends in '/' is a directory entry.
std::vector<std::byte>
buildZip(const std::vector<RawEntry>& entries)
{
    struct Placed {
        std::uint16_t method;
        std::uint32_t crc;
        std::uint32_t size;
        std::uint32_t offset;
        std::vector<std::byte> payload;
    };
    std::vector<Placed> placed;
    std::vector<std::byte> out;
    for (const RawEntry& entry : entries) {
        Placed p;
        p.method = (entry.deflated && !entry.data.empty()) ? 8 : 0;
        p.crc = Serialization::zipCrc32(entry.data);
        p.size = static_cast<std::uint32_t>(entry.data.size());
        p.offset = static_cast<std::uint32_t>(out.size());
        p.payload = (p.method == 8) ? rawDeflate(entry.data) : entry.data;
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, p.method);
        put16(out, 0);
        put16(out, 33);
        put32(out, p.crc);
        put32(out, static_cast<std::uint32_t>(p.payload.size()));
        put32(out, p.size);
        put16(out, static_cast<std::uint32_t>(entry.name.size()));
        put16(out, 0);
        putString(out, entry.name);
        out.insert(out.end(), p.payload.begin(), p.payload.end());
        placed.push_back(std::move(p));
    }
    const std::uint32_t directoryOffset = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Placed& p = placed[i];
        put32(out, 0x02014b50);
        put16(out, 20);
        put16(out, 20);
        put16(out, 0);
        put16(out, p.method);
        put16(out, 0);
        put16(out, 33);
        put32(out, p.crc);
        put32(out, static_cast<std::uint32_t>(p.payload.size()));
        put32(out, p.size);
        put16(out, static_cast<std::uint32_t>(entries[i].name.size()));
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, p.offset);
        putString(out, entries[i].name);
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

bool
writeBytes(const std::string& path,
           const void* bytes,
           std::size_t size)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char*>(bytes), (std::streamsize)size);
    if (!file) {
        ADD_FAILURE() << "Cannot write " << path;

        return false;
    }

    return true;
}

bool
writeText(const std::string& path,
          const std::string& text)
{
    return writeBytes(path, text.data(), text.size());
}

std::string
joined(const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines) {
        text += "\n  " + line;
    }

    return text;
}

// Dialogs::errorDialog writes to std::cerr in background mode and an AppInstance without a GUI
// writes to std::cout, so both are taken.
class ScopedOutputCapture {
public:
    ScopedOutputCapture()
        : _text()
        , _oldOut(std::cout.rdbuf(_text.rdbuf()))
        , _oldErr(std::cerr.rdbuf(_text.rdbuf()))
    {
    }

    ~ScopedOutputCapture()
    {
        std::cout.rdbuf(_oldOut);
        std::cerr.rdbuf(_oldErr);
    }

    std::string text() const
    {
        return _text.str();
    }

private:
    std::ostringstream _text;
    std::streambuf* _oldOut;
    std::streambuf* _oldErr;
};

} // anonymous namespace

class ProjectOpenTest
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

    // Builds a generator -> writer graph and saves it as <stem>.ntpz in dir; the snapshot of
    // the graph that was saved goes to expected.
    QString saveGraph(const QTemporaryDir& dir,
                      const QString& stem,
                      ProjectSnapshot* expected)
    {
        NodePtr generator = createNode(_generatorPluginID);
        NodePtr writer = createNativeWriter();
        EXPECT_TRUE(bool(generator) && bool(writer));
        connectNodes(generator, writer, 0, true);
        *expected = captureProjectSnapshot(getApp()->getProject());

        QString savedPath;
        EXPECT_TRUE(getApp()->getProject()->saveProject(dir.path() + QLatin1Char('/'), stem + QString::fromUtf8(".ntpz"), &savedPath));

        return savedPath;
    }

    // Resets, then opens target the way File>Open and the CLI do; output receives what the open
    // printed, where a refusal's message goes.
    bool open(const QString& target,
              std::string* output)
    {
        const QFileInfo info(target);
        getApp()->getProject()->reset(false, true);
        ScopedOutputCapture capture;
        const bool loaded = getApp()->getProject()->loadProject(info.absolutePath() + QLatin1Char('/'), info.fileName());
        *output = capture.text();

        return loaded;
    }

    std::vector<std::string> loadWarnings() const
    {
        std::vector<std::string> warnings;
        const QStringList list = getApp()->getProject()->getLoadWarnings();
        for (const QString& warning : list) {
            warnings.push_back(warning.toStdString());
        }

        return warnings;
    }

    // Opens target and expects the graph in expected, with the given number of load warnings.
    void expectOpensAs(const QString& target,
                       const ProjectSnapshot& expected,
                       std::size_t warningCount)
    {
        SCOPED_TRACE(target.toStdString());
        std::string output;
        ASSERT_TRUE(open(target, &output)) << output;
        EXPECT_EQ(warningCount, loadWarnings().size()) << joined(loadWarnings());
        EXPECT_TRUE(projectsAreEquivalent(expected, captureProjectSnapshot(getApp()->getProject())));
    }

    // The members of the saved bundle at path, in archive order, with names as saved.
    static std::vector<RawEntry> savedMembers(const QString& path)
    {
        std::vector<RawEntry> entries;
        try {
            Serialization::ZipReader source(path.toStdString());
            for (const Serialization::ZipMember& member : source.members()) {
                const std::span<const std::byte> data = source.data(member);
                RawEntry entry;
                entry.name = member.name;
                entry.data.assign(data.begin(), data.end());
                entries.push_back(std::move(entry));
            }
        } catch (const std::exception& e) {
            ADD_FAILURE() << "Cannot read " << path.toStdString() << ": " << e.what();
        }

        return entries;
    }

    static std::string withoutFirstFolder(const std::string& name)
    {
        return name.substr(name.find('/') + 1);
    }
};

TEST_F(ProjectOpenTest, ASavedProjectSitsUnderItsStemAndOpensBack)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectSnapshot expected;
    const QString saved = saveGraph(tmp, QString::fromUtf8("test"), &expected);

    const std::vector<RawEntry> members = savedMembers(saved);
    ASSERT_FALSE(members.empty());
    for (const RawEntry& member : members) {
        EXPECT_EQ(0u, member.name.rfind("test/", 0)) << member.name;
    }

    expectOpensAs(saved, expected, 0);
}

TEST_F(ProjectOpenTest, ADeflatedRezipWithoutMimetypeAtTheRootAndAStaleManifestWarnsOnce)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectSnapshot expected;
    const QString saved = saveGraph(tmp, QString::fromUtf8("test"), &expected);

    Serialization::BundleManifest stale;
    stale.members.push_back({ "blobs/gone.bin", 3, 0x12345678 });
    const std::string staleManifest = Serialization::encodeManifestYaml(stale);

    std::vector<RawEntry> rezipped;
    for (RawEntry& member : savedMembers(saved)) {
        const std::string relative = withoutFirstFolder(member.name);
        if (relative == "mimetype") {
            continue;
        }
        member.name = relative;
        member.deflated = true;
        if (relative == "manifest.yaml") {
            const std::byte* bytes = reinterpret_cast<const std::byte*>(staleManifest.data());
            member.data.assign(bytes, bytes + staleManifest.size());
        }
        rezipped.push_back(std::move(member));
    }
    const std::vector<std::byte> zip = buildZip(rezipped);
    const QString path = tmp.filePath(QString::fromUtf8("rezipped.ntpz"));
    ASSERT_TRUE(writeBytes(path.toStdString(), zip.data(), zip.size()));

    expectOpensAs(path, expected, 1);
    const std::vector<std::string> warnings = loadWarnings();
    ASSERT_EQ(1u, warnings.size());
    EXPECT_NE(std::string::npos, warnings.front().find("manifest.yaml")) << warnings.front();
}

TEST_F(ProjectOpenTest, AZipWithDirectoryEntriesOpensAlsoRenamed)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectSnapshot expected;
    const QString saved = saveGraph(tmp, QString::fromUtf8("test"), &expected);

    // What `zip -r test.ntpz test/` writes: the folder entry, then its files, deflated.
    std::vector<RawEntry> entries;
    RawEntry folder;
    folder.name = "test/";
    entries.push_back(folder);
    for (RawEntry& member : savedMembers(saved)) {
        member.deflated = true;
        entries.push_back(std::move(member));
    }
    const std::vector<std::byte> zip = buildZip(entries);
    const QString zipped = tmp.filePath(QString::fromUtf8("zipped.ntpz"));
    ASSERT_TRUE(writeBytes(zipped.toStdString(), zip.data(), zip.size()));
    expectOpensAs(zipped, expected, 0);

    const QString renamed = tmp.filePath(QString::fromUtf8("other-name.bin"));
    ASSERT_TRUE(QFile::rename(zipped, renamed));
    expectOpensAs(renamed, expected, 0);
}

TEST_F(ProjectOpenTest, AFolderAndItsProjectYamlOpenLikeTheArchive)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectSnapshot expected;
    const QString saved = saveGraph(tmp, QString::fromUtf8("test"), &expected);

    const std::filesystem::path unzipped = std::filesystem::path(tmp.path().toStdString()) / "unzipped";
    for (const RawEntry& member : savedMembers(saved)) {
        const std::filesystem::path file = unzipped / member.name;
        std::filesystem::create_directories(file.parent_path());
        ASSERT_TRUE(writeBytes(file.string(), member.data.data(), member.data.size()));
    }
    const std::filesystem::path folder = unzipped / "test";

    expectOpensAs(QString::fromStdString(folder.string()), expected, 0);
    expectOpensAs(QString::fromStdString((folder / "project.yaml").string()), expected, 0);
}

TEST_F(ProjectOpenTest, ALegacyXmlAndANonProjectAreRefusedBeforeAnyReset)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectPtr project = getApp()->getProject();
    NodePtr probe = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(probe));
    const std::string probeName = probe->getScriptName();

    const QString legacy = tmp.filePath(QString::fromUtf8("old.ntp"));
    ASSERT_TRUE(writeText(legacy.toStdString(), "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
                                                "<!DOCTYPE boost_serialization>\n"
                                                "<boost_serialization signature=\"serialization::archive\" version=\"20\">\n"
                                                "</boost_serialization>\n"));
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(56, '\0');
    const QString image = tmp.filePath(QString::fromUtf8("image.png"));
    ASSERT_TRUE(writeBytes(image.toStdString(), png.data(), png.size()));

    const struct {
        QString path;
        std::string expected;
    } refused[] = {
        { legacy, "Legacy .ntp projects are not supported" },
        { image, "image.png is not a " NATRON_APPLICATION_NAME " project" },
    };
    for (const auto& refusal : refused) {
        SCOPED_TRACE(refusal.path.toStdString());
        const QFileInfo info(refusal.path);
        ScopedOutputCapture capture;
        const bool loaded = project->loadProject(info.absolutePath() + QLatin1Char('/'), info.fileName());
        const std::string output = capture.text();
        EXPECT_FALSE(loaded);
        EXPECT_NE(std::string::npos, output.find(refusal.expected)) << output;
        EXPECT_EQ(probe, project->getNodeByName(probeName));
    }
}
