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

#include <climits>
#include <filesystem>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoFileFormat.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoFile.h"
#include "Engine/Nodes/Geometry/GeoFileCache.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

static_assert(GeoFileCache::kNodeSpec.isWriter, "GeoFileCache is a render target");
static_assert(GeoFileCache::kNodeSpec.outputKind == eDataKindGeometry, "GeoFileCache produces geometry");

TEST(GeoFileCacheTest, VersionTokens)
{
    EXPECT_EQ(std::string("cache_v7/a.ngeo"), GeoFileCache::substituteVersion("cache_v$VER/a.ngeo", 7));
    EXPECT_EQ(std::string("cache_v007/a.$F4.ngeo"), GeoFileCache::substituteVersion("cache_v$VER3/a.$F4.ngeo", 7));
    EXPECT_EQ(std::string("v1234"), GeoFileCache::substituteVersion("v$VER2", 1234));
    EXPECT_EQ(std::string("a.ngeo"), GeoFileCache::substituteVersion("a.ngeo", 3));
}

class GeoFileCacheNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ASSERT_TRUE(_tmp.isValid());
        _cube = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOPRIMITIVE));
        ASSERT_TRUE(_cube != NULL);
        KnobChoicePtr type = std::dynamic_pointer_cast<KnobChoice>(_cube->getKnobByName(kGeoPrimitiveParamType));
        ASSERT_TRUE(type != NULL);
        type->setValue((int)eGeoShapeCube);

        _transform = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOTRANSFORM));
        ASSERT_TRUE(_transform != NULL);
        connectNodes(_cube, _transform, 0, true);
        KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(_transform->getKnobByName(kGeoTransformParamTranslate));
        ASSERT_TRUE(translate != NULL);
        translate->setValueAtTime(1, 0., ViewSpec::all(), 0);
        translate->setValueAtTime(5, 4., ViewSpec::all(), 0);

        _cache = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOFILECACHE));
        ASSERT_TRUE(_cache != NULL);
        connectNodes(_transform, _cache, 0, true);
        KnobOutputFilePtr file = std::dynamic_pointer_cast<KnobOutputFile>(_cache->getKnobByName(kGeoFileCacheParamFile));
        ASSERT_TRUE(file != NULL);
        file->setValue(path("v$VER3/cache.$F4.ngeo"));
        KnobChoicePtr frameRange = std::dynamic_pointer_cast<KnobChoice>(_cache->getKnobByName(kGeoFileCacheParamFrameRange));
        ASSERT_TRUE(frameRange != NULL);
        frameRange->setValue((int)GeoFileCache::eFrameRangeManual);
        setInt(kGeoFileCacheParamFirstFrame, 1);
        setInt(kGeoFileCacheParamLastFrame, 5);
    }

    virtual void TearDown() OVERRIDE
    {
        NodePtr* nodes[] = { &_cache, &_transform, &_cube };
        for (NodePtr* node : nodes) {
            if (*node) {
                appPTR->removeAllCacheEntriesForHolder(node->get(), true);
                (*node)->destroyNode(false, false);
                node->reset();
            }
        }
        BaseTest::TearDown();
    }

    std::string path(const char* name) const
    {
        return _tmp.filePath(QString::fromUtf8(name)).toStdString();
    }

    void setInt(const char* name,
                int value)
    {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(_cache->getKnobByName(name));
        ASSERT_TRUE(knob != NULL);
        knob->setValue(value);
    }

    void setMode(GeoFileCache::ModeEnum mode)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(_cache->getKnobByName(kGeoFileCacheParamMode));
        ASSERT_TRUE(knob != NULL);
        knob->setValue((int)mode);
    }

    void render(int first,
                int last)
    {
        std::list<AppInstance::RenderWork> works;
        OutputEffectInstance* writer = dynamic_cast<OutputEffectInstance*>(_cache->getEffectInstance().get());
        ASSERT_TRUE(writer != NULL);
        works.push_back(AppInstance::RenderWork(writer, first, last, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
        // The node re-stamps the files it wrote from a slot queued to the main thread.
        QCoreApplication::processEvents();
    }

    std::string persistentMessage() const
    {
        QString message;
        int type = 0;
        _cache->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }

    EffectInstance::RenderRoIRetCode pull(const NodePtr& node,
                                          double time,
                                          GeoDetailConstPtr* detail,
                                          std::string* error)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0), false /*byPassCache*/), detail, error);
    }

    U64 hashAt(const NodePtr& node,
               double time)
    {
        GeoDetailConstPtr detail;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, time, &detail, &error)) << error;
        if (!detail) {
            return 0;
        }

        return detail->contentHash();
    }

    QTemporaryDir _tmp;
    NodePtr _cube;
    NodePtr _transform;
    NodePtr _cache;
};

TEST_F(GeoFileCacheNodeTest, SavesTheRangeAndLoadsItBackExactly)
{
    std::vector<U64> saved;
    for (int frame = 1; frame <= 5; ++frame) {
        GeoDetailConstPtr detail;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_transform, frame, &detail, &error)) << error;
        ASSERT_TRUE(detail != nullptr);
        saved.push_back(geoFileNormalized(*detail).contentHash());
    }
    EXPECT_NE(saved[0], saved[4]);

    render(1, 5);
    const char* const files[] = { "v001/cache.0001.ngeo", "v001/cache.0002.ngeo", "v001/cache.0003.ngeo",
                                  "v001/cache.0004.ngeo", "v001/cache.0005.ngeo" };
    for (const char* name : files) {
        EXPECT_TRUE(QFile::exists(QString::fromStdString(path(name)))) << name;
    }
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("v001/cache.0006.ngeo"))));

    // Moving the input afterwards shows Load reads the files, not the chain.
    KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(_transform->getKnobByName(kGeoTransformParamTranslate));
    ASSERT_TRUE(translate != NULL);
    translate->setValueAtTime(3, 100., ViewSpec::all(), 1);

    setMode(GeoFileCache::eModeLoad);
    for (int frame = 1; frame <= 5; ++frame) {
        EXPECT_EQ(saved[frame - 1], hashAt(_cache, frame)) << "frame " << frame;
    }
    EXPECT_EQ(saved[2], geoFileRead(path("v001/cache.0003.ngeo")).contentHash());
}

TEST_F(GeoFileCacheNodeTest, AnotherVersionWritesBesideThePrevious)
{
    render(1, 1);
    setInt(kGeoFileCacheParamVersion, 2);
    render(2, 2);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(path("v001/cache.0001.ngeo"))));
    EXPECT_TRUE(QFile::exists(QString::fromStdString(path("v002/cache.0002.ngeo"))));
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("v002/cache.0001.ngeo"))));
}

TEST_F(GeoFileCacheNodeTest, OnlySaveModeWrites)
{
    setMode(GeoFileCache::eModePassThrough);
    render(1, 2);
    setMode(GeoFileCache::eModeLoad);
    render(1, 2);
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("v001/cache.0001.ngeo"))));
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("v001/cache.0002.ngeo"))));

    setMode(GeoFileCache::eModePassThrough);
    EXPECT_EQ(hashAt(_transform, 2.), hashAt(_cache, 2.));
}

// The abort lands between frames, so the render is pinned to one render thread; with more, later
// frames could already be in flight when it arrives.
TEST_F(GeoFileCacheNodeTest, CancellingMidRangeKeepsTheFramesWrittenWhole)
{
    struct ParallelRendersScope {
        int saved;

        ParallelRendersScope()
            : saved(appPTR->getCurrentSettings()->getNumberOfParallelRenders())
        {
            appPTR->getCurrentSettings()->setNumberOfParallelRenders(1);
        }

        ~ParallelRendersScope()
        {
            appPTR->getCurrentSettings()->setNumberOfParallelRenders(saved);
        }
    } parallelRenders;

    std::vector<U64> saved;
    for (int frame = 1; frame <= 2; ++frame) {
        GeoDetailConstPtr detail;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_transform, frame, &detail, &error)) << error;
        ASSERT_TRUE(detail != nullptr);
        saved.push_back(geoFileNormalized(*detail).contentHash());
    }

    OutputEffectInstance* writer = dynamic_cast<OutputEffectInstance*>(_cache->getEffectInstance().get());
    ASSERT_TRUE(writer != NULL);
    RenderEnginePtr engine = writer->getRenderEngine();
    ASSERT_TRUE(engine != nullptr);
    int framesReported = 0;
    QMetaObject::Connection abortOnSecondFrame = QObject::connect(engine.get(), &RenderEngine::frameRendered, engine.get(), [&framesReported, engine](int /*time*/, double /*progress*/) {
        if (++framesReported == 2) {
            engine->abortRenderingNoRestart();
        } }, Qt::DirectConnection);
    render(1, 5);
    QObject::disconnect(abortOnSecondFrame);

    EXPECT_EQ(2, framesReported);
    EXPECT_EQ(saved[0], geoFileRead(path("v001/cache.0001.ngeo")).contentHash());
    EXPECT_EQ(saved[1], geoFileRead(path("v001/cache.0002.ngeo")).contentHash());
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("v001/cache.0004.ngeo"))));
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("v001/cache.0005.ngeo"))));
    for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(path("v001"))) {
        const std::string name = e.path().filename().string();
        EXPECT_EQ(std::string::npos, name.find(".tmp")) << "temporary file left behind: " << name;
    }
}

TEST_F(GeoFileCacheNodeTest, LoadingAfterSavingAgainReturnsTheNewGeometry)
{
    render(1, 1);
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOFILE));
    ASSERT_TRUE(reader != NULL);
    KnobFilePtr readerFile = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kGeoFileParamFile));
    ASSERT_TRUE(readerFile != NULL);
    readerFile->setValue(path("v001/cache.$F4.ngeo"));

    setMode(GeoFileCache::eModeLoad);
    const U64 before = hashAt(_cache, 1);
    EXPECT_EQ(before, hashAt(reader, 1));

    KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(_transform->getKnobByName(kGeoTransformParamTranslate));
    ASSERT_TRUE(translate != NULL);
    translate->setValueAtTime(1, 50., ViewSpec::all(), 0);
    setMode(GeoFileCache::eModeSave);
    render(1, 1);
    setMode(GeoFileCache::eModeLoad);

    const U64 written = geoFileRead(path("v001/cache.0001.ngeo")).contentHash();
    EXPECT_NE(before, written);
    EXPECT_EQ(written, hashAt(_cache, 1));
    EXPECT_EQ(written, hashAt(reader, 1));

    appPTR->removeAllCacheEntriesForHolder(reader.get(), true);
    reader->destroyNode(false, false);
}

TEST_F(GeoFileCacheNodeTest, AFramelessNameCannotHoldSeveralFrames)
{
    KnobOutputFilePtr file = std::dynamic_pointer_cast<KnobOutputFile>(_cache->getKnobByName(kGeoFileCacheParamFile));
    ASSERT_TRUE(file != NULL);
    file->setValue(path("single.ngeo"));

    render(1, 3);
    EXPECT_FALSE(QFile::exists(QString::fromStdString(path("single.ngeo"))));
    EXPECT_NE(std::string::npos, persistentMessage().find("no frame number")) << persistentMessage();

    render(2, 2);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(path("single.ngeo"))));
    EXPECT_TRUE(persistentMessage().empty()) << persistentMessage();
}
