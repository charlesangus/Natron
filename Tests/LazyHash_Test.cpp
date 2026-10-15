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

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

// Hashes are recomputed when read, after any edit marked them dirty. None of these tests refreshes
// a hash itself, so each one fails if an edit leaves a hash it changes unmarked.

namespace {

KnobDoublePtr
blurSize(const NodePtr& blur)
{
    return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
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

} // namespace

class LazyHashTest
    : public BaseTest {
protected:
    NodePtr createConstant()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    }

    NodePtr createBlur()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    }
};

// The edit and the read both happen off the main thread, and the main thread processes no event
// in between, so nothing but the mark can make the read see the edit.
TEST_F(LazyHashTest, KnobSetOnAWorkerIsSeenByAReadOnAnotherWorker)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    NodePtr downstream = createBlur();
    ASSERT_TRUE(constant && blur && downstream);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));
    const double original = size->getValue(0);
    const U64 before = blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    std::thread editor([&size, original]() {
        size->setValue(original + 3., ViewSpec::all(), 0);
    });
    editor.join();

    U64 afterOnWorker = 0;
    U64 downstreamAfterOnWorker = 0;
    std::thread reader([&]() {
        downstreamAfterOnWorker = downstream->getHashValue();
        afterOnWorker = blur->getHashValue();
    });
    reader.join();
    EXPECT_NE(before, afterOnWorker);
    EXPECT_NE(downstreamBefore, downstreamAfterOnWorker);
    EXPECT_EQ(afterOnWorker, blur->getHashValue());
    EXPECT_EQ(downstreamAfterOnWorker, downstream->getHashValue());

    QCoreApplication::processEvents();
    size->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());
}

// A persistent knob that does not evaluate on change still renders differently, yet its edits
// bump no knobs age.
TEST_F(LazyHashTest, PersistentKnobThatDoesNotEvaluateOnChangeChangesTheHash)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    NodePtr downstream = createBlur();
    ASSERT_TRUE(constant && blur && downstream);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));
    size->setEvaluateOnChange(false);
    ASSERT_FALSE(size->getEvaluateOnChange());
    ASSERT_TRUE(size->isRenderHashed());

    const double original = size->getValue(0);
    const U64 before = blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    size->setValue(original + 2., ViewSpec::all(), 0);
    EXPECT_NE(before, blur->getHashValue());
    EXPECT_NE(downstreamBefore, downstream->getHashValue());

    size->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());
}

// Roto geometry lives in the context's items, not in knobs. The engine has no undo stack without
// the GUI, so undo is played by moving the point back.
TEST_F(LazyHashTest, RotoPointMoveChangesTheRotoAndDownstreamHashesAndUndoRestoresThem)
{
    getApp()->getProject()->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    NodePtr blur = createBlur();
    ASSERT_TRUE(roto && blur);
    connectNodes(roto, blur, 0, true);
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    const double time = context->getTimelineCurrentTime();
    // Keyed at the current time: movePointByIndex() ignores a point with no keyframe there.
    BezierPtr square = context->makeSquare(1., 6., 4., time);
    ASSERT_TRUE(bool(square));

    const U64 rotoBefore = roto->getHashValue();
    const U64 blurBefore = blur->getHashValue();
    EXPECT_NE(U64(0), rotoBefore);

    square->movePointByIndex(0, time, 2., 3.);
    const U64 rotoMoved = roto->getHashValue();
    EXPECT_NE(rotoBefore, rotoMoved);
    EXPECT_NE(blurBefore, blur->getHashValue());

    square->movePointByIndex(0, time, -2., -3.);
    EXPECT_EQ(rotoBefore, roto->getHashValue());
    EXPECT_EQ(blurBefore, blur->getHashValue());

    square->movePointByIndex(0, time, 2., 3.);
    EXPECT_EQ(rotoMoved, roto->getHashValue());

    getApp()->getProject()->reset(false, true);
}

// x reads y through an expression without being downstream of it: only the expression links them.
TEST_F(LazyHashTest, ExpressionListenerOutsideTheGraphGetsANewHash)
{
    NodePtr x = createBlur();
    NodePtr y = createBlur();
    NodePtr belowX = createBlur();
    ASSERT_TRUE(x && y && belowX);
    connectNodes(x, belowX, 0, true);
    KnobDoublePtr xSize = blurSize(x);
    KnobDoublePtr ySize = blurSize(y);
    ASSERT_TRUE(xSize && ySize);

    ASSERT_NO_THROW(xSize->setExpression(0, y->getScriptName() + ".size.getValue(0)", false, true));
    const U64 before = x->getHashValue();
    const U64 belowBefore = belowX->getHashValue();

    const double original = ySize->getValue(0);
    ySize->setValue(original + 5., ViewSpec::all(), 0);
    EXPECT_NE(before, x->getHashValue());
    EXPECT_NE(belowBefore, belowX->getHashValue());

    ySize->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, x->getHashValue());
    EXPECT_EQ(belowBefore, belowX->getHashValue());
}

TEST_F(LazyHashTest, SlaveNodeHashFollowsItsMaster)
{
    NodePtr master = createBlur();
    NodePtr slave = createBlur();
    NodePtr belowSlave = createBlur();
    ASSERT_TRUE(master && slave && belowSlave);
    connectNodes(slave, belowSlave, 0, true);
    KnobDoublePtr masterSize = blurSize(master);
    KnobDoublePtr slaveSize = blurSize(slave);
    ASSERT_TRUE(masterSize && slaveSize);

    ASSERT_TRUE(slaveSize->slaveTo(0, masterSize, 0));
    const U64 before = slave->getHashValue();
    const U64 belowBefore = belowSlave->getHashValue();

    const double original = masterSize->getValue(0);
    masterSize->setValue(original + 4., ViewSpec::all(), 0);
    EXPECT_NE(before, slave->getHashValue());
    EXPECT_NE(belowBefore, belowSlave->getHashValue());

    masterSize->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, slave->getHashValue());
    EXPECT_EQ(belowBefore, belowSlave->getHashValue());
}

TEST_F(LazyHashTest, InvalidationScopesCoverTheStampsTheyDrop)
{
    typedef FileStampRegistry R;

    EXPECT_TRUE(R::invalidationCovers(R::eInvalidationScopeAll, std::string(), "/shots/a/plate.exr"));
    EXPECT_FALSE(R::invalidationCovers(R::eInvalidationScopeAll, std::string(), std::string()));

    EXPECT_TRUE(R::invalidationCovers(R::eInvalidationScopeDirectory, "/shots/a", "/shots/a/plate.exr"));
    EXPECT_TRUE(R::invalidationCovers(R::eInvalidationScopeDirectory, "/shots/a", "/shots/a/plate.####.exr"));
    EXPECT_FALSE(R::invalidationCovers(R::eInvalidationScopeDirectory, "/shots/a", "/shots/b/plate.exr"));

    EXPECT_TRUE(R::invalidationCovers(R::eInvalidationScopePath, "/shots/a/plate.exr", "/shots/a/plate.exr"));
    EXPECT_FALSE(R::invalidationCovers(R::eInvalidationScopePath, "/shots/a/plate.exr", "/shots/a/other.exr"));
    EXPECT_TRUE(R::invalidationCovers(R::eInvalidationScopePath, "/shots/a/plate.0001.exr", "/shots/a/plate.####.exr"));
    EXPECT_FALSE(R::invalidationCovers(R::eInvalidationScopePath, "/shots/b/plate.0001.exr", "/shots/a/plate.####.exr"));
}

// A file system watcher event is the only notice of a file rewritten on disk; the node reading
// it, otherwise untouched, must take a new hash once the event is handled.
TEST_F(LazyHashTest, WatchedDirectoryChangeGivesTheReaderANewHash)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("watched.exr")).toStdString();
    ASSERT_TRUE(writeFlatExr(path, 16, 12, 0.25f));
    setMTime(path, 1000000);

    NodePtr reader = createWorkingSpaceRead(path);
    ASSERT_TRUE(bool(reader));
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    connectNodes(reader, blur, 0, true);
    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));
    const std::string resolved = fileKnob->getResolvedPath();
    const std::size_t slash = resolved.find_last_of('/');
    ASSERT_NE(std::string::npos, slash);
    const std::string directory = resolved.substr(0, slash);

    QCoreApplication::processEvents();
    const U64 readerBefore = reader->getHashValue();
    const U64 blurBefore = blur->getHashValue();

    ASSERT_TRUE(writeFlatExr(path, 32, 24, 0.75f));
    setMTime(path, 2000000);
    FileStampRegistry::instance().invalidateDirectory(directory);
    QCoreApplication::processEvents();
    EXPECT_NE(readerBefore, reader->getHashValue());
    EXPECT_NE(blurBefore, blur->getHashValue());
}
