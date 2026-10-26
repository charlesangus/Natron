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

#include <atomic>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QThreadPool>

#include <gtest/gtest.h>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// An OpenFX node's metadata follows the frame: its clip preferences run for each set of inputs
// a frame reads, once per such set, off the main thread, and the clips answer for that frame.

namespace {

const int kThreads = 16;
const qint64 kTimeoutMs = 30000;

OfxEffectInstance*
ofxEffectOf(const NodePtr& node)
{
    return dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
}

MetadataSourceTestEffect*
metadataSourceOf(const NodePtr& node)
{
    return dynamic_cast<MetadataSourceTestEffect*>(node->getEffectInstance().get());
}

// The channel count the stream of `inputNb` (-1 for the output) carries.
int
nCompsOf(const NodeMetadataConstPtr& metadata,
         int inputNb)
{
    if ((inputNb == -1) && (metadata->getOutputStorageNComps() > 0) && (metadata->getComponentsType(-1) == kNatronColorLayerID)) {
        return metadata->getOutputStorageNComps();
    }

    return metadata->getNComps(inputNb);
}

int
nCompsAt(const NodePtr& node,
         double time,
         int inputNb = -1)
{
    NodeMetadataConstPtr metadata = node->getEffectInstance()->getMetadata(time, ViewIdx(0));
    EXPECT_TRUE(bool(metadata)) << node->getScriptName() << " time=" << time;

    return metadata ? nCompsOf(metadata, inputNb) : -1;
}

// Runs `body(threadIndex)` on kThreads pool threads at once. While they run, `mainThreadWork` is
// called over and over on this thread, when given, and queued events are processed. False, with
// a failure, if the threads do not all finish within kTimeoutMs.
bool
runOnPoolThreads(const std::function<void(int)>& body,
                 const std::function<void()>& mainThreadWork)
{
    std::atomic<int> finished(0);
    // Leaked on a timeout, since destroying a pool waits for its threads.
    QThreadPool* pool = new QThreadPool();
    pool->setMaxThreadCount(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        pool->start([&, t]() {
            body(t);
            ++finished;
        });
    }
    QElapsedTimer timer;
    timer.start();
    while (mainThreadWork && (finished.load() < kThreads) && (timer.elapsed() < kTimeoutMs)) {
        mainThreadWork();
        QCoreApplication::processEvents();
    }
    const qint64 remaining = kTimeoutMs - timer.elapsed();
    if ((remaining <= 0) || !pool->waitForDone((int)remaining)) {
        ADD_FAILURE() << "the threads did not finish within " << kTimeoutMs << " ms";

        return false;
    }
    delete pool;

    return true;
}

} // namespace

class OfxPerFrameMetadataTest
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

    NodePtr createSource(int nComps)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        MetadataSourceTestEffect* effect = metadataSourceOf(source);
        EXPECT_TRUE(effect != NULL);
        if (effect) {
            effect->getNCompsKnob()->setValue(nComps, ViewSpec::all(), 0);
        }

        return source;
    }

    // A Switch routing an RGB source at frame 1 and an RGBA one from frame 2 on.
    NodePtr createKeyedSwitch()
    {
        NodePtr sourceA = createSource(3);
        NodePtr sourceB = createSource(4);
        NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
        EXPECT_TRUE(sourceA && sourceB && switchNode);
        if (!sourceA || !sourceB || !switchNode) {
            return NodePtr();
        }
        connectNodes(sourceA, switchNode, 0, true);
        connectNodes(sourceB, switchNode, 1, true);
        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
        EXPECT_TRUE(bool(which));
        if (!which) {
            return NodePtr();
        }
        which->setValueAtTime(1, 0, ViewSpec::all(), 0);
        which->setValueAtTime(2, 1, ViewSpec::all(), 0);

        return switchNode;
    }

    // The PointMatrix whose channel set leaves alpha alone, so its output keeps its input's
    // layout: one that writes alpha widens an RGB stream to RGBA.
    NodePtr createRGBPointBelow(const NodePtr& input)
    {
        NodePtr rgbPoint = createNode(QString::fromUtf8(kNatronTestRGBPointPluginID), kNatronTestRGBPointMajor);
        EXPECT_TRUE(bool(rgbPoint));
        if (!rgbPoint) {
            return rgbPoint;
        }
        connectNodes(input, rgbPoint, 0, true);
        EXPECT_TRUE(ofxEffectOf(rgbPoint) != NULL);

        return rgbPoint;
    }

    // A FrameWindow over `input` reading frames first..last, relative to the frame rendered
    // unless `absolute`.
    NodePtr createFrameWindowBelow(const NodePtr& input,
                                   int first,
                                   int last,
                                   bool absolute)
    {
        NodePtr window = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID), kNatronTestFrameWindowMajor);
        EXPECT_TRUE(bool(window));
        if (!window) {
            return window;
        }
        connectNodes(input, window, 0, true);
        KnobIntPtr frameRange = std::dynamic_pointer_cast<KnobInt>(window->getKnobByName(kNatronTestFrameWindowParamFrameRange));
        KnobBoolPtr absoluteKnob = std::dynamic_pointer_cast<KnobBool>(window->getKnobByName(kNatronTestFrameWindowParamAbsolute));
        EXPECT_TRUE(frameRange && absoluteKnob);
        if (!frameRange || !absoluteKnob) {
            return NodePtr();
        }
        absoluteKnob->setValue(absolute);
        frameRange->setValue(first, ViewSpec::all(), 0);
        frameRange->setValue(last, ViewSpec::all(), 1);

        return window;
    }
};

// Below a Switch keyed from RGB to RGBA, the plug-in answers RGB at frame 1 and RGBA after, and
// its clip preferences run once for each of the two input layouts, however many threads ask
// about however many frames.
TEST_F(OfxPerFrameMetadataTest, SwitchedInputRunsClipPreferencesOncePerLayout)
{
    const int kFrames = 100;

    NodePtr switchNode = createKeyedSwitch();
    ASSERT_TRUE(bool(switchNode));
    NodePtr rgbPoint = createRGBPointBelow(switchNode);
    ASSERT_TRUE(bool(rgbPoint));
    OfxEffectInstance* effect = ofxEffectOf(rgbPoint);
    ASSERT_TRUE(effect != NULL);
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(rgbPoint->getKnobByName(kNatronTestPointMatrixParamMix));
    ASSERT_TRUE(bool(mix));

    // A new hash, so that whatever the graph's construction asked is not reused, and every call
    // from here on is for one of the two layouts under it.
    const U64 callsBefore = effect->getPerFrameClipPreferencesCallCountForTest();
    mix->setValue(0.5, ViewSpec::all(), 0);

    std::atomic<int> wrong(0);
    ASSERT_TRUE(runOnPoolThreads([&](int t) {
        for (int i = 0; i < kFrames; ++i) {
            // Each thread starts at a different frame, so that both layouts are asked at once.
            const int frame = 1 + ((i + t * 7) % kFrames);
            NodeMetadataConstPtr metadata = rgbPoint->getEffectInstance()->getMetadata(frame, ViewIdx(0));
            const int expected = (frame == 1) ? 3 : 4;
            if (!metadata || (nCompsOf(metadata, -1) != expected) || (nCompsOf(metadata, 0) != expected)) {
                ++wrong;
            }
        }
    },
                                 std::function<void()>()));

    EXPECT_EQ(0, wrong.load());
    EXPECT_EQ(3, nCompsAt(rgbPoint, 1));
    EXPECT_EQ(4, nCompsAt(rgbPoint, 2));
    EXPECT_EQ(callsBefore + 2, effect->getPerFrameClipPreferencesCallCountForTest());
}

// A plug-in reading frames 1 and 2 at frame 1 gets the widest of the two: RGBA on its source
// clip, which the frames it fetches are converted to, and RGBA out.
TEST_F(OfxPerFrameMetadataTest, TemporalReadMergesTheFramesItNeeds)
{
    NodePtr switchNode = createKeyedSwitch();
    ASSERT_TRUE(bool(switchNode));
    NodePtr window = createFrameWindowBelow(switchNode, 0, 1, false);
    ASSERT_TRUE(bool(window));

    EXPECT_EQ(3, nCompsAt(switchNode, 1));
    EXPECT_EQ(4, nCompsAt(window, 1));
    EXPECT_EQ(4, nCompsAt(window, 1, 0));
    EXPECT_EQ(4, nCompsAt(window, 2, 0));

    // Rendered over readers, the frame RGB at frame 1 is fetched with the clip's RGBA layout.
    getApp()->getProject()->reset(false, true);
    CreateNodeArgs rgbArgs(_readPluginID.toStdString(), getApp()->getProject());
    rgbArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgb-only.exr"));
    NodePtr rgbReader = getApp()->createNode(rgbArgs);
    CreateNodeArgs rgbaArgs(_readPluginID.toStdString(), getApp()->getProject());
    rgbaArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    NodePtr rgbaReader = getApp()->createNode(rgbaArgs);
    NodePtr readSwitch = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
    ASSERT_TRUE(rgbReader && rgbaReader && readSwitch);
    connectNodes(rgbReader, readSwitch, 0, true);
    connectNodes(rgbaReader, readSwitch, 1, true);
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(readSwitch->getKnobByName(kSwitchParamWhich));
    ASSERT_TRUE(bool(which));
    which->setValueAtTime(1, 0, ViewSpec::all(), 0);
    which->setValueAtTime(2, 1, ViewSpec::all(), 0);
    NodePtr readWindow = createFrameWindowBelow(readSwitch, 0, 1, false);
    ASSERT_TRUE(bool(readWindow));
    EXPECT_EQ(4, nCompsAt(readWindow, 1, 0));

    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesInMode(readWindow, 1, ViewIdx(0), 0, RectI(0, 0, 8, 8), std::list<ImageLayerDesc>(),
                                       eRenderSchedulerModeTaskGraph, true, &planes, &error))
        << error;
    const RenderedPlane* color = NULL;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        if (planes[p].layer.isColorLayer()) {
            color = &planes[p];
        }
    }
    ASSERT_TRUE(color != NULL);
    EXPECT_EQ(4u, color->channels.size());
}

// A FrameWindow holding frame 1 is an identity on every frame: a native node below it gets frame
// 1's metadata at frame 2, and the plug-in's clip preferences never run for a frame.
TEST_F(OfxPerFrameMetadataTest, IdentityFrameSkipsClipPreferences)
{
    NodePtr source = createSource(4);
    ASSERT_TRUE(bool(source));
    KnobIntPtr nComps = metadataSourceOf(source)->getNCompsKnob();
    nComps->setValueAtTime(1, 3, ViewSpec::all(), 0);
    nComps->setValueAtTime(2, 4, ViewSpec::all(), 0);
    NodePtr hold = createFrameWindowBelow(source, 1, 1, true);
    ASSERT_TRUE(bool(hold));
    OfxEffectInstance* effect = ofxEffectOf(hold);
    ASSERT_TRUE(effect != NULL);
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    ASSERT_TRUE(bool(blur));
    connectNodes(hold, blur, 0, true);

    const U64 callsBefore = effect->getPerFrameClipPreferencesCallCountForTest();
    NodeMetadataConstPtr held = source->getEffectInstance()->getMetadata(1, ViewIdx(0));
    ASSERT_TRUE(bool(held));
    EXPECT_EQ(4, nCompsAt(source, 2));
    for (int frame = 1; frame <= 3; ++frame) {
        EXPECT_EQ(3, nCompsAt(hold, frame)) << "frame=" << frame;
        EXPECT_EQ(3, nCompsAt(blur, frame)) << "frame=" << frame;
    }
    NodeMetadataConstPtr blurAt2 = blur->getEffectInstance()->getMetadata(2, ViewIdx(0));
    ASSERT_TRUE(bool(blurAt2));
    EXPECT_EQ(held->getBitDepth(-1), blurAt2->getBitDepth(-1));
    EXPECT_EQ(held->getOutputFormat(), blurAt2->getOutputFormat());
    EXPECT_DOUBLE_EQ(held->getPixelAspectRatio(-1), blurAt2->getPixelAspectRatio(-1));
    EXPECT_EQ(callsBefore, effect->getPerFrameClipPreferencesCallCountForTest());
}

// Threads asking for the plug-in's metadata while the main thread edits the graph, and so
// refreshes it, all finish.
TEST_F(OfxPerFrameMetadataTest, QueriesDuringMainThreadEditsDoNotDeadlock)
{
    const int kFrames = 8;
    const int kQueriesPerThread = 400;

    NodePtr switchNode = createKeyedSwitch();
    ASSERT_TRUE(bool(switchNode));
    NodePtr rgbPoint = createRGBPointBelow(switchNode);
    ASSERT_TRUE(bool(rgbPoint));
    NodePtr window = createFrameWindowBelow(rgbPoint, 0, 1, false);
    ASSERT_TRUE(bool(window));
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(rgbPoint->getKnobByName(kNatronTestPointMatrixParamMix));
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
    ASSERT_TRUE(mix && which);

    const std::vector<NodePtr> queried = { rgbPoint, window };
    std::atomic<int> nullAnswers(0);
    int edits = 0;
    ASSERT_TRUE(runOnPoolThreads([&](int t) {
        for (int i = 0; i < kQueriesPerThread; ++i) {
            const double frame = 1 + ((i + t) % kFrames);
            for (const NodePtr& node : queried) {
                if (!node->getEffectInstance()->getMetadata(frame, ViewIdx(0))) {
                    ++nullAnswers;
                }
            }
        } }, [&]() {
        mix->setValue((edits % 2) ? 1. : 0.5, ViewSpec::all(), 0);
        which->setValueAtTime(3, edits % 2, ViewSpec::all(), 0);
        ++edits; }));

    EXPECT_EQ(0, nullAnswers.load());
}
