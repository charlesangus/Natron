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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/mman.h>

#include <gtest/gtest.h>

#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_USING
using Serialization::ZipError;
using Serialization::ZipMember;
using Serialization::ZipReader;
using Serialization::ZipWriter;

namespace {

struct TestMember {
    std::string name;
    std::vector<std::byte> data;
};

std::vector<std::byte>
patternBytes(std::size_t size,
             std::uint32_t seed)
{
    std::vector<std::byte> bytes(size);
    std::uint32_t state = seed * 2654435761u + 1;
    for (std::byte& b : bytes) {
        state = state * 1664525u + 1013904223u;
        b = static_cast<std::byte>(state >> 24);
    }

    return bytes;
}

std::vector<TestMember>
makeMembers(std::size_t count)
{
    static const std::size_t sizes[] = { 0, 1, 63, 64, 65, 1 << 20 };
    std::vector<TestMember> members;
    for (std::size_t i = 0; i < count; ++i) {
        TestMember member;
        member.name = "blobs/node" + std::to_string(i) + ((i % 3 == 0) ? std::string(".bin") : std::string("/curve.bin"));
        member.data = patternBytes(sizes[i % std::size(sizes)], static_cast<std::uint32_t>(i));
        members.push_back(std::move(member));
    }

    return members;
}

void
writeArchive(const std::string& path,
             const std::vector<TestMember>& members)
{
    ZipWriter writer(path);
    for (const TestMember& member : members) {
        writer.addMember(member.name, member.data);
    }
    writer.finish();
}

std::vector<std::byte>
readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());

    return bytes;
}

std::string
asString(std::span<const std::byte> bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void
replaceAll(std::vector<std::byte>& bytes,
           const std::string& from,
           const std::string& to)
{
    ASSERT_EQ(from.size(), to.size());
    auto first = reinterpret_cast<char*>(bytes.data());
    auto last = first + bytes.size();
    int replaced = 0;
    for (auto it = std::search(first, last, from.begin(), from.end()); it != last;
         it = std::search(it + from.size(), last, from.begin(), from.end())) {
        std::memcpy(it, to.data(), to.size());
        ++replaced;
    }
    ASSERT_EQ(replaced, 2) << "expected the name once in the local header and once in the central directory";
}

std::size_t
centralDirectoryOffset(const std::vector<std::byte>& bytes)
{
    std::size_t end = bytes.size() - 22;
    std::uint32_t offset;
    std::memcpy(&offset, bytes.data() + end + 16, 4);

    return offset;
}

QString
findTool(const char* name)
{
    return QStandardPaths::findExecutable(QString::fromUtf8(name));
}

int
runTool(const QString& program,
        const QStringList& args,
        QString* output = nullptr)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    if (!process.waitForFinished(60000)) {
        return -1;
    }
    if (output) {
        *output = QString::fromUtf8(process.readAll());
    }

    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}
} // anon namespace

TEST(ZipStore, RoundTripsMembersOnAlignedOffsets)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (std::size_t count : { 0u, 1u, 50u }) {
        SCOPED_TRACE(count);
        std::string path = dir.filePath(QString::fromUtf8("roundtrip%1.ntpz").arg(count)).toStdString();
        std::vector<TestMember> expected = makeMembers(count);
        writeArchive(path, expected);

        ZipReader reader(path);
        const std::vector<ZipMember>& members = reader.members();
        ASSERT_EQ(members.size(), count + 1);
        EXPECT_EQ(members[0].name, "mimetype");
        EXPECT_EQ(members[0].dataOffset, 38u);
        EXPECT_EQ(asString(reader.data("mimetype")), std::string(Serialization::kNatronProjectMimeType));
        for (std::size_t i = 0; i < count; ++i) {
            const ZipMember& member = members[i + 1];
            EXPECT_EQ(member.name, expected[i].name);
            EXPECT_EQ(member.dataOffset % Serialization::kZipMemberAlignment, 0u) << member.name;
            std::span<const std::byte> data = reader.data(member.name);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(data.data()) % Serialization::kZipMemberAlignment, 0u) << member.name;
            ASSERT_EQ(data.size(), expected[i].data.size()) << member.name;
            EXPECT_TRUE(std::equal(data.begin(), data.end(), expected[i].data.begin())) << member.name;
        }
        EXPECT_NO_THROW(reader.verify());
        EXPECT_FALSE(reader.contains("missing.bin"));
        EXPECT_THROW(reader.data("missing.bin"), ZipError);
    }
}

TEST(ZipStore, OutputIsDeterministic)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::vector<TestMember> members = makeMembers(12);
    std::string first = dir.filePath(QString::fromUtf8("a.ntpz")).toStdString();
    std::string second = dir.filePath(QString::fromUtf8("b.ntpz")).toStdString();
    writeArchive(first, members);
    writeArchive(second, members);
    EXPECT_TRUE(readFile(first) == readFile(second));
}

TEST(ZipStore, ExternalToolsAcceptOutput)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString path = dir.filePath(QString::fromUtf8("external.ntpz"));
    writeArchive(path.toStdString(), makeMembers(50));

    QString python = findTool("python3");
    if (python.isEmpty()) {
        std::cout << "[  SKIPPED ] python3 not found, zipfile check skipped" << std::endl;
    } else {
        QString output;
        int status = runTool(python, { QString::fromUtf8("-I"), QString::fromUtf8("-c"), QString::fromUtf8("import sys, zipfile\n"
                                                                                                           "z = zipfile.ZipFile(sys.argv[1])\n"
                                                                                                           "sys.exit(0 if z.testzip() is None and len(z.namelist()) == 51 else 1)\n"),
                                       path },
                             &output);
        EXPECT_EQ(status, 0) << output.toStdString();
    }

    QString unzip = findTool("unzip");
    if (unzip.isEmpty()) {
        std::cout << "[  SKIPPED ] unzip not found, unzip -t check skipped" << std::endl;
    } else {
        QString output;
        int status = runTool(unzip, { QString::fromUtf8("-tq"), path }, &output);
        EXPECT_EQ(status, 0) << output.toStdString();
    }
}

TEST(ZipStore, AcceptsUnalignedStoredArchiveAndRejectsDeflated)
{
    QString python = findTool("python3");
    if (python.isEmpty()) {
        std::cout << "[  SKIPPED ] python3 not found, foreign archive checks skipped" << std::endl;

        return;
    }
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString stored = dir.filePath(QString::fromUtf8("stored.zip"));
    QString deflated = dir.filePath(QString::fromUtf8("deflated.zip"));
    QString output;
    int status = runTool(python, { QString::fromUtf8("-I"), QString::fromUtf8("-c"), QString::fromUtf8("import sys, zipfile\n"
                                                                                                       "for path, method in ((sys.argv[1], zipfile.ZIP_STORED), (sys.argv[2], zipfile.ZIP_DEFLATED)):\n"
                                                                                                       "    with zipfile.ZipFile(path, 'w', method) as z:\n"
                                                                                                       "        z.writestr('mimetype', 'application/vnd.natron.project+zip')\n"
                                                                                                       "        z.writestr('project.yaml', 'a' * 1000)\n"
                                                                                                       "        z.writestr('blobs/x.bin', bytes(range(256)))\n"),
                                   stored, deflated },
                         &output);
    ASSERT_EQ(status, 0) << output.toStdString();

    ZipReader reader(stored.toStdString());
    EXPECT_NO_THROW(reader.verify());
    EXPECT_EQ(asString(reader.data("project.yaml")), std::string(1000, 'a'));
    std::span<const std::byte> blob = reader.data("blobs/x.bin");
    ASSERT_EQ(blob.size(), 256u);
    for (std::size_t i = 0; i < blob.size(); ++i) {
        EXPECT_EQ(std::to_integer<std::size_t>(blob[i]), i);
    }

    EXPECT_THROW(ZipReader(deflated.toStdString()), ZipError);
}

TEST(ZipStore, WriterRefusesBadNamesAndZip64Sizes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ZipWriter writer(dir.filePath(QString::fromUtf8("names.ntpz")).toStdString());
    std::vector<std::byte> data = patternBytes(10, 1);
    for (const char* name : { "", "../evil", "a/../../evil", "/etc/passwd", "a\\b", "a//b", "./a", "C:/x", "dir/" }) {
        EXPECT_THROW(writer.addMember(name, data), ZipError) << name;
    }
    writer.addMember("ok.bin", data);
    EXPECT_THROW(writer.addMember("ok.bin", data), ZipError);
    EXPECT_THROW(writer.addMember("mimetype", data), ZipError);

    // A reserved, never-touched mapping stands in for a member too large for the classic format;
    // the writer must refuse it before reading any of it.
    const std::size_t hugeSize = std::size_t(1) << 32;
    void* huge = ::mmap(nullptr, hugeSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (huge != MAP_FAILED) {
        EXPECT_THROW(writer.addMember("huge.bin", std::span<const std::byte>(static_cast<const std::byte*>(huge), hugeSize)), ZipError);
        ::munmap(huge, hugeSize);
    } else {
        std::cout << "[  SKIPPED ] cannot reserve 4 GiB of address space, oversize member check skipped" << std::endl;
    }
    writer.finish();
    EXPECT_THROW(writer.addMember("late.bin", data), ZipError);
}

TEST(ZipStore, RejectsMissingAndNonZipFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_THROW(ZipReader(dir.filePath(QString::fromUtf8("absent.ntpz")).toStdString()), ZipError);

    std::string empty = dir.filePath(QString::fromUtf8("empty.ntpz")).toStdString();
    std::ofstream(empty, std::ios::binary).close();
    EXPECT_THROW(ZipReader { empty }, ZipError);

    std::vector<std::byte> text = patternBytes(4096, 7);
    EXPECT_THROW(ZipReader { std::span<const std::byte>(text) }, ZipError);
}

TEST(ZipStore, RejectsEveryTruncation)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath(QString::fromUtf8("small.ntpz")).toStdString();
    writeArchive(path, makeMembers(3));
    std::vector<std::byte> bytes = readFile(path);
    ASSERT_NO_THROW(ZipReader { std::span<const std::byte>(bytes) });

    for (std::size_t size = 0; size < bytes.size(); ++size) {
        std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + size);
        EXPECT_THROW(ZipReader { std::span<const std::byte>(truncated) }, ZipError) << "size " << size;
    }
}

TEST(ZipStore, SurvivesEveryBitFlip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath(QString::fromUtf8("flip.ntpz")).toStdString();
    std::vector<TestMember> members = { { "project.yaml", patternBytes(40, 1) }, { "blobs/a.bin", patternBytes(65, 2) } };
    writeArchive(path, members);
    const std::vector<std::byte> original = readFile(path);

    int rejected = 0;
    int caughtByCrc = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<std::byte> bytes = original;
            bytes[i] ^= static_cast<std::byte>(1 << bit);
            try {
                ZipReader reader { std::span<const std::byte>(bytes) };
                for (const ZipMember& member : reader.members()) {
                    std::span<const std::byte> data = reader.data(member);
                    ASSERT_GE(data.data(), bytes.data());
                    ASSERT_LE(data.data() + data.size(), bytes.data() + bytes.size());
                }
                try {
                    reader.verify();
                } catch (const ZipError&) {
                    ++caughtByCrc;
                }
            } catch (const ZipError&) {
                ++rejected;
            }
        }
    }
    EXPECT_GT(rejected, 0);
    EXPECT_GT(caughtByCrc, 0);
}

TEST(ZipStore, VerifyDetectsCorruptData)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath(QString::fromUtf8("crc.ntpz")).toStdString();
    writeArchive(path, { { "blobs/a.bin", patternBytes(100, 3) } });
    std::vector<std::byte> bytes = readFile(path);
    ZipReader clean { std::span<const std::byte>(bytes) };
    std::size_t offset = clean.find("blobs/a.bin")->dataOffset;
    bytes[offset + 50] ^= std::byte { 0x01 };

    ZipReader reader { std::span<const std::byte>(bytes) };
    EXPECT_THROW(reader.verify(), ZipError);
    EXPECT_THROW(reader.verify("blobs/a.bin"), ZipError);
    EXPECT_NO_THROW(reader.verify("mimetype"));
}

TEST(ZipStore, RejectsHostileNamesAndUnsupportedEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath(QString::fromUtf8("hostile.ntpz")).toStdString();
    writeArchive(path, { { "aa/evil.bin", patternBytes(8, 4) }, { "ab/evil.bin", patternBytes(8, 5) } });
    const std::vector<std::byte> original = readFile(path);
    ASSERT_NO_THROW(ZipReader { std::span<const std::byte>(original) });

    struct Patch {
        const char* from;
        const char* to;
    };
    for (Patch patch : { Patch { "aa/evil.bin", "../evil.bin" }, Patch { "aa/evil.bin", "/a/evil.bin" },
                         Patch { "aa/evil.bin", "a/../il.bin" }, Patch { "aa/evil.bin", "ab/evil.bin" },
                         Patch { "aa/evil.bin", "aa\\evil.bin" } }) {
        std::vector<std::byte> bytes = original;
        replaceAll(bytes, patch.from, patch.to);
        EXPECT_THROW(ZipReader { std::span<const std::byte>(bytes) }, ZipError) << patch.to;
    }

    const std::size_t central = centralDirectoryOffset(original);
    auto patchCentral = [&](std::size_t field, std::vector<std::uint8_t> value) {
        std::vector<std::byte> bytes = original;
        std::memcpy(bytes.data() + central + field, value.data(), value.size());

        return bytes;
    };
    struct FieldPatch {
        const char* what;
        std::size_t field;
        std::vector<std::uint8_t> value;
    };
    for (const FieldPatch& patch : { FieldPatch { "deflated", 10, { 8, 0 } }, FieldPatch { "encrypted", 8, { 1, 0 } },
                                     FieldPatch { "zip64 size", 24, { 0xFF, 0xFF, 0xFF, 0xFF } },
                                     FieldPatch { "size mismatch", 24, { 7, 0, 0, 0 } },
                                     FieldPatch { "local offset past the data", 42, { 0xF0, 0xFF, 0, 0 } } }) {
        std::vector<std::byte> bytes = patchCentral(patch.field, patch.value);
        EXPECT_THROW(ZipReader { std::span<const std::byte>(bytes) }, ZipError) << patch.what;
    }
}
