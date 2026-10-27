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

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

void
writeFile(const std::string& path,
          const std::string& content)
{
    QFile f(QString::fromStdString(path));

    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.c_str(), (qint64)content.size());
}

void
setMTime(const std::string& path,
         time_t seconds)
{
    struct timespec times[2];

    times[0].tv_sec = seconds;
    times[0].tv_nsec = 0;
    times[1].tv_sec = seconds;
    times[1].tv_nsec = 0;
    ASSERT_EQ(0, utimensat(AT_FDCWD, path.c_str(), times, 0));
}

bool
writeFlatExr(const std::string& path,
             int width,
             int height,
             float value)
{
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);

    spec.channelnames = { "R", "G", "B", "A" };
    spec.alpha_channel = 3;
    std::vector<float> pixels((std::size_t)width * height * 4, value);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 1.f;
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

bool
renderedRed(const NodePtr& node,
            double time,
            int width,
            int height,
            float* red)
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesDirect(node, time, ViewIdx(0), 0, RectI(0, 0, width, height), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, false) || (planes.size() != 1) || planes[0].pixels.empty()) {
        ADD_FAILURE() << error;

        return false;
    }
    *red = planes[0].pixels[0];

    return true;
}

KnobFilePtr
fileKnobOf(const NodePtr& reader)
{
    return std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
}

void
pressReload(const NodePtr& reader)
{
    KnobButton* reload = dynamic_cast<KnobButton*>(reader->getKnobByName(kNatronReadParamReload).get());

    ASSERT_TRUE(reload != NULL);
    reload->trigger();
}

} // namespace

TEST(FileStamp, UnchangedFileKeepsItsStamp)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 first = registry.stamp(path);
    registry.invalidate(path);
    EXPECT_EQ(first, registry.stamp(path));
    EXPECT_NE(0u, first);
}

TEST(FileStamp, ChangedFileKeepsItsStampUntilInvalidated)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");
    setMTime(path, 1000000);

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 original = registry.stamp(path);

    setMTime(path, 2000000);
    EXPECT_EQ(original, registry.stamp(path));
    registry.invalidate(path);
    const U64 touched = registry.stamp(path);
    EXPECT_NE(original, touched);

    writeFile(path, "hello, longer content");
    setMTime(path, 2000000);
    EXPECT_EQ(touched, registry.stamp(path));
    registry.invalidate(path);
    EXPECT_NE(touched, registry.stamp(path));
}

TEST(FileStamp, AStampedFileIsNotLookedAtAgain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 first = registry.stamp(path);
    const std::uint64_t accesses = registry.diskAccessCount();
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(first, registry.stamp(path));
    }
    EXPECT_EQ(accesses, registry.diskAccessCount());

    registry.invalidate(path);
    EXPECT_EQ(first, registry.stamp(path));
    EXPECT_EQ(accesses + 1, registry.diskAccessCount());
}

TEST(FileStamp, MissingFileIsStableAndDistinctPerPath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string a = dir.filePath(QString::fromUtf8("missing_a")).toStdString();
    const std::string b = dir.filePath(QString::fromUtf8("missing_b")).toStdString();

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 stampA = registry.stamp(a);
    registry.invalidate(a);
    EXPECT_EQ(stampA, registry.stamp(a));
    EXPECT_NE(stampA, registry.stamp(b));
}

TEST(FileStamp, DetectsFramePatterns)
{
    EXPECT_TRUE(FileStampRegistry::hasFramePattern("/tmp/img.####.exr"));
    EXPECT_TRUE(FileStampRegistry::hasFramePattern("/tmp/img.%04d.exr"));
    EXPECT_TRUE(FileStampRegistry::hasFramePattern("/tmp/img_%v.exr"));
    EXPECT_FALSE(FileStampRegistry::hasFramePattern("/tmp/img.exr"));
    EXPECT_FALSE(FileStampRegistry::hasFramePattern("/tmp/100%.exr"));
}

TEST(FileStamp, IdentityIsKeptUntilInvalidated)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");
    setMTime(path, 1000000);

    FileStampRegistry& registry = FileStampRegistry::instance();
    const FileIdentity first = registry.identity(path);
    EXPECT_TRUE(first.exists);
    EXPECT_EQ(5u, first.size);
    EXPECT_EQ(1000000, first.mtimeSec);
    EXPECT_TRUE(registry.matchesDisk(path));

    writeFile(path, "hello, longer content");
    EXPECT_TRUE(first == registry.identity(path));
    registry.invalidate(path);
    EXPECT_FALSE(first == registry.identity(path));
    EXPECT_TRUE(registry.matchesDisk(path));
}

TEST(FileStamp, AnInPlaceRewriteFailsMatchesDisk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");
    setMTime(path, 1000000);

    FileStampRegistry& registry = FileStampRegistry::instance();
    FileIdentity recorded;
    ASSERT_TRUE(registry.matchesDisk(path, &recorded));
    EXPECT_TRUE(recorded == registry.identity(path));

    writeFile(path, "hello, longer content");
    setMTime(path, 1000000);
    EXPECT_FALSE(registry.matchesDisk(path));
    EXPECT_TRUE(recorded == registry.identity(path));
}

TEST(FileStamp, ARenameReplaceFailsMatchesDisk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    const std::string replacement = dir.filePath(QString::fromUtf8("b.txt")).toStdString();
    writeFile(path, "hello");
    setMTime(path, 1000000);

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 stamp = registry.stamp(path);
    ASSERT_TRUE(registry.matchesDisk(path));

    // Same size and modification time: only the inode tells the files apart.
    writeFile(replacement, "jello");
    setMTime(replacement, 1000000);
    ASSERT_EQ(0, ::rename(replacement.c_str(), path.c_str()));
    EXPECT_FALSE(registry.matchesDisk(path));
    EXPECT_EQ(stamp, registry.stamp(path));
    registry.invalidate(path);
    EXPECT_NE(stamp, registry.stamp(path));
    EXPECT_TRUE(registry.matchesDisk(path));
}

TEST(FileStamp, ASequenceRecordsTheIdentityOfEachFrame)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string frames[2];
    for (int i = 0; i < 2; ++i) {
        frames[i] = dir.filePath(QString::fromUtf8("seq.%1.txt").arg(i + 1, 4, 10, QLatin1Char('0'))).toStdString();
        writeFile(frames[i], i == 0 ? "one" : "three");
        setMTime(frames[i], 1000000);
    }
    const std::string pattern = dir.filePath(QString::fromUtf8("seq.####.txt")).toStdString();

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 stamp = registry.sequenceStamp(pattern);
    const std::uint64_t accesses = registry.diskAccessCount();
    EXPECT_EQ(3u, registry.identity(frames[0]).size);
    EXPECT_EQ(5u, registry.identity(frames[1]).size);
    EXPECT_EQ(accesses, registry.diskAccessCount());

    writeFile(frames[1], "three, rewritten");
    EXPECT_TRUE(registry.matchesDisk(frames[0]));
    EXPECT_FALSE(registry.matchesDisk(frames[1]));
    EXPECT_EQ(stamp, registry.sequenceStamp(pattern));
    registry.invalidate(pattern);
    EXPECT_NE(stamp, registry.sequenceStamp(pattern));
    EXPECT_TRUE(registry.matchesDisk(frames[1]));
}

TEST(FileStamp, OiioReaderFailsOnARewrittenFileUntilReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("pinned.exr")).toStdString();
    ASSERT_TRUE(writeFlatExr(path, 16, 12, 0.25f));

    int parses = 0;
    OiioReadSupport::setHeaderParsedHookForTests([&parses](const std::string&) {
        ++parses;
    });
    std::string error;
    std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(1u, header->subimages.size());
    EXPECT_EQ(16, header->subimages[0].width);
    const RectI window(0, 0, 4, 4);
    std::vector<float> pixels((std::size_t)window.area() * 4, 0.f);
    ASSERT_TRUE(OiioReadSupport::decode(path, 0, 0, 4, window, pixels.data(), (std::size_t)window.width() * 4, &error)) << error;
    EXPECT_FLOAT_EQ(0.25f, pixels[0]);
    EXPECT_EQ(1, parses);

    ASSERT_TRUE(writeFlatExr(path, 20, 10, 0.75f));
    const std::string changed = FileStampRegistry::changedOnDiskMessage(path);
    error.clear();
    EXPECT_FALSE(bool(OiioReadSupport::readHeader(path, &error)));
    EXPECT_EQ(changed, error);
    error.clear();
    EXPECT_FALSE(OiioReadSupport::decode(path, 0, 0, 4, window, pixels.data(), (std::size_t)window.width() * 4, &error));
    EXPECT_EQ(changed, error);
    EXPECT_EQ(1, parses);

    // Reload: the stamp and the header go, and the new file comes in.
    FileStampRegistry::instance().invalidate(path);
    OiioReadSupport::evictHeaders(std::vector<std::string>(1, path));
    error.clear();
    header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    EXPECT_EQ(20, header->subimages[0].width);
    ASSERT_TRUE(OiioReadSupport::decode(path, 0, 0, 4, window, pixels.data(), (std::size_t)window.width() * 4, &error)) << error;
    EXPECT_FLOAT_EQ(0.75f, pixels[0]);
    EXPECT_EQ(2, parses);
    OiioReadSupport::setHeaderParsedHookForTests(std::function<void(const std::string&)>());
}

TEST_F(BaseTest, FileKnobContentHashFollowsTheFileOnlyOnReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("copy.exr")).toStdString();
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"), QString::fromStdString(path)));
    setMTime(path, 1000000);

    CreateNodeArgs readerArgs(_readPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, path);
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader));

    KnobFilePtr fileKnob = fileKnobOf(reader);
    ASSERT_TRUE(bool(fileKnob));

    const U64 before = fileKnob->getContentHash();
    fileKnob->reloadFile();
    EXPECT_EQ(before, fileKnob->getContentHash());

    setMTime(path, 2000000);
    EXPECT_EQ(before, fileKnob->getContentHash());
    fileKnob->reloadFile();
    EXPECT_NE(before, fileKnob->getContentHash());

    setMTime(path, 1000000);
    fileKnob->reloadFile();
    EXPECT_EQ(before, fileKnob->getContentHash());
}

TEST(FileStamp, SequenceStampFollowsItsFramesOnInvalidate)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString fixture = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr");
    std::string frames[4];
    for (int i = 0; i < 4; ++i) {
        frames[i] = dir.filePath(QString::fromUtf8("name.%1.exr").arg(i + 1, 4, 10, QLatin1Char('0'))).toStdString();
    }
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(QFile::copy(fixture, QString::fromStdString(frames[i])));
        setMTime(frames[i], 1000000);
    }
    const std::string pattern = dir.filePath(QString::fromUtf8("name.####.exr")).toStdString();

    FileStampRegistry& registry = FileStampRegistry::instance();
    const U64 original = registry.sequenceStamp(pattern);
    EXPECT_NE(0u, original);
    registry.invalidate(pattern);
    EXPECT_EQ(original, registry.sequenceStamp(pattern));

    setMTime(frames[1], 2000000);
    EXPECT_EQ(original, registry.sequenceStamp(pattern));
    registry.invalidate(pattern);
    const U64 touched = registry.sequenceStamp(pattern);
    EXPECT_NE(original, touched);

    // Invalidating one frame also drops the sequence it belongs to.
    setMTime(frames[1], 1000000);
    registry.invalidate(frames[1]);
    EXPECT_EQ(original, registry.sequenceStamp(pattern));

    ASSERT_TRUE(QFile::copy(fixture, QString::fromStdString(frames[3])));
    setMTime(frames[3], 1000000);
    EXPECT_EQ(original, registry.sequenceStamp(pattern));
    registry.invalidate(pattern);
    const U64 extended = registry.sequenceStamp(pattern);
    EXPECT_NE(original, extended);
    EXPECT_EQ(extended, registry.sequenceStamp(pattern));
}

TEST_F(BaseTest, FileKnobContentHashFollowsASequenceOnlyOnReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString fixture = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr");
    std::string frames[3];
    for (int i = 0; i < 3; ++i) {
        frames[i] = dir.filePath(QString::fromUtf8("seq.%1.exr").arg(i + 1, 4, 10, QLatin1Char('0'))).toStdString();
        ASSERT_TRUE(QFile::copy(fixture, QString::fromStdString(frames[i])));
        setMTime(frames[i], 1000000);
    }
    const std::string pattern = dir.filePath(QString::fromUtf8("seq.####.exr")).toStdString();

    CreateNodeArgs readerArgs(_readPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, pattern);
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader));

    KnobFilePtr fileKnob = fileKnobOf(reader);
    ASSERT_TRUE(bool(fileKnob));

    const U64 before = fileKnob->getContentHash();
    EXPECT_EQ(before, fileKnob->getContentHash());

    setMTime(frames[2], 2000000);
    EXPECT_EQ(before, fileKnob->getContentHash());
    fileKnob->reloadFile();
    EXPECT_NE(before, fileKnob->getContentHash());

    setMTime(frames[2], 1000000);
    fileKnob->reloadFile();
    EXPECT_EQ(before, fileKnob->getContentHash());
}

TEST_F(BaseTest, ReadKeepsAFileOverwrittenInPlaceUntilReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("inplace.exr")).toStdString();
    const int width = 16;
    const int height = 12;
    ASSERT_TRUE(writeFlatExr(path, width, height, 0.25f));
    setMTime(path, 1000000);

    NodePtr reader = createWorkingSpaceRead(path);
    ASSERT_TRUE(bool(reader));
    // A Read with no output does not cache what it renders, and the reader refuses the rewritten
    // file; only an image cached under the unchanged hash keeps answering until Reload.
    KnobBoolPtr forceCaching = std::dynamic_pointer_cast<KnobBool>(reader->getKnobByName("forceCaching"));
    ASSERT_TRUE(bool(forceCaching));
    forceCaching->setValue(true);

    float red = 0.f;
    ASSERT_TRUE(renderedRed(reader, 1., width, height, &red));
    EXPECT_FLOAT_EQ(0.25f, red);
    const U64 before = reader->getHashValue();

    ASSERT_TRUE(writeFlatExr(path, width, height, 0.75f));
    setMTime(path, 2000000);

    EXPECT_EQ(before, reader->getHashValue());
    ASSERT_TRUE(renderedRed(reader, 1., width, height, &red));
    EXPECT_FLOAT_EQ(0.25f, red);

    pressReload(reader);
    EXPECT_NE(before, reader->getHashValue());
    ASSERT_TRUE(renderedRed(reader, 1., width, height, &red));
    EXPECT_FLOAT_EQ(0.75f, red);
}

TEST_F(BaseTest, ReloadOnOneReadRekeysEveryReadOfTheSameFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("shared.exr")).toStdString();
    const int width = 16;
    const int height = 12;
    ASSERT_TRUE(writeFlatExr(path, width, height, 0.25f));
    setMTime(path, 1000000);

    NodePtr first = createWorkingSpaceRead(path);
    NodePtr second = createWorkingSpaceRead(path);
    ASSERT_TRUE(bool(first));
    ASSERT_TRUE(bool(second));

    float red = 0.f;
    ASSERT_TRUE(renderedRed(second, 1., width, height, &red));
    EXPECT_FLOAT_EQ(0.25f, red);
    const U64 firstBefore = first->getHashValue();
    const U64 secondBefore = second->getHashValue();

    ASSERT_TRUE(writeFlatExr(path, width, height, 0.75f));
    setMTime(path, 2000000);
    EXPECT_EQ(secondBefore, second->getHashValue());

    pressReload(first);
    EXPECT_NE(firstBefore, first->getHashValue());
    EXPECT_NE(secondBefore, second->getHashValue());
    ASSERT_TRUE(renderedRed(second, 1., width, height, &red));
    EXPECT_FLOAT_EQ(0.75f, red);
}

TEST_F(BaseTest, RenderingASequenceLooksAtNoFileAfterTheFirstHash)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const int width = 16;
    const int height = 12;
    const int frameCount = 5;
    for (int i = 1; i <= frameCount; ++i) {
        const std::string frame = dir.filePath(QString::fromUtf8("render.%1.exr").arg(i, 4, 10, QLatin1Char('0'))).toStdString();
        ASSERT_TRUE(writeFlatExr(frame, width, height, 0.125f * i));
    }
    const std::string pattern = dir.filePath(QString::fromUtf8("render.####.exr")).toStdString();

    NodePtr reader = createWorkingSpaceRead(pattern);
    ASSERT_TRUE(bool(reader));
    const U64 hash = reader->getHashValue();

    FileStampRegistry& registry = FileStampRegistry::instance();
    const std::uint64_t accesses = registry.diskAccessCount();
    for (int i = 1; i <= frameCount; ++i) {
        float red = 0.f;
        ASSERT_TRUE(renderedRed(reader, i, width, height, &red));
        EXPECT_FLOAT_EQ(0.125f * i, red);
    }
    EXPECT_EQ(hash, reader->getHashValue());
    EXPECT_EQ(accesses, registry.diskAccessCount());
}
