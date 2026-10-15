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
#include <sys/stat.h>

#include <string>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

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

} // namespace

TEST(FileStamp, UnchangedFileKeepsItsStamp)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");

    FileStampRegistry& registry = FileStampRegistry::instance();
    registry.invalidateAll();
    const U64 first = registry.stamp(path);
    registry.invalidate(path);
    EXPECT_EQ(first, registry.stamp(path));
    EXPECT_NE(0u, first);
}

TEST(FileStamp, ChangedMTimeOrSizeChangesStampAfterInvalidate)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("a.txt")).toStdString();
    writeFile(path, "hello");
    setMTime(path, 1000000);

    FileStampRegistry& registry = FileStampRegistry::instance();
    registry.invalidate(path);
    const U64 original = registry.stamp(path);

    setMTime(path, 2000000);
    EXPECT_EQ(original, registry.stamp(path)); // still within the memo window
    registry.invalidate(path);
    const U64 touched = registry.stamp(path);
    EXPECT_NE(original, touched);

    writeFile(path, "hello, longer content");
    setMTime(path, 2000000);
    registry.invalidate(path);
    EXPECT_NE(touched, registry.stamp(path));
}

TEST(FileStamp, MissingFileIsStableAndDistinctPerPath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string a = dir.filePath(QString::fromUtf8("missing_a")).toStdString();
    const std::string b = dir.filePath(QString::fromUtf8("missing_b")).toStdString();

    FileStampRegistry& registry = FileStampRegistry::instance();
    registry.invalidateAll();
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

TEST_F(BaseTest, FileKnobContentHashFollowsTheFileOnDisk)
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

    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));

    FileStampRegistry::instance().invalidateAll();
    const U64 before = fileKnob->getContentHash();
    FileStampRegistry::instance().invalidateAll();
    EXPECT_EQ(before, fileKnob->getContentHash());

    setMTime(path, 2000000);
    FileStampRegistry::instance().invalidate(path);
    EXPECT_NE(before, fileKnob->getContentHash());

    setMTime(path, 1000000);
    FileStampRegistry::instance().invalidate(path);
    EXPECT_EQ(before, fileKnob->getContentHash());
}

TEST(FileStamp, SequenceStampFollowsItsFrames)
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
    registry.invalidateAll();
    const U64 original = registry.sequenceStamp(pattern);
    EXPECT_NE(0u, original);
    registry.invalidate(pattern);
    EXPECT_EQ(original, registry.sequenceStamp(pattern));

    setMTime(frames[1], 2000000);
    registry.invalidate(pattern);
    const U64 touched = registry.sequenceStamp(pattern);
    EXPECT_NE(original, touched);

    setMTime(frames[1], 1000000);
    registry.invalidate(pattern);
    EXPECT_EQ(original, registry.sequenceStamp(pattern));

    ASSERT_TRUE(QFile::copy(fixture, QString::fromStdString(frames[3])));
    setMTime(frames[3], 1000000);
    registry.invalidateDirectory(dir.path().toStdString());
    const U64 extended = registry.sequenceStamp(pattern);
    EXPECT_NE(original, extended);
    registry.invalidateAll();
    EXPECT_EQ(extended, registry.sequenceStamp(pattern));
}

TEST_F(BaseTest, FileKnobContentHashFollowsASequenceOnDisk)
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

    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));

    FileStampRegistry::instance().invalidateAll();
    const U64 before = fileKnob->getContentHash();
    EXPECT_EQ(before, fileKnob->getContentHash());

    setMTime(frames[2], 2000000);
    fileKnob->invalidateFileStamp();
    EXPECT_NE(before, fileKnob->getContentHash());

    setMTime(frames[2], 1000000);
    fileKnob->invalidateFileStamp();
    EXPECT_EQ(before, fileKnob->getContentHash());
}
