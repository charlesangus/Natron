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

#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CLArgs.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/FrameEntry.h"
#include "Engine/FrameKey.h"
#include "Engine/FrameParams.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TextureRect.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

// The content hash is the cache key: equal render state must give an equal hash wherever and
// however it was reached, and any render-affecting difference must give a different one, or a
// cached image is served for the wrong state.
//
// No test refreshes a hash itself: every edit must mark what it changes, and a read recomputes.

namespace {

KnobDoublePtr
blurSize(const NodePtr& blur)
{
    return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
}

NodePtr
createNodeIn(const AppInstancePtr& app,
             const char* pluginID,
             int majorVersion)
{
    CreateNodeArgs args(pluginID, app->getProject());

    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);

    return app->createNode(args);
}

struct Chain {
    NodePtr constant;
    NodePtr blur;
};

// Constant -> Blur with a non-default size, built through the app's own project so it can be
// built identically in another app instance.
Chain
buildChain(const AppInstancePtr& app,
           double size)
{
    Chain chain;

    chain.constant = createNodeIn(app, PLUGINID_NATRON_CONSTANT, -1);
    chain.blur = createNodeIn(app, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
    EXPECT_TRUE(chain.constant && chain.blur);
    if (!chain.constant || !chain.blur) {
        return Chain();
    }
    EXPECT_TRUE(app->getProject()->connectNodes(0, chain.constant, chain.blur));
    KnobDoublePtr knob = blurSize(chain.blur);
    EXPECT_TRUE(bool(knob));
    if (knob) {
        knob->setValue(size, ViewSpec::all(), 0);
    }

    return chain;
}

// A second app instance lives in the same process as the test's own. Quitting it synchronously
// before the test ends keeps it from becoming the top-level app of whatever runs next.
class ScopedSecondApp {
public:
    explicit ScopedSecondApp(const AppInstancePtr& original)
        : _original(original)
    {
        CLArgs args;

        _app = appPTR->newBackgroundInstance(args, true);
        // Creating an instance makes it the top-level one; engine code that asks for the
        // top-level app must keep seeing the test's own.
        if (original) {
            appPTR->setAsTopLevelInstance(original->getAppID());
        }
    }

    ~ScopedSecondApp()
    {
        if (_app) {
            appPTR->quitNow(_app);
            _app.reset();
        }
        if (_original) {
            appPTR->setAsTopLevelInstance(_original->getAppID());
        }
    }

    const AppInstancePtr& get() const
    {
        return _app;
    }

private:
    AppInstancePtr _original;
    AppInstancePtr _app;
};

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

class HashInvariantsTest
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

    NodePtr createMerge()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }
};

// ---------------------------------------------------------------------------------------------
// The same state gives the same hash.
// ---------------------------------------------------------------------------------------------

TEST_F(HashInvariantsTest, SameGraphBuiltTwiceHashesEqual)
{
    Chain first = buildChain(getApp(), 4.);
    Chain second = buildChain(getApp(), 4.);
    ASSERT_TRUE(first.blur && second.blur);

    EXPECT_NE(first.blur->getScriptName(), second.blur->getScriptName());
    EXPECT_NE(U64(0), first.blur->getHashValue());
    EXPECT_EQ(first.constant->getHashValue(), second.constant->getHashValue());
    EXPECT_EQ(first.blur->getHashValue(), second.blur->getHashValue());
}

TEST_F(HashInvariantsTest, SameGraphHashesEqualAfterProjectResetAndRebuild)
{
    project()->reset(false, true);
    U64 before = 0;
    {
        Chain chain = buildChain(getApp(), 4.);
        ASSERT_TRUE(bool(chain.blur));
        before = chain.blur->getHashValue();
    }

    project()->reset(false, true);
    Chain rebuilt = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(rebuilt.blur));
    EXPECT_EQ(before, rebuilt.blur->getHashValue());

    project()->reset(false, true);
}

TEST_F(HashInvariantsTest, SameGraphHashesEqualAfterSaveAndLoad)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    const std::string constantName = chain.constant->getScriptName();
    const std::string blurName = chain.blur->getScriptName();
    const U64 constantHash = chain.constant->getHashValue();
    const U64 blurHash = chain.blur->getHashValue();
    chain = Chain();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("same-state.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project()->saveProject(dirPath, fileName, &savedFilePath));

    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, fileName));

    NodePtr constant = project()->getNodeByName(constantName);
    NodePtr blur = project()->getNodeByName(blurName);
    ASSERT_TRUE(constant && blur);
    EXPECT_EQ(constantHash, constant->getHashValue());
    EXPECT_EQ(blurHash, blur->getHashValue());

    project()->reset(false, true);
}

TEST_F(HashInvariantsTest, SameGraphHashesEqualInASecondAppInstance)
{
    Chain local = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(local.blur));

    ScopedSecondApp second(getApp());
    ASSERT_TRUE(bool(second.get()));
    ASSERT_NE(getApp()->getAppID(), second.get()->getAppID());
    {
        Chain remote = buildChain(second.get(), 4.);
        ASSERT_TRUE(bool(remote.blur));
        EXPECT_EQ(local.constant->getHashValue(), remote.constant->getHashValue());
        EXPECT_EQ(local.blur->getHashValue(), remote.blur->getHashValue());
    }
}

namespace {

// Writes size bytes to dir/name and gives the file a fixed modification time, as an AppImage's
// squashfs does for a bundled plug-in wherever it is mounted.
QString
writeStampedBinary(const QString& dir,
                   const QString& name,
                   int size)
{
    if (!QDir().mkpath(dir)) {
        return QString();
    }
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return QString();
    }
    f.write(QByteArray(size, 'x'));
    // Unflushed bytes written at close would set the modification time again.
    if (!f.flush()) {
        return QString();
    }
    if (!f.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000), QFileDevice::FileModificationTime)) {
        return QString();
    }
    f.close();

    return path;
}

} // namespace

// The disk cache outlives the session, so a hash input that differs per launch, such as the
// mount point of an AppImage holding the OpenFX plug-ins, would miss it on every launch.
TEST(OfxBinaryStamp, SameBinaryStampsEqualWhereverItIsMounted)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString name = QString::fromUtf8("Plugin.ofx");
    const QString first = writeStampedBinary(tmp.path() + QString::fromUtf8("/.mount_aaaa/Plugins/OFX/Natron"), name, 64);
    const QString second = writeStampedBinary(tmp.path() + QString::fromUtf8("/.mount_bbbb/Plugins/OFX/Natron"), name, 64);
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());

    EXPECT_EQ(Plugin::ofxBinaryStamp(first.toStdString()), Plugin::ofxBinaryStamp(second.toStdString()));
}

TEST(OfxBinaryStamp, RebuiltOrRenamedBinaryStampsDiffer)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString name = QString::fromUtf8("Plugin.ofx");
    const QString original = writeStampedBinary(tmp.path() + QString::fromUtf8("/a"), name, 64);
    const QString resized = writeStampedBinary(tmp.path() + QString::fromUtf8("/b"), name, 65);
    const QString renamed = writeStampedBinary(tmp.path() + QString::fromUtf8("/c"), QString::fromUtf8("Other.ofx"), 64);
    ASSERT_FALSE(original.isEmpty());
    ASSERT_FALSE(resized.isEmpty());
    ASSERT_FALSE(renamed.isEmpty());

    const U64 stamp = Plugin::ofxBinaryStamp(original.toStdString());
    EXPECT_NE(stamp, Plugin::ofxBinaryStamp(resized.toStdString()));
    EXPECT_NE(stamp, Plugin::ofxBinaryStamp(renamed.toStdString()));

    QFile f(original);
    ASSERT_TRUE(f.open(QIODevice::ReadWrite));
    ASSERT_TRUE(f.setFileTime(QDateTime::fromSecsSinceEpoch(1700000060), QFileDevice::FileModificationTime));
    f.close();
    EXPECT_NE(stamp, Plugin::ofxBinaryStamp(original.toStdString()));
}

// ---------------------------------------------------------------------------------------------
// Every render-affecting edit changes the hash, and returning to the earlier state restores it.
// Knob undo commands live in Gui/, which this binary does not link, so a return is a value set
// back; KnobUndoHash_Test in GuiTests runs the undo command itself.
// ---------------------------------------------------------------------------------------------

TEST_F(HashInvariantsTest, KnobValueEditChangesHashUpToDownstreamAndRevertRestoresIt)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    NodePtr downstream = createBlur();
    ASSERT_TRUE(bool(downstream));
    connectNodes(chain.blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(chain.blur);
    ASSERT_TRUE(bool(size));
    const U64 before = chain.blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    size->setValue(7., ViewSpec::all(), 0);
    EXPECT_NE(before, chain.blur->getHashValue());
    EXPECT_NE(downstreamBefore, downstream->getHashValue());

    size->setValue(4., ViewSpec::all(), 0);
    EXPECT_EQ(before, chain.blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());
}

// Toggling between two states keeps returning to the same two keys, so both stay reusable in
// the cache instead of every return minting a new key.
TEST_F(HashInvariantsTest, ToggledValuesAlternateBetweenTwoHashes)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    KnobDoublePtr size = blurSize(chain.blur);
    ASSERT_TRUE(bool(size));

    const U64 a = chain.blur->getHashValue();
    size->setValue(9., ViewSpec::all(), 0);
    const U64 b = chain.blur->getHashValue();
    ASSERT_NE(a, b);

    for (int i = 0; i < 3; ++i) {
        size->setValue(4., ViewSpec::all(), 0);
        EXPECT_EQ(a, chain.blur->getHashValue());
        size->setValue(9., ViewSpec::all(), 0);
        EXPECT_EQ(b, chain.blur->getHashValue());
    }
}

// An edit made off the main thread gets no knobs age bump, so only its own mark can tell a later
// read that the state moved; without it the new state renders under the old key.
TEST_F(HashInvariantsTest, KnobEditFromAWorkerThreadChangesHashUpToDownstream)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    NodePtr downstream = createBlur();
    ASSERT_TRUE(bool(downstream));
    connectNodes(chain.blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(chain.blur);
    ASSERT_TRUE(bool(size));
    const U64 before = chain.blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    std::thread editor([&size]() {
        size->setValue(7., ViewSpec::all(), 0);
    });
    editor.join();
    const U64 offThread = chain.blur->getHashValue();
    const U64 downstreamOffThread = downstream->getHashValue();
    EXPECT_NE(before, offThread);
    EXPECT_NE(downstreamBefore, downstreamOffThread);
    // Lets the main thread finish the worker's edit bracket before editing on the main thread.
    QCoreApplication::processEvents();
    EXPECT_EQ(offThread, chain.blur->getHashValue());

    size->setValue(4., ViewSpec::all(), 0);
    EXPECT_EQ(before, chain.blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());

    size->setValue(7., ViewSpec::all(), 0);
    EXPECT_EQ(offThread, chain.blur->getHashValue()) << "the state reached off the main thread hashes like the same state reached on it";
    EXPECT_EQ(downstreamOffThread, downstream->getHashValue());
}

TEST_F(HashInvariantsTest, AnimationKeyEditChangesHashAndRemovingItRestoresIt)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    KnobDoublePtr size = blurSize(chain.blur);
    ASSERT_TRUE(bool(size));
    const U64 before = chain.blur->getHashValue();

    size->setValueAtTime(10., 4., ViewSpec::all(), 0);
    const U64 oneKey = chain.blur->getHashValue();
    EXPECT_NE(before, oneKey) << "a key equal to the static value still changes the curve";

    size->setValueAtTime(20., 8., ViewSpec::all(), 0);
    const U64 twoKeys = chain.blur->getHashValue();
    EXPECT_NE(oneKey, twoKeys);

    size->setValueAtTime(20., 6., ViewSpec::all(), 0);
    EXPECT_NE(twoKeys, chain.blur->getHashValue()) << "moving a key's value must change the hash";

    size->removeAnimation(ViewSpec::all(), 0);
    size->setValue(4., ViewSpec::all(), 0);
    EXPECT_EQ(before, chain.blur->getHashValue());
}

TEST_F(HashInvariantsTest, ExpressionEditChangesHashAndClearingItRestoresIt)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    KnobDoublePtr size = blurSize(chain.blur);
    ASSERT_TRUE(bool(size));
    const U64 before = chain.blur->getHashValue();

    ASSERT_NO_THROW(size->setExpression(0, "2 * 2", false, true));
    const U64 withExpression = chain.blur->getHashValue();
    EXPECT_NE(before, withExpression) << "an expression evaluating to the static value is still a different state";

    ASSERT_NO_THROW(size->setExpression(0, "3 * 2", false, true));
    EXPECT_NE(withExpression, chain.blur->getHashValue());

    size->clearExpression(0, true);
    EXPECT_EQ(before, chain.blur->getHashValue());
}

TEST_F(HashInvariantsTest, ConnectAndDisconnectChangeHashAndRestoreIt)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    ASSERT_TRUE(constant && blur);
    const U64 unconnected = blur->getHashValue();
    ASSERT_NE(U64(0), unconnected);

    connectNodes(constant, blur, 0, true);
    const U64 connected = blur->getHashValue();
    EXPECT_NE(unconnected, connected);

    disconnectNodes(constant, blur, true);
    EXPECT_EQ(unconnected, blur->getHashValue());

    connectNodes(constant, blur, 0, true);
    EXPECT_EQ(connected, blur->getHashValue());
}

TEST_F(HashInvariantsTest, InputSwapChangesHashAndSwappingBackRestoresIt)
{
    NodePtr constant = createConstant();
    NodePtr blurredConstant = createConstant();
    NodePtr blur = createBlur();
    NodePtr merge = createMerge();
    ASSERT_TRUE(constant && blurredConstant && blur && merge);
    connectNodes(blurredConstant, blur, 0, true);
    connectNodes(constant, merge, kNativeMergeInputB, true);
    connectNodes(blur, merge, kNativeMergeInputA, true);
    ASSERT_NE(constant->getHashValue(), blur->getHashValue());

    const U64 straight = merge->getHashValue();
    merge->switchInput0And1();
    ASSERT_EQ(blur, merge->getInput(kNativeMergeInputB));
    ASSERT_EQ(constant, merge->getInput(kNativeMergeInputA));
    const U64 swapped = merge->getHashValue();
    EXPECT_NE(straight, swapped);

    merge->switchInput0And1();
    EXPECT_EQ(straight, merge->getHashValue());
}

// A file rewritten on disk and reloaded is a different input even though no knob moved; without
// its stamp in the key, the old pixels would be served under the old hash.
TEST_F(HashInvariantsTest, ReloadedRewrittenReadFileChangesHashUpToDownstream)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("rewritten.exr")).toStdString();
    ASSERT_TRUE(writeFlatExr(path, 16, 12, 0.25f));
    setMTime(path, 1000000);

    NodePtr reader = createWorkingSpaceRead(path);
    ASSERT_TRUE(bool(reader));
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    connectNodes(reader, blur, 0, true);

    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));

    const U64 readerBefore = reader->getHashValue();
    const U64 blurBefore = blur->getHashValue();

    fileKnob->reloadFile();
    EXPECT_EQ(readerBefore, reader->getHashValue()) << "an untouched file must keep its hash";
    EXPECT_EQ(blurBefore, blur->getHashValue());

    // The mtime is set explicitly because a rewrite within the same second could keep the old one,
    // and the larger image makes sure the size moves too.
    ASSERT_TRUE(writeFlatExr(path, 32, 24, 0.75f));
    setMTime(path, 2000000);
    EXPECT_EQ(readerBefore, reader->getHashValue()) << "a file is only looked at again on Reload";
    fileKnob->reloadFile();
    EXPECT_NE(readerBefore, reader->getHashValue());
    EXPECT_NE(blurBefore, blur->getHashValue());
}

// ---------------------------------------------------------------------------------------------
// Edits that don't change what renders leave the hash alone, so they don't throw away cache.
// ---------------------------------------------------------------------------------------------

TEST_F(HashInvariantsTest, LabelEditsKeepHash)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    const U64 before = chain.blur->getHashValue();

    chain.blur->setLabel("A label that renders nothing");
    EXPECT_EQ(before, chain.blur->getHashValue());

    KnobStringPtr labelKnob = std::dynamic_pointer_cast<KnobString>(chain.blur->getKnobByName(kUserLabelKnobName));
    ASSERT_TRUE(bool(labelKnob));
    labelKnob->setValue(std::string("note shown under the node name"));
    EXPECT_EQ(before, chain.blur->getHashValue());
}

// Without a GUI a node keeps no position at all, so this only guards that moving a node goes
// through nothing the hash reads.
TEST_F(HashInvariantsTest, NodePositionEditKeepsHash)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    const U64 before = chain.blur->getHashValue();

    chain.blur->setPosition(250., -120.);
    EXPECT_EQ(before, chain.blur->getHashValue());
}

// ---------------------------------------------------------------------------------------------
// Regressions for collisions of the age-based key.
// ---------------------------------------------------------------------------------------------

// An unsaved edit renders under one key. With a key that counts edits, reopening the saved
// project and making a different edit reaches the same count, and so serves the first edit's
// pixels from the persistent caches.
TEST_F(HashInvariantsTest, DifferentEditAfterReopenDoesNotReuseTheUnsavedEditsHash)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    const std::string constantName = chain.constant->getScriptName();
    const std::string blurName = chain.blur->getScriptName();
    const U64 saved = chain.blur->getHashValue();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("reopen.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project()->saveProject(dirPath, fileName, &savedFilePath));

    KnobDoublePtr size = blurSize(chain.blur);
    ASSERT_TRUE(bool(size));
    size->setValue(11., ViewSpec::all(), 0);
    const U64 unsavedEdit = chain.blur->getHashValue();
    ASSERT_NE(saved, unsavedEdit);
    chain = Chain();
    size.reset();

    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, fileName));
    NodePtr constant = project()->getNodeByName(constantName);
    NodePtr blur = project()->getNodeByName(blurName);
    ASSERT_TRUE(constant && blur);
    EXPECT_EQ(saved, blur->getHashValue());

    size = blurSize(blur);
    ASSERT_TRUE(bool(size));
    size->setValue(13., ViewSpec::all(), 0);
    EXPECT_NE(unsavedEdit, blur->getHashValue()) << "a different edit after reopening reused the unsaved edit's key";
    EXPECT_NE(saved, blur->getHashValue());

    // The unsaved edit's state, reached again, is the same state and may reuse its pixels.
    size->setValue(11., ViewSpec::all(), 0);
    EXPECT_EQ(unsavedEdit, blur->getHashValue());

    project()->reset(false, true);
}

// Save As keeps the project's creation date, so a key built from it and an edit count gives two
// diverged copies of one project, open side by side, the same keys for different content.
TEST_F(HashInvariantsTest, DivergedSaveAsCopiesDoNotCollide)
{
    Chain chain = buildChain(getApp(), 4.);
    ASSERT_TRUE(bool(chain.blur));
    const std::string constantName = chain.constant->getScriptName();
    const std::string blurName = chain.blur->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString originalName = QString::fromUtf8("original.ntpz");
    const QString copyName = QString::fromUtf8("copy.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project()->saveProject(dirPath, originalName, &savedFilePath));
    ASSERT_TRUE(project()->saveProject(dirPath, copyName, &savedFilePath));
    ASSERT_EQ(copyName, project()->getProjectFilename());

    // Background instances all publish their nodes to Python as app.<name>, so the second
    // instance cannot load nodes under names this one still holds: they would keep no name and
    // lose their connections. Names are not part of the key, so renaming here changes no hash.
    const U64 copyBlurHash = chain.blur->getHashValue();
    chain.constant->setScriptName("copyConstant");
    chain.blur->setScriptName("copyBlur");
    ASSERT_EQ(std::string("copyConstant"), chain.constant->getScriptName());
    ASSERT_EQ(std::string("copyBlur"), chain.blur->getScriptName());
    ASSERT_EQ(copyBlurHash, chain.blur->getHashValue());

    ScopedSecondApp second(getApp());
    ASSERT_TRUE(bool(second.get()));
    ProjectPtr originalProject = second.get()->getProject();
    ASSERT_TRUE(originalProject->loadProject(dirPath, originalName));
    {
        NodePtr originalConstant = originalProject->getNodeByName(constantName);
        NodePtr originalBlur = originalProject->getNodeByName(blurName);
        ASSERT_TRUE(originalConstant && originalBlur);
        EXPECT_EQ(chain.blur->getHashValue(), originalBlur->getHashValue()) << "the copies start in the same state";

        KnobDoublePtr copySize = blurSize(chain.blur);
        KnobDoublePtr originalSize = blurSize(originalBlur);
        ASSERT_TRUE(copySize && originalSize);

        copySize->setValue(6., ViewSpec::all(), 0);
        originalSize->setValue(9., ViewSpec::all(), 0);
        EXPECT_NE(chain.blur->getHashValue(), originalBlur->getHashValue()) << "diverged copies share a key";

        originalSize->setValue(6., ViewSpec::all(), 0);
        EXPECT_EQ(chain.blur->getHashValue(), originalBlur->getHashValue()) << "copies brought back to one state must share a key";

        // Equal numbers of edits on both sides are what an edit-counting key cannot tell apart.
        copySize->setValue(2., ViewSpec::all(), 0);
        originalSize->setValue(3., ViewSpec::all(), 0);
        EXPECT_NE(chain.blur->getHashValue(), originalBlur->getHashValue());
    }

    project()->reset(false, true);
}

namespace {

// Tiles of a viewer texture differ only in their place along x here.
struct ViewerTile {
    int column = 0;
    int displayChannels = 4;
    bool draft = false;
    std::string inputName;
};

// The key of one viewer cache tile of frame time under viewerHash at mipmapLevel, keyed the way
// the viewer keys its textures.
FrameKey
viewerTileKey(const NodePtr& holder,
              SequenceTime time,
              U64 viewerHash,
              unsigned int mipmapLevel,
              const ViewerTile& tile = ViewerTile())
{
    const SettingsPtr settings = appPTR->getCurrentSettings();
    const int tileSize = 1 << settings->getViewerTilesPowerOf2();
    const int bitDepth = (int)settings->getViewersBitDepth();
    std::vector<std::string> channels;
    channels.push_back("R");
    channels.push_back("G");
    channels.push_back("B");
    channels.push_back("A");
    const ImageLayerDesc layer("Color", "Color", "RGBA", channels);

    return FrameKey(holder.get(),
                    time,
                    viewerHash,
                    1.,
                    1.,
                    0,
                    bitDepth,
                    tile.displayChannels,
                    ViewIdx(0),
                    TextureRect(tile.column * tileSize, 0, (tile.column + 1) * tileSize, tileSize, 1 << mipmapLevel, 1.),
                    mipmapLevel,
                    tile.inputName.empty() ? holder->getScriptName() : tile.inputName,
                    layer,
                    "Color.A",
                    bitDepth == (int)eImageBitDepthFloat,
                    tile.draft);
}

FrameEntryPtr
cacheViewerTile(const NodePtr& holder,
                SequenceTime time,
                U64 viewerHash,
                unsigned int mipmapLevel,
                const ViewerTile& tile = ViewerTile())
{
    const SettingsPtr settings = appPTR->getCurrentSettings();
    const int tileSize = 1 << settings->getViewerTilesPowerOf2();
    const int bitDepth = (int)settings->getViewersBitDepth();
    const FrameKey key = viewerTileKey(holder, time, viewerHash, mipmapLevel, tile);
    const RectI tileBounds(0, 0, tileSize, tileSize);
    FrameParamsPtr params(new FrameParams(tileBounds, bitDepth, tileBounds, ImagePtr()));
    FrameEntryPtr entry;
    if (appPTR->getTextureOrCreate(key, params, NULL, &entry) || !entry) {
        return entry;
    }
    entry->allocateMemory();

    return entry;
}

} // namespace

// The timeline's cached line is built from getViewerCachedFrames(). The viewer cache is tiled and
// keeps every tile in its disk portion; a frame the viewer just rendered must still be listed as
// held in RAM, which the line draws in the cached-frame colour, not the dark disk-cached one.
TEST_F(HashInvariantsTest, ViewerCachedFramesListTheCurrentHashAndLevelInRam)
{
    NodePtr holder = createConstant();
    ASSERT_TRUE(bool(holder));
    const U64 current = holder->getHashValue();
    const U64 earlier = current + 1;

    std::vector<FrameEntryPtr> held;
    held.push_back(cacheViewerTile(holder, 3, current, 0));
    held.push_back(cacheViewerTile(holder, 5, current, 0));
    held.push_back(cacheViewerTile(holder, 7, earlier, 0));
    held.push_back(cacheViewerTile(holder, 9, current, 1));
    for (std::size_t i = 0; i < held.size(); ++i) {
        ASSERT_TRUE(bool(held[i])) << "tile " << i << " was cached";
    }

    std::map<SequenceTime, StorageModeEnum> frames;
    appPTR->getViewerCachedFrames(holder->getCacheID(), current, 0, NULL, &frames);
    ASSERT_EQ(2u, frames.size()) << "only the frames of the current hash at level 0";
    ASSERT_TRUE(frames.count(3) && frames.count(5));
    EXPECT_EQ(eStorageModeRAM, frames[3]);
    EXPECT_EQ(eStorageModeRAM, frames[5]);

    appPTR->getViewerCachedFrames(holder->getCacheID(), earlier, 0, NULL, &frames);
    ASSERT_EQ(1u, frames.size()) << "returning to an earlier hash lists its frames again";
    EXPECT_EQ(1u, frames.count(7));

    appPTR->getViewerCachedFrames(holder->getCacheID(), current, 1, NULL, &frames);
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(1u, frames.count(9));

    held.clear();
    appPTR->removeAllCacheEntriesForHolder(holder.get(), true);
    appPTR->getViewerCachedFrames(holder->getCacheID(), current, 0, NULL, &frames);
    EXPECT_TRUE(frames.empty()) << "a cleared cache lists nothing";

    project()->reset(false, true);
}

// The line counts a frame only if a lookup under the viewer's last full-quality settings would hit:
// same display settings, not draft, and every tile of the lookup held.
TEST_F(HashInvariantsTest, ViewerCachedFramesFollowTheLastLookup)
{
    NodePtr holder = createConstant();
    ASSERT_TRUE(bool(holder));
    const U64 hash = holder->getHashValue();
    ViewerTile left;
    ViewerTile right;
    right.column = 1;
    ViewerTile red;
    red.displayChannels = 2;
    ViewerTile draft;
    draft.draft = true;
    ViewerTile otherInput;
    otherInput.inputName = "SomeOtherInput";

    std::vector<FrameEntryPtr> held;
    held.push_back(cacheViewerTile(holder, 1, hash, 0, left));
    held.push_back(cacheViewerTile(holder, 1, hash, 0, right));
    held.push_back(cacheViewerTile(holder, 2, hash, 0, left));
    held.push_back(cacheViewerTile(holder, 3, hash, 0, red));
    held.push_back(cacheViewerTile(holder, 3, hash, 0, ViewerTile { 1, 2, false, std::string() }));
    held.push_back(cacheViewerTile(holder, 4, hash, 0, draft));
    held.push_back(cacheViewerTile(holder, 4, hash, 0, ViewerTile { 1, 4, true, std::string() }));
    held.push_back(cacheViewerTile(holder, 5, hash, 0, otherInput));
    held.push_back(cacheViewerTile(holder, 5, hash, 0, ViewerTile { 1, 4, false, "SomeOtherInput" }));
    for (std::size_t i = 0; i < held.size(); ++i) {
        ASSERT_TRUE(bool(held[i])) << "tile " << i << " was cached";
    }

    ViewerFrameFilter filter;
    filter.settings = viewerTileKey(holder, 7, hash, 0, left);
    filter.tiles.push_back(viewerTileKey(holder, 7, hash, 0, left).getTexRect());
    filter.tiles.push_back(viewerTileKey(holder, 7, hash, 0, right).getTexRect());

    std::map<SequenceTime, StorageModeEnum> frames;
    appPTR->getViewerCachedFrames(holder->getCacheID(), hash, 0, &filter, &frames);
    ASSERT_EQ(1u, frames.size()) << "frame 2 lacks a tile; 3, 4 and 5 differ in channels, draft and input";
    EXPECT_EQ(1u, frames.count(1));

    filter.settings = viewerTileKey(holder, 7, hash, 0, red);
    appPTR->getViewerCachedFrames(holder->getCacheID(), hash, 0, &filter, &frames);
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(1u, frames.count(3));

    filter.settings = viewerTileKey(holder, 7, hash, 0, left);
    filter.tiles.pop_back();
    appPTR->getViewerCachedFrames(holder->getCacheID(), hash, 0, &filter, &frames);
    ASSERT_EQ(2u, frames.size()) << "a lookup of the left tile alone is served by frames 1 and 2";
    EXPECT_TRUE(frames.count(1) && frames.count(2));

    held.clear();
    appPTR->removeAllCacheEntriesForHolder(holder.get(), true);
    project()->reset(false, true);
}
