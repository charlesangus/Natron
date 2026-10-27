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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

void
writeFile(const std::string& path,
          const std::string& content)
{
    {
        QFile f(QString::fromStdString(path));

        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content.c_str(), (qint64)content.size());
    }
    // Each write a second later than the last: USD reloads a layer only when its file's
    // modification time moved (SdfLayer::_Reload), and two writes can land in one clock tick.
    static int writes = 0;
    ++writes;
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(writes), ec);
    ASSERT_FALSE(ec) << ec.message();
}

// A layer defining one Xform at /<primName>. Two names of different lengths give files of
// different sizes, which the file stamp tells apart even within one mtime tick.
std::string
sceneDefining(const std::string& primName)
{
    return "#usda 1.0\n\ndef Xform \"" + primName + "\"\n{\n}\n";
}

// A layer sublayering subLayer.
std::string
sceneSublayering(const std::string& subLayer)
{
    return "#usda 1.0\n(\n    subLayers = [@./" + subLayer + "@]\n)\n";
}

// A layer defining /Leaf with a radius.
std::string
leafWithRadius(double radius)
{
    return "#usda 1.0\n\ndef Sphere \"Leaf\"\n{\n    double radius = " + std::to_string(radius) + "\n}\n";
}

double
leafRadius(const PXR_NS::UsdStageRefPtr& stage)
{
    double radius = -1.;
    PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(PXR_NS::SdfPath("/Leaf")) : PXR_NS::UsdPrim();

    if (!prim.IsValid() || !prim.GetAttribute(PXR_NS::TfToken("radius")).Get(&radius)) {
        return -1.;
    }

    return radius;
}

// Whether the stage's only root prim is /<primName>.
bool
holdsOnly(const PXR_NS::UsdStageRefPtr& stage,
          const std::string& primName)
{
    if (!stage) {
        return false;
    }
    std::size_t count = 0;
    for (const PXR_NS::UsdPrim& prim : stage->GetPseudoRoot().GetChildren()) {
        if (prim.GetName().GetString() != primName) {
            return false;
        }
        ++count;
    }

    return count == 1;
}
} // anonymous namespace

class USDReadTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        // The files read here are centimetre scenes, authored or by USD's fallback: a project in
        // the same units reads them without a units warning.
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_projectFrameRate) {
            _projectFrameRate->setValue(_originalFrameRate);
        }
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr createReader(const std::string& path)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        if (!reader) {
            return reader;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        if (!file) {
            return NodePtr();
        }
        file->setValue(path);

        return reader;
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload,
                                                     std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    static void pressReload(const NodePtr& reader)
    {
        KnobButton* reload = dynamic_cast<KnobButton*>(reader->getKnobByName(kUSDReadParamReload).get());

        ASSERT_TRUE(reload != NULL);
        reload->trigger();
    }

    static std::string getErrorMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);
        EXPECT_EQ((int)eMessageTypeError, type);

        return message.toStdString();
    }

    void setProjectFrameRate(double framesPerSecond)
    {
        KnobDouble* frameRate = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("frameRate").get());

        ASSERT_TRUE(frameRate != NULL);
        if (!_projectFrameRate) {
            _projectFrameRate = frameRate;
            _originalFrameRate = frameRate->getValue();
        }
        frameRate->setValue(framesPerSecond);
    }

    // A stage composing payload, borrowed from the pool nodes read their upstream through.
    static SceneQueryPool::Lease borrow(const ScenePayload& payload)
    {
        std::string error;
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, &error);

        EXPECT_TRUE(lease.isValid()) << error;

        return lease;
    }

    KnobDouble* _projectFrameRate = NULL;
    double _originalFrameRate = 24.;
};

TEST_F(USDReadTest, ReadsTheFilesPrimsAndMetadata)
{
    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    EXPECT_TRUE(reader->getEffectInstance()->producesSceneData());

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_EQ((std::size_t)1, payload.getNumLayers());
    EXPECT_FALSE(reader->hasPersistentMessage());

    const SceneMetadata& metadata = payload.getMetadata();
    EXPECT_EQ(SceneMetadata::eUpAxisY, metadata.upAxis);
    EXPECT_DOUBLE_EQ(0.01, metadata.metersPerUnit);
    EXPECT_EQ(std::string("World"), metadata.defaultPrim);
    EXPECT_DOUBLE_EQ(24., metadata.timeCodesPerSecond);

    SceneQueryPool::Lease lease = borrow(payload);
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).IsValid());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).IsValid());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")).IsValid());
    EXPECT_EQ(PXR_NS::TfToken("World"), stage->GetDefaultPrim().GetName());
}

TEST_F(USDReadTest, ReEvaluatingReusesTheLayer)
{
    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));

    ScenePayload first, second;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &first, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &second, &error)) << error;
    EXPECT_TRUE(first.isSameStack(second));
}

// The same path as a USD file: SdfLayer opens .abc through USD's Alembic file-format plug-in.
TEST_F(USDReadTest, ReadsAlembic)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string abcPath = dir.filePath(QString::fromUtf8("reference.abc")).toStdString();
    {
        PXR_NS::SdfLayerRefPtr reference = PXR_NS::SdfLayer::FindOrOpen(kReferenceScene);
        ASSERT_TRUE(bool(reference));
        ASSERT_TRUE(reference->Export(abcPath)) << "USD has no Alembic file-format plug-in to write " << abcPath;
    }

    NodePtr reader = createReader(abcPath);
    ASSERT_TRUE(bool(reader));

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    SceneQueryPool::Lease lease = borrow(payload);
    ASSERT_TRUE(bool(lease.getStage()));
    EXPECT_TRUE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).IsValid());
}

TEST_F(USDReadTest, MissingFileFailsWithANodeError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("missing.usda")).toStdString();
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    ScenePayload payload;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(reader, &payload, &error));
    EXPECT_NE(std::string::npos, error.find(path)) << error;
    ASSERT_TRUE(reader->hasPersistentMessage());
    EXPECT_EQ(error, getErrorMessage(reader));
}

TEST_F(USDReadTest, MalformedFileFailsWithUsdsDiagnostic)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("malformed.usda")).toStdString();
    writeFile(path, "#usda 1.0\ndef Xform \"A\" {{{\n");
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    ScenePayload payload;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(reader, &payload, &error));
    // USD's parser names the file and the position it stopped at.
    EXPECT_NE(std::string::npos, error.find("malformed.usda")) << error;
    ASSERT_TRUE(reader->hasPersistentMessage());
    EXPECT_EQ(error, getErrorMessage(reader));
}

TEST_F(USDReadTest, ReloadReadsTheRewrittenFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("rewritten.usda")).toStdString();
    writeFile(path, sceneDefining("Before"));
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &before, &error)) << error;
    {
        SceneQueryPool::Lease lease = borrow(before);
        EXPECT_TRUE(holdsOnly(lease.getStage(), "Before"));
    }

    writeFile(path, sceneDefining("AfterTheRewrite"));
    pressReload(reader);

    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &after, &error)) << error;
    EXPECT_NE(before.getStackHash(), after.getStackHash());
    SceneQueryPool::Lease lease = borrow(after);
    EXPECT_TRUE(holdsOnly(lease.getStage(), "AfterTheRewrite"));
}

// The file's layer is USD's one layer for the file, shared by every reader: a file rewritten on
// disk is read again only by a Reload, and a Reload on one reader shows the new content in every
// stage composing the file, a long-lived one like the viewer's included.
TEST_F(USDReadTest, AReloadOnOneReaderUpdatesEveryStageReadingTheFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("reexported.usda")).toStdString();
    writeFile(path, sceneDefining("Before"));
    NodePtr first = createReader(path);
    ASSERT_TRUE(bool(first));

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(first, &before, &error)) << error;
    SceneStage shown;
    shown.sync(before);
    ASSERT_TRUE(holdsOnly(shown.getStage(), "Before"));

    writeFile(path, sceneDefining("AfterTheRewrite"));
    NodePtr second = createReader(path);
    ASSERT_TRUE(bool(second));
    ScenePayload stale;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(second, &stale, &error)) << error;
    {
        SceneQueryPool::Lease lease = borrow(stale);
        EXPECT_TRUE(holdsOnly(lease.getStage(), "Before")) << "read again without a Reload";
    }

    pressReload(second);
    EXPECT_FALSE(second->hasPersistentMessage());
    EXPECT_TRUE(holdsOnly(shown.getStage(), "AfterTheRewrite"));
    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(first, &after, &error)) << error;
    // The other reader re-keyed too: its file's stamp was dropped with the reloading one's.
    EXPECT_NE(before.getStackHash(), after.getStackHash());
    SceneQueryPool::Lease lease = borrow(after);
    EXPECT_TRUE(holdsOnly(lease.getStage(), "AfterTheRewrite"));
}

// A file two sublayers down changes on disk: the Reload reads it again, in the stage a viewer
// keeps and in a new evaluation, and the reader's key moves although its own file did not change.
TEST_F(USDReadTest, ANestedSublayerChangeShowsAfterReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string top = dir.filePath(QString::fromUtf8("top.usda")).toStdString();
    const std::string middle = dir.filePath(QString::fromUtf8("middle.usda")).toStdString();
    const std::string leaf = dir.filePath(QString::fromUtf8("leaf.usda")).toStdString();
    writeFile(leaf, leafWithRadius(1.));
    writeFile(middle, sceneSublayering("leaf.usda"));
    writeFile(top, sceneSublayering("middle.usda"));
    NodePtr reader = createReader(top);
    ASSERT_TRUE(bool(reader));

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &before, &error)) << error;
    SceneStage shown;
    shown.sync(before);
    ASSERT_DOUBLE_EQ(1., leafRadius(shown.getStage()));

    writeFile(leaf, leafWithRadius(2.));
    // Nothing reads the disk again until Reload.
    ScenePayload unchanged;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &unchanged, &error)) << error;
    EXPECT_TRUE(before.isSameStack(unchanged));
    EXPECT_DOUBLE_EQ(1., leafRadius(shown.getStage()));

    pressReload(reader);
    EXPECT_FALSE(reader->hasPersistentMessage());
    EXPECT_DOUBLE_EQ(2., leafRadius(shown.getStage()));
    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &after, &error)) << error;
    EXPECT_NE(before.getStackHash(), after.getStackHash());
    {
        SceneQueryPool::Lease lease = borrow(after);
        EXPECT_DOUBLE_EQ(2., leafRadius(lease.getStage()));
    }
    // A stage opened afresh agrees.
    EXPECT_DOUBLE_EQ(2., leafRadius(SceneOps::compose(after)));
}

// Re-authoring an unchanged file reuses the file's closure walk, but a reload through another
// reader can change what a shared layer brings in: the next authoring walks again, so this
// reader's Reload reaches the layer the shared one sublayers since.
TEST_F(USDReadTest, AReAuthorAfterAnotherReadersReloadFindsTheNewDependency)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string topA = dir.filePath(QString::fromUtf8("topA.usda")).toStdString();
    const std::string topB = dir.filePath(QString::fromUtf8("topB.usda")).toStdString();
    const std::string shared = dir.filePath(QString::fromUtf8("shared.usda")).toStdString();
    const std::string leaf = dir.filePath(QString::fromUtf8("leaf.usda")).toStdString();
    writeFile(shared, leafWithRadius(1.));
    writeFile(topA, sceneSublayering("shared.usda"));
    writeFile(topB, sceneSublayering("shared.usda"));
    NodePtr readerA = createReader(topA);
    NodePtr readerB = createReader(topB);
    ASSERT_TRUE(bool(readerA));
    ASSERT_TRUE(bool(readerB));

    ScenePayload payloadA;
    ScenePayload payloadB;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(readerA, &payloadA, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(readerB, &payloadB, &error)) << error;
    ASSERT_DOUBLE_EQ(1., leafRadius(SceneOps::compose(payloadA)));

    // The shared layer moves /Leaf out to a file of its own; B's Reload reads it again in place.
    writeFile(leaf, leafWithRadius(1.));
    writeFile(shared, sceneSublayering("leaf.usda"));
    pressReload(readerB);
    EXPECT_FALSE(readerB->hasPersistentMessage());

    // A frame offset edit re-authors A's layer over the same, unchanged file.
    KnobDouble* frameOffset = dynamic_cast<KnobDouble*>(readerA->getKnobByName(kUSDReadParamFrameOffset).get());
    ASSERT_TRUE(frameOffset != NULL);
    frameOffset->setValue(3.);
    ScenePayload offsetA;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(readerA, &offsetA, &error)) << error;
    ASSERT_FALSE(payloadA.isSameStack(offsetA));
    SceneStage shown;
    shown.sync(offsetA);
    ASSERT_DOUBLE_EQ(1., leafRadius(shown.getStage()));

    writeFile(leaf, leafWithRadius(2.));
    pressReload(readerA);
    EXPECT_FALSE(readerA->hasPersistentMessage());
    EXPECT_DOUBLE_EQ(2., leafRadius(shown.getStage()));
}

// Reload edits layers in place behind SceneWriteBarrier: a render thread composing and reading
// through 20 Reload rounds, through pool leases and through a long-lived stage it guards with a
// barrier participant as the viewer does, never reads a half-reloaded layer, and every round the
// long-lived stage shows the rewrite.
TEST_F(USDReadTest, RenderThreadsComposeThroughTwentyReloads)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("reloaded.usda")).toStdString();
    writeFile(path, sceneDefining("Before"));
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &before, &error)) << error;
    // Held as the viewer holds its stage, read outside any ReadScope under its own lock.
    std::mutex shownMutex;
    SceneStage shown;
    shown.sync(before);
    ASSERT_TRUE(holdsOnly(shown.getStage(), "Before"));
    const SceneWriteBarrier::ParticipantId participant = SceneWriteBarrier::registerParticipant([&shownMutex]() { shownMutex.lock(); }, [&shownMutex]() { shownMutex.unlock(); });

    std::atomic<bool> done(false);
    std::atomic<int> reads(0);
    std::atomic<int> wrongReads(0);
    // One root prim, named as one of the rewrites names it.
    const auto consistent = [](const PXR_NS::UsdStageRefPtr& stage) {
        if (!stage) {
            return false;
        }
        std::size_t count = 0;
        for (const PXR_NS::UsdPrim& prim : stage->GetPseudoRoot().GetChildren()) {
            const std::string name = prim.GetName().GetString();
            if ((name != "Before") && (name.compare(0, 5, "After") != 0)) {
                return false;
            }
            ++count;
        }

        return count == 1;
    };
    std::thread renderThread([&]() {
        while (!done.load()) {
            bool intact = true;
            {
                std::string borrowError;
                SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(before, &borrowError);
                intact = lease.isValid() && consistent(lease.getStage());
            }
            {
                std::lock_guard<std::mutex> lock(shownMutex);
                intact = intact && consistent(shown.getStage());
            }
            if (!intact) {
                ++wrongReads;
            }
            ++reads;
        }
    });
    while (reads.load() == 0) {
        std::this_thread::yield();
    }

    const U64 writesBefore = SceneWriteBarrier::getWriteCount();
    for (int i = 0; i < 20; ++i) {
        // A longer name each time, so every rewrite also changes the file's size.
        const std::string primName = "After" + std::string(i + 1, 'x');
        writeFile(path, sceneDefining(primName));
        pressReload(reader);
        EXPECT_FALSE(reader->hasPersistentMessage()) << "Rewrite " << i;
        {
            std::lock_guard<std::mutex> lock(shownMutex);
            EXPECT_TRUE(holdsOnly(shown.getStage(), primName)) << "Rewrite " << i;
        }
        ScenePayload after;
        if (evaluate(reader, &after, &error) != EffectInstance::eRenderRoIRetCodeOk) {
            ADD_FAILURE() << "Rewrite " << i << ": " << error;
            break;
        }
        SceneQueryPool::Lease lease = borrow(after);
        EXPECT_TRUE(holdsOnly(lease.getStage(), primName)) << "Rewrite " << i;
    }
    done = true;
    renderThread.join();
    SceneWriteBarrier::unregisterParticipant(participant);

    EXPECT_EQ(writesBefore + 20, SceneWriteBarrier::getWriteCount());
    EXPECT_LT(0, reads.load());
    EXPECT_EQ(0, wrongReads.load());
}

TEST_F(USDReadTest, ResettingTheProjectReleasesIdleQueryStages)
{
    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    {
        SceneQueryPool::Lease lease = borrow(payload);
        ASSERT_TRUE(bool(lease.getStage()));
    }
    ASSERT_LT((std::size_t)0, SceneQueryPool::instance().getNumIdle());
    reader.reset();

    getApp()->getProject()->reset(false, true);

    EXPECT_EQ((std::size_t)0, SceneQueryPool::instance().getNumIdle());
}

// Time maps by seconds: frame 25 of a 25 fps project is one second in, timecode 24 of a 24 tcps
// stage. The frame offset shifts the frame before it is converted.
TEST_F(USDReadTest, TimeMapsBySecondsWithTheFrameOffset)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("timed.usda")).toStdString();
    // The attribute's value is its timecode, interpolated linearly between the samples.
    writeFile(path,
              "#usda 1.0\n"
              "(\n    timeCodesPerSecond = 24\n)\n\n"
              "def Xform \"Timed\"\n{\n"
              "    double natron:timeCode.timeSamples = {\n        0: 0,\n        48: 48,\n    }\n"
              "}\n");
    setProjectFrameRate(25.);
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    const auto valueAtFrame = [this, &reader](double frame, double* timeCode, double* value) {
        ScenePayload payload;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
        *timeCode = SceneOps::frameToTimeCode(payload, frame, getApp()->getProjectFrameRate());
        SceneQueryPool::Lease lease = borrow(payload);
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        ASSERT_TRUE(bool(stage));
        PXR_NS::UsdAttribute attribute = stage->GetAttributeAtPath(PXR_NS::SdfPath("/Timed.natron:timeCode"));
        ASSERT_TRUE(attribute.IsValid());
        ASSERT_TRUE(attribute.Get(value, PXR_NS::UsdTimeCode(*timeCode)));
    };

    double timeCode = 0., value = 0.;
    valueAtFrame(25., &timeCode, &value);
    EXPECT_DOUBLE_EQ(24., timeCode);
    EXPECT_DOUBLE_EQ(24., value);

    KnobDouble* frameOffset = dynamic_cast<KnobDouble*>(reader->getKnobByName(kUSDReadParamFrameOffset).get());
    ASSERT_TRUE(frameOffset != NULL);
    frameOffset->setValue(5.);
    // Frame 20 + 5 is frame 25: one second in, still the file's timecode 24.
    valueAtFrame(20., &timeCode, &value);
    EXPECT_DOUBLE_EQ(19.2, timeCode);
    EXPECT_NEAR(24., value, 1e-9);
}
