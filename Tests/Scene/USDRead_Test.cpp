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
#include "Engine/Nodes/Scene/StageCache.h"
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
    QFile f(QString::fromStdString(path));

    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.c_str(), (qint64)content.size());
}

// A layer defining one Xform at /<primName>. Two names of different lengths give files of
// different sizes, which the file stamp tells apart even within one mtime tick.
std::string
sceneDefining(const std::string& primName)
{
    return "#usda 1.0\n\ndef Xform \"" + primName + "\"\n{\n}\n";
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
    virtual void TearDown() OVERRIDE
    {
        if (_projectFrameRate) {
            _projectFrameRate->setValue(_originalFrameRate);
        }
        StageCache::instance().clear();

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

    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(payload);
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
    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).IsValid());
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
        PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(before);
        ASSERT_TRUE(bool(stage));
        EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Before")).IsValid());
    }

    writeFile(path, sceneDefining("AfterTheRewrite"));
    pressReload(reader);

    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &after, &error)) << error;
    EXPECT_NE(before.getStackHash(), after.getStackHash());
    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(after);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/AfterTheRewrite")).IsValid());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Before")).IsValid());
}

// The file's layer is keyed by the file's stamp: once the stamp moves (dropped here as a Read
// node's purge of the same path drops it), a new USDRead reads the file as it now is on disk,
// though a cached stage still holds the layer of the old content open. That stage is untouched.
TEST_F(USDReadTest, ANewReaderReadsTheRewrittenFileOnceItsStampMoves)
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
    PXR_NS::UsdStageRefPtr cached = StageCache::instance().getStage(before);
    ASSERT_TRUE(holdsOnly(cached, "Before"));

    writeFile(path, sceneDefining("AfterTheRewrite"));
    FileStampRegistry::instance().invalidate(path);
    NodePtr second = createReader(path);
    ASSERT_TRUE(bool(second));

    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(second, &after, &error)) << error;
    EXPECT_NE(before.getStackHash(), after.getStackHash());
    EXPECT_TRUE(holdsOnly(StageCache::instance().getStage(after), "AfterTheRewrite"));
    EXPECT_TRUE(holdsOnly(cached, "Before"));
}

// Reload edits no layer: while the main thread rewrites the file and reloads it, a render thread
// reading the stage composed before, and composing that payload again as a cache miss would,
// keeps seeing the content it was composed from. A reload in place would change it under the
// reader on the first composition after a Reload, besides racing the reads.
TEST_F(USDReadTest, ReloadLeavesTheStagesOfEarlierEvaluationsUntouched)
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
    // Held as the viewer holds the stage it shows, which keeps the file's first layer open.
    PXR_NS::UsdStageRefPtr shown = StageCache::instance().getStage(before);
    ASSERT_TRUE(holdsOnly(shown, "Before"));

    std::atomic<bool> done(false);
    std::atomic<int> reads(0);
    std::atomic<int> wrongReads(0);
    std::thread renderThread([&]() {
        while (!done.load()) {
            const bool shownIntact = holdsOnly(shown, "Before");
            const bool composedIntact = holdsOnly(SceneOps::compose(before), "Before");
            if (!shownIntact || !composedIntact) {
                ++wrongReads;
            }
            ++reads;
        }
    });
    while (reads.load() == 0) {
        std::this_thread::yield();
    }

    for (int i = 0; i < 20; ++i) {
        // A longer name each time, so every rewrite also changes the file's size.
        const std::string primName = "After" + std::string(i + 1, 'x');
        writeFile(path, sceneDefining(primName));
        pressReload(reader);
        ScenePayload after;
        if (evaluate(reader, &after, &error) != EffectInstance::eRenderRoIRetCodeOk) {
            ADD_FAILURE() << "Rewrite " << i << ": " << error;
            break;
        }
        EXPECT_TRUE(holdsOnly(StageCache::instance().getStage(after), primName)) << "Rewrite " << i;
    }
    done = true;
    renderThread.join();

    EXPECT_EQ(0, wrongReads.load());
}

TEST_F(USDReadTest, ResettingTheProjectEmptiesTheStageCache)
{
    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    ASSERT_TRUE(bool(StageCache::instance().getStage(payload)));
    ASSERT_LT((std::size_t)0, StageCache::instance().getNumEntries());
    reader.reset();

    getApp()->getProject()->reset(false, true);

    EXPECT_EQ((std::size_t)0, StageCache::instance().getNumEntries());
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
        PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(payload);
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
