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
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QThread>
#include <QThreadPool>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kConstantPluginID = "net.sf.openfx.ConstantPlugin";
const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

template <typename F>
void
runOnNewThread(F body)
{
    std::unique_ptr<QThread> thread(QThread::create(body));

    thread->start();
    thread->wait();
}

} // namespace

class GraphScalingTLS
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    ParallelRenderArgsPtr makeFrameArgs(double time,
                                        const AbortableRenderInfoPtr& abortInfo,
                                        const NodePtr& treeRoot)
    {
        ParallelRenderArgsPtr args = std::make_shared<ParallelRenderArgs>();

        args->time = time;
        args->view = ViewIdx(0);
        args->abortInfo = abortInfo;
        args->treeRoot = treeRoot;
        args->timeline = getApp()->getTimeLine().get();

        return args;
    }

    // Renders a width x height window of node at time into pixels, as RGBA floats row by row.
    bool renderWindow(const NodePtr& node,
                      double time,
                      int width,
                      int height,
                      std::vector<float>* pixels,
                      std::string* error)
    {
        pixels->clear();

        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 false /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstancePtr effect = node->getEffectInstance();

        const RectI window(0, 0, width, height);
        const RectD canonicalWindow(0., 0., width, height);

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, node, request) == eStatusFailed) {
            *error = "request pass failed";

            return false;
        }
        frameRenderArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           true /*byPassCache*/,
                                           window,
                                           canonicalWindow,
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImageLayerDesc, ImagePtr> layers;
        if ((effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) || layers.empty()) {
            *error = "render failed";

            return false;
        }
        ImagePtr image = layers.begin()->second;
        if (!image || (image->getComponentsCount() != 4) || (image->getBitDepth() != eImageBitDepthFloat)) {
            *error = "render did not produce an RGBA float image";

            return false;
        }
        const RectI bounds = image->getBounds();
        if (!bounds.contains(window)) {
            *error = "rendered image does not cover the window";

            return false;
        }

        pixels->resize((std::size_t)width * height * 4);
        Image::ReadAccess access = image->getReadRights();
        for (int y = 0; y < height; ++y) {
            const float* row = (const float*)access.pixelAt(0, y);
            std::memcpy(&(*pixels)[(std::size_t)y * width * 4], row, sizeof(float) * width * 4);
        }

        return true;
    }
};

TEST_F(GraphScalingTLS, InheritsOnlyTouchedHolders)
{
    NodePtr nodeA = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr nodeB = createNode(QString::fromUtf8(kConstantPluginID));
    ASSERT_TRUE(bool(nodeA) && bool(nodeB));
    EffectInstancePtr effectA = nodeA->getEffectInstance();
    EffectInstancePtr effectB = nodeB->getEffectInstance();
    ASSERT_TRUE(bool(effectA) && bool(effectB));

    std::vector<EffectInstancePtr> others;
    for (int i = 0; i < 1000; ++i) {
        NodePtr node = createNode(QString::fromUtf8(kConstantPluginID));
        ASSERT_TRUE(bool(node));
        others.push_back(node->getEffectInstance());
        ASSERT_TRUE(bool(others.back()));
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsPtr argsA = makeFrameArgs(7., abortInfo, nodeA);
    ParallelRenderArgsPtr argsOthers = makeFrameArgs(3., abortInfo, nodeA);
    effectA->setParallelRenderArgsTLS(argsA);
    for (std::size_t i = 0; i < others.size(); ++i) {
        others[i]->setParallelRenderArgsTLS(argsOthers);
    }
    ASSERT_EQ(argsA.get(), effectA->getParallelRenderArgsTLS().get());

    QThread* mainThread = QThread::currentThread();
    ParallelRenderArgsPtr workerArgsA, workerArgsB, nestedArgsA;
    std::size_t workerCopies = 0;
    std::size_t nestedCopies = 0;
    runOnNewThread([&]() {
        QThread* worker = QThread::currentThread();
        appPTR->getAppTLS()->softCopy(mainThread, worker);

        // The worker has not touched A, so the nested thread must find A's data on main through the chain.
        runOnNewThread([&]() {
            appPTR->getAppTLS()->softCopy(worker, QThread::currentThread());
            const std::size_t before = AppTLS::getNumInheritedCopies();
            nestedArgsA = effectA->getParallelRenderArgsTLS();
            nestedCopies = AppTLS::getNumInheritedCopies() - before;
            appPTR->getAppTLS()->cleanupTLSForThread();
        });

        const std::size_t before = AppTLS::getNumInheritedCopies();
        workerArgsA = effectA->getParallelRenderArgsTLS();
        workerCopies = AppTLS::getNumInheritedCopies() - before;
        workerArgsB = effectB->getParallelRenderArgsTLS();
        appPTR->getAppTLS()->cleanupTLSForThread();
    });

    EXPECT_EQ(argsA.get(), workerArgsA.get());
    EXPECT_FALSE(bool(workerArgsB));
    EXPECT_EQ(1u, workerCopies) << "a worker that touches one holder must copy exactly one";
    EXPECT_EQ(argsA.get(), nestedArgsA.get());
    EXPECT_EQ(1u, nestedCopies);

    effectA->invalidateParallelRenderArgsTLS();
    for (std::size_t i = 0; i < others.size(); ++i) {
        others[i]->invalidateParallelRenderArgsTLS();
    }
    ASSERT_FALSE(bool(effectA->getParallelRenderArgsTLS()));

    ParallelRenderArgsPtr freshArgsA;
    bool freshArgsRead = false;
    runOnNewThread([&]() {
        appPTR->getAppTLS()->softCopy(mainThread, QThread::currentThread());
        freshArgsA = effectA->getParallelRenderArgsTLS();
        freshArgsRead = true;
        appPTR->getAppTLS()->cleanupTLSForThread();
    });
    EXPECT_TRUE(freshArgsRead);
    EXPECT_FALSE(bool(freshArgsA)) << "a worker spawned after main popped its args must not see them";
}

TEST_F(GraphScalingTLS, MultithreadedPixelsMatchSingleThreaded)
{
    const int kSize = 512;
    const int kNumGrades = 50;

    NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(checker));

    std::vector<NodePtr> grades;
    NodePtr previous = checker;
    for (int i = 0; i < kNumGrades; ++i) {
        NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
        ASSERT_TRUE(bool(grade));
        connectNodes(previous, grade, 0, true);
        grades.push_back(grade);
        previous = grade;
    }

    KnobColor* firstMultiply = dynamic_cast<KnobColor*>(grades.front()->getKnobByName("multiply").get());
    ASSERT_TRUE(firstMultiply != NULL);
    const int nDims = firstMultiply->getDimension();
    ASSERT_GE(nDims, 3);
    for (int d = 0; d < nDims; ++d) {
        firstMultiply->setValueAtTime(1., 0.5, ViewSpec::all(), d);
        firstMultiply->setValueAtTime(20., 1.5, ViewSpec::all(), d);
    }
    const std::string firstName = grades.front()->getScriptName();
    for (std::size_t i = 1; i < grades.size(); ++i) {
        KnobIPtr multiply = grades[i]->getKnobByName("multiply");
        ASSERT_TRUE(bool(multiply));
        for (int d = 0; d < nDims; ++d) {
            const std::string expr = firstName + ".multiply.getValue(" + std::to_string(d) + ")";
            ASSERT_NO_THROW(multiply->setExpression(d, expr, false, true));
        }
    }

    // A thread that lost the first Grade's frame args would evaluate the expressions at the timeline frame.
    getApp()->getTimeLine()->seekFrame(1, false, NULL, eTimelineChangeReasonOtherSeek);

    const int savedMaxThreads = QThreadPool::globalInstance()->maxThreadCount();
    std::string error;

    appPTR->setNumberOfThreads(-1);
    appPTR->clearAllCaches();
    std::vector<float> singleThreaded;
    const bool singleOk = renderWindow(grades.back(), 10., kSize, kSize, &singleThreaded, &error);

    appPTR->setNumberOfThreads(0);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    appPTR->clearAllCaches();
    std::vector<float> multiThreaded;
    const bool multiOk = singleOk && renderWindow(grades.back(), 10., kSize, kSize, &multiThreaded, &error);

    QThreadPool::globalInstance()->setMaxThreadCount(savedMaxThreads);

    ASSERT_TRUE(singleOk) << error;
    ASSERT_TRUE(multiOk) << error;
    ASSERT_EQ(singleThreaded.size(), multiThreaded.size());

    std::size_t firstMismatch = singleThreaded.size();
    std::size_t numMismatches = 0;
    for (std::size_t i = 0; i < singleThreaded.size(); ++i) {
        if (std::memcmp(&singleThreaded[i], &multiThreaded[i], sizeof(float)) != 0) {
            if (numMismatches == 0) {
                firstMismatch = i;
            }
            ++numMismatches;
        }
    }
    EXPECT_EQ(0u, numMismatches) << "first mismatch at float " << firstMismatch << ": "
                                 << singleThreaded[firstMismatch < singleThreaded.size() ? firstMismatch : 0] << " vs "
                                 << multiThreaded[firstMismatch < multiThreaded.size() ? firstMismatch : 0];
}
