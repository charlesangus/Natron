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
#include <list>
#include <map>
#include <memory>

#include <gtest/gtest.h>

#include <QFuture>
#include <QString>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/DeviceImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameArgsOverlay.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kConstantPluginID = "Constant";
const char* const kGradePluginID = "Grade";

template <typename F>
void
runOnNewThread(F body)
{
    std::unique_ptr<QThread> thread(QThread::create(body));

    thread->start();
    thread->wait();
}

ImagePtr
makeLocalImage(const ImageLayerDesc& components,
               const RectI& bounds)
{
    RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(components, rod, bounds, /*mipmapLevel=*/0, /*par=*/1.,
                                   eImageBitDepthFloat, eImageFieldingOrderNone, /*useBitmap=*/false);
}

FrameStore::TaskKey
makeKey(const NodePtr& node,
        double time)
{
    FrameStore::TaskKey key;

    key.node = node;
    key.time = time;
    key.view = ViewIdx(0);
    key.mipmapLevel = 0;

    return key;
}

class FakeDeviceImage
    : public DeviceImage {
public:
    FakeDeviceImage(const RectI& bounds,
                    std::size_t bytes)
        : _bounds(bounds)
        , _bytes(bytes)
        , downloads(0)
    {
    }

    virtual std::size_t bytes() const OVERRIDE FINAL { return _bytes; }

    virtual RectI bounds() const OVERRIDE FINAL { return _bounds; }

    virtual int nComps() const OVERRIDE FINAL { return 4; }

    virtual ImagePtr downloadToRam() OVERRIDE FINAL
    {
        ++downloads;

        return makeLocalImage(ImageLayerDesc::getRGBAComponents(), _bounds);
    }

    virtual ImagePtr evictToRam() OVERRIDE FINAL
    {
        ImagePtr image = downloadToRam();

        _bytes = 0;

        return image;
    }

    int downloads;

private:
    RectI _bounds;
    std::size_t _bytes;
};

} // namespace

class FrameRenderContextTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    FrameRenderContextPtr makeContext(const NodePtr& root)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);

        return FrameRenderContext::create(1.,
                                          ViewIdx(0),
                                          false /*isRenderUserInteraction*/,
                                          false /*isSequential*/,
                                          abortInfo,
                                          root,
                                          0 /*textureIndex*/,
                                          getApp()->getTimeLine().get(),
                                          NodePtr(),
                                          false /*isAnalysis*/,
                                          false /*draftMode*/,
                                          RenderStatsPtr());
    }
};

TEST_F(FrameRenderContextTest, TaskThreadGetsFrameArgsFromContext)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade1 = createNode(QString::fromUtf8(kGradePluginID));
    NodePtr grade2 = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(constant) && bool(grade1) && bool(grade2));
    connectNodes(constant, grade1, 0, true);
    connectNodes(grade1, grade2, 0, true);
    EffectInstancePtr constantEffect = constant->getEffectInstance();
    ASSERT_TRUE(bool(constantEffect));

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    FrameRenderContextPtr context = FrameRenderContext::create(1.,
                                                               ViewIdx(0),
                                                               false /*isRenderUserInteraction*/,
                                                               false /*isSequential*/,
                                                               abortInfo,
                                                               grade2,
                                                               0 /*textureIndex*/,
                                                               getApp()->getTimeLine().get(),
                                                               NodePtr(),
                                                               false /*isAnalysis*/,
                                                               false /*draftMode*/,
                                                               RenderStatsPtr());
    ASSERT_TRUE(bool(context));
    ParallelRenderArgsPtr constantArgs = context->getArgs(constant);
    ASSERT_TRUE(bool(constantArgs));
    EXPECT_EQ(1., constantArgs->time);
    EXPECT_TRUE(bool(context->getArgs(grade1)));
    EXPECT_TRUE(bool(context->getArgs(grade2)));

    FrameArgsOverlay::resetStats();
    ParallelRenderArgsPtr argsBeforeScope, workerArgs, nestedArgs, argsAfterScope;
    std::size_t holdersAfterScope = 0;
    QFuture<void> future = QtConcurrent::run([&]() {
        QThread* worker = QThread::currentThread();

        argsBeforeScope = constantEffect->getParallelRenderArgsTLS();
        {
            AppTLS::FrameContextScope scope(context.get());
            workerArgs = constantEffect->getParallelRenderArgsTLS();
            const FrameRenderContext* workerFrame = AppTLS::currentFrameContext();
            const FrameArgsOverlayPtr workerHead = FrameArgsOverlay::currentHead();

            runOnNewThread([&]() {
                AppTLS::SpawnedThreadScope spawned(worker, workerFrame, workerHead, NULL);
                nestedArgs = constantEffect->getParallelRenderArgsTLS();
            });
        }
        holdersAfterScope = AppTLS::getNumHoldersWithDataForCurrentThread();
        argsAfterScope = constantEffect->getParallelRenderArgsTLS();
    });
    future.waitForFinished();

    EXPECT_FALSE(bool(argsBeforeScope)) << "the pool thread must start without frame args for the Constant";
    EXPECT_EQ(constantArgs.get(), workerArgs.get());
    EXPECT_EQ(constantArgs.get(), nestedArgs.get());
    EXPECT_EQ(0u, holdersAfterScope);
    EXPECT_FALSE(bool(argsAfterScope));
    EXPECT_FALSE(bool(constantEffect->getParallelRenderArgsTLS())) << "the context must not install args on the thread that created it";
    EXPECT_EQ(0u, FrameArgsOverlay::stats().mismatches);
}

TEST_F(FrameRenderContextTest, ThreadLocalFrameArgsFollowANestedTaskFrame)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(constant) && bool(grade));
    connectNodes(constant, grade, 0, true);
    EffectInstancePtr constantEffect = constant->getEffectInstance();
    EffectInstancePtr gradeEffect = grade->getEffectInstance();
    ASSERT_TRUE(bool(constantEffect) && bool(gradeEffect));

    FrameRenderContextPtr outer = makeContext(grade);
    ASSERT_TRUE(bool(outer));
    FrameRenderContextPtr extension = FrameRenderContext::createExtension(outer, std::make_shared<FrameRequestMap>(), 1);
    ASSERT_TRUE(bool(extension));
    ParallelRenderArgsPtr outerArgs = outer->getArgs(constant);
    ParallelRenderArgsPtr extensionArgs = extension->getArgs(constant);
    ASSERT_TRUE(bool(outerArgs) && bool(extensionArgs));
    ASSERT_NE(outerArgs.get(), extensionArgs.get());
    ParallelRenderArgsPtr pushed = std::make_shared<ParallelRenderArgs>(*outerArgs);

    FrameArgsOverlay::resetStats();
    ParallelRenderArgsPtr beforeNested, insideNested, insideNestedGrade, afterNested, pushedInside, pushedAfter;
    QFuture<void> future = QtConcurrent::run([&]() {
        AppTLS::FrameContextScope task(outer.get(), 1, 0, true);
        beforeNested = constantEffect->getParallelRenderArgsTLS();
        {
            FrameArgsToken token(gradeEffect.get(), pushed);
            {
                AppTLS::FrameContextScope nested(extension.get(), 1, 0, true);
                insideNested = constantEffect->getParallelRenderArgsTLS();
            }
            pushedInside = gradeEffect->getParallelRenderArgsTLS();
        }
        {
            AppTLS::FrameContextScope nested(extension.get(), 1, 0, true);
            insideNestedGrade = gradeEffect->getParallelRenderArgsTLS();
        }
        afterNested = constantEffect->getParallelRenderArgsTLS();
        pushedAfter = gradeEffect->getParallelRenderArgsTLS();
    });
    future.waitForFinished();

    EXPECT_EQ(outerArgs.get(), beforeNested.get());
    EXPECT_EQ(extensionArgs.get(), insideNested.get()) << "args taken from the outer frame must not answer for a nested task's frame";
    EXPECT_EQ(pushed.get(), pushedInside.get());
    EXPECT_EQ(extension->getArgs(grade).get(), insideNestedGrade.get());
    EXPECT_EQ(outerArgs.get(), afterNested.get()) << "args taken from the nested frame must not outlive it";
    EXPECT_EQ(outer->getArgs(grade).get(), pushedAfter.get());
    EXPECT_EQ(0u, FrameArgsOverlay::stats().mismatches);
}

TEST_F(FrameRenderContextTest, ContextWithoutSchedulerMarkerReportsNoScheduledTask)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    ASSERT_NE(constant, nullptr);

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    FrameRenderContextPtr context = FrameRenderContext::create(1.,
                                                               ViewIdx(0),
                                                               false /*isRenderUserInteraction*/,
                                                               false /*isSequential*/,
                                                               abortInfo,
                                                               constant,
                                                               0 /*textureIndex*/,
                                                               getApp()->getTimeLine().get(),
                                                               NodePtr(),
                                                               false /*isAnalysis*/,
                                                               false /*draftMode*/,
                                                               RenderStatsPtr());
    ASSERT_TRUE(bool(context));

    EXPECT_FALSE(AppTLS::isRunningScheduledTask());
    {
        AppTLS::FrameContextScope plain(context.get());
        EXPECT_EQ(context.get(), AppTLS::currentFrameContext());
        EXPECT_FALSE(AppTLS::isRunningScheduledTask());
        {
            AppTLS::FrameContextScope task(context.get(), 1, 0, true);
            EXPECT_TRUE(AppTLS::isRunningScheduledTask());
        }
        EXPECT_FALSE(AppTLS::isRunningScheduledTask());
    }
    EXPECT_EQ(nullptr, AppTLS::currentFrameContext());
}

TEST_F(FrameRenderContextTest, FrameArgsChainRestoresNestedScopes)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(constant) && bool(grade));
    const TLSHolderBase* a = constant->getEffectInstance()->getTLSHolder();
    const TLSHolderBase* b = grade->getEffectInstance()->getTLSHolder();
    ASSERT_NE(a, b);
    ParallelRenderArgsPtr outer = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr inner = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr other = std::make_shared<ParallelRenderArgs>();

    ASSERT_FALSE(bool(FrameArgsOverlay::currentHead()));
    {
        FrameArgsScope outerScope(a, outer);
        EXPECT_EQ(outer.get(), FrameArgsOverlay::lookup(a).get());
        EXPECT_FALSE(bool(FrameArgsOverlay::lookup(b)));
        {
            FrameArgsScope innerScope(a, inner);
            EXPECT_EQ(inner.get(), FrameArgsOverlay::lookup(a).get());
            {
                FrameArgsScope otherScope(b, other);
                EXPECT_EQ(other.get(), FrameArgsOverlay::lookup(b).get());
                EXPECT_EQ(inner.get(), FrameArgsOverlay::lookup(a).get());
                EXPECT_EQ(3u, FrameArgsOverlay::currentHead()->depth());
            }
            EXPECT_FALSE(bool(FrameArgsOverlay::lookup(b)));
            EXPECT_EQ(inner.get(), FrameArgsOverlay::lookup(a).get());
        }
        EXPECT_EQ(outer.get(), FrameArgsOverlay::lookup(a).get());
    }
    EXPECT_FALSE(bool(FrameArgsOverlay::currentHead()));
    EXPECT_FALSE(bool(FrameArgsOverlay::lookup(a)));
}

TEST_F(FrameRenderContextTest, FrameArgsChainShadowsPerEffect)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(constant) && bool(grade));
    const TLSHolderBase* a = constant->getEffectInstance()->getTLSHolder();
    const TLSHolderBase* b = grade->getEffectInstance()->getTLSHolder();
    ParallelRenderArgsPtr firstA = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr lastA = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr batchB = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr innerB = std::make_shared<ParallelRenderArgs>();

    FrameArgsOverlay::Entries entries;
    entries.push_back(FrameArgsOverlay::Entry(a, firstA));
    entries.push_back(FrameArgsOverlay::Entry(b, batchB));
    entries.push_back(FrameArgsOverlay::Entry(a, lastA));
    {
        FrameArgsScope batch(entries);
        EXPECT_EQ(1u, FrameArgsOverlay::currentHead()->depth()) << "a batch is one link";
        EXPECT_EQ(lastA.get(), FrameArgsOverlay::lookup(a).get()) << "the last of several entries for an effect wins";
        EXPECT_EQ(batchB.get(), FrameArgsOverlay::lookup(b).get());
        {
            FrameArgsScope innerScope(b, innerB);
            EXPECT_EQ(innerB.get(), FrameArgsOverlay::lookup(b).get());
            EXPECT_EQ(lastA.get(), FrameArgsOverlay::lookup(a).get()) << "shadowing one effect leaves the others visible";
        }
        EXPECT_EQ(batchB.get(), FrameArgsOverlay::lookup(b).get());
    }
    EXPECT_FALSE(bool(FrameArgsOverlay::lookup(a)));
    EXPECT_FALSE(bool(FrameArgsOverlay::lookup(b)));
}

TEST_F(FrameRenderContextTest, FrameArgsSpawnedThreadSeesTheCapturedChain)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(constant) && bool(grade));
    const TLSHolderBase* a = constant->getEffectInstance()->getTLSHolder();
    const TLSHolderBase* b = grade->getEffectInstance()->getTLSHolder();
    ParallelRenderArgsPtr captured = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr later = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr laterB = std::make_shared<ParallelRenderArgs>();

    QThread* spawner = QThread::currentThread();
    FrameArgsScope capturedScope(a, captured);
    const FrameArgsOverlayPtr head = FrameArgsOverlay::currentHead();
    FrameArgsScope laterScope(a, later);
    FrameArgsScope laterBScope(b, laterB);

    ParallelRenderArgsPtr beforeScope, insideA, insideB, afterScope;
    bool headAfterScope = true;
    runOnNewThread([&]() {
        beforeScope = FrameArgsOverlay::lookup(a);
        {
            AppTLS::SpawnedThreadScope scope(spawner, NULL, head, NULL);
            insideA = FrameArgsOverlay::lookup(a);
            insideB = FrameArgsOverlay::lookup(b);
        }
        headAfterScope = bool(FrameArgsOverlay::currentHead());
        afterScope = FrameArgsOverlay::lookup(a);
    });

    EXPECT_FALSE(bool(beforeScope));
    EXPECT_EQ(captured.get(), insideA.get()) << "the spawned thread must read the chain captured when it was created";
    EXPECT_FALSE(bool(insideB)) << "pushes made after the capture must stay invisible to the spawned thread";
    EXPECT_FALSE(headAfterScope);
    EXPECT_FALSE(bool(afterScope));
    EXPECT_EQ(later.get(), FrameArgsOverlay::lookup(a).get());
    EXPECT_EQ(laterB.get(), FrameArgsOverlay::lookup(b).get());
}

TEST_F(FrameRenderContextTest, FrameArgsChainFallsBackToTheContext)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    NodePtr stray = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(constant) && bool(grade) && bool(stray));
    connectNodes(constant, grade, 0, true);
    const TLSHolderBase* a = constant->getEffectInstance()->getTLSHolder();
    const TLSHolderBase* b = grade->getEffectInstance()->getTLSHolder();
    const TLSHolderBase* s = stray->getEffectInstance()->getTLSHolder();

    FrameRenderContextPtr context = makeContext(grade);
    ASSERT_TRUE(bool(context));
    ParallelRenderArgsPtr constantArgs = context->getArgs(constant);
    ParallelRenderArgsPtr gradeArgs = context->getArgs(grade);
    ASSERT_TRUE(bool(constantArgs) && bool(gradeArgs));
    ASSERT_FALSE(bool(context->getArgs(stray)));
    ParallelRenderArgsPtr pushed = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr outerGrade = std::make_shared<ParallelRenderArgs>();
    ParallelRenderArgsPtr outerStray = std::make_shared<ParallelRenderArgs>();

    {
        AppTLS::FrameContextScope plain(context.get());
        EXPECT_EQ(constantArgs.get(), FrameArgsOverlay::lookup(a).get()) << "a chain with no base answers from the thread's frame";
        {
            FrameArgsScope scope(a, pushed);
            EXPECT_EQ(pushed.get(), FrameArgsOverlay::lookup(a).get());
            EXPECT_EQ(gradeArgs.get(), FrameArgsOverlay::lookup(b).get());
        }
        EXPECT_EQ(constantArgs.get(), FrameArgsOverlay::lookup(a).get());
    }

    {
        FrameArgsScope gradeScope(b, outerGrade);
        FrameArgsScope strayScope(s, outerStray);
        {
            AppTLS::FrameContextScope task(context.get(), 1, 0, true);
            EXPECT_EQ(1u, FrameArgsOverlay::currentHead()->depth()) << "a scheduler task starts a chain of its own";
            EXPECT_EQ(gradeArgs.get(), FrameArgsOverlay::lookup(b).get()) << "a scheduler task must not read args pushed by what ran it inline";
            EXPECT_FALSE(bool(FrameArgsOverlay::lookup(s))) << "the base answers from its frame alone";
            {
                FrameArgsScope scope(a, pushed);
                EXPECT_EQ(pushed.get(), FrameArgsOverlay::lookup(a).get());
            }
            EXPECT_EQ(constantArgs.get(), FrameArgsOverlay::lookup(a).get());
        }
        EXPECT_EQ(outerGrade.get(), FrameArgsOverlay::lookup(b).get());
        EXPECT_EQ(outerStray.get(), FrameArgsOverlay::lookup(s).get());
    }
    EXPECT_FALSE(bool(FrameArgsOverlay::currentHead()));
}

TEST_F(FrameRenderContextTest, FrameArgsChainIsEmptyWithoutAContext)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    ASSERT_TRUE(bool(constant));
    const TLSHolderBase* a = constant->getEffectInstance()->getTLSHolder();

    ASSERT_EQ(nullptr, AppTLS::currentFrameContext());
    EXPECT_FALSE(bool(FrameArgsOverlay::currentHead()));
    EXPECT_FALSE(bool(FrameArgsOverlay::lookup(a)));
    EXPECT_FALSE(bool(FrameArgsOverlay::lookup(NULL, a, NULL)));

    const FrameArgsOverlayPtr emptyBase = FrameArgsOverlay::makeBase(NULL);
    EXPECT_FALSE(bool(FrameArgsOverlay::lookup(emptyBase.get(), a, NULL)));
}

TEST_F(FrameRenderContextTest, FrameArgsChainRecordsItsDepth)
{
    NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
    ASSERT_TRUE(bool(constant));
    const TLSHolderBase* a = constant->getEffectInstance()->getTLSHolder();
    ParallelRenderArgsPtr args = std::make_shared<ParallelRenderArgs>();

    FrameArgsOverlay::resetStats();
    {
        FrameArgsScope first(a, args);
        FrameArgsScope second(a, args);
        FrameArgsScope third(a, args);
    }
    const FrameArgsOverlay::Stats stats = FrameArgsOverlay::stats();
    EXPECT_GE(stats.maxDepth, 3u);
    EXPECT_EQ(0u, stats.outOfOrderPops);
}

TEST_F(FrameRenderContextTest, FrameStorePutFindRelease)
{
    FrameStore store;
    const RectI bounds(0, 0, 8, 8);
    ImagePtr rgba = makeLocalImage(ImageLayerDesc::getRGBAComponents(), bounds);
    ASSERT_TRUE(bool(rgba));

    const FrameStore::TaskKey key = makeKey(NodePtr(), 1.);
    std::map<ImageLayerDesc, ImagePtr> layers;
    layers[ImageLayerDesc::getRGBAComponents()] = rgba;
    store.put(key, layers, bounds, 2);
    EXPECT_GT(store.bytesInFlight(), 0u);

    std::list<ImageLayerDesc> neededRGBA;
    neededRGBA.push_back(ImageLayerDesc::getRGBAComponents());
    std::list<ImagePtr> found;
    ASSERT_TRUE(store.find(key, neededRGBA, RectI(2, 2, 6, 6), &found));
    ASSERT_EQ(1u, found.size());
    EXPECT_EQ(rgba.get(), found.front().get());

    std::list<ImageLayerDesc> neededAlpha;
    neededAlpha.push_back(ImageLayerDesc::getAlphaComponents());
    std::list<ImagePtr> missingLayer;
    EXPECT_FALSE(store.find(key, neededAlpha, RectI(2, 2, 6, 6), &missingLayer));
    EXPECT_TRUE(missingLayer.empty());

    std::list<ImagePtr> outsideBounds;
    EXPECT_FALSE(store.find(key, neededRGBA, RectI(4, 4, 12, 12), &outsideBounds));
    EXPECT_TRUE(outsideBounds.empty());

    std::list<ImagePtr> otherTime;
    EXPECT_FALSE(store.find(makeKey(NodePtr(), 2.), neededRGBA, RectI(2, 2, 6, 6), &otherTime));

    store.release(key);
    std::list<ImagePtr> afterFirstRelease;
    EXPECT_TRUE(store.find(key, neededRGBA, RectI(2, 2, 6, 6), &afterFirstRelease));
    EXPECT_GT(store.bytesInFlight(), 0u);

    store.release(key);
    std::list<ImagePtr> afterLastRelease;
    EXPECT_FALSE(store.find(key, neededRGBA, RectI(2, 2, 6, 6), &afterLastRelease));
    EXPECT_EQ(0u, store.bytesInFlight());

    store.put(key, layers, bounds, 1);
    EXPECT_GT(store.bytesInFlight(), 0u);
    store.clear();
    EXPECT_EQ(0u, store.bytesInFlight());
    std::list<ImagePtr> afterClear;
    EXPECT_FALSE(store.find(key, neededRGBA, RectI(2, 2, 6, 6), &afterClear));
}

// A cached image may be larger than what the task that stored it rendered, so only that part may be found.
TEST_F(FrameRenderContextTest, FrameStoreFindsOnlyTheRenderedRoI)
{
    FrameStore store;
    ImagePtr rgba = makeLocalImage(ImageLayerDesc::getRGBAComponents(), RectI(0, 0, 8, 8));
    ASSERT_TRUE(bool(rgba));

    const FrameStore::TaskKey key = makeKey(NodePtr(), 1.);
    std::map<ImageLayerDesc, ImagePtr> layers;
    layers[ImageLayerDesc::getRGBAComponents()] = rgba;
    store.put(key, layers, RectI(0, 0, 4, 4), 1);

    std::list<ImageLayerDesc> needed;
    needed.push_back(ImageLayerDesc::getRGBAComponents());
    std::list<ImagePtr> inside;
    EXPECT_TRUE(store.find(key, needed, RectI(1, 1, 4, 4), &inside));
    std::list<ImagePtr> pastRendered;
    EXPECT_FALSE(store.find(key, needed, RectI(2, 2, 6, 6), &pastRendered));
    EXPECT_TRUE(pastRendered.empty());
    store.release(key);
}

TEST_F(FrameRenderContextTest, FrameStoreCountsAnImageStoredUnderTwoKeysOnce)
{
    FrameStore store;
    ImagePtr rgba = makeLocalImage(ImageLayerDesc::getRGBAComponents(), RectI(0, 0, 8, 8));
    ASSERT_TRUE(bool(rgba));
    const std::size_t imageBytes = rgba->size();
    ASSERT_GT(imageBytes, 0u);

    std::map<ImageLayerDesc, ImagePtr> layers;
    layers[ImageLayerDesc::getRGBAComponents()] = rgba;
    const FrameStore::TaskKey input = makeKey(NodePtr(), 1.);
    const FrameStore::TaskKey identity = makeKey(NodePtr(), 2.);
    const RectI rendered(0, 0, 8, 8);

    store.put(input, layers, rendered, 1);
    EXPECT_EQ(imageBytes, store.bytesInFlight());
    store.put(identity, layers, rendered, 1);
    EXPECT_EQ(imageBytes, store.bytesInFlight());

    store.put(identity, layers, rendered, 1);
    EXPECT_EQ(imageBytes, store.bytesInFlight()) << "storing the same image again under a key must not change the count";

    store.release(input);
    EXPECT_EQ(imageBytes, store.bytesInFlight()) << "the image is still stored under the other key";
    std::list<ImageLayerDesc> needed;
    needed.push_back(ImageLayerDesc::getRGBAComponents());
    std::list<ImagePtr> found;
    EXPECT_TRUE(store.find(identity, needed, RectI(0, 0, 8, 8), &found));

    store.release(identity);
    EXPECT_EQ(0u, store.bytesInFlight());

    ImagePtr other = makeLocalImage(ImageLayerDesc::getRGBAComponents(), RectI(0, 0, 4, 4));
    ASSERT_TRUE(bool(other));
    std::map<ImageLayerDesc, ImagePtr> otherLayers;
    otherLayers[ImageLayerDesc::getRGBAComponents()] = other;
    store.put(input, layers, rendered, 1);
    store.put(identity, otherLayers, RectI(0, 0, 4, 4), 1);
    EXPECT_EQ(imageBytes + other->size(), store.bytesInFlight());
    store.clear();
    EXPECT_EQ(0u, store.bytesInFlight());
    store.put(input, layers, rendered, 1);
    EXPECT_EQ(imageBytes, store.bytesInFlight()) << "clear() must forget the images it counted";
    store.release(input);
    EXPECT_EQ(0u, store.bytesInFlight());
}

TEST_F(FrameRenderContextTest, FrameStoreDownloadsADeviceOnlyEntryOnce)
{
    FrameStore store;
    const RectI bounds(0, 0, 8, 8);
    std::shared_ptr<FakeDeviceImage> device = std::make_shared<FakeDeviceImage>(bounds, 1000);
    std::map<ImageLayerDesc, DeviceImagePtr> layers;
    layers[ImageLayerDesc::getRGBAComponents()] = device;
    const FrameStore::TaskKey key = makeKey(NodePtr(), 1.);
    store.putDevice(key, layers, bounds, 3);

    EXPECT_EQ(0u, store.hostBytesInFlight());
    EXPECT_EQ(1000u, store.deviceBytesInFlight());

    std::list<ImageLayerDesc> needed;
    needed.push_back(ImageLayerDesc::getRGBAComponents());
    std::list<DeviceImagePtr> onDevice;
    ASSERT_TRUE(store.findDevice(key, needed, bounds, &onDevice));
    EXPECT_EQ(device.get(), onDevice.front().get());
    EXPECT_EQ(0, store.downloadCount());

    std::list<ImagePtr> first;
    std::list<ImagePtr> second;
    ASSERT_TRUE(store.find(key, needed, RectI(1, 1, 5, 5), &first));
    ASSERT_TRUE(store.find(key, needed, RectI(2, 2, 6, 6), &second));
    EXPECT_EQ(first.front().get(), second.front().get());
    EXPECT_EQ(1, store.downloadCount());
    EXPECT_EQ(1, device->downloads);

    std::list<ImagePtr> outside;
    EXPECT_FALSE(store.find(key, needed, RectI(4, 4, 12, 12), &outside));

    const std::size_t hostBytes = first.front()->size();
    EXPECT_EQ(hostBytes, store.hostBytesInFlight());
    EXPECT_EQ(hostBytes, store.bytesInFlight());
    EXPECT_EQ(1000u, store.deviceBytesInFlight());

    store.release(key);
    store.release(key);
    EXPECT_EQ(hostBytes, store.hostBytesInFlight());
    store.release(key);
    EXPECT_EQ(0u, store.hostBytesInFlight());
    EXPECT_EQ(0u, store.deviceBytesInFlight());
    std::list<DeviceImagePtr> afterRelease;
    EXPECT_FALSE(store.findDevice(key, needed, bounds, &afterRelease));
}

TEST_F(FrameRenderContextTest, FrameStoreAccountsHostAndDeviceBytesApart)
{
    FrameStore store;
    const RectI bounds(0, 0, 8, 8);
    ImagePtr rgba = makeLocalImage(ImageLayerDesc::getRGBAComponents(), bounds);
    ASSERT_TRUE(bool(rgba));
    std::map<ImageLayerDesc, ImagePtr> ramLayers;
    ramLayers[ImageLayerDesc::getRGBAComponents()] = rgba;
    std::map<ImageLayerDesc, DeviceImagePtr> deviceLayers;
    deviceLayers[ImageLayerDesc::getRGBAComponents()] = std::make_shared<FakeDeviceImage>(bounds, 500);

    const FrameStore::TaskKey ramKey = makeKey(NodePtr(), 1.);
    const FrameStore::TaskKey deviceKey = makeKey(NodePtr(), 2.);
    store.put(ramKey, ramLayers, bounds, 1);
    EXPECT_EQ(rgba->size(), store.hostBytesInFlight());
    EXPECT_EQ(0u, store.deviceBytesInFlight());
    store.putDevice(deviceKey, deviceLayers, bounds, 1);
    EXPECT_EQ(rgba->size(), store.hostBytesInFlight());
    EXPECT_EQ(500u, store.deviceBytesInFlight());

    store.release(ramKey);
    EXPECT_EQ(0u, store.hostBytesInFlight());
    EXPECT_EQ(500u, store.deviceBytesInFlight());
    store.release(deviceKey);
    EXPECT_EQ(0u, store.hostBytesInFlight());
    EXPECT_EQ(0u, store.deviceBytesInFlight());

    store.putDevice(deviceKey, deviceLayers, bounds, 1);
    store.clear();
    EXPECT_EQ(0u, store.deviceBytesInFlight());
}

TEST_F(FrameRenderContextTest, FrameStoreEvictedEntryServesRamFindsWithoutDownloading)
{
    FrameStore store;
    const RectI bounds(0, 0, 8, 8);
    std::shared_ptr<FakeDeviceImage> device = std::make_shared<FakeDeviceImage>(bounds, 1000);
    std::map<ImageLayerDesc, DeviceImagePtr> layers;
    layers[ImageLayerDesc::getRGBAComponents()] = device;
    const FrameStore::TaskKey key = makeKey(NodePtr(), 1.);
    store.putDevice(key, layers, bounds, 2);

    EXPECT_EQ(1000u, store.evictToRam(key));
    EXPECT_EQ(0u, store.deviceBytesInFlight());
    EXPECT_GT(store.hostBytesInFlight(), 0u);
    const int downloadsAfterEviction = store.downloadCount();

    std::list<ImageLayerDesc> needed;
    needed.push_back(ImageLayerDesc::getRGBAComponents());
    std::list<ImagePtr> found;
    ASSERT_TRUE(store.find(key, needed, bounds, &found));
    EXPECT_EQ(downloadsAfterEviction, store.downloadCount());
    EXPECT_EQ(1, device->downloads);
    std::list<DeviceImagePtr> onDevice;
    EXPECT_FALSE(store.findDevice(key, needed, bounds, &onDevice));

    store.release(key);
    EXPECT_TRUE(store.find(key, needed, bounds, &found)) << "eviction must not change the consumer count";
    store.release(key);
    EXPECT_EQ(0u, store.hostBytesInFlight());
    EXPECT_EQ(0u, store.deviceBytesInFlight());
}
