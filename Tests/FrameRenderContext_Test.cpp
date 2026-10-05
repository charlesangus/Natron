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
#include "Engine/EffectInstance.h"
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

const char* const kConstantPluginID = "net.sf.openfx.ConstantPlugin";
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

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

} // namespace

class FrameRenderContextTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
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

    ParallelRenderArgsPtr argsBeforeScope, workerArgs, nestedArgs, argsAfterScope;
    std::size_t holdersAfterScope = 0;
    QFuture<void> future = QtConcurrent::run([&]() {
        QThread* worker = QThread::currentThread();

        argsBeforeScope = constantEffect->getParallelRenderArgsTLS();
        {
            AppTLS::FrameContextScope scope(context.get());
            workerArgs = constantEffect->getParallelRenderArgsTLS();

            runOnNewThread([&]() {
                appPTR->getAppTLS()->softCopy(worker, QThread::currentThread());
                nestedArgs = constantEffect->getParallelRenderArgsTLS();
                appPTR->getAppTLS()->cleanupTLSForThread();
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
