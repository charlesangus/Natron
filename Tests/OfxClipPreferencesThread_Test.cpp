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

#include <algorithm>
#include <atomic>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include <gtest/gtest.h>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// Whether an OpenFX plug-in's clip preferences can run off the main thread: from many pool
// threads at once, while the main thread renders the same node, with the answer the main thread
// gets and without writing to the node.

namespace {

const int kThreads = 16;
const int kCallsPerThread = 50;
const qint64 kTimeoutMs = 30000;
const char* const kThreeLayersFixture = NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr";

OfxEffectInstance*
ofxEffectOf(const NodePtr& node)
{
    return dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
}

std::vector<std::string>
optionIDs(const NodePtr& node,
          const std::string& name)
{
    std::vector<std::string> ids;
    KnobChoice* choice = dynamic_cast<KnobChoice*>(node->getKnobByName(name).get());
    EXPECT_TRUE(choice != NULL) << name;
    if (!choice) {
        return ids;
    }
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        ids.push_back(entries[i].id);
    }

    return ids;
}

} // anonymous namespace

class OfxClipPreferencesThreadTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // Runs the clip preferences of `node` kThreads x kCallsPerThread times on a pool of its own,
    // each call over a copy of `defaults`, and hands every answer to `check`, which is called on
    // the pool threads and returns false for a wrong one. Meanwhile the main thread renders
    // `rendered` over and over, when given, so that renders hold the preferences lock for
    // reading. False, with a failure, if the calls do not all finish within kTimeoutMs.
    bool runOnPoolThreads(const NodePtr& node,
                          const NodeMetadata& defaults,
                          const std::function<bool(const NodeMetadata&)>& check,
                          const NodePtr& rendered,
                          int* wrongAnswers,
                          int* failedCalls,
                          int* renders)
    {
        OfxEffectInstance* effect = ofxEffectOf(node);
        if (!effect) {
            ADD_FAILURE() << node->getPluginID() << " is not an OpenFX node";

            return false;
        }
        std::atomic<int> wrong(0);
        std::atomic<int> failed(0);
        std::atomic<int> finished(0);
        std::atomic<int> offMainThread(0);
        QThread* mainThread = QThread::currentThread();

        // Leaked on a timeout, since destroying a pool waits for its threads.
        QThreadPool* pool = new QThreadPool();
        pool->setMaxThreadCount(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            pool->start([&]() {
                if (QThread::currentThread() != mainThread) {
                    ++offMainThread;
                }
                for (int i = 0; i < kCallsPerThread; ++i) {
                    NodeMetadata metadata(defaults);
                    StatusEnum stat = effect->getClipPreferencesOnAnyThreadForTest(metadata);
                    if ((stat != eStatusOK) && (stat != eStatusReplyDefault)) {
                        ++failed;
                    } else if (!check(metadata)) {
                        ++wrong;
                    }
                }
                ++finished;
            });
        }

        QElapsedTimer timer;
        timer.start();
        *renders = 0;
        while (rendered && (finished.load() < kThreads) && (timer.elapsed() < kTimeoutMs)) {
            std::vector<RenderedPlane> planes;
            std::string error;
            // Through the cache: a render that bypasses it first clears every cache, which
            // refreshes every node in the project on this thread and so moves the hash mark
            // counts the callers check, whatever the pool threads do.
            EXPECT_TRUE(renderNodePlanesDirect(rendered, 1., ViewIdx(0), 0, RectI(0, 0, 4, 4), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, false)) << error;
            ++*renders;
        }
        const qint64 remaining = kTimeoutMs - timer.elapsed();
        if ((remaining <= 0) || !pool->waitForDone((int)remaining)) {
            ADD_FAILURE() << node->getPluginID() << ": the clip preferences did not finish within " << kTimeoutMs << " ms";

            return false;
        }
        delete pool;
        EXPECT_EQ(kThreads, offMainThread.load());
        *wrongAnswers = wrong.load();
        *failedCalls = failed.load();

        return true;
    }
};

// The Generator sets its output layout, format and PAR from its knobs. Asked over defaults that
// say otherwise, every pool thread gets the knobs' answer back while the main thread renders the
// node, and the node's hash and its main-thread answer are left alone.
TEST_F(OfxClipPreferencesThreadTest, GeneratorAnswersOnPoolThreadsAsOnTheMainThread)
{
    const int kWidth = 64;
    const int kHeight = 48;
    const double kPar = 2.;

    NodePtr generator = createNode(QString::fromUtf8(kNatronTestGeneratorPluginID), kNatronTestGeneratorMajor);
    ASSERT_TRUE(bool(generator));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatSize, { (double)kWidth, (double)kHeight }));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatPar, { kPar }));
    ASSERT_TRUE(setKnobValue(generator, kNatronTestGeneratorParamExtent, kNatronTestGeneratorExtentFormat));

    NodeMetadataConstPtr mainAnswer = generator->getEffectInstance()->getTimeInvariantMetadata();
    ASSERT_TRUE(bool(mainAnswer));
    ASSERT_EQ(4, mainAnswer->getNComps(-1));
    ASSERT_EQ(RectI(0, 0, kWidth, kHeight), mainAnswer->getOutputFormat());
    ASSERT_DOUBLE_EQ(kPar, mainAnswer->getPixelAspectRatio(-1));

    NodeMetadata defaults(*mainAnswer);
    defaults.setNComps(-1, 1);
    defaults.setOutputFormat(RectI(0, 0, 1, 1));
    defaults.setPixelAspectRatio(-1, 1.);

    const U64 marksBefore = generator->getHashMarkCount();
    const U64 hashBefore = generator->getHashValue();

    int wrong = 0, failed = 0, renders = 0;
    ASSERT_TRUE(runOnPoolThreads(generator, defaults, [&](const NodeMetadata& answer) { return (answer.getNComps(-1) == mainAnswer->getNComps(-1)) && (answer.getOutputFormat() == mainAnswer->getOutputFormat()) && (answer.getPixelAspectRatio(-1) == mainAnswer->getPixelAspectRatio(-1)) && (answer.getBitDepth(-1) == mainAnswer->getBitDepth(-1)); }, generator, &wrong, &failed, &renders));

    EXPECT_EQ(0, failed);
    EXPECT_EQ(0, wrong);
    EXPECT_GT(renders, 0);
    EXPECT_EQ(marksBefore, generator->getHashMarkCount());
    EXPECT_EQ(hashBefore, generator->getHashValue());
    NodeMetadataConstPtr mainAnswerAfter = generator->getEffectInstance()->getTimeInvariantMetadata();
    ASSERT_TRUE(bool(mainAnswerAfter));
    EXPECT_TRUE(*mainAnswer == *mainAnswerAfter);
}

// The MultiPlane test plug-in rebuilds its menus from its input's planes inside its clip
// preferences, as ofxsMultiPlane does, but touches a menu only when its options change. Over an
// unchanged input, pool-thread calls answer as the main thread did and leave the menus, and so
// the node's hash, alone. A rebuild that did change a menu would mark the hash from that thread:
// the per-frame calls must keep plug-ins from doing so.
TEST_F(OfxClipPreferencesThreadTest, MultiPlaneOverAnUnchangedInputLeavesItsMenusAlone)
{
    NodePtr reader = createWorkingSpaceRead(kThreeLayersFixture);
    NodePtr multiPlane = createNode(QString::fromUtf8(kNatronTestMultiPlanePluginID), kNatronTestMultiPlaneMajor);
    ASSERT_TRUE(reader && multiPlane);
    connectNodes(reader, multiPlane, 0, true);

    const std::vector<std::string> planesBefore = optionIDs(multiPlane, kNatronTestMultiPlaneParamInputPlane);
    const std::vector<std::string> channelsBefore = optionIDs(multiPlane, kNatronTestMultiPlaneParamByChannel);
    ASSERT_TRUE(std::find(planesBefore.begin(), planesBefore.end(), std::string("diffuse")) != planesBefore.end());

    NodeMetadataConstPtr mainAnswer = multiPlane->getEffectInstance()->getTimeInvariantMetadata();
    ASSERT_TRUE(bool(mainAnswer));
    const U64 marksBefore = multiPlane->getHashMarkCount();
    const U64 hashBefore = multiPlane->getHashValue();

    int wrong = 0, failed = 0, renders = 0;
    ASSERT_TRUE(runOnPoolThreads(multiPlane, *mainAnswer, [&](const NodeMetadata& answer) { return (answer.getNComps(-1) == mainAnswer->getNComps(-1)) && (answer.getComponentsType(-1) == mainAnswer->getComponentsType(-1)) && (answer.getNComps(0) == mainAnswer->getNComps(0)) && (answer.getOutputFormat() == mainAnswer->getOutputFormat()) && (answer.getPixelAspectRatio(-1) == mainAnswer->getPixelAspectRatio(-1)); }, multiPlane, &wrong, &failed, &renders));

    EXPECT_EQ(0, failed);
    EXPECT_EQ(0, wrong);
    EXPECT_EQ(planesBefore, optionIDs(multiPlane, kNatronTestMultiPlaneParamInputPlane));
    EXPECT_EQ(channelsBefore, optionIDs(multiPlane, kNatronTestMultiPlaneParamByChannel));
    EXPECT_EQ(marksBefore, multiPlane->getHashMarkCount());
    EXPECT_EQ(hashBefore, multiPlane->getHashValue());
}
